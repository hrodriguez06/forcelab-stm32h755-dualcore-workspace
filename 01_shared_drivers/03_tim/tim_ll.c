/**
  ******************************************************************************
  * @file    tim_ll.c
  * @brief   TIM1-8, TIM12-17, LL tier. Contract in tim_ll.h.
  *
  *          Every function translates its LL constants to tim_bm values, lets
  *          TIM_BM_Check* validate them (C6: one copy of the checks and of the
  *          capability table), then writes with the LL inlines of
  *          stm32h7xx_ll_tim.h in the order of the tim_bm rules and reads back.
  *          Sources of the register layout: RM0399 rev 4, 40.4 (TIM1/TIM8), as
  *          tim_bm.c.
  ******************************************************************************
  */

#include "tim_ll.h"

/* C8: no <stddef.h>. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* ==========================================================================
 * LL constant -> tim_bm value. An unknown constant gives a value tim_bm refuses
 * with ERR_PARAM in its field-range check, so the C6 order is tim_bm's.
 * ========================================================================== */

#define LL_CH_ALL   (LL_TIM_CHANNEL_CH1 | LL_TIM_CHANNEL_CH2 | LL_TIM_CHANNEL_CH3 | LL_TIM_CHANNEL_CH4)
#define BM_BAD_CH_MASK  (1UL << 4)      /* outside TIM_BM_CH(1..4): ERR_PARAM in tim_bm */

static uint32_t bm_channel(uint32_t ll_ch)
{
    switch (ll_ch)
    {
    case LL_TIM_CHANNEL_CH1: return 1U;
    case LL_TIM_CHANNEL_CH2: return 2U;
    case LL_TIM_CHANNEL_CH3: return 3U;
    case LL_TIM_CHANNEL_CH4: return 4U;
    default:                 return 0U;  /* channel 0: ERR_PARAM */
    }
}

/* LL_TIM_CHANNEL_CHx is CCxE, bit 4 (x - 1) of CCER; TIM_BM_CH(x) is bit x - 1. */
static uint32_t bm_ch_mask(uint32_t ll_mask)
{
    uint32_t m = 0U;
    for (uint32_t i = 0U; i < 4U; i++)
    {
        if ((ll_mask & (LL_TIM_CHANNEL_CH1 << (4U * i))) != 0U)
        {
            m |= 1UL << i;
        }
    }
    if ((ll_mask & ~LL_CH_ALL) != 0U)
    {
        m |= BM_BAD_CH_MASK;             /* CHxN, CH5, CH6 or a stray bit */
    }
    return m;
}

static uint32_t ll_ch_mask(uint32_t bm_mask)
{
    uint32_t m = 0U;
    for (uint32_t i = 0U; i < 4U; i++)
    {
        if ((bm_mask & (1UL << i)) != 0U)
        {
            m |= LL_TIM_CHANNEL_CH1 << (4U * i);
        }
    }
    return m;
}

static uint32_t bm_count_mode(uint32_t ll_mode)
{
    switch (ll_mode)
    {
    case LL_TIM_COUNTERMODE_UP:          return TIM_BM_COUNT_UP;
    case LL_TIM_COUNTERMODE_DOWN:        return TIM_BM_COUNT_DOWN;
    case LL_TIM_COUNTERMODE_CENTER_DOWN: return TIM_BM_COUNT_CENTER1;   /* CMS = 01 */
    case LL_TIM_COUNTERMODE_CENTER_UP:   return TIM_BM_COUNT_CENTER2;   /* CMS = 10 */
    case LL_TIM_COUNTERMODE_CENTER_UP_DOWN: return TIM_BM_COUNT_CENTER3; /* CMS = 11 */
    default:                             return 0xFFFFFFFFUL;
    }
}

/* LL_TIM_OCMODE_FROZEN..PWM2 are OCxM[2:0] at bits 6:4: the tim_bm value << 4.
   The modes with OCxM[3] (retriggerable OPM, combined, asymmetric) are outside the
   API and become 8, which tim_bm refuses. */
static uint32_t bm_oc_mode(uint32_t ll_mode)
{
    return ((ll_mode & ~TIM_CCMR1_OC1M_2 & ~TIM_CCMR1_OC1M_1 & ~TIM_CCMR1_OC1M_0) == 0U) ? (ll_mode >> 4) : 8U;
}

static uint32_t bm_ic_selection(uint32_t ll_input)
{
    switch (ll_input)
    {
    case LL_TIM_ACTIVEINPUT_DIRECTTI:   return TIM_BM_IC_DIRECT;
    case LL_TIM_ACTIVEINPUT_INDIRECTTI: return TIM_BM_IC_INDIRECT;
    case LL_TIM_ACTIVEINPUT_TRC:        return TIM_BM_IC_TRC;
    default:                            return 0U;
    }
}

