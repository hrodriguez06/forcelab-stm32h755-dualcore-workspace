/**
  ******************************************************************************
  * @file    adc_ll.c
  * @brief   Low-Layer ADC driver implementation for STM32H755.
  ******************************************************************************
  */

#include "adc_ll.h"
#include "gpio_bm.h"
#include <stddef.h>

#define ADC_LL_VREG_STUP_LOOPS           10000UL
#define ADC_LL_CALIBRATION_TIMEOUT_LOOPS 10000000UL
#define ADC_LL_ENABLE_TIMEOUT_LOOPS      10000000UL
#define ADC_LL_DISABLE_TIMEOUT_LOOPS     10000000UL
#define ADC_LL_STOP_TIMEOUT_LOOPS        10000000UL

#ifndef VREFINT_CAL_ADDR
#define VREFINT_CAL_ADDR     ((const uint16_t *)(0x1FF1E860UL))
#endif
#ifndef TEMPSENSOR_CAL1_ADDR
#define TEMPSENSOR_CAL1_ADDR ((const uint16_t *)(0x1FF1E820UL))
#endif
#ifndef TEMPSENSOR_CAL2_ADDR
#define TEMPSENSOR_CAL2_ADDR ((const uint16_t *)(0x1FF1E840UL))
#endif

/* Prescaler divisors indexed by AdcLl_Prescaler_t value */
static const uint16_t s_ll_presc_divs[] = {
    1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256
};
#define ADC_LL_PRESC_COUNT (sizeof(s_ll_presc_divs) / sizeof(s_ll_presc_divs[0]))

/* Sample time cycles x10 for 1.5, 2.5, 8.5, 16.5, 32.5, 64.5, 387.5, 810.5 */
static const uint16_t s_sample_cycles_x10[] = {15, 25, 85, 165, 325, 645, 3875, 8105};

uint32_t ADC_LL_GetBoostBits(uint32_t ker_ck_freq_hz)
{
    /* DBGMCU->IDCODE bits [31:16] contain REV_ID */
    uint32_t rev_id = ((DBGMCU->IDCODE & 0xFFFF0000UL) >> 16U);

    if (rev_id <= 0x1003U) /* STM32H7 silicon Rev.Y */
    {
        return (ker_ck_freq_hz > 20000000UL) ? ADC_CR_BOOST_0 : 0UL;
    }
    else /* STM32H7 silicon Rev.V and newer */
    {
        uint32_t freq = ker_ck_freq_hz / 2U; /* Divider by 2 for Rev.V */
        if (freq <= 6250000UL)
        {
            return 0UL;
        }
        else if (freq <= 12500000UL)
        {
            return ADC_CR_BOOST_0;
        }
        else if (freq <= 25000000UL)
        {
            return ADC_CR_BOOST_1;
        }
        else
        {
            return (ADC_CR_BOOST_0 | ADC_CR_BOOST_1);
        }
    }
}

/**
  * @brief  Translates the resolution to CFGR.RES bits.
  *         Handles Rev V vs Rev Y silicon and optimized modes.
  */
static uint32_t ADC_LL_TranslateResolution(uint32_t resolution)
{
    uint32_t rev_id = ((DBGMCU->IDCODE & 0xFFFF0000UL) >> 16U);
    bool is_rev_v = (rev_id > 0x1003U);

    if (is_rev_v)
    {
        switch (resolution)
        {
            case ADC_LL_RESOLUTION_16B:     return 0x00000000UL;
            case ADC_LL_RESOLUTION_14B:     return ADC_CFGR_RES_0;
            case ADC_LL_RESOLUTION_12B:     return ADC_CFGR_RES_1;
            case ADC_LL_RESOLUTION_10B:     return (ADC_CFGR_RES_0 | ADC_CFGR_RES_1);
            case ADC_LL_RESOLUTION_8B:      return (ADC_CFGR_RES_2 | ADC_CFGR_RES_1 | ADC_CFGR_RES_0);
            case ADC_LL_RESOLUTION_14B_OPT: return (ADC_CFGR_RES_2 | ADC_CFGR_RES_0);
            case ADC_LL_RESOLUTION_12B_OPT: return (ADC_CFGR_RES_2 | ADC_CFGR_RES_1);
            default: return 0xFFFFFFFFUL;
        }
    }
    else /* Rev Y */
    {
        if ((resolution == ADC_LL_RESOLUTION_14B_OPT) ||
            (resolution == ADC_LL_RESOLUTION_12B_OPT))
        {
            return 0xFFFFFFFFUL;
        }
        if ((resolution != ADC_LL_RESOLUTION_16B) &&
            (resolution != ADC_LL_RESOLUTION_14B) &&
            (resolution != ADC_LL_RESOLUTION_12B) &&
            (resolution != ADC_LL_RESOLUTION_10B) &&
            (resolution != ADC_LL_RESOLUTION_8B))
        {
            return 0xFFFFFFFFUL;
        }
        return resolution;
    }
}

