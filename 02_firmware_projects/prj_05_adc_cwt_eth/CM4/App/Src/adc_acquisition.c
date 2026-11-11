/**
  ******************************************************************************
  * @file    adc_acquisition.c
  * @brief   ADC1 + DMA1 continuous acquisition management on Cortex-M4 (Bare-Metal).
  *
  *          Coordinates TIM3 500 kHz TRGO hardware trigger, ADC1 16-bit
  *          sampling on PB1 (Arduino A3, ADC1_INP5 fast channel) biased at a
  *          nominal 1.5 V DC offset, circular DMA into SRAM2 (1024 samples,
  *          512 per half-buffer), and 100% 32-bit integer statistics &
  *          publication to shared memory (SRAM4) under HSEM 4.
  ******************************************************************************
  */

#include "adc_acquisition.h"
#include "adc_bm.h"
#include "dma_bm.h"
#include "gpio_bm.h"
#include "tim_bm.h"
#include "board_def.h"
#include "shared_memory.h"
#include "stm32h7xx_hal.h"
#include <string.h>

#define ADC_BUFFER_SIZE         1024U   /* 2 x 512 samples (512 per half-buffer) */

_Static_assert((ADC_BUFFER_SIZE / 2U) == SHARED_ADC_WAVEFORM_SAMPLES,
               "half of the DMA buffer must hold exactly one 512-sample CWT window");

/* Circular DMA buffer allocated in CM4's own SRAM2 (0x30020000) */
static uint16_t s_adc_dma_buffer[ADC_BUFFER_SIZE] DMA_BM_ALIGN_32;

static AdcBm_Handle_t s_adc_handle;

/* Retention measurement variables */
volatile uint32_t g_adc_lock_hold_cycles_max  = 0U;
volatile uint32_t g_adc_lock_hold_cycles_min  = 0xFFFFFFFFUL;
volatile uint32_t g_adc_lock_hold_cycles_last = 0U;

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
    /* Enable TIM3 peripheral clock on CM4 RCC bank */
    if (TIM_BM_EnableClock(TIM3) != TIM_BM_OK)
    {
        return false;
    }

    /* APB1 timer clock = 200 MHz
     * Prescaler = 19  -> Counter clock = 10 MHz (100 ns per tick)
     * Period = 999    -> Update frequency = 10 MHz / 1000 = 10,000 Hz (100.0 us period)
     * 512-sample half-buffer window = 51.2 ms (8.53 spindle revs @ 10,000 RPM)
     */
    const TimBm_TimeBaseConfig_t cfg = {
        .tim               = TIM3,
        .psc               = 19U,
        .arr               = 999U,
        .count_mode        = TIM_BM_COUNT_UP,
        .repetition        = 0U,
        .arr_preload       = false,
        .one_pulse         = false,
        .urs_overflow_only = false
    };

    if (TIM_BM_InitTimeBase(&cfg) != TIM_BM_OK)
    {
        return false;
    }

    /* Select MMS = 010 (Update event as TRGO) for periodic ADC triggering */
    if (TIM_BM_ConfigMaster(TIM3, TIM_BM_MMS_UPDATE, false) != TIM_BM_OK)
    {
        return false;
    }

    return true;
}

bool ADC_Acquisition_Init(void)
{
    DmaBm_Config_t dma_cfg = {0};
    AdcBm_Config_t adc_cfg = {0};

    /* 1. Initialize Analog Input GPIO pin (PB1 = A3) in bare-metal */
    if (!GPIO_Analog_Init())
    {
        return false;
    }

    /* 2. Configure TIM3 timebase for 500 kHz TRGO trigger (do not start yet) */
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

    /* 5. Configure bare-metal ADC1 driver (PB1 -> Channel 5, 16-bit, TIM3_TRGO) */
    adc_cfg.adc_instance    = ADC1;
    adc_cfg.channel         = BOARD_ADC_CHANNEL; /* 5U: PB1 (ADC1_INP5 Fast Channel) */
    adc_cfg.resolution      = ADC_BM_RESOLUTION_16B;
    adc_cfg.sampling_time   = ADC_BM_SAMPLETIME_8CYCLES_5;
    adc_cfg.trigger_source  = ADC_BM_TRIG_TIM3_TRGO;
    adc_cfg.trigger_edge    = ADC_BM_TRIGGER_RISING;
    adc_cfg.boost_mode      = ADC_BM_BOOST_AUTO;
    adc_cfg.dma_mode        = ADC_BM_DMA_CIRCULAR;
    adc_cfg.dma_circular    = true;
    adc_cfg.buffer          = s_adc_dma_buffer;
    adc_cfg.buffer_length   = ADC_BUFFER_SIZE;
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

    /* 8. Start DMA stream and arm ADC regular conversion */
    DMA_BM_Start(DMA1_Stream0);
    ADC_BM_StartConversion(ADC1);

    /* 9. Start hardware trigger timer (TIM3 counter enable) LAST */
    if (TIM_BM_Start(TIM3) != TIM_BM_OK)
    {
        return false;
    }

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
        g_shared.m4_adc_lock_fail_count++;
    }

    return true;
}

