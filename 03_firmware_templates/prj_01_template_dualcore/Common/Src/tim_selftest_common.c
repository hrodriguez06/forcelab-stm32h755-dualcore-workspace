/**
  ******************************************************************************
  * @file    tim_selftest_common.c
  * @brief   03_tim selftest, code both cores compile: the bench step table, the
  *          line rules of bench_claim() and the F1 oracles typed by hand from
  *          RM0399 (Common/Inc/tim_selftest.h).
  *
  *          The step table, the line rules and the oracle tables are pure: they
  *          touch no register. Each core's App/Src/tim_selftest.c owns its writes,
  *          and calls tst_line_rule() from its bench_claim() before any pin becomes
  *          an output or AF. The two exceptions read or write the registers of
  *          the timer they are given, and each core passes only the instances it
  *          owns: tst_img_read()/tst_reset_ok() (raw reads, T14) and tst_t2_run()
  *          (the T2 matrix, through tim_bm).
  ******************************************************************************
  */

#include "tim_selftest.h"
#include "gpio_bm.h"

#include <string.h>

/* Bench lines (00_gpio README section 8, 03_tim plan "Uso de las lineas"). */
#define PE_REQ     2U       /* DIO12 HOST_REQ, AD2 only                         */
#define PE_DIO8    3U       /* DIO8, AD2 only (TIM15_BKIN in AF4: never)        */
#define PE_DIO10   4U       /* DIO10, AD2 only (TIM15_CH1N in AF4: never)       */
#define PE_T15C1   5U       /* DIO9, MCU only: TIM15_CH1 (AF4), CPU2            */
#define PE_T15C2   6U       /* DIO11, MCU only: TIM15_CH2 (AF4), CPU2           */
#define PE_ACK     7U       /* DIO13 MCU_ACK, MCU only, owned by bench_safe/ack */
#define PA_MCO     8U       /* DIO14, MCU only: MCO1 (AF0), CPU1                */

#define PE_BUS_MASK      0xFF00U                                       /* PE8..PE15 = DIO0..DIO7 */
#define PE_BUS_TIM1      ((1U << 9) | (1U << 11) | (1U << 13) | (1U << 14))  /* TIM1_CH1..CH4 */
#define PE_BUS_CAPTURE   ((1U << 9) | (1U << 11))                     /* S12: AF while the AD2 drives */

#define E(n)   (1U << (n))

/* The single table of permissions (S4). Plan "Banco de pruebas": PE8, PE10,
   PE12 and PE15 are never in AF; PE9/PE11 in AF1 with the AD2 driving only
   under S12; PE5/PE6 in AF4 only on CPU2; PA8 in AF0 only on CPU1 (TREF_A,
   TREF_B), and in AF1 (TIM1_CH1, TIM1 stopped) only for the control of
   TREF_A. PA8 is an MCU-only line: the AD2 never drives DIO14. */
const TstStep_t k_tst_steps[] = {
    /* name            bus            out_pe        af_pe            af_pa       af1_pa     */
    { "T0A",           TST_BUS_NONE,  0U,           0U,              0U,         0U         },
    { "TREF_A",        TST_BUS_NONE,  0U,           0U,              E(PA_MCO),  E(PA_MCO)  },  /* control: PA8 AF1 */
    { "T0B",           TST_BUS_NONE,  0U,           E(PE_T15C1),     E(PA_MCO),  0U         },  /* the accepted controls */
    { "T0B_S5NEG",     TST_BUS_MCU,   PE_BUS_MASK,  0U,              0U,         0U         },  /* 00_gpio T0b(c) */
    { "T0B_TIMEOUT",   TST_BUS_NONE,  0U,           0U,              0U,         0U         },
    { "T0B_DOUT",      TST_BUS_AD2,   0U,           0U,              0U,         0U         },  /* S11: MCU inputs only */
    { "T0B_S12",       TST_BUS_AD2,   0U,           PE_BUS_CAPTURE,  0U,         0U         },  /* S12 */
    /* F1 (protocol v3). R3-2: in the T6 and T8 steps PA8 carries MCO1 = HSE/8 (AF0)
       for the AD2 to capture next to the measured lines; T9 shows it to the AD2
       while TIM17 captures it internally (TI1SEL = MCO1). */
    { "T6_TIM1",       TST_BUS_MCU,   0U,           PE_BUS_TIM1,     E(PA_MCO),  0U         },  /* PWM, CPU1 */
    { "T6_TIM15",      TST_BUS_NONE,  0U,           E(PE_T15C1) | E(PE_T15C2), E(PA_MCO), 0U },  /* PWM, CPU2 */
    { "T8",            TST_BUS_AD2,   0U,           PE_BUS_CAPTURE,  E(PA_MCO),  0U         },  /* capture, S11 + S12 */
    { "T9",            TST_BUS_NONE,  0U,           0U,              E(PA_MCO),  0U         },  /* TISEL = MCO1 */
    { "T11",           TST_BUS_MCU,   0U,           PE_BUS_CAPTURE,  0U,         0U         },  /* one-pulse, CH2 reference */
    { "T13A",          TST_BUS_MCU,   0U,           E(9),            0U,         0U         },  /* DMA -> CCR1 */
    { "T13B",          TST_BUS_AD2,   0U,           E(9),            0U,         0U         },  /* CCR1 -> DMA, S12 */
    { "TREF_B",        TST_BUS_NONE,  0U,           0U,              E(PA_MCO),  0U         },  /* no control: AF0 only */
    { "DONE",          TST_BUS_NONE,  0U,           0U,              0U,         0U         },
};

