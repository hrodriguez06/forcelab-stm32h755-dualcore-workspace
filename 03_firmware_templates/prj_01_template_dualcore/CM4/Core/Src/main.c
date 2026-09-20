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
#include "shared_memory.h"

#define HEARTBEAT_DELAY_LOOPS   2000000UL

static void CPU2_WaitForCPU1(void);

int main(void)
{
    CPU2_WaitForCPU1();

    /* The clock tree is up and owned by CPU1. From here CPU2 configures only
       what belongs to it. HAL_Init() sets up the SysTick of THIS core and the
       NVIC priority grouping; it does not touch the clock tree. */
    HAL_Init();
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    /* Deliberately not calling Shared_Init(): the block belongs to CPU1 and is
       already initialised by the time this core is released. Zeroing it here
       would wipe whatever CPU1 has published. */

    for (;;)
    {
        volatile uint32_t spin;

        /* A single aligned word, so the increment is atomic on this core and
           needs no HSEM. Anything wider - or a read-modify-write that CPU1
           also performs - must be taken under SHARED_LOCK(). */
        g_shared.m4_heartbeat++;

        /* A crude delay on purpose: this template does not claim a timer for
           CPU2, because which timer CPU2 owns is an application decision.
           Replace with a real time base when you give it one. */
        for (spin = 0U; spin < HEARTBEAT_DELAY_LOOPS; spin++)
        {
        }
    }
}

/**
  * @brief  Parks CPU2 in STOP until CPU1 releases HSEM 0.
  *
  * @note   HSEM 0 is the channel ST reserves for this handshake, and the
  *         notification is what wakes the core out of STOP - not a poll loop.
  *         The order matters: activate the notification BEFORE entering STOP,
  *         or CPU1 can release the semaphore in the window between the two and
  *         CPU2 sleeps for ever.
  */
static void CPU2_WaitForCPU1(void)
{
    __HAL_RCC_HSEM_CLK_ENABLE();

    /* Subscribe this core to HSEM 0 before sleeping. */
    HAL_HSEM_ActivateNotification(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_BOOT));

    /* Enter STOP on D2. Dropping D2CKRDY is the signal CPU1 polls for. */
    HAL_PWREx_ClearPendingEvent();
    HAL_PWREx_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFE, PWR_D2_DOMAIN);

    /* Woken by the HSEM notification: unsubscribe and clear it. */
    __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_BOOT));
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
