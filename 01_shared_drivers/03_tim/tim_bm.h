/**
  ******************************************************************************
  * @file    tim_bm.h
  * @brief   General-purpose, advanced and basic timers (TIM1-8, TIM12-17),
  *          bare-metal tier.
  *
  *          Driver tier: *_bm. Direct register access only: stm32h7xx.h,
  *          <stdint.h> and <stdbool.h>, no ST HAL and no LL headers (C8), no
  *          state in RAM (C7). Every silicon fact the driver needs (counter
  *          width, channels, ITR wiring, TISEL values, interrupt vectors,
  *          DMAMUX1 requests, RCC enable bit) lives in one table of capabilities
  *          per instance in tim_bm.c, each row citing RM0399 rev 4 (C1).
  *
  *          Contract (03_tim plan, revision 4):
  *            - every parameter is checked before the first access, RCC
  *              included; the order of the refusals is fixed: NULL, instance,
  *              channel or event, capability of the instance, field range,
  *              state (ERR_BUSY) (C6);
  *            - a write the hardware would ignore is refused with ERR_BUSY and
  *              nothing written; every configuration write is read back, and a
  *              mismatch returns ERR_VERIFY (C12);
  *            - in RCC only this core's enable bank is written, never a reset
  *              register, RCC_CFGR or RCC_D2CFGR (read only); SR is cleared
  *              with one write; the read-modify-writes of DIER, CCER, BDTR,
  *              CR1, CR2, SMCR, CCMRx and TISEL run with PRIMASK saved and
  *              restored (C10);
  *            - the only wait is TIM_BM_WaitFlags, bounded in DWT cycles (C13);
  *            - no interrupt handler, no callback, no interrupt controller call:
  *              TIM_BM_GetIRQn only returns the number (C14).
  *
  *          Pins are the caller's (the GPIO driver), and so are the DMA streams.
  *          Ownership: one core per instance; the driver takes no cross-core
  *          lock. See this driver's README.md (written at close).
  ******************************************************************************
  */

#ifndef TIM_BM_H
#define TIM_BM_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Status ------------------------------------------------------------------- */
typedef enum
{
    TIM_BM_OK = 0,
    TIM_BM_ERR_PARAM,        /*!< refused before any peripheral access (C6)               */
    TIM_BM_ERR_UNSUPPORTED,  /*!< the instance lacks this feature (no channel 3 on TIM12)  */
    TIM_BM_ERR_RANGE,        /*!< the request is not reachable with this PSC/ARR width     */
    TIM_BM_ERR_BUSY,         /*!< the hardware would ignore the write in this state (C12)  */
    TIM_BM_ERR_VERIFY,       /*!< read-back differs from what was written (C12)            */
    TIM_BM_ERR_TIMEOUT,      /*!< bounded wait expired, or CYCCNT not running (C13)        */
    TIM_BM_ERR_HAL           /*!< tim_hal only: a HAL call returned an error               */
} TimBm_Status_t;

/* ---- Capabilities (silicon facts, RM0399 ch. 40-44) --------------------------- */
typedef struct
{
    uint8_t  counter_bits;     /*!< 16 or 32 (TIM2, TIM5)                                   */
    uint8_t  channels;         /*!< 0 (TIM6/7), 1 (TIM13/14/16/17), 2 (TIM12/15), 4          */
    uint16_t itr_mask;         /*!< bit n = ITRn (0..8) is wired (tables 353, 358, 361, 366) */
    uint8_t  mms_max;          /*!< highest valid MMS: 2 (TIM6/7), 5 (TIM12/15), 7 (others);
                                    0 where there is no TRGO (master_mode = false)          */
    bool     up_down_center;   /*!< TIM1-5, TIM8                                            */
    bool     repetition;       /*!< TIM1/8 (16-bit RCR), TIM15/16/17 (8-bit RCR)            */
    bool     bdtr;             /*!< MOE gate: TIM1, TIM8, TIM15, TIM16, TIM17               */
    bool     slave_mode;       /*!< SMCR: TIM1-5, TIM8, TIM12, TIM15                        */
    bool     master_mode;      /*!< MMS/TRGO: TIM1-8, TIM12, TIM15 (not 13/14/16/17)        */
    uint16_t tisel_mask[4];    /*!< bit v = TIxSEL value v is valid, per TI1..TI4; bit 0 (the
                                    CHx pin) is set for every channel the instance has      */
} TimBm_Caps_t;

