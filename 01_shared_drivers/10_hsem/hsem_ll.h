/**
  ******************************************************************************
  * @file    hsem_ll.h
  * @brief   Low-Layer HSEM driver for STM32H755.
  *
  *          Driver tier: *_ll (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Wraps ST LL static inline functions and CMSIS core:
  *            - LL_HSEM_1StepLock
  *            - LL_HSEM_2StepLock
  *            - LL_HSEM_ReleaseLock
  *            - LL_HSEM_GetStatus
  *            - LL_HSEM_EnableIT_C* / LL_HSEM_DisableIT_C*
  *            - LL_HSEM_ClearFlag_C* / LL_HSEM_IsActiveFlag_C*
  *
  *          Key properties:
  *            - Zero state in RAM (.bss = 0, .data = 0).
  *            - Exact semantic and status parity with hsem_bm.
  *            - Self-contained header, does not require USE_FULL_LL_DRIVER,
  *              and does not include stm32h7xx_hal.h.
  *            - Enforces memory barriers (__DMB) and S1 same-core collision
  *              detection under PRIMASK.
  ******************************************************************************
  */

#ifndef HSEM_LL_H
#define HSEM_LL_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32h7xx.h"
#include "stm32h7xx_ll_hsem.h"
#include "hsem_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Semaphore acquisition & release (LL Tier) --------------------------- */

/**
  * @brief  Acquires a semaphore in 1-step mode (PROCID = 0) using LL.
  * @param  id Semaphore ID (0..31).
  * @retval HSEM_BM_OK on success,
  *         HSEM_BM_BUSY if held by the other core,
  *         HSEM_BM_BUSY_SELF if already held by this core,
  *         HSEM_BM_ERR_PARAM if id > 31.
  */
HsemBm_Status_t HSEM_LL_FastTake(uint32_t id);

/**
  * @brief  Acquires a semaphore in 2-step mode with explicit process ID using LL.
  * @param  id      Semaphore ID (0..31).
  * @param  proc_id Process ID (1..255).
  * @retval HSEM_BM_OK on success,
  *         HSEM_BM_BUSY if held by the other core,
  *         HSEM_BM_BUSY_SELF if already held by this core,
  *         HSEM_BM_ERR_PARAM if invalid arguments.
  */
HsemBm_Status_t HSEM_LL_Take(uint32_t id, uint32_t proc_id);

/**
  * @brief  Attempts acquisition with a cycle-bounded timeout using LL.
  * @param  id         Semaphore ID (0..31).
  * @param  proc_id    0 for 1-step, 1..255 for 2-step.
  * @param  max_cycles Cycle limit measured via DWT CYCCNT (1..2^31-1).
  * @retval HSEM_BM_OK, HSEM_BM_TIMEOUT, or parameter/timebase error.
  */
HsemBm_Status_t HSEM_LL_TakeTimeout(uint32_t id, uint32_t proc_id, uint32_t max_cycles);

/**
  * @brief  Attempts acquisition with cycle timeout and cooperative relax hook.
  */
HsemBm_Status_t HSEM_LL_TakeTimeoutEx(uint32_t id, uint32_t proc_id, uint32_t max_cycles,
                                     void (*relax)(void *ctx), void *ctx);

/**
  * @brief  Releases a semaphore held by the calling core using LL.
  * @param  id      Semaphore ID (0..31).
  * @param  proc_id Process ID used during acquisition (0 for 1-step, 1..255 for 2-step).
  * @retval HSEM_BM_OK on success,
  *         HSEM_BM_ERR_NOT_OWNER if not locked or held by another context/core,
  *         HSEM_BM_ERR_PARAM on invalid parameter.
  */
HsemBm_Status_t HSEM_LL_Release(uint32_t id, uint32_t proc_id);

/* ---- Notification: the calling core's line only (LL Tier) ---------------- */

/**
  * @brief  Enables notifications for a set of semaphores on this core using LL.
  * @param  sem_mask Bitmask of semaphores (bit N = semaphore N).
  * @retval true on success, false on invalid parameter.
  */
bool            HSEM_LL_NotifyEnable(uint32_t sem_mask);

/**
  * @brief  Disables notifications for a set of semaphores on this core using LL.
  * @param  sem_mask Bitmask of semaphores (bit N = semaphore N).
  * @retval true on success, false on invalid parameter.
  */
bool            HSEM_LL_NotifyDisable(uint32_t sem_mask);

/**
  * @brief  One-shot notification acknowledge service for the calling core using LL.
  * @retval Bitmask of semaphores that triggered the interrupt.
  */
uint32_t        HSEM_LL_NotifyTake(void);

/* Note: Diagnostic queries (IsLocked, GetOwner, GetLockedMask), recovery,
   and boot handshake are provided by hsem_bm and are not re-implemented
   in the LL tier (C8). */

#ifdef __cplusplus
}
#endif

#endif /* HSEM_LL_H */