const uint32_t k_tst_step_count = (uint32_t)(sizeof(k_tst_steps) / sizeof(k_tst_steps[0]));

uint32_t tst_step_index(const char *name)
{
    for (uint32_t i = 0U; i < k_tst_step_count; i++)
    {
        if (strcmp(k_tst_steps[i].name, name) == 0)
        {
            return i;
        }
    }
    return TST_STEP_NONE;
}

static bool rule_gpioe(uint32_t core, const TstStep_t *s, uint32_t pin, uint32_t mode, uint32_t af)
{
    const uint16_t bit = (uint16_t)(1U << pin);

    if ((pin <= 1U) || (pin == PE_REQ) || (pin == PE_DIO8) || (pin == PE_DIO10) || (pin == PE_ACK))
    {
        return false;   /* PE0 (R4), PE1 (LD2), the AD2-only lines (PE4 in AF4 = TIM15_CH1N), MCU_ACK */
    }
    if ((pin == PE_T15C1) || (pin == PE_T15C2))
    {
        /* MCU only, TIM15 of CPU2: AF4 and nothing else, never from CPU1. */
        return (core == 1U) && (mode == GPIO_BM_MODE_AF) && (af == TST_AF_TIM15) && ((s->af_pe & bit) != 0U);
    }
    /* The bus, PE8..PE15: CPU1 only. */
    if (core != 0U)
    {
        return false;
    }
    if (mode == GPIO_BM_MODE_OUTPUT)
    {
        return (s->bus == (uint8_t)TST_BUS_MCU) && ((s->out_pe & bit) != 0U);
    }
    /* AF: only TIM1_CH1..CH4 in AF1 (PE8, PE10, PE12, PE15 never). */
    if ((af != TST_AF_TIM1) || ((PE_BUS_TIM1 & bit) == 0U) || ((s->af_pe & bit) == 0U))
    {
        return false;
    }
    if (s->bus == (uint8_t)TST_BUS_MCU)
    {
        return true;
    }
    /* "bus = AD2": only the two capture inputs, and only as an S12 candidate. */
    return (s->bus == (uint8_t)TST_BUS_AD2) && ((PE_BUS_CAPTURE & bit) != 0U);
}

bool tst_line_rule(uint32_t core, uint32_t step, const GPIO_TypeDef *port, uint32_t pin,
                   uint32_t mode, uint32_t af)
{
    if ((pin > 15U) || ((mode != GPIO_BM_MODE_OUTPUT) && (mode != GPIO_BM_MODE_AF)))
    {
        return false;
    }
    if ((port == GPIOC) && (pin == 13U))
    {
        return false;   /* PC13 (user button): never touched by this selftest (R4) */
    }
    if ((port == GPIOA) && ((pin == 13U) || (pin == 14U)))
    {
        return false;   /* SWD */
    }
    if ((port == GPIOF) && ((pin == 14U) || (pin == 15U)))
    {
        return false;   /* scope lines with their 22k loads: not used by the TIM bench */
    }
    const bool wired = (port == GPIOE) || ((port == GPIOA) && (pin == PA_MCO));
    if (!wired)
    {
        return true;    /* nothing outside drives an unwired pin */
    }
    if (step >= k_tst_step_count)
    {
        return false;   /* between steps nothing wired may be driven */
    }
    const TstStep_t *s = &k_tst_steps[step];
    if (port == GPIOA)
    {
        /* PA8, CPU1 only: MCO1 in AF0, or TIM1_CH1 in AF1 for the control of TREF_A. */
        if ((core != 0U) || (mode != GPIO_BM_MODE_AF))
        {
            return false;
        }
        return ((af == TST_AF_MCO1) && ((s->af_pa & (1U << PA_MCO)) != 0U)) ||
               ((af == TST_AF_TIM1) && ((s->af1_pa & (1U << PA_MCO)) != 0U));
    }
    return rule_gpioe(core, s, pin, mode, af);
}