/** true only for the exact base address of TIM1-8 or TIM12-17 (TIM1 + 4 is not a timer). */
bool           TIM_BM_IsValidInstance(const TIM_TypeDef *tim);

/** Copies the instance's row of the capability table. ERR_PARAM: NULL or not a timer. */
TimBm_Status_t TIM_BM_GetCaps(const TIM_TypeDef *tim, TimBm_Caps_t *out);

/* ---- Clock -------------------------------------------------------------------- */
/**
  * @brief  Sets the instance's enable bit in THIS core's RCC bank (RCC->APB1LENR
  *         for TIM2-7 and TIM12-14, RCC->APB2ENR for TIM1, TIM8, TIM15-17) and
  *         reads it back (ERR_VERIFY if it did not stick).
  * @note   RM0399 9.5.10: the enable registers are banked per core, and the
  *         timer is clocked while EITHER core has its bit set. Disabling it from
  *         one core does not stop it while the other core still enables it.
  */
TimBm_Status_t TIM_BM_EnableClock(TIM_TypeDef *tim);    /* this core's RCC bank, read back  */
TimBm_Status_t TIM_BM_DisableClock(TIM_TypeDef *tim);   /* this core's bank only (9.5.10)   */

/**
  * @brief  Pure: the timer kernel clock of RM0399 table 59 from the bus clock
  *         rcc_hclk1 and the fields D2PPREx (3 bits, 0xx and 100..111) and
  *         TIMPRE (0 or 1). ERR_PARAM: out NULL, d2ppre > 7 or timpre > 1.
  *         Integer division by 1, 2, 4 or 8 (truncated).
  */
TimBm_Status_t TIM_BM_KernelClockFromFields(uint32_t hclk1_hz, uint32_t d2ppre, uint32_t timpre,
                                            uint32_t *out_hz);                 /* pure, table 59 */

/**
  * @brief  Reads D2PPRE1 (instances on APB1) or D2PPRE2 (APB2) from RCC_D2CFGR
  *         and TIMPRE from RCC_CFGR, and calls TIM_BM_KernelClockFromFields.
  *         Reads only; hclk1_hz is the caller's (the driver holds no clock tree).
  */
TimBm_Status_t TIM_BM_GetKernelClockHz(const TIM_TypeDef *tim, uint32_t hclk1_hz,
                                       uint32_t *out_hz);   /* reads D2PPRE1/2 and TIMPRE only */

/* ---- Pure arithmetic (C15) ------------------------------------------------------ */
typedef struct
{
    uint32_t psc;
    uint32_t arr;
    uint64_t ticks;            /*!< (psc + 1) * (arr + 1)                                    */
    int32_t  error_ppm;        /*!< (clk / ticks - target) / target * 1e6, toward zero      */
} TimBm_TimeBase_t;

/**
  * @brief  PSC/ARR for target_hz from timer_clock_hz, with the SMALLEST prescaler
  *         that fits arr_max (maximum resolution; plan "Reglas de calculo"):
  *           psc = ceil(clk / (target * (arr_max + 1))) - 1   (ERR_RANGE if > 65535)
  *           n   = (clk + d / 2) / d, d = (psc + 1) * target, clamped to arr_max + 1
  *                 (ERR_RANGE if n < 2)
  *           arr = n - 1, ticks = (psc + 1) * n,
  *           error_ppm = (clk * 1e6 / ticks - target * 1e6) / target, 64-bit,
  *                       truncated toward zero.
  *         ERR_PARAM: out NULL, target_hz = 0, target_hz > timer_clock_hz / 2,
  *         arr_max = 0. Pure: integers only, no peripheral access.
  */
TimBm_Status_t TIM_BM_CalcTimeBase(uint32_t timer_clock_hz, uint32_t target_hz,
                                   uint32_t arr_max, TimBm_TimeBase_t *out);

