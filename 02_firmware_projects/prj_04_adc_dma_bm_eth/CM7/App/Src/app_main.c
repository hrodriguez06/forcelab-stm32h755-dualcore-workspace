/**
  ******************************************************************************
  * @file    app_main.c
  * @brief   Application entry point of CPU1 (Cortex-M7).
  *
  *          Two tasks, chosen to exercise the parts of a dual-core system that
  *          are easy to get wrong and hard to debug later:
  *
  *            heartbeat  writes g_shared.m7_heartbeat and blinks LD1, so the
  *                       board says something even with no console attached.
  *            status     reads g_shared.m4_heartbeat and prints both counters.
  *                       A CPU2 counter that stays at zero is the single most
  *                       useful symptom this firmware can give you: it means
  *                       CPU2 never ran, which is an option-byte or a Flash
  *                       bank 2 problem, not an application one. The first
  *                       sample reports "starting" rather than a verdict,
  *                       because there is no earlier sample to compare it
  *                       with - from the second one on, "NOT RESPONDING"
  *                       means it.
  *
  *          Replace both with the real application. Keep the shared-memory
  *          access pattern: every field is volatile and anything wider than a
  *          word is taken under its HSEM.
  ******************************************************************************
  */

#include "app_main.h"
#include "app_log.h"
#include "main.h"
#include "led_bm.h"
#include "shared_memory.h"
#include "net_config.h"
#include "net_task.h"

#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST
#include "adc_bm.h"
#include "adc_ll.h"
#endif

#if defined(DMA_DRIVER_SELFTEST) && DMA_DRIVER_SELFTEST
#include "dma_selftest.h"
#endif

#if defined(HSEM_DRIVER_SELFTEST) && HSEM_DRIVER_SELFTEST
#include "hsem_selftest.h"
#endif

#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#define HEARTBEAT_TASK_STACK_WORDS   256U
#define HEARTBEAT_TASK_PRIO          (tskIDLE_PRIORITY + 1U)
#define HEARTBEAT_PERIOD_MS          500U

#define STATUS_TASK_STACK_WORDS      512U
#define STATUS_TASK_PRIO             (tskIDLE_PRIORITY + 1U)
#define STATUS_PERIOD_MS             2000U

static void Heartbeat_Task(void *arg);
static void Status_Task(void *arg);

void app_main(void)
{
    LED_BootStage(LED_BOOT_APP_ENTERED);
    App_Log_Init();

    APP_LOG(" ");
    APP_LOG("=== STM32H755ZI dual-core Ethernet / lwIP - CPU1 (Cortex-M7) ===");
    APP_LOG("[sys] sysclk=%lu Hz hclk=%lu Hz",
            (unsigned long)HAL_RCC_GetSysClockFreq(),
            (unsigned long)HAL_RCC_GetHCLKFreq());
    APP_LOG("[sys] silicon revision id=0x%04lX",
            (unsigned long)((DBGMCU->IDCODE & DBGMCU_IDCODE_REV_ID_Msk) >>
                            DBGMCU_IDCODE_REV_ID_Pos));

    /* Shared memory was already initialised by CPU1 in main() before CPU2
       was released from HSEM 0. Verify the magic word before proceeding. */
    APP_LOG("[ipc] shared block at %p, magic=0x%08lX",
            (void *)&g_shared, (unsigned long)g_shared.magic);
    if (g_shared.magic != SHARED_MAGIC)
    {
        APP_LOG("[ipc] FATAL: shared block magic mismatch (expected 0x%08lX, got 0x%08lX)",
                (unsigned long)SHARED_MAGIC, (unsigned long)g_shared.magic);
        Error_Handler();
    }
    APP_LOG("[ipc] CPU2 boot handshake: %s",
            CM4_BOOT_ENABLED ? "enabled (HSEM 0)" : "DISABLED - CPU2 not started");

    if (xTaskCreate(Heartbeat_Task, "hb", HEARTBEAT_TASK_STACK_WORDS, NULL,
                    HEARTBEAT_TASK_PRIO, NULL) != pdPASS ||
        xTaskCreate(Status_Task, "status", STATUS_TASK_STACK_WORDS, NULL,
                    STATUS_TASK_PRIO, NULL) != pdPASS ||
        xTaskCreate(Net_TaskEntry, "net", NET_APP_THREAD_STACK_WORDS, NULL,
                    NET_APP_THREAD_PRIO, NULL) != pdPASS)
    {
        APP_LOG("[sys] FATAL: could not create the application tasks");
        Error_Handler();
    }

    LED_BootStage(LED_BOOT_SCHEDULER);
    vTaskStartScheduler();

    /* Only reached if the kernel could not allocate the idle or timer task. */
    Error_Handler();
}

/* ---- Tasks -------------------------------------------------------------- */

