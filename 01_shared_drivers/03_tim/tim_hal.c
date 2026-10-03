/**
  ******************************************************************************
  * @file    tim_hal.c
  * @brief   TIM1-8, TIM12-17, HAL tier. Contract in tim_hal.h.
  *
  *          Every function checks the handle, lets TIM_BM_Check* validate the
  *          request (C6: one copy of the checks and of the capability table),
  *          calls the ST HAL and reads back the fields that HAL call writes
  *          (C12). The HAL calls that read-modify-write CCER, DIER or BDTR run
  *          with PRIMASK saved and restored (C10); HAL_TIM_Base_Init and
  *          HAL_TIM_OnePulse_Init touch only CR1, PSC, ARR, RCR and EGR, and may
  *          call the project's MSP, so they run with interrupts as they are.
  *          The read-backs follow the HAL's own conditions (the CMSIS
  *          IS_TIM_*_INSTANCE macros it uses). HAL references: STM32CubeH7
  *          8ec3113, HAL driver e3c518e (V1.11.6), Src/stm32h7xx_hal_tim.c.
  ******************************************************************************
  */

#include "tim_hal.h"

#if defined(HAL_TIM_MODULE_ENABLED)

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

/* TIM_CHANNEL_1..4 = 0x0, 0x4, 0x8, 0xC; channel validated 1..4 by tim_bm. */
static uint32_t hal_channel(uint32_t channel)
{
    return (channel - 1U) * 4U;
}

/* TIM_BM_CH(channel), or a bit outside TIM_BM_CH(1..4) that tim_bm refuses with
   ERR_PARAM (channel 0 or > 4). */
static uint32_t bm_mask(uint32_t channel)
{
    return ((channel >= 1U) && (channel <= 4U)) ? TIM_BM_CH(channel) : (1UL << 4);
}

/* The handle checks of C6, before tim_bm's: a timer instance in the handle. */
static bool handle_ok(const TIM_HandleTypeDef *h)
{
    return (h != NULL) && TIM_BM_IsValidInstance(h->Instance);
}

/* CCMRx of channel ch (1..4) and the shift of its 8-bit field; OCxM[3] is bit 16 + shift. */
static uint32_t ccmr_read(const TIM_TypeDef *tim, uint32_t ch)
{
    return (ch <= 2U) ? tim->CCMR1 : tim->CCMR2;
}

static uint32_t ccmr_shift(uint32_t ch)
{
    return ((ch - 1U) & 1U) * 8U;
}

static uint32_t ccr_read(const TIM_TypeDef *tim, uint32_t ch)
{
    switch (ch)
    {
    case 1U:  return tim->CCR1;
    case 2U:  return tim->CCR2;
    case 3U:  return tim->CCR3;
    default:  return tim->CCR4;
    }
}

/* The HAL starts the counter except in the trigger slave modes, where the trigger
   does (HAL_TIM_PWM_Start, HAL_TIM_IC_Start). With OPM = 1 the hardware may clear
   CEN before the read-back, so CEN is not read back then. */
static bool cen_expected(const TIM_TypeDef *tim)
{
    if ((tim->CR1 & TIM_CR1_OPM) != 0U)
    {
        return false;
    }
    return !(IS_TIM_SLAVE_INSTANCE(tim) && IS_TIM_SLAVEMODE_TRIGGER_ENABLED(tim->SMCR & TIM_SMCR_SMS));
}

/* ==========================================================================
 * Time base
 * ========================================================================== */

