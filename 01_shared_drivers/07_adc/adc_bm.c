/**
  ******************************************************************************
  * @file    adc_bm.c
  * @brief   Bare-metal ADC driver implementation for STM32H755.
  ******************************************************************************
  */

#include "adc_bm.h"
#include <stddef.h>
#include "gpio_bm.h"

#define ADC_BM_VREG_STUP_LOOPS           10000UL
#define ADC_BM_CALIBRATION_TIMEOUT_LOOPS 10000000UL
#define ADC_BM_ENABLE_TIMEOUT_LOOPS      10000000UL
#define ADC_BM_DISABLE_TIMEOUT_LOOPS     10000000UL
#define ADC_BM_STOP_TIMEOUT_LOOPS        10000000UL

#ifndef ADC_CR_BITS_PROPERTY_RS
#define ADC_CR_BITS_PROPERTY_RS (ADC_CR_ADCAL | ADC_CR_JADSTP | ADC_CR_ADSTP | \
                                 ADC_CR_JADSTART | ADC_CR_ADSTART | ADC_CR_ADDIS | ADC_CR_ADEN)
#endif

/* Prescaler divisors indexed by AdcBm_Prescaler_t value */
static const uint16_t s_presc_divs[] = {
    1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256
};
#define ADC_BM_PRESC_COUNT (sizeof(s_presc_divs) / sizeof(s_presc_divs[0]))

/**
  * @brief  Translates the logical resolution constant to the CFGR.RES bit pattern
  *         appropriate for the current silicon revision.
  * @return CFGR.RES bits on success, 0xFFFFFFFFUL on invalid/unsupported combo.
  */
static uint32_t ADC_BM_TranslateResolution(uint32_t resolution)
{
    uint32_t rev_id = ((DBGMCU->IDCODE & 0xFFFF0000UL) >> 16U);
    bool is_rev_v = (rev_id > 0x1003U);

    if (is_rev_v)
    {
        /* Rev V encoding (RM0399 rev 4+):
         * 000 = 16-bit, 001 = 14-bit, 010 = 12-bit, 011 = 10-bit,
         * 101 = 14-bit optimized, 110 = 12-bit optimized, 111 = 8-bit.
         * 100 = Reserved, must not be used. */
        switch (resolution)
        {
            case ADC_BM_RESOLUTION_16B:     return 0x00000000UL;
            case ADC_BM_RESOLUTION_14B:     return ADC_CFGR_RES_0;
            case ADC_BM_RESOLUTION_12B:     return ADC_CFGR_RES_1;
            case ADC_BM_RESOLUTION_10B:     return (ADC_CFGR_RES_0 | ADC_CFGR_RES_1);
            case ADC_BM_RESOLUTION_8B:      return (ADC_CFGR_RES_2 | ADC_CFGR_RES_1 | ADC_CFGR_RES_0);
            case ADC_BM_RESOLUTION_14B_OPT: return (ADC_CFGR_RES_2 | ADC_CFGR_RES_0);
            case ADC_BM_RESOLUTION_12B_OPT: return (ADC_CFGR_RES_2 | ADC_CFGR_RES_1);
            default: return 0xFFFFFFFFUL;
        }
    }
    else /* Rev Y */
    {
        /* Rev Y encoding: 00 = 16b, 01 = 14b, 10 = 12b, 11 = 10b, 100 = 8b.
         * Optimized modes not available on Rev Y. */
        if ((resolution == ADC_BM_RESOLUTION_14B_OPT) ||
            (resolution == ADC_BM_RESOLUTION_12B_OPT))
        {
            return 0xFFFFFFFFUL;
        }
        /* On Rev Y, the existing constants map directly to hardware bits */
        return resolution;
    }
}

/**
  * @brief  Writes ADCx_CCR.CKMODE and PRESC only when the partner ADC is off.
  *
  *         RM0399 §26.4.3: these fields must not be changed while either ADC of
  *         the pair is enabled. If the other ADC is already running, this
  *         function verifies that the current CCR matches the request and fails
  *         without writing if it does not, rather than silently overwriting it.
  *
  * @retval true   CCR is now set (or was already correct).
  * @retval false  Conflict: the other ADC is enabled with different settings.
  */
