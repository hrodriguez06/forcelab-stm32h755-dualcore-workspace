/**
  ******************************************************************************
  * @file    hsem_bm.c
  * @brief   Bare-Metal Hardware Semaphore (HSEM) driver implementation.
  *
  * Rules enforced:
  *   C1:  No fixed semaphore IDs, clear keys, or proc IDs in driver.
  *   C6:  Parameter validation precedes all peripheral register accesses.
  *   C7:  Zero state in RAM (.bss = 0, .data = 0).
  *   C8:  Depends strictly on CMSIS (stm32h7xx.h).
  *   C10: DMB on successful take and before release; all accesses 32-bit.
  *   C11: Release verified before writing; RLR read only in FastTake/TakeOrNotify.
  *   C12: PRIMASK saved and restored, never forced to 0.
  *   C13: Bounded waits in DWT cycles.
  ******************************************************************************
  */

#include "hsem_bm.h"

/* Register bit definitions matching RM0399 Chapter 11 */
#define HSEM_R_LOCK_BIT         (1UL << 31)
#define HSEM_R_COREID_SHIFT     8U
#define HSEM_R_COREID_MASK      (0x0FUL << HSEM_R_COREID_SHIFT)
#define HSEM_R_PROCID_SHIFT     0U
#define HSEM_R_PROCID_MASK      (0xFFUL << HSEM_R_PROCID_SHIFT)

#define HSEM_RLR_LOCK_BIT       (1UL << 31)
#define HSEM_RLR_COREID_SHIFT   8U
#define HSEM_RLR_COREID_MASK    (0x0FUL << HSEM_RLR_COREID_SHIFT)

#define HSEM_CR_KEY_SHIFT       16U
#define HSEM_CR_COREID_SHIFT    8U
#define HSEM_KEYR_KEY_SHIFT     16U

#define CORTEX_M7_PARTNO        0xC27U
#define CORTEX_M4_PARTNO        0xC24U
#define DWT_LAR_UNLOCK_KEY      0xC5ACCE55U

/* Helper: verify core identity and banked clock enable without RAM state */
static inline HsemBm_Status_t CheckInit(void)
{
    uint32_t part_no = (SCB->CPUID & SCB_CPUID_PARTNO_Msk) >> SCB_CPUID_PARTNO_Pos;
#if defined(CORE_CM4)
    if (part_no != CORTEX_M4_PARTNO)
    {
        return HSEM_BM_ERR_CORE;
    }
#else
    if (part_no != CORTEX_M7_PARTNO)
    {
        return HSEM_BM_ERR_CORE;
    }
#endif

    if ((RCC->AHB4ENR & RCC_AHB4ENR_HSEMEN) == 0U)
    {
        return HSEM_BM_ERR_CORE;
    }

    return HSEM_BM_OK;
}

HsemBm_Status_t HSEM_BM_CheckInit(void)
{
    return CheckInit();
}

HsemBm_Status_t HSEM_BM_Init(void)
{
    uint32_t part_no = (SCB->CPUID & SCB_CPUID_PARTNO_Msk) >> SCB_CPUID_PARTNO_Pos;
#if defined(CORE_CM4)
    if (part_no != CORTEX_M4_PARTNO)
    {
        return HSEM_BM_ERR_CORE;
    }
#else
    if (part_no != CORTEX_M7_PARTNO)
    {
        return HSEM_BM_ERR_CORE;
    }
#endif

    /* Enable HSEM peripheral clock on the calling core's AHB4 bank with readback */
    RCC->AHB4ENR |= RCC_AHB4ENR_HSEMEN;
    (void)RCC->AHB4ENR;

    if ((RCC->AHB4ENR & RCC_AHB4ENR_HSEMEN) == 0U)
    {
        return HSEM_BM_ERR_CORE;
    }

    return HSEM_BM_OK;
}

