/**
  ******************************************************************************
  * @file    adc_hal.c
  * @brief   ADC + DMA driver implementation for STM32H755.
  ******************************************************************************
  */

#include "adc_hal.h"
#include "gpio_bm.h"

#if defined(HAL_ADC_MODULE_ENABLED)

#ifndef VREFINT_CAL_ADDR
#define VREFINT_CAL_ADDR     ((const uint16_t *)(0x1FF1E860UL))
#endif
#ifndef TEMPSENSOR_CAL1_ADDR
#define TEMPSENSOR_CAL1_ADDR ((const uint16_t *)(0x1FF1E820UL))
#endif
#ifndef TEMPSENSOR_CAL2_ADDR
#define TEMPSENSOR_CAL2_ADDR ((const uint16_t *)(0x1FF1E840UL))
#endif

static AdcHal_CalibrationData_t s_hal_cal;
static bool s_hal_cal_valid = false;

bool ADC_HAL_InjectCalibration(const AdcHal_CalibrationData_t *cal)
{
    if (cal == NULL)
    {
        return false;
    }
    if ((cal->vrefint_cal == 0U) || (cal->ts_cal2 <= cal->ts_cal1))
    {
        return false;
    }
    s_hal_cal = *cal;
    s_hal_cal_valid = true;
    return true;
}

static inline uint16_t ADC_HAL_GetVrefintCal(void)
{
    if (s_hal_cal_valid)
    {
        return s_hal_cal.vrefint_cal;
    }
#if defined(CORE_CM4)
    /* System memory (0x1FF00000 - 0x1FFFFFFF) is in D1 domain and NOT decoded by
     * the D2-to-D1 AHB bus bridge. Access from Cortex-M4 generates a HardFault.
     * We require explicit injection from CM7 via ADC_HAL_InjectCalibration().
     * If uncalibrated, return 0U (never invent hardcoded constants). */
    return 0U;
#else
    return *VREFINT_CAL_ADDR;
#endif
}

static inline uint16_t ADC_HAL_GetTsCal1(void)
{
    if (s_hal_cal_valid)
    {
        return s_hal_cal.ts_cal1;
    }
#if defined(CORE_CM4)
    return 0U;
#else
    return *TEMPSENSOR_CAL1_ADDR;
#endif
}

static inline uint16_t ADC_HAL_GetTsCal2(void)
{
    if (s_hal_cal_valid)
    {
        return s_hal_cal.ts_cal2;
    }
#if defined(CORE_CM4)
    return 0U;
#else
    return *TEMPSENSOR_CAL2_ADDR;
#endif
}

static const uint32_t s_hal_ranks[16] = {
    ADC_REGULAR_RANK_1,  ADC_REGULAR_RANK_2,  ADC_REGULAR_RANK_3,  ADC_REGULAR_RANK_4,
    ADC_REGULAR_RANK_5,  ADC_REGULAR_RANK_6,  ADC_REGULAR_RANK_7,  ADC_REGULAR_RANK_8,
    ADC_REGULAR_RANK_9,  ADC_REGULAR_RANK_10, ADC_REGULAR_RANK_11, ADC_REGULAR_RANK_12,
    ADC_REGULAR_RANK_13, ADC_REGULAR_RANK_14, ADC_REGULAR_RANK_15, ADC_REGULAR_RANK_16
};

/* Cycles x 10 for each SMPR setting 000..111 */
static const uint16_t s_sample_cycles_x10[8] = {
    15,   /* 000: 1.5 cycles */
    25,   /* 001: 2.5 cycles */
    85,   /* 010: 8.5 cycles */
    165,  /* 011: 16.5 cycles */
    325,  /* 100: 32.5 cycles */
    645,  /* 101: 64.5 cycles */
    3875, /* 110: 387.5 cycles */
    8105  /* 111: 810.5 cycles */
};

#define ADC_HAL_MAX_INSTANCES  3U
static AdcHal_Handle_t *s_adc_instances[ADC_HAL_MAX_INSTANCES] = {NULL, NULL, NULL};

