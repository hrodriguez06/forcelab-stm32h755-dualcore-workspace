/**
  ******************************************************************************
  * @file    adc_acquisition.c
  * @brief   ADC1 + DMA1 continuous acquisition management on Cortex-M4 (Bare-Metal).
  *
  *          Coordinates TIM3 500 kHz TRGO hardware trigger, ADC1 16-bit
  *          sampling on the pin board_def.h selects (PB1 by default, a fast
  *          channel), circular DMA into SRAM2 (1024 samples, 512 per half),
  *          and statistics calculation & publication to shared memory
  *          (SRAM4) under HSEM 4 using pure bare-metal register drivers.
  ******************************************************************************
  */

#include "adc_acquisition.h"
#include "adc_bm.h"
#include "dma_bm.h"
#include "gpio_bm.h"
#include "board_def.h"
#include "shared_memory.h"
#include "stm32h7xx_hal.h"
#include <string.h>

#define ADC_BUFFER_SIZE         1024U   /* 2 x 512 samples (512 per half-buffer) */

/* ADC_Acquisition_Process() copies SHARED_ADC_WAVEFORM_SAMPLES out of ONE half
   of the buffer below, but that constant lives in shared_memory.h - a header
   shared with CPU1 - while ADC_BUFFER_SIZE is local to this file. Assert the
   contract instead of trusting that both numbers get edited. */
_Static_assert((ADC_BUFFER_SIZE / 2U) == SHARED_ADC_WAVEFORM_SAMPLES,
               "half of the DMA buffer must hold exactly one waveform snapshot");

/* Circular DMA buffer allocated in CM4's own SRAM2 (0x30020000) */
static uint16_t s_adc_dma_buffer[ADC_BUFFER_SIZE] DMA_BM_ALIGN_32;

static AdcBm_Handle_t s_adc_handle;

static void GPIO_Analog_Init(void)
{
    /* Enable GPIO port clock via bare-metal helper */
    GPIO_BM_EnablePortClock(BOARD_ADC_PORT);

    /* Determine pin number 0..15 from pin bitmask */
    uint32_t pin_num = 0U;
    uint32_t pin_mask = (uint32_t)BOARD_ADC_PIN;
    while ((pin_mask > 1U) && (pin_num < 15U))
    {
        pin_mask >>= 1U;
        pin_num++;
    }

    /* Set MODER[1:0] = 0b11 (Analog Mode) */
    BOARD_ADC_PORT->MODER |= (3UL << (pin_num * 2U));

    /* Set PUPDR[1:0] = 0b00 (No pull-up, no pull-down) */
    BOARD_ADC_PORT->PUPDR &= ~(3UL << (pin_num * 2U));
}

static bool TIM3_Trigger_Init(void)
{
    /* Enable TIM3 clock on APB1 */
    RCC->APB1LENR |= RCC_APB1LENR_TIM3EN;
    volatile uint32_t dummy = RCC->APB1LENR;
    (void)dummy;

    /* APB1 timer clock = 200 MHz
     * Prescaler = 0   -> Counter clock = 200 MHz (5 ns per tick)
     * Period = 399    -> Update frequency = 200 MHz / 400 = 500,000 Hz (2.0 us period)
     * Exactly 50 samples per period for a 10 kHz signal.
     */
    TIM3->CR1 = 0U;
    TIM3->PSC = 0U;
    TIM3->ARR = 399U;

    /* Set MMS (Master Mode Selection) to 010 (Update event sent as TRGO) */
    TIM3->CR2 &= ~TIM_CR2_MMS_Msk;
    TIM3->CR2 |= (0x2U << TIM_CR2_MMS_Pos);

    return true;
}

