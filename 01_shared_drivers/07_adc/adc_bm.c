/**
  ******************************************************************************
  * @file    adc_bm.c
  * @brief   Bare-metal ADC driver implementation for STM32H755.
  ******************************************************************************
  */

#include "adc_bm.h"
#include <stddef.h>

#define ADC_BM_VREG_STUP_LOOPS           10000UL
#define ADC_BM_CALIBRATION_TIMEOUT_LOOPS 10000000UL
#define ADC_BM_ENABLE_TIMEOUT_LOOPS      10000000UL
#define ADC_BM_DISABLE_TIMEOUT_LOOPS     10000000UL
#define ADC_BM_STOP_TIMEOUT_LOOPS        10000000UL

#ifndef ADC_CR_BITS_PROPERTY_RS
#define ADC_CR_BITS_PROPERTY_RS (ADC_CR_ADCAL | ADC_CR_JADSTP | ADC_CR_ADSTP | \
                                 ADC_CR_JADSTART | ADC_CR_ADSTART | ADC_CR_ADDIS | ADC_CR_ADEN)
#endif

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

bool ADC_BM_Init(AdcBm_Handle_t *handle, const AdcBm_Config_t *config)
{
    if ((handle == NULL) || (config == NULL) || (config->adc_instance == NULL) ||
        (config->buffer == NULL) || (config->buffer_length < 2U) ||
        ((config->buffer_length & 1U) != 0U) || (config->channel > 19U) ||
        (config->sampling_time > ADC_BM_SAMPLETIME_810CYCLES_5))
    {
        return false;
    }

    if ((config->resolution != ADC_BM_RESOLUTION_16B) &&
        (config->resolution != ADC_BM_RESOLUTION_14B) &&
        (config->resolution != ADC_BM_RESOLUTION_12B) &&
        (config->resolution != ADC_BM_RESOLUTION_10B) &&
        (config->resolution != ADC_BM_RESOLUTION_8B))
    {
        return false;
    }

    handle->config = *config;
    handle->ready_half = 0U;
    handle->has_new_data = 0U;
    handle->stats = (AdcBm_Stats_t){0};

    ADC_TypeDef *adc = config->adc_instance;

    /* 1. Enable peripheral clock and ensure asynchronous kernel clock.
       AHB1ENR/AHB4ENR are per-core banked, not shared state: see the note in
       00_gpio/gpio_bm.c before reading this as a dual-core race.
       ADC12_COMMON->CCR below is a different matter - it is shared by ADC1 and
       ADC2, so this write reaches both. And it is also why Init clears the
       registers by hand instead of pulsing ADC12RST: RCC_AHB1RSTR is at 0x080,
       in the bank both cores really do share, and the reset would take the
       other ADC down with it. */
    if ((adc == ADC1) || (adc == ADC2))
    {
        RCC->AHB1ENR |= RCC_AHB1ENR_ADC12EN;
        volatile uint32_t dummy = RCC->AHB1ENR;
        (void)dummy;

        /* Asynchronous clock ker_ck, prescaler /1 */
        ADC12_COMMON->CCR &= ~(ADC_CCR_CKMODE | ADC_CCR_PRESC);
    }
#if defined(ADC3)
    else if (adc == ADC3)
    {
        RCC->AHB4ENR |= RCC_AHB4ENR_ADC3EN;
        volatile uint32_t dummy = RCC->AHB4ENR;
        (void)dummy;

        /* Asynchronous clock ker_ck, prescaler /1 */
        ADC3_COMMON->CCR &= ~(ADC_CCR_CKMODE | ADC_CCR_PRESC);
    }
#endif
    else
    {
        return false;
    }

    /* Disable ADC before re-configuring (safe on warm reset / re-init) */
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

    /* Startup stabilization delay tADCVREG_STUP (~20 us at 200/400 MHz) */
    for (volatile uint32_t i = 0U; i < ADC_BM_VREG_STUP_LOOPS; i++)
    {
        __NOP();
    }

    /* 4. Configure BOOST[1:0] bits while ADSTART=0, JADSTART=0, ADCAL=0 */
    uint32_t boost_bits;
    if (config->boost_mode == ADC_BM_BOOST_AUTO)
    {
        boost_bits = ADC_BM_GetBoostBits(25000000UL);
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
    cfgr |= (config->resolution & ADC_CFGR_RES_Msk);
    cfgr |= ADC_CFGR_OVRMOD; /* Overwrite old data on overrun */

    if (config->dma_mode != ADC_BM_DMA_DISABLED)
    {
        cfgr |= (config->dma_mode & ADC_CFGR_DMNGT_Msk);
    }
    else if (config->dma_circular)
    {
        /* Legacy fallback: DMNGT = 0b11 (DMA circular mode) */
        cfgr |= (ADC_CFGR_DMNGT_0 | ADC_CFGR_DMNGT_1);
    }

    if (config->trigger_edge != ADC_BM_TRIGGER_DISABLED)
    {
        cfgr |= ((config->trigger_source << ADC_CFGR_EXTSEL_Pos) & ADC_CFGR_EXTSEL_Msk);
        cfgr |= ((config->trigger_edge << ADC_CFGR_EXTEN_Pos) & ADC_CFGR_EXTEN_Msk);
    }
    adc->CFGR = cfgr;

    /* 7. Channel preselection in PCSEL (mandatory for STM32H7) */
    adc->PCSEL |= (1UL << (config->channel & 0x1FUL));

    /* 8. Regular sequencer: length 1, channel on SQ1 */
    adc->SQR1 &= ~(ADC_SQR1_L | ADC_SQR1_SQ1);
    adc->SQR1 |= ((config->channel & 0x1FUL) << ADC_SQR1_SQ1_Pos);

    /* 9. Configure sampling time */
    if (config->channel <= 9U)
    {
        adc->SMPR1 &= ~(0x7UL << (config->channel * 3U));
        adc->SMPR1 |= ((config->sampling_time & 0x7UL) << (config->channel * 3U));
    }
    else
    {
        adc->SMPR2 &= ~(0x7UL << ((config->channel - 10U) * 3U));
        adc->SMPR2 |= ((config->sampling_time & 0x7UL) << ((config->channel - 10U) * 3U));
    }

    /* 10. Single-ended input mode */
    adc->DIFSEL &= ~(1UL << (config->channel & 0x1FUL));

    /* 11. Enable overrun interrupt in IER */
    adc->IER |= ADC_IER_OVRIE;

    return true;
}

bool ADC_BM_Deinit(AdcBm_Handle_t *handle)
{
    if ((handle == NULL) || (handle->config.adc_instance == NULL))
    {
        return false;
    }

    ADC_TypeDef *adc = handle->config.adc_instance;

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
        handle->stats.sample_count += (handle->config.buffer_length / 2U);
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
        handle->stats.sample_count += (handle->config.buffer_length / 2U);
    }

    if ((flags & (DMA_BM_FLAG_TE | DMA_BM_FLAG_DME | DMA_BM_FLAG_FE)) != 0U)
    {
        handle->stats.dma_error_count++;
    }
}

