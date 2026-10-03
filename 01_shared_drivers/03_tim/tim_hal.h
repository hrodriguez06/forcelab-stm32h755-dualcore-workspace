/**
  ******************************************************************************
  * @file    tim_hal.h
  * @brief   General-purpose, advanced and basic timers (TIM1-8, TIM12-17),
  *          HAL tier: a thin wrapper of the ST HAL for code that already holds a
  *          TIM_HandleTypeDef.
  *
  *          Driver tier: *_hal. Adds stm32h7xx_hal.h to tim_bm (C8) and builds
  *          nothing without HAL_TIM_MODULE_ENABLED. No state in RAM (C7): the
  *          handle is the caller's.
  *
  *          Contract (03_tim plan, revision 7, "Contrato de API: tim_hal"):
  *            - h == NULL, cfg == NULL, an h->Instance that is not a timer, or an
  *              h->Instance other than cfg->tim: TIM_BM_ERR_PARAM, nothing read or
  *              written; then tim_bm validates the request (TIM_BM_Check*, the
  *              C6 order and states of the tim_bm operation) before any HAL call;
  *            - after each HAL call the fields that call writes are read back
  *              (C12): a HAL error gives TIM_BM_ERR_HAL, HAL_OK with a read-back
  *              that differs gives TIM_BM_ERR_VERIFY;
  *            - no callback and no MSP (C14). HAL_TIM_Base_Init calls the
  *              project's MSP init hook when h->State is RESET;
  *            - TIM_HAL_Deinit calls TIM_BM_Deinit and puts the handle back in
  *              its reset state, without HAL_TIM_Base_DeInit (no MSP call);
  *            - the values are tim_bm's: its configuration structures, channel
  *              1..4 (not TIM_CHANNEL_x), TIM_BM_MMS_*, TIM_BM_SMS_*, TIM_BM_TRG_*.
  *
  *          From the reset image, the same configuration leaves the registers as
  *          tim_bm does (T2). Started from another image, the HAL leaves some bits
  *          tim_bm clears (CR1 UDIS/UIFREMAP, OCxCE, CCxNP of a channel without a
  *          complementary output, ...) and clears some tim_bm leaves (DIER TIE/TDE
  *          in ConfigSlave, CR2 MMS2 and OISx): the list, with the line of
  *          stm32h7xx_hal_tim.c behind each bit, is in the F3 report and, at close,
  *          in this driver's README.
  ******************************************************************************
  */

#ifndef TIM_HAL_H
#define TIM_HAL_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx_hal.h"
#include "tim_bm.h"

#if defined(HAL_TIM_MODULE_ENABLED)

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  As TIM_BM_InitTimeBase, through HAL_TIM_Base_Init (h->Init is filled from
  *         cfg). CEN = 0 required (ERR_BUSY). When CMS != 00, a first
  *         HAL_TIM_Base_Init in up mode clears CMS before the write of the requested
  *         mode (R5-3); OPM through HAL_TIM_OnePulse_Init when it must change; URS
  *         with __HAL_TIM_URS_ENABLE/DISABLE; then UIF is cleared (C11).
  */
TimBm_Status_t TIM_HAL_InitTimeBase(TIM_HandleTypeDef *h, const TimBm_TimeBaseConfig_t *cfg);

/**
  * @brief  As TIM_BM_ConfigOutput: HAL_TIM_PWM_ConfigChannel for PWM1/PWM2,
  *         HAL_TIM_OC_ConfigChannel for the other modes, then OCxPE with
  *         __HAL_TIM_ENABLE/DISABLE_OCxPRELOAD. CCxE must be 0 (ERR_BUSY).
  */
TimBm_Status_t TIM_HAL_ConfigOutput(TIM_HandleTypeDef *h, const TimBm_OcConfig_t *cfg);

/** HAL_TIM_PWM_Start: CCxE, MOE where the instance has BDTR, then CEN except in the
    trigger slave modes. The channel (1..4) must be an output (ERR_PARAM). */
TimBm_Status_t TIM_HAL_StartPwm(TIM_HandleTypeDef *h, uint32_t channel);

/** HAL_TIM_PWM_Stop: CCxE = 0; MOE = 0 and CEN = 0 once no channel is enabled. */
TimBm_Status_t TIM_HAL_StopPwm(TIM_HandleTypeDef *h, uint32_t channel);

/** As TIM_BM_ConfigInput: HAL_TIM_IC_ConfigChannel, then HAL_TIMEx_TISelection. */
TimBm_Status_t TIM_HAL_ConfigInput(TIM_HandleTypeDef *h, const TimBm_IcConfig_t *cfg);

/** HAL_TIM_IC_Start: CCxE, then CEN except in the trigger slave modes. The channel
    (1..4) must be an input (ERR_PARAM). */
TimBm_Status_t TIM_HAL_StartCapture(TIM_HandleTypeDef *h, uint32_t channel);

/** As TIM_BM_ConfigMaster: HAL_TIMEx_MasterConfigSynchronization (MMS2 = 0 on TIM1
    and TIM8). */
TimBm_Status_t TIM_HAL_ConfigMaster(TIM_HandleTypeDef *h, uint32_t mms, bool msm);

/**
  * @brief  As TIM_BM_ConfigSlave(sms, ts): HAL_TIM_SlaveConfigSynchro, called once
  *         per step of the TS note (RM0399 40.4.3): SMS = 0, then TS, then SMS. The
  *         HAL rewrites the input stage of TI1FP1/TI1F_ED/TI2FP2 with the trigger
  *         polarity and filter: tim_hal passes the values already in CCER/CCMR1, so
  *         the stage keeps what ConfigInput gave it. The HAL also clears DIER TIE
  *         and TDE.
  */
TimBm_Status_t TIM_HAL_ConfigSlave(TIM_HandleTypeDef *h, uint32_t sms, uint32_t ts);

/** TIM_BM_Deinit, then the handle as HAL_TIM_Base_DeInit leaves it (State, channel
    states and DMA burst state RESET, unlocked), without the MSP call. */
TimBm_Status_t TIM_HAL_Deinit(TIM_HandleTypeDef *h);

#ifdef __cplusplus
}
#endif

#endif /* HAL_TIM_MODULE_ENABLED */

#endif /* TIM_HAL_H */
