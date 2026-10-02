/**
  ******************************************************************************
  * @file    tim_selftest.c
  * @brief   03_tim selftest, CPU2 side (preset selftest-tim): the bench of F0
  *          and the TIM15 tests of F1 (tim_bm) and F2 (tim_ll).
  *
  *          CPU2 has no console. It publishes its Rev, build id and step-table
  *          version in the SRAM4 block (Common/Inc/tim_selftest.h), serves the
  *          commands CPU1 writes there, and CPU1 prints and judges the results.
  *
  *          Electrical safety: the only wired pins CPU2 may ever configure are
  *          PE5/PE6 (TIM15_CH1/CH2, AF4, MCU-only lines). bench_claim() judges
  *          every output/AF against the step CPU1 published (S4), and
  *          bench_safe() puts both back in input (S2). pin_cfg() is the only
  *          caller of the GPIO driver for a wired pin.
  ******************************************************************************
  */

#include "tim_selftest.h"
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "tim_bm.h"
#include "tim_ll.h"
#include "stm32h7xx_hal.h"

/* CPU1 sends the next command within seconds; a minute of silence means it
   stopped (a fault, or a bench abort that skipped the DONE). CPU2 then goes to
   S2 and on to the application loop instead of waiting for ever. */
#define TST_CM4_IDLE_TIMEOUT_MS   60000U

#define PE_T15C1   5U
#define PE_T15C2   6U

/* ---- S2 --------------------------------------------------------------------------- */

/* PE5/PE6 in input without pull, AF fields 0. Raw writes inside the cross-core
   lock (taken if it can be; the writes happen regardless, interrupts masked). */
static void bench_safe(void)
{
    GpioBm_Section_t s;
    const bool locked = (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK);
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOEEN;      /* CPU2's bank (RM0399 9.5.10) */
    (void)RCC->AHB4ENR;
    const uint32_t m2 = (3UL << (PE_T15C1 * 2U)) | (3UL << (PE_T15C2 * 2U));
    GPIOE->MODER   = GPIOE->MODER & ~m2;
    GPIOE->PUPDR   = GPIOE->PUPDR & ~m2;
    GPIOE->AFR[0] &= ~((0xFUL << (PE_T15C1 * 4U)) | (0xFUL << (PE_T15C2 * 4U)));
    __DSB();
    __set_PRIMASK(pm);
    if (locked)
    {
        GPIO_BM_SectionExit(&s);
    }
}

static void bench_abort(void)
{
    TST_AREA->m4_aborts++;
    bench_safe();
}

/* ---- S4 ----------------------------------------------------------------------------- */

/* All or nothing, against the step CPU1 published in the block. */
static bool bench_claim(const GPIO_TypeDef *port, uint32_t mask, uint32_t mode, uint32_t af)
{
    const uint32_t step = TST_AREA->step;
    if ((mask & 0xFFFFUL) == 0U)
    {
        return false;
    }
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if (((mask & (1UL << p)) != 0U) && !tst_line_rule(1U, step, port, p, mode, af))
        {
            return false;
        }
    }
    return true;
}

static bool pin_cfg(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af, uint32_t pull)
{
    GpioBm_Status_t st;
    if ((mode == GPIO_BM_MODE_OUTPUT) || (mode == GPIO_BM_MODE_AF))
    {
        if (!bench_claim(port, 1UL << pin, mode, af))
        {
            bench_abort();
            return false;
        }
    }
    if (mode == GPIO_BM_MODE_AF)
    {
        st = GPIO_BM_ConfigAlternateEx(port, pin, af, GPIO_BM_OTYPE_PP, pull, GPIO_BM_SPEED_LOW);
    }
    else if (mode == GPIO_BM_MODE_INPUT)
    {
        st = GPIO_BM_ConfigInput(port, pin, pull);
    }
    else
    {
        st = GPIO_BM_ERR_PARAM;    /* CPU2 never makes a wired pin a GPIO output */
    }
    if (st != GPIO_BM_OK)
    {
        bench_abort();
        return false;
    }
    return true;
}

/* ---- T0b(b) on CPU2 --------------------------------------------------------------- */

typedef struct { GPIO_TypeDef *port; uint16_t mask; uint8_t mode; uint8_t af; } Claim_t;

#define O  GPIO_BM_MODE_OUTPUT
#define A  GPIO_BM_MODE_AF
/* Case k sets bit k of the result when accepted. Expected, by hand, in
   tim_selftest.h: only case 0 in step T0B, none between steps. */