void ADC_BM_IRQHandler_ADC(AdcBm_Handle_t *handle)
{
    if ((handle == NULL) || (handle->config.adc_instance == NULL))
    {
        return;
    }

    ADC_TypeDef *adc = handle->config.adc_instance;
    if ((adc->ISR & ADC_ISR_OVR) != 0U)
    {
        adc->ISR = ADC_ISR_OVR; /* Clear overrun flag (rc_w1: direct assignment) */
        handle->stats.adc_overrun_count++;
    }
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
    if ((handle == NULL) || (handle->config.buffer == NULL))
    {
        return;
    }

    uint32_t half_len = handle->config.buffer_length / 2U;
    if (half_len == 0U)
    {
        return;
    }

    uint32_t start_idx = (half == 0U) ? 0U : half_len;
    uint32_t end_idx = start_idx + half_len;
    const uint16_t *buf = handle->config.buffer;

    uint32_t sum = 0U;
    uint16_t min_val = 0xFFFFU;
    uint16_t max_val = 0U;

    for (uint32_t i = start_idx; i < end_idx; i++)
    {
        uint16_t val = buf[i];
        sum += val;
        if (val < min_val)
        {
            min_val = val;
        }
        if (val > max_val)
        {
            max_val = val;
        }
    }

    handle->stats.latest_raw  = buf[end_idx - 1U];
    handle->stats.average_raw = (uint16_t)(sum / half_len);
    handle->stats.min_raw     = min_val;
    handle->stats.max_raw     = max_val;

    uint32_t max_code;
    switch (handle->config.resolution)
    {
        case ADC_BM_RESOLUTION_16B: max_code = 65535U; break;
        case ADC_BM_RESOLUTION_14B: max_code = 16383U; break;
        case ADC_BM_RESOLUTION_12B: max_code = 4095U;  break;
        case ADC_BM_RESOLUTION_10B: max_code = 1023U;  break;
        case ADC_BM_RESOLUTION_8B:  max_code = 255U;   break;
        default:                    max_code = 65535U; break;
    }

    /* 65535 * 3300 = 216,265,500 < 2^32-1: fits safely in uint32_t without 64-bit libcalls */
    handle->stats.millivolts = ((uint32_t)handle->stats.average_raw * vref_mv) / max_code;
}