bool HSEM_BM_TimebaseStart(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#if !defined(CORE_CM4)
    /* Unlock DWT Software Lock Access Register on Cortex-M7 */
    *((volatile uint32_t *)0xE0001FB0) = DWT_LAR_UNLOCK_KEY;
#endif
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    uint32_t c0 = DWT->CYCCNT;
    for (volatile uint32_t k = 0U; k < 200U; k++)
    {
        __NOP();
    }
    uint32_t c1 = DWT->CYCCNT;

    return (c1 != c0);
}

uint32_t HSEM_BM_GetCoreId(void)
{
#if defined(CORE_CM4)
    return HSEM_BM_COREID_CPU2; /* 1U */
#else
    return HSEM_BM_COREID_CPU1; /* 3U */
#endif
}

HsemBm_Status_t HSEM_BM_FastTake(uint32_t id)
{
    if (id >= HSEM_BM_NUM_SEMAPHORES)
    {
        return HSEM_BM_ERR_PARAM;
    }

    HsemBm_Status_t init_st = CheckInit();
    if (init_st != HSEM_BM_OK)
    {
        return init_st;
    }

    uint32_t my_core = HSEM_BM_GetCoreId();
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    /* Rule 2 (S1): check R[id] under PRIMASK to detect self-collision */
    uint32_t r_val = HSEM->R[id];
    if ((r_val & HSEM_R_LOCK_BIT) != 0U)
    {
        uint32_t holder_core = (r_val & HSEM_R_COREID_MASK) >> HSEM_R_COREID_SHIFT;
        if (holder_core == my_core)
        {
            __set_PRIMASK(primask);
            return HSEM_BM_BUSY_SELF;
        }
    }

    /* Attempt 1-step lock via RLR[id] */
    uint32_t rlr_val = HSEM->RLR[id];
    uint32_t expected = HSEM_RLR_LOCK_BIT | (my_core << HSEM_RLR_COREID_SHIFT);

    if (rlr_val == expected)
    {
        __DMB();
        __set_PRIMASK(primask);
        return HSEM_BM_OK;
    }

    __set_PRIMASK(primask);
    return HSEM_BM_BUSY;
}

HsemBm_Status_t HSEM_BM_Take(uint32_t id, uint32_t proc_id)
{
    if ((id >= HSEM_BM_NUM_SEMAPHORES) || (proc_id == 0U) || (proc_id > HSEM_BM_PROCID_MAX))
    {
        return HSEM_BM_ERR_PARAM;
    }

    HsemBm_Status_t init_st = CheckInit();
    if (init_st != HSEM_BM_OK)
    {
        return init_st;
    }

    uint32_t my_core = HSEM_BM_GetCoreId();

    /* Rule 3 (RM0399 §11.3.3): read R[id] first */
    uint32_t r_val = HSEM->R[id];
    if ((r_val & HSEM_R_LOCK_BIT) != 0U)
    {
        uint32_t holder_core = (r_val & HSEM_R_COREID_MASK) >> HSEM_R_COREID_SHIFT;
        if (holder_core == my_core)
        {
            return HSEM_BM_BUSY_SELF;
        }
        return HSEM_BM_BUSY;
    }

    /* Write 2-step lock attempt */
    uint32_t write_val = HSEM_R_LOCK_BIT | (my_core << HSEM_R_COREID_SHIFT) | (proc_id & HSEM_R_PROCID_MASK);
    HSEM->R[id] = write_val;

    /* Read back to verify ownership */
    uint32_t read_back = HSEM->R[id];
    if (read_back == write_val)
    {
        __DMB();
        return HSEM_BM_OK;
    }

    if ((read_back & HSEM_R_LOCK_BIT) != 0U)
    {
        uint32_t holder_core = (read_back & HSEM_R_COREID_MASK) >> HSEM_R_COREID_SHIFT;
        if (holder_core == my_core)
        {
            return HSEM_BM_BUSY_SELF;
        }
    }

    return HSEM_BM_BUSY;
}