static const Claim_t k_claims[TST_M4_CLAIM_CASES] = {
    { GPIOE, 1U << PE_T15C1, A, 4U },              /*  0 PE5 TIM15_CH1: the accepted control      */
    { GPIOE, 1U << PE_T15C2, A, 4U },              /*  1 PE6: not allowed by step T0B             */
    { GPIOE, 1U << PE_T15C1, A, 1U },              /*  2 PE5 in AF1: wrong AF                      */
    { GPIOE, 1U << PE_T15C1, O, 0U },              /*  3 PE5 as a GPIO output                      */
    { GPIOE, 1U << 4, A, 4U },                     /*  4 PE4 TIM15_CH1N: AD2-only line             */
    { GPIOE, 1U << 3, A, 4U },                     /*  5 PE3 TIM15_BKIN: AD2-only line             */
    { GPIOE, 1U << 2, O, 0U },                     /*  6 PE2 HOST_REQ                              */
    { GPIOE, 1U << 7, O, 0U },                     /*  7 PE7 MCU_ACK                               */
    { GPIOE, 1U << 0, O, 0U },                     /*  8 PE0: never touched (plan R4)              */
    { GPIOE, 1U << 1, O, 0U },                     /*  9 PE1 LD2                                   */
    { GPIOE, 1U << 9, A, 1U },                     /* 10 PE9: CPU1's bus                           */
    { GPIOA, 1U << 8, A, 0U },                     /* 11 PA8 MCO1: CPU1's line                     */
    { GPIOE, (1U << PE_T15C1) | (1U << 4), A, 4U },/* 12 mixed: PE5 with PE4                       */
};
#undef O
#undef A

/* The configuration registers of GPIOA and GPIOE, read raw: a refused claim
   writes none of them (the same oracle as CPU1's port_snap). */
#define SNAP_REGS   12U
typedef struct { uint32_t r[SNAP_REGS]; } PortSnap_t;

static void port_snap(PortSnap_t *s)
{
    const GPIO_TypeDef *const ports[2] = { GPIOA, GPIOE };
    uint32_t i = 0U;
    for (uint32_t k = 0U; k < 2U; k++)
    {
        s->r[i++] = ports[k]->MODER;
        s->r[i++] = ports[k]->OTYPER;
        s->r[i++] = ports[k]->OSPEEDR;
        s->r[i++] = ports[k]->PUPDR;
        s->r[i++] = ports[k]->AFR[0];
        s->r[i++] = ports[k]->AFR[1];
    }
}

/* Bit k set = register k of the snapshot differs. */
static uint32_t port_snap_diff(const PortSnap_t *a, const PortSnap_t *b)
{
    uint32_t diff = 0U;
    for (uint32_t k = 0U; k < SNAP_REGS; k++)
    {
        if (a->r[k] != b->r[k])
        {
            diff |= 1UL << k;
        }
    }
    return diff;
}

static void run_claims(TstClaimResult_t *r)
{
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    uint32_t acc = 0U;
    r->step_seen = TST_AREA->step;
    for (uint32_t k = 0U; k < TST_M4_CLAIM_CASES; k++)
    {
        if (bench_claim(k_claims[k].port, k_claims[k].mask, k_claims[k].mode, k_claims[k].af))
        {
            acc |= 1UL << k;
        }
    }
    r->accepted     = acc;
    r->cases        = TST_M4_CLAIM_CASES;
    port_snap(&after);
    r->regs_changed = port_snap_diff(&before, &after);
}

/* PE5 to AF4 through the claim (allowed in step T0B; PE5 is an MCU-only line,
   the AD2 never drives DIO9), then bench_safe() must bring it back to input. */
static void run_safe(TstSafeResult_t *r)
{
    /* audit O1: the snapshot must see the valid write, exactly in the words it writes */
    PortSnap_t s0;
    PortSnap_t s1;
    port_snap(&s0);
    r->claim_ok = pin_cfg(GPIOE, PE_T15C1, GPIO_BM_MODE_AF, TST_AF_TIM15, GPIO_BM_PULL_NONE) ? 1U : 0U;
    port_snap(&s1);
    r->ctrl_changed = port_snap_diff(&s0, &s1);
    r->af_moder = (GPIOE->MODER >> (PE_T15C1 * 2U)) & 3UL;
    r->af_afr   = (GPIOE->AFR[0] >> (PE_T15C1 * 4U)) & 0xFUL;
    bench_safe();
    r->safe_moder = (GPIOE->MODER >> (PE_T15C1 * 2U)) & 3UL;
    r->safe_afr   = (GPIOE->AFR[0] >> (PE_T15C1 * 4U)) & 0xFUL;
    r->safe_pupdr = (GPIOE->PUPDR >> (PE_T15C1 * 2U)) & 3UL;
    r->pe6_moder  = (GPIOE->MODER >> (PE_T15C2 * 2U)) & 3UL;
}

/* ==========================================================================================
 * F1: tim_bm on CPU2 (TIM15, CPU2's instance; TIM2's enable bit in CPU2's RCC bank for T16).
 * Each command writes m4_res[] (layout: TST_R_* and the comments in tim_selftest.h) and
 * CPU1 judges it. The oracles are the hand tables of Common/Src/tim_selftest_common.c,
 * CPU2's CYCCNT (200 MHz, the timer clock: 1 cycle per tick) and the registers read raw.
 * ========================================================================================== */

#define CM4_CYC_PER_MS   200000U
#define TEST_IRQ_PRIO    6U

static void res_clear(void)
{
    for (uint32_t i = 0U; i < TST_M4_RES_WORDS; i++)
    {
        TST_AREA->m4_res[i] = 0U;
    }
}

static void m4_delay_cycles(uint32_t n)
{
    const uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < n)
    {
    }
}