/**
  * @brief  Configures common clock (CKMODE and PRESC) safely.
  *         If the other ADC is enabled, verifies compatibility.
  */
static bool ADC_LL_ConfigureCommonClock(ADC_Common_TypeDef *common,
                                        ADC_TypeDef *adc,
                                        AdcLl_ClockMode_t clock_mode,
                                        AdcLl_Prescaler_t prescaler)
{
    /* RM0399 §26.4.3: CKMODE and PRESC can only be written when ALL ADCs of the common block
     * are disabled (ADEN = 0). Check both the target ADC and its partner. */
    bool any_enabled = (LL_ADC_IsEnabled(adc) != 0U);
    if (common == ADC12_COMMON)
    {
        ADC_TypeDef *other = (adc == ADC1) ? ADC2 : ADC1;
        if (LL_ADC_IsEnabled(other) != 0U)
        {
            any_enabled = true;
        }
    }

    uint32_t desired_ckmode = ((uint32_t)clock_mode) & ADC_CCR_CKMODE_Msk;
    uint32_t desired_presc  = (((uint32_t)prescaler) << ADC_CCR_PRESC_Pos) & ADC_CCR_PRESC_Msk;

    if (any_enabled)
    {
        uint32_t current = common->CCR;
        if (((current & ADC_CCR_CKMODE_Msk) != desired_ckmode) ||
            ((current & ADC_CCR_PRESC_Msk)  != desired_presc))
        {
            return false;
        }
        return true;
    }

    LL_ADC_SetCommonClock(common, desired_ckmode | desired_presc);
    return true;
}

