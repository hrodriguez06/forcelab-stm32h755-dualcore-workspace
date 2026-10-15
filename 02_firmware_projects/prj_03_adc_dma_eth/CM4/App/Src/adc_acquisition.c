/**
  ******************************************************************************
  * @file    adc_acquisition.c
  * @brief   ADC1 + DMA1 continuous acquisition management on Cortex-M4.
  *
  *          Coordinates TIM3 500 kHz TRGO hardware trigger, ADC1 16-bit
  *          sampling on the pin board_def.h selects (PB1 by default, a fast
  *          channel), circular DMA into SRAM2 (1024 samples, 512 per half),
  *          and statistics calculation & publication to shared memory
  *          (SRAM4) under HSEM 4.
  ******************************************************************************
  */

#include "adc_acquisition.h"
#include "adc_hal.h"
#include "tim_hal.h"
#include "board_def.h"
#include "shared_memory.h"
#include "stm32h7xx_hal.h"
#include <string.h>

#define ADC_BUFFER_SIZE         1024U   /* 2 x 512 samples (512 per half-buffer) */

/* ADC_Acquisition_Process() copies SHARED_ADC_WAVEFORM_SAMPLES out of ONE half
   of the buffer below, but that constant lives in shared_memory.h - a header
   shared with CPU1 - while ADC_BUFFER_SIZE is local to this file. Nothing in
   the type system ties them together, so shrinking the DMA buffer would turn
   that memcpy into a read past the end of the half with no diagnostic at all.
   Assert the contract instead of trusting that both numbers get edited. */
_Static_assert((ADC_BUFFER_SIZE / 2U) == SHARED_ADC_WAVEFORM_SAMPLES,
               "half of the DMA buffer must hold exactly one waveform snapshot");

/* Circular DMA buffer allocated in CM4's own SRAM2 (0x30020000) */
static uint16_t s_adc_dma_buffer[ADC_BUFFER_SIZE] __attribute__((aligned(32)));

static AdcHal_Handle_t s_adc_handle;
static TIM_HandleTypeDef s_htim3;

static bool GPIO_Analog_Init(void)
{
    const GpioBm_PinConfig_t cfg = {
        .port          = BOARD_ADC_PORT,
        .pin_idx       = GPIO_BM_PIN_INDEX(BOARD_ADC_PIN),
        .mode          = GPIO_BM_MODE_ANALOG,
        .otype         = GPIO_BM_OTYPE_PP,
        .speed         = GPIO_BM_SPEED_LOW,
        .pull          = GPIO_BM_PULL_NONE,
        .af            = 0U,
        .initial_level = 0U,
    };

    return (GPIO_BM_ConfigPin(&cfg) == GPIO_BM_OK);
}

static bool TIM3_Trigger_Init(void)
{
    /* Habilitar reloj en el banco RCC del propio núcleo CM4 */
    if (TIM_BM_EnableClock(TIM3) != TIM_BM_OK)
    {
        return false;
    }

    s_htim3.Instance = TIM3;

    const TimBm_TimeBaseConfig_t cfg = {
        .tim               = TIM3,
        .psc               = 0U,
        .arr               = 399U,
        .count_mode        = TIM_BM_COUNT_UP,
        .repetition        = 0U,
        .arr_preload       = false,  /* ARPE = 0: actualización inmediata */
        .one_pulse         = false,
        .urs_overflow_only = false
    };

    /* Inicializar base de tiempos (genera UG bajo MMS=000, sin TRGO espurio) */
    if (TIM_HAL_InitTimeBase(&s_htim3, &cfg) != TIM_BM_OK)
    {
        return false;
    }

    if (TIM_HAL_ConfigMaster(&s_htim3, TIM_BM_MMS_UPDATE, false) != TIM_BM_OK)
    {
        return false;
    }

    return true;
}