/* ==========================================================================================
 * F1 oracles (03_tim plan "Plan de pruebas"): typed by hand from RM0399 rev 4, never taken
 * from tim_bm.c. The register maps are tables 355/356 (TIM1/TIM8), 360 (TIM2-5), 363
 * (TIM12), 365 (TIM13/14), 368 (TIM15), 370 (TIM16/17) and 371 (TIM6/7).
 * The T2 runner below touches the registers of the instance it is given: each core
 * calls it only on the instances it owns.
 * ========================================================================================== */

#define R(i)   (1UL << (i))

int32_t tst_kind(const TIM_TypeDef *tim)
{
    if ((tim == TIM1) || (tim == TIM8))   { return TST_K_ADV; }
    if ((tim == TIM2) || (tim == TIM5))   { return TST_K_GP32; }
    if ((tim == TIM3) || (tim == TIM4))   { return TST_K_GP16; }
    if ((tim == TIM6) || (tim == TIM7))   { return TST_K_BASIC; }
    if (tim == TIM12)                     { return TST_K_T12; }
    if ((tim == TIM13) || (tim == TIM14)) { return TST_K_T1314; }
    if (tim == TIM15)                     { return TST_K_T15; }
    if ((tim == TIM16) || (tim == TIM17)) { return TST_K_T1617; }
    return -1;
}

uint32_t tst_present(int32_t kind)
{
    /* CCR1..CCR4 are words 13..16, CCR5/CCR6 22/23 */
    const uint32_t ccr4 = R(13) | R(14) | R(15) | R(16);
    switch (kind)
    {
    case TST_K_ADV:   /* tables 355, 356 */
        return R(TST_RI_CR1) | R(TST_RI_CR2) | R(TST_RI_SMCR) | R(TST_RI_DIER) | R(TST_RI_SR) | R(TST_RI_CCMR1) |
               R(TST_RI_CCMR2) | R(TST_RI_CCER) | R(TST_RI_CNT) | R(TST_RI_PSC) | R(TST_RI_ARR) | R(TST_RI_RCR) |
               ccr4 | R(TST_RI_BDTR) | R(TST_RI_DCR) | R(TST_RI_CCMR3) | R(22) | R(23) | R(TST_RI_AF1) |
               R(TST_RI_AF2) | R(TST_RI_TISEL);
    case TST_K_GP32:  /* table 360 */
    case TST_K_GP16:
        return R(TST_RI_CR1) | R(TST_RI_CR2) | R(TST_RI_SMCR) | R(TST_RI_DIER) | R(TST_RI_SR) | R(TST_RI_CCMR1) |
               R(TST_RI_CCMR2) | R(TST_RI_CCER) | R(TST_RI_CNT) | R(TST_RI_PSC) | R(TST_RI_ARR) | ccr4 |
               R(TST_RI_DCR) | R(TST_RI_AF1) | R(TST_RI_TISEL);
    case TST_K_BASIC: /* table 371 */
        return R(TST_RI_CR1) | R(TST_RI_CR2) | R(TST_RI_DIER) | R(TST_RI_SR) | R(TST_RI_CNT) | R(TST_RI_PSC) |
               R(TST_RI_ARR);
    case TST_K_T12:   /* table 363 */
        return R(TST_RI_CR1) | R(TST_RI_CR2) | R(TST_RI_SMCR) | R(TST_RI_DIER) | R(TST_RI_SR) | R(TST_RI_CCMR1) |
               R(TST_RI_CCER) | R(TST_RI_CNT) | R(TST_RI_PSC) | R(TST_RI_ARR) | R(13) | R(14) | R(TST_RI_TISEL);
    case TST_K_T1314: /* table 365 */
        return R(TST_RI_CR1) | R(TST_RI_DIER) | R(TST_RI_SR) | R(TST_RI_CCMR1) | R(TST_RI_CCER) | R(TST_RI_CNT) |
               R(TST_RI_PSC) | R(TST_RI_ARR) | R(13) | R(TST_RI_TISEL);
    case TST_K_T15:   /* table 368 */
        return R(TST_RI_CR1) | R(TST_RI_CR2) | R(TST_RI_SMCR) | R(TST_RI_DIER) | R(TST_RI_SR) | R(TST_RI_CCMR1) |
               R(TST_RI_CCER) | R(TST_RI_CNT) | R(TST_RI_PSC) | R(TST_RI_ARR) | R(TST_RI_RCR) | R(13) | R(14) |
               R(TST_RI_BDTR) | R(TST_RI_DCR) | R(TST_RI_AF1) | R(TST_RI_TISEL);
    case TST_K_T1617: /* table 370 */
        return R(TST_RI_CR1) | R(TST_RI_CR2) | R(TST_RI_DIER) | R(TST_RI_SR) | R(TST_RI_CCMR1) | R(TST_RI_CCER) |
               R(TST_RI_CNT) | R(TST_RI_PSC) | R(TST_RI_ARR) | R(TST_RI_RCR) | R(13) | R(TST_RI_BDTR) | R(TST_RI_DCR) |
               R(TST_RI_AF1) | R(TST_RI_TISEL);
    default:
        return 0U;
    }
}