void ADC_Acquisition_Process(void)
{
    uint8_t ready_half;

    if (ADC_BM_GetReadyHalf(&s_adc_handle, &ready_half))
    {
        /* Determine source pointer for the completed 512-sample half */
        const uint16_t *src = (ready_half == 0U) ?
                              &s_adc_dma_buffer[0] :
                              &s_adc_dma_buffer[ADC_BUFFER_SIZE / 2U];

        /* Compute 32-bit integer statistics on the completed half */
        ADC_BM_ComputeStats(&s_adc_handle, ready_half, BOARD_ADC_VREF_MV);

        /* Compute 32-bit integer AC/DC metrics relative to 1500 mV bias
         * (Zero float/64-bit operations: min_raw * 3300 <= 216,265,500 fits in uint32_t) */
        uint32_t min_raw  = (uint32_t)s_adc_handle.stats.min_raw;
        uint32_t max_raw  = (uint32_t)s_adc_handle.stats.max_raw;
        uint32_t min_mv   = (min_raw * BOARD_ADC_VREF_MV) / 65535UL;
        uint32_t max_mv   = (max_raw * BOARD_ADC_VREF_MV) / 65535UL;
        uint32_t vpp_mv   = (max_mv >= min_mv) ? (max_mv - min_mv) : 0UL;
        int32_t  ac_min   = (int32_t)min_mv - (int32_t)ADC_NOMINAL_OFFSET_MV;
        int32_t  ac_max   = (int32_t)max_mv - (int32_t)ADC_NOMINAL_OFFSET_MV;
        int32_t  dc_err   = (int32_t)s_adc_handle.stats.millivolts - (int32_t)ADC_NOMINAL_OFFSET_MV;

        /* Publish telemetry and 512-sample waveform snapshot to shared memory under HSEM 4 */
        if (Shared_TryLockCycles(HSEM_ID_ADC_DATA, SHARED_ADC_TRYLOCK_CYCLES))
        {
            uint32_t t_hold_start = DWT->CYCCNT;

            g_shared.adc.latest_raw         = s_adc_handle.stats.latest_raw;
            g_shared.adc.average_raw        = s_adc_handle.stats.average_raw;
            g_shared.adc.min_raw            = s_adc_handle.stats.min_raw;
            g_shared.adc.max_raw            = s_adc_handle.stats.max_raw;
            g_shared.adc.millivolts         = s_adc_handle.stats.millivolts;
            g_shared.adc.vpp_mv             = vpp_mv;
            g_shared.adc.ac_min_mv          = ac_min;
            g_shared.adc.ac_max_mv          = ac_max;
            g_shared.adc.dc_offset_error_mv = dc_err;
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

            uint32_t hold_cyc = DWT->CYCCNT - t_hold_start;
            g_adc_lock_hold_cycles_last = hold_cyc;
            if (hold_cyc > g_adc_lock_hold_cycles_max) { g_adc_lock_hold_cycles_max = hold_cyc; }
            if (hold_cyc < g_adc_lock_hold_cycles_min) { g_adc_lock_hold_cycles_min = hold_cyc; }
            if (hold_cyc > g_shared.m4_adc_lock_hold_cycles_max) { g_shared.m4_adc_lock_hold_cycles_max = hold_cyc; }
        }
        else
        {
            g_shared.m4_adc_lock_fail_count++;
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
