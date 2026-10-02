/**
  ******************************************************************************
  * @file    tim_bm.c
  * @brief   TIM1-8, TIM12-17, bare-metal tier. Contract in tim_bm.h.
  *
  *          Sources: RM0399 rev 4, as the .txt chapters in
  *          00_docs_stm32h7/docs/peripherals/ (timers 40..44_*.txt, RCC
  *          09_*.txt, DMAMUX 18_*.txt; the line numbers below are of those
  *          files), ES0445 rev 6 2.16 and the CMSIS device header stm32h755xx.h
  *          (interrupt numbers).
  ******************************************************************************
  */

#include "tim_bm.h"

/* C8: stm32h7xx.h, <stdint.h> and <stdbool.h> only, so no <stddef.h>. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* ==========================================================================
 * Capability table (C1, plan rule 2): one row per instance, the only place the
 * driver learns a silicon fact. Every validation below reads it.
 * ========================================================================== */

/* Register-layout facts that are not in TimBm_Caps_t. */
#define F_UDC      (1U << 0)   /* CR1.DIR and CR1.CMS (up/down/center)            */
#define F_BDTR     (1U << 1)   /* BDTR (MOE)                                      */
#define F_SLAVE    (1U << 2)   /* SMCR                                            */
#define F_MASTER   (1U << 3)   /* CR2.MMS                                         */
#define F_CR2      (1U << 4)   /* CR2 exists (TIM16/17: without MMS)              */
#define F_DCR      (1U << 5)   /* DCR exists                                      */
#define F_AF1      (1U << 6)   /* AF1 exists (reset value in af_reset)            */
#define F_AF2      (1U << 7)   /* AF2 exists (reset value 1)                      */
#define F_CH56     (1U << 8)   /* CCMR3, CCR5, CCR6 (TIM1/TIM8)                   */
#define F_ARR32R   (1U << 9)   /* ARR resets to 0xFFFF FFFF (TIM2-5, 41.4.15 l. 3199) */

#define IRQ_NONE   ((IRQn_Type)(-128))

typedef struct
{
    uint32_t  base;
    uint8_t   apb2;            /* 0: RCC_APB1LENR, 1: RCC_APB2ENR (this core's bank) */
    uint8_t   rcc_bit;
    uint8_t   counter_bits;
    uint8_t   channels;
    uint16_t  itr_mask;
    uint8_t   mms_max;
    uint8_t   rcr_bits;        /* 0, 8 or 16                                       */
    uint16_t  flags;           /* F_*                                              */
    uint8_t   af1_reset;       /* reset value of AF1 where F_AF1                   */
    uint16_t  tisel_mask[4];
    IRQn_Type irq_up;
    IRQn_Type irq_cc;
    IRQn_Type irq_trg;
    uint8_t   mux_up;          /* DMAMUX1 request (RM0399 table 126), 0 = none     */
    uint8_t   mux_cc[4];
} TimEntry_t;

#define ADV   (F_UDC | F_BDTR | F_SLAVE | F_MASTER | F_CR2 | F_DCR | F_AF1 | F_AF2 | F_CH56)
#define GP4   (F_UDC | F_SLAVE | F_MASTER | F_CR2 | F_DCR | F_AF1 | F_ARR32R)

