/**
  ******************************************************************************
  * @file    app_main.c
  * @brief   Application entry point of CPU1 (Cortex-M7).
  *
  *          Creates the networking task and hands the CPU to FreeRTOS. Every
  *          lwIP object is created from inside that task, because tcpip_init()
  *          needs a running scheduler.
  ******************************************************************************
  */

#include "app_main.h"
#include "app_log.h"
#include "net_config.h"
#include "net_task.h"
#include "main.h"
#include "led_bm.h"

#include "FreeRTOS.h"
#include "task.h"

void app_main(void)
{
    LED_BootStage(LED_BOOT_APP_ENTERED);
    App_Log_Init();

    APP_LOG(" ");
    APP_LOG("=== STM32H755ZI CPU1 - Ethernet / lwIP node ===");
    APP_LOG("[sys] sysclk=%lu Hz hclk=%lu Hz",
            (unsigned long)HAL_RCC_GetSysClockFreq(),
            (unsigned long)HAL_RCC_GetHCLKFreq());
    APP_LOG("[sys] silicon revision id=0x%04lX",
            (unsigned long)((DBGMCU->IDCODE & DBGMCU_IDCODE_REV_ID_Msk) >>
                            DBGMCU_IDCODE_REV_ID_Pos));

    if (xTaskCreate(Net_TaskEntry, "net", NET_APP_THREAD_STACK_WORDS, NULL,
                    NET_APP_THREAD_PRIO, NULL) != pdPASS)
    {
        APP_LOG("[sys] FATAL: could not create the networking task");
        Error_Handler();
    }

    LED_BootStage(LED_BOOT_SCHEDULER);
    vTaskStartScheduler();

    /* Only reached if the kernel could not allocate the idle or timer task. */
    Error_Handler();
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