HsemBm_Status_t HSEM_BM_TakeTimeout(uint32_t id, uint32_t proc_id, uint32_t max_cycles)
{
    return HSEM_BM_TakeTimeoutEx(id, proc_id, max_cycles, (void (*)(void *))0, (void *)0);
}

HsemBm_Status_t HSEM_BM_TakeTimeoutEx(uint32_t id, uint32_t proc_id, uint32_t max_cycles,
                                      void (*relax)(void *ctx), void *ctx)
{
    if ((id >= HSEM_BM_NUM_SEMAPHORES) || (proc_id > HSEM_BM_PROCID_MAX))
    {
        return HSEM_BM_ERR_PARAM;
    }
    if ((max_cycles == 0U) || (max_cycles > 0x7FFFFFFFUL))
    {
        return HSEM_BM_ERR_PARAM;
    }

    HsemBm_Status_t init_st = CheckInit();
    if (init_st != HSEM_BM_OK)
    {
        return init_st;
    }

    /* Rule 5 (C13): CYCCNT must be active */
    if (((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U) ||
        ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U))
    {
        return HSEM_BM_ERR_TIMEBASE;
    }

    uint32_t start_cycles = DWT->CYCCNT;

    for (;;)
    {
        HsemBm_Status_t status;
        if (proc_id == 0U)
        {
            status = HSEM_BM_FastTake(id);
        }
        else
        {
            status = HSEM_BM_Take(id, proc_id);
        }

        if (status != HSEM_BM_BUSY)
        {
            return status;
        }

        if (relax != (void (*)(void *))0)
        {
            relax(ctx);
        }

        uint32_t elapsed = DWT->CYCCNT - start_cycles;
        if (elapsed >= max_cycles)
        {
            return HSEM_BM_TIMEOUT;
        }
    }
}

HsemBm_Status_t HSEM_BM_Release(uint32_t id, uint32_t proc_id)
{
    if ((id >= HSEM_BM_NUM_SEMAPHORES) || (proc_id > HSEM_BM_PROCID_MAX))
    {
        return HSEM_BM_ERR_PARAM;
    }

    HsemBm_Status_t init_st = CheckInit();
    if (init_st != HSEM_BM_OK)
    {
        return init_st;
    }

    uint32_t my_core = HSEM_BM_GetCoreId();

    /* Rule 4 (C11): Honest release. Read and check ownership before writing. */
    uint32_t r_val = HSEM->R[id];
    if ((r_val & HSEM_R_LOCK_BIT) == 0U)
    {
        return HSEM_BM_ERR_NOT_OWNER;
    }

    uint32_t holder_core = (r_val & HSEM_R_COREID_MASK) >> HSEM_R_COREID_SHIFT;
    uint32_t holder_proc = (r_val & HSEM_R_PROCID_MASK) >> HSEM_R_PROCID_SHIFT;

    if ((holder_core != my_core) || (holder_proc != proc_id))
    {
        return HSEM_BM_ERR_NOT_OWNER;
    }

    /* Owner confirmed: write unlock word with LOCK = 0 */
    __DMB();
    HSEM->R[id] = (my_core << HSEM_R_COREID_SHIFT) | (proc_id & HSEM_R_PROCID_MASK);

    return HSEM_BM_OK;
}

bool HSEM_BM_IsLocked(uint32_t id)
{
    if ((id >= HSEM_BM_NUM_SEMAPHORES) || (CheckInit() != HSEM_BM_OK))
    {
        return false;
    }

    /* Rule 6 (C11): Reads HSEM_Rx only, never HSEM_RLRx */
    uint32_t r_val = HSEM->R[id];
    return ((r_val & HSEM_R_LOCK_BIT) != 0U);
}