static const TimEntry_t k_tim[] = {
    /* TIM1: 40_*.txt. ITR0-3 = TIM15, TIM2, TIM3, TIM4 (table 353, l. 3214); MMS 0-7
       (40.4.2, l. 3050); RCR REP[15:0] (40.4.15, l. 3970); BDTR (40.4.20, l. 4088);
       TI1SEL 0000 CH1, 0001 COMP1, TI2-4SEL 0000 only (40.4.30, l. 4769); AF1 and AF2
       reset 0x1 (40.4.26 l. 4432, 40.4.27 l. 4523). DMAMUX1 11-15 (18_*.txt table 126,
       l. 76). Vectors TIM1_UP/CC/TRG_COM (stm32h755xx.h). */
    { TIM1_BASE,  1U, RCC_APB2ENR_TIM1EN_Pos,   16U, 4U, 0x000FU, 7U, 16U, ADV, 1U,
      { 0x0003U, 0x0001U, 0x0001U, 0x0001U },
      TIM1_UP_IRQn, TIM1_CC_IRQn, TIM1_TRG_COM_IRQn, 15U, { 11U, 12U, 13U, 14U } },
    /* TIM2: 41_*.txt. 32-bit (l. 21); ITR0-6 = TIM1, TIM8, TIM3, TIM4, ETH PPS, OTG HS
       SOF, OTG FS SOF (table 358, l. 2604); MMS 0-7 (41.4.2, l. 2448); TI4SEL 0001-0011
       COMP1/COMP2/both, TI1-3SEL 0000 only (41.4.26, l. 3464); AF1 reset 0 (41.4.22,
       l. 3374). DMAMUX1 18-22. */
    { TIM2_BASE,  0U, RCC_APB1LENR_TIM2EN_Pos,  32U, 4U, 0x007FU, 7U, 0U, GP4, 0U,
      { 0x0001U, 0x0001U, 0x0001U, 0x000FU },
      TIM2_IRQn, TIM2_IRQn, TIM2_IRQn, 22U, { 18U, 19U, 20U, 21U } },
    /* TIM3: ITR0-4 = TIM1, TIM2, TIM15, TIM4, ETH PPS (table 358); TI1SEL 0001-0011
       COMP1/COMP2/both (41.4.27, l. 3501); AF1 reset 0 (41.4.23, l. 3399). DMAMUX1 23-27. */
    { TIM3_BASE,  0U, RCC_APB1LENR_TIM3EN_Pos,  16U, 4U, 0x001FU, 7U, 0U, GP4, 0U,
      { 0x000FU, 0x0001U, 0x0001U, 0x0001U },
      TIM3_IRQn, TIM3_IRQn, TIM3_IRQn, 27U, { 23U, 24U, 25U, 26U } },
    /* TIM4: ITR0-3 = TIM1, TIM2, TIM3, TIM8 (table 358); TISEL 0000 only (41.4.28,
       l. 3541); AF1 reset 0 (41.4.24, l. 3423). DMAMUX1 29-32: no TIM4_CH4 request. */
    { TIM4_BASE,  0U, RCC_APB1LENR_TIM4EN_Pos,  16U, 4U, 0x000FU, 7U, 0U, GP4, 0U,
      { 0x0001U, 0x0001U, 0x0001U, 0x0001U },
      TIM4_IRQn, TIM4_IRQn, TIM4_IRQn, 32U, { 29U, 30U, 31U, 0U } },
    /* TIM5: 32-bit (l. 21); ITR0-3 = TIM1, TIM8, TIM3, TIM4, ITR6-8 = fdcan1_soc, OTG HS
       SOF, OTG FS SOF (table 358); TI1SEL 0001-0010 fdcan1_tmp/rtp (41.4.29, l. 3577);
       AF1 reset 0 (41.4.25, l. 3442). DMAMUX1 55-59. */
    { TIM5_BASE,  0U, RCC_APB1LENR_TIM5EN_Pos,  32U, 4U, 0x01CFU, 7U, 0U, GP4, 0U,
      { 0x0007U, 0x0001U, 0x0001U, 0x0001U },
      TIM5_IRQn, TIM5_IRQn, TIM5_IRQn, 59U, { 55U, 56U, 57U, 58U } },
    /* TIM6, TIM7: 44_*.txt. No channel, MMS 0-2 (44.4.2, l. 476), DIER UDE/UIE only
       (44.4.3, l. 496), no SMCR, no TISEL. DMAMUX1 69/70. */
    { TIM6_BASE,  0U, RCC_APB1LENR_TIM6EN_Pos,  16U, 0U, 0x0000U, 2U, 0U, F_MASTER | F_CR2, 0U,
      { 0U, 0U, 0U, 0U },
      TIM6_DAC_IRQn, IRQ_NONE, IRQ_NONE, 69U, { 0U, 0U, 0U, 0U } },
    { TIM7_BASE,  0U, RCC_APB1LENR_TIM7EN_Pos,  16U, 0U, 0x0000U, 2U, 0U, F_MASTER | F_CR2, 0U,
      { 0U, 0U, 0U, 0U },
      TIM7_IRQn, IRQ_NONE, IRQ_NONE, 70U, { 0U, 0U, 0U, 0U } },
    /* TIM8: as TIM1; ITR0-3 = TIM1, TIM2, TIM4, TIM5 (table 353); TI1SEL 0001 COMP2
       (40.4.31, l. 4799); AF1/AF2 reset 0x1 (40.4.28 l. 4599, 40.4.29 l. 4692).
       DMAMUX1 47-51. Vectors shared with TIM12/13/14. */
    { TIM8_BASE,  1U, RCC_APB2ENR_TIM8EN_Pos,   16U, 4U, 0x000FU, 7U, 16U, ADV, 1U,
      { 0x0003U, 0x0001U, 0x0001U, 0x0001U },
      TIM8_UP_TIM13_IRQn, TIM8_CC_IRQn, TIM8_TRG_COM_TIM14_IRQn, 51U, { 47U, 48U, 49U, 50U } },
    /* TIM12: 42_*.txt. Up only (42.4.1, l. 1264); ITR0-3 = TIM4, TIM5, TIM13 OC1, TIM14
       OC1 (table 361, l. 1426); MMS 0-5 (42.4.2, l. 1337); TI1SEL 0001 spdifrx_frame_sync
       (42.4.15, l. 1884); no RCR, BDTR, DCR or AF1; no DMA request (table 126). */
    { TIM12_BASE, 0U, RCC_APB1LENR_TIM12EN_Pos, 16U, 2U, 0x000FU, 5U, 0U, F_SLAVE | F_MASTER | F_CR2, 0U,
      { 0x0003U, 0x0001U, 0U, 0U },
      TIM8_BRK_TIM12_IRQn, TIM8_BRK_TIM12_IRQn, TIM8_BRK_TIM12_IRQn, 0U, { 0U, 0U, 0U, 0U } },
    /* TIM13, TIM14: one channel, no CR2 (so no TRGO) and no SMCR (42.5.1-42.5.13);
       TISEL 0000 only (42.5.12 l. 2789, 42.5.13 l. 2805); no DMA request. */
    { TIM13_BASE, 0U, RCC_APB1LENR_TIM13EN_Pos, 16U, 1U, 0x0000U, 0U, 0U, 0U, 0U,
      { 0x0001U, 0U, 0U, 0U },
      TIM8_UP_TIM13_IRQn, TIM8_UP_TIM13_IRQn, IRQ_NONE, 0U, { 0U, 0U, 0U, 0U } },
    { TIM14_BASE, 0U, RCC_APB1LENR_TIM14EN_Pos, 16U, 1U, 0x0000U, 0U, 0U, 0U, 0U,
      { 0x0001U, 0U, 0U, 0U },
      TIM8_TRG_COM_TIM14_IRQn, TIM8_TRG_COM_TIM14_IRQn, IRQ_NONE, 0U, { 0U, 0U, 0U, 0U } },
    /* TIM15: 43_*.txt. Up only (43.5.1, l. 1811); ITR0-3 = TIM1, TIM3, TIM16 OC1, TIM17
       OC1 (table 366, l. 2004); MMS 0-5 (43.5.2, l. 1899); REP[7:0] (43.5.13, l. 2531);
       TI1SEL 0001-0110 TIM2/3/4_CH1, LSE, CSI, MCO2, TI2SEL 0001-0011 TIM2/3/4_CH2
       (43.5.20, l. 2830); AF1 reset 0x1 (43.5.19, l. 2762). DMAMUX1 105/106: no CH2. */
    { TIM15_BASE, 1U, RCC_APB2ENR_TIM15EN_Pos,  16U, 2U, 0x000FU, 5U, 8U,
      F_BDTR | F_SLAVE | F_MASTER | F_CR2 | F_DCR | F_AF1, 1U,
      { 0x007FU, 0x000FU, 0U, 0U },
      TIM15_IRQn, TIM15_IRQn, TIM15_IRQn, 106U, { 105U, 0U, 0U, 0U } },
    /* TIM16, TIM17: CR2 without MMS (43.6.2, l. 3605), no SMCR; REP[7:0] (43.6.12,
       l. 4072); BDTR (43.6.14, l. 4112); TIM16 TI1SEL 0001-0011 LSI/LSE/WKUP_IT
       (43.6.18, l. 4357); TIM17 TI1SEL 0010 HSE_1MHz, 0011 MCO1, 0001 reserved
       (43.6.20, l. 4450); AF1 reset 0x1 (43.6.17 l. 4285, 43.6.19 l. 4378).
       DMAMUX1 109/110 and 111/112. */
    { TIM16_BASE, 1U, RCC_APB2ENR_TIM16EN_Pos,  16U, 1U, 0x0000U, 0U, 8U, F_BDTR | F_CR2 | F_DCR | F_AF1, 1U,
      { 0x000FU, 0U, 0U, 0U },
      TIM16_IRQn, TIM16_IRQn, IRQ_NONE, 110U, { 109U, 0U, 0U, 0U } },
    { TIM17_BASE, 1U, RCC_APB2ENR_TIM17EN_Pos,  16U, 1U, 0x0000U, 0U, 8U, F_BDTR | F_CR2 | F_DCR | F_AF1, 1U,
      { 0x000DU, 0U, 0U, 0U },
      TIM17_IRQn, TIM17_IRQn, IRQ_NONE, 112U, { 111U, 0U, 0U, 0U } },
};

#define TIM_COUNT   (sizeof(k_tim) / sizeof(k_tim[0]))

/* ==========================================================================
 * Register fields used below (RM0399 40.4.x; the CMSIS names are not used for
 * the channel fields because the driver indexes them by channel).
 * ========================================================================== */
#define EV_ALL      (TIM_BM_EV_UPDATE | TIM_BM_EV_CC1 | TIM_BM_EV_CC2 | TIM_BM_EV_CC3 | \
                     TIM_BM_EV_CC4 | TIM_BM_EV_TRIGGER)
#define EV_DMA      (TIM_BM_EV_UPDATE | TIM_BM_EV_CC1 | TIM_BM_EV_CC2 | TIM_BM_EV_CC3 | TIM_BM_EV_CC4)
#define DIER_DMA_SHIFT  8U                      /* UDE = bit 8, CCxDE = bit 8 + x */
#define SR_CCOF_SHIFT   9U                      /* CC1OF = bit 9                  */

#define CR1_CEN     (1UL << 0)
#define CR1_URS     (1UL << 2)
#define CR1_OPM     (1UL << 3)
#define CR1_DIR     (1UL << 4)
#define CR1_CMS_POS 5U
#define CR1_CMS_MSK (3UL << CR1_CMS_POS)
#define CR1_ARPE    (1UL << 7)

#define CR2_MMS_POS 4U
#define CR2_MMS_MSK (7UL << CR2_MMS_POS)