static bool kind_is_32(int32_t kind)
{
    return kind == TST_K_GP32;
}

void tst_reset_image(int32_t kind, TstImg_t *value, TstImg_t *mask)
{
    const uint32_t present = tst_present(kind);
    const uint32_t c = kind_is_32(kind) ? 0xFFFFFFFFUL : 0xFFFFUL;   /* counter-wide registers */
    for (uint32_t i = 0U; i < TST_RI_N; i++)
    {
        value->v[i] = 0U;
        mask->v[i]  = ((present & R(i)) != 0U) ? 0xFFFFFFFFUL : 0U;
    }
    /* ARR resets to all ones: 0xFFFF (40.4.14, 42.4.12, 42.5.10, 43.5.12, 43.6.11, 44.4.8),
       0xFFFF FFFF on TIM2-5 (41.4.15), of which TIM3/TIM4 implement 16 bits. */
    value->v[TST_RI_ARR] = ((kind == TST_K_GP32) || (kind == TST_K_GP16)) ? 0xFFFFFFFFUL : 0xFFFFUL;
    if ((present & R(TST_RI_CNT)) != 0U)
    {
        mask->v[TST_RI_CNT] = c;
        mask->v[TST_RI_ARR] = c;
        for (uint32_t i = 13U; i <= 16U; i++)
        {
            if ((present & R(i)) != 0U)
            {
                mask->v[i] = c;
            }
        }
    }
    /* AF1 and AF2 reset to 0x0000 0001 (BKINE) on TIM1/TIM8 (40.4.26-29), AF1 on TIM15
       (43.5.19) and TIM16/17 (43.6.17, 43.6.19); TIM2-5 AF1 resets to 0 (41.4.22-25). */
    if ((kind == TST_K_ADV) || (kind == TST_K_T15) || (kind == TST_K_T1617))
    {
        value->v[TST_RI_AF1] = 1U;
    }
    if (kind == TST_K_ADV)
    {
        value->v[TST_RI_AF2] = 1U;
    }
}

void tst_img_read(const TIM_TypeDef *tim, TstImg_t *img)
{
    const volatile uint32_t *r = (const volatile uint32_t *)tim;
    for (uint32_t i = 0U; i < TST_RI_N; i++)
    {
        img->v[i] = ((i == TST_RI_EGR) || (i == TST_RI_DMAR) || (i == 20U)) ? 0U : r[i];
    }
}

bool tst_reset_ok(const TIM_TypeDef *tim, uint32_t *bad_idx, uint32_t *got, uint32_t *exp)
{
    /* static: the selftest runs in one task per core, and its stack is small */
    static TstImg_t img;
    static TstImg_t value;
    static TstImg_t mask;
    const int32_t kind = tst_kind(tim);
    if (kind < 0)
    {
        return false;
    }
    tst_img_read(tim, &img);
    tst_reset_image(kind, &value, &mask);
    for (uint32_t i = 0U; i < TST_RI_N; i++)
    {
        if (((img.v[i] ^ value.v[i]) & mask.v[i]) != 0U)
        {
            *bad_idx = i;
            *got = img.v[i];
            *exp = value.v[i] & mask.v[i];
            return false;
        }
    }
    return true;
}

/* Plan rule 2, typed again from RM0399 rev 4: ITR from tables 353 (TIM1/8), 358 (TIM2-5),
   361 (TIM12), 366 (TIM15); MMS from the CR2 sections; TISEL from 40.4.30-31 (TIM1
   TI1SEL 0001 = COMP1, TIM8 TI1SEL 0001 = COMP2: the plan's table says "ninguno", the RM
   rules), 41.4.26-29, 42.4.15, 42.5.12-13, 43.5.20, 43.6.18, 43.6.20. */
