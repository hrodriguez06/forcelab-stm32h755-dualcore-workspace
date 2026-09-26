/**
  ******************************************************************************
  * @file    hsem_hal.h
  * @brief   HAL-tier HSEM driver for STM32H755.
  *
  *          Driver tier: *_hal (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Wraps ST HAL HSEM functions (HAL_HSEM_*):
  *            - FastTake (1-step)
  *            - Take (2-step)
  *            - TakeTimeout / TakeTimeoutEx
  *            - Release
  *            - NotifyEnable / NotifyDisable
  *            - RegisterCallback / IRQService / Dispatch
  *
  *          Remediates ST HAL bugs and deficiencies:
  *            - S1: Same-core context collision detection on FastTake under PRIMASK.
  *            - S2: Missing memory barriers (__DMB).
  *            - S6: Clears stale interrupt flag before activating notification.
  *            - S7: Guards notification enable/disable with PRIMASK.
  *            - S10: Validates parameters upfront and normalizes return statuses:
  *                   OK -> HAL_OK, BUSY/BUSY_SELF -> HAL_BUSY,
  *                   TIMEOUT -> HAL_TIMEOUT, ERR_* -> HAL_ERROR.
  *            - Non-destructive: Does NOT define HAL_HSEM_FreeCallback or
  *              HSEMx_IRQHandler (preserves project symbol ownership).
  ******************************************************************************
  */

#ifndef HSEM_HAL_H
#define HSEM_HAL_H

#include "stm32h7xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

#if defined(HAL_HSEM_MODULE_ENABLED)

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Semaphore acquisition & release (HAL Tier) -------------------------- */

/**
  * @brief  Acquires a semaphore in 1-step mode (PROCID = 0) with S1 collision detection.
  * @param  id Semaphore ID (0..31).
  * @retval HAL_OK on success,
  *         HAL_BUSY if already held by this core (BUSY_SELF) or other core,
  *         HAL_ERROR on invalid parameter (id > 31).
  */
HAL_StatusTypeDef HSEM_HAL_FastTake(uint32_t id);

/**
  * @brief  Acquires a semaphore in 2-step mode with explicit process ID.
  * @param  id      Semaphore ID (0..31).
  * @param  proc_id Process ID (1..255).
  * @retval HAL_OK on success,
  *         HAL_BUSY if held by this core or other core,
  *         HAL_ERROR on invalid parameters.
  */
HAL_StatusTypeDef HSEM_HAL_Take(uint32_t id, uint32_t proc_id);

/**
  * @brief  Attempts acquisition with a cycle-bounded timeout using DWT.
  * @param  id         Semaphore ID (0..31).
  * @param  proc_id    0 for 1-step, 1..255 for 2-step.
  * @param  max_cycles Cycle limit measured via DWT CYCCNT (1..2^31-1).
  * @retval HAL_OK, HAL_TIMEOUT, HAL_BUSY, or HAL_ERROR.
  */
HAL_StatusTypeDef HSEM_HAL_TakeTimeout(uint32_t id, uint32_t proc_id, uint32_t max_cycles);

/**
  * @brief  Attempts acquisition with cycle timeout and cooperative relax hook.
  */
HAL_StatusTypeDef HSEM_HAL_TakeTimeoutEx(uint32_t id, uint32_t proc_id, uint32_t max_cycles,
                                        void (*relax)(void *ctx), void *ctx);

/**
  * @brief  Releases a semaphore held by the calling core.
  * @param  id      Semaphore ID (0..31).
  * @param  proc_id Process ID used during acquisition (0 for 1-step, 1..255 for 2-step).
  * @retval HAL_OK on success,
  *         HAL_ERROR if not owner or parameter error.
  */
HAL_StatusTypeDef HSEM_HAL_Release(uint32_t id, uint32_t proc_id);

/* ---- Notification: the calling core's line only (HAL Tier) --------------- */

/**
  * @brief  Enables notifications for semaphores, clearing stale flags first (S6, S7).
  * @param  sem_mask Bitmask of semaphores (bit N = semaphore N).
  * @retval HAL_OK on success, HAL_ERROR if sem_mask == 0.
  */
HAL_StatusTypeDef HSEM_HAL_NotifyEnable(uint32_t sem_mask);

/**
  * @brief  Disables notifications for semaphores under PRIMASK (S7).
  * @param  sem_mask Bitmask of semaphores (bit N = semaphore N).
  * @retval HAL_OK on success, HAL_ERROR if sem_mask == 0.
  */
HAL_StatusTypeDef HSEM_HAL_NotifyDisable(uint32_t sem_mask);

/* ---- Callback registration & IRQ Service (HAL Tier) ---------------------- */

typedef void (*HsemHal_FreeCallback_t)(uint32_t sem_mask, void *ctx);

/**
  * @brief  Registers the application notification callback for this core.
  * @param  cb  Callback function invoked upon semaphore release.
  * @param  ctx User context pointer passed to callback.
  * @retval HAL_OK.
  */
HAL_StatusTypeDef HSEM_HAL_RegisterCallback(HsemHal_FreeCallback_t cb, void *ctx);

/**
  * @brief  HSEM IRQ service routine. Call from the project's HSEMx_IRQHandler.
  *         Invokes HAL_HSEM_IRQHandler() and ends with __DSB().
  */
void              HSEM_HAL_IRQService(void);

/**
  * @brief  Dispatches notifications to the registered callback.
  *         Call from the project's HAL_HSEM_FreeCallback(sem_mask).
  * @param  sem_mask Bitmask of released semaphores.
  */
void              HSEM_HAL_Dispatch(uint32_t sem_mask);

#ifdef __cplusplus
}
#endif

#endif /* HAL_HSEM_MODULE_ENABLED */

#endif /* HSEM_HAL_H */
