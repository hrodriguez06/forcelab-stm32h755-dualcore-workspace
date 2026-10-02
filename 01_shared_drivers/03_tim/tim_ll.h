/**
  ******************************************************************************
  * @file    tim_ll.h
  * @brief   General-purpose, advanced and basic timers (TIM1-8, TIM12-17),
  *          LL tier: the operations of tim_bm with the constants of the ST LL
  *          header (LL_TIM_COUNTERMODE_*, LL_TIM_OCMODE_*, LL_TIM_CHANNEL_CH1..4,
  *          LL_TIM_IC_POLARITY_*, LL_TIM_TS_*, ...).
  *
  *          Driver tier: *_ll. Adds only the static inline functions of
  *          stm32h7xx_ll_tim.h to tim_bm (C8): no USE_FULL_LL_DRIVER, no
  *          LL_TIM_Init, no state in RAM (C7).
  *
  *          Contract (03_tim plan, revision 6, "Contrato de API: tim_ll"):
  *            - every LL constant is translated to the tim_bm value and the call
  *              is validated by tim_bm (TIM_BM_Check*): one copy of the checks and
  *              of the capability table. An LL constant this API does not know
  *              becomes an out-of-range tim_bm value, so it is refused with
  *              TIM_BM_ERR_PARAM at the place the C6 order gives a field error;
  *            - the registers are written with the LL inlines, in the order of
  *              the tim_bm rules 3-11 (R5-3 included), read-modify-writes under
  *              PRIMASK saved and restored (C10), and read back (C12);
  *            - for the same configuration the register image is the one tim_bm
  *              leaves, bit for bit (T2);
  *            - the status is TimBm_Status_t.
  *
  *          Event masks (interrupts, DMA requests, flags, EGR) use the interrupt
  *          enable bit of each event: LL_TIM_DIER_UIE, LL_TIM_DIER_CC1IE..CC4IE
  *          and LL_TIM_DIER_TIE. The SR flags LL_TIM_SR_* and the EGR bits sit at
  *          the same positions, and so do the TIM_BM_EV_* values, so no event
  *          mask is translated. Channel masks are ORs of LL_TIM_CHANNEL_CH1..4.
  *
  *          Clock, Deinit, the flags and the lookups call tim_bm (TIM_BM_Deinit
  *          writes the reset image of the capability table; the RCC bank is not
  *          in stm32h7xx_ll_tim.h). The pure functions, the capabilities and the
  *          kernel clock take no LL constant: use TIM_BM_* for them.
  ******************************************************************************
  */

#ifndef TIM_LL_H
#define TIM_LL_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"
#include "stm32h7xx_ll_tim.h"
#include "tim_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Clock (tim_bm) ------------------------------------------------------------ */
TimBm_Status_t TIM_LL_EnableClock(TIM_TypeDef *tim);    /* TIM_BM_EnableClock: this core's bank */
TimBm_Status_t TIM_LL_DisableClock(TIM_TypeDef *tim);   /* TIM_BM_DisableClock                  */

/* ---- Time base ------------------------------------------------------------------- */
typedef struct
{
    TIM_TypeDef *tim;
    uint32_t     prescaler;      /*!< PSC, 0..65535                                         */
    uint32_t     counter_mode;   /*!< LL_TIM_COUNTERMODE_UP, _DOWN, _CENTER_DOWN (CMS = 01),
                                      _CENTER_UP (10), _CENTER_UP_DOWN (11)                  */
    uint32_t     autoreload;     /*!< ARR, 1..counter maximum                               */
    uint32_t     repetition;     /*!< RCR; 0 where the instance has none                    */
    bool         arr_preload;    /*!< ARPE                                                  */
    bool         one_pulse;      /*!< OPM                                                   */
    bool         urs_overflow_only;  /*!< URS                                               */
} TimLl_TimeBaseConfig_t;