/**
  * @brief  CCR for a PWM1 duty on an up-counting timer:
  *         ccr = ((arr + 1) * duty_x100 + 5000) / 10000 (64-bit). 0 gives 0 %,
  *         10000 gives arr + 1 (CCR > ARR: 100 %, RM0399 40.3.11).
  *         ERR_PARAM: ccr NULL, counter_bits not 16 or 32, arr above the counter
  *         maximum, duty_x100 > 10000. ERR_RANGE: duty_x100 = 10000 and
  *         arr + 1 > 2^counter_bits - 1 (CCR cannot exceed ARR). Pure.
  */
TimBm_Status_t TIM_BM_DutyToCompare(uint32_t arr, uint32_t counter_bits,
                                    uint32_t duty_x100,            /* 0..10000 = 0.00..100.00 % */
                                    uint32_t *ccr);

/* ---- Time base --------------------------------------------------------------- */
#define TIM_BM_COUNT_UP       0U
#define TIM_BM_COUNT_DOWN     1U
#define TIM_BM_COUNT_CENTER1  2U
#define TIM_BM_COUNT_CENTER2  3U
#define TIM_BM_COUNT_CENTER3  4U

typedef struct
{
    TIM_TypeDef *tim;
    uint32_t     psc;          /*!< 0..65535                                                */
    uint32_t     arr;          /*!< 1..0xFFFF, or 1..0xFFFFFFFF on 32-bit counters           */
    uint32_t     count_mode;   /*!< TIM_BM_COUNT_*; non-UP needs caps.up_down_center         */
    uint32_t     repetition;   /*!< 0 if !caps.repetition; <= 0xFF on TIM15/16/17            */
    bool         arr_preload;  /*!< ARPE                                                     */
    bool         one_pulse;    /*!< OPM                                                      */
    bool         urs_overflow_only;  /*!< URS: UG and slave resets do not raise UIF          */
} TimBm_TimeBaseConfig_t;

/**
  * @brief  Programs the time base with the counter stopped (C11):
  *         CR1 (mode, ARPE, OPM, URS = 1 for now, CEN = 0) -> PSC -> ARR -> RCR
  *         -> UG (loads PSC/ARR/RCR, raises no UIF because URS = 1) -> CR1 final
  *         -> SR = 0 -> read-back of CR1, PSC, ARR and RCR. After it SR.UIF = 0.
  * @retval ERR_PARAM        NULL, not a timer, psc > 65535, arr = 0 (40.4.14: the
  *                          counter would not count), arr above the counter,
  *                          count_mode > CENTER3, repetition above the RCR width.
  * @retval ERR_UNSUPPORTED  a non-UP mode without up_down_center, or repetition
  *                          != 0 without an RCR.
  * @retval ERR_BUSY         CEN = 1 (PSC and the mode cannot change on the fly).
  * @retval ERR_VERIFY       a register did not read back (an unclocked timer).
  */
TimBm_Status_t TIM_BM_InitTimeBase(const TimBm_TimeBaseConfig_t *cfg);   /* CEN = 0 required */

/** ARR = arr (1..counter maximum), live: through the preload when ARPE = 1. Read back. */
TimBm_Status_t TIM_BM_SetPeriod(TIM_TypeDef *tim, uint32_t arr);         /* live, preloaded  */

/** CEN = 1. Not read back: in one-pulse mode the hardware clears CEN itself. */
TimBm_Status_t TIM_BM_Start(TIM_TypeDef *tim);

/** CEN = 0, read back. */
TimBm_Status_t TIM_BM_Stop(TIM_TypeDef *tim);

/**
  * @brief  Back to the reset values of RM0399 without an RCC reset (C10, C11):
  *         BDTR.MOE = 0 -> CCER = 0 -> CR1.CEN = 0 -> DIER = 0 -> CR2, SMCR,
  *         CCMR1-3, CNT, PSC, ARR, RCR, CCR1-6, BDTR, DCR, AF1, AF2 and TISEL to
  *         their reset values (only the registers the instance has) -> SR = 0
  *         -> read-back of all of them. Writes nothing in RCC.
  * @note   The prescaler's shadow register keeps its last value until the next
  *         update event (InitTimeBase generates one): Deinit does not raise UG,
  *         because with MMS = reset an UG is a TRGO pulse to any slave.
  * @retval ERR_VERIFY when a register does not read back its reset value (for
  *         instance an unclocked timer, which reads 0 where ARR resets to 0xFFFF).
  */