bool ADC_Acquisition_Init(void)
{
    AdcHal_Config_t adc_cfg = {0};

    /* Enable peripheral clocks for DMA1 and ADC1/2 */
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();

    /* Initialize Analog Input GPIO */
    if (!GPIO_Analog_Init())
    {
        return false;
    }

    /* Configure TIM3 timebase for 500 kHz TRGO */
    if (!TIM3_Trigger_Init())
    {
        return false;
    }

    /* Configure NVIC for DMA1 Stream 0 and ADC */
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);

    HAL_NVIC_SetPriority(ADC_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(ADC_IRQn);

    /* Configure ADC + DMA driver */
    adc_cfg.adc_instance   = ADC1;
    adc_cfg.channel        = BOARD_ADC_CHANNEL;
    adc_cfg.resolution     = ADC_RESOLUTION_16B;
    adc_cfg.sampling_time  = ADC_SAMPLETIME_8CYCLES_5;
    adc_cfg.trigger_source = ADC_EXTERNALTRIG_T3_TRGO;
    adc_cfg.trigger_edge   = ADC_EXTERNALTRIGCONVEDGE_RISING;
    adc_cfg.boost_mode     = ADC_HAL_BOOST_LE_25MHZ;
    adc_cfg.dma_stream     = DMA1_Stream0;
    adc_cfg.dma_request    = DMA_REQUEST_ADC1;
    adc_cfg.buffer         = s_adc_dma_buffer;
    adc_cfg.buffer_length  = ADC_BUFFER_SIZE;

    if (!ADC_HAL_Init(&s_adc_handle, &adc_cfg))
    {
        return false;
    }

    /* Calibrate offset and linearity before starting conversions */
    if (!ADC_HAL_Calibrate(&s_adc_handle))
    {
        return false;
    }

    /* Start circular DMA transfer */
    if (!ADC_HAL_Start_DMA(&s_adc_handle))
    {
        return false;
    }

    /* Start hardware trigger timer */
    if (TIM_BM_Start(TIM3) != TIM_BM_OK)
    {
        return false;
    }

    /* Initial metadata in shared memory */
    SHARED_LOCK(HSEM_ID_ADC_DATA);
    g_shared.adc.channel          = BOARD_ADC_CHANNEL;
    g_shared.adc.sampling_rate_hz = BOARD_ADC_SAMPLING_RATE_HZ;
    g_shared.adc.waveform_seq     = 0U;
    g_shared.adc.waveform_len     = SHARED_ADC_WAVEFORM_SAMPLES;
    SHARED_UNLOCK(HSEM_ID_ADC_DATA);

    return true;
}

void ADC_Acquisition_Process(void)
{
    uint8_t ready_half;

    if (ADC_HAL_GetReadyHalf(&s_adc_handle, &ready_half))
    {
        /* Determine source pointer for the completed half */
        const uint16_t *src = (ready_half == 0U) ?
                              &s_adc_dma_buffer[0] :
                              &s_adc_dma_buffer[ADC_BUFFER_SIZE / 2U];

        /* Compute integer statistics on the completed half */
        ADC_HAL_ComputeStats(&s_adc_handle, ready_half, BOARD_ADC_VREF_MV);

        /* Publish telemetry and waveform snapshot to shared memory.
         * NEVER lock HSEM in ISR: this function runs strictly in super-loop context.
         */
        SHARED_LOCK(HSEM_ID_ADC_DATA);
        g_shared.adc.latest_raw         = s_adc_handle.stats.latest_raw;
        g_shared.adc.average_raw        = s_adc_handle.stats.average_raw;
        g_shared.adc.min_raw            = s_adc_handle.stats.min_raw;
        g_shared.adc.max_raw            = s_adc_handle.stats.max_raw;
        g_shared.adc.millivolts         = s_adc_handle.stats.millivolts;
        g_shared.adc.sample_count       = s_adc_handle.stats.sample_count;
        g_shared.adc.dma_tc_count       = s_adc_handle.stats.dma_tc_count;
        g_shared.adc.dma_ht_count       = s_adc_handle.stats.dma_ht_count;
        g_shared.adc.dma_error_count     = s_adc_handle.stats.dma_error_count;
        g_shared.adc.half_overrun_count = s_adc_handle.stats.half_overrun_count;
        g_shared.adc.channel            = BOARD_ADC_CHANNEL;
        g_shared.adc.sampling_rate_hz   = BOARD_ADC_SAMPLING_RATE_HZ;
        g_shared.adc.timestamp_ms       = HAL_GetTick();
        g_shared.adc.waveform_seq++;
        g_shared.adc.waveform_len       = SHARED_ADC_WAVEFORM_SAMPLES;
        memcpy((void *)g_shared.adc.waveform, src, sizeof(uint16_t) * SHARED_ADC_WAVEFORM_SAMPLES);
        SHARED_UNLOCK(HSEM_ID_ADC_DATA);
    }
}

void ADC_Acquisition_DMA_IRQHandler(void)
{
    ADC_HAL_IRQHandler_DMA(&s_adc_handle);
}

void ADC_Acquisition_ADC_IRQHandler(void)
{
    ADC_HAL_IRQHandler_ADC(&s_adc_handle);
}