static bool ADC_BM_ConfigureCCR(ADC_Common_TypeDef *common,
                                ADC_TypeDef *adc,
                                AdcBm_ClockMode_t clock_mode,
                                AdcBm_Prescaler_t prescaler)
{
    /* RM0399 §26.4.3: CKMODE and PRESC can only be written when ALL ADCs of the common block
     * are disabled (ADEN = 0). Check both the target ADC and its partner. */
    bool any_enabled = ((adc->CR & ADC_CR_ADEN) != 0U);
    if (common == ADC12_COMMON)
    {
        ADC_TypeDef *other = (adc == ADC1) ? ADC2 : ADC1;
        if ((other->CR & ADC_CR_ADEN) != 0U)
        {
            any_enabled = true;
        }
    }

    uint32_t desired_ckmode = ((uint32_t)clock_mode) & ADC_CCR_CKMODE_Msk;
    uint32_t desired_presc  = (((uint32_t)prescaler) << ADC_CCR_PRESC_Pos) & ADC_CCR_PRESC_Msk;

    if (any_enabled)
    {
        /* Cannot write CCR - verify it already has what we need */
        uint32_t current = common->CCR;
        if (((current & ADC_CCR_CKMODE_Msk) != desired_ckmode) ||
            ((current & ADC_CCR_PRESC_Msk)  != desired_presc))
        {
            return false; /* Conflict with enabled ADC configuration */
        }
        return true; /* Already correct, nothing to write */
    }

    /* All relevant ADCs are disabled - safe to write */
    uint32_t ccr = common->CCR;
    ccr &= ~(ADC_CCR_CKMODE | ADC_CCR_PRESC);
    ccr |= desired_ckmode;
    ccr |= desired_presc;
    common->CCR = ccr;
    return true;
}

uint32_t ADC_BM_GetBoostBits(uint32_t ker_ck_freq_hz)
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

#ifndef VREFINT_CAL_ADDR
#define VREFINT_CAL_ADDR     ((const uint16_t *)(0x1FF1E860UL))
#endif
#ifndef TEMPSENSOR_CAL1_ADDR
#define TEMPSENSOR_CAL1_ADDR ((const uint16_t *)(0x1FF1E820UL))
#endif
#ifndef TEMPSENSOR_CAL2_ADDR
#define TEMPSENSOR_CAL2_ADDR ((const uint16_t *)(0x1FF1E840UL))
#endif

static inline uint16_t ADC_BM_GetVrefintCal(void)
{
#if defined(CORE_CM4)
    /* RM0399 Section 2.1 / Figure 1 Note 2: System Memory (0x1FF00000 - 0x1FFFFFFF) is in D1
     * and NOT decoded by the D2-to-D1 AHB bridge. Access from Cortex-M4 generates
     * a HardFault. If pointing to default 0x1FF1xxxx address, return nominal factory
     * calibration (1.212V @ 3.3V 16-bit: (1.212/3.3)*65535 = 24068 -> 24200). */
    if (((uint32_t)VREFINT_CAL_ADDR >= 0x1FF00000UL) &&
        ((uint32_t)VREFINT_CAL_ADDR <= 0x1FFFFFFFUL))
    {
        return 24200U;
    }
#endif
    return *VREFINT_CAL_ADDR;
}

static inline uint16_t ADC_BM_GetTsCal1(void)
{
#if defined(CORE_CM4)
    if (((uint32_t)TEMPSENSOR_CAL1_ADDR >= 0x1FF00000UL) &&
        ((uint32_t)TEMPSENSOR_CAL1_ADDR <= 0x1FFFFFFFUL))
    {
        return 12365U; /* Nominal 30 deg C @ 3.3V, 16-bit */
    }
#endif
    return *TEMPSENSOR_CAL1_ADDR;
}

static inline uint16_t ADC_BM_GetTsCal2(void)
{
#if defined(CORE_CM4)
    if (((uint32_t)TEMPSENSOR_CAL2_ADDR >= 0x1FF00000UL) &&
        ((uint32_t)TEMPSENSOR_CAL2_ADDR <= 0x1FFFFFFFUL))
    {
        return 16276U; /* Nominal 130 deg C @ 3.3V, 16-bit */
    }
#endif
    return *TEMPSENSOR_CAL2_ADDR;
}

