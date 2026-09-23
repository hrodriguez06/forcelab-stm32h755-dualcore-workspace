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
            /* Test T8: D-cache coherency test on AXI SRAM buffer */
            __attribute__((section(".axi_ram"), aligned(32))) static volatile uint32_t s_dcache_test_buf[8];

            /* 1. Write baseline value to buffer and flush to physical RAM */
            s_dcache_test_buf[0] = 0x11223344UL;
            SCB_CleanDCache_by_Addr((uint32_t *)s_dcache_test_buf, 32);
            __DSB();

            /* 2. Overwrite in cache (dirty line in L1 D-cache) */
            s_dcache_test_buf[0] = 0xDEADBEEFUL;
            __DMB();

            /* 3. Invalidate line WITHOUT clean - discards dirty data from L1 D-cache */
            SCB_InvalidateDCache_by_Addr((uint32_t *)s_dcache_test_buf, 32);
            __DSB();
            __ISB();

            /* 4. Read back: must hit RAM and return original baseline 0x11223344UL */
            uint32_t readback = s_dcache_test_buf[0];
            bool dcache_invalidate_real = (readback == 0x11223344UL);
            bool align_ok = (((uint32_t)s_dcache_test_buf & 0x1FUL) == 0U);

            APP_LOG("[cm7_dcache_test] %s: 32B alignment=%d, discard_dirty_verified=%d (readback=0x%08lX)",
                    (dcache_invalidate_real && align_ok) ? "PASS" : "FAIL",
                    align_ok ? 1 : 0,
                    dcache_invalidate_real ? 1 : 0,
                    (unsigned long)readback);
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