TimBm_Status_t TIM_BM_Deinit(TIM_TypeDef *tim);   /* reset values, outputs first; no RCC write */

static inline uint32_t TIM_BM_GetCounter(const TIM_TypeDef *tim)
{
    return tim->CNT;
}

/* ---- Events, flags, interrupts, DMA requests ---------------------------------- */
#define TIM_BM_EV_UPDATE   (1UL << 0)   /* bit positions follow DIER/SR (RM0399 40.4.4) */
#define TIM_BM_EV_CC1      (1UL << 1)
#define TIM_BM_EV_CC2      (1UL << 2)
#define TIM_BM_EV_CC3      (1UL << 3)
#define TIM_BM_EV_CC4      (1UL << 4)
#define TIM_BM_EV_TRIGGER  (1UL << 6)

/* Channel masks (ch_mask): bit n - 1 = channel n, as CCER orders its channels. */
#define TIM_BM_CH(n)       (1UL << ((uint32_t)(n) - 1UL))

/**
  * @brief  DIER |= ev_mask (UIE, CC1IE..CC4IE, TIE), under PRIMASK (C10), read back.
  *         ERR_PARAM: empty mask or a bit outside TIM_BM_EV_*; ERR_UNSUPPORTED: a
  *         channel the instance lacks, or TRIGGER without a slave controller.
  */
TimBm_Status_t TIM_BM_EnableIrq(TIM_TypeDef *tim, uint32_t ev_mask);    /* C10: under PRIMASK */
TimBm_Status_t TIM_BM_DisableIrq(TIM_TypeDef *tim, uint32_t ev_mask);

/**
  * @brief  DIER |= UDE / CCxDE for TIM_BM_EV_UPDATE and TIM_BM_EV_CC1..CC4, under
  *         PRIMASK, read back. ERR_UNSUPPORTED where RM0399 table 126 lists no
  *         DMAMUX1 request for that event (TIM4 CC4, TIM15 CC2, TIM12-14), so a
  *         request that reaches no stream is refused instead of silently lost.
  *         TRIGGER is not part of this API (ERR_PARAM).
  */
TimBm_Status_t TIM_BM_EnableDmaRequest(TIM_TypeDef *tim, uint32_t ev_mask);   /* UPDATE, CC1-4 */
TimBm_Status_t TIM_BM_DisableDmaRequest(TIM_TypeDef *tim, uint32_t ev_mask);

/**
  * @brief  Reads SR, takes = SR & ev_mask (bits the instance has), clears them
  *         with ONE write SR = ~taken (rc_w0: no read-modify-write), then DSB so
  *         the interrupt line has dropped before an ISR returns. Returns taken;
  *         0 for an invalid instance (nothing accessed).
  */
uint32_t       TIM_BM_TakeFlags(TIM_TypeDef *tim, uint32_t ev_mask);   /* SR = ~taken, then DSB */

/** As TakeFlags for CC1OF..CC4OF; ch_mask and the result use TIM_BM_CH(n). */
uint32_t       TIM_BM_TakeOvercapture(TIM_TypeDef *tim, uint32_t ch_mask);   /* CCxOF          */

/**
  * @brief  Polls TIM_BM_TakeFlags(ev_mask) until a flag is taken or max_cycles
  *         DWT cycles have passed (C13). ERR_TIMEOUT also when CYCCNT is not
  *         counting (DWT off), so the wait can never be endless. *taken gets
  *         the flags taken (0 on timeout). ERR_PARAM: taken NULL or a bad mask.
  */
TimBm_Status_t TIM_BM_WaitFlags(TIM_TypeDef *tim, uint32_t ev_mask, uint32_t max_cycles,
                                uint32_t *taken);                           /* C13          */

/** EGR = ev_mask (UG, CC1G..CC4G, TG). Same mask rules as EnableIrq; EGR is write-only. */
TimBm_Status_t TIM_BM_GenerateEvent(TIM_TypeDef *tim, uint32_t ev_mask);   /* EGR           */