/* The tier of the command being served (F2): TST_TIER_LL in arg0 selects tim_ll. */
static const TstTier_t *s_tr4 = &k_tst_tier_bm;

static uint32_t t14_tim15(void)
{
    uint32_t idx = 0U;
    uint32_t got = 0U;
    uint32_t exp = 0U;
    return ((s_tr4->deinit(TIM15) == TIM_BM_OK) && tst_reset_ok(TIM15, &idx, &got, &exp)) ? 1U : 0U;
}

static TimBm_TimeBaseConfig_t tb15(uint32_t psc, uint32_t arr)
{
    TimBm_TimeBaseConfig_t c = { TIM15, psc, arr, TIM_BM_COUNT_UP, 0U, false, false, false };
    return c;
}

/* ---- T1 ------------------------------------------------------------------------------ */

typedef struct { TstImg_t im; uint32_t rcc[2]; } M4Snap_t;
static M4Snap_t s_m4_b;
static M4Snap_t s_m4_a;

static void m4_snap(M4Snap_t *s)
{
    tst_img_read(TIM15, &s->im);
    s->rcc[0] = RCC_C1->APB2ENR;
    s->rcc[1] = RCC_C2->APB2ENR;
}

/* Changed words of TIM15 (bit i), RCC words in bits 30/31. */
static uint32_t m4_diff(bool running)
{
    uint32_t d = 0U;
    for (uint32_t i = 0U; i < TST_RI_N; i++)
    {
        if (running && ((i == TST_RI_CNT) || (i == TST_RI_SR)))
        {
            continue;
        }
        if (s_m4_a.im.v[i] != s_m4_b.im.v[i])
        {
            d |= 1UL << i;
        }
    }
    d |= (s_m4_a.rcc[0] != s_m4_b.rcc[0]) ? (1UL << 30) : 0U;
    d |= (s_m4_a.rcc[1] != s_m4_b.rcc[1]) ? (1UL << 31) : 0U;
    return d;
}

static void run_t1(volatile uint32_t *r)
{
    static const uint8_t k_expect[] = {
        TIM_BM_ERR_PARAM,        /* 0 InitTimeBase(TIM15 repetition 0x100: RCR is 8 bits) */
        TIM_BM_ERR_UNSUPPORTED,  /* 1 ConfigOutput(TIM15 channel 3)                       */
        TIM_BM_ERR_UNSUPPORTED,  /* 2 InitTimeBase(TIM15 center 1)                        */
        TIM_BM_ERR_UNSUPPORTED,  /* 3 ConfigSlave(TIM15 ITR4: not wired, table 366)       */
        TIM_BM_ERR_UNSUPPORTED,  /* 4 ConfigMaster(TIM15 OC3REF > MMS 5)                  */
        TIM_BM_ERR_UNSUPPORTED,  /* 5 EnableDmaRequest(TIM15 CC2: none in table 126)      */
        TIM_BM_ERR_UNSUPPORTED,  /* 6 ConfigInput(TIM15 CH2 TI2SEL = 0100, reserved)      */
        TIM_BM_ERR_BUSY,         /* 7 ConfigOutput(TIM15 CH1) with CC1E = 1               */
        TIM_BM_ERR_BUSY,         /* 8 InitTimeBase(TIM15) with CEN = 1                    */
        TIM_BM_ERR_PARAM,        /* 9 EnableClock(TIM1 + 4)                               */
    };
    const uint32_t n = sizeof(k_expect) / sizeof(k_expect[0]);
    uint32_t bad = 0U;
    uint32_t touched = 0U;
    (void)TIM_BM_EnableClock(TIM15);
    for (uint32_t k = 0U; k < n; k++)
    {
        TimBm_TimeBaseConfig_t tb = tb15(0U, 999U);
        TimBm_OcConfig_t oc = { TIM15, 1U, TIM_BM_OC_PWM1, 100U, false, true };
        TimBm_IcConfig_t ic = { TIM15, 2U, TIM_BM_IC_DIRECT, TIM_BM_EDGE_RISING, 1U, 0U, 4U };
        TimBm_Status_t st = TIM_BM_OK;
        (void)TIM_BM_Deinit(TIM15);
        switch (k)
        {
        case 7:
            (void)TIM_BM_ConfigOutput(&oc);
            (void)TIM_BM_OutputEnable(TIM15, TIM_BM_CH(1));
            break;
        case 8:
            (void)TIM_BM_InitTimeBase(&tb);
            (void)TIM_BM_Start(TIM15);
            break;
        default:
            break;
        }
        m4_snap(&s_m4_b);
        switch (k)
        {
        case 0: tb.repetition = 0x100U; st = TIM_BM_InitTimeBase(&tb); break;
        case 1: oc.channel = 3U; st = TIM_BM_ConfigOutput(&oc); break;
        case 2: tb.count_mode = TIM_BM_COUNT_CENTER1; st = TIM_BM_InitTimeBase(&tb); break;
        case 3: st = TIM_BM_ConfigSlave(TIM15, TIM_BM_SMS_TRIGGER, TIM_BM_TRG_ITR(4)); break;
        case 4: st = TIM_BM_ConfigMaster(TIM15, TIM_BM_MMS_OC3REF, false); break;
        case 5: st = TIM_BM_EnableDmaRequest(TIM15, TIM_BM_EV_CC2); break;
        case 6: st = TIM_BM_ConfigInput(&ic); break;
        case 7: st = TIM_BM_ConfigOutput(&oc); break;
        case 8: st = TIM_BM_InitTimeBase(&tb); break;
        default: st = TIM_BM_EnableClock((TIM_TypeDef *)(TIM1_BASE + 4U)); break;
        }
        m4_snap(&s_m4_a);
        r[TST_R_T1_STATUS0 + k] = (uint32_t)st;
        if (st != (TimBm_Status_t)k_expect[k])
        {
            bad |= 1UL << k;
        }
        if (m4_diff(k == 8U) != 0U)
        {
            touched |= 1UL << k;
        }
        (void)TIM_BM_Stop(TIM15);
    }
    (void)TIM_BM_Deinit(TIM15);
    /* O1: the valid call changes exactly CCMR1 and CCR1 */
    TimBm_OcConfig_t oc = { TIM15, 1U, TIM_BM_OC_PWM1, 100U, false, true };
    m4_snap(&s_m4_b);
    r[TST_R_T1_CTRL_ST] = (uint32_t)TIM_BM_ConfigOutput(&oc);
    m4_snap(&s_m4_a);
    r[TST_R_T1_CTRL_DIFF] = m4_diff(false);
    r[TST_R_T1_CTRL_EXP] = (1UL << TST_RI_CCMR1) | (1UL << TST_RI_CCR1);
    r[TST_R_T1_CASES] = n;
    r[TST_R_T1_BAD] = bad;
    r[TST_R_T1_TOUCHED] = touched;
    (void)TIM_BM_Deinit(TIM15);
}

