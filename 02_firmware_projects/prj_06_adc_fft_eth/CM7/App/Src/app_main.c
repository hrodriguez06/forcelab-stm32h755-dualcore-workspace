/**
  ******************************************************************************
  * @file    app_main.c
  * @brief   Application entry point of CPU1 (Cortex-M7) for `prj_06_adc_fft_eth`.
  *
  *          FreeRTOS Tasks on Cortex-M7:
  *            1. Heartbeat_Task : Blinks LD1 (green) and increments m7_heartbeat.
  *            2. Status_Task    : Logs CPU2 (CM4) liveness/HardFault diagnostics,
  *                                4096-pt FFT metrics, and 16-band db4 WPT metrics
  *                                over USART3 every 2 seconds.
  *            3. vDspProcessTask: Polls D3 SRAM4 (`g_shared.dsp`) for completed
  *                                4096-sample blocks from CM4, snapshots the
  *                                ready buffer into cached D1 AXI SRAM, runs
  *                                `DSP_Engine_ProcessBlock` (4096-pt Blackman-
  *                                Harris RFFT) and `DSP_Wavelet_ProcessBlock`
  *                                (4-level db4 WPT + HF transient detector),
  *                                updates `g_shared.dsp`, and streams UDP `:5002`
  *                                telemetry packets.
  *            4. Net_TaskEntry  : Starts lwIP TCP/IP stack, LAN8742 PHY link
  *                                supervision, UDP echo (`:7`), and UDP DSP
  *                                server (`:5002`).
  ******************************************************************************
  */

#include "app_main.h"
#include "app_log.h"
#include "main.h"
#include "led_bm.h"
#include "shared_memory.h"
#include "net_config.h"
#include "net_task.h"
#include "dsp_engine.h"
#include "dsp_wavelet.h"
#include "udp_dsp.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#define HEARTBEAT_TASK_STACK_WORDS   256U
#define HEARTBEAT_TASK_PRIO          (tskIDLE_PRIORITY + 1U)
#define HEARTBEAT_PERIOD_MS          500U

#define STATUS_TASK_STACK_WORDS      512U
#define STATUS_TASK_PRIO             (tskIDLE_PRIORITY + 1U)
#define STATUS_PERIOD_MS             2000U

#define DSP_TASK_STACK_WORDS         1024U
#define DSP_TASK_PRIO                (tskIDLE_PRIORITY + 2U)
#define DSP_POLL_PERIOD_MS           5U

/* Working buffers for `vDspProcessTask` in cached D1 AXI SRAM (0x24000000) */
__attribute__((section(".axi_ram"), aligned(32)))
static uint16_t s_local_raw_block[DSP_BLOCK_SIZE];

__attribute__((section(".axi_ram"), aligned(32)))
static DSP_Result_t s_fft_result;

__attribute__((section(".axi_ram"), aligned(32)))
static WPT_Result_t s_wpt_result;

/* Snapshot buffers for `Status_Task` logging */
__attribute__((section(".axi_ram"), aligned(32)))
static DSP_Result_t s_status_fft;

__attribute__((section(".axi_ram"), aligned(32)))
static WPT_Result_t s_status_wpt;

static void Heartbeat_Task(void *arg);
static void Status_Task(void *arg);
static void vDspProcessTask(void *arg);

