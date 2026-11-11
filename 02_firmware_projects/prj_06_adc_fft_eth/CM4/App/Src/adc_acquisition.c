/**
  ******************************************************************************
  * @file    adc_acquisition.c
  * @brief   ADC1 + DMA1_Stream0 + TIM3 continuous 4096-sample ping-pong
  *          acquisition management on Cortex-M4 (Bare-Metal shared drivers).
  *
  *          Coordinates TIM3 TRGO hardware trigger (default 25.6 kHz, runtime
  *          reconfigurable 1..100 kHz via shared memory command), ADC1 16-bit
  *          sampling on PB1 (ADC1_INP5 Fast Channel, default) or PA3 (INP15),
  *          circular DMA into SRAM2 (8192 samples = 2 x 4096 half-buffers),
  *          and 32-bit integer statistics + publication to D3 SRAM4 (`g_shared`)
  *          under HSEM 4.
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

#define ADC_HALF_BUFFER_SIZE    DSP_BLOCK_SIZE                  /* 4096 samples */
#define ADC_BUFFER_SIZE         (ADC_HALF_BUFFER_SIZE * 2U)     /* 8192 samples */

/* Circular DMA buffer allocated in CM4's own SRAM2 (0x30020000, 16 KB) */
static uint16_t s_adc_dma_buffer[ADC_BUFFER_SIZE] DMA_BM_ALIGN_32;

static AdcBm_Handle_t s_adc_handle;
static uint32_t       s_requested_fs_hz = BOARD_ADC_SAMPLING_RATE_HZ;
static uint32_t       s_actual_fs_hz    = BOARD_ADC_SAMPLING_RATE_HZ;
static bool           s_is_running      = false;

static bool GPIO_Analog_Init(void)
{
    const GpioBm_PinConfig_t cfg = {
        .port             = BOARD_ADC_PORT,
        .pin_idx          = GPIO_BM_PIN_INDEX(BOARD_ADC_PIN),
        .mode             = GPIO_BM_MODE_ANALOG,
        .otype            = GPIO_BM_OTYPE_PP,
        .speed            = GPIO_BM_SPEED_LOW,
        .pull             = GPIO_BM_PULL_NONE,
        .af               = 0U,
        .initial_level    = 0U,
        .allow_debug_pins = false
    };

    return (GPIO_BM_ConfigPin(&cfg) == GPIO_BM_OK);
}

static bool TIM3_Configure_Trigger(uint32_t target_fs_hz, bool start_now)
{
    TimBm_TimeBase_t calc = {0};

    if (target_fs_hz < BOARD_ADC_MIN_FS_HZ)
    {
        target_fs_hz = BOARD_ADC_MIN_FS_HZ;
    }
    else if (target_fs_hz > BOARD_ADC_MAX_FS_HZ)
    {
        target_fs_hz = BOARD_ADC_MAX_FS_HZ;
    }

    if (TIM_BM_EnableClock(TIM3) != TIM_BM_OK)
    {
        return false;
    }

    (void)TIM_BM_Stop(TIM3);

    /* Calculate optimal 16-bit PSC/ARR for TIM3 @ 200 MHz APB1 timer clock */
    if (TIM_BM_CalcTimeBase(BOARD_TIM_APB1_CLK_HZ,
                            target_fs_hz,
                            0xFFFFUL,
                            &calc) != TIM_BM_OK)
    {
        return false;
    }

    const TimBm_TimeBaseConfig_t cfg = {
        .tim               = TIM3,
        .psc               = calc.psc,
        .arr               = calc.arr,
        .count_mode        = TIM_BM_COUNT_UP,
        .repetition        = 0U,
        .arr_preload       = true,
        .one_pulse         = false,
        .urs_overflow_only = false
    };

    if (TIM_BM_InitTimeBase(&cfg) != TIM_BM_OK)
    {
        return false;
    }

    /* Select MMS = 010 (Update event as TRGO) for periodic ADC1 triggering */
    if (TIM_BM_ConfigMaster(TIM3, TIM_BM_MMS_UPDATE, false) != TIM_BM_OK)
    {
        return false;
    }

    s_requested_fs_hz = target_fs_hz;
    s_actual_fs_hz    = (calc.ticks > 0ULL)
        ? (uint32_t)((uint64_t)BOARD_TIM_APB1_CLK_HZ / calc.ticks)
        : target_fs_hz;

    if (start_now)
    {
        if (TIM_BM_Start(TIM3) != TIM_BM_OK)
        {
            return false;
        }
        s_is_running = true;
    }
    else
    {
        s_is_running = false;
    }

    return true;
}