/* F2: the TIM15 refusals with LL constants (tim_ll), the same snapshot and layout. */
static void run_t1_ll(volatile uint32_t *r)
{
    static const uint8_t k_expect[] = {
        TIM_BM_ERR_PARAM,        /*  0 InitTimeBase(TIM15 repetition 0x100: RCR is 8 bits)          */
        TIM_BM_ERR_UNSUPPORTED,  /*  1 ConfigOutput(TIM15 LL_TIM_CHANNEL_CH3)                       */
        TIM_BM_ERR_UNSUPPORTED,  /*  2 InitTimeBase(TIM15 LL_TIM_COUNTERMODE_CENTER_DOWN)           */
        TIM_BM_ERR_UNSUPPORTED,  /*  3 ConfigSlave(TIM15 LL_TIM_TS_ITR4: not wired, table 366)      */
        TIM_BM_ERR_UNSUPPORTED,  /*  4 ConfigMaster(TIM15 LL_TIM_TRGO_OC3REF > MMS 5)               */
        TIM_BM_ERR_UNSUPPORTED,  /*  5 EnableDmaRequest(TIM15 LL_TIM_DIER_CC2IE: none in table 126) */
        TIM_BM_ERR_UNSUPPORTED,  /*  6 ConfigInput(TIM15 CH2 remap TI2SEL = 0100, reserved)         */
        TIM_BM_ERR_BUSY,         /*  7 ConfigOutput(TIM15 CH1) with CC1E = 1                        */
        TIM_BM_ERR_BUSY,         /*  8 InitTimeBase(TIM15) with CEN = 1                             */
        TIM_BM_ERR_PARAM,        /*  9 EnableClock(TIM1 + 4)                                        */
        TIM_BM_ERR_PARAM,        /* 10 ConfigOutput(TIM15 LL_TIM_CHANNEL_CH1N)                      */
        TIM_BM_ERR_PARAM,        /* 11 InitTimeBase(TIM15 counter mode 0x0008: no LL constant)      */
    };
    const uint32_t n = sizeof(k_expect) / sizeof(k_expect[0]);
    uint32_t bad = 0U;
    uint32_t touched = 0U;
    (void)TIM_LL_EnableClock(TIM15);
    for (uint32_t k = 0U; k < n; k++)
    {
        TimLl_TimeBaseConfig_t tb = { TIM15, 0U, LL_TIM_COUNTERMODE_UP, 999U, 0U, false, false, false };
        TimLl_OcConfig_t oc = { TIM15, LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_PWM1, 100U, false, true };
        TimLl_IcConfig_t ic = { TIM15, LL_TIM_CHANNEL_CH2, LL_TIM_ACTIVEINPUT_DIRECTTI, LL_TIM_IC_POLARITY_RISING,
                                LL_TIM_ICPSC_DIV1, LL_TIM_IC_FILTER_FDIV1, TIM_TISEL_TI2SEL_2 };   /* TI2SEL 0100 */
        TimBm_Status_t st = TIM_BM_OK;
        (void)TIM_LL_Deinit(TIM15);
        if (k == 7U)
        {
            (void)TIM_LL_ConfigOutput(&oc);
            (void)TIM_LL_OutputEnable(TIM15, LL_TIM_CHANNEL_CH1);
        }
        else if (k == 8U)
        {
            (void)TIM_LL_InitTimeBase(&tb);
            (void)TIM_LL_Start(TIM15);
        }
        m4_snap(&s_m4_b);
        switch (k)
        {
        case 0:  tb.repetition = 0x100U; st = TIM_LL_InitTimeBase(&tb); break;
        case 1:  oc.channel = LL_TIM_CHANNEL_CH3; st = TIM_LL_ConfigOutput(&oc); break;
        case 2:  tb.counter_mode = LL_TIM_COUNTERMODE_CENTER_DOWN; st = TIM_LL_InitTimeBase(&tb); break;
        case 3:  st = TIM_LL_ConfigSlave(TIM15, LL_TIM_SLAVEMODE_TRIGGER, LL_TIM_TS_ITR4); break;
        case 4:  st = TIM_LL_ConfigMaster(TIM15, LL_TIM_TRGO_OC3REF, false); break;
        case 5:  st = TIM_LL_EnableDmaRequest(TIM15, LL_TIM_DIER_CC2IE); break;
        case 6:  st = TIM_LL_ConfigInput(&ic); break;
        case 7:  st = TIM_LL_ConfigOutput(&oc); break;
        case 8:  st = TIM_LL_InitTimeBase(&tb); break;
        case 9:  st = TIM_LL_EnableClock((TIM_TypeDef *)(TIM1_BASE + 4U)); break;
        case 10: oc.channel = LL_TIM_CHANNEL_CH1N; st = TIM_LL_ConfigOutput(&oc); break;
        default: tb.counter_mode = 0x0008U; st = TIM_LL_InitTimeBase(&tb); break;
        }
        m4_snap(&s_m4_a);
        r[TST_R_T1_STATUS0 + k] = (uint32_t)st;
        if (st != (TimBm_Status_t)k_expect[k])
        {
            bad |= 1UL << k;
        }
        if (m4_diff(k == 8U) != 0U)
        {
            touched |= 1UL << k;
        }
        (void)TIM_LL_Stop(TIM15);
    }
    (void)TIM_LL_Deinit(TIM15);
    /* O1: the valid LL call changes exactly CCMR1 and CCR1 */
    const TimLl_OcConfig_t oc = { TIM15, LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_PWM1, 100U, false, true };
    m4_snap(&s_m4_b);
    r[TST_R_T1_CTRL_ST] = (uint32_t)TIM_LL_ConfigOutput(&oc);
    m4_snap(&s_m4_a);
    r[TST_R_T1_CTRL_DIFF] = m4_diff(false);
    r[TST_R_T1_CTRL_EXP] = (1UL << TST_RI_CCMR1) | (1UL << TST_RI_CCR1);
    r[TST_R_T1_CASES] = n;
    r[TST_R_T1_BAD] = bad;
    r[TST_R_T1_TOUCHED] = touched;
    (void)TIM_LL_Deinit(TIM15);
}