static uint32_t bm_edge(uint32_t ll_polarity)
{
    switch (ll_polarity)
    {
    case LL_TIM_IC_POLARITY_RISING:   return TIM_BM_EDGE_RISING;
    case LL_TIM_IC_POLARITY_FALLING:  return TIM_BM_EDGE_FALLING;
    case LL_TIM_IC_POLARITY_BOTHEDGE: return TIM_BM_EDGE_BOTH;
    default:                          return 2U;                 /* CCxNP alone: no edge */
    }
}

static uint32_t bm_ic_prescaler(uint32_t ll_psc)
{
    switch (ll_psc)
    {
    case LL_TIM_ICPSC_DIV1: return 1U;
    case LL_TIM_ICPSC_DIV2: return 2U;
    case LL_TIM_ICPSC_DIV4: return 4U;
    case LL_TIM_ICPSC_DIV8: return 8U;
    default:                return 3U;
    }
}

#define LL_FILTER_MSK  (TIM_CCMR1_IC1F << 16U)   /* the LL_TIM_IC_FILTER_* field */

static uint32_t bm_filter(uint32_t ll_filter)
{
    return ((ll_filter & ~LL_FILTER_MSK) == 0U) ? (ll_filter >> 20) : 16U;
}

/* TIxSEL of input ti (1..4) sits at bits 8 (ti - 1) + 3..0 of TISEL, where the
   LL_TIM_TIMy_TIx_RMP_* constants put it. */
static uint32_t bm_tisel(uint32_t ll_remap, uint32_t ti)
{
    if ((ti == 0U) || (ti > 4U))
    {
        return 16U;
    }
    const uint32_t sh = 8U * (ti - 1U);
    return ((ll_remap & ~(TIM_TISEL_TI1SEL << sh)) == 0U) ? (ll_remap >> sh) : 16U;
}

static uint32_t bm_mms(uint32_t ll_trgo)
{
    return ((ll_trgo & ~TIM_CR2_MMS) == 0U) ? (ll_trgo >> TIM_CR2_MMS_Pos) : 8U;
}

static uint32_t bm_sms(uint32_t ll_mode)
{
    switch (ll_mode)
    {
    case LL_TIM_SLAVEMODE_DISABLED:              return TIM_BM_SMS_DISABLED;
    case LL_TIM_SLAVEMODE_RESET:                 return TIM_BM_SMS_RESET;
    case LL_TIM_SLAVEMODE_GATED:                 return TIM_BM_SMS_GATED;
    case LL_TIM_SLAVEMODE_TRIGGER:               return TIM_BM_SMS_TRIGGER;
    case LL_TIM_CLOCKSOURCE_EXT_MODE1:           return TIM_BM_SMS_EXT_CLOCK1;
    case LL_TIM_SLAVEMODE_COMBINED_RESETTRIGGER: return TIM_BM_SMS_RESET_TRIGGER;
    default:                                     return 1U;      /* not a TIM_BM_SMS_* */
    }
}

static uint32_t bm_trigger(uint32_t ll_ts)
{
    switch (ll_ts)
    {
    case LL_TIM_TS_ITR0:    return TIM_BM_TRG_ITR(0);
    case LL_TIM_TS_ITR1:    return TIM_BM_TRG_ITR(1);
    case LL_TIM_TS_ITR2:    return TIM_BM_TRG_ITR(2);
    case LL_TIM_TS_ITR3:    return TIM_BM_TRG_ITR(3);
    case LL_TIM_TS_ITR4:    return TIM_BM_TRG_ITR(4);
    case LL_TIM_TS_ITR5:    return TIM_BM_TRG_ITR(5);
    case LL_TIM_TS_ITR6:    return TIM_BM_TRG_ITR(6);
    case LL_TIM_TS_ITR7:    return TIM_BM_TRG_ITR(7);
    case LL_TIM_TS_ITR8:    return TIM_BM_TRG_ITR(8);
    case LL_TIM_TS_TI1F_ED: return TIM_BM_TRG_TI1F_ED;
    case LL_TIM_TS_TI1FP1:  return TIM_BM_TRG_TI1FP1;
    case LL_TIM_TS_TI2FP2:  return TIM_BM_TRG_TI2FP2;
    default:                return 19U;  /* ETRF, ITR9-13 (not on this device), other */
    }
}

/* ==========================================================================
 * Register layout used by the writes and the read-backs (RM0399 40.4.7-40.4.11)
 * ========================================================================== */

/* PRIMASK section (C10): saved and restored, never unconditionally enabled. */
static inline uint32_t irq_save(void)
{
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    return pm;
}

