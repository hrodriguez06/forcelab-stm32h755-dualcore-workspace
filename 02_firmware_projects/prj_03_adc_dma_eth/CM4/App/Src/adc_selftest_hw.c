/**
  ******************************************************************************
  * @file    adc_selftest_hw.c
  * @brief   Hardware tests T2, T3, T3b, T4 and T6 of the shared ADC driver,
  *          HAL tier. See adc_selftest_hw.h for what each one proves.
  ******************************************************************************
  */

#include "adc_selftest_hw.h"
#include "main.h"
#include "adc_hal.h"
#include <string.h>

/* adc_ker_ck is PLL2P = 25 MHz, programmed by CPU1 in CM7/Core/Src/main.c. */
#define STHW_KER_CK_HZ          25000000UL
/* APB1 is divided by 2, so the APB1 timers run at twice PCLK1 (RM0399 8.5.9). */
#define STHW_TIM_CLK_HZ         (2UL * BOARD_APB_HZ)
#define STHW_ADC1_PIN_IDX       ((uint32_t)__builtin_ctz((uint32_t)BOARD_ADC_PIN))

#define STHW_READS_PER_POINT    16U
#define STHW_READ_TIMEOUT_MS    10U

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

static uint16_t s_t3_buf[STHW_T3_BUF_LEN]   __attribute__((aligned(32)));
static uint16_t s_t3b_buf[STHW_T3B_BUF_LEN] __attribute__((aligned(32)));

static AdcHal_Handle_t s_hal;
static AdcHal_Handle_t s_hal2;

/* ---- Resolution sweep table ------------------------------------------------ */

typedef struct
{
    uint32_t res;
    uint8_t  bits;
    uint8_t  code_rev_v;   /* CFGR.RES expected on Rev V (RM0399 ADC_CFGR) */
    uint8_t  code_rev_y;   /* CFGR.RES expected on Rev Y                    */
    bool     rev_v_only;
} Sthw_Res_t;