bool ADC_LL_InitEx(AdcLl_Handle_t *handle, const AdcLl_ConfigEx_t *config)
{
    if ((handle == NULL) || (config == NULL) || (config->adc_instance == NULL) ||
        (config->channels == NULL) || (config->num_channels == 0U) || (config->num_channels > 16U))
    {
        return false;
    }

    bool needs_dma = (config->dma_mode != ADC_LL_DMA_DISABLED) || config->dma_circular;
    if (needs_dma)
    {
        if ((config->buffer == NULL) || (config->buffer_length < 2U) ||
            ((config->buffer_length & 1U) != 0U))
        {
            return false;
        }
    }

    uint32_t res_bits = ADC_LL_TranslateResolution(config->resolution);
    if (res_bits == 0xFFFFFFFFUL)
    {
        return false;
    }

    if ((uint32_t)config->prescaler >= ADC_LL_PRESC_COUNT)
    {
        return false;
    }

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if (needs_dma)
    {
        if ((((uint32_t)config->buffer) & 0x1FUL) != 0U)
        {
            return false;
        }
        if ((((config->buffer_length / 2U) * sizeof(uint16_t)) & 0x1FUL) != 0U)
        {
            return false;
        }
    }
#endif

    /* Calculate effective ADC clock frequency and analog clock (RM §26.4.3) */
    uint32_t eff_freq = (config->kernel_clock_hz != 0U)
                        ? config->kernel_clock_hz : 25000000UL;
    if (config->clock_mode == ADC_LL_CLOCK_ASYNC)
    {
        uint32_t pidx = (uint32_t)config->prescaler;
        if (pidx < ADC_LL_PRESC_COUNT)
        {
            eff_freq /= s_ll_presc_divs[pidx];
        }
    }
    else
    {
        /* Synchronous clock mode: presc ignored, CKMODE divides adc_sclk */
        if (config->clock_mode == ADC_LL_CLOCK_SYNC_2) { eff_freq /= 2U; }
        else if (config->clock_mode == ADC_LL_CLOCK_SYNC_4) { eff_freq /= 4U; }
    }
    /* Fixed divide-by-2 before the analog core (RM §26.4.3) */
    uint32_t f_analog = eff_freq / 2U;
    if (f_analog == 0U)
    {
        f_analog = 1U;
    }

    /* Validate channels and internal channels sampling time limits (DS12923) */
    bool has_vref = false;
    bool has_temp = false;
    bool has_vbat = false;

    for (uint32_t i = 0U; i < config->num_channels; i++)
    {
        uint32_t ch  = config->channels[i].channel;
        uint32_t smp = config->channels[i].sampling_time;

        if ((ch > 19U) || (smp > ADC_LL_SAMPLETIME_810CYCLES_5))
        {
            return false;
        }

        if (config->adc_instance == ADC3)
        {
            uint32_t min_ns = 0U;
            if (ch == ADC_LL_CHANNEL_VREFINT)
            {
                has_vref = true;
                min_ns = 4300U; /* 4.3 us minimum */
            }
            else if (ch == ADC_LL_CHANNEL_TEMPSENSOR)
            {
                has_temp = true;
                min_ns = 9000U; /* 9.0 us minimum */
            }
            else if (ch == ADC_LL_CHANNEL_VBAT)
            {
                has_vbat = true;
                min_ns = 9000U; /* 9.0 us minimum */
            }

            if (min_ns > 0U)
            {
                uint64_t cycles_x10 = (uint64_t)s_sample_cycles_x10[smp & 7U];
                if ((cycles_x10 * 100000000ULL) < ((uint64_t)min_ns * (uint64_t)f_analog))
                {
                    return false;
                }
            }
        }
    }

    /* Validate oversampling parameters (F6): prevent 16-bit truncation */
    if (config->oversampling_enable)
    {
        uint32_t ratio = config->oversampling_ratio;
        uint32_t shift = config->oversampling_shift;

        if ((ratio < 2U) || (ratio > 1024U) || (shift > 8U))
        {
            return false;
        }

        uint32_t orig_bits;
        switch (config->resolution)
        {
            case ADC_LL_RESOLUTION_16B:     orig_bits = 16U; break;
            case ADC_LL_RESOLUTION_14B:
            case ADC_LL_RESOLUTION_14B_OPT: orig_bits = 14U; break;
            case ADC_LL_RESOLUTION_12B:
            case ADC_LL_RESOLUTION_12B_OPT: orig_bits = 12U; break;
            case ADC_LL_RESOLUTION_10B:     orig_bits = 10U; break;
            case ADC_LL_RESOLUTION_8B:      orig_bits = 8U;  break;
            default:                        orig_bits = 16U; break;
        }

        /* Added bits = ceil(log2(ratio)) */
        uint32_t add_bits = 0U;
        while ((1U << add_bits) < ratio)
        {
            add_bits++;
        }

        if ((orig_bits + add_bits - shift) > 16U)
        {
            return false;
        }
    }

    handle->config_ex = *config;
    handle->ready_half = 0U;
    handle->has_new_data = 0U;
    handle->stats = (AdcLl_Stats_t){0};

    handle->config.adc_instance    = config->adc_instance;
    handle->config.channel         = (config->num_channels > 0U && config->channels != NULL) ? config->channels[0].channel : 0U;
    handle->config.resolution      = config->resolution;
    handle->config.sampling_time   = (config->num_channels > 0U && config->channels != NULL) ? config->channels[0].sampling_time : 0U;
    handle->config.trigger_source  = config->trigger_source;
    handle->config.trigger_edge    = config->trigger_edge;
    handle->config.boost_mode      = config->boost_mode;
    handle->config.dma_mode        = config->dma_mode;
    handle->config.dma_circular    = config->dma_circular;
    handle->config.buffer          = config->buffer;
    handle->config.buffer_length   = config->buffer_length;
    handle->config.kernel_clock_hz = config->kernel_clock_hz;
    handle->config.clock_mode      = config->clock_mode;
    handle->config.prescaler       = config->prescaler;
    handle->config.analog_pin_port = config->analog_pin_port;
    handle->config.analog_pin_idx  = config->analog_pin_idx;

    ADC_TypeDef *adc = config->adc_instance;

    /* 0. Optional analog pin configuration */
    if (config->analog_pin_port != NULL)
    {
        if (!GPIO_BM_ConfigAnalog(config->analog_pin_port, config->analog_pin_idx))
        {
            return false;
        }
    }

    /* 1. Clock gating and common clock register */
    if ((adc == ADC1) || (adc == ADC2))
    {
        RCC->AHB1ENR |= RCC_AHB1ENR_ADC12EN;
        volatile uint32_t dummy = RCC->AHB1ENR;
        (void)dummy;
    }
#if defined(ADC3)
    else if (adc == ADC3)
    {
        RCC->AHB4ENR |= RCC_AHB4ENR_ADC3EN;
        volatile uint32_t dummy = RCC->AHB4ENR;
        (void)dummy;
    }
#endif
    else
    {
        return false;
    }

    /* Disable ADC before touching CCR or configuring registers */
    if (LL_ADC_IsEnabled(adc) != 0U)
    {
        (void)ADC_LL_Disable(adc);
    }

    /* Configure common clock register (safe now that target ADC is disabled) */
    if ((adc == ADC1) || (adc == ADC2))
    {
        if (!ADC_LL_ConfigureCommonClock(ADC12_COMMON, adc, config->clock_mode, config->prescaler))
        {
            return false;
        }
    }
#if defined(ADC3)
    else if (adc == ADC3)
    {
        if (!ADC_LL_ConfigureCommonClock(ADC3_COMMON, adc, config->clock_mode, config->prescaler))
        {
            return false;
        }

        uint32_t ccr = ADC3_COMMON->CCR;
        if (has_vref) { ccr |= ADC_CCR_VREFEN; }
        if (has_temp) { ccr |= ADC_CCR_TSEN; }
        if (has_vbat) { ccr |= ADC_CCR_VBATEN; }
        ADC3_COMMON->CCR = ccr;
    }
#endif
    else
    {
        return false;
    }

    /* Disable ADC before re-configuring */
    (void)ADC_LL_Disable(adc);

    /* Clean all configuration, sequencer, and channel select registers */
    adc->CFGR   = 0U;
    adc->CFGR2  = 0U;
    adc->PCSEL  = 0U;
    adc->SQR1   = 0U;
    adc->SQR2   = 0U;
    adc->SQR3   = 0U;
    adc->SQR4   = 0U;
    adc->SMPR1  = 0U;
    adc->SMPR2  = 0U;
    adc->DIFSEL = 0U;
    adc->IER    = 0U;
    adc->ISR    = adc->ISR;

    /* 2. Exit Deep-Power-Down mode */
    LL_ADC_DisableDeepPowerDown(adc);

    /* 3. Enable internal voltage regulator */
    LL_ADC_EnableInternalRegulator(adc);

    /* Wait for LDORDY (ADC_ISR bit 12) with loop fallback */
    {
        uint32_t timeout = ADC_LL_VREG_STUP_LOOPS;
        while ((LL_ADC_IsActiveFlag_LDORDY(adc) == 0U) && (timeout > 0U))
        {
            timeout--;
        }
    }

    /* 4. Configure BOOST[1:0] bits */
    uint32_t boost_bits;
    if (config->boost_mode == ADC_LL_BOOST_AUTO)
    {
        boost_bits = ADC_LL_GetBoostBits(eff_freq);
    }
    else
    {
        boost_bits = (uint32_t)config->boost_mode;
    }
    uint32_t cr = adc->CR;
    cr &= ~(ADC_CR_ADSTART | ADC_CR_JADSTART | ADC_CR_ADSTP |
            ADC_CR_JADSTP | ADC_CR_ADCAL | ADC_CR_BOOST);
    cr |= (boost_bits & ADC_CR_BOOST);
    adc->CR = cr;

    /* 5. Silicon Errata ES0445: disable injected context queue */
    adc->CFGR |= ADC_CFGR_JQDIS;

    /* 6. Configure resolution, DMA mode, overrun and trigger in CFGR */
    LL_ADC_SetResolution(adc, res_bits);
    LL_ADC_REG_SetOverrun(adc, LL_ADC_REG_OVR_DATA_OVERWRITTEN);

    if (config->dma_mode != ADC_LL_DMA_DISABLED)
    {
        LL_ADC_REG_SetDataTransferMode(adc, (uint32_t)config->dma_mode);
    }
    else if (config->dma_circular)
    {
        LL_ADC_REG_SetDataTransferMode(adc, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);
    }

    if (config->trigger_edge != ADC_LL_TRIGGER_DISABLED)
    {
        LL_ADC_REG_SetTriggerSource(adc, config->trigger_source << ADC_CFGR_EXTSEL_Pos);
        LL_ADC_REG_SetTriggerEdge(adc, config->trigger_edge);
    }

    /* 7. Oversampling configuration in CFGR2 */
    if (config->oversampling_enable)
    {
        adc->CFGR2 |= (ADC_CFGR2_ROVSE |
                       (config->oversampling_ratio & ADC_CFGR2_OVSR_Msk) |
                       (config->oversampling_shift & ADC_CFGR2_OVSS_Msk));
    }

    /* 8. Analog Watchdog 1 (AWD1) configuration */
    if (config->awd_enable)
    {
        LL_ADC_SetAnalogWDThresholds(adc, LL_ADC_AWD1, LL_ADC_AWD_THRESHOLD_HIGH, config->awd_high_threshold);
        LL_ADC_SetAnalogWDThresholds(adc, LL_ADC_AWD1, LL_ADC_AWD_THRESHOLD_LOW,  config->awd_low_threshold);

        if (config->awd_all_channels)
        {
            LL_ADC_SetAnalogWDMonitChannels(adc, LL_ADC_AWD1, LL_ADC_AWD_ALL_CHANNELS_REG);
        }
        else
        {
            uint32_t awd_ch = __LL_ADC_DECIMAL_NB_TO_CHANNEL(config->awd_channel);
            LL_ADC_SetAnalogWDMonitChannels(adc, LL_ADC_AWD1,
                (awd_ch & ADC_CHANNEL_ID_MASK) | ADC_CFGR_AWD1EN | ADC_CFGR_AWD1SGL);
        }
        LL_ADC_EnableIT_AWD1(adc);
    }

    /* 9. Configure channel preselection, sampling time, and sequencer */
    uint32_t ll_ranks[] = {
        LL_ADC_REG_RANK_1,  LL_ADC_REG_RANK_2,  LL_ADC_REG_RANK_3,  LL_ADC_REG_RANK_4,
        LL_ADC_REG_RANK_5,  LL_ADC_REG_RANK_6,  LL_ADC_REG_RANK_7,  LL_ADC_REG_RANK_8,
        LL_ADC_REG_RANK_9,  LL_ADC_REG_RANK_10, LL_ADC_REG_RANK_11, LL_ADC_REG_RANK_12,
        LL_ADC_REG_RANK_13, LL_ADC_REG_RANK_14, LL_ADC_REG_RANK_15, LL_ADC_REG_RANK_16
    };
    uint32_t seq_len_macro = (config->num_channels - 1U) << ADC_SQR1_L_Pos;
    LL_ADC_REG_SetSequencerLength(adc, seq_len_macro);

    for (uint32_t i = 0U; i < config->num_channels; i++)
    {
        uint32_t ch_dec = config->channels[i].channel;
        uint32_t smp    = config->channels[i].sampling_time;
        uint32_t ll_ch  = __LL_ADC_DECIMAL_NB_TO_CHANNEL(ch_dec);

        LL_ADC_SetChannelPreselection(adc, ll_ch);
        LL_ADC_SetChannelSingleDiff(adc, ll_ch, LL_ADC_SINGLE_ENDED);
        LL_ADC_SetChannelSamplingTime(adc, ll_ch, smp);
        LL_ADC_REG_SetSequencerRanks(adc, ll_ranks[i], ll_ch);
    }

    /* 10. Enable overrun interrupt in IER */
    LL_ADC_EnableIT_OVR(adc);

    return true;
}