/* ---- T2 ------------------------------------------------------------------------------ */

static void run_t2(volatile uint32_t *r)
{
    static TstT2Result_t res;
    (void)TIM_BM_EnableClock(TIM15);
    tst_t2_run(TIM15, s_tr4, NULL, &res);
    r[TST_R_T2_CONFIGS] = res.configs;
    r[TST_R_T2_MISMATCH] = res.mismatches;
    r[TST_R_T2_FIRST_CFG] = res.first_cfg;
    r[TST_R_T2_FIRST_REG] = res.first_idx;
    r[TST_R_T2_FIRST_GOT] = res.first_got;
    r[TST_R_T2_FIRST_EXP] = res.first_exp;
    r[TST_R_T2_CTRL] = res.ctrl_detected;
    r[TST_R_T2_CALLS_BAD] = res.calls_bad;
    r[TST_R_T2_PAR_CONFIGS] = res.par_configs;
    r[TST_R_T2_PAR_MISMATCH] = res.par_mismatches;
    r[TST_R_T2_PAR_FIRST_CFG] = res.par_first_cfg;
    r[TST_R_T2_PAR_FIRST_REG] = res.par_first_idx;
    r[TST_R_T2_PAR_FIRST_LL] = res.par_first_tier;
    r[TST_R_T2_PAR_FIRST_BM] = res.par_first_bm;
    r[TST_R_T2_PAR_CTRL] = res.par_ctrl;
}

/* ---- T5 ------------------------------------------------------------------------------ */

static void t5_15(const TimBm_TimeBaseConfig_t *c, uint32_t ticks, volatile uint32_t *out)
{
    const uint32_t window = 100U * CM4_CYC_PER_MS;
    uint32_t n = 0U;
    uint32_t first = 0U;
    uint32_t last = 0U;
    (void)s_tr4->init_tb(c);
    (void)s_tr4->start(TIM15);
    const uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < (window + (8U * ticks) + CM4_CYC_PER_MS))
    {
        if (s_tr4->take_flags(TIM15, TIM_BM_EV_UPDATE) != 0U)
        {
            const uint32_t now = DWT->CYCCNT;
            if (n == 0U)
            {
                first = now;
            }
            last = now;
            n++;
            if ((last - first) >= window)
            {
                break;
            }
        }
    }
    (void)s_tr4->stop(TIM15);
    out[0] = ticks;
    out[1] = n;
    out[2] = last - first;
}