/** As TIM_BM_InitTimeBase (rule 3, R5-3), validated by TIM_BM_CheckTimeBase. */
TimBm_Status_t TIM_LL_InitTimeBase(const TimLl_TimeBaseConfig_t *cfg);
TimBm_Status_t TIM_LL_SetPeriod(TIM_TypeDef *tim, uint32_t autoreload);   /* LL_TIM_SetAutoReload */
TimBm_Status_t TIM_LL_Start(TIM_TypeDef *tim);                            /* LL_TIM_EnableCounter  */
TimBm_Status_t TIM_LL_Stop(TIM_TypeDef *tim);                             /* LL_TIM_DisableCounter */
TimBm_Status_t TIM_LL_Deinit(TIM_TypeDef *tim);                           /* TIM_BM_Deinit         */

static inline uint32_t TIM_LL_GetCounter(const TIM_TypeDef *tim)
{
    return LL_TIM_GetCounter(tim);
}

/* ---- Events, flags, interrupts, DMA requests ---------------------------------- */
TimBm_Status_t TIM_LL_EnableIrq(TIM_TypeDef *tim, uint32_t ev_mask);          /* LL_TIM_EnableIT_*      */
TimBm_Status_t TIM_LL_DisableIrq(TIM_TypeDef *tim, uint32_t ev_mask);
TimBm_Status_t TIM_LL_EnableDmaRequest(TIM_TypeDef *tim, uint32_t ev_mask);   /* LL_TIM_EnableDMAReq_*  */
TimBm_Status_t TIM_LL_DisableDmaRequest(TIM_TypeDef *tim, uint32_t ev_mask);
TimBm_Status_t TIM_LL_GenerateEvent(TIM_TypeDef *tim, uint32_t ev_mask);      /* LL_TIM_GenerateEvent_* */
uint32_t       TIM_LL_TakeFlags(TIM_TypeDef *tim, uint32_t ev_mask);          /* TIM_BM_TakeFlags       */
uint32_t       TIM_LL_TakeOvercapture(TIM_TypeDef *tim, uint32_t ch_mask);    /* LL channel mask        */
TimBm_Status_t TIM_LL_WaitFlags(TIM_TypeDef *tim, uint32_t ev_mask, uint32_t max_cycles,
                                uint32_t *taken);                             /* TIM_BM_WaitFlags       */
TimBm_Status_t TIM_LL_GetIRQn(const TIM_TypeDef *tim, uint32_t ev, IRQn_Type *out);
TimBm_Status_t TIM_LL_GetDmaRequest(const TIM_TypeDef *tim, uint32_t ev, uint32_t *request_id);

/* ---- Output compare / PWM ------------------------------------------------------ */
typedef struct
{
    TIM_TypeDef *tim;
    uint32_t     channel;        /*!< LL_TIM_CHANNEL_CH1..CH4                               */
    uint32_t     oc_mode;        /*!< LL_TIM_OCMODE_FROZEN .. LL_TIM_OCMODE_PWM2            */
    uint32_t     compare;        /*!< CCR                                                   */
    bool         active_low;     /*!< CCxP                                                  */
    bool         preload;        /*!< OCxPE                                                 */
} TimLl_OcConfig_t;

/** As TIM_BM_ConfigOutput (rule 5), validated by TIM_BM_CheckOutput. */
TimBm_Status_t TIM_LL_ConfigOutput(const TimLl_OcConfig_t *cfg);
TimBm_Status_t TIM_LL_SetCompare(TIM_TypeDef *tim, uint32_t channel, uint32_t ccr);
TimBm_Status_t TIM_LL_OutputEnable(TIM_TypeDef *tim, uint32_t ch_mask);    /* CCxE, then MOE    */
TimBm_Status_t TIM_LL_OutputDisable(TIM_TypeDef *tim, uint32_t ch_mask);   /* MOE = 0 when none */

/** CCRx = ccr, no check (ISRs): channel LL_TIM_CHANNEL_CH1..CH4, anything else is ignored. */
static inline void TIM_LL_WriteCcr(TIM_TypeDef *tim, uint32_t channel, uint32_t ccr)
{
    switch (channel)
    {
    case LL_TIM_CHANNEL_CH1: LL_TIM_OC_SetCompareCH1(tim, ccr); break;
    case LL_TIM_CHANNEL_CH2: LL_TIM_OC_SetCompareCH2(tim, ccr); break;
    case LL_TIM_CHANNEL_CH3: LL_TIM_OC_SetCompareCH3(tim, ccr); break;
    case LL_TIM_CHANNEL_CH4: LL_TIM_OC_SetCompareCH4(tim, ccr); break;
    default: break;
    }
}