bool ADC_LL_Init(AdcLl_Handle_t *handle, const AdcLl_Config_t *config)
{
    if ((handle == NULL) || (config == NULL))
    {
        return false;
    }

    handle->single_channel.channel       = config->channel;
    handle->single_channel.sampling_time = config->sampling_time;

    AdcLl_ConfigEx_t ex = {
        .adc_instance        = config->adc_instance,
        .channels            = &handle->single_channel,
        .num_channels        = 1U,
        .resolution          = config->resolution,
        .trigger_source      = config->trigger_source,
        .trigger_edge        = config->trigger_edge,
        .boost_mode          = config->boost_mode,
        .dma_mode            = config->dma_mode,
        .dma_circular        = config->dma_circular,
        .buffer              = config->buffer,
        .buffer_length       = config->buffer_length,
        .kernel_clock_hz     = config->kernel_clock_hz,
        .clock_mode          = config->clock_mode,
        .prescaler           = config->prescaler,
        .analog_pin_port     = config->analog_pin_port,
        .analog_pin_idx      = config->analog_pin_idx,
        .oversampling_enable = false,
        .oversampling_ratio  = 0U,
        .oversampling_shift  = 0U,
        .awd_enable          = false,
        .awd_all_channels    = false,
        .awd_channel         = 0U,
        .awd_high_threshold  = 0U,
        .awd_low_threshold   = 0U
    };

    bool ok = ADC_LL_InitEx(handle, &ex);
    if (ok)
    {
        handle->config = *config;
    }
    return ok;
}

