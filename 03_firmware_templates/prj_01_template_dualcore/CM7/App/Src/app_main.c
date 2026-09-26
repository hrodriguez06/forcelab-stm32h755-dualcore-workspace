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
  *                       useful symptom this template can give you: it means
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
    APP_LOG("=== STM32H755ZI dual-core template - CPU1 (Cortex-M7) ===");
    APP_LOG("[sys] sysclk=%lu Hz hclk=%lu Hz",
            (unsigned long)HAL_RCC_GetSysClockFreq(),
            (unsigned long)HAL_RCC_GetHCLKFreq());
    APP_LOG("[sys] silicon revision id=0x%04lX",
            (unsigned long)((DBGMCU->IDCODE & DBGMCU_IDCODE_REV_ID_Msk) >>
                            DBGMCU_IDCODE_REV_ID_Pos));

    /* Shared memory was already initialised by CPU1 in main() before CPU2
       was released from HSEM 0. Verify the magic word before proceeding. */
    if (g_shared.magic != SHARED_MAGIC)
    {
        APP_LOG("[ipc] FATAL: shared block magic mismatch (expected 0x%08lX, got 0x%08lX)",
                (unsigned long)SHARED_MAGIC, (unsigned long)g_shared.magic);
        Error_Handler();
    }
    APP_LOG("[ipc] shared block at %p, magic=0x%08lX",
            (void *)&g_shared, (unsigned long)g_shared.magic);
    APP_LOG("[ipc] CPU2 boot handshake: %s",
            CM4_BOOT_ENABLED ? "enabled (HSEM 0)" : "DISABLED - CPU2 not started");

    if (xTaskCreate(Heartbeat_Task, "hb", HEARTBEAT_TASK_STACK_WORDS, NULL,
                    HEARTBEAT_TASK_PRIO, NULL) != pdPASS ||
        xTaskCreate(Status_Task, "status", STATUS_TASK_STACK_WORDS, NULL,
                    STATUS_TASK_PRIO, NULL) != pdPASS)
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
           compare against. Saying NOT RESPONDING there is a lie the timing
           makes easy to hit: Shared_Init() leaves the block zeroed, and CPU1
           can reach this task before CPU2 has woken from STOP and reached its
           first increment - all the more so once the application before the
           scheduler is leaner than this template's. Reporting "starting"
           instead costs one sample of delay and keeps a zero counter meaning
           what the file header says it means. */
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