static AdcHal_Handle_t *ADC_HAL_GetHandle(ADC_TypeDef *instance)
{
    if (instance == ADC1)
    {
        return s_adc_instances[0];
    }
#if defined(ADC2)
    if (instance == ADC2)
    {
        return s_adc_instances[1];
    }
#endif
#if defined(ADC3)
    if (instance == ADC3)
    {
        return s_adc_instances[2];
    }
#endif
    return NULL;
}

bool ADC_HAL_InitEx(AdcHal_Handle_t *handle, const AdcHal_ConfigEx_t *config)
{
    if ((handle == NULL) || (config == NULL))
    {
        return false;
    }

    if ((config->num_channels == 0U) || (config->num_channels > 16U) || (config->channels == NULL))
    {
        return false;
    }

    /* Validate peripheral instance */
    if ((config->adc_instance != ADC1)
#if defined(ADC2)
        && (config->adc_instance != ADC2)
#endif
#if defined(ADC3)
        && (config->adc_instance != ADC3)
#endif
    )
    {
        return false;
    }

    /* 0. Optional analog pin configuration */
    if (config->analog_pin_port != NULL)
    {
        if (!GPIO_BM_ConfigAnalog(config->analog_pin_port, config->analog_pin_idx))
        {
            return false;
        }
    }

    /* 1. Enable peripheral clock */
    if ((config->adc_instance == ADC1) || (config->adc_instance == ADC2))
    {
        RCC->AHB1ENR |= RCC_AHB1ENR_ADC12EN;
        volatile uint32_t dummy = RCC->AHB1ENR;
        (void)dummy;
    }
#if defined(ADC3)
    else if (config->adc_instance == ADC3)
    {
        RCC->AHB4ENR |= RCC_AHB4ENR_ADC3EN;
        volatile uint32_t dummy = RCC->AHB4ENR;
        (void)dummy;
    }
#endif

    /* 2. Check ADC3 internal channels and validate DS12923 minimum sampling time */
    bool has_vref = false;
    bool has_temp = false;
    bool has_vbat = false;
    uint32_t eff_freq = (config->kernel_clock_hz > 0U) ? config->kernel_clock_hz : 25000000UL;
    /* Fixed divide-by-2 before the SAR analog core (RM0399 §26.4.3, R14) */
    uint32_t f_analog = eff_freq / 2U;
    if (f_analog == 0U)
    {
        f_analog = 1U;
    }

    for (uint32_t i = 0; i < config->num_channels; i++)
    {
        uint32_t ch = config->channels[i].channel;
        uint32_t ch_num = ch & 0x1FUL;
        uint32_t smp = config->channels[i].sampling_time;

#if defined(ADC3)
        if (config->adc_instance == ADC3)
        {
            uint32_t min_ns = 0U;
            if ((ch == ADC_CHANNEL_VREFINT) || (ch_num == 19U))
            {
                has_vref = true;
                min_ns = 4300U; /* 4.3 us minimum */
            }
            else if ((ch == ADC_CHANNEL_TEMPSENSOR) || (ch_num == 18U))
            {
                has_temp = true;
                min_ns = 9000U; /* 9.0 us minimum */
            }
            else if ((ch == ADC_CHANNEL_VBAT) || (ch_num == 17U))
            {
                has_vbat = true;
                min_ns = 9000U; /* 9.0 us minimum */
            }

            if (min_ns > 0U)
            {
                uint64_t cycles_x10 = (uint64_t)s_sample_cycles_x10[smp & 7U];
                if ((cycles_x10 * 100000000ULL) < ((uint64_t)min_ns * (uint64_t)f_analog))
                {
                    return false; /* Sampling time is shorter than datasheet limit */
                }
            }
        }
#else
        (void)has_vref;
        (void)has_temp;
        (void)has_vbat;
        (void)ch_num;
        (void)smp;
#endif
    }

    /* 3. Validate oversampling parameters (F6): prevent 16-bit truncation */
    uint32_t hal_ovs_ratio = 0U;
    uint32_t hal_ovs_shift = ADC_RIGHTBITSHIFT_NONE;
    if (config->oversampling_enable)
    {
        uint32_t orig_bits;
        switch (config->resolution)
        {
            case ADC_RESOLUTION_16B: orig_bits = 16U; break;
            case ADC_RESOLUTION_14B: orig_bits = 14U; break;
            case ADC_RESOLUTION_12B: orig_bits = 12U; break;
            case ADC_RESOLUTION_10B: orig_bits = 10U; break;
            case ADC_RESOLUTION_8B:  orig_bits = 8U;  break;
            default:                 orig_bits = 16U; break;
        }

        uint32_t ratio = config->oversampling_ratio;
        uint32_t add_bits = 0U;
        if (ratio >= 2U)
        {
            hal_ovs_ratio = ratio;
            while ((1U << add_bits) < ratio)
            {
                add_bits++;
            }
        }
        else
        {
            hal_ovs_ratio = 2U << (ratio & 7U);
            add_bits = (ratio & 7U) + 1U;
        }

        hal_ovs_shift = config->oversampling_shift;
        uint32_t shift_bits = (hal_ovs_shift & ADC_CFGR2_OVSS_Msk) >> ADC_CFGR2_OVSS_Pos;
        if ((shift_bits == 0U) && (hal_ovs_shift > 0U) && (hal_ovs_shift <= 8U))
        {
            shift_bits = hal_ovs_shift;
            hal_ovs_shift = shift_bits << ADC_CFGR2_OVSS_Pos;
        }

        if ((orig_bits + add_bits - shift_bits) > 16U)
        {
            return false; /* Result exceeds 16 bits and would be truncated by halfword DMA */
        }
    }

    /* 4. Validate DMA buffer parameters if DMA is configured */
    bool use_dma = ((config->dma_stream != NULL) && (config->buffer != NULL));
    if (use_dma)
    {
        if ((config->buffer_length < (2U * config->num_channels)) ||
            ((config->buffer_length & 1U) != 0U) ||
            (config->buffer_length > 65535U))
        {
            return false;
        }

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
        /* D-cache coherency check (CLAUDE.md §2A): buffer 32-byte aligned, half-buffer multiple of 32 bytes */
        if ((((uint32_t)config->buffer) & 0x1FUL) != 0U)
        {
            return false;
        }
        if ((((config->buffer_length / 2U) * sizeof(uint16_t)) & 0x1FUL) != 0U)
        {
            return false;
        }
#endif
    }

    handle->config_ex = *config;
    handle->ready_half = 0U;
    handle->has_new_data = 0U;
    handle->stats = (AdcHal_Stats_t){0};

    /* 5. Configure ADC peripheral */
    handle->hadc.Instance = config->adc_instance;
    handle->hadc.Init.ClockPrescaler           = (config->clock_prescaler != 0U) ? config->clock_prescaler : ADC_CLOCK_ASYNC_DIV1;
    handle->hadc.Init.Resolution               = config->resolution;
    handle->hadc.Init.ScanConvMode             = (config->num_channels > 1U) ? ADC_SCAN_ENABLE : ADC_SCAN_DISABLE;
    handle->hadc.Init.EOCSelection             = ADC_EOC_SINGLE_CONV;
    handle->hadc.Init.LowPowerAutoWait         = DISABLE;
    handle->hadc.Init.ContinuousConvMode       = config->continuous_mode ? ENABLE : DISABLE;
    handle->hadc.Init.NbrOfConversion          = config->num_channels;
    handle->hadc.Init.DiscontinuousConvMode    = DISABLE;
    handle->hadc.Init.ExternalTrigConv         = config->trigger_source;
    handle->hadc.Init.ExternalTrigConvEdge     = config->trigger_edge;
    handle->hadc.Init.ConversionDataManagement = use_dma ? ADC_CONVERSIONDATA_DMA_CIRCULAR : ADC_CONVERSIONDATA_DR;
    handle->hadc.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    handle->hadc.Init.LeftBitShift             = ADC_LEFTBITSHIFT_NONE;

    if (config->oversampling_enable)
    {
        handle->hadc.Init.OversamplingMode                   = ENABLE;
        handle->hadc.Init.Oversampling.Ratio                 = hal_ovs_ratio;
        handle->hadc.Init.Oversampling.RightBitShift         = hal_ovs_shift;
        handle->hadc.Init.Oversampling.TriggeredMode         = ADC_TRIGGEREDMODE_SINGLE_TRIGGER;
        handle->hadc.Init.Oversampling.OversamplingStopReset = ADC_REGOVERSAMPLING_CONTINUED_MODE;
    }
    else
    {
        handle->hadc.Init.OversamplingMode = DISABLE;
    }

    if (HAL_ADC_Init(&handle->hadc) != HAL_OK)
    {
        return false;
    }

    /* Apply BOOST[1:0] while ADSTART=0 and JADSTART=0.
     * HAL_ADC_Init already computes BOOST mode via ADC_ConfigureBoostMode.
     * Never write a BOOST less than what ST calculated; take the higher of the two. */
    uint32_t current_boost = handle->hadc.Instance->CR & ADC_CR_BOOST;
    uint32_t selected_boost;
    if (config->boost_mode == ADC_HAL_BOOST_AUTO)
    {
        selected_boost = current_boost;
    }
    else
    {
        uint32_t req_boost = ((uint32_t)config->boost_mode) & ADC_CR_BOOST;
        selected_boost = (req_boost > current_boost) ? req_boost : current_boost;
    }

    uint32_t cr = handle->hadc.Instance->CR;
    cr &= ~(ADC_CR_ADSTART | ADC_CR_JADSTART | ADC_CR_ADSTP | ADC_CR_JADSTP | ADC_CR_ADCAL | ADC_CR_BOOST);
    cr |= selected_boost;
    handle->hadc.Instance->CR = cr;

    /* Apply Silicon Errata ES0445 2.10.x: disable injected context queue */
    handle->hadc.Instance->CFGR |= ADC_CFGR_JQDIS;

#if defined(ADC3)
    if (config->adc_instance == ADC3)
    {
        uint32_t ccr = ADC3_COMMON->CCR;
        if (has_vref) { ccr |= ADC_CCR_VREFEN; }
        if (has_temp) { ccr |= ADC_CCR_TSEN; }
        if (has_vbat) { ccr |= ADC_CCR_VBATEN; }
        ADC3_COMMON->CCR = ccr;
    }
#endif

    /* 6. Configure Sequencer Channels */
    for (uint32_t i = 0; i < config->num_channels; i++)
    {
        ADC_ChannelConfTypeDef sConfig = {0};
        sConfig.Channel      = config->channels[i].channel;
        sConfig.Rank         = s_hal_ranks[i];
        sConfig.SamplingTime = config->channels[i].sampling_time;
        sConfig.SingleDiff   = ADC_SINGLE_ENDED;
        sConfig.OffsetNumber = ADC_OFFSET_NONE;
        sConfig.Offset       = 0U;

        if (HAL_ADC_ConfigChannel(&handle->hadc, &sConfig) != HAL_OK)
        {
            return false;
        }
    }

    /* 7. Configure Analog Watchdog 1 if requested */
    if (config->awd_enable)
    {
        ADC_AnalogWDGConfTypeDef awd = {0};
        awd.WatchdogNumber = ADC_ANALOGWATCHDOG_1;
        awd.WatchdogMode   = config->awd_all_channels ? ADC_ANALOGWATCHDOG_ALL_REG : ADC_ANALOGWATCHDOG_SINGLE_REG;
        awd.Channel        = config->awd_channel;
        awd.ITMode         = ENABLE;
        awd.HighThreshold  = config->awd_high_threshold;
        awd.LowThreshold   = config->awd_low_threshold;

        if (HAL_ADC_AnalogWDGConfig(&handle->hadc, &awd) != HAL_OK)
        {
            return false;
        }
    }

    /* 8. Configure DMA if requested */
    if (use_dma)
    {
        handle->hdma.Instance                 = config->dma_stream;
        handle->hdma.Init.Request             = config->dma_request;
        handle->hdma.Init.Direction           = DMA_PERIPH_TO_MEMORY;
        handle->hdma.Init.PeriphInc           = DMA_PINC_DISABLE;
        handle->hdma.Init.MemInc              = DMA_MINC_ENABLE;
        handle->hdma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
        handle->hdma.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
        handle->hdma.Init.Mode                = DMA_CIRCULAR;
        handle->hdma.Init.Priority            = DMA_PRIORITY_HIGH;
        handle->hdma.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;

        if (HAL_DMA_Init(&handle->hdma) != HAL_OK)
        {
            return false;
        }

        __HAL_LINKDMA(&handle->hadc, DMA_Handle, handle->hdma);
    }

#if (USE_HAL_ADC_REGISTER_CALLBACKS == 1)
    /* Register callbacks per handle unconditionally if callback registration is enabled (R9) */
    (void)HAL_ADC_RegisterCallback(&handle->hadc, HAL_ADC_CONVERSION_HALF_CB_ID, ADC_HAL_OnConvHalfCplt);
    (void)HAL_ADC_RegisterCallback(&handle->hadc, HAL_ADC_CONVERSION_COMPLETE_CB_ID, ADC_HAL_OnConvCplt);
    (void)HAL_ADC_RegisterCallback(&handle->hadc, HAL_ADC_ERROR_CB_ID, ADC_HAL_OnError);
    (void)HAL_ADC_RegisterCallback(&handle->hadc, HAL_ADC_LEVEL_OUT_OF_WINDOW_1_CB_ID, ADC_HAL_OnLevelOutOfWindow);
#endif

    /* 9. Track handle in internal table only after successful initialization */
    if (config->adc_instance == ADC1)
    {
        s_adc_instances[0] = handle;
    }
#if defined(ADC2)
    else if (config->adc_instance == ADC2)
    {
        s_adc_instances[1] = handle;
    }
#endif
#if defined(ADC3)
    else if (config->adc_instance == ADC3)
    {
        s_adc_instances[2] = handle;
    }
#endif

    return true;
}