bool ADC_LL_Deinit(AdcLl_Handle_t *handle)
{
    if (handle == NULL)
    {
        return false;
    }

    ADC_TypeDef *adc = (handle->config_ex.adc_instance != NULL)
                       ? handle->config_ex.adc_instance
                       : handle->config.adc_instance;
    if (adc == NULL)
    {
        return false;
    }

    (void)ADC_LL_Disable(adc);

    adc->IER = 0U;
    adc->ISR = adc->ISR;

    adc->CFGR   = 0U;
    adc->CFGR2  = 0U;
    adc->PCSEL  = 0U;
    adc->SQR1   = 0U;
    adc->SQR2   = 0U;
    adc->SQR3   = 0U;
    adc->SQR4   = 0U;
    adc->SMPR1  = 0U;
    adc->SMPR2  = 0U;
    adc->DIFSEL = 0U;

#if defined(ADC3)
    if (adc == ADC3)
    {
        ADC3_COMMON->CCR &= ~(ADC_CCR_VREFEN | ADC_CCR_TSEN | ADC_CCR_VBATEN);
    }
#endif

    LL_ADC_DisableInternalRegulator(adc);
    LL_ADC_EnableDeepPowerDown(adc);

    handle->ready_half = 0U;
    handle->has_new_data = 0U;

    return true;
}