static void run_t5(volatile uint32_t *r)
{
    (void)TIM_BM_EnableClock(TIM15);
    TimBm_TimeBaseConfig_t a = tb15(1U, 499U);          /* (a) 1000 ticks */
    t5_15(&a, 1000U, &r[0]);
    (void)s_tr4->deinit(TIM15);
    TimBm_TimeBaseConfig_t b = tb15(1U, 499U);          /* (b) RCR = 3: 4000 ticks */
    b.repetition = 3U;
    t5_15(&b, 4000U, &r[3]);
    uint32_t hz = 0U;
    const uint32_t hclk = HAL_RCC_GetHCLKFreq();
    r[7] = (uint32_t)TIM_BM_GetKernelClockHz(TIM15, hclk, &hz);
    r[6] = hz;
    r[9] = hclk;
    r[8] = t14_tim15();
}

/* ---- T6: TIM15 PWM on PE5/PE6 (step T6_TIM15) -------------------------------------- */

static void run_t6(uint32_t arg_in, volatile uint32_t *r)
{
    s_tr4 = ((arg_in & TST_T6_LL) != 0U) ? &k_tst_tier_ll : &k_tst_tier_bm;
    const uint32_t arg = arg_in & ~TST_T6_LL;
    (void)TIM_BM_EnableClock(TIM15);
    if (arg == TST_T6_OFF)
    {
        (void)pin_cfg(GPIOE, PE_T15C1, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE);
        (void)pin_cfg(GPIOE, PE_T15C2, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE);
        r[1] = t14_tim15();
        r[0] = 0U;
        return;
    }
    if (!pin_cfg(GPIOE, PE_T15C1, GPIO_BM_MODE_AF, TST_AF_TIM15, GPIO_BM_PULL_DOWN) ||
        !pin_cfg(GPIOE, PE_T15C2, GPIO_BM_MODE_AF, TST_AF_TIM15, GPIO_BM_PULL_DOWN))
    {
        r[0] = 98U;
        return;
    }
    if (arg == TST_T6_CONTROL)
    {
        /* both channels configured, CC1E = CC2E = 0 (and so MOE = 0): the pins are not
           driven by the timer and follow the pull-down */
        TimBm_Status_t st = s_tr4->deinit(TIM15);
        const TimBm_TimeBaseConfig_t c = { TIM15, 0U, 199U, TIM_BM_COUNT_UP, 0U, true, false, false };
        const TimBm_OcConfig_t o1 = { TIM15, 1U, TIM_BM_OC_PWM1, 100U, false, true };
        const TimBm_OcConfig_t o2 = { TIM15, 2U, TIM_BM_OC_PWM1, 100U, false, true };
        if (st == TIM_BM_OK) { st = s_tr4->init_tb(&c); }
        if (st == TIM_BM_OK) { st = s_tr4->config_output(&o1); }
        if (st == TIM_BM_OK) { st = s_tr4->config_output(&o2); }
        if (st == TIM_BM_OK) { st = s_tr4->start(TIM15); }
        r[0] = (uint32_t)st;
        return;
    }
    const uint32_t ch = arg >> 8;
    const uint32_t k = arg & 0xFFU;
    if ((ch < 1U) || (ch > 2U) || (k >= (TST_T6_POINTS - 1U)))
    {
        r[0] = 97U;
        return;
    }
    r[0] = (uint32_t)tst_t6_apply(s_tr4, TIM15, ch, &k_tst_t6[k]);
}

/* ---- T10(d): TIM15 captures TRC = ITR0 (TIM1 TRGO, run by CPU1) ---------------------- */

static void run_t10d_arm(volatile uint32_t *r)
{
    (void)TIM_BM_EnableClock(TIM15);
    const TimBm_TimeBaseConfig_t c = tb15(0U, 0xFFFFU);
    const TimBm_IcConfig_t ic = { TIM15, 1U, TIM_BM_IC_TRC, TIM_BM_EDGE_RISING, 1U, 0U, 0U };
    TimBm_Status_t st = TIM_BM_Deinit(TIM15);
    if (st == TIM_BM_OK) { st = TIM_BM_InitTimeBase(&c); }
    if (st == TIM_BM_OK) { st = TIM_BM_ConfigSlave(TIM15, TIM_BM_SMS_DISABLED, TIM_BM_TRG_ITR(0)); }
    if (st == TIM_BM_OK) { st = TIM_BM_ConfigInput(&ic); }
    if (st == TIM_BM_OK) { st = TIM_BM_CaptureEnable(TIM15, TIM_BM_CH(1)); }
    if (st == TIM_BM_OK) { st = TIM_BM_Start(TIM15); }
    r[0] = (uint32_t)st;
}

static void run_t10d_read(volatile uint32_t *r)
{
    uint32_t n = 0U;
    (void)TIM_BM_TakeFlags(TIM15, TIM_BM_EV_CC1);
    const uint32_t t0 = DWT->CYCCNT;
    while ((n < 4U) && ((DWT->CYCCNT - t0) < CM4_CYC_PER_MS))
    {
        if (TIM_BM_TakeFlags(TIM15, TIM_BM_EV_CC1) != 0U)
        {
            r[2U + n] = TIM_BM_ReadCapture(TIM15, 1U);
            n++;
        }
    }
    (void)TIM_BM_Stop(TIM15);
    r[1] = n;
    r[6] = t14_tim15();
}