/** The interrupt number that serves ONE event of the instance (stm32h755xx.h). */
TimBm_Status_t TIM_BM_GetIRQn(const TIM_TypeDef *tim, uint32_t ev, IRQn_Type *out);

/** The DMAMUX1 request id of ONE event (UPDATE, CC1..CC4), RM0399 table 126. */
TimBm_Status_t TIM_BM_GetDmaRequest(const TIM_TypeDef *tim, uint32_t ev, uint32_t *request_id);

/* ---- Output compare / PWM (channels 1..4) ------------------------------------- */
#define TIM_BM_OC_FROZEN          0U   /* OCxM values, RM0399 40.4.7 */
#define TIM_BM_OC_ACTIVE          1U
#define TIM_BM_OC_INACTIVE        2U
#define TIM_BM_OC_TOGGLE          3U
#define TIM_BM_OC_FORCE_INACTIVE  4U
#define TIM_BM_OC_FORCE_ACTIVE    5U
#define TIM_BM_OC_PWM1            6U
#define TIM_BM_OC_PWM2            7U

typedef struct
{
    TIM_TypeDef *tim;
    uint32_t     channel;      /*!< 1..caps.channels                                        */
    uint32_t     mode;         /*!< TIM_BM_OC_*                                             */
    uint32_t     compare;      /*!< CCR, <= counter max                                     */
    bool         active_low;   /*!< CCxP                                                    */
    bool         preload;      /*!< OCxPE                                                   */
} TimBm_OcConfig_t;

/**
  * @brief  The whole channel before it is enabled (C11): CCMRx (CCxS = 00 output,
  *         OCxM, OCxPE; OCxFE, OCxCE and OCxM[3] cleared) -> CCRx -> CCER.CCxP
  *         (CCxNP cleared) -> read-back. CCxE, CCxNE, MOE, OSSI and OSSR are not
  *         written. ERR_BUSY: CCxE = 1 (CCxS cannot change, RM0399 40.4.7).
  */
TimBm_Status_t TIM_BM_ConfigOutput(const TimBm_OcConfig_t *cfg);   /* CCxE must be 0 (C12)  */

/** CCRx = ccr (<= counter maximum), read back (an input channel's CCR is read-only). */
TimBm_Status_t TIM_BM_SetCompare(TIM_TypeDef *tim, uint32_t channel, uint32_t ccr);

/** CCRx = ccr with no check at all: for ISRs and DMA-free updates. channel 1..4. */
static inline void TIM_BM_WriteCcr(TIM_TypeDef *tim, uint32_t channel, uint32_t ccr)
{
    (&tim->CCR1)[channel - 1U] = ccr;
}

/**
  * @brief  CCxE for every channel of ch_mask, then MOE on the instances with BDTR
  *         (MOE last, C11). Read back. ERR_PARAM: a channel of the mask is not an
  *         output (CCxS != 00: CCxE would enable a capture instead).
  */
TimBm_Status_t TIM_BM_OutputEnable(TIM_TypeDef *tim, uint32_t ch_mask);   /* CCxE, then MOE    */

/** CCxE = 0 for ch_mask; MOE = 0 when no CCxE is left set. Read back. */
TimBm_Status_t TIM_BM_OutputDisable(TIM_TypeDef *tim, uint32_t ch_mask);  /* MOE = 0 when none */

/* ---- Input capture ------------------------------------------------------------ */
#define TIM_BM_IC_DIRECT    1U   /* CCxS = 01 */
#define TIM_BM_IC_INDIRECT  2U   /* CCxS = 10 */
#define TIM_BM_IC_TRC       3U   /* CCxS = 11, needs caps.slave_mode */
#define TIM_BM_EDGE_RISING  0U
#define TIM_BM_EDGE_FALLING 1U
#define TIM_BM_EDGE_BOTH    3U