bool HSEM_BM_GetOwner(uint32_t id, HsemBm_Owner_t *out)
{
    if ((id >= HSEM_BM_NUM_SEMAPHORES) || (out == (HsemBm_Owner_t *)0) || (CheckInit() != HSEM_BM_OK))
    {
        return false;
    }

    uint32_t r_val = HSEM->R[id];
    bool is_locked = ((r_val & HSEM_R_LOCK_BIT) != 0U);

    out->locked = is_locked;
    out->core_id = is_locked ? ((r_val & HSEM_R_COREID_MASK) >> HSEM_R_COREID_SHIFT) : 0U;
    out->proc_id = is_locked ? ((r_val & HSEM_R_PROCID_MASK) >> HSEM_R_PROCID_SHIFT) : 0U;

    return true;
}

uint32_t HSEM_BM_GetLockedMask(uint32_t core_id)
{
    if (((core_id != HSEM_BM_COREID_CPU1) && (core_id != HSEM_BM_COREID_CPU2)) || (CheckInit() != HSEM_BM_OK))
    {
        return 0U;
    }

    uint32_t mask = 0U;
    for (uint32_t i = 0U; i < HSEM_BM_NUM_SEMAPHORES; i++)
    {
        uint32_t r_val = HSEM->R[i];
        if ((r_val & HSEM_R_LOCK_BIT) != 0U)
        {
            uint32_t holder = (r_val & HSEM_R_COREID_MASK) >> HSEM_R_COREID_SHIFT;
            if (holder == core_id)
            {
                mask |= (1UL << i);
            }
        }
    }

    return mask;
}

bool HSEM_BM_SetClearKey(uint16_t key)
{
    if (key == 0U)
    {
        return false;
    }

    HSEM->KEYR = ((uint32_t)key << HSEM_KEYR_KEY_SHIFT);
    return true;
}

HsemBm_Status_t HSEM_BM_ReleaseAllOfCore(uint32_t core_id, uint16_t key)
{
    if ((key == 0U) || ((core_id != HSEM_BM_COREID_CPU1) && (core_id != HSEM_BM_COREID_CPU2)))
    {
        return HSEM_BM_ERR_PARAM;
    }

    uint32_t mask_before = HSEM_BM_GetLockedMask(core_id);
    if (mask_before == 0U)
    {
        return HSEM_BM_OK;
    }

    /* Rule 9: write clear command to HSEM_CR */
    HSEM->CR = ((uint32_t)key << HSEM_CR_KEY_SHIFT) | (core_id << HSEM_CR_COREID_SHIFT);
    __DMB();

    /* Verify whether hardware accepted the key (m7) */
    uint32_t mask_after = HSEM_BM_GetLockedMask(core_id);
    if ((mask_after & mask_before) == 0U)
    {
        return HSEM_BM_OK;
    }

    return HSEM_BM_ERR_PARAM;
}

bool HSEM_BM_NotifyEnable(uint32_t sem_mask)
{
    if ((sem_mask == 0U) || (CheckInit() != HSEM_BM_OK))
    {
        return false;
    }

    /* Rule 7: Clear CnICR first, then enable CnIER under PRIMASK */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    HSEM_COMMON->ICR = sem_mask;
    HSEM_COMMON->IER |= sem_mask;

    __set_PRIMASK(primask);
    return true;
}

bool HSEM_BM_NotifyRearm(uint32_t sem_mask)
{
    if ((sem_mask == 0U) || (CheckInit() != HSEM_BM_OK))
    {
        return false;
    }

    /* R-M1: Re-arm CnIER without clearing CnICR so releases arriving between
     * NotifyTake and Rearm are preserved.
     */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    HSEM_COMMON->IER |= sem_mask;

    __set_PRIMASK(primask);
    return true;
}

