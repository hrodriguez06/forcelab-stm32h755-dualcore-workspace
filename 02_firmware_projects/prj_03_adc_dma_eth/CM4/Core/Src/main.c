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
#include "hsem_bm.h"
#include "adc_acquisition.h"

#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST
#include "adc_hal.h"
#include "adc_selftest_hw.h"
#include <stdio.h>
#include <string.h>
static void CM4_Test_Adc_Hal(void);
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

int main(void)
{
    uint32_t last_heartbeat_tick = 0U;

    CPU2_WaitForCPU1();

    /* Silicon errata ES0445 2.2.2: Disable FPU lazy stacking to avoid
       potential register corruption during interrupt entry. */
    FPU->FPCCR &= ~FPU_FPCCR_LSPEN_Msk;

    cpu2_woke = 0xC0FFEEUL;

    /* The clock tree is up and owned by CPU1. From here CPU2 configures only
       what belongs to it. HAL_Init() sets up the SysTick of THIS core and the
       NVIC priority grouping; it does not touch the clock tree. */
    HAL_Init();
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    if (HSEM_BM_Init() != HSEM_BM_OK)
    {
        Error_Handler();
    }
    if (!HSEM_BM_TimebaseStart())
    {
        Error_Handler();
    }
    Shared_GpioLockInstall();

    /* Deliberately not calling Shared_Init(): the block belongs to CPU1 and is
       already initialised by the time this core is released. Zeroing it here
       would wipe whatever CPU1 has published. */

#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST
    CM4_Test_Adc_Hal();

    /* Hardware tests T2, T3, T4, T6 (adc_selftest_hw.h). Must run before
       ADC_Acquisition_Init(): they borrow ADC1/2/3, DMA1_Stream0/1 and TIM3. */
    ADC_SelfTestHw_Run();
#endif

    /* Initialize ADC1 + DMA1 with TIM3 hardware trigger */
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

        /* Process completed ADC DMA half-buffer if ready */
        ADC_Acquisition_Process();

#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST
        /* One result line per mailbox slot CPU1 has freed. */
        ADC_SelfTestHw_Poll();
#endif
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
    HAL_HSEM_ActivateNotification(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_BOOT_SYNC));

    /* SEVONPEND: wake on an interrupt that is merely PENDING, without enabling
       it in the NVIC and without vectoring to a handler.
     *
     * This line is why CPU2 wakes at all, and leaving it out fails silently.
     * HAL_PWREx_EnterSTOPMode() issues a single __WFE(), and WFE resumes only
     * on an *event*. Releasing HSEM 0 sets HSEM_C2ISR and HSEM_C2MISR, but a
     * pending interrupt is not an event unless it is NVIC-enabled or
     * SEVONPEND is set - and enabling HSEM2_IRQn instead would vector into the
     * weak Default_Handler, which is an infinite loop.
     *
     * ST's reference CM4 code (Projects/NUCLEO-H745ZI-Q/Applications/FreeRTOS/
     * FreeRTOS_AMP_Dual_RTOS/CM4/Src/main.c) omits it, because there CPU1
     * leaves the D2 domain idle and the domain exits DSTOP through the
     * peripheral wake-up path instead. Here CPU1 holds D2 awake - the console
     * USART3 sits on APB1, in D2 - so CPU2 is only in CSleep and the core's
     * own event logic is the only way back.
     *
     * Symptom when it is missing: HSEM_C2IER = 1 and HSEM_C2MISR = 1 (the
     * handshake worked), CPU1 runs normally, and CPU2 never leaves this
     * function. Nothing faults. */
    SET_BIT(SCB->SCR, SCB_SCR_SEVONPEND_Msk);

    /* Enter STOP on D2. Dropping D2CKRDY is the signal CPU1 polls for. */
    HAL_PWREx_ClearPendingEvent();
    HAL_PWREx_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFE, PWR_D2_DOMAIN);

    /* Woken by the HSEM notification: unsubscribe, clear it, and put the core
       back to its normal wake-up behaviour. */
    __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_BOOT_SYNC));
    HAL_HSEM_DeactivateNotification(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_BOOT_SYNC));
    CLEAR_BIT(SCB->SCR, SCB_SCR_SEVONPEND_Msk);
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

