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
#include "adc_bm.h"

#include <stdbool.h>

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
        SHARED_LOCK(HSEM_ID_M4_TO_M7_MBOX);
        if (g_shared.m4_to_m7_len > 0U)
        {
            APP_LOG("[m4_uart_test] %s", (char *)g_shared.m4_to_m7);
            g_shared.m4_to_m7_len = 0U;
        }
        SHARED_UNLOCK(HSEM_ID_M4_TO_M7_MBOX);
#endif

#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST
        static bool s_dcache_tested = false;
        if (!s_dcache_tested)
        {
            s_dcache_tested = true;
            /* Test T8: D-cache coherency test on AXI SRAM buffer using DMA2 M2M and ComputeStats */
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

            /* 2. Overwrite in cache (dirty lines in L1 D-cache with 0x9999 = 39321) */
            for (uint32_t i = 0U; i < 64U; i++)
            {
                s_dcache_buf[i] = 0x9999U;
            }
            __DMB();

            /* 2. Configure DMA2 Stream 0 in Memory-to-Memory mode to transfer s_dma_src into s_dcache_buf */
            RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
            volatile uint32_t dummy = RCC->AHB1ENR;
            (void)dummy;

            DMA2_Stream0->CR &= ~DMA_SxCR_EN;
            while ((DMA2_Stream0->CR & DMA_SxCR_EN) != 0U) {}

            DMA2->LIFCR = (DMA_LIFCR_CTCIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0);
            DMA2_Stream0->PAR  = (uint32_t)s_dma_src;
            DMA2_Stream0->M0AR = (uint32_t)s_dcache_buf;
            DMA2_Stream0->NDTR = 64U;
            DMA2_Stream0->FCR  = 0U; /* Direct mode */
            DMA2_Stream0->CR   = DMA_SxCR_DIR_1 | DMA_SxCR_MINC | DMA_SxCR_PINC | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_PL;
            DMA2_Stream0->CR  |= DMA_SxCR_EN;

            /* Wait for DMA transfer complete */
            while ((DMA2->LISR & DMA_LISR_TCIF0) == 0U) {}
            DMA2->LIFCR = (DMA_LIFCR_CTCIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0);

            /* 3. Direct CPU read before invalidation: hits dirty cache line (0x9999 = 39321) */
            uint16_t raw_direct_before = s_dcache_buf[0];

            /* 4. ComputeStats internally invalidates D-cache and computes stats from physical RAM */
            AdcBm_Handle_t test_bm = {0};
            test_bm.config_ex.buffer = s_dcache_buf;
            test_bm.config_ex.buffer_length = 64U;
            test_bm.config_ex.num_channels = 1U;
            test_bm.config_ex.resolution = ADC_BM_RESOLUTION_16B;

            ADC_BM_ComputeStats(&test_bm, 0U, 3300U);
            uint16_t stats_avg = test_bm.stats.average_raw;

            /* 5. Direct CPU read after ComputeStats: now returns 2048 from refreshed cache line */
            uint16_t raw_direct_after = s_dcache_buf[0];

            bool align_ok = (((uint32_t)s_dcache_buf & 0x1FUL) == 0U);
            bool dcache_verified = (raw_direct_before == 0x9999U) && (stats_avg == 2048U) && (raw_direct_after == 2048U);

            APP_LOG("[cm7_dcache_test] %s: 32B alignment=%d, dma_dcache_verified=%d (dirty_cpu=%u, stats_dma=%u)",
                    (dcache_verified && align_ok) ? "PASS" : "FAIL",
                    align_ok ? 1 : 0,
                    dcache_verified ? 1 : 0,
                    (unsigned int)raw_direct_before,
                    (unsigned int)stats_avg);
        }

        SHARED_LOCK(HSEM_ID_M4_TO_M7_MBOX);
        if (g_shared.m4_to_m7_len > 0U)
        {
            APP_LOG("[m4_adc_test] %s", (char *)g_shared.m4_to_m7);
            g_shared.m4_to_m7_len = 0U;
        }
        SHARED_UNLOCK(HSEM_ID_M4_TO_M7_MBOX);
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