/* ---- T12: TIM15 interrupts ----------------------------------------------------------- */

static volatile uint32_t s_t15_irq;
static volatile uint32_t s_t15_spur;

void TIM15_IRQHandler(void)
{
    if (s_tr4->take_flags(TIM15, TIM_BM_EV_UPDATE) != 0U)
    {
        s_t15_irq++;
    }
    else
    {
        s_t15_spur++;
    }
}

static void irq15(bool on)
{
    if (on)
    {
        NVIC_SetPriority(TIM15_IRQn, TEST_IRQ_PRIO);
        NVIC_ClearPendingIRQ(TIM15_IRQn);
        NVIC_EnableIRQ(TIM15_IRQn);
    }
    else
    {
        NVIC_DisableIRQ(TIM15_IRQn);
        NVIC_ClearPendingIRQ(TIM15_IRQn);
    }
}

static void run_t12(volatile uint32_t *r)
{
    (void)TIM_BM_EnableClock(TIM15);
    const TimBm_TimeBaseConfig_t c = tb15(199U, 99U);    /* 10 kHz */
    /* (a) */
    (void)s_tr4->deinit(TIM15);
    s_t15_irq = 0U;
    s_t15_spur = 0U;
    (void)s_tr4->init_tb(&c);
    (void)s_tr4->enable_irq(TIM15, TIM_BM_EV_UPDATE);
    irq15(true);
    const uint32_t t0 = DWT->CYCCNT;
    (void)s_tr4->start(TIM15);
    m4_delay_cycles(100U * CM4_CYC_PER_MS);
    (void)s_tr4->stop(TIM15);
    r[1] = DWT->CYCCNT - t0;
    irq15(false);
    r[0] = s_t15_irq;
    /* (b) no spurious IRQ after InitTimeBase + EnableIrq without Start */
    (void)s_tr4->deinit(TIM15);
    s_t15_irq = 0U;
    (void)s_tr4->init_tb(&c);
    (void)s_tr4->enable_irq(TIM15, TIM_BM_EV_UPDATE);
    irq15(true);
    m4_delay_cycles(10U * CM4_CYC_PER_MS);
    irq15(false);
    r[2] = s_t15_irq;
    /* control: by hand, UG with URS = 0, UIF left set */
    (void)s_tr4->deinit(TIM15);
    s_t15_irq = 0U;
    TIM15->PSC = 199U;
    TIM15->ARR = 99U;
    TIM15->EGR = TIM_EGR_UG;
    (void)s_tr4->enable_irq(TIM15, TIM_BM_EV_UPDATE);
    irq15(true);
    m4_delay_cycles(10U * CM4_CYC_PER_MS);
    irq15(false);
    r[3] = s_t15_irq;
    /* UIE = 0 with the counter running */
    (void)s_tr4->deinit(TIM15);
    s_t15_irq = 0U;
    (void)s_tr4->init_tb(&c);
    irq15(true);
    (void)s_tr4->start(TIM15);
    m4_delay_cycles(10U * CM4_CYC_PER_MS);
    (void)s_tr4->stop(TIM15);
    irq15(false);
    r[4] = s_t15_irq + s_t15_spur;
    r[5] = t14_tim15();
    IRQn_Type irq = (IRQn_Type)0;
    const TimBm_Status_t st = (s_tr4 == &k_tst_tier_ll) ? TIM_LL_GetIRQn(TIM15, LL_TIM_DIER_UIE, &irq)
                                                     : TIM_BM_GetIRQn(TIM15, TIM_BM_EV_UPDATE, &irq);
    r[6] = ((st == TIM_BM_OK) && (irq == TIM15_IRQn)) ? 1U : 0U;
}

/* ---- T16: TIM2's bit in CPU2's RCC bank ---------------------------------------------- */

static void run_t16(uint32_t arg, volatile uint32_t *r)
{
    TimBm_Status_t st = TIM_BM_OK;
    if (arg == 1U)
    {
        st = TIM_BM_EnableClock(TIM2);
        r[3] = RCC_C2->APB1LENR;
    }
    if (st == TIM_BM_OK)
    {
        st = TIM_BM_DisableClock(TIM2);
    }
    r[0] = (uint32_t)st;
    r[1] = RCC_C1->APB1LENR;
    r[2] = RCC_C2->APB1LENR;
}

/* ---- B1 ------------------------------------------------------------------------------ */

