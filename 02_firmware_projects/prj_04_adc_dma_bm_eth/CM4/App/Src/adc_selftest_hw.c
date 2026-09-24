/**
  ******************************************************************************
  * @file    adc_selftest_hw.c
  * @brief   Hardware tests T2, T3, T3b, T4 and T6 of the shared ADC driver,
  *          BM and LL tiers. See adc_selftest_hw.h for what each one proves.
  ******************************************************************************
  */

#include "adc_selftest_hw.h"
#include "main.h"
#include "adc_bm.h"
#include "adc_ll.h"
#include "dma_bm.h"
#include <string.h>

/* adc_ker_ck is PLL2P = 25 MHz, programmed by CPU1 in CM7/Core/Src/main.c. */
#define STHW_KER_CK_HZ          25000000UL
/* APB1 is divided by 2, so the APB1 timers run at twice PCLK1 (RM0399 8.5.9). */
#define STHW_TIM_CLK_HZ         (2UL * BOARD_APB_HZ)

#if (BOARD_ADC_USE_FAST_CHANNEL_PB1 == 1U)
#define STHW_ADC1_CHANNEL       5U   /* PB1: ADC12_INP5, fast channel  */
#else
#define STHW_ADC1_CHANNEL       15U  /* PA3: ADC12_INP15, slow channel */
#endif
#define STHW_ADC1_PIN_IDX       ((uint32_t)__builtin_ctz((uint32_t)BOARD_ADC_PIN))

#define STHW_READS_PER_POINT    16U
#define STHW_SPIN               100000UL

#define STHW_T3_BUF_LEN         1024U
#define STHW_T3_WINDOW_MS       1000U

/* Two ranks of 810.5 cycles at 12.5 MHz take 131 us, inside the 200 us period. */
#define STHW_T3B_RATE_HZ        5000UL
#define STHW_T3B_BUF_LEN        64U
#define STHW_T3B_WINDOW_MS      100U

#define STHW_T4_WINDOW_MS       25U
#define STHW_T4_DMA_STOP_MS     20U

#define STHW_DONE_MAGIC         0xADC0FEEDUL

volatile AdcSelfTestHw_Results_t g_adc_selftest_hw;

static uint16_t s_t3_buf[STHW_T3_BUF_LEN]   DMA_BM_ALIGN_32;
static uint16_t s_t3b_buf[STHW_T3B_BUF_LEN] DMA_BM_ALIGN_32;

static AdcBm_Handle_t s_bm;
static AdcBm_Handle_t s_bm2;
static AdcLl_Handle_t s_ll;
static AdcLl_Handle_t s_ll2;

static void Selftest_Heartbeat(void)
{
    static uint32_t last_tick = 0U;
    uint32_t now = HAL_GetTick();
    if ((now - last_tick) >= 100U)
    {
        last_tick = now;
        g_shared.m4_heartbeat++;
    }
}

/* ---- Resolution sweep table ------------------------------------------------ */

typedef struct
{
    uint32_t bm;
    uint32_t ll;
    uint8_t  bits;
    uint8_t  code_rev_v;   /* CFGR.RES expected on Rev V (RM0399 ADC_CFGR)   */
    uint8_t  code_rev_y;   /* CFGR.RES expected on Rev Y                      */
    bool     rev_v_only;
} Sthw_Res_t;

static const Sthw_Res_t k_res[ADC_STHW_RES_POINTS] = {
    { ADC_BM_RESOLUTION_16B,     ADC_LL_RESOLUTION_16B,     16U, 0U, 0U, false },
    { ADC_BM_RESOLUTION_14B,     ADC_LL_RESOLUTION_14B,     14U, 1U, 1U, false },
    { ADC_BM_RESOLUTION_12B,     ADC_LL_RESOLUTION_12B,     12U, 2U, 2U, false },
    { ADC_BM_RESOLUTION_10B,     ADC_LL_RESOLUTION_10B,     10U, 3U, 3U, false },
    { ADC_BM_RESOLUTION_8B,      ADC_LL_RESOLUTION_8B,       8U, 7U, 4U, false },
    { ADC_BM_RESOLUTION_14B_OPT, ADC_LL_RESOLUTION_14B_OPT, 14U, 5U, 0U, true  },
    { ADC_BM_RESOLUTION_12B_OPT, ADC_LL_RESOLUTION_12B_OPT, 12U, 6U, 0U, true  },
};

/* ---- Flow description shared by T3, T3b and T4 ----------------------------- */

typedef struct
{
    ADC_TypeDef        *adc;
    DMA_Stream_TypeDef *stream;
    uint32_t            dmamux_req;
    uint16_t           *buf;
    uint32_t            buf_len;
    uint32_t            rate_hz;
    uint32_t            window_ms;
    uint32_t            dma_stop_ms;   /* 0 = never stop the DMA (T4 sets it) */
    uint32_t            num_channels;
} Sthw_FlowCfg_t;

typedef struct
{
    bool     started;
    uint32_t elapsed_ms;
    uint32_t ht;
    uint32_t tc;
    uint32_t dma_err;
    uint32_t adc_ovr;
    uint32_t half_ovr;
    uint32_t halves;
    uint16_t avg[2];
} Sthw_Flow_t;

/* ---- Small helpers ------------------------------------------------------------ */