bool ADC_HAL_Init(AdcHal_Handle_t *handle, const AdcHal_Config_t *config)
{
    if ((handle == NULL) || (config == NULL))
    {
        return false;
    }

    handle->single_channel.channel       = config->channel;
    handle->single_channel.sampling_time = config->sampling_time;

    AdcHal_ConfigEx_t ex = {
        .adc_instance        = config->adc_instance,
        .channels            = &handle->single_channel,
        .num_channels        = 1U,
        .resolution          = config->resolution,
        .trigger_source      = config->trigger_source,
        .trigger_edge        = config->trigger_edge,
        .boost_mode          = config->boost_mode,
        .dma_stream          = config->dma_stream,
        .dma_request         = config->dma_request,
        .buffer              = config->buffer,
        .buffer_length       = config->buffer_length,
        .kernel_clock_hz     = 0U,
        .analog_pin_port     = NULL,
        .analog_pin_idx      = 0U,
        .oversampling_enable = false,
        .oversampling_ratio  = 0U,
        .oversampling_shift  = 0U,
        .awd_enable          = false,
        .awd_all_channels    = false,
        .awd_channel         = 0U,
        .awd_high_threshold  = 0U,
        .awd_low_threshold   = 0U
    };

    if (!ADC_HAL_InitEx(handle, &ex))
    {
        return false;
    }

    handle->config = *config;
    return true;
}