bool ADC_LL_Calibrate(ADC_TypeDef *adc)
{
    if (adc == NULL)
    {
        return false;
    }

    if (LL_ADC_IsEnabled(adc) != 0U)
    {
        return false;
    }

    LL_ADC_StartCalibration(adc, LL_ADC_CALIB_OFFSET_LINEARITY, LL_ADC_SINGLE_ENDED);

    uint32_t timeout = ADC_LL_CALIBRATION_TIMEOUT_LOOPS;
    while ((LL_ADC_IsCalibrationOnGoing(adc) != 0U) && (timeout > 0U))
    {
        timeout--;
    }

    return (timeout > 0U);
}

bool ADC_LL_Enable(ADC_TypeDef *adc)
{
    if (adc == NULL)
    {
        return false;
    }

    LL_ADC_ClearFlag_ADRDY(adc);
    LL_ADC_Enable(adc);

    uint32_t timeout = ADC_LL_ENABLE_TIMEOUT_LOOPS;
    while ((LL_ADC_IsActiveFlag_ADRDY(adc) == 0U) && (timeout > 0U))
    {
        timeout--;
    }

    return (timeout > 0U);
}

bool ADC_LL_Disable(ADC_TypeDef *adc)
{
    if (adc == NULL)
    {
        return false;
    }

    if (LL_ADC_IsEnabled(adc) == 0U)
    {
        return true;
    }

    if (LL_ADC_REG_IsConversionOngoing(adc) != 0U)
    {
        if (!ADC_LL_StopConversion(adc))
        {
            return false;
        }
    }

    LL_ADC_Disable(adc);

    uint32_t timeout = ADC_LL_DISABLE_TIMEOUT_LOOPS;
    while ((LL_ADC_IsEnabled(adc) != 0U) && (timeout > 0U))
    {
        timeout--;
    }

    return (timeout > 0U);
}

void ADC_LL_StartConversion(ADC_TypeDef *adc)
{
    if (adc != NULL)
    {
        LL_ADC_REG_StartConversion(adc);
    }
}

bool ADC_LL_StopConversion(ADC_TypeDef *adc)
{
    if (adc == NULL)
    {
        return false;
    }

    if (LL_ADC_REG_IsConversionOngoing(adc) == 0U)
    {
        return true;
    }

    LL_ADC_REG_StopConversion(adc);

    uint32_t timeout = ADC_LL_STOP_TIMEOUT_LOOPS;
    while ((LL_ADC_REG_IsConversionOngoing(adc) != 0U) && (timeout > 0U))
    {
        timeout--;
    }

    return (timeout > 0U);
}