bool tst_hand_caps(const TIM_TypeDef *tim, TimBm_Caps_t *c)
{
    const int32_t kind = tst_kind(tim);
    if (kind < 0)
    {
        return false;
    }
    *c = (TimBm_Caps_t){ 0 };
    c->counter_bits = kind_is_32(kind) ? 32U : 16U;
    switch (kind)
    {
    case TST_K_ADV:
        c->channels = 4U; c->itr_mask = 0x000FU; c->mms_max = 7U; c->up_down_center = true; c->repetition = true;
        c->bdtr = true; c->slave_mode = true; c->master_mode = true;
        c->tisel_mask[0] = 0x0003U; c->tisel_mask[1] = 0x0001U; c->tisel_mask[2] = 0x0001U; c->tisel_mask[3] = 0x0001U;
        break;
    case TST_K_GP32:
    case TST_K_GP16:
        c->channels = 4U; c->mms_max = 7U; c->up_down_center = true; c->slave_mode = true; c->master_mode = true;
        c->tisel_mask[0] = 0x0001U; c->tisel_mask[1] = 0x0001U; c->tisel_mask[2] = 0x0001U; c->tisel_mask[3] = 0x0001U;
        if (tim == TIM2)      { c->itr_mask = 0x007FU; c->tisel_mask[3] = 0x000FU; }   /* ITR0-6; TI4: COMP1/2 */
        else if (tim == TIM3) { c->itr_mask = 0x001FU; c->tisel_mask[0] = 0x000FU; }   /* ITR0-4; TI1: COMP1/2 */
        else if (tim == TIM4) { c->itr_mask = 0x000FU; }                               /* ITR0-3               */
        else                  { c->itr_mask = 0x01CFU; c->tisel_mask[0] = 0x0007U; }   /* TIM5: ITR0-3, 6-8; TI1: fdcan1 */
        break;
    case TST_K_BASIC:
        c->mms_max = 2U; c->master_mode = true;
        break;
    case TST_K_T12:
        c->channels = 2U; c->itr_mask = 0x000FU; c->mms_max = 5U; c->slave_mode = true; c->master_mode = true;
        c->tisel_mask[0] = 0x0003U; c->tisel_mask[1] = 0x0001U;                         /* TI1: spdifrx */
        break;
    case TST_K_T1314:
        c->channels = 1U; c->tisel_mask[0] = 0x0001U;
        break;
    case TST_K_T15:
        c->channels = 2U; c->itr_mask = 0x000FU; c->mms_max = 5U; c->repetition = true; c->bdtr = true;
        c->slave_mode = true; c->master_mode = true;
        c->tisel_mask[0] = 0x007FU; c->tisel_mask[1] = 0x000FU;                         /* TI1: 1-6, TI2: 1-3 */
        break;
    default:   /* TST_K_T1617 */
        c->channels = 1U; c->repetition = true; c->bdtr = true;
        c->tisel_mask[0] = (tim == TIM16) ? 0x000FU : 0x000DU;                           /* TIM17: 0, 2, 3 */
        break;
    }
    return true;
}

/* ---- T2: the field encodings, by hand ------------------------------------------- */

/* CR1 (40.4.1): URS bit 2, OPM bit 3, DIR bit 4, CMS bits 6:5, ARPE bit 7. */
static uint32_t enc_cr1(const TimBm_TimeBaseConfig_t *tb)
{
    uint32_t v = 0U;
    if (tb->urs_overflow_only) { v |= 1UL << 2; }
    if (tb->one_pulse)         { v |= 1UL << 3; }
    if (tb->count_mode == TIM_BM_COUNT_DOWN) { v |= 1UL << 4; }
    if (tb->count_mode >= TIM_BM_COUNT_CENTER1) { v |= (tb->count_mode - 1U) << 5; }   /* CENTER1 -> 01 */
    if (tb->arr_preload)       { v |= 1UL << 7; }
    return v;
}

/* CCMR1/CCMR2 (40.4.7, 40.4.9): channel n in the byte (n-1)%2 of CCMR1 (n = 1, 2) or
   CCMR2 (n = 3, 4). Output: CCxS 1:0 = 00, OCxPE bit 3, OCxM bits 6:4. Input: CCxS
   1:0, ICxPSC 3:2, ICxF 7:4. */
static uint32_t ccmr_word(uint32_t ch)
{
    return (ch <= 2U) ? TST_RI_CCMR1 : TST_RI_CCMR2;
}

static uint32_t ccmr_sh(uint32_t ch)
{
    return ((ch - 1U) % 2U) * 8U;
}