static void ADC_ServiceCommands(void)
{
    uint32_t seq = g_shared.dsp.cmd_seq;
    if (seq == g_shared.dsp.cmd_ack)
    {
        return;
    }

    __DMB();
    uint32_t req_run = g_shared.dsp.cmd_run;
    uint32_t req_fs  = g_shared.dsp.cmd_fs_hz;

    if ((req_fs >= BOARD_ADC_MIN_FS_HZ) &&
        (req_fs <= BOARD_ADC_MAX_FS_HZ) &&
        (req_fs != s_requested_fs_hz))
    {
        (void)TIM3_Configure_Trigger(req_fs, (req_run != 0U));
    }
    else if ((req_run != 0U) && !s_is_running)
    {
        (void)TIM_BM_Start(TIM3);
        s_is_running = true;
    }
    else if ((req_run == 0U) && s_is_running)
    {
        (void)TIM_BM_Stop(TIM3);
        s_is_running = false;
    }

    if (Shared_TryLockCycles(HSEM_ID_ADC_DATA, SHARED_ADC_TRYLOCK_CYCLES))
    {
        g_shared.dsp.fs_hz = s_actual_fs_hz;
        if (s_is_running)
        {
            g_shared.dsp.flags |= SHARED_ADC_FLAG_RUNNING;
        }
        else
        {
            g_shared.dsp.flags &= ~SHARED_ADC_FLAG_RUNNING;
        }
        g_shared.dsp.cmd_ack = seq;
        __DMB();
        SHARED_UNLOCK(HSEM_ID_ADC_DATA);
    }
    else
    {
        g_shared.m4_adc_lock_fail_count++;
    }
}