#define SMCR_SMS_MSK  ((1UL << 16) | 7UL)       /* SMS[3] bit 16, SMS[2:0] bits 2:0   */
#define SMCR_TS_MSK   ((3UL << 20) | (7UL << 4)) /* TS[4:3] bits 21:20, TS[2:0] 6:4   */
#define SMCR_MSM      (1UL << 7)

#define BDTR_MOE    (1UL << 15)
#define EGR_UG      (1UL << 0)

/* ---- PRIMASK section (C10): saved and restored, never unconditionally enabled - */
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

/* ==========================================================================
 * Validation (C6): pure, no peripheral access
 * ========================================================================== */

static const TimEntry_t *entry(const TIM_TypeDef *tim)
{
    if (tim == NULL)
    {
        return NULL;
    }
    for (uint32_t i = 0U; i < TIM_COUNT; i++)
    {
        if (k_tim[i].base == (uint32_t)tim)
        {
            return &k_tim[i];
        }
    }
    return NULL;
}

static uint32_t counter_max(const TimEntry_t *e)
{
    return (e->counter_bits == 32U) ? 0xFFFFFFFFUL : 0xFFFFUL;
}

/* Events the instance has: UIF always, CCxIF per channel, TIF with a slave
   controller (TIE/TIF/TG exist only there: 41.4.4, 42.4.4, 43.5.4). */
static uint32_t ev_valid(const TimEntry_t *e)
{
    uint32_t m = TIM_BM_EV_UPDATE;
    for (uint32_t ch = 1U; ch <= e->channels; ch++)
    {
        m |= 1UL << ch;
    }
    if ((e->flags & F_SLAVE) != 0U)
    {
        m |= TIM_BM_EV_TRIGGER;
    }
    return m;
}

/* An event mask: PARAM if empty or outside TIM_BM_EV_*, UNSUPPORTED if the
   instance lacks one of the events. */
static TimBm_Status_t ev_check(const TimEntry_t *e, uint32_t ev_mask)
{
    if ((ev_mask == 0U) || ((ev_mask & ~EV_ALL) != 0U))
    {
        return TIM_BM_ERR_PARAM;
    }
    if ((ev_mask & ~ev_valid(e)) != 0U)
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    return TIM_BM_OK;
}

/* A single channel 1..4: PARAM outside 1..4, UNSUPPORTED beyond the instance's. */
static TimBm_Status_t ch_check(const TimEntry_t *e, uint32_t channel)
{
    if ((channel == 0U) || (channel > 4U))
    {
        return TIM_BM_ERR_PARAM;
    }
    if (channel > e->channels)
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    return TIM_BM_OK;
}

/* A channel mask TIM_BM_CH(1..4). */
static TimBm_Status_t chmask_check(const TimEntry_t *e, uint32_t ch_mask)
{
    if ((ch_mask == 0U) || ((ch_mask & ~0xFUL) != 0U))
    {
        return TIM_BM_ERR_PARAM;
    }
    if ((ch_mask >> e->channels) != 0U)
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    return TIM_BM_OK;
}

/* CCER bits CCxE of a channel mask: CC1E bit 0, CC2E bit 4, ... */
static uint32_t ccer_e_bits(uint32_t ch_mask)
{
    uint32_t b = 0U;
    for (uint32_t i = 0U; i < 4U; i++)
    {
        if ((ch_mask & (1UL << i)) != 0U)
        {
            b |= 1UL << (4U * i);
        }
    }
    return b;
}

/* CCMRx of a channel and the shift of its 8-bit field (OCxM[3] at 16 + shift). */
static volatile uint32_t *ccmr_reg(TIM_TypeDef *tim, uint32_t channel)
{
    return (channel <= 2U) ? &tim->CCMR1 : &tim->CCMR2;
}

static uint32_t ccmr_shift(uint32_t channel)
{
    return ((channel - 1U) & 1U) * 8U;
}

static uint32_t ccmr_field_mask(uint32_t channel)
{
    const uint32_t sh = ccmr_shift(channel);
    return (0xFFUL << sh) | (1UL << (16U + sh));
}

/* CCxS of a channel, read: 0 = output. Reads only. */
static uint32_t ccxs(TIM_TypeDef *tim, uint32_t channel)
{
    return (*ccmr_reg(tim, channel) >> ccmr_shift(channel)) & 3UL;
}

static bool prescaler_code(uint32_t prescaler, uint32_t *code)
{
    switch (prescaler)
    {
    case 1U: *code = 0U; return true;
    case 2U: *code = 1U; return true;
    case 4U: *code = 2U; return true;
    case 8U: *code = 3U; return true;
    default: return false;
    }
}

static bool edge_valid(uint32_t edge)
{
    return (edge == TIM_BM_EDGE_RISING) || (edge == TIM_BM_EDGE_FALLING) || (edge == TIM_BM_EDGE_BOTH);
}

/* CCER polarity bits of a channel for an input edge: CCxP = bit 0 of edge,
   CCxNP = bit 1 (RM0399 40.4.11: 00 rising, 01 falling, 11 both). */
static uint32_t ccer_edge_bits(uint32_t channel, uint32_t edge)
{
    const uint32_t sh = 4U * (channel - 1U);
    return (((edge & 1U) != 0U) ? (2UL << sh) : 0U) | (((edge & 2U) != 0U) ? (8UL << sh) : 0U);
}

static uint32_t ccer_pol_mask(uint32_t channel)
{
    return 0xAUL << (4U * (channel - 1U));    /* CCxP | CCxNP */
}

/* TS[4:0] of a driver trigger value, false if it is not one. */
static bool ts_code(uint32_t trigger, uint32_t *ts)
{
    if (trigger <= 3U)
    {
        *ts = trigger;                         /* ITR0-3: 00000-00011 */
    }
    else if (trigger <= 8U)
    {
        *ts = trigger + 4U;                    /* ITR4-8: 01000-01100 (41.4.3) */
    }
    else if (trigger == TIM_BM_TRG_TI1F_ED)
    {
        *ts = 4U;
    }
    else if (trigger == TIM_BM_TRG_TI1FP1)
    {
        *ts = 5U;
    }
    else if (trigger == TIM_BM_TRG_TI2FP2)
    {
        *ts = 6U;
    }
    else
    {
        return false;                          /* ETRF (00111) has no driver value */
    }
    return true;
}

static uint32_t smcr_ts_bits(uint32_t ts)
{
    return ((ts & 7UL) << 4) | (((ts >> 3) & 3UL) << 20);
}

static uint32_t smcr_sms_bits(uint32_t sms)
{
    return (sms & 7UL) | (((sms >> 3) & 1UL) << 16);
}

static uint32_t smcr_sms(uint32_t smcr)
{
    return (smcr & 7UL) | (((smcr >> 16) & 1UL) << 3);
}

/* ES0445 2.16.1: one-pulse master in reset + trigger with MSM = 1 can miss the
   trigger. True when the combination (opm, sms, msm) is that one. */
static bool errata_2_16_1(bool opm, uint32_t sms, bool msm)
{
    return opm && msm && (sms == TIM_BM_SMS_RESET_TRIGGER);
}

/* ==========================================================================
 * Capabilities
 * ========================================================================== */

bool TIM_BM_IsValidInstance(const TIM_TypeDef *tim)
{
    return entry(tim) != NULL;
}