static inline void irq_restore(uint32_t pm)
{
    __set_PRIMASK(pm);
}

/* CCMRx of channel ch (1..4) and the shift of its 8-bit field; OCxM[3] is bit
   16 + shift. */
static uint32_t ccmr_read(const TIM_TypeDef *tim, uint32_t ch)
{
    return (ch <= 2U) ? LL_TIM_ReadReg(tim, CCMR1) : LL_TIM_ReadReg(tim, CCMR2);
}

static uint32_t ccmr_shift(uint32_t ch)
{
    return ((ch - 1U) & 1U) * 8U;
}

static uint32_t ccmr_field_mask(uint32_t ch)
{
    const uint32_t sh = ccmr_shift(ch);
    return (0xFFUL << sh) | (1UL << (16U + sh));
}

static uint32_t ccer_pol_mask(uint32_t ch)
{
    return (TIM_CCER_CC1P | TIM_CCER_CC1NP) << (4U * (ch - 1U));
}

static uint32_t ccr_read(const TIM_TypeDef *tim, uint32_t ch)
{
    switch (ch)
    {
    case 1U:  return LL_TIM_OC_GetCompareCH1(tim);
    case 2U:  return LL_TIM_OC_GetCompareCH2(tim);
    case 3U:  return LL_TIM_OC_GetCompareCH3(tim);
    default:  return LL_TIM_OC_GetCompareCH4(tim);
    }
}

static void ccr_write(TIM_TypeDef *tim, uint32_t ch, uint32_t ccr)
{
    switch (ch)
    {
    case 1U:  LL_TIM_OC_SetCompareCH1(tim, ccr); break;
    case 2U:  LL_TIM_OC_SetCompareCH2(tim, ccr); break;
    case 3U:  LL_TIM_OC_SetCompareCH3(tim, ccr); break;
    default:  LL_TIM_OC_SetCompareCH4(tim, ccr); break;
    }
}

static bool has_bdtr(const TIM_TypeDef *tim)
{
    TimBm_Caps_t caps;
    return (TIM_BM_GetCaps(tim, &caps) == TIM_BM_OK) && caps.bdtr;
}

/* ==========================================================================
 * Clock
 * ========================================================================== */

TimBm_Status_t TIM_LL_EnableClock(TIM_TypeDef *tim)
{
    return TIM_BM_EnableClock(tim);
}

TimBm_Status_t TIM_LL_DisableClock(TIM_TypeDef *tim)
{
    return TIM_BM_DisableClock(tim);
}

/* ==========================================================================
 * Time base
 * ========================================================================== */

