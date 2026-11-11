/**
  ******************************************************************************
  * @file    app_main.c
  * @brief   Application entry point of CPU1 (Cortex-M7) for `prj_05_adc_cwt_eth`.
  *
  *          FreeRTOS Tasks on Cortex-M7:
  *            1. Heartbeat_Task : Blinks LD1 (green) and increments m7_heartbeat.
  *            2. Status_Task    : Logs CPU2 (CM4) liveness, 1.5 V DC offset
  *                                telemetry, and CWT execution time over USART3.
  *            3. vCwtProcessTask: Consumes 512-sample windows from SRAM4 under
  *                                HSEM 4, removes the 1.5 V DC offset, and
  *                                computes the 24-scale Complex Morlet CWT.
  *            4. Net_TaskEntry  : Starts lwIP TCP/IP stack, LAN8742 PHY link
  *                                supervision, and UDP CWT server on `:5001`.
  ******************************************************************************
  */

#include "app_main.h"
#include "app_log.h"
#include "main.h"
#include "led_bm.h"
#include "shared_memory.h"
#include "net_config.h"
#include "net_task.h"
#include "cwt_processor.h"

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

#define CWT_TASK_STACK_WORDS         1024U
#define CWT_TASK_PRIO                (tskIDLE_PRIORITY + 2U)

static void Heartbeat_Task(void *arg);
static void Status_Task(void *arg);

/* Static snapshot for status task logging */
static CwtFrameResult_t s_status_cwt_snap __attribute__((section(".axi_ram"), aligned(32)));

void app_main(void)
{
    LED_BootStage(LED_BOOT_APP_ENTERED);
    App_Log_Init();

#ifndef FW_GIT_REV
#define FW_GIT_REV "unknown"
#endif

    APP_LOG(" ");
    APP_LOG("=== STM32H755ZI Dual-Core Morlet CWT + Ethernet - CPU1 (Cortex-M7) - Rev %s ===", FW_GIT_REV);
    APP_LOG("[sys] sysclk=%lu Hz hclk=%lu Hz",
            (unsigned long)HAL_RCC_GetSysClockFreq(),
            (unsigned long)HAL_RCC_GetHCLKFreq());
    APP_LOG("[sys] silicon revision id=0x%04lX",
            (unsigned long)((DBGMCU->IDCODE & DBGMCU_IDCODE_REV_ID_Msk) >>
                            DBGMCU_IDCODE_REV_ID_Pos));
    APP_LOG("[adc] input=%s (ADC1_INP%u, %lu SPS, nominal DC offset=%u mV / raw=%u)",
            BOARD_ADC_PIN_NAME,
            (unsigned)BOARD_ADC_CHANNEL,
            (unsigned long)BOARD_ADC_SAMPLING_RATE_HZ,
            (unsigned)ADC_NOMINAL_OFFSET_MV,
            (unsigned)ADC_NOMINAL_OFFSET_RAW);

    APP_LOG("[ipc] shared block at %p, magic=0x%08lX",
            (void *)&g_shared, (unsigned long)g_shared.magic);
    if (g_shared.magic != SHARED_MAGIC)
    {
        APP_LOG("[ipc] FATAL: shared block magic mismatch (expected 0x%08lX, got 0x%08lX)",
                (unsigned long)SHARED_MAGIC, (unsigned long)g_shared.magic);
        Error_Handler();
    }

    /* Precompute FFT twiddle tables and Complex Morlet daughter wavelets */
    CWT_Init();

    if (xTaskCreate(Heartbeat_Task, "hb", HEARTBEAT_TASK_STACK_WORDS, NULL,
                    HEARTBEAT_TASK_PRIO, NULL) != pdPASS ||
        xTaskCreate(Status_Task, "status", STATUS_TASK_STACK_WORDS, NULL,
                    STATUS_TASK_PRIO, NULL) != pdPASS ||
        xTaskCreate(vCwtProcessTask, "cwt_dsp", CWT_TASK_STACK_WORDS, NULL,
                    CWT_TASK_PRIO, NULL) != pdPASS ||
        xTaskCreate(Net_TaskEntry, "net", NET_APP_THREAD_STACK_WORDS, NULL,
                    NET_APP_THREAD_PRIO, NULL) != pdPASS)
    {
        APP_LOG("[sys] FATAL: could not create the application tasks");
        Error_Handler();
    }

    LED_BootStage(LED_BOOT_SCHEDULER);
    vTaskStartScheduler();

    Error_Handler();
}

static void Heartbeat_Task(void *arg)
{
    TickType_t last = xTaskGetTickCount();

    (void)arg;

    for (;;)
    {
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

        if (CWT_GetLatestFrame(&s_status_cwt_snap))
        {
            uint32_t peak_f_hz = (uint32_t)(s_status_cwt_snap.peak_freq_hz + 0.5f);
            uint32_t peak_m_mv = (uint32_t)(s_status_cwt_snap.max_magnitude_mv + 0.5f);
            APP_LOG("[cwt] m7=%lu m4=%lu (%s) | seq=%lu dc=%u mV (err=%+d mV) vpp=%u mV | "
                    "cwt=%lu us (%lu cyc) peak=%lu Hz (%lu mV)",
                    (unsigned long)g_shared.m7_heartbeat,
                    (unsigned long)m4,
                    state,
                    (unsigned long)s_status_cwt_snap.frame_seq,
                    (unsigned)s_status_cwt_snap.measured_dc_mv,
                    (int)s_status_cwt_snap.dc_offset_error_mv,
                    (unsigned)s_status_cwt_snap.vpp_mv,
                    (unsigned long)s_status_cwt_snap.cwt_exec_us,
                    (unsigned long)s_status_cwt_snap.cwt_exec_cycles,
                    (unsigned long)peak_f_hz,
                    (unsigned long)peak_m_mv);
        }
        else
        {
            APP_LOG("[ipc] m7=%lu m4=%lu  (CPU2 %s)",
                    (unsigned long)g_shared.m7_heartbeat,
                    (unsigned long)m4, state);
        }

        previous_m4   = m4;
        have_previous = true;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(STATUS_PERIOD_MS));
    }
}

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