TimBm_Status_t TIM_BM_GetCaps(const TIM_TypeDef *tim, TimBm_Caps_t *out)
{
    const TimEntry_t *e = entry(tim);
    if ((out == NULL) || (e == NULL))
    {
        return TIM_BM_ERR_PARAM;
    }
    out->counter_bits   = e->counter_bits;
    out->channels       = e->channels;
    out->itr_mask       = e->itr_mask;
    out->mms_max        = e->mms_max;
    out->up_down_center = (e->flags & F_UDC) != 0U;
    out->repetition     = e->rcr_bits != 0U;
    out->bdtr           = (e->flags & F_BDTR) != 0U;
    out->slave_mode     = (e->flags & F_SLAVE) != 0U;
    out->master_mode    = (e->flags & F_MASTER) != 0U;
    for (uint32_t i = 0U; i < 4U; i++)
    {
        out->tisel_mask[i] = e->tisel_mask[i];
    }
    return TIM_BM_OK;
}

/* ==========================================================================
 * Clock
 * ========================================================================== */

/* This core's enable register of the instance: RCC->APB1LENR / RCC->APB2ENR sit
   at offsets 0xE8 / 0xF0, in the per-core bank RM0399 9.7.1 maps at one address
   (rcc/09_*.txt l. 3347): CPU1 lands in RCC_C1, CPU2 in RCC_C2. */
static volatile uint32_t *enr_reg(const TimEntry_t *e)
{
    return (e->apb2 != 0U) ? &RCC->APB2ENR : &RCC->APB1LENR;
}

TimBm_Status_t TIM_BM_EnableClock(TIM_TypeDef *tim)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    volatile uint32_t *reg = enr_reg(e);
    const uint32_t bit = 1UL << e->rcc_bit;
    const uint32_t pm = irq_save();
    *reg |= bit;
    const uint32_t rb = *reg;                  /* the read that lets the enable land */
    irq_restore(pm);
    return ((rb & bit) != 0U) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

TimBm_Status_t TIM_BM_DisableClock(TIM_TypeDef *tim)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    volatile uint32_t *reg = enr_reg(e);
    const uint32_t bit = 1UL << e->rcc_bit;
    const uint32_t pm = irq_save();
    *reg &= ~bit;
    const uint32_t rb = *reg;
    irq_restore(pm);
    return ((rb & bit) == 0U) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

/* RM0399 table 59 (rcc/09_*.txt l. 1556): with TIMPRE = 0 the timer clock is
   hclk1 for D2PPRE = 0xx and 100, then hclk1/2, /4, /8 for 101, 110, 111; with
   TIMPRE = 1 it is hclk1 up to 101, then hclk1/2 and /4 for 110 and 111. */
TimBm_Status_t TIM_BM_KernelClockFromFields(uint32_t hclk1_hz, uint32_t d2ppre, uint32_t timpre,
                                            uint32_t *out_hz)
{
    if ((out_hz == NULL) || (d2ppre > 7U) || (timpre > 1U))
    {
        return TIM_BM_ERR_PARAM;
    }
    const uint32_t first_divided = (timpre == 0U) ? 5U : 6U;
    const uint32_t shift = (d2ppre < first_divided) ? 0U : (d2ppre - first_divided + 1U);
    *out_hz = hclk1_hz >> shift;
    return TIM_BM_OK;
}

TimBm_Status_t TIM_BM_GetKernelClockHz(const TIM_TypeDef *tim, uint32_t hclk1_hz, uint32_t *out_hz)
{
    const TimEntry_t *e = entry(tim);
    if ((out_hz == NULL) || (e == NULL))
    {
        return TIM_BM_ERR_PARAM;
    }
    const uint32_t d2cfgr = RCC->D2CFGR;
    const uint32_t d2ppre = (e->apb2 != 0U) ? ((d2cfgr & RCC_D2CFGR_D2PPRE2_Msk) >> RCC_D2CFGR_D2PPRE2_Pos)
                                            : ((d2cfgr & RCC_D2CFGR_D2PPRE1_Msk) >> RCC_D2CFGR_D2PPRE1_Pos);
    const uint32_t timpre = (RCC->CFGR & RCC_CFGR_TIMPRE_Msk) >> RCC_CFGR_TIMPRE_Pos;
    return TIM_BM_KernelClockFromFields(hclk1_hz, d2ppre, timpre, out_hz);
}

/* ==========================================================================
 * Pure arithmetic (C15): the rules of the plan, "Reglas de calculo"
 * ========================================================================== */

TimBm_Status_t TIM_BM_CalcTimeBase(uint32_t timer_clock_hz, uint32_t target_hz,
                                   uint32_t arr_max, TimBm_TimeBase_t *out)
{
    if ((out == NULL) || (target_hz == 0U) || (target_hz > (timer_clock_hz / 2U)) || (arr_max == 0U))
    {
        return TIM_BM_ERR_PARAM;
    }
    const uint64_t clk = timer_clock_hz;
    const uint64_t target = target_hz;
    const uint64_t span = target * ((uint64_t)arr_max + 1U);          /* <= 2^63 */
    const uint64_t psc = ((clk + span - 1U) / span) - 1U;             /* clk >= 2 target: ceil >= 1 */
    if (psc > 65535U)
    {
        return TIM_BM_ERR_RANGE;
    }
    const uint64_t d = (psc + 1U) * target;
    uint64_t n = (clk + (d / 2U)) / d;
    if (n > ((uint64_t)arr_max + 1U))
    {
        n = (uint64_t)arr_max + 1U;
    }
    if (n < 2U)
    {
        return TIM_BM_ERR_RANGE;
    }
    const uint64_t ticks = (psc + 1U) * n;
    const int64_t num = (int64_t)((clk * 1000000U) / ticks) - (int64_t)(target * 1000000U);
    out->psc = (uint32_t)psc;
    out->arr = (uint32_t)(n - 1U);
    out->ticks = ticks;
    out->error_ppm = (int32_t)(num / (int64_t)target);                /* C: toward zero */
    return TIM_BM_OK;
}

TimBm_Status_t TIM_BM_DutyToCompare(uint32_t arr, uint32_t counter_bits, uint32_t duty_x100, uint32_t *ccr)
{
    if ((ccr == NULL) || ((counter_bits != 16U) && (counter_bits != 32U)) || (duty_x100 > 10000U))
    {
        return TIM_BM_ERR_PARAM;
    }
    const uint64_t cmax = (counter_bits == 32U) ? 0xFFFFFFFFULL : 0xFFFFULL;
    if ((uint64_t)arr > cmax)
    {
        return TIM_BM_ERR_PARAM;
    }
    if ((duty_x100 == 10000U) && (((uint64_t)arr + 1U) > cmax))
    {
        return TIM_BM_ERR_RANGE;               /* 100 % needs CCR = ARR + 1 (40.3.11) */
    }
    *ccr = (uint32_t)(((((uint64_t)arr + 1U) * duty_x100) + 5000U) / 10000U);
    return TIM_BM_OK;
}

/* ==========================================================================
 * Time base
 * ========================================================================== */