TimBm_Status_t TIM_LL_InitTimeBase(const TimLl_TimeBaseConfig_t *cfg)
{
    if (cfg == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_TimeBaseConfig_t bm = {
        .tim = cfg->tim, .psc = cfg->prescaler, .arr = cfg->autoreload,
        .count_mode = bm_count_mode(cfg->counter_mode), .repetition = cfg->repetition,
        .arr_preload = cfg->arr_preload, .one_pulse = cfg->one_pulse,
        .urs_overflow_only = cfg->urs_overflow_only,
    };
    const TimBm_Status_t st = TIM_BM_CheckTimeBase(&bm);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    TimBm_Caps_t caps;
    (void)TIM_BM_GetCaps(cfg->tim, &caps);     /* a timer: validated above */
    TIM_TypeDef *tim = cfg->tim;
    const uint32_t mode = cfg->counter_mode;

    /* Rule 3 and R5-3: CR1 with CMS = 00 -> CR1 (mode, ARPE, OPM, URS = 1, CEN = 0)
       -> PSC -> ARR -> RCR -> UG -> CR1 final -> SR = 0. */
    const uint32_t pm = irq_save();
    LL_TIM_SetCounterMode(tim, LL_TIM_COUNTERMODE_UP);  /* CMS = 00 first: DIR is read-only
                                                           while CMS != 00 (40.4.1)       */
    LL_TIM_SetUpdateSource(tim, LL_TIM_UPDATESOURCE_COUNTER);   /* URS = 1 until the UG   */
    LL_TIM_SetCounterMode(tim, mode);
    LL_TIM_SetClockDivision(tim, LL_TIM_CLOCKDIVISION_DIV1);
    LL_TIM_EnableUpdateEvent(tim);                      /* UDIS = 0 */
    LL_TIM_DisableUIFRemap(tim);
    if (cfg->arr_preload)
    {
        LL_TIM_EnableARRPreload(tim);
    }
    else
    {
        LL_TIM_DisableARRPreload(tim);
    }
    LL_TIM_SetOnePulseMode(tim, cfg->one_pulse ? LL_TIM_ONEPULSEMODE_SINGLE : LL_TIM_ONEPULSEMODE_REPETITIVE);
    LL_TIM_SetPrescaler(tim, cfg->prescaler);
    LL_TIM_SetAutoReload(tim, cfg->autoreload);
    if (caps.repetition)
    {
        LL_TIM_SetRepetitionCounter(tim, cfg->repetition);
    }
    LL_TIM_GenerateEvent_UPDATE(tim);                   /* loads PSC, ARR, RCR; no UIF */
    LL_TIM_SetUpdateSource(tim, cfg->urs_overflow_only ? LL_TIM_UPDATESOURCE_COUNTER
                                                       : LL_TIM_UPDATESOURCE_REGULAR);
    LL_TIM_WriteReg(tim, SR, 0U);                       /* one write, no RMW (C10) */
    __DSB();
    irq_restore(pm);

    uint32_t cr1 = mode;
    cr1 |= cfg->arr_preload ? TIM_CR1_ARPE : 0U;
    cr1 |= cfg->one_pulse ? TIM_CR1_OPM : 0U;
    cr1 |= cfg->urs_overflow_only ? TIM_CR1_URS : 0U;
    /* In center-aligned mode DIR is a read-only status bit (40.4.1). */
    const uint32_t cr1_mask = ((mode & TIM_CR1_CMS) != 0U) ? ~TIM_CR1_DIR : 0xFFFFFFFFUL;
    if (((LL_TIM_ReadReg(tim, CR1) & 0xFFFFU & cr1_mask) != (cr1 & cr1_mask)) ||
        (LL_TIM_GetPrescaler(tim) != cfg->prescaler) || (LL_TIM_GetAutoReload(tim) != cfg->autoreload) ||
        (caps.repetition && (LL_TIM_GetRepetitionCounter(tim) != cfg->repetition)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_LL_SetPeriod(TIM_TypeDef *tim, uint32_t autoreload)
{
    const TimBm_Status_t st = TIM_BM_CheckPeriod(tim, autoreload);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    LL_TIM_SetAutoReload(tim, autoreload);
    return (LL_TIM_GetAutoReload(tim) == autoreload) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

TimBm_Status_t TIM_LL_Start(TIM_TypeDef *tim)
{
    if (!TIM_BM_IsValidInstance(tim))
    {
        return TIM_BM_ERR_PARAM;
    }
    const uint32_t pm = irq_save();
    LL_TIM_EnableCounter(tim);                 /* not read back: OPM clears CEN itself */
    irq_restore(pm);
    return TIM_BM_OK;
}

TimBm_Status_t TIM_LL_Stop(TIM_TypeDef *tim)
{
    if (!TIM_BM_IsValidInstance(tim))
    {
        return TIM_BM_ERR_PARAM;
    }
    const uint32_t pm = irq_save();
    LL_TIM_DisableCounter(tim);
    irq_restore(pm);
    return (LL_TIM_IsEnabledCounter(tim) == 0U) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

TimBm_Status_t TIM_LL_Deinit(TIM_TypeDef *tim)
{
    return TIM_BM_Deinit(tim);                 /* the reset image is the capability table's */
}

/* ==========================================================================
 * Events, flags, interrupts, DMA requests (event masks need no translation)
 * ========================================================================== */

static void dier_irq(TIM_TypeDef *tim, uint32_t ev_mask, bool set)
{
    if ((ev_mask & LL_TIM_DIER_UIE) != 0U)   { if (set) { LL_TIM_EnableIT_UPDATE(tim); } else { LL_TIM_DisableIT_UPDATE(tim); } }
    if ((ev_mask & LL_TIM_DIER_CC1IE) != 0U) { if (set) { LL_TIM_EnableIT_CC1(tim); } else { LL_TIM_DisableIT_CC1(tim); } }
    if ((ev_mask & LL_TIM_DIER_CC2IE) != 0U) { if (set) { LL_TIM_EnableIT_CC2(tim); } else { LL_TIM_DisableIT_CC2(tim); } }
    if ((ev_mask & LL_TIM_DIER_CC3IE) != 0U) { if (set) { LL_TIM_EnableIT_CC3(tim); } else { LL_TIM_DisableIT_CC3(tim); } }
    if ((ev_mask & LL_TIM_DIER_CC4IE) != 0U) { if (set) { LL_TIM_EnableIT_CC4(tim); } else { LL_TIM_DisableIT_CC4(tim); } }
    if ((ev_mask & LL_TIM_DIER_TIE) != 0U)   { if (set) { LL_TIM_EnableIT_TRIG(tim); } else { LL_TIM_DisableIT_TRIG(tim); } }
}

static void dier_dma(TIM_TypeDef *tim, uint32_t ev_mask, bool set)
{
    if ((ev_mask & LL_TIM_DIER_UIE) != 0U)   { if (set) { LL_TIM_EnableDMAReq_UPDATE(tim); } else { LL_TIM_DisableDMAReq_UPDATE(tim); } }
    if ((ev_mask & LL_TIM_DIER_CC1IE) != 0U) { if (set) { LL_TIM_EnableDMAReq_CC1(tim); } else { LL_TIM_DisableDMAReq_CC1(tim); } }
    if ((ev_mask & LL_TIM_DIER_CC2IE) != 0U) { if (set) { LL_TIM_EnableDMAReq_CC2(tim); } else { LL_TIM_DisableDMAReq_CC2(tim); } }
    if ((ev_mask & LL_TIM_DIER_CC3IE) != 0U) { if (set) { LL_TIM_EnableDMAReq_CC3(tim); } else { LL_TIM_DisableDMAReq_CC3(tim); } }
    if ((ev_mask & LL_TIM_DIER_CC4IE) != 0U) { if (set) { LL_TIM_EnableDMAReq_CC4(tim); } else { LL_TIM_DisableDMAReq_CC4(tim); } }
}

static TimBm_Status_t dier_common(TIM_TypeDef *tim, uint32_t ev_mask, uint32_t use, bool set)
{
    const TimBm_Status_t st = TIM_BM_CheckEvents(tim, ev_mask, use);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    /* UDE = bit 8, CCxDE = bit 8 + x: the DMA request bit of each event (40.4.4). */
    const uint32_t bits = (use == TIM_BM_USE_DMA) ? (ev_mask << TIM_DIER_UDE_Pos) : ev_mask;
    const uint32_t pm = irq_save();
    if (use == TIM_BM_USE_DMA)
    {
        dier_dma(tim, ev_mask, set);
    }
    else
    {
        dier_irq(tim, ev_mask, set);
    }
    irq_restore(pm);
    return ((LL_TIM_ReadReg(tim, DIER) & bits) == (set ? bits : 0U)) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

TimBm_Status_t TIM_LL_EnableIrq(TIM_TypeDef *tim, uint32_t ev_mask)
{
    return dier_common(tim, ev_mask, TIM_BM_USE_IRQ, true);
}

TimBm_Status_t TIM_LL_DisableIrq(TIM_TypeDef *tim, uint32_t ev_mask)
{
    return dier_common(tim, ev_mask, TIM_BM_USE_IRQ, false);
}

TimBm_Status_t TIM_LL_EnableDmaRequest(TIM_TypeDef *tim, uint32_t ev_mask)
{
    return dier_common(tim, ev_mask, TIM_BM_USE_DMA, true);
}

TimBm_Status_t TIM_LL_DisableDmaRequest(TIM_TypeDef *tim, uint32_t ev_mask)
{
    return dier_common(tim, ev_mask, TIM_BM_USE_DMA, false);
}

TimBm_Status_t TIM_LL_GenerateEvent(TIM_TypeDef *tim, uint32_t ev_mask)
{
    const TimBm_Status_t st = TIM_BM_CheckEvents(tim, ev_mask, TIM_BM_USE_IRQ);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    /* EGR is write-only and each LL inline writes one bit: the events are generated
       one at a time, in bit order (tim_bm writes the whole mask at once). */
    if ((ev_mask & LL_TIM_DIER_UIE) != 0U)   { LL_TIM_GenerateEvent_UPDATE(tim); }
    if ((ev_mask & LL_TIM_DIER_CC1IE) != 0U) { LL_TIM_GenerateEvent_CC1(tim); }
    if ((ev_mask & LL_TIM_DIER_CC2IE) != 0U) { LL_TIM_GenerateEvent_CC2(tim); }
    if ((ev_mask & LL_TIM_DIER_CC3IE) != 0U) { LL_TIM_GenerateEvent_CC3(tim); }
    if ((ev_mask & LL_TIM_DIER_CC4IE) != 0U) { LL_TIM_GenerateEvent_CC4(tim); }
    if ((ev_mask & LL_TIM_DIER_TIE) != 0U)   { LL_TIM_GenerateEvent_TRIG(tim); }
    return TIM_BM_OK;
}

uint32_t TIM_LL_TakeFlags(TIM_TypeDef *tim, uint32_t ev_mask)
{
    return TIM_BM_TakeFlags(tim, ev_mask);     /* one SR write and DSB, in one place */
}

uint32_t TIM_LL_TakeOvercapture(TIM_TypeDef *tim, uint32_t ch_mask)
{
    return ll_ch_mask(TIM_BM_TakeOvercapture(tim, bm_ch_mask(ch_mask)));
}

TimBm_Status_t TIM_LL_WaitFlags(TIM_TypeDef *tim, uint32_t ev_mask, uint32_t max_cycles, uint32_t *taken)
{
    return TIM_BM_WaitFlags(tim, ev_mask, max_cycles, taken);
}

TimBm_Status_t TIM_LL_GetIRQn(const TIM_TypeDef *tim, uint32_t ev, IRQn_Type *out)
{
    return TIM_BM_GetIRQn(tim, ev, out);
}

TimBm_Status_t TIM_LL_GetDmaRequest(const TIM_TypeDef *tim, uint32_t ev, uint32_t *request_id)
{
    return TIM_BM_GetDmaRequest(tim, ev, request_id);
}

/* ==========================================================================
 * Output compare / PWM
 * ========================================================================== */

TimBm_Status_t TIM_LL_ConfigOutput(const TimLl_OcConfig_t *cfg)
{
    if (cfg == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_OcConfig_t bm = {
        .tim = cfg->tim, .channel = bm_channel(cfg->channel), .mode = bm_oc_mode(cfg->oc_mode),
        .compare = cfg->compare, .active_low = cfg->active_low, .preload = cfg->preload,
    };
    const TimBm_Status_t st = TIM_BM_CheckOutput(&bm);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    TIM_TypeDef *tim = cfg->tim;
    const uint32_t llch = cfg->channel;
    const uint32_t ch = bm.channel;

    /* Rule 5: CCMRx (CCxS = 00, OCxM, OCxPE, OCxFE = OCxCE = 0) -> CCRx -> CCER. */
    uint32_t pm = irq_save();
    LL_TIM_OC_SetMode(tim, llch, cfg->oc_mode);        /* CCxS = 00 and OCxM[3:0] */
    if (cfg->preload)
    {
        LL_TIM_OC_EnablePreload(tim, llch);
    }
    else
    {
        LL_TIM_OC_DisablePreload(tim, llch);
    }
    LL_TIM_OC_DisableFast(tim, llch);
    LL_TIM_OC_DisableClear(tim, llch);
    irq_restore(pm);
    ccr_write(tim, ch, cfg->compare);
    pm = irq_save();
    /* CCxP from active_low and CCxNP = 0 in one read-modify-write: the CCER field
       LL_TIM_IC_SetPolarity writes. LL_TIM_OC_SetPolarity leaves CCxNP alone, and
       the header has no LL_TIM_CHANNEL_CH4N to clear CC4NP with. */
    LL_TIM_IC_SetPolarity(tim, llch, cfg->active_low ? LL_TIM_IC_POLARITY_FALLING : LL_TIM_IC_POLARITY_RISING);
    irq_restore(pm);

    const uint32_t field = ((bm.mode << 4) | (cfg->preload ? (1UL << 3) : 0U)) << ccmr_shift(ch);
    const uint32_t pol = cfg->active_low ? (TIM_CCER_CC1P << (4U * (ch - 1U))) : 0U;
    if (((ccmr_read(tim, ch) & ccmr_field_mask(ch)) != field) || (ccr_read(tim, ch) != cfg->compare) ||
        ((LL_TIM_ReadReg(tim, CCER) & ccer_pol_mask(ch)) != pol))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_LL_SetCompare(TIM_TypeDef *tim, uint32_t channel, uint32_t ccr)
{
    const uint32_t ch = bm_channel(channel);
    const TimBm_Status_t st = TIM_BM_CheckCompare(tim, ch, ccr);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    ccr_write(tim, ch, ccr);
    return (ccr_read(tim, ch) == ccr) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

TimBm_Status_t TIM_LL_OutputEnable(TIM_TypeDef *tim, uint32_t ch_mask)
{
    const TimBm_Status_t st = TIM_BM_CheckChannels(tim, bm_ch_mask(ch_mask), TIM_BM_USE_OUTPUT_ON);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    const bool bdtr = has_bdtr(tim);
    const uint32_t pm = irq_save();
    LL_TIM_CC_EnableChannel(tim, ch_mask);
    if (bdtr)
    {
        LL_TIM_EnableAllOutputs(tim);          /* MOE last (C11) */
    }
    irq_restore(pm);
    if (((LL_TIM_ReadReg(tim, CCER) & ch_mask) != ch_mask) || (bdtr && (LL_TIM_IsEnabledAllOutputs(tim) == 0U)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_LL_OutputDisable(TIM_TypeDef *tim, uint32_t ch_mask)
{
    const TimBm_Status_t st = TIM_BM_CheckChannels(tim, bm_ch_mask(ch_mask), TIM_BM_USE_OUTPUT_OFF);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    const bool bdtr = has_bdtr(tim);
    bool moe_off = false;
    const uint32_t pm = irq_save();
    LL_TIM_CC_DisableChannel(tim, ch_mask);
    if (bdtr && ((LL_TIM_ReadReg(tim, CCER) & LL_CH_ALL) == 0U))
    {
        LL_TIM_DisableAllOutputs(tim);
        moe_off = true;
    }
    irq_restore(pm);
    if (((LL_TIM_ReadReg(tim, CCER) & ch_mask) != 0U) || (moe_off && (LL_TIM_IsEnabledAllOutputs(tim) != 0U)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

/* ==========================================================================
 * Input capture
 * ========================================================================== */

/* One input channel, as tim_bm's ic_write: the whole CCMRx field (OCxM[3] has no
   input setter, so LL_TIM_OC_SetMode(FROZEN) clears it and CCxS first; CCxE = 0,
   so the channel drives nothing meanwhile), then CCER CCxP/CCxNP. */
static void ic_write(TIM_TypeDef *tim, uint32_t llch, uint32_t active_input, uint32_t prescaler,
                     uint32_t filter, uint32_t polarity)
{
    const uint32_t pm = irq_save();
    LL_TIM_OC_SetMode(tim, llch, LL_TIM_OCMODE_FROZEN);
    LL_TIM_IC_Config(tim, llch, active_input | prescaler | filter | polarity);
    irq_restore(pm);
}

static bool ic_verify(const TIM_TypeDef *tim, uint32_t ch, uint32_t active_input, uint32_t prescaler,
                      uint32_t filter, uint32_t polarity)
{
    const uint32_t field = ((active_input | prescaler | filter) >> 16) << ccmr_shift(ch);
    return ((ccmr_read(tim, ch) & ccmr_field_mask(ch)) == field) &&
           ((LL_TIM_ReadReg(tim, CCER) & ccer_pol_mask(ch)) == (polarity << (4U * (ch - 1U))));
}

/* TIxSEL of input ti only: LL_TIM_SetRemap writes the four fields, so the other
   three are written back as they were (one read-modify-write under PRIMASK). */
static void tisel_write(TIM_TypeDef *tim, uint32_t ti, uint32_t value)
{
    const uint32_t sh = 8U * (ti - 1U);
    const uint32_t pm = irq_save();
    LL_TIM_SetRemap(tim, (LL_TIM_ReadReg(tim, TISEL) & ~(TIM_TISEL_TI1SEL << sh)) | (value << sh));
    irq_restore(pm);
}

static bool tisel_verify(const TIM_TypeDef *tim, uint32_t ti, uint32_t value)
{
    return ((LL_TIM_ReadReg(tim, TISEL) >> (8U * (ti - 1U))) & TIM_TISEL_TI1SEL) == value;
}

TimBm_Status_t TIM_LL_ConfigInput(const TimLl_IcConfig_t *cfg)
{
    if (cfg == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const uint32_t ch = bm_channel(cfg->channel);
    const TimBm_IcConfig_t bm = {
        .tim = cfg->tim, .channel = ch, .selection = bm_ic_selection(cfg->active_input),
        .edge = bm_edge(cfg->polarity), .prescaler = bm_ic_prescaler(cfg->prescaler),
        .filter = bm_filter(cfg->filter), .tisel = bm_tisel(cfg->remap, ch),
    };
    const TimBm_Status_t st = TIM_BM_CheckInput(&bm);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    TIM_TypeDef *tim = cfg->tim;
    ic_write(tim, cfg->channel, cfg->active_input, cfg->prescaler, cfg->filter, cfg->polarity);
    tisel_write(tim, ch, bm.tisel);
    if (!ic_verify(tim, ch, cfg->active_input, cfg->prescaler, cfg->filter, cfg->polarity) ||
        !tisel_verify(tim, ch, bm.tisel))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_LL_ConfigPwmInput(TIM_TypeDef *tim, uint32_t channel, uint32_t polarity,
                                     uint32_t prescaler, uint32_t filter, uint32_t remap)
{
    const uint32_t ti = (channel == LL_TIM_CHANNEL_CH1) ? 1U : ((channel == LL_TIM_CHANNEL_CH2) ? 2U : 0U);
    const uint32_t tisel = bm_tisel(remap, ti);
    const TimBm_Status_t st = TIM_BM_CheckPwmInput(tim, ti, bm_edge(polarity), bm_ic_prescaler(prescaler),
                                                   bm_filter(filter), tisel);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    const uint32_t other = (ti == 1U) ? LL_TIM_CHANNEL_CH2 : LL_TIM_CHANNEL_CH1;
    const uint32_t opposite = (polarity == LL_TIM_IC_POLARITY_RISING) ? LL_TIM_IC_POLARITY_FALLING
                                                                      : LL_TIM_IC_POLARITY_RISING;
    const uint32_t trigger = (ti == 1U) ? LL_TIM_TS_TI1FP1 : LL_TIM_TS_TI2FP2;

    /* RM0399 40.3.8, in tim_bm's order: TIti direct, the other channel indirect on
       the opposite edge, TIxSEL, then the slave controller SMS = 0 -> TS -> reset. */
    ic_write(tim, channel, LL_TIM_ACTIVEINPUT_DIRECTTI, prescaler, filter, polarity);
    ic_write(tim, other, LL_TIM_ACTIVEINPUT_INDIRECTTI, prescaler, filter, opposite);
    tisel_write(tim, ti, tisel);
    const uint32_t pm = irq_save();
    LL_TIM_SetSlaveMode(tim, LL_TIM_SLAVEMODE_DISABLED);
    LL_TIM_SetTriggerInput(tim, trigger);
    LL_TIM_SetSlaveMode(tim, LL_TIM_SLAVEMODE_RESET);
    irq_restore(pm);

    if (!ic_verify(tim, ti, LL_TIM_ACTIVEINPUT_DIRECTTI, prescaler, filter, polarity) ||
        !ic_verify(tim, 3U - ti, LL_TIM_ACTIVEINPUT_INDIRECTTI, prescaler, filter, opposite) ||
        !tisel_verify(tim, ti, tisel) ||
        ((LL_TIM_ReadReg(tim, SMCR) & (TIM_SMCR_TS | TIM_SMCR_SMS)) != (trigger | LL_TIM_SLAVEMODE_RESET)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

static TimBm_Status_t capture_common(TIM_TypeDef *tim, uint32_t ch_mask, bool set)
{
    const TimBm_Status_t st = TIM_BM_CheckChannels(tim, bm_ch_mask(ch_mask), TIM_BM_USE_CAPTURE);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    const uint32_t pm = irq_save();
    if (set)
    {
        LL_TIM_CC_EnableChannel(tim, ch_mask);
    }
    else
    {
        LL_TIM_CC_DisableChannel(tim, ch_mask);
    }
    irq_restore(pm);
    return ((LL_TIM_ReadReg(tim, CCER) & ch_mask) == (set ? ch_mask : 0U)) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

TimBm_Status_t TIM_LL_CaptureEnable(TIM_TypeDef *tim, uint32_t ch_mask)
{
    return capture_common(tim, ch_mask, true);
}

TimBm_Status_t TIM_LL_CaptureDisable(TIM_TypeDef *tim, uint32_t ch_mask)
{
    return capture_common(tim, ch_mask, false);
}

/* ==========================================================================
 * Master / slave
 * ========================================================================== */

TimBm_Status_t TIM_LL_ConfigMaster(TIM_TypeDef *tim, uint32_t trgo, bool msm)
{
    const TimBm_Status_t st = TIM_BM_CheckMaster(tim, bm_mms(trgo), msm);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    TimBm_Caps_t caps;
    (void)TIM_BM_GetCaps(tim, &caps);
    const uint32_t pm = irq_save();
    LL_TIM_SetTriggerOutput(tim, trgo);
    if (caps.slave_mode)
    {
        if (msm)
        {
            LL_TIM_EnableMasterSlaveMode(tim);
        }
        else
        {
            LL_TIM_DisableMasterSlaveMode(tim);
        }
    }
    irq_restore(pm);
    if (((LL_TIM_ReadReg(tim, CR2) & TIM_CR2_MMS) != trgo) ||
        (caps.slave_mode && (((LL_TIM_ReadReg(tim, SMCR) & TIM_SMCR_MSM) != 0U) != msm)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_LL_ConfigSlave(TIM_TypeDef *tim, uint32_t slave_mode, uint32_t trigger)
{
    const TimBm_Status_t st = TIM_BM_CheckSlave(tim, bm_sms(slave_mode), bm_trigger(trigger));
    if (st != TIM_BM_OK)
    {
        return st;
    }
    /* RM0399 40.4.3, TS note: SMS = 0 -> TS -> SMS. */
    const uint32_t pm = irq_save();
    LL_TIM_SetSlaveMode(tim, LL_TIM_SLAVEMODE_DISABLED);
    LL_TIM_SetTriggerInput(tim, trigger);
    LL_TIM_SetSlaveMode(tim, slave_mode);
    irq_restore(pm);
    if ((LL_TIM_ReadReg(tim, SMCR) & (TIM_SMCR_TS | TIM_SMCR_SMS)) != (trigger | slave_mode))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}