/* ---- Input capture ------------------------------------------------------------ */
typedef struct
{
    TIM_TypeDef *tim;
    uint32_t     channel;        /*!< LL_TIM_CHANNEL_CH1..CH4                               */
    uint32_t     active_input;   /*!< LL_TIM_ACTIVEINPUT_DIRECTTI, _INDIRECTTI, _TRC         */
    uint32_t     polarity;       /*!< LL_TIM_IC_POLARITY_RISING, _FALLING, _BOTHEDGE        */
    uint32_t     prescaler;      /*!< LL_TIM_ICPSC_DIV1, _DIV2, _DIV4, _DIV8                */
    uint32_t     filter;         /*!< LL_TIM_IC_FILTER_FDIV1 .. LL_TIM_IC_FILTER_FDIV32_N8  */
    uint32_t     remap;          /*!< TISEL of the channel's own input TIx: 0 (the pin) or
                                      an LL_TIM_TIMy_TIx_RMP_* of that input               */
} TimLl_IcConfig_t;

/** As TIM_BM_ConfigInput, validated by TIM_BM_CheckInput. */
TimBm_Status_t TIM_LL_ConfigInput(const TimLl_IcConfig_t *cfg);

/**
  * @brief  As TIM_BM_ConfigPwmInput: channel LL_TIM_CHANNEL_CH1 (TI1) or CH2
  *         (TI2), polarity LL_TIM_IC_POLARITY_RISING or _FALLING, prescaler
  *         LL_TIM_ICPSC_*, filter LL_TIM_IC_FILTER_*, remap of that TIx.
  */
TimBm_Status_t TIM_LL_ConfigPwmInput(TIM_TypeDef *tim, uint32_t channel, uint32_t polarity,
                                     uint32_t prescaler, uint32_t filter, uint32_t remap);
TimBm_Status_t TIM_LL_CaptureEnable(TIM_TypeDef *tim, uint32_t ch_mask);
TimBm_Status_t TIM_LL_CaptureDisable(TIM_TypeDef *tim, uint32_t ch_mask);

/** CCRx of an input channel, no check: channel LL_TIM_CHANNEL_CH1..CH4, else 0. */
static inline uint32_t TIM_LL_ReadCapture(const TIM_TypeDef *tim, uint32_t channel)
{
    switch (channel)
    {
    case LL_TIM_CHANNEL_CH1: return LL_TIM_IC_GetCaptureCH1(tim);
    case LL_TIM_CHANNEL_CH2: return LL_TIM_IC_GetCaptureCH2(tim);
    case LL_TIM_CHANNEL_CH3: return LL_TIM_IC_GetCaptureCH3(tim);
    case LL_TIM_CHANNEL_CH4: return LL_TIM_IC_GetCaptureCH4(tim);
    default: return 0U;
    }
}

/* ---- Master / slave ------------------------------------------------------------ */
/** As TIM_BM_ConfigMaster: trgo LL_TIM_TRGO_RESET .. LL_TIM_TRGO_OC4REF, MSM = msm. */
TimBm_Status_t TIM_LL_ConfigMaster(TIM_TypeDef *tim, uint32_t trgo, bool msm);

/**
  * @brief  As TIM_BM_ConfigSlave: slave_mode LL_TIM_SLAVEMODE_DISABLED, _RESET,
  *         _GATED, _TRIGGER, _COMBINED_RESETTRIGGER or LL_TIM_CLOCKSOURCE_EXT_MODE1
  *         (external clock mode 1, SMS = 0111); trigger LL_TIM_TS_ITR0..ITR8,
  *         _TI1F_ED, _TI1FP1 or _TI2FP2 (ETRF and ITR9-13 are refused).
  */
TimBm_Status_t TIM_LL_ConfigSlave(TIM_TypeDef *tim, uint32_t slave_mode, uint32_t trigger);

#ifdef __cplusplus
}
#endif

#endif /* TIM_LL_H */
