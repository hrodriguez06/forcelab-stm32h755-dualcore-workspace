/**
  ******************************************************************************
  * @file    main.c
  * @brief   CPU2 (Cortex-M4) entry point - bare metal, no RTOS.
  *
  *          Boot contract with CPU1, from
  *          00_docs_stm32h7/docs/00_core_and_system/dual_core_boot_ipc.md:
  *
  *            1. Both cores leave reset together (BCM7 and BCM4 both set).
  *            2. CPU2 does NOT touch RCC, the PLLs or the power supply. CPU1
  *               owns the clock tree. CPU2 running before that is configured
  *               would execute at the wrong frequency and, worse, would race
  *               CPU1's writes to RCC.
  *            3. So CPU2 immediately enters STOP on the D2 domain and waits
  *               for the HSEM 0 notification. Entering STOP is what drops
  *               D2CKRDY, which is how CPU1 detects that CPU2 has parked.
  *            4. CPU1 configures everything, then takes and releases HSEM 0.
  *               That wakes CPU2, which from then on only initialises its own
  *               peripherals.
  *
  *          Deviating from this is the usual cause of a dual-core board that
  *          works "sometimes": the failure depends on which core wins a race
  *          during the first microseconds.
  ******************************************************************************
  */

#include "main.h"
#include "fw_build_id.h"
#include "shared_memory.h"
#if defined(GPIO_DRIVER_SELFTEST)
#include "gpio_selftest.h"
#endif
#if defined(TIM_DRIVER_SELFTEST)
#include "tim_selftest.h"
#endif

#define HEARTBEAT_DELAY_LOOPS   2000000UL

/* Liveness markers in CPU2's OWN SRAM2, deliberately not in the shared block.
   If g_shared.m4_heartbeat never moves, these say why: cpu2_woke tells you
   whether the HSEM handshake completed at all, and cpu2_loops whether the
   super-loop is running. A debugger can read them without halting anything -
   nothing else on the board writes SRAM2. */
volatile uint32_t cpu2_woke  = 0UL;
volatile uint32_t cpu2_loops = 0UL;

static void CPU2_WaitForCPU1(void);
static void CPU2_PublishBuild(void);

int main(void)
{
    CPU2_WaitForCPU1();

    cpu2_woke = 0xC0FFEEUL;

    /* The clock tree is up and owned by CPU1. From here CPU2 configures only
       what belongs to it. HAL_Init() sets up the SysTick of THIS core and the
       NVIC priority grouping; it does not touch the clock tree. */
    HAL_Init();
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    HSEM_BM_Init();
    HSEM_BM_TimebaseStart();

    /* Before this core's first pin configuration: CPU1 may be configuring
       pins of the same port from now on (Common/Inc/shared_memory.h). */
    Shared_GpioLockInstall();

    /* R3-c: which image this core runs, before it declares itself alive (its
       first m4_heartbeat increment, below or after the selftest). */
    CPU2_PublishBuild();

    /* Deliberately not calling Shared_Init(): the block belongs to CPU1 and is
       already initialised by the time this core is released. Zeroing it here
       would wipe whatever CPU1 has published. */

#if defined(GPIO_DRIVER_SELFTEST)
    /* Serves CPU1's test commands until CPU1 says it is done, then falls
       through to the application loop. */
    GPIO_Selftest_CM4_Run();
#endif
#if defined(TIM_DRIVER_SELFTEST)
    /* Serves CPU1's 03_tim bench commands until it says it is done. */
    TIM_Selftest_CM4_Run();
#endif

    for (;;)
    {
        volatile uint32_t spin;

        /* A single aligned word, so the increment is atomic on this core and
           needs no HSEM. Anything wider - or a read-modify-write that CPU1
           also performs - must be taken under SHARED_LOCK(). */
        cpu2_loops++;
        g_shared.m4_heartbeat++;

        /* A crude delay on purpose: this template does not claim a timer for
           CPU2, because which timer CPU2 owns is an application decision.
           Replace with a real time base when you give it one. */
        for (spin = 0U; spin < HEARTBEAT_DELAY_LOOPS; spin++)
        {
        }
    }
}

/* Copies FW_GIT_REV and FW_BUILD_ID into this core's fields of g_shared. */
static void copy_str(volatile char *dst, uint32_t size, const char *src)
{
    uint32_t i = 0U;
    for (; (i + 1U < size) && (src[i] != '\0'); i++)
    {
        dst[i] = src[i];
    }
    for (; i < size; i++)
    {
        dst[i] = '\0';
    }
}

static void CPU2_PublishBuild(void)
{
    copy_str(g_shared.m4_fw_rev, (uint32_t)sizeof(g_shared.m4_fw_rev), FW_GIT_REV);
    copy_str(g_shared.m4_fw_build_id, (uint32_t)sizeof(g_shared.m4_fw_build_id), FW_BUILD_ID);
    __DMB();
}

static bool Shared_MagicReady(void *ctx)
{
    (void)ctx;
    return (g_shared.magic == SHARED_MAGIC);
}

/**
  * @brief  Parks CPU2 in STOP until CPU1 releases HSEM 0 and shared memory is ready.
  */
static void CPU2_WaitForCPU1(void)
{
    const HsemBm_BootWait_t cfg = {
        .sem_id = HSEM_ID_BOOT_SYNC,
        .ready  = Shared_MagicReady,
        .ctx    = NULL
    };
    (void)HSEM_BM_BootWaitForCpu1(&cfg);
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;)
    {
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    Error_Handler();
}
#endif