static uint32_t psc_bits(uint32_t prescaler)
{
    return (prescaler == 8U) ? 3U : ((prescaler == 4U) ? 2U : ((prescaler == 2U) ? 1U : 0U));
}

/* TS[4:0] (41.4.3): ITR0-3 = 00000-00011, TI1F_ED 00100, TI1FP1 00101, TI2FP2 00110,
   ITR4-8 = 01000-01100; TS[2:0] in SMCR bits 6:4, TS[4:3] in bits 21:20. */
static uint32_t enc_ts(uint32_t trg)
{
    static const uint8_t k_ts[19] = { 0U, 1U, 2U, 3U, 8U, 9U, 10U, 11U, 12U,
                                      0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
                                      4U, 5U, 6U };
    const uint32_t ts = k_ts[trg];
    return ((ts & 7U) << 4) | ((ts >> 3) << 20);
}

/* SMS[3:0] (40.4.3): SMS[2:0] bits 2:0, SMS[3] bit 16. */
static uint32_t enc_sms(uint32_t sms)
{
    return (sms & 7U) | ((sms >> 3) << 16);
}

static void t2_check(TIM_TypeDef *tim, const TstT2Cfg_t *cfg, TimBm_Status_t st, const TstImg_t *exp,
                     const TstImg_t *mask, TstT2Hook_t hook, TstT2Result_t *res)
{
    static TstImg_t got;
    tst_img_read(tim, &got);
    res->configs++;
    res->per_class[cfg->cls]++;
    if (st != TIM_BM_OK)
    {
        res->calls_bad++;
    }
    for (uint32_t i = 0U; i < TST_RI_N; i++)
    {
        if ((i != TST_RI_CNT) && (((got.v[i] ^ exp->v[i]) & mask->v[i]) != 0U))
        {
            if (res->mismatches == 0U)
            {
                res->first_cfg = res->configs - 1U;
                res->first_idx = i;
                res->first_got = got.v[i];
                res->first_exp = exp->v[i] & mask->v[i];
                res->first_cls = cfg->cls;
            }
            res->mismatches++;
            break;
        }
    }
    if (hook != NULL)
    {
        hook(cfg, &got);
    }
}