bool ADC_HAL_Deinit(AdcHal_Handle_t *handle)
{
    if ((handle == NULL) || (handle->hadc.Instance == NULL))
    {
        return false;
    }

    if (handle->hadc.DMA_Handle != NULL)
    {
        (void)ADC_HAL_Stop_DMA(handle);
    }
    else
    {
        (void)HAL_ADC_Stop(&handle->hadc);
    }
    (void)HAL_ADC_DeInit(&handle->hadc);
    if (handle->hdma.Instance != NULL)
    {
        (void)HAL_DMA_DeInit(&handle->hdma);
    }

    if (handle->hadc.Instance == ADC1)
    {
        s_adc_instances[0] = NULL;
    }
#if defined(ADC2)
    else if (handle->hadc.Instance == ADC2)
    {
        s_adc_instances[1] = NULL;
    }
#endif
#if defined(ADC3)
    else if (handle->hadc.Instance == ADC3)
    {
        s_adc_instances[2] = NULL;
    }
#endif

    handle->ready_half = 0U;
    handle->has_new_data = 0U;

    return true;
}

bool ADC_HAL_Calibrate(AdcHal_Handle_t *handle)
{
    if (handle == NULL)
    {
        return false;
    }

    /* Calibrate both linearity and offset for single-ended mode */
    if (HAL_ADCEx_Calibration_Start(&handle->hadc, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED) != HAL_OK)
    {
        return false;
    }

    return true;
}