static void Heartbeat_Task(void *arg)
{
    TickType_t last = xTaskGetTickCount();

    (void)arg;

    for (;;)
    {
        /* A single aligned word: the write is atomic on this core, so no HSEM
           is needed. Anything wider would need SHARED_LOCK(). */
        g_shared.m7_heartbeat++;

        LED_Toggle(LED_GREEN);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

__attribute__((unused)) static bool DrainMbox(char *dest, size_t dest_size)
{
    bool has_data = false;
    SHARED_LOCK(HSEM_ID_M4_TO_M7_MBOX);
    if (g_shared.m4_to_m7_len > 0U)
    {
        uint32_t len = g_shared.m4_to_m7_len;
        if (len >= dest_size)
        {
            len = (uint32_t)(dest_size - 1U);
        }
        memcpy(dest, (const void *)g_shared.m4_to_m7, len);
        dest[len] = '\0';
        g_shared.m4_to_m7_len = 0U;
        has_data = true;
    }
    SHARED_UNLOCK(HSEM_ID_M4_TO_M7_MBOX);
    return has_data;
}

static void Status_Task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    uint32_t   previous_m4 = 0U;
    bool       have_previous = false;

    (void)arg;

    for (;;)
    {
        uint32_t    m4 = g_shared.m4_heartbeat;
        const char *state;

        /* The verdict compares two samples, so the first pass has nothing to
           compare against, and reporting a verdict there would be a coin toss:
           Shared_Init() leaves the block zeroed and CPU1 can reach this task
           before CPU2 has woken from STOP. This firmware happens to lose that
           race - the banner, the magic check and lwIP give CPU2 time - but
           that is timing, not design, and trimming the work before the
           scheduler is enough to flip it. Kept in step with
           03_firmware_templates/prj_01_template_dualcore, where it did fire on
           the opening line of every boot. */
        if (m4 != previous_m4)
        {
            state = "alive";
        }
        else if (!have_previous)
        {
            state = "starting";
        }
        else
        {
            state = "NOT RESPONDING";
        }

#if defined(UART_DRIVER_SELFTEST) && UART_DRIVER_SELFTEST
        char mbox_uart[SHARED_MBOX_SIZE];
        if (DrainMbox(mbox_uart, sizeof(mbox_uart)))
        {
            APP_LOG("[m4_uart_test] %s", mbox_uart);
        }
#endif

#if defined(DMA_DRIVER_SELFTEST) && DMA_DRIVER_SELFTEST
        char mbox_dma[SHARED_MBOX_SIZE];
        if (DrainMbox(mbox_dma, sizeof(mbox_dma)))
        {
            APP_LOG("[m4_dma_test] %s", mbox_dma);
        }

        static bool s_m7_dma_tested = false;
        if (!s_m7_dma_tested)
        {
            s_m7_dma_tested = true;
            DMA_SelfTest_CM7_Run();
        }
#endif

#if defined(HSEM_DRIVER_SELFTEST) && HSEM_DRIVER_SELFTEST
        char mbox_hsem[SHARED_MBOX_SIZE];
        if (DrainMbox(mbox_hsem, sizeof(mbox_hsem)))
        {
            APP_LOG("[m4_hsem_test] %s", mbox_hsem);
        }

        static bool s_m7_hsem_tested = false;
        if (!s_m7_hsem_tested)
        {
            s_m7_hsem_tested = true;
            HSEM_SelfTest_CM7_Run();
        }
#endif

#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST
        static bool s_dcache_tested = false;
        if (!s_dcache_tested)
        {
            s_dcache_tested = true;
            /* Test T8: D-cache coherency test on AXI SRAM buffer using DMA2 M2M and ComputeStats (BM & LL) */
            __attribute__((section(".axi_ram"), aligned(32))) static uint16_t s_dcache_buf[64];
            __attribute__((section(".dma_buf"), aligned(32))) static uint16_t s_dma_src[64];

            for (uint32_t i = 0U; i < 64U; i++)
            {
                s_dma_src[i] = 2048U;
            }

            /* 1. Allocate lines in L1 D-cache and flush clean to physical RAM */
            for (uint32_t i = 0U; i < 64U; i++)
            {
                s_dcache_buf[i] = 0x1111U;
            }
            SCB_CleanDCache_by_Addr((uint32_t *)s_dcache_buf, sizeof(s_dcache_buf));
            __DSB();

            /* 2. Overwrite in cache (dirty lines in L1 D-cache: first half 0x9999, second half 0x8888) */
            for (uint32_t i = 0U; i < 32U; i++)
            {
                s_dcache_buf[i] = 0x9999U;
            }
            for (uint32_t i = 32U; i < 64U; i++)
            {
                s_dcache_buf[i] = 0x8888U;
            }
            __DMB();

            /* 3. Configure DMA2 Stream 0 in Memory-to-Memory mode to transfer s_dma_src into s_dcache_buf */
            RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
            volatile uint32_t dummy = RCC->AHB1ENR;
            (void)dummy;

            DMA2_Stream0->CR &= ~DMA_SxCR_EN;
            while ((DMA2_Stream0->CR & DMA_SxCR_EN) != 0U) {}

            DMA2->LIFCR = (DMA_LIFCR_CTCIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0);
            DMA2_Stream0->PAR  = (uint32_t)s_dma_src;
            DMA2_Stream0->M0AR = (uint32_t)s_dcache_buf;
            DMA2_Stream0->NDTR = 64U;
            /* RM0399 §15.3.18: In M2M mode, FIFO mode is forced by hardware (DMDIS=1) */
            DMA2_Stream0->FCR  = DMA_SxFCR_FTH_1;
            DMA2_Stream0->CR   = DMA_SxCR_DIR_1 | DMA_SxCR_MINC | DMA_SxCR_PINC | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_PL;
            DMA2_Stream0->CR  |= DMA_SxCR_EN;

            /* Wait for DMA transfer complete with timeout and transfer error check (W4) */
            uint32_t dma_timeout = 1000000U;
            while (((DMA2->LISR & (DMA_LISR_TCIF0 | DMA_LISR_TEIF0)) == 0U) && (--dma_timeout > 0U)) {}
            bool dma_ok = ((DMA2->LISR & DMA_LISR_TCIF0) != 0U) && ((DMA2->LISR & DMA_LISR_TEIF0) == 0U) && (dma_timeout > 0U);
            DMA2->LIFCR = (DMA_LIFCR_CTCIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0);

            /* 4. Direct CPU read before invalidation: hits dirty cache line (0x9999 = 39321) */
            uint16_t raw_bm_before = s_dcache_buf[0];
            uint16_t raw_ll_before = s_dcache_buf[32];

            /* 5. ComputeStats on first half using BM (internally calls SCB_InvalidateDCache_by_Addr) */
            AdcBm_Handle_t test_bm = {0};
            test_bm.config_ex.buffer = s_dcache_buf;
            test_bm.config_ex.buffer_length = 64U;
            test_bm.config_ex.num_channels = 1U;
            test_bm.config_ex.resolution = ADC_BM_RESOLUTION_16B;

            ADC_BM_ComputeStats(&test_bm, 0U, 3300U);
            uint16_t bm_stats_avg = test_bm.stats.average_raw;
            uint16_t raw_bm_after = s_dcache_buf[0];

            /* 6. ComputeStats on second half using LL (internally calls SCB_InvalidateDCache_by_Addr) */
            AdcLl_Handle_t test_ll = {0};
            test_ll.config_ex.buffer = s_dcache_buf;
            test_ll.config_ex.buffer_length = 64U;
            test_ll.config_ex.num_channels = 1U;
            test_ll.config_ex.resolution = ADC_LL_RESOLUTION_16B;

            ADC_LL_ComputeStats(&test_ll, 1U, 3300U);
            uint16_t ll_stats_avg = test_ll.stats.average_raw;
            uint16_t raw_ll_after = s_dcache_buf[32];

            bool align_ok = (((uint32_t)s_dcache_buf & 0x1FUL) == 0U);
            bool bm_verified = (raw_bm_before == 0x9999U) && (bm_stats_avg == 2048U) && (raw_bm_after == 2048U);
            bool ll_verified = (raw_ll_before == 0x8888U) && (ll_stats_avg == 2048U) && (raw_ll_after == 2048U);
            bool dcache_verified = dma_ok && align_ok && bm_verified && ll_verified;

            APP_LOG("[cm7_dcache_test] %s: 32B alignment=%d, dma_dcache_verified=%d (BM: dirty=%u->%u, LL: dirty=%u->%u)",
                    dcache_verified ? "PASS" : "FAIL",
                    align_ok ? 1 : 0,
                    dcache_verified ? 1 : 0,
                    (unsigned int)raw_bm_before,
                    (unsigned int)bm_stats_avg,
                    (unsigned int)raw_ll_before,
                    (unsigned int)ll_stats_avg);
        }

        char mbox_adc[SHARED_MBOX_SIZE];
        if (DrainMbox(mbox_adc, sizeof(mbox_adc)))
        {
            APP_LOG("[m4_adc_test] %s", mbox_adc);
        }
#endif

        APP_LOG("[ipc] m7=%lu m4=%lu  (CPU2 %s)",
                (unsigned long)g_shared.m7_heartbeat,
                (unsigned long)m4, state);

        previous_m4   = m4;
        have_previous = true;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(STATUS_PERIOD_MS));
    }
}

/* ---- FreeRTOS hooks ----------------------------------------------------- */

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    APP_LOG("[sys] FATAL: stack overflow in task %s", task_name);
    Error_Handler();
}

void vApplicationMallocFailedHook(void)
{
    APP_LOG("[sys] FATAL: FreeRTOS heap exhausted");
    Error_Handler();
}