bool ADC_Acquisition_Init(void)
{
    DmaBm_Config_t dma_cfg = {0};
    AdcBm_Config_t adc_cfg = {0};

    /* 1. Initialize Analog Input GPIO pin (PB1 default or PA3) in bare-metal */
    if (!GPIO_Analog_Init())
    {
        return false;
    }

    /* 2. Configure TIM3 timebase for 25.6 kHz TRGO trigger (do not start yet) */
    if (!TIM3_Configure_Trigger(BOARD_ADC_SAMPLING_RATE_HZ, false))
    {
        return false;
    }

    /* 3. Configure NVIC for DMA1 Stream 0 and ADC */
    NVIC_SetPriority(DMA1_Stream0_IRQn, 5);
    NVIC_EnableIRQ(DMA1_Stream0_IRQn);

    NVIC_SetPriority(ADC_IRQn, 5);
    NVIC_EnableIRQ(ADC_IRQn);

    /* 4. Configure bare-metal DMA1 Stream 0 (8192 half-words = 2 x 4096) */
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

    /* 5. Configure bare-metal ADC1 driver (16-bit, 8.5 cycles, TIM3_TRGO) */
    adc_cfg.adc_instance    = ADC1;
    adc_cfg.channel         = BOARD_ADC_CHANNEL;
    adc_cfg.resolution      = ADC_BM_RESOLUTION_16B;
    adc_cfg.sampling_time   = ADC_BM_SAMPLETIME_8CYCLES_5;
    adc_cfg.trigger_source  = ADC_BM_TRIG_TIM3_TRGO;
    adc_cfg.trigger_edge    = ADC_BM_TRIGGER_RISING;
    adc_cfg.boost_mode      = ADC_BM_BOOST_AUTO;
    adc_cfg.dma_mode        = ADC_BM_DMA_CIRCULAR;
    adc_cfg.dma_circular    = true;
    adc_cfg.buffer          = s_adc_dma_buffer;
    adc_cfg.buffer_length   = ADC_BUFFER_SIZE;
    adc_cfg.kernel_clock_hz = BOARD_ADC_KER_CLK_HZ;

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
    s_is_running = true;

    /* 10. Publish initial metadata in shared memory */
    if (Shared_TryLockCycles(HSEM_ID_ADC_DATA, SHARED_ADC_TRYLOCK_CYCLES))
    {
        g_shared.dsp.dsp_magic  = DSP_SHARED_MAGIC;
        g_shared.dsp.channel    = adc_cfg.channel;
        g_shared.dsp.fs_hz      = s_actual_fs_hz;
        g_shared.dsp.cmd_fs_hz  = s_requested_fs_hz;
        g_shared.dsp.cmd_run    = 1U;
        g_shared.dsp.block_size = ADC_HALF_BUFFER_SIZE;
        g_shared.dsp.flags      = SHARED_ADC_FLAG_CALIBRATED | SHARED_ADC_FLAG_RUNNING;
        __DMB();
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
    uint8_t ready_half = 0U;

    /* 1. Service any pending runtime commands from Cortex-M7 */
    ADC_ServiceCommands();

    /* 2. Check if a 4096-sample DMA half-buffer has completed */
    if (!ADC_BM_GetReadyHalf(&s_adc_handle, &ready_half))
    {
        return;
    }

    const uint16_t *src = (ready_half == 0U)
        ? &s_adc_dma_buffer[0]
        : &s_adc_dma_buffer[ADC_HALF_BUFFER_SIZE];

    /* 3. Compute 32-bit integer statistics on the completed 4096-sample half */
    ADC_BM_ComputeStats(&s_adc_handle, ready_half, BOARD_ADC_VREF_MV);

    uint32_t min_raw = (uint32_t)s_adc_handle.stats.min_raw;
    uint32_t max_raw = (uint32_t)s_adc_handle.stats.max_raw;
    uint32_t min_mv  = (min_raw * BOARD_ADC_VREF_MV) / 65535UL;
    uint32_t max_mv  = (max_raw * BOARD_ADC_VREF_MV) / 65535UL;
    uint32_t vpp_mv  = (max_mv >= min_mv) ? (max_mv - min_mv) : 0UL;

    /*
     * 4. Copy the 4096 samples from SRAM2 into the non-cacheable ping-pong
     *    slot in D3 SRAM4 (`g_shared.dsp.raw_buffer[ready_half]`) BEFORE taking
     *    the short metadata HSEM lock so CM7 never spins during an 8 KB copy.
     */
    memcpy((void *)g_shared.dsp.raw_buffer[ready_half],
           src,
           sizeof(uint16_t) * ADC_HALF_BUFFER_SIZE);
    __DMB();

    uint32_t flags = SHARED_ADC_FLAG_CALIBRATED | SHARED_ADC_FLAG_DATA_VALID;
    if (s_is_running)
    {
        flags |= SHARED_ADC_FLAG_RUNNING;
    }
    if ((s_adc_handle.stats.half_overrun_count > 0U) ||
        (s_adc_handle.stats.adc_overrun_count > 0U))
    {
        flags |= SHARED_ADC_FLAG_OVERRUN;
    }
    if (s_adc_handle.stats.dma_error_count > 0U)
    {
        flags |= SHARED_ADC_FLAG_DMA_ERROR;
    }

    /* 5. Publish block sequence and telemetry under HSEM 4 */
    if (Shared_TryLockCycles(HSEM_ID_ADC_DATA, SHARED_ADC_TRYLOCK_CYCLES))
    {
        uint32_t t_hold_start = DWT->CYCCNT;

        g_shared.dsp.ready_idx         = (uint32_t)ready_half;
        g_shared.dsp.active_idx        = (uint32_t)(ready_half ^ 1U);
        g_shared.dsp.block_id          = g_shared.dsp.block_id + 1U;
        g_shared.dsp.t_block_ms        = HAL_GetTick();
        g_shared.dsp.fs_hz             = s_actual_fs_hz;
        g_shared.dsp.channel           = s_adc_handle.config.channel;

        g_shared.dsp.latest_raw        = s_adc_handle.stats.latest_raw;
        g_shared.dsp.average_raw       = s_adc_handle.stats.average_raw;
        g_shared.dsp.min_raw           = s_adc_handle.stats.min_raw;
        g_shared.dsp.max_raw           = s_adc_handle.stats.max_raw;
        g_shared.dsp.millivolts        = s_adc_handle.stats.millivolts;
        g_shared.dsp.vpp_mv            = vpp_mv;
        g_shared.dsp.sample_count      = s_adc_handle.stats.sample_count;
        g_shared.dsp.dma_tc_count      = s_adc_handle.stats.dma_tc_count;
        g_shared.dsp.dma_ht_count      = s_adc_handle.stats.dma_ht_count;
        g_shared.dsp.dma_error_count   = s_adc_handle.stats.dma_error_count;
        g_shared.dsp.overrun_count     = s_adc_handle.stats.half_overrun_count;
        g_shared.dsp.adc_overrun_count = s_adc_handle.stats.adc_overrun_count;
        g_shared.dsp.flags             = flags;
        __DMB();

        SHARED_UNLOCK(HSEM_ID_ADC_DATA);

        uint32_t hold_cyc = DWT->CYCCNT - t_hold_start;
        if (hold_cyc > g_shared.m4_adc_lock_hold_cycles_max)
        {
            g_shared.m4_adc_lock_hold_cycles_max = hold_cyc;
        }
    }
    else
    {
        g_shared.m4_adc_lock_fail_count++;
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