static void run_b1_ll(volatile uint32_t *r)
{
    (void)TIM_LL_EnableClock(TIM15);
    (void)TIM_LL_Deinit(TIM15);
    const TimLl_TimeBaseConfig_t c = { TIM15, 0U, LL_TIM_COUNTERMODE_UP, 999U, 0U, true, false, false };
    const TimLl_OcConfig_t oc = { TIM15, LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_PWM1, 100U, false, true };
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    uint32_t c0 = DWT->CYCCNT;
    (void)TIM_LL_InitTimeBase(&c);
    r[3] = DWT->CYCCNT - c0;
    c0 = DWT->CYCCNT;
    (void)TIM_LL_ConfigOutput(&oc);
    r[2] = DWT->CYCCNT - c0;
    c0 = DWT->CYCCNT;
    (void)TIM_LL_OutputEnable(TIM15, LL_TIM_CHANNEL_CH1);
    r[1] = DWT->CYCCNT - c0;
    c0 = DWT->CYCCNT;
    (void)TIM_LL_TakeFlags(TIM15, LL_TIM_DIER_UIE);
    r[0] = DWT->CYCCNT - c0;
    __set_PRIMASK(pm);
    r[4] = t14_tim15();
}

static void run_b1(volatile uint32_t *r)
{
    (void)TIM_BM_EnableClock(TIM15);
    (void)TIM_BM_Deinit(TIM15);
    const TimBm_TimeBaseConfig_t c = { TIM15, 0U, 999U, TIM_BM_COUNT_UP, 0U, true, false, false };
    const TimBm_OcConfig_t oc = { TIM15, 1U, TIM_BM_OC_PWM1, 100U, false, true };
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    uint32_t c0 = DWT->CYCCNT;
    (void)TIM_BM_InitTimeBase(&c);
    r[3] = DWT->CYCCNT - c0;
    c0 = DWT->CYCCNT;
    (void)TIM_BM_ConfigOutput(&oc);
    r[2] = DWT->CYCCNT - c0;
    c0 = DWT->CYCCNT;
    (void)TIM_BM_OutputEnable(TIM15, TIM_BM_CH(1));
    r[1] = DWT->CYCCNT - c0;
    c0 = DWT->CYCCNT;
    (void)TIM_BM_TakeFlags(TIM15, TIM_BM_EV_UPDATE);
    r[0] = DWT->CYCCNT - c0;
    __set_PRIMASK(pm);
    r[4] = t14_tim15();
}

static const TstTier_t *tier_of(uint32_t a0)
{
    return (a0 == TST_TIER_LL) ? &k_tst_tier_ll : &k_tst_tier_bm;
}

static void copy_str(volatile char *dst, uint32_t n, const char *src)
{
    uint32_t i = 0U;
    for (; (i + 1U < n) && (src[i] != '\0'); i++)
    {
        dst[i] = src[i];
    }
    for (; i < n; i++)
    {
        dst[i] = '\0';
    }
}

void TIM_Selftest_CM4_Run(void)
{
    TstArea_t *a = TST_AREA;

    /* CPU1 opened the session before releasing this core. */
    if (a->magic != TST_AREA_MAGIC)
    {
        return;
    }
    const uint32_t session = a->session;
    bench_safe();
    copy_str(a->m4_rev, sizeof(a->m4_rev), FW_GIT_REV);
    copy_str(a->m4_build_id, sizeof(a->m4_build_id), FW_BUILD_ID);
    a->m4_version = TIM_BENCH_VERSION;
    uint32_t last_seq = a->cmd_seq;
    __DMB();
    a->m4_ready = session;

    for (;;)
    {
        const uint32_t t0 = HAL_GetTick();
        while (a->cmd_seq == last_seq)
        {
            if ((HAL_GetTick() - t0) > TST_CM4_IDLE_TIMEOUT_MS)
            {
                a->m4_timeout = 1U;
                bench_safe();
                return;
            }
        }
        __DMB();
        last_seq = a->cmd_seq;
        const uint32_t cmd = a->cmd;
        const uint32_t a0  = a->arg0;

        switch (cmd)
        {
        case TST_CMD_CLAIMS:
            if (a0 < 2U)
            {
                run_claims(&a->m4_claims[a0]);
            }
            break;
        case TST_CMD_SAFE:
            run_safe(&a->m4_safe);
            break;
        case TST_CMD_T1:
            res_clear();
            if (a0 == TST_TIER_LL) { run_t1_ll(a->m4_res); } else { run_t1(a->m4_res); }
            break;
        case TST_CMD_T2:        res_clear(); s_tr4 = tier_of(a0); run_t2(a->m4_res); break;
        case TST_CMD_T5:        res_clear(); s_tr4 = tier_of(a0); run_t5(a->m4_res); break;
        case TST_CMD_T6:        res_clear(); run_t6(a0, a->m4_res); break;
        case TST_CMD_T10D_ARM:  res_clear(); run_t10d_arm(a->m4_res); break;
        case TST_CMD_T10D_READ: run_t10d_read(a->m4_res); break;
        case TST_CMD_T12:       res_clear(); s_tr4 = tier_of(a0); run_t12(a->m4_res); break;
        case TST_CMD_T16:       res_clear(); run_t16(a0, a->m4_res); break;
        case TST_CMD_B1:
            res_clear();
            if (a0 == TST_TIER_LL) { run_b1_ll(a->m4_res); } else { run_b1(a->m4_res); }
            break;
        case TST_CMD_DONE:
            bench_safe();
            __DMB();
            a->ack_seq = last_seq;
            return;
        default:
            break;
        }
        s_tr4 = &k_tst_tier_bm;                /* every command starts on tim_bm */
        __DMB();
        a->ack_seq = last_seq;
    }
}