void tst_t2_run(TIM_TypeDef *tim, TstT2Hook_t hook, TstT2Result_t *res)
{
    static const uint32_t k_sms[6] = { TIM_BM_SMS_DISABLED, TIM_BM_SMS_RESET, TIM_BM_SMS_GATED, TIM_BM_SMS_TRIGGER,
                                       TIM_BM_SMS_EXT_CLOCK1, TIM_BM_SMS_RESET_TRIGGER };
    static const uint32_t k_icpsc[4] = { 1U, 2U, 4U, 8U };
    static const uint32_t k_icf[3] = { 0U, 7U, 15U };
    static const uint32_t k_edge[3] = { TIM_BM_EDGE_RISING, TIM_BM_EDGE_FALLING, TIM_BM_EDGE_BOTH };
    TimBm_Caps_t caps;
    static TstImg_t reset;     /* static: a small task stack on both cores */
    static TstImg_t mask;
    static TstImg_t exp;
    static TstImg_t mk;
    static TstT2Cfg_t cfg;
    *res = (TstT2Result_t){ 0 };
    const int32_t kind = tst_kind(tim);
    if ((kind < 0) || !tst_hand_caps(tim, &caps))
    {
        res->calls_bad = 1U;
        return;
    }
    tst_reset_image(kind, &reset, &mask);
    const uint32_t cmax = (caps.counter_bits == 32U) ? 0xFFFFFFFFUL : 0xFFFFUL;

    /* Time base: every count mode, ARPE, OPM, URS. */
    const uint32_t n_modes = caps.up_down_center ? 5U : 1U;
    for (uint32_t m = 0U; m < n_modes; m++)
    {
        for (uint32_t b = 0U; b < 8U; b++)
        {
            cfg = (TstT2Cfg_t){ 0 };
            cfg.cls = TST_T2_TB;
            cfg.tb = (TimBm_TimeBaseConfig_t){ tim, 7U, (caps.counter_bits == 32U) ? 0x12345UL : 999UL, m,
                                               caps.repetition ? 3U : 0U, (b & 1U) != 0U, (b & 2U) != 0U,
                                               (b & 4U) != 0U };
            (void)TIM_BM_Deinit(tim);
            const TimBm_Status_t st = TIM_BM_InitTimeBase(&cfg.tb);
            exp = reset;
            exp.v[TST_RI_CR1] = enc_cr1(&cfg.tb);
            exp.v[TST_RI_PSC] = cfg.tb.psc;
            exp.v[TST_RI_ARR] = cfg.tb.arr;
            exp.v[TST_RI_RCR] = cfg.tb.repetition;
            mk = mask;
            if (m >= TIM_BM_COUNT_CENTER1)
            {
                mk.v[TST_RI_CR1] &= ~(1UL << 4);    /* DIR: read-only status in center mode (40.4.1) */
            }
            if ((m == 0U) && (b == 0U))
            {
                /* The control: an expected image altered on purpose must be flagged. */
                const uint32_t cr1 = tim->CR1;
                exp.v[TST_RI_CR1] ^= 1UL << 7;
                res->ctrl_detected = (((cr1 ^ exp.v[TST_RI_CR1]) & mk.v[TST_RI_CR1]) != 0U) ? 1U : 0U;
                exp.v[TST_RI_CR1] ^= 1UL << 7;
            }
            t2_check(tim, &cfg, st, &exp, &mk, hook, res);
        }
    }

    /* Output compare: 8 modes x 2 polarities x preload, every channel. */
    for (uint32_t ch = 1U; ch <= caps.channels; ch++)
    {
        for (uint32_t mode = 0U; mode < 8U; mode++)
        {
            for (uint32_t b = 0U; b < 4U; b++)
            {
                cfg = (TstT2Cfg_t){ 0 };
                cfg.cls = TST_T2_OC;
                cfg.oc = (TimBm_OcConfig_t){ tim, ch, mode, 0x1234U & cmax, (b & 1U) != 0U, (b & 2U) != 0U };
                (void)TIM_BM_Deinit(tim);
                const TimBm_Status_t st = TIM_BM_ConfigOutput(&cfg.oc);
                exp = reset;
                exp.v[ccmr_word(ch)] = ((mode << 4) | (cfg.oc.preload ? (1UL << 3) : 0U)) << ccmr_sh(ch);
                exp.v[TST_RI_CCR1 + ch - 1U] = cfg.oc.compare;
                exp.v[TST_RI_CCER] = cfg.oc.active_low ? (1UL << (4U * (ch - 1U) + 1U)) : 0U;   /* CCxP */
                t2_check(tim, &cfg, st, &exp, &mask, hook, res);
            }
        }
    }

    /* Input capture: 3 selections x 3 edges x 4 prescalers x filters 0/7/15 x valid TISEL. */
    for (uint32_t ch = 1U; ch <= caps.channels; ch++)
    {
        for (uint32_t sel = TIM_BM_IC_DIRECT; sel <= TIM_BM_IC_TRC; sel++)
        {
            if (((sel == TIM_BM_IC_INDIRECT) && (caps.channels < 2U)) || ((sel == TIM_BM_IC_TRC) && !caps.slave_mode))
            {
                continue;
            }
            for (uint32_t e = 0U; e < 3U; e++)
            {
                for (uint32_t p = 0U; p < 4U; p++)
                {
                    for (uint32_t f = 0U; f < 3U; f++)
                    {
                        for (uint32_t ts = 0U; ts < 16U; ts++)
                        {
                            if ((caps.tisel_mask[ch - 1U] & (1UL << ts)) == 0U)
                            {
                                continue;
                            }
                            cfg = (TstT2Cfg_t){ 0 };
                            cfg.cls = TST_T2_IC;
                            cfg.ic = (TimBm_IcConfig_t){ tim, ch, sel, k_edge[e], k_icpsc[p], k_icf[f], ts };
                            (void)TIM_BM_Deinit(tim);
                            const TimBm_Status_t st = TIM_BM_ConfigInput(&cfg.ic);
                            exp = reset;
                            exp.v[ccmr_word(ch)] = (sel | (psc_bits(k_icpsc[p]) << 2) | (k_icf[f] << 4)) << ccmr_sh(ch);
                            /* CCxP bit 4(n-1)+1, CCxNP bit 4(n-1)+3 (40.4.11): 00 rising, 01 falling, 11 both */
                            exp.v[TST_RI_CCER] = (((k_edge[e] & 1U) != 0U) ? (1UL << (4U * (ch - 1U) + 1U)) : 0U) |
                                                 (((k_edge[e] & 2U) != 0U) ? (1UL << (4U * (ch - 1U) + 3U)) : 0U);
                            exp.v[TST_RI_TISEL] = ts << (8U * (ch - 1U));   /* TIxSEL at bits 8(n-1)+3:8(n-1) */
                            t2_check(tim, &cfg, st, &exp, &mask, hook, res);
                        }
                    }
                }
            }
        }
    }

    /* Master: every MMS up to the instance's, with and without MSM. CR2 MMS bits 6:4,
       SMCR MSM bit 7. */
    if (caps.master_mode)
    {
        for (uint32_t mms = 0U; mms <= caps.mms_max; mms++)
        {
            for (uint32_t msm = 0U; msm < (caps.slave_mode ? 2U : 1U); msm++)
            {
                cfg = (TstT2Cfg_t){ 0 };
                cfg.cls = TST_T2_MS;
                cfg.mms = mms;
                cfg.msm = msm != 0U;
                (void)TIM_BM_Deinit(tim);
                const TimBm_Status_t st = TIM_BM_ConfigMaster(tim, mms, cfg.msm);
                exp = reset;
                exp.v[TST_RI_CR2] = mms << 4;
                exp.v[TST_RI_SMCR] = cfg.msm ? (1UL << 7) : 0U;
                t2_check(tim, &cfg, st, &exp, &mask, hook, res);
            }
        }
    }

    /* Slave: every SMS of the API x every ITR the instance wires, TI1F_ED, TI1FP1, TI2FP2
       (gated on TI1F_ED is forbidden by 40.4.3 and left out). */
    if (caps.slave_mode)
    {
        for (uint32_t s = 0U; s < 6U; s++)
        {
            for (uint32_t trg = 0U; trg <= TIM_BM_TRG_TI2FP2; trg++)
            {
                const bool itr = (trg <= 8U) && ((caps.itr_mask & (1UL << trg)) != 0U);
                const bool ti = (trg >= TIM_BM_TRG_TI1F_ED);
                if ((!itr && !ti) || ((k_sms[s] == TIM_BM_SMS_GATED) && (trg == TIM_BM_TRG_TI1F_ED)))
                {
                    continue;
                }
                cfg = (TstT2Cfg_t){ 0 };
                cfg.cls = TST_T2_SL;
                cfg.sms = k_sms[s];
                cfg.trg = trg;
                (void)TIM_BM_Deinit(tim);
                const TimBm_Status_t st = TIM_BM_ConfigSlave(tim, cfg.sms, trg);
                exp = reset;
                exp.v[TST_RI_SMCR] = enc_ts(trg) | enc_sms(cfg.sms);
                t2_check(tim, &cfg, st, &exp, &mask, hook, res);
            }
        }
    }
    (void)TIM_BM_Deinit(tim);
}