TimBm_Status_t TIM_BM_InitTimeBase(const TimBm_TimeBaseConfig_t *cfg)
{
    if (cfg == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimEntry_t *e = entry(cfg->tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    /* capability */
    if ((cfg->count_mode != TIM_BM_COUNT_UP) && (cfg->count_mode <= TIM_BM_COUNT_CENTER3) &&
        ((e->flags & F_UDC) == 0U))
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    if ((cfg->repetition != 0U) && (e->rcr_bits == 0U))
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    /* field range */
    const uint32_t rcr_max = (e->rcr_bits == 16U) ? 0xFFFFU : ((e->rcr_bits == 8U) ? 0xFFU : 0U);
    if ((cfg->psc > 65535U) || (cfg->arr == 0U) || (cfg->arr > counter_max(e)) ||
        (cfg->count_mode > TIM_BM_COUNT_CENTER3) || (cfg->repetition > rcr_max))
    {
        return TIM_BM_ERR_PARAM;
    }
    TIM_TypeDef *tim = cfg->tim;
    /* state: reads only */
    if ((tim->CR1 & CR1_CEN) != 0U)
    {
        return TIM_BM_ERR_BUSY;
    }
    if (((e->flags & F_SLAVE) != 0U) && cfg->one_pulse)
    {
        const uint32_t smcr = tim->SMCR;
        if (errata_2_16_1(true, smcr_sms(smcr), (smcr & SMCR_MSM) != 0U))
        {
            return TIM_BM_ERR_UNSUPPORTED;    /* ES0445 2.16.1, the third way into it */
        }
    }

    uint32_t cr1 = 0U;
    if (cfg->count_mode == TIM_BM_COUNT_DOWN)
    {
        cr1 |= CR1_DIR;
    }
    else if (cfg->count_mode >= TIM_BM_COUNT_CENTER1)
    {
        cr1 |= (cfg->count_mode - TIM_BM_COUNT_CENTER1 + 1U) << CR1_CMS_POS;   /* CMS = 01..11 */
    }
    if (cfg->arr_preload)
    {
        cr1 |= CR1_ARPE;
    }
    if (cfg->one_pulse)
    {
        cr1 |= CR1_OPM;
    }
    if (cfg->urs_overflow_only)
    {
        cr1 |= CR1_URS;
    }

    const uint32_t pm = irq_save();
    /* DIR is read-only while CMS != 0 (40.4.1): leaving center mode takes one write that
       clears CMS before the one that sets DIR, or a DOWN after a center mode stays up. */
    tim->CR1 = CR1_URS;
    tim->CR1 = cr1 | CR1_URS;                  /* CEN = 0; URS = 1 until the UG below */
    tim->PSC = cfg->psc;
    tim->ARR = cfg->arr;
    if (e->rcr_bits != 0U)
    {
        tim->RCR = cfg->repetition;
    }
    tim->EGR = EGR_UG;                         /* loads PSC, ARR, RCR; no UIF with URS = 1 */
    tim->CR1 = cr1;
    tim->SR = 0U;
    __DSB();
    irq_restore(pm);

    /* In center-aligned mode DIR is a read-only status bit (40.4.1). */
    const uint32_t cr1_mask = (cfg->count_mode >= TIM_BM_COUNT_CENTER1) ? ~CR1_DIR : 0xFFFFFFFFUL;
    if (((tim->CR1 & 0xFFFFU & cr1_mask) != (cr1 & cr1_mask)) || (tim->PSC != cfg->psc) ||
        (tim->ARR != cfg->arr) || ((e->rcr_bits != 0U) && (tim->RCR != cfg->repetition)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_BM_SetPeriod(TIM_TypeDef *tim, uint32_t arr)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    if ((arr == 0U) || (arr > counter_max(e)))
    {
        return TIM_BM_ERR_PARAM;
    }
    tim->ARR = arr;
    return (tim->ARR == arr) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

TimBm_Status_t TIM_BM_Start(TIM_TypeDef *tim)
{
    if (entry(tim) == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const uint32_t pm = irq_save();
    tim->CR1 |= CR1_CEN;
    irq_restore(pm);
    return TIM_BM_OK;
}

TimBm_Status_t TIM_BM_Stop(TIM_TypeDef *tim)
{
    if (entry(tim) == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const uint32_t pm = irq_save();
    tim->CR1 &= ~CR1_CEN;
    irq_restore(pm);
    return ((tim->CR1 & CR1_CEN) == 0U) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

/* One register of the reset image, written then checked. */
typedef struct
{
    volatile uint32_t *reg;
    uint32_t           value;
    uint32_t           mask;     /* bits compared on read-back */
} ResetReg_t;

TimBm_Status_t TIM_BM_Deinit(TIM_TypeDef *tim)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const uint32_t cmax = counter_max(e);
    const bool bdtr = (e->flags & F_BDTR) != 0U;

    /* Outputs first (C11): MOE, then CCER, then the counter, then the requests. */
    uint32_t pm = irq_save();
    if (bdtr)
    {
        tim->BDTR &= ~BDTR_MOE;
    }
    if (e->channels != 0U)
    {
        tim->CCER = 0U;
    }
    /* CMS = 0 here, so the CR1 = 0 of the image below can clear DIR, which is read-only
       while CMS != 0 (40.4.1): stopped during a down-count in center mode, DIR reads 1. */
    tim->CR1 &= ~(CR1_CEN | CR1_CMS_MSK);
    tim->DIER = 0U;
    irq_restore(pm);

    /* The reset values: RM0399 register maps (tables 355, 356, 360, 363, 365,
       368, 370, 371). All 0 except ARR (counter maximum; 0xFFFF FFFF on TIM2-5,
       41.4.15) and AF1/AF2 (0x1 on TIM1, TIM8, TIM15-17: BKINE). */
    ResetReg_t img[24];
    uint32_t n = 0U;
    img[n++] = (ResetReg_t){ &tim->CR1, 0U, 0xFFFFU };
    if ((e->flags & F_CR2) != 0U)
    {
        img[n++] = (ResetReg_t){ &tim->CR2, 0U, 0xFFFFFFFFU };
    }
    if ((e->flags & F_SLAVE) != 0U)
    {
        img[n++] = (ResetReg_t){ &tim->SMCR, 0U, 0xFFFFFFFFU };
    }
    if (e->channels != 0U)
    {
        img[n++] = (ResetReg_t){ &tim->CCMR1, 0U, 0xFFFFFFFFU };
    }
    if (e->channels == 4U)
    {
        img[n++] = (ResetReg_t){ &tim->CCMR2, 0U, 0xFFFFFFFFU };
    }
    if ((e->flags & F_CH56) != 0U)
    {
        img[n++] = (ResetReg_t){ &tim->CCMR3, 0U, 0xFFFFFFFFU };
        img[n++] = (ResetReg_t){ &tim->CCR5, 0U, 0xFFFFFFFFU };
        img[n++] = (ResetReg_t){ &tim->CCR6, 0U, 0xFFFFFFFFU };
    }
    img[n++] = (ResetReg_t){ &tim->CNT, 0U, 0xFFFFFFFFU };
    img[n++] = (ResetReg_t){ &tim->PSC, 0U, 0xFFFFU };
    img[n++] = (ResetReg_t){ &tim->ARR, ((e->flags & F_ARR32R) != 0U) ? 0xFFFFFFFFUL : cmax, cmax };
    if (e->rcr_bits != 0U)
    {
        img[n++] = (ResetReg_t){ &tim->RCR, 0U, 0xFFFFU };
    }
    for (uint32_t ch = 1U; ch <= e->channels; ch++)
    {
        img[n++] = (ResetReg_t){ &(&tim->CCR1)[ch - 1U], 0U, cmax };
    }
    if (bdtr)
    {
        img[n++] = (ResetReg_t){ &tim->BDTR, 0U, 0xFFFFFFFFU };
    }
    if ((e->flags & F_DCR) != 0U)
    {
        img[n++] = (ResetReg_t){ &tim->DCR, 0U, 0xFFFFU };
    }
    if ((e->flags & F_AF1) != 0U)
    {
        img[n++] = (ResetReg_t){ &tim->AF1, e->af1_reset, 0xFFFFFFFFU };
    }
    if ((e->flags & F_AF2) != 0U)
    {
        img[n++] = (ResetReg_t){ &tim->AF2, 1U, 0xFFFFFFFFU };
    }
    if (e->channels != 0U)
    {
        img[n++] = (ResetReg_t){ &tim->TISEL, 0U, 0xFFFFFFFFU };
    }

    pm = irq_save();
    for (uint32_t i = 0U; i < n; i++)
    {
        *img[i].reg = img[i].value;
    }
    tim->SR = 0U;
    __DSB();
    irq_restore(pm);

    bool ok = (tim->DIER == 0U) && (tim->SR == 0U) && ((e->channels == 0U) || (tim->CCER == 0U));
    for (uint32_t i = 0U; ok && (i < n); i++)
    {
        ok = ((*img[i].reg & img[i].mask) == (img[i].value & img[i].mask));
    }
    return ok ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

/* ==========================================================================
 * Events, flags, interrupts, DMA requests
 * ========================================================================== */

static TimBm_Status_t dier_update(TIM_TypeDef *tim, uint32_t bits, bool set)
{
    const uint32_t pm = irq_save();
    if (set)
    {
        tim->DIER |= bits;
    }
    else
    {
        tim->DIER &= ~bits;
    }
    irq_restore(pm);
    const uint32_t rb = tim->DIER & bits;
    return (rb == (set ? bits : 0U)) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

static TimBm_Status_t irq_common(TIM_TypeDef *tim, uint32_t ev_mask, bool set)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_Status_t st = ev_check(e, ev_mask);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    return dier_update(tim, ev_mask, set);     /* UIE = bit 0, CCxIE = bit x, TIE = bit 6 */
}

TimBm_Status_t TIM_BM_EnableIrq(TIM_TypeDef *tim, uint32_t ev_mask)
{
    return irq_common(tim, ev_mask, true);
}

TimBm_Status_t TIM_BM_DisableIrq(TIM_TypeDef *tim, uint32_t ev_mask)
{
    return irq_common(tim, ev_mask, false);
}

static uint32_t mux_request(const TimEntry_t *e, uint32_t ev)
{
    if (ev == TIM_BM_EV_UPDATE)
    {
        return e->mux_up;
    }
    for (uint32_t ch = 1U; ch <= 4U; ch++)
    {
        if (ev == (1UL << ch))
        {
            return (ch <= e->channels) ? e->mux_cc[ch - 1U] : 0U;
        }
    }
    return 0U;
}

static TimBm_Status_t dmareq_common(TIM_TypeDef *tim, uint32_t ev_mask, bool set)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    if ((ev_mask == 0U) || ((ev_mask & ~EV_DMA) != 0U))
    {
        return TIM_BM_ERR_PARAM;
    }
    for (uint32_t b = 0U; b <= 4U; b++)
    {
        if (((ev_mask & (1UL << b)) != 0U) && (mux_request(e, 1UL << b) == 0U))
        {
            return TIM_BM_ERR_UNSUPPORTED;
        }
    }
    return dier_update(tim, ev_mask << DIER_DMA_SHIFT, set);
}

TimBm_Status_t TIM_BM_EnableDmaRequest(TIM_TypeDef *tim, uint32_t ev_mask)
{
    return dmareq_common(tim, ev_mask, true);
}

TimBm_Status_t TIM_BM_DisableDmaRequest(TIM_TypeDef *tim, uint32_t ev_mask)
{
    return dmareq_common(tim, ev_mask, false);
}

uint32_t TIM_BM_TakeFlags(TIM_TypeDef *tim, uint32_t ev_mask)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return 0U;
    }
    const uint32_t taken = tim->SR & ev_mask & ev_valid(e);
    if (taken != 0U)
    {
        tim->SR = ~taken;                      /* one write; rc_w0 bits written 1 keep their value */
        __DSB();
    }
    return taken;
}

uint32_t TIM_BM_TakeOvercapture(TIM_TypeDef *tim, uint32_t ch_mask)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return 0U;
    }
    const uint32_t valid = (1UL << e->channels) - 1U;
    const uint32_t bits = (ch_mask & valid) << SR_CCOF_SHIFT;
    const uint32_t taken = tim->SR & bits;
    if (taken != 0U)
    {
        tim->SR = ~taken;
        __DSB();
    }
    return taken >> SR_CCOF_SHIFT;
}

/* CYCCNT is counting: trace enabled in DEMCR and CYCCNTENA in DWT_CTRL. */
static bool cyccnt_running(void)
{
    return ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) != 0U) &&
           ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
}

TimBm_Status_t TIM_BM_WaitFlags(TIM_TypeDef *tim, uint32_t ev_mask, uint32_t max_cycles, uint32_t *taken)
{
    if (taken == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_Status_t st = ev_check(e, ev_mask);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    *taken = 0U;
    if (!cyccnt_running())
    {
        return TIM_BM_ERR_TIMEOUT;             /* no time base: the wait could not end (C13) */
    }
    const uint32_t t0 = DWT->CYCCNT;
    uint32_t last = t0;
    uint32_t stalled = 0U;
    for (;;)
    {
        const uint32_t f = TIM_BM_TakeFlags(tim, ev_mask);
        if (f != 0U)
        {
            *taken = f;
            return TIM_BM_OK;
        }
        const uint32_t now = DWT->CYCCNT;
        if ((now - t0) >= max_cycles)
        {
            return TIM_BM_ERR_TIMEOUT;
        }
        /* CYCCNT stopped after the check above (DWT disabled meanwhile). */
        stalled = (now == last) ? (stalled + 1U) : 0U;
        if (stalled > 1000U)
        {
            return TIM_BM_ERR_TIMEOUT;
        }
        last = now;
    }
}

TimBm_Status_t TIM_BM_GenerateEvent(TIM_TypeDef *tim, uint32_t ev_mask)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_Status_t st = ev_check(e, ev_mask);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    tim->EGR = ev_mask;                        /* UG = bit 0, CCxG = bit x, TG = bit 6 */
    return TIM_BM_OK;
}

static bool single_event(uint32_t ev)
{
    return (ev != 0U) && ((ev & (ev - 1U)) == 0U) && ((ev & ~EV_ALL) == 0U);
}

TimBm_Status_t TIM_BM_GetIRQn(const TIM_TypeDef *tim, uint32_t ev, IRQn_Type *out)
{
    const TimEntry_t *e = entry(tim);
    if ((out == NULL) || (e == NULL))
    {
        return TIM_BM_ERR_PARAM;
    }
    if (!single_event(ev))
    {
        return TIM_BM_ERR_PARAM;
    }
    if ((ev & ev_valid(e)) == 0U)
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    const IRQn_Type irq = (ev == TIM_BM_EV_UPDATE) ? e->irq_up : ((ev == TIM_BM_EV_TRIGGER) ? e->irq_trg : e->irq_cc);
    if (irq == IRQ_NONE)
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    *out = irq;
    return TIM_BM_OK;
}

TimBm_Status_t TIM_BM_GetDmaRequest(const TIM_TypeDef *tim, uint32_t ev, uint32_t *request_id)
{
    const TimEntry_t *e = entry(tim);
    if ((request_id == NULL) || (e == NULL))
    {
        return TIM_BM_ERR_PARAM;
    }
    if (!single_event(ev) || ((ev & ~EV_DMA) != 0U))
    {
        return TIM_BM_ERR_PARAM;
    }
    const uint32_t id = mux_request(e, ev);
    if (id == 0U)
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    *request_id = id;
    return TIM_BM_OK;
}

/* ==========================================================================
 * Output compare / PWM
 * ========================================================================== */

TimBm_Status_t TIM_BM_ConfigOutput(const TimBm_OcConfig_t *cfg)
{
    if (cfg == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimEntry_t *e = entry(cfg->tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    TimBm_Status_t st = ch_check(e, cfg->channel);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    if ((cfg->mode > TIM_BM_OC_PWM2) || (cfg->compare > counter_max(e)))
    {
        return TIM_BM_ERR_PARAM;
    }
    TIM_TypeDef *tim = cfg->tim;
    const uint32_t ch = cfg->channel;
    const uint32_t e_bit = 1UL << (4U * (ch - 1U));
    if ((tim->CCER & e_bit) != 0U)
    {
        return TIM_BM_ERR_BUSY;                /* CCxS is read-only while CCxE = 1 (40.4.7) */
    }

    volatile uint32_t *ccmr = ccmr_reg(tim, ch);
    const uint32_t fmask = ccmr_field_mask(ch);
    const uint32_t field = ((cfg->mode << 4) | (cfg->preload ? (1UL << 3) : 0U)) << ccmr_shift(ch);
    const uint32_t pol = cfg->active_low ? (2UL << (4U * (ch - 1U))) : 0U;

    uint32_t pm = irq_save();
    *ccmr = (*ccmr & ~fmask) | field;          /* CCxS = 00, OCxFE = OCxCE = OCxM[3] = 0 */
    irq_restore(pm);
    (&tim->CCR1)[ch - 1U] = cfg->compare;
    pm = irq_save();
    tim->CCER = (tim->CCER & ~ccer_pol_mask(ch)) | pol;
    irq_restore(pm);

    if (((*ccmr & fmask) != field) || ((&tim->CCR1)[ch - 1U] != cfg->compare) ||
        ((tim->CCER & ccer_pol_mask(ch)) != pol))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_BM_SetCompare(TIM_TypeDef *tim, uint32_t channel, uint32_t ccr)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_Status_t st = ch_check(e, channel);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    if (ccr > counter_max(e))
    {
        return TIM_BM_ERR_PARAM;
    }
    (&tim->CCR1)[channel - 1U] = ccr;
    return ((&tim->CCR1)[channel - 1U] == ccr) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

/* All channels of ch_mask have CCxS == 00 (want_output) or != 00 (inputs). */
static bool channels_are(TIM_TypeDef *tim, uint32_t ch_mask, bool want_output)
{
    for (uint32_t ch = 1U; ch <= 4U; ch++)
    {
        if ((ch_mask & TIM_BM_CH(ch)) != 0U)
        {
            if ((ccxs(tim, ch) == 0U) != want_output)
            {
                return false;
            }
        }
    }
    return true;
}

TimBm_Status_t TIM_BM_OutputEnable(TIM_TypeDef *tim, uint32_t ch_mask)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_Status_t st = chmask_check(e, ch_mask);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    if (!channels_are(tim, ch_mask, true))
    {
        return TIM_BM_ERR_PARAM;               /* an input: CCxE would start a capture */
    }
    const uint32_t bits = ccer_e_bits(ch_mask);
    const bool bdtr = (e->flags & F_BDTR) != 0U;
    const uint32_t pm = irq_save();
    tim->CCER |= bits;
    if (bdtr)
    {
        tim->BDTR |= BDTR_MOE;                 /* MOE last (C11) */
    }
    irq_restore(pm);
    if (((tim->CCER & bits) != bits) || (bdtr && ((tim->BDTR & BDTR_MOE) == 0U)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_BM_OutputDisable(TIM_TypeDef *tim, uint32_t ch_mask)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_Status_t st = chmask_check(e, ch_mask);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    const uint32_t bits = ccer_e_bits(ch_mask);
    const uint32_t all_e = ccer_e_bits(0xFU);
    const bool bdtr = (e->flags & F_BDTR) != 0U;
    bool moe_off = false;
    const uint32_t pm = irq_save();
    tim->CCER &= ~bits;
    if (bdtr && ((tim->CCER & all_e) == 0U))
    {
        tim->BDTR &= ~BDTR_MOE;
        moe_off = true;
    }
    irq_restore(pm);
    if (((tim->CCER & bits) != 0U) || (moe_off && ((tim->BDTR & BDTR_MOE) != 0U)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

/* ==========================================================================
 * Input capture
 * ========================================================================== */

/* Writes one input channel: CCMRx field, CCER polarity, TIxSEL. Caller validated. */
static void ic_write(TIM_TypeDef *tim, uint32_t ch, uint32_t ccs, uint32_t edge, uint32_t psc_code,
                     uint32_t filter)
{
    volatile uint32_t *ccmr = ccmr_reg(tim, ch);
    const uint32_t field = (ccs | (psc_code << 2) | (filter << 4)) << ccmr_shift(ch);
    const uint32_t pm = irq_save();
    *ccmr = (*ccmr & ~ccmr_field_mask(ch)) | field;
    tim->CCER = (tim->CCER & ~ccer_pol_mask(ch)) | ccer_edge_bits(ch, edge);
    irq_restore(pm);
}

static bool ic_verify(TIM_TypeDef *tim, uint32_t ch, uint32_t ccs, uint32_t edge, uint32_t psc_code,
                      uint32_t filter)
{
    const uint32_t field = (ccs | (psc_code << 2) | (filter << 4)) << ccmr_shift(ch);
    return ((*ccmr_reg(tim, ch) & ccmr_field_mask(ch)) == field) &&
           ((tim->CCER & ccer_pol_mask(ch)) == ccer_edge_bits(ch, edge));
}

static void tisel_write(TIM_TypeDef *tim, uint32_t ti, uint32_t value)
{
    const uint32_t sh = 8U * (ti - 1U);
    const uint32_t pm = irq_save();
    tim->TISEL = (tim->TISEL & ~(0xFUL << sh)) | (value << sh);
    irq_restore(pm);
}

static bool tisel_verify(TIM_TypeDef *tim, uint32_t ti, uint32_t value)
{
    return ((tim->TISEL >> (8U * (ti - 1U))) & 0xFUL) == value;
}

TimBm_Status_t TIM_BM_ConfigInput(const TimBm_IcConfig_t *cfg)
{
    if (cfg == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimEntry_t *e = entry(cfg->tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    TimBm_Status_t st = ch_check(e, cfg->channel);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    const uint32_t ch = cfg->channel;
    /* capability */
    if ((cfg->selection == TIM_BM_IC_TRC) && ((e->flags & F_SLAVE) == 0U))
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    if ((cfg->selection == TIM_BM_IC_INDIRECT) && (e->channels < 2U))
    {
        return TIM_BM_ERR_UNSUPPORTED;         /* no paired channel to take TIy from */
    }
    if ((cfg->tisel <= 15U) && ((e->tisel_mask[ch - 1U] & (1UL << cfg->tisel)) == 0U))
    {
        return TIM_BM_ERR_UNSUPPORTED;         /* reserved TIxSEL value on this instance */
    }
    /* field range */
    uint32_t psc_code = 0U;
    if ((cfg->selection < TIM_BM_IC_DIRECT) || (cfg->selection > TIM_BM_IC_TRC) || !edge_valid(cfg->edge) ||
        !prescaler_code(cfg->prescaler, &psc_code) || (cfg->filter > 15U) || (cfg->tisel > 15U))
    {
        return TIM_BM_ERR_PARAM;
    }
    TIM_TypeDef *tim = cfg->tim;
    /* state */
    if ((tim->CCER & (1UL << (4U * (ch - 1U)))) != 0U)
    {
        return TIM_BM_ERR_BUSY;
    }
    ic_write(tim, ch, cfg->selection, cfg->edge, psc_code, cfg->filter);
    tisel_write(tim, ch, cfg->tisel);
    if (!ic_verify(tim, ch, cfg->selection, cfg->edge, psc_code, cfg->filter) || !tisel_verify(tim, ch, cfg->tisel))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_BM_ConfigPwmInput(TIM_TypeDef *tim, uint32_t ti, uint32_t edge,
                                     uint32_t prescaler, uint32_t filter, uint32_t tisel)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    if ((ti != 1U) && (ti != 2U))
    {
        return TIM_BM_ERR_PARAM;
    }
    /* capability: two channels on one TI and a slave controller (40.3.8) */
    if (((e->flags & F_SLAVE) == 0U) || (e->channels < 2U))
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    if ((tisel <= 15U) && ((e->tisel_mask[ti - 1U] & (1UL << tisel)) == 0U))
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    /* field range */
    uint32_t psc_code = 0U;
    if (((edge != TIM_BM_EDGE_RISING) && (edge != TIM_BM_EDGE_FALLING)) ||
        !prescaler_code(prescaler, &psc_code) || (filter > 15U) || (tisel > 15U))
    {
        return TIM_BM_ERR_PARAM;
    }
    /* state */
    if (((tim->CCER & ccer_e_bits(0x3U)) != 0U) || (smcr_sms(tim->SMCR) != 0U))
    {
        return TIM_BM_ERR_BUSY;
    }
    const uint32_t other = (ti == 1U) ? 2U : 1U;
    const uint32_t opposite = (edge == TIM_BM_EDGE_RISING) ? TIM_BM_EDGE_FALLING : TIM_BM_EDGE_RISING;
    const uint32_t ts = (ti == 1U) ? 5U : 6U;  /* TI1FP1 = 00101, TI2FP2 = 00110 */

    ic_write(tim, ti, TIM_BM_IC_DIRECT, edge, psc_code, filter);
    ic_write(tim, other, TIM_BM_IC_INDIRECT, opposite, psc_code, filter);
    tisel_write(tim, ti, tisel);
    const uint32_t pm = irq_save();
    uint32_t smcr = tim->SMCR & ~SMCR_SMS_MSK;
    tim->SMCR = smcr;                          /* SMS = 0 (already), then TS, then SMS */
    smcr = (smcr & ~SMCR_TS_MSK) | smcr_ts_bits(ts);
    tim->SMCR = smcr;
    tim->SMCR = smcr | smcr_sms_bits(TIM_BM_SMS_RESET);
    irq_restore(pm);

    if (!ic_verify(tim, ti, TIM_BM_IC_DIRECT, edge, psc_code, filter) ||
        !ic_verify(tim, other, TIM_BM_IC_INDIRECT, opposite, psc_code, filter) || !tisel_verify(tim, ti, tisel) ||
        ((tim->SMCR & (SMCR_TS_MSK | SMCR_SMS_MSK)) != (smcr_ts_bits(ts) | smcr_sms_bits(TIM_BM_SMS_RESET))))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

static TimBm_Status_t capture_common(TIM_TypeDef *tim, uint32_t ch_mask, bool set)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_Status_t st = chmask_check(e, ch_mask);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    if (!channels_are(tim, ch_mask, false))
    {
        return TIM_BM_ERR_PARAM;               /* an output: CCxE would drive the pin */
    }
    const uint32_t bits = ccer_e_bits(ch_mask);
    const uint32_t pm = irq_save();
    if (set)
    {
        tim->CCER |= bits;
    }
    else
    {
        tim->CCER &= ~bits;
    }
    irq_restore(pm);
    return ((tim->CCER & bits) == (set ? bits : 0U)) ? TIM_BM_OK : TIM_BM_ERR_VERIFY;
}

TimBm_Status_t TIM_BM_CaptureEnable(TIM_TypeDef *tim, uint32_t ch_mask)
{
    return capture_common(tim, ch_mask, true);
}

TimBm_Status_t TIM_BM_CaptureDisable(TIM_TypeDef *tim, uint32_t ch_mask)
{
    return capture_common(tim, ch_mask, false);
}

/* ==========================================================================
 * Master / slave
 * ========================================================================== */

TimBm_Status_t TIM_BM_ConfigMaster(TIM_TypeDef *tim, uint32_t mms, bool msm)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    const bool slave = (e->flags & F_SLAVE) != 0U;
    /* capability */
    if (((e->flags & F_MASTER) == 0U) || ((mms <= 7U) && (mms > e->mms_max)) || (msm && !slave))
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    /* field range */
    if (mms > 7U)
    {
        return TIM_BM_ERR_PARAM;
    }
    /* state: ES0445 2.16.1 */
    if (msm && errata_2_16_1((tim->CR1 & CR1_OPM) != 0U, smcr_sms(tim->SMCR), true))
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    const uint32_t pm = irq_save();
    tim->CR2 = (tim->CR2 & ~CR2_MMS_MSK) | (mms << CR2_MMS_POS);
    if (slave)
    {
        tim->SMCR = msm ? (tim->SMCR | SMCR_MSM) : (tim->SMCR & ~SMCR_MSM);
    }
    irq_restore(pm);
    if ((((tim->CR2 & CR2_MMS_MSK) >> CR2_MMS_POS) != mms) || (slave && (((tim->SMCR & SMCR_MSM) != 0U) != msm)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

static bool sms_valid(uint32_t sms)
{
    return (sms == TIM_BM_SMS_DISABLED) || ((sms >= TIM_BM_SMS_RESET) && (sms <= TIM_BM_SMS_RESET_TRIGGER));
}

TimBm_Status_t TIM_BM_ConfigSlave(TIM_TypeDef *tim, uint32_t sms, uint32_t trigger)
{
    const TimEntry_t *e = entry(tim);
    if (e == NULL)
    {
        return TIM_BM_ERR_PARAM;
    }
    /* capability */
    if ((e->flags & F_SLAVE) == 0U)
    {
        return TIM_BM_ERR_UNSUPPORTED;
    }
    if ((trigger <= 8U) && ((e->itr_mask & (1UL << trigger)) == 0U))
    {
        return TIM_BM_ERR_UNSUPPORTED;         /* ITRn not wired on this instance */
    }
    /* field range */
    uint32_t ts = 0U;
    if (!sms_valid(sms) || !ts_code(trigger, &ts) ||
        ((sms == TIM_BM_SMS_GATED) && (trigger == TIM_BM_TRG_TI1F_ED)))
    {
        return TIM_BM_ERR_PARAM;               /* gated on TI1F_ED: forbidden by 40.4.3 */
    }
    /* state */
    const uint32_t smcr0 = tim->SMCR;
    if ((sms != TIM_BM_SMS_DISABLED) && (smcr_sms(smcr0) != 0U))
    {
        return TIM_BM_ERR_BUSY;                /* TS only changes with SMS = 0 (40.4.3 note) */
    }
    if ((sms == TIM_BM_SMS_RESET_TRIGGER) && errata_2_16_1((tim->CR1 & CR1_OPM) != 0U, sms, (smcr0 & SMCR_MSM) != 0U))
    {
        return TIM_BM_ERR_UNSUPPORTED;         /* ES0445 2.16.1 */
    }
    const uint32_t pm = irq_save();
    uint32_t smcr = tim->SMCR & ~SMCR_SMS_MSK;
    tim->SMCR = smcr;                          /* SMS = 0 */
    smcr = (smcr & ~SMCR_TS_MSK) | smcr_ts_bits(ts);
    tim->SMCR = smcr;                          /* TS */
    tim->SMCR = smcr | smcr_sms_bits(sms);     /* SMS */
    irq_restore(pm);
    if ((tim->SMCR & (SMCR_TS_MSK | SMCR_SMS_MSK)) != (smcr_ts_bits(ts) | smcr_sms_bits(sms)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}