typedef struct
{
    TIM_TypeDef *tim;
    uint32_t     channel;      /*!< 1..caps.channels                                        */
    uint32_t     selection;    /*!< TIM_BM_IC_*                                             */
    uint32_t     edge;         /*!< TIM_BM_EDGE_*                                           */
    uint32_t     prescaler;    /*!< 1, 2, 4 or 8 events per capture                         */
    uint32_t     filter;       /*!< ICxF, 0..15                                             */
    uint32_t     tisel;        /*!< 0 = the CHx pin; else a value in caps.tisel_mask         */
} TimBm_IcConfig_t;

/**
  * @brief  CCMRx (CCxS, ICxPSC, ICxF) -> CCER (CCxP, CCxNP from edge) -> TISEL
  *         field TIxSEL of the channel's own input TIx -> read-back.
  * @retval ERR_UNSUPPORTED  TRC without a slave controller, INDIRECT on a
  *                          one-channel timer, a TIxSEL value RM0399 marks
  *                          reserved for that instance.
  * @retval ERR_BUSY         CCxE = 1.
  */
TimBm_Status_t TIM_BM_ConfigInput(const TimBm_IcConfig_t *cfg);    /* CCxE must be 0 (C12)  */

/**
  * @brief  PWM input mode (RM0399 40.3.8): TIx captured on two channels, the
  *         period on CCR of channel ti (DIRECT, `edge`) and the high (or low)
  *         time on the other channel (INDIRECT, the opposite edge), with the
  *         slave controller in reset mode on TI1FP1 (ti = 1) or TI2FP2 (ti = 2).
  *         `tisel` programs TIxSEL of TIti. Needs caps.slave_mode and two
  *         channels. edge: RISING or FALLING. ERR_BUSY: CC1E or CC2E set, or
  *         a slave mode already active (SMS != 0).
  */
TimBm_Status_t TIM_BM_ConfigPwmInput(TIM_TypeDef *tim, uint32_t ti /* 1 or 2 */, uint32_t edge,
                                     uint32_t prescaler, uint32_t filter, uint32_t tisel);

/** CCxE for ch_mask, read back. ERR_PARAM: a channel is an output (CCxS = 00). */
TimBm_Status_t TIM_BM_CaptureEnable(TIM_TypeDef *tim, uint32_t ch_mask);
TimBm_Status_t TIM_BM_CaptureDisable(TIM_TypeDef *tim, uint32_t ch_mask);

/** CCRx of an input channel, no check. channel 1..4. */
static inline uint32_t TIM_BM_ReadCapture(const TIM_TypeDef *tim, uint32_t channel)
{
    return (&tim->CCR1)[channel - 1U];
}

/* ---- Master / slave ----------------------------------------------------------- */
#define TIM_BM_MMS_RESET    0U   /* MMS values, RM0399 40.4.2 */
#define TIM_BM_MMS_ENABLE   1U
#define TIM_BM_MMS_UPDATE   2U
#define TIM_BM_MMS_CMP_PULSE 3U
#define TIM_BM_MMS_OC1REF   4U
#define TIM_BM_MMS_OC2REF   5U
#define TIM_BM_MMS_OC3REF   6U
#define TIM_BM_MMS_OC4REF   7U

#define TIM_BM_SMS_DISABLED       0U   /* SMS values, RM0399 40.4.3 */
#define TIM_BM_SMS_RESET          4U
#define TIM_BM_SMS_GATED          5U
#define TIM_BM_SMS_TRIGGER        6U
#define TIM_BM_SMS_EXT_CLOCK1     7U
#define TIM_BM_SMS_RESET_TRIGGER  8U

/* Trigger sources: driver values, NOT the TS[4:0] encoding (TS mixes ITR0-3 =
   00000-00011, TI1F_ED = 00100, TI1FP1 = 00101, TI2FP2 = 00110, ETRF = 00111 and
   ITR4-8 = 01000-01100, RM0399 41.4.3). ETRF has no constant and is refused. */
#define TIM_BM_TRG_ITR(n)   (n)      /* n = 0..8, only where caps.itr_mask has bit n */
#define TIM_BM_TRG_TI1F_ED  16U
#define TIM_BM_TRG_TI1FP1   17U
#define TIM_BM_TRG_TI2FP2   18U