static bool Sthw_IsRevV(void)
{
    return (((DBGMCU->IDCODE & 0xFFFF0000UL) >> 16U) > 0x1003U);
}

static uint32_t Sthw_AbsDiff(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

static uint32_t Sthw_Rate_x10(uint32_t events, uint32_t ms)
{
    return (ms == 0U) ? 0U : ((events * 10000U) / ms);
}

static void Tim3_Prepare(uint32_t rate_hz)
{
    RCC->APB1LENR |= RCC_APB1LENR_TIM3EN;
    (void)RCC->APB1LENR;

    TIM3->CR1 = 0U;
    TIM3->PSC = 0U;
    TIM3->ARR = (STHW_TIM_CLK_HZ / rate_hz) - 1U;
    TIM3->CNT = 0U;
    TIM3->CR2 = (TIM3->CR2 & ~TIM_CR2_MMS_Msk) | (0x2UL << TIM_CR2_MMS_Pos); /* TRGO = update */
    TIM3->SR  = 0U;
}

static void Tim3_Run(void)
{
    TIM3->CR1 |= TIM_CR1_CEN;
}

static void Tim3_Halt(void)
{
    TIM3->CR1 &= ~TIM_CR1_CEN;
    TIM3->CNT = 0U;
    TIM3->SR  = 0U;
}

static void Sthw_DmaConfig(DmaBm_Config_t *dma, const Sthw_FlowCfg_t *fc)
{
    memset(dma, 0, sizeof(*dma));
    dma->stream           = fc->stream;
    dma->request_id       = fc->dmamux_req;
    dma->direction        = DMA_BM_DIR_PERIPH_TO_MEM;
    dma->periph_data_size = DMA_BM_DATA_SIZE_HALFWORD;
    dma->mem_data_size    = DMA_BM_DATA_SIZE_HALFWORD;
    dma->mem_inc          = true;
    dma->circular         = true;
    dma->priority         = DMA_BM_PRIORITY_HIGH;
    dma->enable_it_ht     = true;
    dma->enable_it_tc     = true;
    dma->enable_it_te     = true;
    dma->enable_it_dme    = true;
    dma->periph_addr      = (uint32_t)&fc->adc->DR;
    dma->mem_addr         = (uint32_t)fc->buf;
    dma->buffer_length    = fc->buf_len;
}

/* ============================================================================
 * T2 - resolution sweep on real conversions
 * ========================================================================== */

static bool T2_Bm_Point(uint32_t res, uint16_t *avg, uint8_t *code)
{
    AdcBm_ChannelConfig_t ch = { .channel = ADC_BM_CHANNEL_VREFINT,
                                 .sampling_time = ADC_BM_SAMPLETIME_810CYCLES_5 };
    AdcBm_ConfigEx_t cfg = {0};
    cfg.adc_instance    = ADC3;
    cfg.channels        = &ch;
    cfg.num_channels    = 1U;
    cfg.resolution      = res;
    cfg.trigger_edge    = ADC_BM_TRIGGER_DISABLED;
    cfg.boost_mode      = ADC_BM_BOOST_AUTO;
    cfg.dma_mode        = ADC_BM_DMA_DISABLED;
    cfg.kernel_clock_hz = STHW_KER_CK_HZ;

    memset(&s_bm, 0, sizeof(s_bm));
    if (!ADC_BM_InitEx(&s_bm, &cfg))
    {
        return false;
    }
    *code = (uint8_t)((ADC3->CFGR & ADC_CFGR_RES_Msk) >> ADC_CFGR_RES_Pos);

    uint32_t sum = 0U;
    uint32_t n = 0U;
    if (ADC_BM_Calibrate(ADC3) && ADC_BM_Enable(ADC3))
    {
        for (uint32_t i = 0U; i < STHW_READS_PER_POINT; i++)
        {
            Selftest_Heartbeat();
            uint16_t raw = 0U;
            if (ADC_BM_ReadSingle(ADC3, ADC_BM_CHANNEL_VREFINT, ADC_BM_SAMPLETIME_810CYCLES_5, &raw, STHW_SPIN))
            {
                sum += raw;
                n++;
            }
        }
    }
    (void)ADC_BM_Deinit(&s_bm);

    if (n != STHW_READS_PER_POINT)
    {
        return false;
    }
    *avg = (uint16_t)((sum + (n / 2U)) / n);
    return true;
}

static bool T2_Ll_Point(uint32_t res, uint16_t *avg, uint8_t *code)
{
    AdcLl_ChannelConfig_t ch = { .channel = ADC_LL_CHANNEL_VREFINT,
                                 .sampling_time = ADC_LL_SAMPLETIME_810CYCLES_5 };
    AdcLl_ConfigEx_t cfg = {0};
    cfg.adc_instance    = ADC3;
    cfg.channels        = &ch;
    cfg.num_channels    = 1U;
    cfg.resolution      = res;
    cfg.trigger_edge    = ADC_LL_TRIGGER_DISABLED;
    cfg.boost_mode      = ADC_LL_BOOST_AUTO;
    cfg.dma_mode        = ADC_LL_DMA_DISABLED;
    cfg.kernel_clock_hz = STHW_KER_CK_HZ;

    memset(&s_ll, 0, sizeof(s_ll));
    if (!ADC_LL_InitEx(&s_ll, &cfg))
    {
        return false;
    }
    *code = (uint8_t)((ADC3->CFGR & ADC_CFGR_RES_Msk) >> ADC_CFGR_RES_Pos);

    uint32_t sum = 0U;
    uint32_t n = 0U;
    if (ADC_LL_Calibrate(ADC3) && ADC_LL_Enable(ADC3))
    {
        for (uint32_t i = 0U; i < STHW_READS_PER_POINT; i++)
        {
            Selftest_Heartbeat();
            uint16_t raw = 0U;
            if (ADC_LL_ReadSingle(ADC3, ADC_LL_CHANNEL_VREFINT, ADC_LL_SAMPLETIME_810CYCLES_5, &raw, STHW_SPIN))
            {
                sum += raw;
                n++;
            }
        }
    }
    (void)ADC_LL_Deinit(&s_ll);

    if (n != STHW_READS_PER_POINT)
    {
        return false;
    }
    *avg = (uint16_t)((sum + (n / 2U)) / n);
    return true;
}

/* The 16-bit point is the reference. Every other point, scaled back to 16 bits,
   must land within 2 % of it or 2 LSB of its own resolution, whichever is
   larger - a wrong RES encoding misses by a factor of 4 or more. */
static bool T2_Evaluate(uint32_t tier, bool rev_v)
{
    uint32_t ref = g_adc_selftest_hw.t2_raw[tier][0];
    if (ref == 0U)
    {
        return false;
    }

    for (uint32_t i = 0U; i < ADC_STHW_RES_POINTS; i++)
    {
        if (k_res[i].rev_v_only && !rev_v)
        {
            continue;
        }

        uint8_t want = rev_v ? k_res[i].code_rev_v : k_res[i].code_rev_y;
        if (g_adc_selftest_hw.t2_res_code[tier][i] != want)
        {
            return false;
        }

        uint32_t shift  = 16U - (uint32_t)k_res[i].bits;
        uint32_t scaled = (uint32_t)g_adc_selftest_hw.t2_raw[tier][i] << shift;
        uint32_t tol    = ref / 50U;
        if ((2UL << shift) > tol)
        {
            tol = 2UL << shift;
        }
        if (Sthw_AbsDiff(scaled, ref) > tol)
        {
            return false;
        }
    }
    return true;
}

static void T2_Run(bool rev_v)
{
    for (uint32_t i = 0U; i < ADC_STHW_RES_POINTS; i++)
    {
        if (k_res[i].rev_v_only && !rev_v)
        {
            continue;
        }

        uint16_t avg = 0U;
        uint8_t  code = 0xFFU;
        if (T2_Bm_Point(k_res[i].bm, &avg, &code))
        {
            g_adc_selftest_hw.t2_raw[ADC_STHW_TIER_BM][i] = avg;
        }
        g_adc_selftest_hw.t2_res_code[ADC_STHW_TIER_BM][i] = code;

        avg = 0U;
        code = 0xFFU;
        if (T2_Ll_Point(k_res[i].ll, &avg, &code))
        {
            g_adc_selftest_hw.t2_raw[ADC_STHW_TIER_LL][i] = avg;
        }
        g_adc_selftest_hw.t2_res_code[ADC_STHW_TIER_LL][i] = code;
    }

    for (uint32_t t = 0U; t < ADC_STHW_TIERS; t++)
    {
        if (T2_Evaluate(t, rev_v))
        {
            g_adc_selftest_hw.pass |= ADC_STHW_PASS_T2(t);
        }
    }
}

/* ============================================================================
 * T3 / T3b / T4 - triggered DMA flows
 * ========================================================================== */

static void Bm_Flow(const Sthw_FlowCfg_t *fc, const AdcBm_ChannelConfig_t *chs, Sthw_Flow_t *out)
{
    DmaBm_Config_t dma;
    Sthw_DmaConfig(&dma, fc);

    AdcBm_ConfigEx_t cfg = {0};
    cfg.adc_instance    = fc->adc;
    cfg.channels        = chs;
    cfg.num_channels    = fc->num_channels;
    cfg.resolution      = ADC_BM_RESOLUTION_16B;
    cfg.trigger_source  = ADC_BM_TRIG_TIM3_TRGO;
    cfg.trigger_edge    = ADC_BM_TRIGGER_RISING;
    cfg.boost_mode      = ADC_BM_BOOST_AUTO;
    cfg.dma_mode        = ADC_BM_DMA_CIRCULAR;
    cfg.buffer          = fc->buf;
    cfg.buffer_length   = fc->buf_len;
    cfg.kernel_clock_hz = STHW_KER_CK_HZ;
    cfg.clock_mode      = ADC_BM_CLOCK_ASYNC;
    cfg.prescaler       = ADC_BM_PRESC_DIV1;
    if (fc->adc == ADC1)
    {
        cfg.analog_pin_port = BOARD_ADC_PORT;
        cfg.analog_pin_idx  = STHW_ADC1_PIN_IDX;
    }

    memset(out, 0, sizeof(*out));
    memset(&s_bm, 0, sizeof(s_bm));
    Tim3_Prepare(fc->rate_hz);

    if (DMA_BM_InitStream(&dma) && ADC_BM_InitEx(&s_bm, &cfg) &&
        ADC_BM_Calibrate(fc->adc) && ADC_BM_Enable(fc->adc))
    {
        out->started = true;
        DMA_BM_Start(fc->stream);
        ADC_BM_StartConversion(fc->adc);
        Tim3_Run();

        bool     dma_stopped = false;
        uint32_t t0 = HAL_GetTick();
        for (;;)
        {
            Selftest_Heartbeat();
            uint32_t now = HAL_GetTick() - t0;
            if (now >= fc->window_ms)
            {
                break;
            }
            if ((fc->dma_stop_ms != 0U) && !dma_stopped && (now >= fc->dma_stop_ms))
            {
                (void)DMA_BM_Stop(fc->stream);
                dma_stopped = true;
            }
            if (DMA_BM_GetFlags(fc->stream) != 0U)
            {
                ADC_BM_IRQHandler_DMA(&s_bm, fc->stream);
            }
            if ((fc->adc->ISR & ADC_ISR_OVR) != 0U)
            {
                ADC_BM_IRQHandler_ADC(&s_bm);
            }
            uint8_t half = 0U;
            if (ADC_BM_GetReadyHalf(&s_bm, &half))
            {
                out->halves++;
                for (uint32_t i = 0U; (i < fc->num_channels) && (i < 2U); i++)
                {
                    ADC_BM_ComputeStatsChannel(&s_bm, half, i, BOARD_ADC_VREF_MV);
                    out->avg[i] = s_bm.stats.average_raw;
                }
            }
        }
        out->elapsed_ms = HAL_GetTick() - t0;
    }

    Tim3_Halt();
    (void)ADC_BM_StopConversion(fc->adc);
    (void)DMA_BM_Stop(fc->stream);
    (void)DMA_BM_DeinitStream(fc->stream);

    out->ht       = s_bm.stats.dma_ht_count;
    out->tc       = s_bm.stats.dma_tc_count;
    out->dma_err  = s_bm.stats.dma_error_count;
    out->adc_ovr  = s_bm.stats.adc_overrun_count;
    out->half_ovr = s_bm.stats.half_overrun_count;

    (void)ADC_BM_Deinit(&s_bm);
}

static void Ll_Flow(const Sthw_FlowCfg_t *fc, const AdcLl_ChannelConfig_t *chs, Sthw_Flow_t *out)
{
    DmaBm_Config_t dma;
    Sthw_DmaConfig(&dma, fc);

    AdcLl_ConfigEx_t cfg = {0};
    cfg.adc_instance    = fc->adc;
    cfg.channels        = chs;
    cfg.num_channels    = fc->num_channels;
    cfg.resolution      = ADC_LL_RESOLUTION_16B;
    cfg.trigger_source  = ADC_LL_TRIG_TIM3_TRGO;
    cfg.trigger_edge    = ADC_LL_TRIGGER_RISING;
    cfg.boost_mode      = ADC_LL_BOOST_AUTO;
    cfg.dma_mode        = ADC_LL_DMA_CIRCULAR;
    cfg.buffer          = fc->buf;
    cfg.buffer_length   = fc->buf_len;
    cfg.kernel_clock_hz = STHW_KER_CK_HZ;
    cfg.clock_mode      = ADC_LL_CLOCK_ASYNC;
    cfg.prescaler       = ADC_LL_PRESC_DIV1;
    if (fc->adc == ADC1)
    {
        cfg.analog_pin_port = BOARD_ADC_PORT;
        cfg.analog_pin_idx  = STHW_ADC1_PIN_IDX;
    }

    memset(out, 0, sizeof(*out));
    memset(&s_ll, 0, sizeof(s_ll));
    Tim3_Prepare(fc->rate_hz);

    if (DMA_BM_InitStream(&dma) && ADC_LL_InitEx(&s_ll, &cfg) &&
        ADC_LL_Calibrate(fc->adc) && ADC_LL_Enable(fc->adc))
    {
        out->started = true;
        DMA_BM_Start(fc->stream);
        ADC_LL_StartConversion(fc->adc);
        Tim3_Run();

        bool     dma_stopped = false;
        uint32_t t0 = HAL_GetTick();
        for (;;)
        {
            Selftest_Heartbeat();
            uint32_t now = HAL_GetTick() - t0;
            if (now >= fc->window_ms)
            {
                break;
            }
            if ((fc->dma_stop_ms != 0U) && !dma_stopped && (now >= fc->dma_stop_ms))
            {
                (void)DMA_BM_Stop(fc->stream);
                dma_stopped = true;
            }
            if (DMA_BM_GetFlags(fc->stream) != 0U)
            {
                ADC_LL_IRQHandler_DMA(&s_ll, fc->stream);
            }
            if ((fc->adc->ISR & ADC_ISR_OVR) != 0U)
            {
                ADC_LL_IRQHandler_ADC(&s_ll);
            }
            uint8_t half = 0U;
            if (ADC_LL_GetReadyHalf(&s_ll, &half))
            {
                out->halves++;
                for (uint32_t i = 0U; (i < fc->num_channels) && (i < 2U); i++)
                {
                    ADC_LL_ComputeStatsChannel(&s_ll, half, i, BOARD_ADC_VREF_MV);
                    out->avg[i] = s_ll.stats.average_raw;
                }
            }
        }
        out->elapsed_ms = HAL_GetTick() - t0;
    }

    Tim3_Halt();
    (void)ADC_LL_StopConversion(fc->adc);
    (void)DMA_BM_Stop(fc->stream);
    (void)DMA_BM_DeinitStream(fc->stream);

    out->ht       = s_ll.stats.dma_ht_count;
    out->tc       = s_ll.stats.dma_tc_count;
    out->dma_err  = s_ll.stats.dma_error_count;
    out->adc_ovr  = s_ll.stats.adc_overrun_count;
    out->half_ovr = s_ll.stats.half_overrun_count;

    (void)ADC_LL_Deinit(&s_ll);
}

static const Sthw_FlowCfg_t k_t3_flow = {
    .adc = ADC1, .stream = DMA1_Stream0, .dmamux_req = DMA_REQUEST_ADC1,
    .buf = s_t3_buf, .buf_len = STHW_T3_BUF_LEN,
    .rate_hz = BOARD_ADC_SAMPLING_RATE_HZ, .window_ms = STHW_T3_WINDOW_MS,
    .dma_stop_ms = 0U, .num_channels = 1U
};

static const Sthw_FlowCfg_t k_t3b_flow = {
    .adc = ADC3, .stream = DMA1_Stream1, .dmamux_req = DMA_REQUEST_ADC3,
    .buf = s_t3b_buf, .buf_len = STHW_T3B_BUF_LEN,
    .rate_hz = STHW_T3B_RATE_HZ, .window_ms = STHW_T3B_WINDOW_MS,
    .dma_stop_ms = 0U, .num_channels = 2U
};

static const Sthw_FlowCfg_t k_t4_flow = {
    .adc = ADC1, .stream = DMA1_Stream0, .dmamux_req = DMA_REQUEST_ADC1,
    .buf = s_t3_buf, .buf_len = STHW_T3_BUF_LEN,
    .rate_hz = BOARD_ADC_SAMPLING_RATE_HZ, .window_ms = STHW_T4_WINDOW_MS,
    .dma_stop_ms = STHW_T4_DMA_STOP_MS, .num_channels = 1U
};

static bool T3_Evaluate(const Sthw_Flow_t *f)
{
    uint32_t expect_x10 = (BOARD_ADC_SAMPLING_RATE_HZ * 10U) / STHW_T3_BUF_LEN; /* 4882 = 488.2/s */
    uint32_t tol_x10    = expect_x10 / 100U;                                     /* 1 %           */

    return f->started &&
           (f->elapsed_ms >= STHW_T3_WINDOW_MS) &&
           (Sthw_AbsDiff(Sthw_Rate_x10(f->ht, f->elapsed_ms), expect_x10) <= tol_x10) &&
           (Sthw_AbsDiff(Sthw_Rate_x10(f->tc, f->elapsed_ms), expect_x10) <= tol_x10) &&
           (f->dma_err == 0U) && (f->adc_ovr == 0U) && (f->half_ovr == 0U);
}

/* Rank 0 must be VREFINT - within 3 % of T2's 16-bit reading when T2 produced
   one - and rank 1 the temperature sensor, which sits far lower (V30 = 0.62 V
   against VREFINT = 1.216 V, DS12923). Swapped or smeared ranks fail. */
static bool T3b_Evaluate(const Sthw_Flow_t *f, uint16_t vref_ref)
{
    if (!f->started || (f->halves < 4U))
    {
        return false;
    }

    uint32_t vref = f->avg[0];
    uint32_t ts   = f->avg[1];
    bool vref_ok = (vref_ref != 0U) ? (Sthw_AbsDiff(vref, vref_ref) <= ((uint32_t)vref_ref * 3U) / 100U)
                                    : ((vref >= 20000U) && (vref <= 28000U));
    bool ts_ok   = (ts >= 9000U) && (ts <= 20000U);

    return vref_ok && ts_ok && (Sthw_AbsDiff(vref, ts) > 3000U);
}

static bool T4_Evaluate(const Sthw_Flow_t *f)
{
    return f->started && (f->adc_ovr > 0U) && (f->dma_err == 0U);
}

static void T3_T4_Run(void)
{
    static const AdcBm_ChannelConfig_t bm_adc1[1] = {
        { .channel = STHW_ADC1_CHANNEL, .sampling_time = ADC_BM_SAMPLETIME_8CYCLES_5 }
    };
    static const AdcLl_ChannelConfig_t ll_adc1[1] = {
        { .channel = STHW_ADC1_CHANNEL, .sampling_time = ADC_LL_SAMPLETIME_8CYCLES_5 }
    };
    static const AdcBm_ChannelConfig_t bm_adc3[2] = {
        { .channel = ADC_BM_CHANNEL_VREFINT,    .sampling_time = ADC_BM_SAMPLETIME_810CYCLES_5 },
        { .channel = ADC_BM_CHANNEL_TEMPSENSOR, .sampling_time = ADC_BM_SAMPLETIME_810CYCLES_5 }
    };
    static const AdcLl_ChannelConfig_t ll_adc3[2] = {
        { .channel = ADC_LL_CHANNEL_VREFINT,    .sampling_time = ADC_LL_SAMPLETIME_810CYCLES_5 },
        { .channel = ADC_LL_CHANNEL_TEMPSENSOR, .sampling_time = ADC_LL_SAMPLETIME_810CYCLES_5 }
    };

    Sthw_Flow_t f;

    for (uint32_t t = 0U; t < ADC_STHW_TIERS; t++)
    {
        /* T3 */
        if (t == ADC_STHW_TIER_BM) { Bm_Flow(&k_t3_flow, bm_adc1, &f); }
        else                       { Ll_Flow(&k_t3_flow, ll_adc1, &f); }
        g_adc_selftest_hw.t3_elapsed_ms[t] = f.elapsed_ms;
        g_adc_selftest_hw.t3_ht[t]         = f.ht;
        g_adc_selftest_hw.t3_tc[t]         = f.tc;
        g_adc_selftest_hw.t3_dma_err[t]    = f.dma_err;
        g_adc_selftest_hw.t3_adc_ovr[t]    = f.adc_ovr;
        g_adc_selftest_hw.t3_half_ovr[t]   = f.half_ovr;
        g_adc_selftest_hw.t3_avg[t]        = f.avg[0];
        if (T3_Evaluate(&f))
        {
            g_adc_selftest_hw.pass |= ADC_STHW_PASS_T3(t);
        }

        /* T3b */
        if (t == ADC_STHW_TIER_BM) { Bm_Flow(&k_t3b_flow, bm_adc3, &f); }
        else                       { Ll_Flow(&k_t3b_flow, ll_adc3, &f); }
        g_adc_selftest_hw.t3b_halves[t] = f.halves;
        g_adc_selftest_hw.t3b_avg[t][0] = f.avg[0];
        g_adc_selftest_hw.t3b_avg[t][1] = f.avg[1];
        if (T3b_Evaluate(&f, g_adc_selftest_hw.t2_raw[t][0]))
        {
            g_adc_selftest_hw.pass |= ADC_STHW_PASS_T3B(t);
        }

        /* T4 */
        if (t == ADC_STHW_TIER_BM) { Bm_Flow(&k_t4_flow, bm_adc1, &f); }
        else                       { Ll_Flow(&k_t4_flow, ll_adc1, &f); }
        g_adc_selftest_hw.t4_adc_ovr[t] = f.adc_ovr;
        g_adc_selftest_hw.t4_dma_err[t] = f.dma_err;
        if (T4_Evaluate(&f))
        {
            g_adc_selftest_hw.pass |= ADC_STHW_PASS_T4(t);
        }
    }
}

/* ============================================================================
 * T6 - ADCx_CCR is shared by ADC1 and ADC2
 * ========================================================================== */

static void T6_Bm(void)
{
    const uint32_t t = ADC_STHW_TIER_BM;
    AdcBm_ChannelConfig_t ch = { .channel = STHW_ADC1_CHANNEL,
                                 .sampling_time = ADC_BM_SAMPLETIME_8CYCLES_5 };
    AdcBm_ConfigEx_t cfg = {0};
    cfg.adc_instance    = ADC2;
    cfg.channels        = &ch;
    cfg.num_channels    = 1U;
    cfg.resolution      = ADC_BM_RESOLUTION_16B;
    cfg.trigger_edge    = ADC_BM_TRIGGER_DISABLED;
    cfg.boost_mode      = ADC_BM_BOOST_AUTO;
    cfg.dma_mode        = ADC_BM_DMA_DISABLED;
    cfg.kernel_clock_hz = STHW_KER_CK_HZ;
    cfg.clock_mode      = ADC_BM_CLOCK_ASYNC;
    cfg.prescaler       = ADC_BM_PRESC_DIV1;

    memset(&s_bm2, 0, sizeof(s_bm2));
    bool up = ADC_BM_InitEx(&s_bm2, &cfg) && ADC_BM_Enable(ADC2);
    g_adc_selftest_hw.t6_adc2_up[t]    = up;
    g_adc_selftest_hw.t6_ccr_before[t] = ADC12_COMMON->CCR;

    /* ADC1 asks for /2 while ADC2 runs on /1: must be refused. */
    cfg.adc_instance = ADC1;
    cfg.prescaler    = ADC_BM_PRESC_DIV2;
    memset(&s_bm, 0, sizeof(s_bm));
    g_adc_selftest_hw.t6_rejected[t]      = !ADC_BM_InitEx(&s_bm, &cfg);
    g_adc_selftest_hw.t6_ccr_after[t]     = ADC12_COMMON->CCR;
    g_adc_selftest_hw.t6_adc2_still_on[t] = ((ADC2->CR & ADC_CR_ADEN) != 0U);

    /* The same settings as ADC2 are compatible: must be accepted. */
    cfg.prescaler = ADC_BM_PRESC_DIV1;
    memset(&s_bm, 0, sizeof(s_bm));
    g_adc_selftest_hw.t6_accepted[t] = ADC_BM_InitEx(&s_bm, &cfg);

    (void)ADC_BM_Deinit(&s_bm);
    (void)ADC_BM_Deinit(&s_bm2);
}

static void T6_Ll(void)
{
    const uint32_t t = ADC_STHW_TIER_LL;
    AdcLl_ChannelConfig_t ch = { .channel = STHW_ADC1_CHANNEL,
                                 .sampling_time = ADC_LL_SAMPLETIME_8CYCLES_5 };
    AdcLl_ConfigEx_t cfg = {0};
    cfg.adc_instance    = ADC2;
    cfg.channels        = &ch;
    cfg.num_channels    = 1U;
    cfg.resolution      = ADC_LL_RESOLUTION_16B;
    cfg.trigger_edge    = ADC_LL_TRIGGER_DISABLED;
    cfg.boost_mode      = ADC_LL_BOOST_AUTO;
    cfg.dma_mode        = ADC_LL_DMA_DISABLED;
    cfg.kernel_clock_hz = STHW_KER_CK_HZ;
    cfg.clock_mode      = ADC_LL_CLOCK_ASYNC;
    cfg.prescaler       = ADC_LL_PRESC_DIV1;

    memset(&s_ll2, 0, sizeof(s_ll2));
    bool up = ADC_LL_InitEx(&s_ll2, &cfg) && ADC_LL_Enable(ADC2);
    g_adc_selftest_hw.t6_adc2_up[t]    = up;
    g_adc_selftest_hw.t6_ccr_before[t] = ADC12_COMMON->CCR;

    cfg.adc_instance = ADC1;
    cfg.prescaler    = ADC_LL_PRESC_DIV2;
    memset(&s_ll, 0, sizeof(s_ll));
    g_adc_selftest_hw.t6_rejected[t]      = !ADC_LL_InitEx(&s_ll, &cfg);
    g_adc_selftest_hw.t6_ccr_after[t]     = ADC12_COMMON->CCR;
    g_adc_selftest_hw.t6_adc2_still_on[t] = ((ADC2->CR & ADC_CR_ADEN) != 0U);

    cfg.prescaler = ADC_LL_PRESC_DIV1;
    memset(&s_ll, 0, sizeof(s_ll));
    g_adc_selftest_hw.t6_accepted[t] = ADC_LL_InitEx(&s_ll, &cfg);

    (void)ADC_LL_Deinit(&s_ll);
    (void)ADC_LL_Deinit(&s_ll2);
}

static void T6_Run(void)
{
    T6_Bm();
    T6_Ll();

    for (uint32_t t = 0U; t < ADC_STHW_TIERS; t++)
    {
        if (g_adc_selftest_hw.t6_adc2_up[t] &&
            g_adc_selftest_hw.t6_rejected[t] &&
            (g_adc_selftest_hw.t6_ccr_after[t] == g_adc_selftest_hw.t6_ccr_before[t]) &&
            g_adc_selftest_hw.t6_adc2_still_on[t] &&
            g_adc_selftest_hw.t6_accepted[t])
        {
            g_adc_selftest_hw.pass |= ADC_STHW_PASS_T6(t);
        }
    }
}

/* ============================================================================
 * Console lines (at most SHARED_MBOX_SIZE - 1 characters each)
 * ========================================================================== */

#define STHW_MAX_LINES  8U

static char     s_lines[STHW_MAX_LINES][SHARED_MBOX_SIZE];
static uint32_t s_line_count;
static uint32_t s_line_next;
static uint32_t s_line_len;

static void Line_Begin(void)
{
    s_line_len = 0U;
    if (s_line_count < STHW_MAX_LINES)
    {
        s_lines[s_line_count][0] = '\0';
    }
}

static void Line_End(void)
{
    if (s_line_count < STHW_MAX_LINES)
    {
        s_line_count++;
    }
}

static void Line_Str(const char *s)
{
    if (s_line_count >= STHW_MAX_LINES)
    {
        return;
    }
    char *dst = s_lines[s_line_count];
    while ((*s != '\0') && (s_line_len < (SHARED_MBOX_SIZE - 1U)))
    {
        dst[s_line_len++] = *s++;
    }
    dst[s_line_len] = '\0';
}

static void Line_U32(uint32_t v)
{
    char     tmp[11];
    uint32_t n = 0U;
    do
    {
        tmp[n++] = (char)('0' + (int)(v % 10U));
        v /= 10U;
    } while ((v != 0U) && (n < sizeof(tmp) - 1U));

    char out[12];
    uint32_t k = 0U;
    while (n > 0U)
    {
        out[k++] = tmp[--n];
    }
    out[k] = '\0';
    Line_Str(out);
}

static void Line_X10(uint32_t v_x10)
{
    Line_U32(v_x10 / 10U);
    Line_Str(".");
    Line_U32(v_x10 % 10U);
}

static void Line_Pass(const char *label, uint32_t bit)
{
    Line_Str(label);
    Line_Str(((g_adc_selftest_hw.pass & bit) != 0U) ? "1" : "0");
}

static void Sthw_BuildLines(void)
{
    const uint32_t bm = ADC_STHW_TIER_BM;
    const uint32_t ll = ADC_STHW_TIER_LL;

    Line_Begin();
    Line_Pass("HW T2 BM:", ADC_STHW_PASS_T2(bm));
    Line_Pass(" LL:", ADC_STHW_PASS_T2(ll));
    Line_Str(" r8=");
    Line_U32(g_adc_selftest_hw.t2_res_code[bm][4]);
    Line_Str("/");
    Line_U32(g_adc_selftest_hw.t2_res_code[ll][4]);
    Line_Str(" v16=");
    Line_U32(g_adc_selftest_hw.t2_raw[bm][0]);
    Line_Str(" v8=");
    Line_U32(g_adc_selftest_hw.t2_raw[bm][4]);
    Line_Str("/");
    Line_U32(g_adc_selftest_hw.t2_raw[ll][4]);
    Line_End();

    for (uint32_t t = 0U; t < ADC_STHW_TIERS; t++)
    {
        Line_Begin();
        Line_Pass((t == bm) ? "HW T3 BM:" : "HW T3 LL:", ADC_STHW_PASS_T3(t));
        Line_Str(" ht=");
        Line_X10(Sthw_Rate_x10(g_adc_selftest_hw.t3_ht[t], g_adc_selftest_hw.t3_elapsed_ms[t]));
        Line_Str(" tc=");
        Line_X10(Sthw_Rate_x10(g_adc_selftest_hw.t3_tc[t], g_adc_selftest_hw.t3_elapsed_ms[t]));
        Line_Str(" err=");
        Line_U32(g_adc_selftest_hw.t3_dma_err[t]);
        Line_Str(" ovr=");
        Line_U32(g_adc_selftest_hw.t3_adc_ovr[t]);
        Line_Str(" hovr=");
        Line_U32(g_adc_selftest_hw.t3_half_ovr[t]);
        Line_End();
    }

    Line_Begin();
    Line_Pass("HW T3b BM:", ADC_STHW_PASS_T3B(bm));
    Line_Pass(" LL:", ADC_STHW_PASS_T3B(ll));
    Line_Str(" vref=");
    Line_U32(g_adc_selftest_hw.t3b_avg[bm][0]);
    Line_Str("/");
    Line_U32(g_adc_selftest_hw.t3b_avg[ll][0]);
    Line_Str(" ts=");
    Line_U32(g_adc_selftest_hw.t3b_avg[bm][1]);
    Line_Str("/");
    Line_U32(g_adc_selftest_hw.t3b_avg[ll][1]);
    Line_End();

    Line_Begin();
    Line_Pass("HW T4 BM:", ADC_STHW_PASS_T4(bm));
    Line_Pass(" LL:", ADC_STHW_PASS_T4(ll));
    Line_Str(" ovr=");
    Line_U32(g_adc_selftest_hw.t4_adc_ovr[bm]);
    Line_Str("/");
    Line_U32(g_adc_selftest_hw.t4_adc_ovr[ll]);
    Line_Str(" dma=");
    Line_U32(g_adc_selftest_hw.t4_dma_err[bm]);
    Line_Str("/");
    Line_U32(g_adc_selftest_hw.t4_dma_err[ll]);
    Line_End();

    Line_Begin();
    Line_Pass("HW T6 BM:", ADC_STHW_PASS_T6(bm));
    Line_Pass(" LL:", ADC_STHW_PASS_T6(ll));
    Line_Str(" rej=");
    Line_U32(g_adc_selftest_hw.t6_rejected[bm] ? 1U : 0U);
    Line_Str("/");
    Line_U32(g_adc_selftest_hw.t6_rejected[ll] ? 1U : 0U);
    Line_Str(" ok=");
    Line_U32(g_adc_selftest_hw.t6_accepted[bm] ? 1U : 0U);
    Line_Str("/");
    Line_U32(g_adc_selftest_hw.t6_accepted[ll] ? 1U : 0U);
    Line_Str(" adc2=");
    Line_U32(g_adc_selftest_hw.t6_adc2_still_on[bm] ? 1U : 0U);
    Line_Str("/");
    Line_U32(g_adc_selftest_hw.t6_adc2_still_on[ll] ? 1U : 0U);
    Line_End();

    Line_Begin();
    Line_Str("HW RESULT ");
    Line_Str((g_adc_selftest_hw.pass == ADC_STHW_PASS_ALL) ? "PASS" : "FAIL");
    Line_Str(" mask=");
    Line_U32(g_adc_selftest_hw.pass);
    Line_Str("/");
    Line_U32(ADC_STHW_PASS_ALL);
    Line_End();
}

/* ============================================================================
 * Public entry points
 * ========================================================================== */

void ADC_SelfTestHw_Run(void)
{
    bool rev_v = Sthw_IsRevV();
    g_adc_selftest_hw.rev_id = (DBGMCU->IDCODE & 0xFFFF0000UL) >> 16U;
    g_adc_selftest_hw.pass   = 0U;

    T2_Run(rev_v);
    T3_T4_Run();
    T6_Run();

    /* Every stream and ADC above raised its IRQ lines with the NVIC lines still
       disabled. Drop the pending bits so the application does not take a stale
       interrupt the moment ADC_Acquisition_Init() enables them. */
    NVIC_ClearPendingIRQ(DMA1_Stream0_IRQn);
    NVIC_ClearPendingIRQ(DMA1_Stream1_IRQn);
    NVIC_ClearPendingIRQ(ADC_IRQn);
    NVIC_ClearPendingIRQ(ADC3_IRQn);

    Sthw_BuildLines();
    g_adc_selftest_hw.done = STHW_DONE_MAGIC;
}

void ADC_SelfTestHw_Poll(void)
{
    if ((s_line_next >= s_line_count) || (g_shared.m4_to_m7_len != 0U))
    {
        return;
    }

    SHARED_LOCK(HSEM_ID_M4_TO_M7_MBOX);
    if (g_shared.m4_to_m7_len == 0U)
    {
        const char *src = s_lines[s_line_next];
        uint32_t n = 0U;
        while ((src[n] != '\0') && (n < (SHARED_MBOX_SIZE - 1U)))
        {
            g_shared.m4_to_m7[n] = (uint8_t)src[n];
            n++;
        }
        g_shared.m4_to_m7[n] = 0U;
        __DMB();
        g_shared.m4_to_m7_len = n;
        s_line_next++;
    }
    SHARED_UNLOCK(HSEM_ID_M4_TO_M7_MBOX);
}