static const Sthw_Res_t k_res[ADC_STHW_RES_POINTS] = {
    { ADC_RESOLUTION_16B,     16U, 0U, 0U, false },
    { ADC_RESOLUTION_14B,     14U, 1U, 1U, false },
    { ADC_RESOLUTION_12B,     12U, 2U, 2U, false },
    { ADC_RESOLUTION_10B,     10U, 3U, 3U, false },
    { ADC_RESOLUTION_8B,       8U, 7U, 4U, false },
    { ADC_RESOLUTION_14B_OPT, 14U, 5U, 0U, true  },
    { ADC_RESOLUTION_12B_OPT, 12U, 6U, 0U, true  },
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

/* BOOST[1:0] a given ADC input clock needs: RM0399 ADC_CR, with the fixed /2
   in front of the analog block on Rev V. Same table as ADC_ConfigureBoostMode. */
static uint32_t Sthw_BoostNeeded(uint32_t f_in_hz, bool rev_v)
{
    if (!rev_v)
    {
        return (f_in_hz > 20000000UL) ? 1U : 0U;
    }
    uint32_t f = f_in_hz / 2U;
    if (f <= 6250000UL)  { return 0U; }
    if (f <= 12500000UL) { return 1U; }
    if (f <= 25000000UL) { return 2U; }
    return 3U;
}

/* Input clock actually selected by ADC12_COMMON->CCR (asynchronous mode only). */
static uint32_t Sthw_CcrClockHz(uint32_t ccr)
{
    static const uint16_t k_div[12] = { 1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256 };
    uint32_t presc = (ccr & ADC_CCR_PRESC_Msk) >> ADC_CCR_PRESC_Pos;
    return (presc < 12U) ? (STHW_KER_CK_HZ / k_div[presc]) : 0U;
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

/* ============================================================================
 * T2 - resolution sweep on real conversions
 * ========================================================================== */

static bool T2_Point(uint32_t res, uint16_t *avg, uint8_t *code)
{
    AdcHal_ChannelConfig_t ch = { .channel = ADC_CHANNEL_VREFINT,
                                  .sampling_time = ADC_SAMPLETIME_810CYCLES_5 };
    AdcHal_ConfigEx_t cfg = {0};
    cfg.adc_instance    = ADC3;
    cfg.channels        = &ch;
    cfg.num_channels    = 1U;
    cfg.resolution      = res;
    cfg.trigger_source  = ADC_SOFTWARE_START;
    cfg.trigger_edge    = ADC_EXTERNALTRIGCONVEDGE_NONE;
    cfg.boost_mode      = ADC_HAL_BOOST_AUTO;
    cfg.kernel_clock_hz = STHW_KER_CK_HZ;

    memset(&s_hal, 0, sizeof(s_hal));
    if (!ADC_HAL_InitEx(&s_hal, &cfg))
    {
        return false;
    }
    *code = (uint8_t)((ADC3->CFGR & ADC_CFGR_RES_Msk) >> ADC_CFGR_RES_Pos);

    uint32_t sum = 0U;
    uint32_t n = 0U;
    if (ADC_HAL_Calibrate(&s_hal))
    {
        for (uint32_t i = 0U; i < STHW_READS_PER_POINT; i++)
        {
            uint16_t raw = 0U;
            if (ADC_HAL_ReadSingle(&s_hal, ADC_CHANNEL_VREFINT, ADC_SAMPLETIME_810CYCLES_5, &raw, STHW_READ_TIMEOUT_MS))
            {
                sum += raw;
                n++;
            }
        }
    }
    (void)ADC_HAL_Deinit(&s_hal);

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
static bool T2_Evaluate(bool rev_v)
{
    uint32_t ref = g_adc_selftest_hw.t2_raw[0];
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
        if (g_adc_selftest_hw.t2_res_code[i] != want)
        {
            return false;
        }

        uint32_t shift  = 16U - (uint32_t)k_res[i].bits;
        uint32_t scaled = (uint32_t)g_adc_selftest_hw.t2_raw[i] << shift;
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
        if (T2_Point(k_res[i].res, &avg, &code))
        {
            g_adc_selftest_hw.t2_raw[i] = avg;
        }
        g_adc_selftest_hw.t2_res_code[i] = code;
    }

    if (T2_Evaluate(rev_v))
    {
        g_adc_selftest_hw.pass |= ADC_STHW_PASS_T2;
    }
}

/* ============================================================================
 * T3 / T3b / T4 - triggered DMA flows
 * ========================================================================== */

static void Hal_Flow(const Sthw_FlowCfg_t *fc, const AdcHal_ChannelConfig_t *chs, Sthw_Flow_t *out)
{
    AdcHal_ConfigEx_t cfg = {0};
    cfg.adc_instance    = fc->adc;
    cfg.channels        = chs;
    cfg.num_channels    = fc->num_channels;
    cfg.resolution      = ADC_RESOLUTION_16B;
    cfg.trigger_source  = ADC_EXTERNALTRIG_T3_TRGO;
    cfg.trigger_edge    = ADC_EXTERNALTRIGCONVEDGE_RISING;
    cfg.boost_mode      = ADC_HAL_BOOST_AUTO;
    cfg.dma_stream      = fc->stream;
    cfg.dma_request     = fc->dmamux_req;
    cfg.buffer          = fc->buf;
    cfg.buffer_length   = fc->buf_len;
    cfg.kernel_clock_hz = STHW_KER_CK_HZ;
    if (fc->adc == ADC1)
    {
        cfg.analog_pin_port = BOARD_ADC_PORT;
        cfg.analog_pin_idx  = STHW_ADC1_PIN_IDX;
    }

    /* The HAL tier does not clock the DMA controller: the application does. */
    __HAL_RCC_DMA1_CLK_ENABLE();

    memset(out, 0, sizeof(*out));
    memset(&s_hal, 0, sizeof(s_hal));
    Tim3_Prepare(fc->rate_hz);

    if (ADC_HAL_InitEx(&s_hal, &cfg) && ADC_HAL_Calibrate(&s_hal) && ADC_HAL_Start_DMA(&s_hal))
    {
        out->started = true;
        Tim3_Run();

        bool     dma_stopped = false;
        uint32_t t0 = HAL_GetTick();
        for (;;)
        {
            uint32_t now = HAL_GetTick() - t0;
            if (now >= fc->window_ms)
            {
                break;
            }
            if ((fc->dma_stop_ms != 0U) && !dma_stopped && (now >= fc->dma_stop_ms))
            {
                /* Pull the stream out from under the HAL, as a bus fault or a
                   stuck consumer would: the ADC keeps converting, nobody reads DR. */
                ((DMA_Stream_TypeDef *)s_hal.hdma.Instance)->CR &= ~DMA_SxCR_EN;
                dma_stopped = true;
            }
            ADC_HAL_IRQHandler_DMA(&s_hal);
            if ((fc->adc->ISR & ADC_ISR_OVR) != 0U)
            {
                ADC_HAL_IRQHandler_ADC(&s_hal);
            }
            uint8_t half = 0U;
            if (ADC_HAL_GetReadyHalf(&s_hal, &half))
            {
                out->halves++;
                for (uint32_t i = 0U; (i < fc->num_channels) && (i < 2U); i++)
                {
                    ADC_HAL_ComputeStatsChannel(&s_hal, half, i, BOARD_ADC_VREF_MV);
                    out->avg[i] = s_hal.stats.average_raw;
                }
            }
        }
        out->elapsed_ms = HAL_GetTick() - t0;
    }

    Tim3_Halt();

    out->ht       = s_hal.stats.dma_ht_count;
    out->tc       = s_hal.stats.dma_tc_count;
    out->dma_err  = s_hal.stats.dma_error_count;
    out->adc_ovr  = s_hal.stats.adc_overrun_count;
    out->half_ovr = s_hal.stats.half_overrun_count;

    (void)ADC_HAL_Deinit(&s_hal);
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

static void T3_T4_Run(void)
{
    static const AdcHal_ChannelConfig_t adc1_chs[1] = {
        { .channel = BOARD_ADC_CHANNEL, .sampling_time = ADC_SAMPLETIME_8CYCLES_5 }
    };
    static const AdcHal_ChannelConfig_t adc3_chs[2] = {
        { .channel = ADC_CHANNEL_VREFINT,    .sampling_time = ADC_SAMPLETIME_810CYCLES_5 },
        { .channel = ADC_CHANNEL_TEMPSENSOR, .sampling_time = ADC_SAMPLETIME_810CYCLES_5 }
    };

    Sthw_Flow_t f;

    /* T3 */
    Hal_Flow(&k_t3_flow, adc1_chs, &f);
    g_adc_selftest_hw.t3_elapsed_ms = f.elapsed_ms;
    g_adc_selftest_hw.t3_ht         = f.ht;
    g_adc_selftest_hw.t3_tc         = f.tc;
    g_adc_selftest_hw.t3_dma_err    = f.dma_err;
    g_adc_selftest_hw.t3_adc_ovr    = f.adc_ovr;
    g_adc_selftest_hw.t3_half_ovr   = f.half_ovr;
    g_adc_selftest_hw.t3_avg        = f.avg[0];

    uint32_t expect_x10 = (BOARD_ADC_SAMPLING_RATE_HZ * 10U) / STHW_T3_BUF_LEN; /* 4882 = 488.2/s */
    uint32_t tol_x10    = expect_x10 / 100U;                                     /* 1 %           */
    if (f.started && (f.elapsed_ms >= STHW_T3_WINDOW_MS) &&
        (Sthw_AbsDiff(Sthw_Rate_x10(f.ht, f.elapsed_ms), expect_x10) <= tol_x10) &&
        (Sthw_AbsDiff(Sthw_Rate_x10(f.tc, f.elapsed_ms), expect_x10) <= tol_x10) &&
        (f.dma_err == 0U) && (f.adc_ovr == 0U) && (f.half_ovr == 0U))
    {
        g_adc_selftest_hw.pass |= ADC_STHW_PASS_T3;
    }

    /* T3b: rank 0 must be VREFINT (within 3 % of T2's 16-bit reading), rank 1
       the temperature sensor, far lower (V30 = 0.62 V vs 1.216 V, DS12923). */
    Hal_Flow(&k_t3b_flow, adc3_chs, &f);
    g_adc_selftest_hw.t3b_halves = f.halves;
    g_adc_selftest_hw.t3b_avg[0] = f.avg[0];
    g_adc_selftest_hw.t3b_avg[1] = f.avg[1];
    {
        uint32_t ref  = g_adc_selftest_hw.t2_raw[0];
        uint32_t vref = f.avg[0];
        uint32_t ts   = f.avg[1];
        bool vref_ok  = (ref != 0U) ? (Sthw_AbsDiff(vref, ref) <= (ref * 3U) / 100U)
                                    : ((vref >= 20000U) && (vref <= 28000U));
        bool ts_ok    = (ts >= 9000U) && (ts <= 20000U);
        if (f.started && (f.halves >= 4U) && vref_ok && ts_ok && (Sthw_AbsDiff(vref, ts) > 3000U))
        {
            g_adc_selftest_hw.pass |= ADC_STHW_PASS_T3B;
        }
    }

    /* T4 */
    Hal_Flow(&k_t4_flow, adc1_chs, &f);
    g_adc_selftest_hw.t4_adc_ovr = f.adc_ovr;
    g_adc_selftest_hw.t4_dma_err = f.dma_err;
    if (f.started && (f.adc_ovr > 0U) && (f.dma_err == 0U))
    {
        g_adc_selftest_hw.pass |= ADC_STHW_PASS_T4;
    }
}

/* ============================================================================
 * T6 - ADCx_CCR is shared by ADC1 and ADC2
 * ========================================================================== */

static void T6_Run(bool rev_v)
{
    AdcHal_ChannelConfig_t ch = { .channel = BOARD_ADC_CHANNEL,
                                  .sampling_time = ADC_SAMPLETIME_8CYCLES_5 };
    AdcHal_ConfigEx_t cfg = {0};
    cfg.adc_instance    = ADC2;
    cfg.channels        = &ch;
    cfg.num_channels    = 1U;
    cfg.resolution      = ADC_RESOLUTION_16B;
    cfg.trigger_source  = ADC_SOFTWARE_START;
    cfg.trigger_edge    = ADC_EXTERNALTRIGCONVEDGE_NONE;
    cfg.boost_mode      = ADC_HAL_BOOST_AUTO;
    cfg.kernel_clock_hz = STHW_KER_CK_HZ;
    cfg.clock_prescaler = ADC_CLOCK_ASYNC_DIV1;

    /* ADC2 up and enabled: one software conversion leaves ADEN set. */
    memset(&s_hal2, 0, sizeof(s_hal2));
    bool up = ADC_HAL_InitEx(&s_hal2, &cfg) &&
              (HAL_ADC_Start(&s_hal2.hadc) == HAL_OK) &&
              (HAL_ADC_PollForConversion(&s_hal2.hadc, STHW_READ_TIMEOUT_MS) == HAL_OK) &&
              ((ADC2->CR & ADC_CR_ADEN) != 0U);
    g_adc_selftest_hw.t6_adc2_up    = up;
    g_adc_selftest_hw.t6_ccr_before = ADC12_COMMON->CCR;

    /* ADC1 asks for /2 while ADC2 runs on /1: must be refused. */
    cfg.adc_instance    = ADC1;
    cfg.clock_prescaler = ADC_CLOCK_ASYNC_DIV2;
    memset(&s_hal, 0, sizeof(s_hal));
    bool accepted_conflict = ADC_HAL_InitEx(&s_hal, &cfg);
    g_adc_selftest_hw.t6_rejected      = !accepted_conflict;
    g_adc_selftest_hw.t6_ccr_after     = ADC12_COMMON->CCR;
    g_adc_selftest_hw.t6_adc2_still_on = ((ADC2->CR & ADC_CR_ADEN) != 0U);
    g_adc_selftest_hw.t6_boost         = (ADC1->CR & ADC_CR_BOOST) >> ADC_CR_BOOST_Pos;
    g_adc_selftest_hw.t6_boost_needed  = Sthw_BoostNeeded(Sthw_CcrClockHz(ADC12_COMMON->CCR), rev_v);
    if (accepted_conflict)
    {
        (void)ADC_HAL_Deinit(&s_hal);
    }

    /* The same settings as ADC2 are compatible: must be accepted. */
    cfg.clock_prescaler = ADC_CLOCK_ASYNC_DIV1;
    memset(&s_hal, 0, sizeof(s_hal));
    g_adc_selftest_hw.t6_accepted = ADC_HAL_InitEx(&s_hal, &cfg);
    if (g_adc_selftest_hw.t6_accepted)
    {
        (void)ADC_HAL_Deinit(&s_hal);
    }

    (void)ADC_HAL_Deinit(&s_hal2);

    if (g_adc_selftest_hw.t6_adc2_up &&
        g_adc_selftest_hw.t6_rejected &&
        (g_adc_selftest_hw.t6_ccr_after == g_adc_selftest_hw.t6_ccr_before) &&
        g_adc_selftest_hw.t6_adc2_still_on &&
        g_adc_selftest_hw.t6_accepted)
    {
        g_adc_selftest_hw.pass |= ADC_STHW_PASS_T6;
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
    Line_Begin();
    Line_Pass("HW T2 HAL:", ADC_STHW_PASS_T2);
    Line_Str(" r8=");
    Line_U32(g_adc_selftest_hw.t2_res_code[4]);
    Line_Str(" v16=");
    Line_U32(g_adc_selftest_hw.t2_raw[0]);
    Line_Str(" v12=");
    Line_U32(g_adc_selftest_hw.t2_raw[2]);
    Line_Str(" v8=");
    Line_U32(g_adc_selftest_hw.t2_raw[4]);
    Line_End();

    Line_Begin();
    Line_Pass("HW T3 HAL:", ADC_STHW_PASS_T3);
    Line_Str(" ht=");
    Line_X10(Sthw_Rate_x10(g_adc_selftest_hw.t3_ht, g_adc_selftest_hw.t3_elapsed_ms));
    Line_Str(" tc=");
    Line_X10(Sthw_Rate_x10(g_adc_selftest_hw.t3_tc, g_adc_selftest_hw.t3_elapsed_ms));
    Line_Str(" err=");
    Line_U32(g_adc_selftest_hw.t3_dma_err);
    Line_Str(" ovr=");
    Line_U32(g_adc_selftest_hw.t3_adc_ovr);
    Line_Str(" hovr=");
    Line_U32(g_adc_selftest_hw.t3_half_ovr);
    Line_End();

    Line_Begin();
    Line_Pass("HW T3b HAL:", ADC_STHW_PASS_T3B);
    Line_Str(" vref=");
    Line_U32(g_adc_selftest_hw.t3b_avg[0]);
    Line_Str(" ts=");
    Line_U32(g_adc_selftest_hw.t3b_avg[1]);
    Line_Str(" halves=");
    Line_U32(g_adc_selftest_hw.t3b_halves);
    Line_End();

    Line_Begin();
    Line_Pass("HW T4 HAL:", ADC_STHW_PASS_T4);
    Line_Str(" ovr=");
    Line_U32(g_adc_selftest_hw.t4_adc_ovr);
    Line_Str(" dma=");
    Line_U32(g_adc_selftest_hw.t4_dma_err);
    Line_End();

    Line_Begin();
    Line_Pass("HW T6 HAL:", ADC_STHW_PASS_T6);
    Line_Str(" rej=");
    Line_U32(g_adc_selftest_hw.t6_rejected ? 1U : 0U);
    Line_Str(" ok=");
    Line_U32(g_adc_selftest_hw.t6_accepted ? 1U : 0U);
    Line_Str(" adc2=");
    Line_U32(g_adc_selftest_hw.t6_adc2_still_on ? 1U : 0U);
    Line_Str(" boost=");
    Line_U32(g_adc_selftest_hw.t6_boost);
    Line_Str(" need=");
    Line_U32(g_adc_selftest_hw.t6_boost_needed);
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
    T6_Run(rev_v);

    /* The streams and ADCs above raised their IRQ lines with the NVIC lines
       still disabled. Drop the pending bits so the application does not take a
       stale interrupt when ADC_Acquisition_Init() enables them. */
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