/* Sample time cycles x10 for 1.5, 2.5, 8.5, 16.5, 32.5, 64.5, 387.5, 810.5 */
static const uint16_t s_sample_cycles_x10[] = {15, 25, 85, 165, 325, 645, 3875, 8105};

bool ADC_BM_InitEx(AdcBm_Handle_t *handle, const AdcBm_ConfigEx_t *config)
{
    if ((handle == NULL) || (config == NULL) || (config->adc_instance == NULL) ||
        (config->channels == NULL) || (config->num_channels == 0U) || (config->num_channels > 16U))
    {
        return false;
    }

    bool needs_dma = (config->dma_mode != ADC_BM_DMA_DISABLED) || config->dma_circular;
    if (needs_dma)
    {
        if ((config->buffer == NULL) || (config->buffer_length < 2U) ||
            ((config->buffer_length & 1U) != 0U))
        {
            return false;
        }
    }

    uint32_t res_bits = ADC_BM_TranslateResolution(config->resolution);
    if (res_bits == 0xFFFFFFFFUL)
    {
        return false;
    }

    if ((uint32_t)config->prescaler >= ADC_BM_PRESC_COUNT)
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
    if (config->clock_mode == ADC_BM_CLOCK_ASYNC)
    {
        uint32_t pidx = (uint32_t)config->prescaler;
        if (pidx < ADC_BM_PRESC_COUNT)
        {
            eff_freq /= s_presc_divs[pidx];
        }
    }
    else
    {
        /* Synchronous clock mode: presc ignored, CKMODE divides adc_sclk */
        if (config->clock_mode == ADC_BM_CLOCK_SYNC_2) { eff_freq /= 2U; }
        else if (config->clock_mode == ADC_BM_CLOCK_SYNC_4) { eff_freq /= 4U; }
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
        uint32_t ch = config->channels[i].channel;
        uint32_t smp = config->channels[i].sampling_time;

        if ((ch > 19U) || (smp > ADC_BM_SAMPLETIME_810CYCLES_5))
        {
            return false;
        }

        if (config->adc_instance == ADC3)
        {
            uint32_t min_ns = 0U;
            if (ch == ADC_BM_CHANNEL_VREFINT)
            {
                has_vref = true;
                min_ns = 4300U; /* 4.3 us minimum */
            }
            else if (ch == ADC_BM_CHANNEL_TEMPSENSOR)
            {
                has_temp = true;
                min_ns = 9000U; /* 9.0 us minimum */
            }
            else if (ch == ADC_BM_CHANNEL_VBAT)
            {
                has_vbat = true;
                min_ns = 9000U; /* 9.0 us minimum */
            }

            if (min_ns > 0U)
            {
                uint64_t cycles_x10 = (uint64_t)s_sample_cycles_x10[smp & 7U];
                /* cycles_x10 / 10 / f_analog >= min_ns / 1,000,000,000 */
                if ((cycles_x10 * 100000000ULL) < ((uint64_t)min_ns * (uint64_t)f_analog))
                {
                    return false; /* Sampling time is shorter than datasheet limit */
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

        /* Original bits */
        uint32_t orig_bits;
        switch (config->resolution)
        {
            case ADC_BM_RESOLUTION_16B:     orig_bits = 16U; break;
            case ADC_BM_RESOLUTION_14B:
            case ADC_BM_RESOLUTION_14B_OPT: orig_bits = 14U; break;
            case ADC_BM_RESOLUTION_12B:
            case ADC_BM_RESOLUTION_12B_OPT: orig_bits = 12U; break;
            case ADC_BM_RESOLUTION_10B:     orig_bits = 10U; break;
            case ADC_BM_RESOLUTION_8B:      orig_bits = 8U;  break;
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
            return false; /* Result exceeds 16 bits and would be truncated by halfword DMA */
        }
    }

    handle->config_ex = *config;
    handle->ready_half = 0U;
    handle->has_new_data = 0U;
    handle->stats = (AdcBm_Stats_t){0};

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

    /* 1. Enable peripheral clock in RCC */
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
    if ((adc->CR & ADC_CR_ADEN) != 0U)
    {
        (void)ADC_BM_Disable(adc);
    }

    /* Configure common clock register (safe now that target ADC is disabled) */
    if ((adc == ADC1) || (adc == ADC2))
    {
        if (!ADC_BM_ConfigureCCR(ADC12_COMMON, adc, config->clock_mode, config->prescaler))
        {
            return false;
        }
    }
#if defined(ADC3)
    else if (adc == ADC3)
    {
        if (!ADC_BM_ConfigureCCR(ADC3_COMMON, adc, config->clock_mode, config->prescaler))
        {
            return false;
        }

        /* Enable internal channel paths in ADC3_COMMON->CCR */
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
    (void)ADC_BM_Disable(adc);

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
    adc->ISR    = adc->ISR; /* Clear any pending rc_w1 flags */

    /* 2. Exit Deep-Power-Down mode */
    adc->CR &= ~ADC_CR_DEEPPWD;

    /* 3. Enable internal voltage regulator */
    adc->CR |= ADC_CR_ADVREGEN;

    /* Wait for LDORDY (ADC_ISR bit 12) with loop fallback */
    {
        uint32_t timeout = ADC_BM_VREG_STUP_LOOPS;
        while (((adc->ISR & ADC_ISR_LDORDY) == 0U) && (timeout > 0U))
        {
            timeout--;
        }
    }

    /* 4. Configure BOOST[1:0] bits */
    uint32_t boost_bits;
    if (config->boost_mode == ADC_BM_BOOST_AUTO)
    {
        boost_bits = ADC_BM_GetBoostBits(eff_freq);
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

    /* 6. Configure resolution, DMA mode and external trigger in CFGR */
    uint32_t cfgr = adc->CFGR;
    cfgr &= ~(ADC_CFGR_EXTSEL | ADC_CFGR_EXTEN | ADC_CFGR_RES | ADC_CFGR_DMNGT);
    cfgr |= (res_bits & ADC_CFGR_RES_Msk);
    cfgr |= ADC_CFGR_OVRMOD; /* Overwrite old data on overrun */

    if (config->dma_mode != ADC_BM_DMA_DISABLED)
    {
        cfgr |= (config->dma_mode & ADC_CFGR_DMNGT_Msk);
    }
    else if (config->dma_circular)
    {
        cfgr |= (ADC_CFGR_DMNGT_0 | ADC_CFGR_DMNGT_1);
    }

    if (config->trigger_edge != ADC_BM_TRIGGER_DISABLED)
    {
        cfgr |= ((config->trigger_source << ADC_CFGR_EXTSEL_Pos) & ADC_CFGR_EXTSEL_Msk);
        cfgr |= ((config->trigger_edge << ADC_CFGR_EXTEN_Pos) & ADC_CFGR_EXTEN_Msk);
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
        adc->HTR1 = (config->awd_high_threshold & ADC_HTR_HT_Msk);
        adc->LTR1 = (config->awd_low_threshold  & ADC_LTR_LT_Msk);

        cfgr |= ADC_CFGR_AWD1EN;
        if (!config->awd_all_channels)
        {
            cfgr |= (ADC_CFGR_AWD1SGL | ((config->awd_channel & 0x1FUL) << ADC_CFGR_AWD1CH_Pos));
        }
        adc->IER |= ADC_IER_AWD1IE;
    }

    adc->CFGR = cfgr;

    /* 9. Configure channel preselection, sampling time, and sequencer (SQR1..4) */
    adc->SQR1 &= ~ADC_SQR1_L;
    adc->SQR1 |= (((config->num_channels - 1U) << ADC_SQR1_L_Pos) & ADC_SQR1_L_Msk);

    for (uint32_t i = 0U; i < config->num_channels; i++)
    {
        uint32_t ch  = config->channels[i].channel;
        uint32_t smp = config->channels[i].sampling_time;

        /* Channel preselection */
        adc->PCSEL |= (1UL << (ch & 0x1FUL));

        /* Single-ended mode */
        adc->DIFSEL &= ~(1UL << (ch & 0x1FUL));

        /* Sampling time */
        if (ch <= 9U)
        {
            adc->SMPR1 &= ~(0x7UL << (ch * 3U));
            adc->SMPR1 |= ((smp & 0x7UL) << (ch * 3U));
        }
        else
        {
            adc->SMPR2 &= ~(0x7UL << ((ch - 10U) * 3U));
            adc->SMPR2 |= ((smp & 0x7UL) << ((ch - 10U) * 3U));
        }

        /* Rank in regular sequence (rank 1 to 16) */
        uint32_t rank = i + 1U;
        if (rank <= 4U)
        {
            uint32_t shift = (rank == 1U) ? ADC_SQR1_SQ1_Pos :
                             (rank == 2U) ? ADC_SQR1_SQ2_Pos :
                             (rank == 3U) ? ADC_SQR1_SQ3_Pos : ADC_SQR1_SQ4_Pos;
            adc->SQR1 &= ~(0x1FUL << shift);
            adc->SQR1 |= ((ch & 0x1FUL) << shift);
        }
        else if (rank <= 9U)
        {
            uint32_t shift = (rank - 5U) * 6U;
            adc->SQR2 &= ~(0x1FUL << shift);
            adc->SQR2 |= ((ch & 0x1FUL) << shift);
        }
        else if (rank <= 14U)
        {
            uint32_t shift = (rank - 10U) * 6U;
            adc->SQR3 &= ~(0x1FUL << shift);
            adc->SQR3 |= ((ch & 0x1FUL) << shift);
        }
        else
        {
            uint32_t shift = (rank - 15U) * 6U;
            adc->SQR4 &= ~(0x1FUL << shift);
            adc->SQR4 |= ((ch & 0x1FUL) << shift);
        }
    }

    /* 10. Enable overrun interrupt in IER */
    adc->IER |= ADC_IER_OVRIE;

    return true;
}

bool ADC_BM_Init(AdcBm_Handle_t *handle, const AdcBm_Config_t *config)
{
    if ((handle == NULL) || (config == NULL))
    {
        return false;
    }

    handle->single_channel.channel       = config->channel;
    handle->single_channel.sampling_time = config->sampling_time;

    AdcBm_ConfigEx_t ex = {
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

    bool ok = ADC_BM_InitEx(handle, &ex);
    if (ok)
    {
        handle->config = *config;
    }
    return ok;
}

bool ADC_BM_Deinit(AdcBm_Handle_t *handle)
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

    /* 1. Stop conversion and disable ADC */
    (void)ADC_BM_Disable(adc);

    /* 2. Disable interrupts and clear flags */
    adc->IER = 0U;
    adc->ISR = adc->ISR; /* Clear all rc_w1 flags */

    /* 3. Clear configuration and channel select registers */
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
        /* Clear internal channel enables in ADC3_COMMON->CCR */
        ADC3_COMMON->CCR &= ~(ADC_CCR_VREFEN | ADC_CCR_TSEN | ADC_CCR_VBATEN);
    }
#endif

    /* 4. Disable internal voltage regulator and enter deep-power-down */
    adc->CR &= ~ADC_CR_ADVREGEN;
    adc->CR |= ADC_CR_DEEPPWD;

    /* 5. Clear handle state */
    handle->ready_half = 0U;
    handle->has_new_data = 0U;

    return true;
}

bool ADC_BM_Calibrate(ADC_TypeDef *adc)
{
    if (adc == NULL)
    {
        return false;
    }

    /* Calibration is allowed only when ADC is disabled (ADEN = 0) */
    if ((adc->CR & ADC_CR_ADEN) != 0U)
    {
        return false;
    }

    /* Single-ended input calibration with linearity calibration enabled */
    uint32_t cr = adc->CR;
    cr &= ~(ADC_CR_ADCALDIF | ADC_CR_BITS_PROPERTY_RS);
    cr |= (ADC_CR_ADCALLIN | ADC_CR_ADCAL);
    adc->CR = cr;

    uint32_t timeout = ADC_BM_CALIBRATION_TIMEOUT_LOOPS;
    while (((adc->CR & ADC_CR_ADCAL) != 0U) && (timeout > 0U))
    {
        timeout--;
    }

    return (timeout > 0U);
}

bool ADC_BM_Enable(ADC_TypeDef *adc)
{
    if (adc == NULL)
    {
        return false;
    }

    /* Clear ADRDY flag (rc_w1: write 1 to clear) */
    adc->ISR = ADC_ISR_ADRDY;

    /* Enable ADC without re-triggering other RS bits */
    uint32_t cr = adc->CR;
    cr &= ~ADC_CR_BITS_PROPERTY_RS;
    cr |= ADC_CR_ADEN;
    adc->CR = cr;

    uint32_t timeout = ADC_BM_ENABLE_TIMEOUT_LOOPS;
    while (((adc->ISR & ADC_ISR_ADRDY) == 0U) && (timeout > 0U))
    {
        timeout--;
    }

    return (timeout > 0U);
}

bool ADC_BM_Disable(ADC_TypeDef *adc)
{
    if (adc == NULL)
    {
        return false;
    }

    if ((adc->CR & ADC_CR_ADEN) == 0U)
    {
        return true;
    }

    /* Stop ongoing conversions if active before requesting disable */
    if ((adc->CR & ADC_CR_ADSTART) != 0U)
    {
        if (!ADC_BM_StopConversion(adc))
        {
            return false;
        }
    }

    uint32_t cr = adc->CR;
    cr &= ~ADC_CR_BITS_PROPERTY_RS;
    cr |= ADC_CR_ADDIS;
    adc->CR = cr;

    uint32_t timeout = ADC_BM_DISABLE_TIMEOUT_LOOPS;
    while (((adc->CR & ADC_CR_ADEN) != 0U) && (timeout > 0U))
    {
        timeout--;
    }

    return (timeout > 0U);
}

void ADC_BM_StartConversion(ADC_TypeDef *adc)
{
    if (adc != NULL)
    {
        uint32_t cr = adc->CR;
        cr &= ~ADC_CR_BITS_PROPERTY_RS;
        cr |= ADC_CR_ADSTART;
        adc->CR = cr;
    }
}

bool ADC_BM_StopConversion(ADC_TypeDef *adc)
{
    if (adc == NULL)
    {
        return false;
    }

    if ((adc->CR & ADC_CR_ADSTART) == 0U)
    {
        return true;
    }

    uint32_t cr = adc->CR;
    cr &= ~ADC_CR_BITS_PROPERTY_RS;
    cr |= ADC_CR_ADSTP;
    adc->CR = cr;

    uint32_t timeout = ADC_BM_STOP_TIMEOUT_LOOPS;
    while (((adc->CR & (ADC_CR_ADSTP | ADC_CR_ADSTART)) != 0U) && (timeout > 0U))
    {
        timeout--;
    }

    return (timeout > 0U);
}

void ADC_BM_IRQHandler_DMA(AdcBm_Handle_t *handle, DMA_Stream_TypeDef *stream)
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

bool ADC_BM_ReadSingle(ADC_TypeDef *adc, uint32_t channel, uint32_t sampling_time, uint16_t *out_raw, uint32_t spin_limit)
{
    if ((adc == NULL) || (channel > 19U) || (out_raw == NULL))
    {
        return false;
    }

    /* Reject if a regular conversion is in progress or DMA is active */
    if (((adc->CR & ADC_CR_ADSTART) != 0U) || ((adc->CFGR & ADC_CFGR_DMNGT) != 0U))
    {
        return false;
    }

    /* 1. Ensure ADC is enabled */
    if ((adc->CR & ADC_CR_ADEN) == 0U)
    {
        if (!ADC_BM_Enable(adc))
        {
            return false;
        }
    }

    /* 2. Channel preselection (mandatory on H7) */
    adc->PCSEL |= (1UL << (channel & 0x1FUL));

    /* 3. Configure sequence length 1, SQ1 = channel */
    adc->SQR1 &= ~(ADC_SQR1_L | ADC_SQR1_SQ1);
    adc->SQR1 |= ((channel & 0x1FUL) << ADC_SQR1_SQ1_Pos);

    /* 4. Configure sampling time */
    if (channel <= 9U)
    {
        adc->SMPR1 &= ~(0x7UL << (channel * 3U));
        adc->SMPR1 |= ((sampling_time & 0x7UL) << (channel * 3U));
    }
    else
    {
        adc->SMPR2 &= ~(0x7UL << ((channel - 10U) * 3U));
        adc->SMPR2 |= ((sampling_time & 0x7UL) << ((channel - 10U) * 3U));
    }

    /* 5. Trigger: software (EXTEN = 0) */
    adc->CFGR &= ~ADC_CFGR_EXTEN;

    /* 6. Clear pending EOC flag and start conversion */
    adc->ISR = ADC_ISR_EOC;
    adc->CR |= ADC_CR_ADSTART;

    /* 7. Poll EOC */
    uint32_t count = spin_limit;
    while (((adc->ISR & ADC_ISR_EOC) == 0U) && (count > 0U))
    {
        count--;
    }

    if (count == 0U)
    {
        return false;
    }

    /* 8. Read result */
    *out_raw = (uint16_t)(adc->DR & 0xFFFFU);
    adc->ISR = ADC_ISR_EOC;

    return true;
}

void ADC_BM_IRQHandler_ADC(AdcBm_Handle_t *handle)
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

    uint32_t isr = adc->ISR;
    if ((isr & ADC_ISR_OVR) != 0U)
    {
        adc->ISR = ADC_ISR_OVR; /* Clear overrun flag */
        handle->stats.adc_overrun_count++;
    }

    if ((isr & ADC_ISR_AWD1) != 0U)
    {
        adc->ISR = ADC_ISR_AWD1;       /* Clear analog watchdog flag */
        adc->IER &= ~ADC_IER_AWD1IE;   /* Mask AWD1 interrupt to prevent storm */
        handle->stats.awd_event_count++;
    }
}

uint32_t ADC_BM_TakeAwdEvents(AdcBm_Handle_t *handle)
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
        adc->ISR = ADC_ISR_AWD1;      /* Clear pending */
        adc->IER |= ADC_IER_AWD1IE;   /* Re-arm interrupt */
    }

    __set_PRIMASK(primask);
    return events;
}

bool ADC_BM_GetReadyHalf(AdcBm_Handle_t *handle, uint8_t *out_half)
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

void ADC_BM_ComputeStats(AdcBm_Handle_t *handle, uint8_t half, uint32_t vref_mv)
{
    ADC_BM_ComputeStatsChannel(handle, half, 0U, vref_mv);
}

void ADC_BM_ComputeStatsChannel(AdcBm_Handle_t *handle, uint8_t half, uint32_t channel_seq_idx, uint32_t vref_mv)
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
        case ADC_BM_RESOLUTION_16B:     max_code = 65535U; break;
        case ADC_BM_RESOLUTION_14B:     max_code = 16383U; break;
        case ADC_BM_RESOLUTION_14B_OPT: max_code = 16383U; break;
        case ADC_BM_RESOLUTION_12B:     max_code = 4095U;  break;
        case ADC_BM_RESOLUTION_12B_OPT: max_code = 4095U;  break;
        case ADC_BM_RESOLUTION_10B:     max_code = 1023U;  break;
        case ADC_BM_RESOLUTION_8B:      max_code = 255U;   break;
        default:                        max_code = 65535U; break;
    }

    /* 65535 * 3300 = 216,265,500 < 2^32-1: fits safely in uint32_t without 64-bit libcalls */
    handle->stats.millivolts = ((uint32_t)handle->stats.average_raw * vref_mv) / max_code;
}

uint32_t ADC_BM_CalculateVDDA(uint16_t vrefint_raw, uint8_t resolution_bits)
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
    uint32_t cal = (uint32_t)ADC_BM_GetVrefintCal();
    /* VREFINT_CAL is measured at VDDA = 3300 mV at 16-bit resolution */
    return (uint32_t)((3300UL * cal) / raw_16b);
}

float ADC_BM_CalculateTemperature(uint16_t temp_raw, uint32_t vdda_mv, uint8_t resolution_bits)
{
    uint32_t raw_16b = (uint32_t)temp_raw;
    if ((resolution_bits > 0U) && (resolution_bits < 16U))
    {
        raw_16b <<= (16U - resolution_bits);
    }
    float cal1 = (float)ADC_BM_GetTsCal1();
    float cal2 = (float)ADC_BM_GetTsCal2();

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
