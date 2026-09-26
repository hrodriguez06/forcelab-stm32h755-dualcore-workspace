/**
  ******************************************************************************
  * @file    hsem_hal.c
  * @brief   HAL-tier HSEM driver implementation for STM32H755.
  *
  *          Tier: *_hal (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Wraps ST HAL HSEM functions (HAL_HSEM_*).
  *          Fixes S1, S2, S6, S7, S10 while preserving application symbol
  *          ownership of HAL_HSEM_FreeCallback and IRQ handlers.
  ******************************************************************************
  */

#include "hsem_hal.h"
#include "hsem_bm.h"
#include <stddef.h>

#if defined(HAL_HSEM_MODULE_ENABLED)

/* ---- Semaphore acquisition & release (HAL Tier) -------------------------- */

HAL_StatusTypeDef HSEM_HAL_FastTake(uint32_t id)
{
    if (id > 31U)
    {
        return HAL_ERROR;
    }

    if (HSEM_BM_CheckInit() != HSEM_BM_OK)
    {
        return HAL_ERROR;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    /* Rule 2 (S1): check if already taken by this core */
    uint32_t my_core = HSEM_BM_GetCoreId();
    uint32_t r = HSEM->R[id];
    if ((r & HSEM_R_LOCK) != 0U)
    {
        uint32_t holder_core = (r & HSEM_R_COREID_Msk) >> HSEM_R_COREID_Pos;
        if (holder_core == my_core)
        {
            __set_PRIMASK(primask);
            return HAL_BUSY; /* Map BUSY_SELF -> HAL_BUSY */
        }
    }

    HAL_StatusTypeDef status = HAL_HSEM_FastTake(id);
    if (status == HAL_OK)
    {
        __DMB();
        __set_PRIMASK(primask);
        return HAL_OK;
    }

    __set_PRIMASK(primask);
    return HAL_BUSY;
}

HAL_StatusTypeDef HSEM_HAL_Take(uint32_t id, uint32_t proc_id)
{
    if (id > 31U || proc_id == 0U || proc_id > 255U)
    {
        return HAL_ERROR;
    }

    if (HSEM_BM_CheckInit() != HSEM_BM_OK)
    {
        return HAL_ERROR;
    }

    /* Check if already locked by any core (no re-entry, RM0399 §11.3.3) */
    uint32_t r = HSEM->R[id];
    if ((r & HSEM_R_LOCK) != 0U)
    {
        return HAL_BUSY;
    }

    HAL_StatusTypeDef status = HAL_HSEM_Take(id, proc_id);
    if (status == HAL_OK)
    {
        __DMB();
        return HAL_OK;
    }

    return HAL_BUSY;
}

HAL_StatusTypeDef HSEM_HAL_TakeTimeoutEx(uint32_t id, uint32_t proc_id, uint32_t max_cycles,
                                        void (*relax)(void *ctx), void *ctx)
{
    if (id > 31U || proc_id > 255U || max_cycles == 0U || max_cycles > 0x7FFFFFFFUL)
    {
        return HAL_ERROR;
    }

    if (HSEM_BM_CheckInit() != HSEM_BM_OK)
    {
        return HAL_ERROR;
    }

    /* M2: Check both TRCENA and CYCCNTENA */
    if (((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U) ||
        ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U))
    {
        return HAL_ERROR; /* ERR_TIMEBASE -> HAL_ERROR */
    }

    /* M1: S1 check - if already locked by this core, return HAL_BUSY immediately (BUSY_SELF) */
    uint32_t my_core = HSEM_BM_GetCoreId();
    uint32_t r = HSEM->R[id];
    if ((r & HSEM_R_LOCK) != 0U)
    {
        uint32_t holder_core = (r & HSEM_R_COREID_Msk) >> HSEM_R_COREID_Pos;
        if (holder_core == my_core)
        {
            return HAL_BUSY;
        }
    }

    uint32_t start = DWT->CYCCNT;
    for (;;)
    {
        HAL_StatusTypeDef st = (proc_id == 0U) ? HSEM_HAL_FastTake(id) : HSEM_HAL_Take(id, proc_id);
        if (st == HAL_OK)
        {
            return HAL_OK;
        }
        if (st == HAL_ERROR)
        {
            return st;
        }

        /* Check if it became held by THIS core while spinning */
        r = HSEM->R[id];
        if ((r & HSEM_R_LOCK) != 0U)
        {
            uint32_t holder_core = (r & HSEM_R_COREID_Msk) >> HSEM_R_COREID_Pos;
            if (holder_core == my_core)
            {
                return HAL_BUSY;
            }
        }

        uint32_t elapsed = DWT->CYCCNT - start;
        if (elapsed >= max_cycles)
        {
            return HAL_TIMEOUT;
        }

        if (relax != NULL)
        {
            relax(ctx);
        }
    }
}

HAL_StatusTypeDef HSEM_HAL_TakeTimeout(uint32_t id, uint32_t proc_id, uint32_t max_cycles)
{
    return HSEM_HAL_TakeTimeoutEx(id, proc_id, max_cycles, NULL, NULL);
}

HAL_StatusTypeDef HSEM_HAL_Release(uint32_t id, uint32_t proc_id)
{
    if (id > 31U || proc_id > 255U)
    {
        return HAL_ERROR;
    }

    if (HSEM_BM_CheckInit() != HSEM_BM_OK)
    {
        return HAL_ERROR;
    }

    /* Check ownership before writing (C11) */
    uint32_t my_core = HSEM_BM_GetCoreId();
    uint32_t r = HSEM->R[id];
    if ((r & HSEM_R_LOCK) == 0U)
    {
        return HAL_ERROR; /* Not locked: ERR_NOT_OWNER -> HAL_ERROR */
    }

    uint32_t holder_core = (r & HSEM_R_COREID_Msk) >> HSEM_R_COREID_Pos;
    uint32_t holder_proc = (r & HSEM_R_PROCID_Msk) >> HSEM_R_PROCID_Pos;
    if ((holder_core != my_core) || (holder_proc != proc_id))
    {
        return HAL_ERROR; /* Not owner: ERR_NOT_OWNER -> HAL_ERROR */
    }

    __DMB();
    HAL_HSEM_Release(id, proc_id);
    return HAL_OK;
}

/* ---- Notification: the calling core's line only (HAL Tier) --------------- */

HAL_StatusTypeDef HSEM_HAL_NotifyEnable(uint32_t sem_mask)
{
    if (sem_mask == 0U)
    {
        return HAL_ERROR;
    }

    if (HSEM_BM_CheckInit() != HSEM_BM_OK)
    {
        return HAL_ERROR;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    __HAL_HSEM_CLEAR_FLAG(sem_mask);
    HAL_HSEM_ActivateNotification(sem_mask);

    __set_PRIMASK(primask);
    return HAL_OK;
}

HAL_StatusTypeDef HSEM_HAL_NotifyDisable(uint32_t sem_mask)
{
    if (sem_mask == 0U)
    {
        return HAL_ERROR;
    }

    if (HSEM_BM_CheckInit() != HSEM_BM_OK)
    {
        return HAL_ERROR;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    HAL_HSEM_DeactivateNotification(sem_mask);
    __HAL_HSEM_CLEAR_FLAG(sem_mask);

    __set_PRIMASK(primask);
    return HAL_OK;
}

/* ---- Callback registration & IRQ Service (HAL Tier) ---------------------- */

static HsemHal_FreeCallback_t s_callback = NULL;
static void                  *s_callback_ctx = NULL;

HAL_StatusTypeDef HSEM_HAL_RegisterCallback(HsemHal_FreeCallback_t cb, void *ctx)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    s_callback = cb;
    s_callback_ctx = ctx;
    __set_PRIMASK(primask);
    return HAL_OK;
}

void HSEM_HAL_IRQService(void)
{
    HAL_HSEM_IRQHandler();
    __DSB();
}

void HSEM_HAL_Dispatch(uint32_t sem_mask)
{
    if (s_callback != NULL)
    {
        s_callback(sem_mask, s_callback_ctx);
    }
}

#endif /* HAL_HSEM_MODULE_ENABLED */