bool ADC_LL_ReadSingle(ADC_TypeDef *adc, uint32_t channel, uint32_t sampling_time, uint16_t *out_raw, uint32_t spin_limit)
{
    if ((adc == NULL) || (channel > 19U) || (out_raw == NULL))
    {
        return false;
    }

    /* Reject if a regular conversion is in progress or DMA is active */
    if ((LL_ADC_REG_IsConversionOngoing(adc) != 0U) || ((adc->CFGR & ADC_CFGR_DMNGT) != 0U))
    {
        return false;
    }

    /* 1. Ensure ADC is enabled */
    if (LL_ADC_IsEnabled(adc) == 0U)
    {
        if (!ADC_LL_Enable(adc))
        {
            return false;
        }
    }

    /* 2. Channel preselection */
    uint32_t ll_ch = __LL_ADC_DECIMAL_NB_TO_CHANNEL(channel);
    LL_ADC_SetChannelPreselection(adc, ll_ch);

    /* 3. Configure sequence length 1, SQ1 = channel */
    LL_ADC_REG_SetSequencerLength(adc, LL_ADC_REG_SEQ_SCAN_DISABLE);
    LL_ADC_REG_SetSequencerRanks(adc, LL_ADC_REG_RANK_1, ll_ch);

    /* 4. Configure sampling time */
    LL_ADC_SetChannelSamplingTime(adc, ll_ch, sampling_time);

    /* 5. Trigger: software */
    LL_ADC_REG_SetTriggerEdge(adc, LL_ADC_REG_TRIG_EXT_RISING);
    adc->CFGR &= ~ADC_CFGR_EXTEN;

    /* 6. Clear pending EOC flag and start conversion */
    LL_ADC_ClearFlag_EOC(adc);
    LL_ADC_REG_StartConversion(adc);

    /* 7. Poll EOC */
    uint32_t count = spin_limit;
    while ((LL_ADC_IsActiveFlag_EOC(adc) == 0U) && (count > 0U))
    {
        count--;
    }

    if (count == 0U)
    {
        return false;
    }

    /* 8. Read result */
    *out_raw = (uint16_t)LL_ADC_REG_ReadConversionData16(adc);
    LL_ADC_ClearFlag_EOC(adc);

    return true;
}

void ADC_LL_IRQHandler_DMA(AdcLl_Handle_t *handle, DMA_Stream_TypeDef *stream)
{
    if ((handle == NULL) || (stream == NULL))
    {
        return;
    }

    uint32_t buf_len = (handle->config_ex.buffer_length > 0U)
                       ? handle->config_ex.buffer_length
                       : handle->config.buffer_length;
    uint32_t half_samples = buf_len / 2U;

    uint32_t flags = DMA_BM_GetFlags(stream);
    DMA_BM_ClearFlags(stream, flags);

    if ((flags & DMA_BM_FLAG_HT) != 0U)
    {
        if (handle->has_new_data != 0U)
        {
            handle->stats.half_overrun_count++;
        }
        handle->ready_half = 0U;
        handle->has_new_data = 1U;
        handle->stats.dma_ht_count++;
        handle->stats.sample_count += half_samples;
    }

    if ((flags & DMA_BM_FLAG_TC) != 0U)
    {
        if (handle->has_new_data != 0U)
        {
            handle->stats.half_overrun_count++;
        }
        handle->ready_half = 1U;
        handle->has_new_data = 1U;
        handle->stats.dma_tc_count++;
        handle->stats.sample_count += half_samples;
    }

    if ((flags & (DMA_BM_FLAG_TE | DMA_BM_FLAG_DME | DMA_BM_FLAG_FE)) != 0U)
    {
        handle->stats.dma_error_count++;
    }
}

void ADC_LL_IRQHandler_ADC(AdcLl_Handle_t *handle)
{
    if (handle == NULL)
    {
        return;
    }

    ADC_TypeDef *adc = (handle->config_ex.adc_instance != NULL)
                       ? handle->config_ex.adc_instance
                       : handle->config.adc_instance;
    if (adc == NULL)
    {
        return;
    }

    if (LL_ADC_IsActiveFlag_OVR(adc) != 0U)
    {
        LL_ADC_ClearFlag_OVR(adc);
        handle->stats.adc_overrun_count++;
    }

    if (LL_ADC_IsActiveFlag_AWD1(adc) != 0U)
    {
        LL_ADC_ClearFlag_AWD1(adc);
        LL_ADC_DisableIT_AWD1(adc);    /* Mask AWD1 interrupt to prevent storm */
        handle->stats.awd_event_count++;
    }
}