/**
  * @brief  CR2.MMS = mms (TRGO) and SMCR.MSM = msm, read back.
  * @retval ERR_UNSUPPORTED  no TRGO on the instance, mms above caps.mms_max, msm
  *                          without a slave controller, or ES0445 2.16.1: msm with
  *                          OPM = 1 and SMS = reset + trigger (one-pulse would
  *                          miss the trigger).
  */
TimBm_Status_t TIM_BM_ConfigMaster(TIM_TypeDef *tim, uint32_t mms, bool msm);   /* mms <= caps.mms_max */

/**
  * @brief  Slave mode, following the TS note of RM0399 40.4.3: SMS = 0 -> TS ->
  *         SMS, read back. TS is only changed while SMS = 0, so a call with a
  *         slave mode already active (SMS != 0) is refused with ERR_BUSY unless
  *         it asks for TIM_BM_SMS_DISABLED, which is how a slave mode is left.
  * @retval ERR_PARAM        sms not one of TIM_BM_SMS_* (the encoder modes are
  *                          outside the API), trigger not a TIM_BM_TRG_* value,
  *                          GATED on TI1F_ED (RM0399 40.4.3 note).
  * @retval ERR_UNSUPPORTED  no slave controller, an ITR the instance does not wire
  *                          (tables 353, 358, 361, 366), or ES0445 2.16.1:
  *                          RESET_TRIGGER with MSM = 1 and OPM = 1.
  */
TimBm_Status_t TIM_BM_ConfigSlave(TIM_TypeDef *tim, uint32_t sms, uint32_t trigger);

/* ---- Validation only (added in F2) ---------------------------------------------- */
/* Each TIM_BM_Check* runs the checks of the operation it names, in the same order
   (C6) and with the same status, and writes nothing. Like the operation, it reads the
   peripheral only for the state checks (ERR_BUSY, ES0445 2.16.1, CCxS of a channel
   mask). The operation calls it, so there is one copy of each check; tim_ll calls it
   before it writes through the LL inlines. OK means the operation would write. */
#define TIM_BM_USE_IRQ         0U   /* Enable/DisableIrq, GenerateEvent, WaitFlags          */
#define TIM_BM_USE_DMA         1U   /* Enable/DisableDmaRequest                             */
#define TIM_BM_USE_OUTPUT_ON   2U   /* OutputEnable: every channel an output (CCxS = 00)    */
#define TIM_BM_USE_OUTPUT_OFF  3U   /* OutputDisable: the mask only                         */
#define TIM_BM_USE_CAPTURE     4U   /* CaptureEnable/Disable: every channel an input        */

TimBm_Status_t TIM_BM_CheckTimeBase(const TimBm_TimeBaseConfig_t *cfg);              /* InitTimeBase  */
TimBm_Status_t TIM_BM_CheckPeriod(const TIM_TypeDef *tim, uint32_t arr);             /* SetPeriod     */
TimBm_Status_t TIM_BM_CheckOutput(const TimBm_OcConfig_t *cfg);                      /* ConfigOutput  */
TimBm_Status_t TIM_BM_CheckCompare(const TIM_TypeDef *tim, uint32_t channel, uint32_t ccr); /* SetCompare */
TimBm_Status_t TIM_BM_CheckChannels(const TIM_TypeDef *tim, uint32_t ch_mask, uint32_t use);
TimBm_Status_t TIM_BM_CheckInput(const TimBm_IcConfig_t *cfg);                       /* ConfigInput   */
TimBm_Status_t TIM_BM_CheckPwmInput(const TIM_TypeDef *tim, uint32_t ti, uint32_t edge,
                                    uint32_t prescaler, uint32_t filter, uint32_t tisel);
TimBm_Status_t TIM_BM_CheckEvents(const TIM_TypeDef *tim, uint32_t ev_mask, uint32_t use);
TimBm_Status_t TIM_BM_CheckMaster(const TIM_TypeDef *tim, uint32_t mms, bool msm);   /* ConfigMaster  */
TimBm_Status_t TIM_BM_CheckSlave(const TIM_TypeDef *tim, uint32_t sms, uint32_t trigger); /* ConfigSlave */

#ifdef __cplusplus
}
#endif

#endif /* TIM_BM_H */