bool ADC_Acquisition_Init(void)
{
    DmaBm_Config_t dma_cfg = {0};
    AdcBm_Config_t adc_cfg = {0};

    /* 1. Initialize Analog Input GPIO pin in bare-metal */
    GPIO_Analog_Init();

    /* 2. Configure TIM3 timebase for 500 kHz TRGO trigger */
    if (!TIM3_Trigger_Init())
    {
        return false;
    }

    /* 3. Configure NVIC for DMA1 Stream 0 and ADC */
    NVIC_SetPriority(DMA1_Stream0_IRQn, 5);
    NVIC_EnableIRQ(DMA1_Stream0_IRQn);

    NVIC_SetPriority(ADC_IRQn, 5);
    NVIC_EnableIRQ(ADC_IRQn);

    /* 4. Configure bare-metal DMA1 Stream 0 */
    dma_cfg.stream           = BOARD_ADC_DMA_STREAM;
    dma_cfg.request_id       = BOARD_ADC_DMA_REQUEST;
    dma_cfg.direction        = DMA_BM_DIR_PERIPH_TO_MEM;
    dma_cfg.periph_data_size = DMA_BM_DATA_SIZE_HALFWORD;
    dma_cfg.mem_data_size    = DMA_BM_DATA_SIZE_HALFWORD;
    dma_cfg.periph_inc       = false;
    dma_cfg.mem_inc          = true;
    dma_cfg.circular         = true;
    dma_cfg.priority         = DMA_BM_PRIORITY_HIGH;
    dma_cfg.enable_it_ht     = true;
    dma_cfg.enable_it_tc     = true;
    dma_cfg.enable_it_te     = true;
    dma_cfg.enable_it_dme    = true;
    dma_cfg.periph_addr      = (uint32_t)&(ADC1->DR);
    dma_cfg.mem_addr         = (uint32_t)s_adc_dma_buffer;
    dma_cfg.buffer_length    = ADC_BUFFER_SIZE;

    if (!DMA_BM_InitStream(&dma_cfg))
    {
        return false;
    }

    /* 5. Configure bare-metal ADC1 driver */
    adc_cfg.adc_instance   = ADC1;
#if (BOARD_ADC_USE_FAST_CHANNEL_PB1 == 1U)
    adc_cfg.channel        = 5U;  /* PB1: ADC12_INP5 (Fast Channel) */
#else
    adc_cfg.channel        = 15U; /* PA3: ADC12_INP15 (Slow Channel) */
#endif
    adc_cfg.resolution     = ADC_BM_RESOLUTION_16B;
    adc_cfg.sampling_time  = ADC_BM_SAMPLETIME_8CYCLES_5;
    adc_cfg.trigger_source = ADC_BM_TRIG_TIM3_TRGO;
    adc_cfg.trigger_edge   = ADC_BM_TRIGGER_RISING;
    adc_cfg.boost_mode     = ADC_BM_BOOST_AUTO;
    adc_cfg.dma_mode       = ADC_BM_DMA_CIRCULAR;
    adc_cfg.dma_circular   = true;
    adc_cfg.buffer         = s_adc_dma_buffer;
    adc_cfg.buffer_length  = ADC_BUFFER_SIZE;
    adc_cfg.kernel_clock_hz = 25000000UL;

    if (!ADC_BM_Init(&s_adc_handle, &adc_cfg))
    {
        return false;
    }

    /* 6. Calibrate offset and linearity before starting conversions */
    if (!ADC_BM_Calibrate(ADC1))
    {
        return false;
    }

    /* 7. Enable ADC core */
    if (!ADC_BM_Enable(ADC1))
    {
        return false;
    }

    /* 8. Start DMA stream and ADC regular conversion */
    DMA_BM_Start(DMA1_Stream0);
    ADC_BM_StartConversion(ADC1);

    /* 9. Start hardware trigger timer (TIM3 counter enable) */
    TIM3->CR1 |= TIM_CR1_CEN;

    /* 10. Initial metadata in shared memory */
    if (Shared_TryLockCycles(HSEM_ID_ADC_DATA, SHARED_ADC_TRYLOCK_CYCLES))
    {
        g_shared.adc.channel          = adc_cfg.channel;
        g_shared.adc.sampling_rate_hz = BOARD_ADC_SAMPLING_RATE_HZ;
        g_shared.adc.waveform_seq     = 0U;
        g_shared.adc.waveform_len     = SHARED_ADC_WAVEFORM_SAMPLES;
        SHARED_UNLOCK(HSEM_ID_ADC_DATA);
    }
    else
    {
        g_shared.adc_lock_fail_count++;
    }

    return true;
}

void ADC_Acquisition_Process(void)
{
    uint8_t ready_half;

    if (ADC_BM_GetReadyHalf(&s_adc_handle, &ready_half))
    {
        /* Determine source pointer for the completed half */
        const uint16_t *src = (ready_half == 0U) ?
                              &s_adc_dma_buffer[0] :
                              &s_adc_dma_buffer[ADC_BUFFER_SIZE / 2U];

        /* Compute integer statistics on the completed half */
        ADC_BM_ComputeStats(&s_adc_handle, ready_half, BOARD_ADC_VREF_MV);

        /* Publish telemetry and waveform snapshot to shared memory.
         * NEVER lock HSEM in ISR: this function runs strictly in super-loop context.
         */
        if (Shared_TryLockCycles(HSEM_ID_ADC_DATA, SHARED_ADC_TRYLOCK_CYCLES))
        {
            g_shared.adc.latest_raw         = s_adc_handle.stats.latest_raw;
            g_shared.adc.average_raw        = s_adc_handle.stats.average_raw;
            g_shared.adc.min_raw            = s_adc_handle.stats.min_raw;
            g_shared.adc.max_raw            = s_adc_handle.stats.max_raw;
            g_shared.adc.millivolts         = s_adc_handle.stats.millivolts;
            g_shared.adc.sample_count       = s_adc_handle.stats.sample_count;
            g_shared.adc.dma_tc_count       = s_adc_handle.stats.dma_tc_count;
            g_shared.adc.dma_ht_count       = s_adc_handle.stats.dma_ht_count;
            g_shared.adc.dma_error_count    = s_adc_handle.stats.dma_error_count;
            g_shared.adc.half_overrun_count = s_adc_handle.stats.half_overrun_count;
            g_shared.adc.adc_overrun_count  = s_adc_handle.stats.adc_overrun_count;
            g_shared.adc.channel            = s_adc_handle.config.channel;
            g_shared.adc.sampling_rate_hz   = BOARD_ADC_SAMPLING_RATE_HZ;
            g_shared.adc.timestamp_ms       = HAL_GetTick();
            g_shared.adc.waveform_seq++;
            g_shared.adc.waveform_len       = SHARED_ADC_WAVEFORM_SAMPLES;
            memcpy((void *)g_shared.adc.waveform, src, sizeof(uint16_t) * SHARED_ADC_WAVEFORM_SAMPLES);
            SHARED_UNLOCK(HSEM_ID_ADC_DATA);
        }
        else
        {
            g_shared.adc_lock_fail_count++;
        }
    }
}

void ADC_Acquisition_DMA_IRQHandler(void)
{
    ADC_BM_IRQHandler_DMA(&s_adc_handle, DMA1_Stream0);
}

void ADC_Acquisition_ADC_IRQHandler(void)
{
    ADC_BM_IRQHandler_ADC(&s_adc_handle);
}