uint32_t ADC_LL_TakeAwdEvents(AdcLl_Handle_t *handle)
{
    if (handle == NULL)
    {
        return 0U;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint32_t events = handle->stats.awd_event_count;
    handle->stats.awd_event_count = 0U;

    ADC_TypeDef *adc = (handle->config_ex.adc_instance != NULL)
                       ? handle->config_ex.adc_instance
                       : handle->config.adc_instance;
    if ((adc != NULL) && handle->config_ex.awd_enable)
    {
        LL_ADC_ClearFlag_AWD1(adc);
        LL_ADC_EnableIT_AWD1(adc);     /* Re-arm interrupt */
    }

    __set_PRIMASK(primask);
    return events;
}

bool ADC_LL_GetReadyHalf(AdcLl_Handle_t *handle, uint8_t *out_half)
{
    if ((handle == NULL) || (out_half == NULL))
    {
        return false;
    }

    uint32_t primask = __get_PRIMASK();
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

void ADC_LL_ComputeStats(AdcLl_Handle_t *handle, uint8_t half, uint32_t vref_mv)
{
    ADC_LL_ComputeStatsChannel(handle, half, 0U, vref_mv);
}

void ADC_LL_ComputeStatsChannel(AdcLl_Handle_t *handle, uint8_t half, uint32_t channel_seq_idx, uint32_t vref_mv)
{
    if (handle == NULL)
    {
        return;
    }

    const uint16_t *buf = (handle->config_ex.buffer != NULL)
                          ? handle->config_ex.buffer
                          : handle->config.buffer;
    if (buf == NULL)
    {
        return;
    }

    uint32_t buffer_length = (handle->config_ex.buffer_length > 0U)
                             ? handle->config_ex.buffer_length
                             : handle->config.buffer_length;

    uint32_t num_channels = (handle->config_ex.num_channels > 0U)
                            ? handle->config_ex.num_channels : 1U;
    if (channel_seq_idx >= num_channels)
    {
        return;
    }

    uint32_t half_len = buffer_length / 2U;
    if (half_len == 0U)
    {
        return;
    }

    uint32_t start_idx = (half == 0U) ? 0U : half_len;
    uint32_t end_idx = start_idx + half_len;

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
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

    uint32_t resolution = (handle->config_ex.resolution != 0U)
                          ? handle->config_ex.resolution
                          : handle->config.resolution;
    uint32_t max_code;
    switch (resolution)
    {
        case ADC_LL_RESOLUTION_16B:     max_code = 65535U; break;
        case ADC_LL_RESOLUTION_14B:     max_code = 16383U; break;
        case ADC_LL_RESOLUTION_14B_OPT: max_code = 16383U; break;
        case ADC_LL_RESOLUTION_12B:     max_code = 4095U;  break;
        case ADC_LL_RESOLUTION_12B_OPT: max_code = 4095U;  break;
        case ADC_LL_RESOLUTION_10B:     max_code = 1023U;  break;
        case ADC_LL_RESOLUTION_8B:      max_code = 255U;   break;
        default:                        max_code = 65535U; break;
    }

    /* 65535 * 3300 = 216,265,500 < 2^32-1: fits safely in uint32_t without 64-bit libcalls */
    handle->stats.millivolts = ((uint32_t)handle->stats.average_raw * vref_mv) / max_code;
}

uint32_t ADC_LL_CalculateVDDA(uint16_t vrefint_raw, uint8_t resolution_bits)
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
    uint32_t cal = (uint32_t)(*VREFINT_CAL_ADDR);
    /* VREFINT_CAL is measured at VDDA = 3300 mV at 16-bit resolution */
    return (uint32_t)((3300UL * cal) / raw_16b);
}

float ADC_LL_CalculateTemperature(uint16_t temp_raw, uint32_t vdda_mv, uint8_t resolution_bits)
{
    uint32_t raw_16b = (uint32_t)temp_raw;
    if ((resolution_bits > 0U) && (resolution_bits < 16U))
    {
        raw_16b <<= (16U - resolution_bits);
    }
    float cal1 = (float)(*TEMPSENSOR_CAL1_ADDR);
    float cal2 = (float)(*TEMPSENSOR_CAL2_ADDR);

    if (cal2 <= cal1)
    {
        return 0.0f;
    }

    /* Scale raw reading if VDDA differs from nominal 3300 mV */
    float raw_norm = (float)raw_16b;
    if (vdda_mv > 0U)
    {
        raw_norm = raw_norm * ((float)vdda_mv / 3300.0f);
    }

    /* STM32H755: TS_CAL1 @ 30 deg C, TS_CAL2 @ 130 deg C (range = 100 deg C) */
    return ((130.0f - 30.0f) / (cal2 - cal1)) * (raw_norm - cal1) + 30.0f;
}