bool HSEM_BM_NotifyDisable(uint32_t sem_mask)
{
    if ((sem_mask == 0U) || (CheckInit() != HSEM_BM_OK))
    {
        return false;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    HSEM_COMMON->IER &= ~sem_mask;
    HSEM_COMMON->ICR = sem_mask;

    __set_PRIMASK(primask);
    return true;
}

uint32_t HSEM_BM_NotifyTake(void)
{
    if (CheckInit() != HSEM_BM_OK)
    {
        return 0U;
    }

    uint32_t misr = HSEM_COMMON->MISR;
    if (misr != 0U)
    {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        /* One-shot: disable received bits, clear interrupt status flags */
        HSEM_COMMON->IER &= ~misr;
        HSEM_COMMON->ICR = misr;
        __DSB();

        __set_PRIMASK(primask);
    }
    return misr;
}

uint32_t HSEM_BM_GetRawStatus(void)
{
    if (CheckInit() != HSEM_BM_OK)
    {
        return 0U;
    }
    return HSEM_COMMON->ISR;
}

IRQn_Type HSEM_BM_GetIRQn(void)
{
#if defined(CORE_CM4)
    return HSEM2_IRQn;
#else
    return HSEM1_IRQn;
#endif
}

HsemBm_Status_t HSEM_BM_TakeOrNotify(uint32_t id, uint32_t proc_id)
{
    if (id >= HSEM_BM_NUM_SEMAPHORES)
    {
        return HSEM_BM_ERR_PARAM;
    }

    HsemBm_Status_t init_st = CheckInit();
    if (init_st != HSEM_BM_OK)
    {
        return init_st;
    }

    HsemBm_Status_t status;
    if (proc_id == 0U)
    {
        status = HSEM_BM_FastTake(id);
    }
    else
    {
        status = HSEM_BM_Take(id, proc_id);
    }

    if (status == HSEM_BM_OK)
    {
        return HSEM_BM_OK;
    }
    if (status == HSEM_BM_BUSY_SELF)
    {
        /* M3: Do not arm notification if held by this core; return BUSY_SELF immediately */
        return HSEM_BM_BUSY_SELF;
    }
    if (status != HSEM_BM_BUSY)
    {
        return status;
    }

    /* RM0399 §11.3.7 sequence: clear ICR and retry once */
    HSEM_COMMON->ICR = (1UL << id);

    if (proc_id == 0U)
    {
        status = HSEM_BM_FastTake(id);
    }
    else
    {
        status = HSEM_BM_Take(id, proc_id);
    }

    if (status == HSEM_BM_OK)
    {
        return HSEM_BM_OK;
    }
    if (status == HSEM_BM_BUSY_SELF)
    {
        return HSEM_BM_BUSY_SELF;
    }

    /* Arm notification on failure (held by other core) */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    HSEM_COMMON->IER |= (1UL << id);
    __set_PRIMASK(primask);

    return HSEM_BM_BUSY;
}

/* ---- Boot Handshake (F2 Implementation) ---------------------------------- */

HsemBm_Status_t HSEM_BM_BootWaitForCpu1(const HsemBm_BootWait_t *cfg)
{
#if !defined(CORE_CM4)
    (void)cfg;
    return HSEM_BM_ERR_CORE; /* CPU2 function only */
#else
    if ((cfg == (const HsemBm_BootWait_t *)0) || (cfg->sem_id >= HSEM_BM_NUM_SEMAPHORES))
    {
        return HSEM_BM_ERR_PARAM;
    }

    /* Enable HSEM peripheral clock on CPU2 bank with readback */
    RCC->AHB4ENR |= RCC_AHB4ENR_HSEMEN;
    (void)RCC->AHB4ENR;

    uint32_t sem_mask = (1UL << cfg->sem_id);
    /* m1: Do not clear ICR upon entry so an early release flag is not dropped */
    HSEM_COMMON->IER |= sem_mask;

    uint32_t scr_orig = SCB->SCR;
    SCB->SCR = scr_orig | SCB_SCR_SEVONPEND_Msk | SCB_SCR_SLEEPDEEP_Msk;

    /* D2 Domain Stop mode on CPU2 (PDDS_D2 = 0) */
    PWR->CPU2CR &= ~PWR_CPU2CR_PDDS_D2;

    __DSB();
    __ISB();

    /* Loop until semaphore flag is asserted AND ready condition is met */
    for (;;)
    {
        /* Clear any latched event from previous execution / wake */
        __SEV();
        __WFE();
        /* Enter D2 domain STOP mode */
        __WFE();

        /* B2: Record that CPU2 woke from WFE */
        if (cfg->wake_count != (volatile uint32_t *)0)
        {
            (*cfg->wake_count)++;
        }

        /* Woken up: check if HSEM notification arrived and ready callback passes */
        bool sem_asserted = ((HSEM_COMMON->ISR & sem_mask) != 0U);
        if (sem_asserted && ((cfg->ready == (bool (*)(void *))0) || cfg->ready(cfg->ctx)))
        {
            break;
        }

        /* Not ready yet (spurious EXTI event or premature HSEM release before ready).
           If HSEM was released prematurely, clear ISR flag in ICR so CPU2 can enter
           STOP again and be re-notified on the subsequent valid release. */
        if (sem_asserted)
        {
            HSEM_COMMON->ICR = sem_mask;
        }
    }

    /* Cleanup notification and restore SCR */
    HSEM_COMMON->ICR = sem_mask;
    HSEM_COMMON->IER &= ~sem_mask;
    SCB->SCR = scr_orig;

    /* m2: RM0399 §7.7.4 requires clearing pending IRQ in NVIC after waking via SEVONPEND */
    NVIC_ClearPendingIRQ(HSEM2_IRQn);

    return HSEM_BM_OK;
#endif
}

HsemBm_Status_t HSEM_BM_BootWaitCpu2Parked(uint32_t max_cycles)
{
#if defined(CORE_CM4)
    (void)max_cycles;
    return HSEM_BM_ERR_CORE; /* CPU1 function only */
#else
    if ((max_cycles == 0U) || (max_cycles > 0x7FFFFFFFUL))
    {
        return HSEM_BM_ERR_PARAM;
    }

    if (((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U) ||
        ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U))
    {
        return HSEM_BM_ERR_TIMEBASE;
    }

    uint32_t start_cycles = DWT->CYCCNT;

    while ((RCC->CR & RCC_CR_D2CKRDY) != 0U)
    {
        if ((DWT->CYCCNT - start_cycles) >= max_cycles)
        {
            return HSEM_BM_TIMEOUT;
        }
    }

    return HSEM_BM_OK;
#endif
}

HsemBm_Status_t HSEM_BM_BootReleaseCpu2(uint32_t sem_id, uint32_t max_cycles)
{
#if defined(CORE_CM4)
    (void)sem_id;
    (void)max_cycles;
    return HSEM_BM_ERR_CORE; /* CPU1 function only */
#else
    if (sem_id >= HSEM_BM_NUM_SEMAPHORES)
    {
        return HSEM_BM_ERR_PARAM;
    }
    if ((max_cycles == 0U) || (max_cycles > 0x7FFFFFFFUL))
    {
        return HSEM_BM_ERR_PARAM;
    }

    if (((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U) ||
        ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U))
    {
        return HSEM_BM_ERR_TIMEBASE;
    }

    uint32_t start_cycles = DWT->CYCCNT;

    do
    {
        HsemBm_Status_t take_st = HSEM_BM_FastTake(sem_id);
        if (take_st != HSEM_BM_OK)
        {
            return take_st;
        }

        HsemBm_Status_t rel_st = HSEM_BM_Release(sem_id, 0U);
        if (rel_st != HSEM_BM_OK)
        {
            return rel_st;
        }

        if ((DWT->CYCCNT - start_cycles) >= max_cycles)
        {
            return HSEM_BM_TIMEOUT;
        }
    } while ((RCC->CR & RCC_CR_D2CKRDY) == 0U);

    return HSEM_BM_OK;
#endif
}
