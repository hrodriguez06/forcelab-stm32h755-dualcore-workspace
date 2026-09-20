/**
  ******************************************************************************
  * @file    adc_hal.c
  * @brief   ADC + DMA driver implementation for STM32H755.
  ******************************************************************************
  */

#include "adc_hal.h"

#if defined(HAL_ADC_MODULE_ENABLED)

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

bool ADC_HAL_Init(AdcHal_Handle_t *handle, const AdcHal_Config_t *config)
{
    ADC_ChannelConfTypeDef sConfig = {0};

    if ((handle == NULL) || (config == NULL) || (config->buffer == NULL) || (config->buffer_length < 2U))
    {
        return false;
    }

    handle->config = *config;
    handle->ready_half = 0U;
    handle->has_new_data = 0U;
    handle->stats = (AdcHal_Stats_t){0};

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

    /* 1. Configure ADC peripheral */
    handle->hadc.Instance = config->adc_instance;
    handle->hadc.Init.ClockPrescaler           = ADC_CLOCK_ASYNC_DIV1;
    handle->hadc.Init.Resolution               = config->resolution;
    handle->hadc.Init.ScanConvMode             = ADC_SCAN_DISABLE;
    handle->hadc.Init.EOCSelection             = ADC_EOC_SINGLE_CONV;
    handle->hadc.Init.LowPowerAutoWait         = DISABLE;
    handle->hadc.Init.ContinuousConvMode       = DISABLE; /* Triggered per sample */
    handle->hadc.Init.NbrOfConversion          = 1U;
    handle->hadc.Init.DiscontinuousConvMode    = DISABLE;
    handle->hadc.Init.ExternalTrigConv         = config->trigger_source;
    handle->hadc.Init.ExternalTrigConvEdge     = config->trigger_edge;
    handle->hadc.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;
    handle->hadc.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    handle->hadc.Init.LeftBitShift             = ADC_LEFTBITSHIFT_NONE;
    handle->hadc.Init.OversamplingMode         = DISABLE;

    if (HAL_ADC_Init(&handle->hadc) != HAL_OK)
    {
        return false;
    }

    /* Apply RM0399 / adc.txt:4597 BOOST[1:0] while ADSTART=0 and JADSTART=0.
     * Mask out write-1-to-trigger/clear bits to avoid accidental side-effects. */
    uint32_t cr = handle->hadc.Instance->CR;
    cr &= ~(ADC_CR_ADSTART | ADC_CR_JADSTART | ADC_CR_ADSTP | ADC_CR_JADSTP | ADC_CR_ADCAL | ADC_CR_BOOST);
    cr |= (uint32_t)config->boost_mode;
    handle->hadc.Instance->CR = cr;

    /* Apply Silicon Errata ES0445 2.10.x: disable injected context queue */
    handle->hadc.Instance->CFGR |= ADC_CFGR_JQDIS;

    /* 2. Configure Channel */
    sConfig.Channel      = config->channel;
    sConfig.Rank         = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = config->sampling_time;
    sConfig.SingleDiff   = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset       = 0U;

    if (HAL_ADC_ConfigChannel(&handle->hadc, &sConfig) != HAL_OK)
    {
        return false;
    }

    /* 3. Configure DMA */
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
    if ((handle == NULL) || (handle->config.buffer == NULL))
    {
        return false;
    }

    if (HAL_ADC_Start_DMA(&handle->hadc, (uint32_t *)handle->config.buffer, handle->config.buffer_length) != HAL_OK)
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

    return (HAL_ADC_Stop_DMA(&handle->hadc) == HAL_OK);
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

void ADC_HAL_ComputeStats(AdcHal_Handle_t *handle, uint8_t half, uint32_t vref_mv)
{
    uint32_t half_len;
    uint32_t start_idx;
    uint32_t end_idx;
    uint32_t sum = 0U;
    uint16_t min_val = 0xFFFFU;
    uint16_t max_val = 0U;
    uint16_t *buf;
    uint32_t max_code;

    if ((handle == NULL) || (handle->config.buffer == NULL))
    {
        return;
    }

    half_len = handle->config.buffer_length / 2U;
    if (half_len == 0U)
    {
        return;
    }

    start_idx = (half == 0U) ? 0U : half_len;
    end_idx = start_idx + half_len;
    buf = handle->config.buffer;

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

    switch (handle->config.resolution)
    {
        case ADC_RESOLUTION_16B: max_code = 65535U; break;
        case ADC_RESOLUTION_14B: max_code = 16383U; break;
        case ADC_RESOLUTION_12B: max_code = 4095U;  break;
        case ADC_RESOLUTION_10B: max_code = 1023U;  break;
        case ADC_RESOLUTION_8B:  max_code = 255U;   break;
        default:                 max_code = 65535U; break;
    }

    handle->stats.millivolts = (uint32_t)(((uint64_t)handle->stats.average_raw * (uint64_t)vref_mv) / max_code);
}

/* ==========================================================================
 * HAL Callbacks. Run in ISR context. Keep them lightweight (NO HSEM!).
 * ========================================================================== */

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
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
        handle->stats.sample_count += (handle->config.buffer_length / 2U);
    }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
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
        handle->stats.sample_count += (handle->config.buffer_length / 2U);
    }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    AdcHal_Handle_t *handle = ADC_HAL_GetHandle(hadc->Instance);
    if (handle != NULL)
    {
        handle->stats.dma_error_count++;
    }
}

#endif /* HAL_ADC_MODULE_ENABLED */
