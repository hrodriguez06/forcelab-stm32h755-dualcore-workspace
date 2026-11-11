/**
  ******************************************************************************
  * @file    main.c
  * @brief   CPU2 (Cortex-M4) entry point - bare metal, no RTOS.
  *
  *          Boot contract with CPU1:
  *            1. Both cores leave reset together (BCM7 and BCM4 both set).
  *            2. CPU2 immediately enters STOP on the D2 domain and waits
  *               for the HSEM 0 notification and SHARED_MAGIC.
  *            3. CPU1 configures clocks, power, and SRAM4 shared memory, then
  *               releases HSEM 0.
  *            4. CPU2 wakes, installs the cross-core GPIO HSEM 5 lock, and
  *               initializes bare-metal ADC1 + DMA1_Stream0 + TIM3 (500 kSPS).
  ******************************************************************************
  */

#include "main.h"
#include "shared_memory.h"
#include "hsem_bm.h"
#include "adc_acquisition.h"

/* Liveness markers in CPU2's own SRAM2 */
volatile uint32_t cpu2_woke  = 0UL;
volatile uint32_t cpu2_loops = 0UL;

static void CPU2_WaitForCPU1(void);

int main(void)
{
    uint32_t last_heartbeat_tick = 0U;

    CPU2_WaitForCPU1();

    /* Silicon errata ES0445 2.2.2: Disable FPU lazy stacking to avoid
       potential register corruption during interrupt entry. */
    FPU->FPCCR &= ~FPU_FPCCR_LSPEN_Msk;

    cpu2_woke = 0xC0FFEEUL;

    /* HAL_Init() sets up the SysTick of THIS core and NVIC priority grouping. */
    HAL_Init();
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    HSEM_BM_Init();
    HSEM_BM_TimebaseStart();

    /* Install cross-core GPIO lock (HSEM 5) before any pin configuration */
    Shared_GpioLockInstall();

    /* Initialize ADC1 + DMA1_Stream0 with TIM3 500 kHz TRGO trigger on PB1 (A3) */
    if (!ADC_Acquisition_Init())
    {
        Error_Handler();
    }

    for (;;)
    {
        uint32_t now;

        cpu2_loops++;

        /* Update CM4 heartbeat every 100 ms */
        now = HAL_GetTick();
        if ((now - last_heartbeat_tick) >= 100U)
        {
            last_heartbeat_tick = now;
            g_shared.m4_heartbeat++;
        }

        /* Process completed 512-sample ADC DMA half-buffer if ready */
        ADC_Acquisition_Process();
    }
}

static bool Shared_MagicReady(void *ctx)
{
    (void)ctx;
    return (g_shared.magic == SHARED_MAGIC);
}

static void CPU2_WaitForCPU1(void)
{
    const HsemBm_BootWait_t cfg = {
        .sem_id     = HSEM_ID_BOOT_SYNC,
        .ready      = Shared_MagicReady,
        .ctx        = NULL,
        .wake_count = &g_shared.cpu2_wake_count
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
