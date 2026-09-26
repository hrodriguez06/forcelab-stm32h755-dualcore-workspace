/**
  ******************************************************************************
  * @file    hsem_ll.c
  * @brief   Low-Layer HSEM driver implementation for STM32H755.
  *
  *          Tier: *_ll (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Zero state in RAM (.bss = 0, .data = 0).
  *          Implements parity with hsem_bm using ST LL inline helpers.
  ******************************************************************************
  */

#include "hsem_ll.h"
#include <stddef.h>

/* ---- Semaphore acquisition & release (LL Tier) --------------------------- */

HsemBm_Status_t HSEM_LL_FastTake(uint32_t id)
{
    if (id > 31U)
    {
        return HSEM_BM_ERR_PARAM;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    /* Rule 2 (S1): check if already taken by this core */
    if (LL_HSEM_IsSemaphoreLocked(HSEM, id) &&
        (LL_HSEM_GetCoreId(HSEM, id) == LL_HSEM_COREID))
    {
        __set_PRIMASK(primask);
        return HSEM_BM_BUSY_SELF;
    }

    uint32_t status = LL_HSEM_1StepLock(HSEM, id);
    if (status == 0UL)
    {
        __DMB();
        __set_PRIMASK(primask);
        return HSEM_BM_OK;
    }

    __set_PRIMASK(primask);
    return HSEM_BM_BUSY;
}

HsemBm_Status_t HSEM_LL_Take(uint32_t id, uint32_t proc_id)
{
    if (id > 31U || proc_id == 0U || proc_id > 255U)
    {
        return HSEM_BM_ERR_PARAM;
    }

    /* Check if already held by this core (no re-entry) or other core */
    if (LL_HSEM_IsSemaphoreLocked(HSEM, id))
    {
        if (LL_HSEM_GetCoreId(HSEM, id) == LL_HSEM_COREID)
        {
            return HSEM_BM_BUSY_SELF;
        }
        return HSEM_BM_BUSY;
    }

    uint32_t status = LL_HSEM_2StepLock(HSEM, id, proc_id);
    if (status == 0UL)
    {
        __DMB();
        return HSEM_BM_OK;
    }

    if (LL_HSEM_IsSemaphoreLocked(HSEM, id) &&
        (LL_HSEM_GetCoreId(HSEM, id) == LL_HSEM_COREID))
    {
        return HSEM_BM_BUSY_SELF;
    }

    return HSEM_BM_BUSY;
}

HsemBm_Status_t HSEM_LL_TakeTimeoutEx(uint32_t id, uint32_t proc_id, uint32_t max_cycles,
                                     void (*relax)(void *ctx), void *ctx)
{
    if (id > 31U || proc_id > 255U || max_cycles == 0U || max_cycles > 0x7FFFFFFFUL)
    {
        return HSEM_BM_ERR_PARAM;
    }

    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
    {
        return HSEM_BM_ERR_TIMEBASE;
    }

    uint32_t start = DWT->CYCCNT;
    for (;;)
    {
        HsemBm_Status_t st = (proc_id == 0U) ? HSEM_LL_FastTake(id) : HSEM_LL_Take(id, proc_id);
        if (st == HSEM_BM_OK)
        {
            return HSEM_BM_OK;
        }
        if (st == HSEM_BM_ERR_PARAM)
        {
            return st;
        }

        uint32_t elapsed = DWT->CYCCNT - start;
        if (elapsed >= max_cycles)
        {
            return HSEM_BM_TIMEOUT;
        }

        if (relax != NULL)
        {
            relax(ctx);
        }
    }
}

HsemBm_Status_t HSEM_LL_TakeTimeout(uint32_t id, uint32_t proc_id, uint32_t max_cycles)
{
    return HSEM_LL_TakeTimeoutEx(id, proc_id, max_cycles, NULL, NULL);
}

HsemBm_Status_t HSEM_LL_Release(uint32_t id, uint32_t proc_id)
{
    if (id > 31U || proc_id > 255U)
    {
        return HSEM_BM_ERR_PARAM;
    }

    /* Check ownership before writing (C11) */
    if (!LL_HSEM_IsSemaphoreLocked(HSEM, id) ||
        (LL_HSEM_GetCoreId(HSEM, id) != LL_HSEM_COREID) ||
        (LL_HSEM_GetProcessId(HSEM, id) != proc_id))
    {
        return HSEM_BM_ERR_NOT_OWNER;
    }

    __DMB();
    LL_HSEM_ReleaseLock(HSEM, id, proc_id);
    return HSEM_BM_OK;
}

/* ---- Notification: the calling core's line only (LL Tier) ---------------- */

bool HSEM_LL_NotifyEnable(uint32_t sem_mask)
{
    if (sem_mask == 0U)
    {
        return false;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

#if defined(CORE_CM4)
    LL_HSEM_ClearFlag_C2ICR(HSEM, sem_mask);
    LL_HSEM_EnableIT_C2IER(HSEM, sem_mask);
#else
    LL_HSEM_ClearFlag_C1ICR(HSEM, sem_mask);
    LL_HSEM_EnableIT_C1IER(HSEM, sem_mask);
#endif

    __set_PRIMASK(primask);
    return true;
}

bool HSEM_LL_NotifyDisable(uint32_t sem_mask)
{
    if (sem_mask == 0U)
    {
        return false;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

#if defined(CORE_CM4)
    LL_HSEM_DisableIT_C2IER(HSEM, sem_mask);
    LL_HSEM_ClearFlag_C2ICR(HSEM, sem_mask);
#else
    LL_HSEM_DisableIT_C1IER(HSEM, sem_mask);
    LL_HSEM_ClearFlag_C1ICR(HSEM, sem_mask);
#endif

    __set_PRIMASK(primask);
    return true;
}

uint32_t HSEM_LL_NotifyTake(void)
{
    uint32_t active;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

#if defined(CORE_CM4)
    active = READ_REG(HSEM->C2MISR);
    if (active != 0U)
    {
        LL_HSEM_DisableIT_C2IER(HSEM, active);
        LL_HSEM_ClearFlag_C2ICR(HSEM, active);
        __DSB();
    }
#else
    active = READ_REG(HSEM->C1MISR);
    if (active != 0U)
    {
        LL_HSEM_DisableIT_C1IER(HSEM, active);
        LL_HSEM_ClearFlag_C1ICR(HSEM, active);
        __DSB();
    }
#endif

    __set_PRIMASK(primask);
    return active;
}