TimBm_Status_t TIM_HAL_InitTimeBase(TIM_HandleTypeDef *h, const TimBm_TimeBaseConfig_t *cfg)
{
    static const uint32_t k_mode[5] = { TIM_COUNTERMODE_UP, TIM_COUNTERMODE_DOWN, TIM_COUNTERMODE_CENTERALIGNED1,
                                        TIM_COUNTERMODE_CENTERALIGNED2, TIM_COUNTERMODE_CENTERALIGNED3 };
    if ((h == NULL) || (cfg == NULL) || !TIM_BM_IsValidInstance(h->Instance) || (h->Instance != cfg->tim))
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_Status_t st = TIM_BM_CheckTimeBase(cfg);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    TIM_TypeDef *tim = h->Instance;
    const uint32_t mode = k_mode[cfg->count_mode];     /* validated: 0..4 */
    h->Init.Prescaler         = cfg->psc;
    h->Init.Period            = cfg->arr;
    h->Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    h->Init.RepetitionCounter = cfg->repetition;
    h->Init.AutoReloadPreload = cfg->arr_preload ? TIM_AUTORELOAD_PRELOAD_ENABLE : TIM_AUTORELOAD_PRELOAD_DISABLE;

    /* TIM_Base_SetConfig writes DIR and CMS in one CR1 write. R5-3: DIR is read-only
       while CMS != 00 (40.4.1), so leaving center mode takes a write that clears CMS
       first: a HAL_TIM_Base_Init in up mode. */
    HAL_StatusTypeDef hs = HAL_OK;
    if ((tim->CR1 & TIM_CR1_CMS) != 0U)
    {
        h->Init.CounterMode = TIM_COUNTERMODE_UP;
        hs = HAL_TIM_Base_Init(h);
    }
    h->Init.CounterMode = mode;
    if (hs == HAL_OK)
    {
        hs = HAL_TIM_Base_Init(h);
    }
    /* HAL_TIM_Base_Init keeps CR1.OPM; HAL_TIM_OnePulse_Init writes it (State is READY
       by now: no MSP call). */
    if ((hs == HAL_OK) && (((tim->CR1 & TIM_CR1_OPM) != 0U) != cfg->one_pulse))
    {
        hs = HAL_TIM_OnePulse_Init(h, cfg->one_pulse ? TIM_OPMODE_SINGLE : TIM_OPMODE_REPETITIVE);
    }
    if (hs != HAL_OK)
    {
        return TIM_BM_ERR_HAL;
    }
    const uint32_t pm = irq_save();
    if (cfg->urs_overflow_only)
    {
        __HAL_TIM_URS_ENABLE(h);
    }
    else
    {
        __HAL_TIM_URS_DISABLE(h);
    }
    __HAL_TIM_CLEAR_FLAG(h, TIM_FLAG_UPDATE);          /* C11: SR.UIF = 0 after the init */
    irq_restore(pm);

    /* Read-back of what was written: CMS/DIR (DIR is a status bit in center mode),
       ARPE, OPM and URS; CKD where the HAL writes it; PSC, ARR, RCR; UIF. */
    uint32_t cr1 = mode;
    cr1 |= cfg->arr_preload ? TIM_CR1_ARPE : 0U;
    cr1 |= cfg->one_pulse ? TIM_CR1_OPM : 0U;
    cr1 |= cfg->urs_overflow_only ? TIM_CR1_URS : 0U;
    uint32_t mask = TIM_CR1_CMS | TIM_CR1_DIR | TIM_CR1_ARPE | TIM_CR1_OPM | TIM_CR1_URS;
    if ((mode & TIM_CR1_CMS) != 0U)
    {
        mask &= ~TIM_CR1_DIR;
    }
    if (IS_TIM_CLOCK_DIVISION_INSTANCE(tim))
    {
        mask |= TIM_CR1_CKD;
    }
    if (((tim->CR1 & mask) != cr1) || (tim->PSC != cfg->psc) || (tim->ARR != cfg->arr) ||
        (IS_TIM_REPETITION_COUNTER_INSTANCE(tim) && (tim->RCR != cfg->repetition)) ||
        ((tim->SR & TIM_SR_UIF) != 0U))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

/* ==========================================================================
 * Output compare / PWM
 * ========================================================================== */

TimBm_Status_t TIM_HAL_ConfigOutput(TIM_HandleTypeDef *h, const TimBm_OcConfig_t *cfg)
{
    static const uint32_t k_ocm[8] = { TIM_OCMODE_TIMING, TIM_OCMODE_ACTIVE, TIM_OCMODE_INACTIVE, TIM_OCMODE_TOGGLE,
                                       TIM_OCMODE_FORCED_INACTIVE, TIM_OCMODE_FORCED_ACTIVE, TIM_OCMODE_PWM1,
                                       TIM_OCMODE_PWM2 };
    if ((h == NULL) || (cfg == NULL) || !TIM_BM_IsValidInstance(h->Instance) || (h->Instance != cfg->tim))
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_Status_t st = TIM_BM_CheckOutput(cfg);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    TIM_TypeDef *tim = h->Instance;
    const uint32_t ch = cfg->channel;
    const uint32_t hch = hal_channel(ch);
    const bool pwm = cfg->mode >= TIM_BM_OC_PWM1;
    TIM_OC_InitTypeDef oc;
    oc.OCMode       = k_ocm[cfg->mode];
    oc.Pulse        = cfg->compare;
    oc.OCPolarity   = cfg->active_low ? TIM_OCPOLARITY_LOW : TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode   = TIM_OCFAST_DISABLE;
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;

    const uint32_t pm = irq_save();
    const HAL_StatusTypeDef hs = pwm ? HAL_TIM_PWM_ConfigChannel(h, &oc, hch) : HAL_TIM_OC_ConfigChannel(h, &oc, hch);
    if (hs == HAL_OK)
    {
        /* HAL_TIM_PWM_ConfigChannel always sets OCxPE; HAL_TIM_OC_ConfigChannel never
           writes it. */
        if (cfg->preload)
        {
            __HAL_TIM_ENABLE_OCxPRELOAD(h, hch);
        }
        else
        {
            __HAL_TIM_DISABLE_OCxPRELOAD(h, hch);
        }
    }
    irq_restore(pm);
    if (hs != HAL_OK)
    {
        return TIM_BM_ERR_HAL;
    }

    /* Read-back of the fields TIM_OCx_SetConfig (and the PWM call) write: OCxM with
       OCxM[3], CCxS, OCxPE, OCxFE (PWM only); CCRx; CCxE = 0 and CCxP, and CCxNE =
       CCxNP = 0 on the channels with a complementary output; CR2 OISx/OISxN = 0 on
       the instances with a break input. */
    const uint32_t sh = ccmr_shift(ch);
    const uint32_t f_mask = (TIM_CCMR1_OC1M | TIM_CCMR1_CC1S | TIM_CCMR1_OC1PE | (pwm ? TIM_CCMR1_OC1FE : 0U)) << sh;
    const uint32_t f_exp = ((cfg->mode << TIM_CCMR1_OC1M_Pos) | (cfg->preload ? TIM_CCMR1_OC1PE : 0U)) << sh;
    const uint32_t csh = 4U * (ch - 1U);
    const uint32_t e_mask = (TIM_CCER_CC1E | TIM_CCER_CC1P |
                             (IS_TIM_CCXN_INSTANCE(tim, hch) ? (TIM_CCER_CC1NE | TIM_CCER_CC1NP) : 0U)) << csh;
    const uint32_t e_exp = cfg->active_low ? (TIM_CCER_CC1P << csh) : 0U;
    const uint32_t ois = (ch == 4U) ? TIM_CR2_OIS4 : ((TIM_CR2_OIS1 | TIM_CR2_OIS1N) << (2U * (ch - 1U)));
    if (((ccmr_read(tim, ch) & f_mask) != f_exp) || (ccr_read(tim, ch) != cfg->compare) ||
        ((tim->CCER & e_mask) != e_exp) || (IS_TIM_BREAK_INSTANCE(tim) && ((tim->CR2 & ois) != 0U)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_HAL_StartPwm(TIM_HandleTypeDef *h, uint32_t channel)
{
    if (!handle_ok(h))
    {
        return TIM_BM_ERR_PARAM;
    }
    TIM_TypeDef *tim = h->Instance;
    const TimBm_Status_t st = TIM_BM_CheckChannels(tim, bm_mask(channel), TIM_BM_USE_OUTPUT_ON);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    const uint32_t pm = irq_save();
    const HAL_StatusTypeDef hs = HAL_TIM_PWM_Start(h, hal_channel(channel));
    irq_restore(pm);
    if (hs != HAL_OK)
    {
        return TIM_BM_ERR_HAL;
    }
    const uint32_t e = TIM_CCER_CC1E << (4U * (channel - 1U));
    if (((tim->CCER & e) == 0U) || (IS_TIM_BREAK_INSTANCE(tim) && ((tim->BDTR & TIM_BDTR_MOE) == 0U)) ||
        (cen_expected(tim) && ((tim->CR1 & TIM_CR1_CEN) == 0U)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_HAL_StopPwm(TIM_HandleTypeDef *h, uint32_t channel)
{
    if (!handle_ok(h))
    {
        return TIM_BM_ERR_PARAM;
    }
    TIM_TypeDef *tim = h->Instance;
    const TimBm_Status_t st = TIM_BM_CheckChannels(tim, bm_mask(channel), TIM_BM_USE_OUTPUT_OFF);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    const uint32_t pm = irq_save();
    const HAL_StatusTypeDef hs = HAL_TIM_PWM_Stop(h, hal_channel(channel));
    irq_restore(pm);
    if (hs != HAL_OK)
    {
        return TIM_BM_ERR_HAL;
    }
    /* __HAL_TIM_MOE_DISABLE and __HAL_TIM_DISABLE act only once no CCxE/CCxNE is left. */
    const bool none = (tim->CCER & (TIM_CCER_CCxE_MASK | TIM_CCER_CCxNE_MASK)) == 0U;
    if (((tim->CCER & (TIM_CCER_CC1E << (4U * (channel - 1U)))) != 0U) ||
        (none && IS_TIM_BREAK_INSTANCE(tim) && ((tim->BDTR & TIM_BDTR_MOE) != 0U)) ||
        (none && ((tim->CR1 & TIM_CR1_CEN) != 0U)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

/* ==========================================================================
 * Input capture
 * ========================================================================== */

TimBm_Status_t TIM_HAL_ConfigInput(TIM_HandleTypeDef *h, const TimBm_IcConfig_t *cfg)
{
    static const uint32_t k_sel[4] = { 0U, TIM_ICSELECTION_DIRECTTI, TIM_ICSELECTION_INDIRECTTI, TIM_ICSELECTION_TRC };
    static const uint32_t k_pol[4] = { TIM_ICPOLARITY_RISING, TIM_ICPOLARITY_FALLING, 0U, TIM_ICPOLARITY_BOTHEDGE };
    if ((h == NULL) || (cfg == NULL) || !TIM_BM_IsValidInstance(h->Instance) || (h->Instance != cfg->tim))
    {
        return TIM_BM_ERR_PARAM;
    }
    const TimBm_Status_t st = TIM_BM_CheckInput(cfg);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    TIM_TypeDef *tim = h->Instance;
    const uint32_t ch = cfg->channel;
    const uint32_t hch = hal_channel(ch);
    /* ICxPSC code: 1, 2, 4, 8 events -> 00, 01, 10, 11 (validated by tim_bm) */
    const uint32_t psc = (cfg->prescaler == 8U) ? 3U : ((cfg->prescaler == 4U) ? 2U : ((cfg->prescaler == 2U) ? 1U : 0U));
    TIM_IC_InitTypeDef ic;
    ic.ICPolarity  = k_pol[cfg->edge];
    ic.ICSelection = k_sel[cfg->selection];
    ic.ICPrescaler = psc << TIM_CCMR1_IC1PSC_Pos;
    ic.ICFilter    = cfg->filter;

    const uint32_t pm = irq_save();
    HAL_StatusTypeDef hs = HAL_TIM_IC_ConfigChannel(h, &ic, hch);
    if (hs == HAL_OK)
    {
        hs = HAL_TIMEx_TISelection(h, cfg->tisel << (8U * (ch - 1U)), hch);
    }
    irq_restore(pm);
    if (hs != HAL_OK)
    {
        return TIM_BM_ERR_HAL;
    }

    /* Read-back of what TIM_TIx_SetConfig, the ICxPSC write and HAL_TIMEx_TISelection
       write: CCxS, ICxPSC, ICxF; CCxE (0, as it was) and CCxP/CCxNP; TIxSEL. */
    const uint32_t sh = ccmr_shift(ch);
    const uint32_t f_exp = (cfg->selection | (psc << TIM_CCMR1_IC1PSC_Pos) | (cfg->filter << TIM_CCMR1_IC1F_Pos)) << sh;
    const uint32_t csh = 4U * (ch - 1U);
    const uint32_t e_mask = (TIM_CCER_CC1E | TIM_CCER_CC1P | TIM_CCER_CC1NP) << csh;
    const uint32_t e_exp = (k_pol[cfg->edge] & (TIM_CCER_CC1P | TIM_CCER_CC1NP)) << csh;
    const uint32_t tsh = 8U * (ch - 1U);
    if (((ccmr_read(tim, ch) & (0xFFUL << sh)) != f_exp) || ((tim->CCER & e_mask) != e_exp) ||
        (((tim->TISEL >> tsh) & TIM_TISEL_TI1SEL) != cfg->tisel))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

TimBm_Status_t TIM_HAL_StartCapture(TIM_HandleTypeDef *h, uint32_t channel)
{
    if (!handle_ok(h))
    {
        return TIM_BM_ERR_PARAM;
    }
    TIM_TypeDef *tim = h->Instance;
    const TimBm_Status_t st = TIM_BM_CheckChannels(tim, bm_mask(channel), TIM_BM_USE_CAPTURE);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    const uint32_t pm = irq_save();
    const HAL_StatusTypeDef hs = HAL_TIM_IC_Start(h, hal_channel(channel));
    irq_restore(pm);
    if (hs != HAL_OK)
    {
        return TIM_BM_ERR_HAL;
    }
    if (((tim->CCER & (TIM_CCER_CC1E << (4U * (channel - 1U)))) == 0U) ||
        (cen_expected(tim) && ((tim->CR1 & TIM_CR1_CEN) == 0U)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

/* ==========================================================================
 * Master / slave
 * ========================================================================== */

TimBm_Status_t TIM_HAL_ConfigMaster(TIM_HandleTypeDef *h, uint32_t mms, bool msm)
{
    if (!handle_ok(h))
    {
        return TIM_BM_ERR_PARAM;
    }
    TIM_TypeDef *tim = h->Instance;
    const TimBm_Status_t st = TIM_BM_CheckMaster(tim, mms, msm);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    TIM_MasterConfigTypeDef mc;
    mc.MasterOutputTrigger  = mms << TIM_CR2_MMS_Pos;   /* TIM_TRGO_RESET .. TIM_TRGO_OC4REF */
    mc.MasterOutputTrigger2 = TIM_TRGO2_RESET;
    mc.MasterSlaveMode      = msm ? TIM_MASTERSLAVEMODE_ENABLE : TIM_MASTERSLAVEMODE_DISABLE;
    const uint32_t pm = irq_save();
    const HAL_StatusTypeDef hs = HAL_TIMEx_MasterConfigSynchronization(h, &mc);
    irq_restore(pm);
    if (hs != HAL_OK)
    {
        return TIM_BM_ERR_HAL;
    }
    if (((tim->CR2 & TIM_CR2_MMS) != mc.MasterOutputTrigger) ||
        (IS_TIM_TRGO2_INSTANCE(tim) && ((tim->CR2 & TIM_CR2_MMS2) != 0U)) ||
        (IS_TIM_SLAVE_INSTANCE(tim) && ((tim->SMCR & TIM_SMCR_MSM) != mc.MasterSlaveMode)))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

/* One HAL_TIM_SlaveConfigSynchro writing SMCR.SMS = sms and SMCR.TS = ts (HAL encodings,
   which are the register's). For the triggers whose HAL path also rewrites an input
   stage (TIM_SlaveTimer_SetConfig: TI1F_ED the IC1F filter; TI1FP1 and TI2FP2 the
   filter and CCxP/CCxNP; ETRF the ETR fields) the values passed are the ones already in
   the registers, so that stage keeps its configuration. */
static HAL_StatusTypeDef slave_call(TIM_HandleTypeDef *h, uint32_t sms, uint32_t ts)
{
    const TIM_TypeDef *tim = h->Instance;
    const uint32_t ccer = tim->CCER;
    const uint32_t ccmr1 = tim->CCMR1;
    const uint32_t smcr = tim->SMCR;
    TIM_SlaveConfigTypeDef s;
    s.SlaveMode        = sms;
    s.InputTrigger     = ts;
    s.TriggerPrescaler = TIM_TRIGGERPRESCALER_DIV1;
    if (ts == TIM_TS_TI2FP2)
    {
        s.TriggerPolarity = (ccer >> 4) & (TIM_CCER_CC1P | TIM_CCER_CC1NP);
        s.TriggerFilter   = (ccmr1 & TIM_CCMR1_IC2F) >> TIM_CCMR1_IC2F_Pos;
    }
    else if (ts == TIM_TS_ETRF)
    {
        s.TriggerPolarity  = smcr & TIM_SMCR_ETP;
        s.TriggerPrescaler = smcr & TIM_SMCR_ETPS;
        s.TriggerFilter    = (smcr & TIM_SMCR_ETF) >> TIM_SMCR_ETF_Pos;
    }
    else
    {
        s.TriggerPolarity = ccer & (TIM_CCER_CC1P | TIM_CCER_CC1NP);
        s.TriggerFilter   = (ccmr1 & TIM_CCMR1_IC1F) >> TIM_CCMR1_IC1F_Pos;
    }
    return HAL_TIM_SlaveConfigSynchro(h, &s);
}

TimBm_Status_t TIM_HAL_ConfigSlave(TIM_HandleTypeDef *h, uint32_t sms, uint32_t ts)
{
    static const uint32_t k_sms[9] = { TIM_SLAVEMODE_DISABLE, 0U, 0U, 0U, TIM_SLAVEMODE_RESET, TIM_SLAVEMODE_GATED,
                                       TIM_SLAVEMODE_TRIGGER, TIM_SLAVEMODE_EXTERNAL1,
                                       TIM_SLAVEMODE_COMBINED_RESETTRIGGER };
    static const uint32_t k_ts[19] = { TIM_TS_ITR0, TIM_TS_ITR1, TIM_TS_ITR2, TIM_TS_ITR3, TIM_TS_ITR4, TIM_TS_ITR5,
                                       TIM_TS_ITR6, TIM_TS_ITR7, TIM_TS_ITR8, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                                       TIM_TS_TI1F_ED, TIM_TS_TI1FP1, TIM_TS_TI2FP2 };
    if (!handle_ok(h))
    {
        return TIM_BM_ERR_PARAM;
    }
    TIM_TypeDef *tim = h->Instance;
    const TimBm_Status_t st = TIM_BM_CheckSlave(tim, sms, ts);
    if (st != TIM_BM_OK)
    {
        return st;
    }
    const uint32_t hsms = k_sms[sms];                   /* validated: a TIM_BM_SMS_* */
    const uint32_t hts = k_ts[ts];                      /* validated: ITR0..8, TI1F_ED, TI1FP1, TI2FP2 */

    /* RM0399 40.4.3, TS note: TS changes only with SMS = 000. TIM_SlaveTimer_SetConfig
       writes TS and SMS in one SMCR write, so each step is its own call: SMS = 0 with TS
       as it is (only when SMS != 0), TS with SMS = 0, then SMS. */
    const uint32_t pm = irq_save();
    const uint32_t ccer0 = tim->CCER;
    const uint32_t ccmr10 = tim->CCMR1;
    const uint32_t smcr0 = tim->SMCR;
    HAL_StatusTypeDef hs = HAL_OK;
    if ((smcr0 & TIM_SMCR_SMS) != 0U)
    {
        hs = slave_call(h, TIM_SLAVEMODE_DISABLE, smcr0 & TIM_SMCR_TS);
    }
    if (hs == HAL_OK)
    {
        hs = slave_call(h, TIM_SLAVEMODE_DISABLE, hts);
    }
    if ((hs == HAL_OK) && (hsms != TIM_SLAVEMODE_DISABLE))
    {
        hs = slave_call(h, hsms, hts);
    }
    irq_restore(pm);
    if (hs != HAL_OK)
    {
        return TIM_BM_ERR_HAL;
    }
    /* SMCR: TS and SMS, the other fields as they were; DIER: TIE = TDE = 0 (the HAL
       clears them); CCER and CCMR1: the input stages rewritten with their own values. */
    if ((tim->SMCR != ((smcr0 & ~(TIM_SMCR_TS | TIM_SMCR_SMS)) | hts | hsms)) ||
        ((tim->DIER & (TIM_DIER_TIE | TIM_DIER_TDE)) != 0U) || (tim->CCER != ccer0) || (tim->CCMR1 != ccmr10))
    {
        return TIM_BM_ERR_VERIFY;
    }
    return TIM_BM_OK;
}

/* ==========================================================================
 * Deinit
 * ========================================================================== */

TimBm_Status_t TIM_HAL_Deinit(TIM_HandleTypeDef *h)
{
    if (!handle_ok(h))
    {
        return TIM_BM_ERR_PARAM;
    }
    /* The registers: tim_bm's reset image (rule 11), not HAL_TIM_Base_DeInit, which would
       call the project's MSP. */
    const TimBm_Status_t st = TIM_BM_Deinit(h->Instance);
    /* The handle as HAL_TIM_Base_DeInit leaves it, MSP call aside. */
    h->DMABurstState = HAL_DMA_BURST_STATE_RESET;
    TIM_CHANNEL_STATE_SET_ALL(h, HAL_TIM_CHANNEL_STATE_RESET);
    TIM_CHANNEL_N_STATE_SET_ALL(h, HAL_TIM_CHANNEL_STATE_RESET);
    h->State = HAL_TIM_STATE_RESET;
    __HAL_UNLOCK(h);
    return st;
}

#endif /* HAL_TIM_MODULE_ENABLED */