/* ---- T6 points ------------------------------------------------------------------ */

const TstT6Point_t k_tst_t6[TST_T6_POINTS] = {
    { 0U, 199U, TIM_BM_OC_PWM1, 100U, 0U, 0U },   /* P0: 1 MHz, 50 %             */
    { 9U, 199U, TIM_BM_OC_PWM1, 1U,   0U, 0U },   /* P1: 100 kHz, CCR 1 = 50 ns  */
    { 9U, 199U, TIM_BM_OC_PWM1, 0U,   0U, 0U },   /* P2: 0 %                     */
    { 9U, 199U, TIM_BM_OC_PWM1, 50U,  0U, 0U },   /* P3: 25 %                    */
    { 9U, 199U, TIM_BM_OC_PWM1, 100U, 0U, 0U },   /* P4: 50 %                    */
    { 9U, 199U, TIM_BM_OC_PWM1, 200U, 0U, 0U },   /* P5: 100 % (CCR = ARR + 1)   */
    { 9U, 199U, TIM_BM_OC_PWM1, 50U,  1U, 0U },   /* P6: active_low              */
    { 9U, 199U, TIM_BM_OC_PWM2, 50U,  0U, 0U },   /* P7: PWM2                    */
    { 0U, 199U, TIM_BM_OC_PWM1, 50U,  0U, 1U },   /* P8: center 1 (TIM1 only)    */
};

TimBm_Status_t tst_t6_apply(TIM_TypeDef *tim, uint32_t ch, const TstT6Point_t *p)
{
    const TimBm_TimeBaseConfig_t c = { tim, p->psc, p->arr, (p->center != 0U) ? TIM_BM_COUNT_CENTER1 : TIM_BM_COUNT_UP,
                                       0U, true, false, false };
    const TimBm_OcConfig_t oc = { tim, ch, p->mode, p->ccr, p->low != 0U, true };
    TimBm_Status_t st = TIM_BM_Deinit(tim);
    if (st == TIM_BM_OK) { st = TIM_BM_InitTimeBase(&c); }
    if (st == TIM_BM_OK) { st = TIM_BM_ConfigOutput(&oc); }
    if (st == TIM_BM_OK) { st = TIM_BM_OutputEnable(tim, TIM_BM_CH(ch)); }
    if (st == TIM_BM_OK) { st = TIM_BM_Start(tim); }
    return st;
}