void app_main(void)
{
    LED_BootStage(LED_BOOT_APP_ENTERED);
    App_Log_Init();

#ifndef FW_GIT_REV
#define FW_GIT_REV "unknown"
#endif

    APP_LOG(" ");
    APP_LOG("=== STM32H755ZI Dual-Core 4096-pt FFT + 16-Band db4 WPT + Ethernet - CPU1 (CM7) - Rev %s ===",
            FW_GIT_REV);
    APP_LOG("[sys] sysclk=%lu Hz hclk=%lu Hz",
            (unsigned long)HAL_RCC_GetSysClockFreq(),
            (unsigned long)HAL_RCC_GetHCLKFreq());
    APP_LOG("[sys] silicon revision id=0x%04lX",
            (unsigned long)((DBGMCU->IDCODE & DBGMCU_IDCODE_REV_ID_Msk) >>
                            DBGMCU_IDCODE_REV_ID_Pos));
    APP_LOG("[adc] input=%s (ADC1_INP%u, default %lu SPS, block=%u samples)",
            BOARD_ADC_PIN_NAME,
            (unsigned)BOARD_ADC_CHANNEL,
            (unsigned long)BOARD_ADC_SAMPLING_RATE_HZ,
            (unsigned)DSP_BLOCK_SIZE);

    APP_LOG("[ipc] shared block at %p (%u bytes), magic=0x%08lX",
            (void *)&g_shared,
            (unsigned)sizeof(g_shared),
            (unsigned long)g_shared.magic);
    if (g_shared.magic != SHARED_MAGIC)
    {
        APP_LOG("[ipc] FATAL: shared block magic mismatch (expected 0x%08lX, got 0x%08lX)",
                (unsigned long)SHARED_MAGIC, (unsigned long)g_shared.magic);
        Error_Handler();
    }

    /* Initialize 4096-pt Blackman-Harris FFT and 4-Level db4 WPT engines */
    if (DSP_Engine_Init() != 0)
    {
        APP_LOG("[dsp] FATAL: CMSIS-DSP 4096-pt RFFT init failed");
        Error_Handler();
    }
    DSP_Wavelet_Init();

    if (xTaskCreate(Heartbeat_Task, "hb", HEARTBEAT_TASK_STACK_WORDS, NULL,
                    HEARTBEAT_TASK_PRIO, NULL) != pdPASS ||
        xTaskCreate(Status_Task, "status", STATUS_TASK_STACK_WORDS, NULL,
                    STATUS_TASK_PRIO, NULL) != pdPASS ||
        xTaskCreate(vDspProcessTask, "fft_wpt", DSP_TASK_STACK_WORDS, NULL,
                    DSP_TASK_PRIO, NULL) != pdPASS ||
        xTaskCreate(Net_TaskEntry, "net", NET_APP_THREAD_STACK_WORDS, NULL,
                    NET_APP_THREAD_PRIO, NULL) != pdPASS)
    {
        APP_LOG("[sys] FATAL: could not create the FreeRTOS application tasks");
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

static void vDspProcessTask(void *arg)
{
    uint32_t last_block_id = 0U;

    (void)arg;

    for (;;)
    {
        uint32_t cur_block_id  = 0U;
        uint32_t ready_idx     = 0U;
        uint32_t fs_hz         = BOARD_ADC_SAMPLING_RATE_HZ;
        uint32_t overrun_count = 0U;
        bool     has_new_block = false;

        if (Shared_TryLockCycles(HSEM_ID_ADC_DATA, SHARED_ADC_TRYLOCK_CYCLES))
        {
            uint32_t t_hold = DWT->CYCCNT;
            cur_block_id = g_shared.dsp.block_id;
            if ((cur_block_id != last_block_id) &&
                ((g_shared.dsp.flags & SHARED_ADC_FLAG_DATA_VALID) != 0U))
            {
                ready_idx     = g_shared.dsp.ready_idx & 1U;
                fs_hz         = g_shared.dsp.fs_hz;
                overrun_count = g_shared.dsp.overrun_count;
                has_new_block = true;
            }
            SHARED_UNLOCK(HSEM_ID_ADC_DATA);

            uint32_t hold_cyc = DWT->CYCCNT - t_hold;
            if (hold_cyc > g_shared.m7_adc_lock_hold_cycles_max)
            {
                g_shared.m7_adc_lock_hold_cycles_max = hold_cyc;
            }
        }
        else
        {
            g_shared.m7_adc_lock_fail_count++;
        }

        if (!has_new_block)
        {
            vTaskDelay(pdMS_TO_TICKS(DSP_POLL_PERIOD_MS));
            continue;
        }

        last_block_id = cur_block_id;

        /* Copy 4096 raw samples from non-cacheable SRAM4 into cached AXI SRAM */
        memcpy(s_local_raw_block,
               (const void *)g_shared.dsp.raw_buffer[ready_idx],
               sizeof(s_local_raw_block));

        uint32_t t_start_cyc = DWT->CYCCNT;

        /* 1. Run 4096-point Blackman-Harris RFFT and harmonic analysis */
        DSP_Engine_ProcessBlock(s_local_raw_block,
                                fs_hz,
                                cur_block_id,
                                &s_fft_result);

        /* 2. Run 4-Level Daubechies-4 (db4) Wavelet Packet Transform (16 bands) */
        DSP_Wavelet_ProcessBlock(s_local_raw_block,
                                 fs_hz,
                                 cur_block_id,
                                 s_fft_result.dc_offset_mv,
                                 &s_wpt_result);

        uint32_t exec_cycles = DWT->CYCCNT - t_start_cyc;
        uint32_t exec_us     = exec_cycles / (BOARD_SYSCLK_HZ / 1000000UL);
        UDP_DSP_SetLastExecTimeUs(exec_us);

        /* 3. Publish summary metrics back into shared SRAM4 under HSEM 4 */
        if (Shared_TryLockCycles(HSEM_ID_ADC_DATA, SHARED_ADC_TRYLOCK_CYCLES))
        {
            g_shared.dsp.dc_offset_mv   = (uint16_t)(s_fft_result.dc_offset_mv + 0.5f);
            g_shared.dsp.v_pp_mv        = (uint16_t)(s_fft_result.v_pp_mv + 0.5f);
            g_shared.dsp.f0_hz_x10      = (uint32_t)(s_fft_result.f0_hz * 10.0f + 0.5f);
            g_shared.dsp.f0_mag_mv      = (uint16_t)(s_fft_result.f0_mag_mv + 0.5f);
            g_shared.dsp.noise_floor_mv = (uint16_t)(s_fft_result.noise_floor_mv + 0.5f);
            g_shared.dsp.snr_db_x10     = (int16_t)(s_fft_result.snr_db * 10.0f);
            g_shared.dsp.thd_pct_x10    = (uint16_t)(s_fft_result.thd_pct * 10.0f + 0.5f);

            for (uint32_t h = 0U; h < DSP_MAX_HARMONICS; h++)
            {
                g_shared.dsp.harm_mag_mv[h] = (uint16_t)(s_fft_result.harm_mag_mv[h] + 0.5f);
            }

            for (uint32_t b = 0U; b < WPT_NUM_BANDS; b++)
            {
                float e_x10 = s_wpt_result.energy_pct[b] * 10.0f + 0.5f;
                if (e_x10 > 1000.0f) { e_x10 = 1000.0f; }
                g_shared.dsp.wpt_energy_pct_x10[b] = (uint16_t)e_x10;
                g_shared.dsp.wpt_rms_mv[b]         = (uint16_t)(s_wpt_result.rms_mv[b] + 0.5f);
            }

            g_shared.dsp.wpt_transient_mag_mv   = (uint16_t)(s_wpt_result.transient_mag_mv + 0.5f);
            g_shared.dsp.wpt_transient_sample   = s_wpt_result.transient_sample;
            g_shared.dsp.wpt_transient_count    = s_wpt_result.transient_count;
            g_shared.dsp.wpt_crest_x10          = (uint16_t)(s_wpt_result.crest_factor_hf * 10.0f + 0.5f);
            g_shared.dsp.wpt_entropy_norm_x1000 = (uint16_t)(s_wpt_result.entropy_norm * 1000.0f + 0.5f);
            g_shared.dsp.fft_ready              = 1U;
            g_shared.dsp.flags                 |= SHARED_DSP_FLAG_FFT_READY;
            __DMB();
            SHARED_UNLOCK(HSEM_ID_ADC_DATA);
        }

        /* 4. Update UDP server snapshot and stream 144-byte packet if active */
        UDP_DSP_PublishAndStream(&s_fft_result,
                                 &s_wpt_result,
                                 s_local_raw_block,
                                 overrun_count);
    }
}

static void Status_Task(void *arg)
{
    TickType_t last          = xTaskGetTickCount();
    uint32_t   previous_m4   = 0U;
    bool       have_previous = false;

    (void)arg;

    for (;;)
    {
        uint32_t    m4 = g_shared.m4_heartbeat;
        const char *state;

        if (g_shared.dsp.fault_flag != 0U)
        {
            APP_LOG("[FAULT] CM4 HardFault! CFSR=0x%08lX HFSR=0x%08lX BFAR=0x%08lX PC=0x%08lX LR=0x%08lX",
                    (unsigned long)g_shared.dsp.fault_cfsr,
                    (unsigned long)g_shared.dsp.fault_hfsr,
                    (unsigned long)g_shared.dsp.fault_bfar,
                    (unsigned long)g_shared.dsp.fault_pc,
                    (unsigned long)g_shared.dsp.fault_lr);
            vTaskDelayUntil(&last, pdMS_TO_TICKS(STATUS_PERIOD_MS));
            continue;
        }

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

        uint32_t exec_us = 0U;
        if (UDP_DSP_GetLatestResults(&s_status_fft, &s_status_wpt, &exec_us) != 0)
        {
            uint32_t f0_int    = (uint32_t)(s_status_fft.f0_hz * 10.0f + 0.5f) / 10U;
            uint32_t f0_dec    = (uint32_t)(s_status_fft.f0_hz * 10.0f + 0.5f) % 10U;
            int      snr_x10   = (int)(s_status_fft.snr_db * 10.0f);
            int      snr_int   = snr_x10 / 10;
            int      snr_dec   = abs(snr_x10) % 10;
            uint32_t thd_int   = (uint32_t)(s_status_fft.thd_pct * 10.0f + 0.5f) / 10U;
            uint32_t thd_dec   = (uint32_t)(s_status_fft.thd_pct * 10.0f + 0.5f) % 10U;
            uint32_t ent_x1000 = (uint32_t)(s_status_wpt.entropy_norm * 1000.0f + 0.5f);
            uint32_t crest_int = (uint32_t)(s_status_wpt.crest_factor_hf * 10.0f + 0.5f) / 10U;
            uint32_t crest_dec = (uint32_t)(s_status_wpt.crest_factor_hf * 10.0f + 0.5f) % 10U;

            APP_LOG("[dsp] m7=%lu m4=%lu (%s) | blk=%lu fs=%lu Hz dc=%u mV vpp=%u mV | "
                    "f0=%lu.%lu Hz (%u mV) SNR=%d.%d dB THD=%lu.%lu%% | "
                    "WPT H=0.%03lu crest=%lu.%lu glitches=%lu (%lu us)",
                    (unsigned long)g_shared.m7_heartbeat,
                    (unsigned long)m4,
                    state,
                    (unsigned long)s_status_fft.block_id,
                    (unsigned long)s_status_fft.fs_hz,
                    (unsigned)(s_status_fft.dc_offset_mv + 0.5f),
                    (unsigned)(s_status_fft.v_pp_mv + 0.5f),
                    (unsigned long)f0_int, (unsigned long)f0_dec,
                    (unsigned)(s_status_fft.f0_mag_mv + 0.5f),
                    snr_int, snr_dec,
                    (unsigned long)thd_int, (unsigned long)thd_dec,
                    (unsigned long)ent_x1000,
                    (unsigned long)crest_int, (unsigned long)crest_dec,
                    (unsigned long)s_status_wpt.transient_count,
                    (unsigned long)exec_us);
        }
        else
        {
            APP_LOG("[ipc] m7=%lu m4=%lu (CPU2 %s)",
                    (unsigned long)g_shared.m7_heartbeat,
                    (unsigned long)m4,
                    state);
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