bool ADC_HAL_Start_DMA(AdcHal_Handle_t *handle)
{
    if ((handle == NULL) || (handle->config_ex.buffer == NULL) || (handle->hadc.DMA_Handle == NULL))
    {
        return false;
    }

    if (HAL_ADC_Start_DMA(&handle->hadc, (uint32_t *)handle->config_ex.buffer, handle->config_ex.buffer_length) != HAL_OK)
    {
        return false;
    }

    return true;
}

bool ADC_HAL_Stop_DMA(AdcHal_Handle_t *handle)
{
    if (handle == NULL)
    {
        return false;
    }

    if (handle->hadc.DMA_Handle != NULL)
    {
        return (HAL_ADC_Stop_DMA(&handle->hadc) == HAL_OK);
    }
    else
    {
        return (HAL_ADC_Stop(&handle->hadc) == HAL_OK);
    }
}

bool ADC_HAL_ReadSingle(AdcHal_Handle_t *handle, uint32_t channel, uint32_t sampling_time, uint16_t *out_raw, uint32_t timeout_ms)
{
    if ((handle == NULL) || (handle->hadc.Instance == NULL) || (out_raw == NULL))
    {
        return false;
    }

    /* Precondition check (R8): ADC must not have active conversion or DMA configured */
    if (((handle->hadc.Instance->CR & ADC_CR_ADSTART) != 0U) ||
        ((handle->hadc.Instance->CFGR & ADC_CFGR_DMNGT) != 0U))
    {
        return false;
    }

    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel      = channel;
    sConfig.Rank         = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = sampling_time;
    sConfig.SingleDiff   = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset       = 0U;

    if (HAL_ADC_ConfigChannel(&handle->hadc, &sConfig) != HAL_OK)
    {
        return false;
    }

    /* Temporarily ensure software trigger */
    uint32_t prev_cfgr = handle->hadc.Instance->CFGR;
    handle->hadc.Instance->CFGR &= ~ADC_CFGR_EXTEN;

    if (HAL_ADC_Start(&handle->hadc) != HAL_OK)
    {
        handle->hadc.Instance->CFGR = prev_cfgr;
        return false;
    }

    if (HAL_ADC_PollForConversion(&handle->hadc, timeout_ms) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&handle->hadc);
        handle->hadc.Instance->CFGR = prev_cfgr;
        return false;
    }

    *out_raw = (uint16_t)HAL_ADC_GetValue(&handle->hadc);
    (void)HAL_ADC_Stop(&handle->hadc);
    handle->hadc.Instance->CFGR = prev_cfgr;

    return true;
}