#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST
static void CM4_Test_Adc_Hal(void)
{
    bool uncal_check_ok = false;
    bool cal_injected_ok = false;
    bool hal_init_ok = false;
    bool hal_cal_ok  = false;
    bool read_vref_ok = false;
    bool read_temp_ok = false;
    bool res_sweep_ok = true;
    bool ovs_trunc_rejected = false;
    bool ovs_valid_accepted = false;
    bool ovs_hw_verified = false;
    bool awd_trip_ok = false;
    bool awd_notrip_ok = false;
    uint32_t vdda_hal = 0;
    int32_t temp_hal = 0;
    uint16_t ovs_raw = 0;

    /* 0. Verify uncalibrated failure behavior on CM4 (S2 requirement: never invent constants) */
    if ((ADC_HAL_CalculateVDDA(24200U, 16U) == 0U) &&
        (ADC_HAL_CalculateTemperature(12000U, 3300U, 16U) == ADC_HAL_TEMPERATURE_ERROR))
    {
        uncal_check_ok = true;
    }

    /* Wait for M7 to publish valid calibration words */
    uint32_t wait_spins = 1000000U;
    while ((g_shared.cal_valid == 0U) && (wait_spins > 0U))
    {
        wait_spins--;
    }

    if (g_shared.cal_valid != 0U)
    {
        AdcHal_CalibrationData_t hal_cal = {
            .vrefint_cal = g_shared.vrefint_cal,
            .ts_cal1     = g_shared.ts_cal1,
            .ts_cal2     = g_shared.ts_cal2
        };
        if (ADC_HAL_InjectCalibration(&hal_cal))
        {
            cal_injected_ok = true;
        }
    }

    AdcHal_ChannelConfig_t hal_chs[2] = {
        { .channel = ADC_CHANNEL_VREFINT,    .sampling_time = ADC_SAMPLETIME_810CYCLES_5 },
        { .channel = ADC_CHANNEL_TEMPSENSOR, .sampling_time = ADC_SAMPLETIME_810CYCLES_5 }
    };

    AdcHal_ConfigEx_t ex = {
        .adc_instance        = ADC3,
        .channels            = hal_chs,
        .num_channels        = 2U,
        .resolution          = ADC_RESOLUTION_16B,
        .trigger_source      = ADC_SOFTWARE_START,
        .trigger_edge        = ADC_EXTERNALTRIGCONVEDGE_NONE,
        .boost_mode          = ADC_HAL_BOOST_AUTO,
        .dma_stream          = NULL,
        .dma_request         = 0U,
        .buffer              = NULL,
        .buffer_length       = 0U,
        .kernel_clock_hz     = 25000000UL,
        .analog_pin_port     = NULL,
        .analog_pin_idx      = 0U,
        .oversampling_enable = false,
        .oversampling_ratio  = 0U,
        .oversampling_shift  = 0U,
        .awd_enable          = false,
        .awd_all_channels    = false,
        .awd_channel         = 0U,
        .awd_high_threshold  = 0U,
        .awd_low_threshold   = 0U,
        .clock_prescaler     = 0U,
        .clock_mode          = 0U,
        .continuous_mode     = false
    };

    static AdcHal_Handle_t h_hal;
    memset(&h_hal, 0, sizeof(h_hal));
    hal_init_ok = ADC_HAL_InitEx(&h_hal, &ex);

    uint16_t vref_raw = 0;
    if (hal_init_ok)
    {
        hal_cal_ok = ADC_HAL_Calibrate(&h_hal);

        /* 1. ReadSingle on VREFINT */
        if (ADC_HAL_ReadSingle(&h_hal, ADC_CHANNEL_VREFINT, ADC_SAMPLETIME_810CYCLES_5, &vref_raw, 100U))
        {
            vdda_hal = ADC_HAL_CalculateVDDA(vref_raw, 16U);
            if ((vdda_hal >= 3100U) && (vdda_hal <= 3500U))
            {
                read_vref_ok = true;
            }
        }

        /* 2. ReadSingle on TEMPSENSOR */
        uint16_t temp_raw = 0;
        if (ADC_HAL_ReadSingle(&h_hal, ADC_CHANNEL_TEMPSENSOR, ADC_SAMPLETIME_810CYCLES_5, &temp_raw, 100U))
        {
            temp_hal = ADC_HAL_CalculateTemperature(temp_raw, vdda_hal, 16U);
            if ((temp_hal >= 10) && (temp_hal <= 90))
            {
                read_temp_ok = true;
            }
        }

        (void)ADC_HAL_Deinit(&h_hal);
    }

    /* 3. T2 Resolution Sweep (16, 14, 12, 10, 8 bits) checking 2^(16-N) scaling */
    if (vref_raw > 0U)
    {
        uint32_t v_16 = ADC_HAL_CalculateVDDA(vref_raw, 16U);
        uint32_t v_14 = ADC_HAL_CalculateVDDA((uint16_t)(vref_raw >> 2U), 14U);
        uint32_t v_12 = ADC_HAL_CalculateVDDA((uint16_t)(vref_raw >> 4U), 12U);
        uint32_t v_10 = ADC_HAL_CalculateVDDA((uint16_t)(vref_raw >> 6U), 10U);
        uint32_t v_8  = ADC_HAL_CalculateVDDA((uint16_t)(vref_raw >> 8U), 8U);

        int32_t d14 = (int32_t)v_16 - (int32_t)v_14; if (d14 < 0) d14 = -d14;
        int32_t d12 = (int32_t)v_16 - (int32_t)v_12; if (d12 < 0) d12 = -d12;
        int32_t d10 = (int32_t)v_16 - (int32_t)v_10; if (d10 < 0) d10 = -d10;
        int32_t d8  = (int32_t)v_16 - (int32_t)v_8;  if (d8 < 0)  d8 = -d8;

        if ((d14 > 10) || (d12 > 10) || (d10 > 15) || (d8 > 35))
        {
            res_sweep_ok = false;
        }
    }
    else
    {
        res_sweep_ok = false;
    }

    /* 4. Oversampling Truncation Validation */
    /* 16-bit + 256x with 0 shift = 24 bits > 16 bits: must be rejected */
    memset(&h_hal, 0, sizeof(h_hal));
    AdcHal_ConfigEx_t ovs_bad = ex;
    ovs_bad.oversampling_enable = true;
    ovs_bad.oversampling_ratio  = ADC_HAL_OVERSAMPLING_256X;
    ovs_bad.oversampling_shift  = ADC_HAL_OVERSAMPLING_SHIFT_NONE;
    if (!ADC_HAL_InitEx(&h_hal, &ovs_bad))
    {
        ovs_trunc_rejected = true;
    }

    /* With shift 8 bits: must be accepted */
    memset(&h_hal, 0, sizeof(h_hal));
    AdcHal_ConfigEx_t ovs_good = ex;
    ovs_good.oversampling_enable = true;
    ovs_good.oversampling_ratio  = ADC_HAL_OVERSAMPLING_256X;
    ovs_good.oversampling_shift  = ADC_HAL_OVERSAMPLING_SHIFT_8B;
    if (ADC_HAL_InitEx(&h_hal, &ovs_good))
    {
        ovs_valid_accepted = true;
        (void)ADC_HAL_Deinit(&h_hal);
    }

    /* 5. T7 Oversampling x16 shift 4: CFGR2 Register Readback & Hardware Conversion */
    memset(&h_hal, 0, sizeof(h_hal));
    AdcHal_ConfigEx_t ovs_t7 = ex;
    ovs_t7.oversampling_enable = true;
    ovs_t7.oversampling_ratio  = ADC_HAL_OVERSAMPLING_16X;
    ovs_t7.oversampling_shift  = ADC_HAL_OVERSAMPLING_SHIFT_4B;
    if (ADC_HAL_InitEx(&h_hal, &ovs_t7))
    {
        uint32_t cfgr2_val = ADC3->CFGR2;
        bool rovse_ok = ((cfgr2_val & ADC_CFGR2_ROVSE) != 0U);
        bool ovsr_ok  = (((cfgr2_val & ADC_CFGR2_OVSR_Msk) >> ADC_CFGR2_OVSR_Pos) == 15U);
        bool ovss_ok  = (((cfgr2_val & ADC_CFGR2_OVSS_Msk) >> ADC_CFGR2_OVSS_Pos) == 4U);

        if (rovse_ok && ovsr_ok && ovss_ok)
        {
            (void)ADC_HAL_Calibrate(&h_hal);
            if (ADC_HAL_ReadSingle(&h_hal, ADC_CHANNEL_VREFINT, ADC_SAMPLETIME_810CYCLES_5, &ovs_raw, 100U))
            {
                if ((ovs_raw >= 20000U) && (ovs_raw <= 30000U))
                {
                    ovs_hw_verified = true;
                }
            }
        }
        (void)ADC_HAL_Deinit(&h_hal);
    }

    /* 6. T5 Analog Watchdog 1: Trip and No-trip tests */
    /* Trip test: high_threshold = 100 */
    memset(&h_hal, 0, sizeof(h_hal));
    AdcHal_ConfigEx_t awd_cfg = ex;
    awd_cfg.awd_enable          = true;
    awd_cfg.awd_all_channels    = true;
    awd_cfg.awd_high_threshold  = 100U;
    awd_cfg.awd_low_threshold   = 0U;
    if (ADC_HAL_InitEx(&h_hal, &awd_cfg))
    {
        uint16_t dummy_raw = 0;
        (void)ADC_HAL_ReadSingle(&h_hal, ADC_CHANNEL_VREFINT, ADC_SAMPLETIME_810CYCLES_5, &dummy_raw, 100U);
        if ((h_hal.hadc.Instance->ISR & ADC_ISR_AWD1) != 0U)
        {
            ADC_HAL_IRQHandler_ADC(&h_hal);
            if (ADC_HAL_TakeAwdEvents(&h_hal) > 0U)
            {
                awd_trip_ok = true;
            }
        }
        (void)ADC_HAL_Deinit(&h_hal);
    }

    /* No-trip test: high_threshold = 65535 */
    memset(&h_hal, 0, sizeof(h_hal));
    AdcHal_ConfigEx_t awd_notrip = ex;
    awd_notrip.awd_enable          = true;
    awd_notrip.awd_all_channels    = true;
    awd_notrip.awd_high_threshold  = 65535U;
    awd_notrip.awd_low_threshold   = 0U;
    if (ADC_HAL_InitEx(&h_hal, &awd_notrip))
    {
        uint16_t dummy_raw = 0;
        (void)ADC_HAL_ReadSingle(&h_hal, ADC_CHANNEL_VREFINT, ADC_SAMPLETIME_810CYCLES_5, &dummy_raw, 100U);
        if ((h_hal.hadc.Instance->ISR & ADC_ISR_AWD1) == 0U)
        {
            awd_notrip_ok = true;
        }
        (void)ADC_HAL_Deinit(&h_hal);
    }

    /* Format report into g_shared.m4_to_m7 */
    SHARED_LOCK(HSEM_ID_M4_TO_M7_MBOX);
    char *p = (char *)g_shared.m4_to_m7;
    int pos = 0;

    const char *prefix = "HAL:I=";
    while (*prefix != '\0') { p[pos++] = *prefix++; }
    p[pos++] = hal_init_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'C'; p[pos++] = '=';
    p[pos++] = hal_cal_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'U'; p[pos++] = '=';
    p[pos++] = uncal_check_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'J'; p[pos++] = '=';
    p[pos++] = cal_injected_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'V'; p[pos++] = '=';
    p[pos++] = read_vref_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'T'; p[pos++] = '=';
    p[pos++] = read_temp_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'R'; p[pos++] = '=';
    p[pos++] = res_sweep_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'O'; p[pos++] = '=';
    p[pos++] = (ovs_trunc_rejected && ovs_valid_accepted && ovs_hw_verified) ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'A'; p[pos++] = '=';
    p[pos++] = (awd_trip_ok && awd_notrip_ok) ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'V'; p[pos++] = 'd'; p[pos++] = '=';
    char tmp[10]; int ti = 0; uint32_t v = vdda_hal;
    if (v == 0) { p[pos++] = '0'; }
    else {
        while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) { p[pos++] = tmp[--ti]; }
    }

    p[pos] = '\0';
    __DMB();
    g_shared.m4_to_m7_len = (uint32_t)pos;
    SHARED_UNLOCK(HSEM_ID_M4_TO_M7_MBOX);
}
#endif /* ADC_DRIVER_SELFTEST */