void ADC_HAL_IRQHandler_DMA(AdcHal_Handle_t *handle)
{
    if (handle != NULL)
    {
        HAL_DMA_IRQHandler(&handle->hdma);
    }
}

void ADC_HAL_IRQHandler_ADC(AdcHal_Handle_t *handle)
{
    if (handle != NULL)
    {
        HAL_ADC_IRQHandler(&handle->hadc);
    }
}

uint32_t ADC_HAL_TakeAwdEvents(AdcHal_Handle_t *handle)
{
    if (handle == NULL)
    {
        return 0U;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint32_t count = handle->stats.awd_event_count;
    handle->stats.awd_event_count = 0U;

    if ((handle->hadc.Instance != NULL) && handle->config_ex.awd_enable)
    {
        handle->hadc.Instance->ISR = ADC_ISR_AWD1;    /* Clear pending flag */
        handle->hadc.Instance->IER |= ADC_IER_AWD1IE; /* Re-arm interrupt */
    }

    __set_PRIMASK(primask);
    return count;
}

bool ADC_HAL_GetReadyHalf(AdcHal_Handle_t *handle, uint8_t *out_half)
{
    uint32_t primask;

    if ((handle == NULL) || (out_half == NULL))
    {
        return false;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    if (handle->has_new_data == 0U)
    {
        __set_PRIMASK(primask);
        return false;
    }

    *out_half = handle->ready_half;
    handle->has_new_data = 0U;

    __set_PRIMASK(primask);
    return true;
}

void ADC_HAL_ComputeStatsChannel(AdcHal_Handle_t *handle, uint8_t half, uint32_t channel_seq_idx, uint32_t vref_mv)
{
    if ((handle == NULL) || (handle->config_ex.buffer == NULL))
    {
        return;
    }

    uint32_t num_channels = (handle->config_ex.num_channels > 0U)
                            ? handle->config_ex.num_channels : 1U;
    if (channel_seq_idx >= num_channels)
    {
        return;
    }

    uint32_t half_len = handle->config_ex.buffer_length / 2U;
    if (half_len == 0U)
    {
        return;
    }

    uint32_t start_idx = (half == 0U) ? 0U : half_len;
    uint32_t end_idx = start_idx + half_len;
    const uint16_t *buf = handle->config_ex.buffer;

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    /* Invalidate D-cache before reading buffer filled by DMA (CLAUDE.md §2A, ES0445 2.1.5) */
    SCB_InvalidateDCache_by_Addr((uint32_t *)&buf[start_idx], (int32_t)(half_len * sizeof(uint16_t)));
    __DMB();
#endif

    uint32_t sum = 0U;
    uint16_t min_val = 0xFFFFU;
    uint16_t max_val = 0U;
    uint32_t count = 0U;
    uint16_t latest = 0U;

    for (uint32_t i = start_idx + channel_seq_idx; i < end_idx; i += num_channels)
    {
        uint16_t val = buf[i];
        sum += val;
        latest = val;
        count++;
        if (val < min_val)
        {
            min_val = val;
        }
        if (val > max_val)
        {
            max_val = val;
        }
    }

    if (count == 0U)
    {
        return;
    }

    handle->stats.latest_raw  = latest;
    handle->stats.average_raw = (uint16_t)(sum / count);
    handle->stats.min_raw     = min_val;
    handle->stats.max_raw     = max_val;

    uint32_t max_code;
    switch (handle->config_ex.resolution)
    {
        case ADC_RESOLUTION_16B: max_code = 65535U; break;
        case ADC_RESOLUTION_14B: max_code = 16383U; break;
        case ADC_RESOLUTION_12B: max_code = 4095U;  break;
        case ADC_RESOLUTION_10B: max_code = 1023U;  break;
        case ADC_RESOLUTION_8B:  max_code = 255U;   break;
        default:                 max_code = 65535U; break;
    }

    /* 65535 * 3300 = 216,265,500 < 2^32-1: fits safely in uint32_t without 64-bit libcalls */
    handle->stats.millivolts = ((uint32_t)handle->stats.average_raw * vref_mv) / max_code;
}

void ADC_HAL_ComputeStats(AdcHal_Handle_t *handle, uint8_t half, uint32_t vref_mv)
{
    ADC_HAL_ComputeStatsChannel(handle, half, 0U, vref_mv);
}

uint32_t ADC_HAL_CalculateVDDA(uint16_t vrefint_raw, uint8_t resolution_bits)
{
    uint32_t raw_16b = (uint32_t)vrefint_raw;
    if ((resolution_bits > 0U) && (resolution_bits < 16U))
    {
        raw_16b <<= (16U - resolution_bits);
    }
    if (raw_16b == 0U)
    {
        return 0U;
    }
    uint32_t cal = (uint32_t)ADC_HAL_GetVrefintCal();
    if (cal == 0U)
    {
        return 0U;
    }
    /* VREFINT_CAL is measured at VDDA = 3300 mV at 16-bit resolution */
    return (uint32_t)((3300UL * cal) / raw_16b);
}

int32_t ADC_HAL_CalculateTemperature(uint16_t temp_raw, uint32_t vdda_mv, uint8_t resolution_bits)
{
    uint32_t raw_16b = (uint32_t)temp_raw;
    if ((resolution_bits > 0U) && (resolution_bits < 16U))
    {
        raw_16b <<= (16U - resolution_bits);
    }
    uint32_t cal1 = (uint32_t)ADC_HAL_GetTsCal1();
    uint32_t cal2 = (uint32_t)ADC_HAL_GetTsCal2();

    if ((cal1 == 0U) || (cal2 <= cal1))
    {
        return ADC_HAL_TEMPERATURE_ERROR;
    }

    /* Scale raw reading if VDDA differs from nominal 3300 mV */
    uint32_t raw_norm = raw_16b;
    if (vdda_mv > 0U)
    {
        raw_norm = (uint32_t)(((uint64_t)raw_16b * vdda_mv) / 3300UL);
    }

    /* STM32H755: TS_CAL1 @ 30 deg C, TS_CAL2 @ 130 deg C (range = 100 deg C) */
    int32_t temp_c = (((int32_t)(130 - 30) * ((int32_t)raw_norm - (int32_t)cal1)) / (int32_t)(cal2 - cal1)) + 30;
    return temp_c;
}

/* ==========================================================================
 * HAL Callbacks. Run in ISR context. Keep them lightweight (NO HSEM!).
 * ========================================================================== */

void ADC_HAL_OnConvHalfCplt(ADC_HandleTypeDef *hadc)
{
    if (hadc == NULL)
    {
        return;
    }

    AdcHal_Handle_t *handle = ADC_HAL_GetHandle(hadc->Instance);
    if (handle != NULL)
    {
        if (handle->has_new_data != 0U)
        {
            handle->stats.half_overrun_count++;
        }
        handle->ready_half = 0U;
        handle->has_new_data = 1U;
        handle->stats.dma_ht_count++;
        handle->stats.sample_count += (handle->config_ex.buffer_length / 2U);
    }
}

void ADC_HAL_OnConvCplt(ADC_HandleTypeDef *hadc)
{
    if (hadc == NULL)
    {
        return;
    }

    AdcHal_Handle_t *handle = ADC_HAL_GetHandle(hadc->Instance);
    if (handle != NULL)
    {
        if (handle->has_new_data != 0U)
        {
            handle->stats.half_overrun_count++;
        }
        handle->ready_half = 1U;
        handle->has_new_data = 1U;
        handle->stats.dma_tc_count++;
        handle->stats.sample_count += (handle->config_ex.buffer_length / 2U);
    }
}

void ADC_HAL_OnError(ADC_HandleTypeDef *hadc)
{
    if (hadc == NULL)
    {
        return;
    }

    AdcHal_Handle_t *handle = ADC_HAL_GetHandle(hadc->Instance);
    if (handle != NULL)
    {
        /* Separate ADC hardware overrun from DMA bus/transfer errors (A5) */
        if ((hadc->ErrorCode & HAL_ADC_ERROR_OVR) != 0U)
        {
            handle->stats.adc_overrun_count++;
        }
        if ((hadc->ErrorCode & HAL_ADC_ERROR_DMA) != 0U)
        {
            handle->stats.dma_error_count++;
        }
        /* Fallback if error code reported neither specifically */
        if ((hadc->ErrorCode & (HAL_ADC_ERROR_OVR | HAL_ADC_ERROR_DMA)) == 0U)
        {
            handle->stats.dma_error_count++;
        }

        /* Clear ErrorCode to prevent counting same error repeatedly on subsequent events (R7) */
        hadc->ErrorCode = HAL_ADC_ERROR_NONE;
    }
}

void ADC_HAL_OnLevelOutOfWindow(ADC_HandleTypeDef *hadc)
{
    if (hadc == NULL)
    {
        return;
    }

    AdcHal_Handle_t *handle = ADC_HAL_GetHandle(hadc->Instance);
    if (handle != NULL)
    {
        /* Mask AWD1 interrupt to prevent interrupt storm until TakeAwdEvents re-arms (R9) */
        hadc->Instance->IER &= ~ADC_IER_AWD1IE;
        handle->stats.awd_event_count++;
    }
}

#if (!defined(USE_HAL_ADC_REGISTER_CALLBACKS) || (USE_HAL_ADC_REGISTER_CALLBACKS == 0))
#ifndef ADC_HAL_CUSTOM_CALLBACKS

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    ADC_HAL_OnConvHalfCplt(hadc);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    ADC_HAL_OnConvCplt(hadc);
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    ADC_HAL_OnError(hadc);
}

void HAL_ADC_LevelOutOfWindowCallback(ADC_HandleTypeDef *hadc)
{
    ADC_HAL_OnLevelOutOfWindow(hadc);
}

#endif /* ADC_HAL_CUSTOM_CALLBACKS */
#endif /* USE_HAL_ADC_REGISTER_CALLBACKS == 0 */

#endif /* HAL_ADC_MODULE_ENABLED */
