/**
  ******************************************************************************
  * @file    hsem_selftest.c
  * @brief   HSEM driver self-test implementation for Cortex-M7 (F1 Selftest).
  ******************************************************************************
  */

#include "hsem_selftest.h"
#include "hsem_selftest_shared.h"
#include "hsem_bm.h"
#include "hsem_ll.h"
#include "hsem_hal.h"
#include "board_def.h"
#include "shared_memory.h"
#include "app_log.h"
#include "stm32h7xx.h"
#include "stm32h7xx_hal_hsem.h"

#define H10_STEPS  13U

typedef struct {
    uint32_t R[8];    /* R[24] through R[31] */
    uint32_t IER;     /* CnIER */
    uint32_t ISR;     /* CnISR */
} HsemSnapshot_t;

typedef struct {
    HsemBm_Status_t st;
    HsemSnapshot_t  snap;
} H10_StepResult_t;

typedef struct {
    HAL_StatusTypeDef st;
    HsemSnapshot_t    snap;
} H10_StepResult_HAL_t;

static void Hsem_TakeSnapshot_M7(HsemSnapshot_t *s)
{
    for (uint32_t i = 0; i < 8; i++)
    {
        s->R[i] = HSEM->R[24 + i];
    }
    s->IER = HSEM->C1IER;
    s->ISR = HSEM->C1ISR;
}

static void Hsem_ResetTestRegisters_M7(void)
{
    (void)HSEM_BM_Release(24, 0);
    (void)HSEM_BM_Release(24, 99);
    (void)HSEM_BM_Release(25, 42);
    (void)HSEM_BM_Release(25, 43);
    (void)HSEM_BM_Release(26, 0);
    HSEM->C1IER = 0U;
    HSEM->C1ICR = 0xFFFFFFFFUL;
    __DSB();
    __ISB();
}

static void RunSequence_BM_M7(H10_StepResult_t res[H10_STEPS])
{
    Hsem_ResetTestRegisters_M7();

    /* Step 0: FastTake on sem 24 */
    res[0].st = HSEM_BM_FastTake(24);
    Hsem_TakeSnapshot_M7(&res[0].snap);

    /* Step 1: Re-entrant FastTake on sem 24 -> BUSY_SELF */
    res[1].st = HSEM_BM_FastTake(24);
    Hsem_TakeSnapshot_M7(&res[1].snap);

    /* Step 2: Re-entrant TakeTimeoutEx on sem 24 -> BUSY_SELF (M1) */
    res[2].st = HSEM_BM_TakeTimeoutEx(24, 0, 1000U, NULL, NULL);
    Hsem_TakeSnapshot_M7(&res[2].snap);

    /* Step 3: 2-step Take on sem 25, proc 42 */
    res[3].st = HSEM_BM_Take(25, 42);
    Hsem_TakeSnapshot_M7(&res[3].snap);

    /* Step 4: Re-entrant Take on sem 25, proc 43 -> BUSY_SELF */
    res[4].st = HSEM_BM_Take(25, 43);
    Hsem_TakeSnapshot_M7(&res[4].snap);

    /* Step 5: Release sem 25 with wrong proc 99 -> ERR_NOT_OWNER */
    res[5].st = HSEM_BM_Release(25, 99);
    Hsem_TakeSnapshot_M7(&res[5].snap);

    /* Step 6: Release sem 25 with correct proc 42 -> OK */
    res[6].st = HSEM_BM_Release(25, 42);
    Hsem_TakeSnapshot_M7(&res[6].snap);

    /* Step 7: Release sem 24 with correct proc 0 -> OK */
    res[7].st = HSEM_BM_Release(24, 0);
    Hsem_TakeSnapshot_M7(&res[7].snap);

    /* Step 8: Parameter error FastTake sem 32 -> ERR_PARAM */
    res[8].st = HSEM_BM_FastTake(32);
    Hsem_TakeSnapshot_M7(&res[8].snap);

    /* Step 9: Parameter error Take proc 256 -> ERR_PARAM */
    res[9].st = HSEM_BM_Take(24, 256);
    Hsem_TakeSnapshot_M7(&res[9].snap);

    /* Step 10: Parameter error Release sem 32 -> ERR_PARAM */
    res[10].st = HSEM_BM_Release(32, 0);
    Hsem_TakeSnapshot_M7(&res[10].snap);

    /* Step 11: NotifyEnable sem 26 */
    bool en = HSEM_BM_NotifyEnable(1UL << 26);
    res[11].st = en ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M7(&res[11].snap);

    /* Step 12: NotifyDisable sem 26 */
    bool dis = HSEM_BM_NotifyDisable(1UL << 26);
    res[12].st = dis ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M7(&res[12].snap);
}

static void RunSequence_LL_M7(H10_StepResult_t res[H10_STEPS])
{
    Hsem_ResetTestRegisters_M7();

    /* Step 0: FastTake on sem 24 */
    res[0].st = (HsemBm_Status_t)HSEM_LL_FastTake(24);
    Hsem_TakeSnapshot_M7(&res[0].snap);

    /* Step 1: Re-entrant FastTake on sem 24 -> BUSY_SELF */
    res[1].st = (HsemBm_Status_t)HSEM_LL_FastTake(24);
    Hsem_TakeSnapshot_M7(&res[1].snap);

    /* Step 2: Re-entrant TakeTimeoutEx on sem 24 -> BUSY_SELF (M1) */
    res[2].st = (HsemBm_Status_t)HSEM_LL_TakeTimeoutEx(24, 0, 1000U, NULL, NULL);
    Hsem_TakeSnapshot_M7(&res[2].snap);

    /* Step 3: 2-step Take on sem 25, proc 42 */
    res[3].st = (HsemBm_Status_t)HSEM_LL_Take(25, 42);
    Hsem_TakeSnapshot_M7(&res[3].snap);

    /* Step 4: Re-entrant Take on sem 25, proc 43 -> BUSY_SELF */
    res[4].st = (HsemBm_Status_t)HSEM_LL_Take(25, 43);
    Hsem_TakeSnapshot_M7(&res[4].snap);

    /* Step 5: Release sem 25 with wrong proc 99 -> ERR_NOT_OWNER */
    res[5].st = (HsemBm_Status_t)HSEM_LL_Release(25, 99);
    Hsem_TakeSnapshot_M7(&res[5].snap);

    /* Step 6: Release sem 25 with correct proc 42 -> OK */
    res[6].st = (HsemBm_Status_t)HSEM_LL_Release(25, 42);
    Hsem_TakeSnapshot_M7(&res[6].snap);

    /* Step 7: Release sem 24 with correct proc 0 -> OK */
    res[7].st = (HsemBm_Status_t)HSEM_LL_Release(24, 0);
    Hsem_TakeSnapshot_M7(&res[7].snap);

    /* Step 8: Parameter error FastTake sem 32 -> ERR_PARAM */
    res[8].st = (HsemBm_Status_t)HSEM_LL_FastTake(32);
    Hsem_TakeSnapshot_M7(&res[8].snap);

    /* Step 9: Parameter error Take proc 256 -> ERR_PARAM */
    res[9].st = (HsemBm_Status_t)HSEM_LL_Take(24, 256);
    Hsem_TakeSnapshot_M7(&res[9].snap);

    /* Step 10: Parameter error Release sem 32 -> ERR_PARAM */
    res[10].st = (HsemBm_Status_t)HSEM_LL_Release(32, 0);
    Hsem_TakeSnapshot_M7(&res[10].snap);

    /* Step 11: NotifyEnable sem 26 */
    bool en = HSEM_LL_NotifyEnable(1UL << 26);
    res[11].st = en ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M7(&res[11].snap);

    /* Step 12: NotifyDisable sem 26 */
    bool dis = HSEM_LL_NotifyDisable(1UL << 26);
    res[12].st = dis ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M7(&res[12].snap);
}

static HAL_StatusTypeDef BmStatusToHal(HsemBm_Status_t bm)
{
    switch (bm)
    {
        case HSEM_BM_OK:
            return HAL_OK;
        case HSEM_BM_BUSY:
        case HSEM_BM_BUSY_SELF:
            return HAL_BUSY;
        case HSEM_BM_TIMEOUT:
            return HAL_TIMEOUT;
        case HSEM_BM_ERR_PARAM:
        case HSEM_BM_ERR_NOT_OWNER:
        case HSEM_BM_ERR_TIMEBASE:
        case HSEM_BM_ERR_CORE:
        default:
            return HAL_ERROR;
    }
}

static void RunSequence_HAL_M7(H10_StepResult_HAL_t res[H10_STEPS])
{
    Hsem_ResetTestRegisters_M7();

    /* Step 0: FastTake on sem 24 */
    res[0].st = HSEM_HAL_FastTake(24);
    Hsem_TakeSnapshot_M7(&res[0].snap);

    /* Step 1: Re-entrant FastTake on sem 24 -> HAL_BUSY */
    res[1].st = HSEM_HAL_FastTake(24);
    Hsem_TakeSnapshot_M7(&res[1].snap);

    /* Step 2: Re-entrant TakeTimeoutEx on sem 24 -> HAL_BUSY (M1) */
    res[2].st = HSEM_HAL_TakeTimeoutEx(24, 0, 1000U, NULL, NULL);
    Hsem_TakeSnapshot_M7(&res[2].snap);

    /* Step 3: 2-step Take on sem 25, proc 42 */
    res[3].st = HSEM_HAL_Take(25, 42);
    Hsem_TakeSnapshot_M7(&res[3].snap);

    /* Step 4: Re-entrant Take on sem 25, proc 43 -> HAL_BUSY */
    res[4].st = HSEM_HAL_Take(25, 43);
    Hsem_TakeSnapshot_M7(&res[4].snap);

    /* Step 5: Release sem 25 with wrong proc 99 -> HAL_ERROR */
    res[5].st = HSEM_HAL_Release(25, 99);
    Hsem_TakeSnapshot_M7(&res[5].snap);

    /* Step 6: Release sem 25 with correct proc 42 -> HAL_OK */
    res[6].st = HSEM_HAL_Release(25, 42);
    Hsem_TakeSnapshot_M7(&res[6].snap);

    /* Step 7: Release sem 24 with correct proc 0 -> HAL_OK */
    res[7].st = HSEM_HAL_Release(24, 0);
    Hsem_TakeSnapshot_M7(&res[7].snap);

    /* Step 8: Parameter error FastTake sem 32 -> HAL_ERROR */
    res[8].st = HSEM_HAL_FastTake(32);
    Hsem_TakeSnapshot_M7(&res[8].snap);

    /* Step 9: Parameter error Take proc 256 -> HAL_ERROR */
    res[9].st = HSEM_HAL_Take(24, 256);
    Hsem_TakeSnapshot_M7(&res[9].snap);

    /* Step 10: Parameter error Release sem 32 -> HAL_ERROR */
    res[10].st = HSEM_HAL_Release(32, 0);
    Hsem_TakeSnapshot_M7(&res[10].snap);

    /* Step 11: NotifyEnable sem 26 */
    res[11].st = HSEM_HAL_NotifyEnable(1UL << 26);
    Hsem_TakeSnapshot_M7(&res[11].snap);

    /* Step 12: NotifyDisable sem 26 */
    res[12].st = HSEM_HAL_NotifyDisable(1UL << 26);
    Hsem_TakeSnapshot_M7(&res[12].snap);
}

static bool CompareSnapshots_M7(const H10_StepResult_t bm[H10_STEPS],
                               const H10_StepResult_t ll[H10_STEPS],
                               bool log_mismatch)
{
    for (uint32_t i = 0; i < H10_STEPS; i++)
    {
        if (bm[i].st != ll[i].st)
        {
            if (log_mismatch)
            {
                APP_LOG("[m7_hsem_test] H10 step %lu status mismatch: BM=%d LL=%d",
                        i, (int)bm[i].st, (int)ll[i].st);
            }
            return false;
        }
        for (uint32_t r = 0; r < 8; r++)
        {
            if (bm[i].snap.R[r] != ll[i].snap.R[r])
            {
                if (log_mismatch)
                {
                    APP_LOG("[m7_hsem_test] H10 step %lu R[%lu] mismatch: BM=0x%08lX LL=0x%08lX",
                            i, 24 + r, bm[i].snap.R[r], ll[i].snap.R[r]);
                }
                return false;
            }
        }
        if (bm[i].snap.IER != ll[i].snap.IER)
        {
            if (log_mismatch)
            {
                APP_LOG("[m7_hsem_test] H10 step %lu IER mismatch: BM=0x%08lX LL=0x%08lX",
                        i, bm[i].snap.IER, ll[i].snap.IER);
            }
            return false;
        }
        if (bm[i].snap.ISR != ll[i].snap.ISR)
        {
            if (log_mismatch)
            {
                APP_LOG("[m7_hsem_test] H10 step %lu ISR mismatch: BM=0x%08lX LL=0x%08lX",
                        i, bm[i].snap.ISR, ll[i].snap.ISR);
            }
            return false;
        }
    }
    return true;
}

static bool CompareSnapshots_BM_HAL_M7(const H10_StepResult_t bm[H10_STEPS],
                                      const H10_StepResult_HAL_t hal[H10_STEPS],
                                      bool log_mismatch)
{
    for (uint32_t i = 0; i < H10_STEPS; i++)
    {
        HAL_StatusTypeDef expected_hal = BmStatusToHal(bm[i].st);
        if (expected_hal != hal[i].st)
        {
            if (log_mismatch)
            {
                APP_LOG("[m7_hsem_test] H10 step %lu HAL status mismatch: BM->HAL=%d HAL=%d",
                        i, (int)expected_hal, (int)hal[i].st);
            }
            return false;
        }
        for (uint32_t r = 0; r < 8; r++)
        {
            if (bm[i].snap.R[r] != hal[i].snap.R[r])
            {
                if (log_mismatch)
                {
                    APP_LOG("[m7_hsem_test] H10 step %lu HAL R[%lu] mismatch: BM=0x%08lX HAL=0x%08lX",
                            i, 24 + r, bm[i].snap.R[r], hal[i].snap.R[r]);
                }
                return false;
            }
        }
        if (bm[i].snap.IER != hal[i].snap.IER)
        {
            if (log_mismatch)
            {
                APP_LOG("[m7_hsem_test] H10 step %lu HAL IER mismatch: BM=0x%08lX HAL=0x%08lX",
                        i, bm[i].snap.IER, hal[i].snap.IER);
            }
            return false;
        }
        if (bm[i].snap.ISR != hal[i].snap.ISR)
        {
            if (log_mismatch)
            {
                APP_LOG("[m7_hsem_test] H10 step %lu HAL ISR mismatch: BM=0x%08lX HAL=0x%08lX",
                        i, bm[i].snap.ISR, hal[i].snap.ISR);
            }
            return false;
        }
    }
    return true;
}

static volatile uint32_t s_relax_call_count = 0U;
static void RelaxHook(void *ctx)
{
    (void)ctx;
    s_relax_call_count++;
}

/* NVIC ISR state on CM7 (B3) */
static volatile uint32_t s_m7_irq_count = 0U;

static void M7_HalCallback(uint32_t sem_mask, void *ctx)
{
    (void)ctx;
    if ((sem_mask & (1UL << HSEM_TEST_ID_NOTIFY)) != 0U)
    {
        s_m7_irq_count++;
        HSEM_TEST_SHARED->h9_m7_notifications = s_m7_irq_count;
        /* Re-arm for next notification (HAL notification is one-shot) */
        (void)HSEM_HAL_NotifyEnable(1UL << HSEM_TEST_ID_NOTIFY);
    }
}

void HAL_HSEM_FreeCallback(uint32_t sem_mask)
{
    HSEM_HAL_Dispatch(sem_mask);
}

void HSEM_SelfTest_CM7_IRQHandler(void)
{
    HSEM_HAL_IRQService();
}

void HSEM_SelfTest_CM7_Run(void)
{
    APP_LOG(" ");
    APP_LOG("=================================================================");
#if defined(FW_GIT_REV)
    APP_LOG(" STM32H755 HSEM Driver Self-Test - Rev %s", FW_GIT_REV);
#else
    APP_LOG(" STM32H755 HSEM Driver Self-Test");
#endif
    APP_LOG(" Phase F1-F4: Core, Characterization, Contention, Parity & Recovery");
    APP_LOG("=================================================================");

    /* 1. Ensure HSEM clock is enabled on CM7 */
    __HAL_RCC_HSEM_CLK_ENABLE();
    (void)RCC->AHB4ENR; /* Readback */

    /* 2. DWT CYCCNT initialization on CM7 with LAR unlock */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    *((volatile uint32_t *)0xE0001FB0) = 0xC5ACCE55U; /* Unlock DWT LAR */
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    uint32_t t0 = DWT->CYCCNT;
    for (volatile uint32_t k = 0; k < 500U; k++)
    {
        __NOP();
    }
    uint32_t t1 = DWT->CYCCNT;
    uint32_t delta = t1 - t0;

    HSEM_TEST_SHARED->cm7_dwt_delta = delta;
    HSEM_TEST_SHARED->cm7_dwt_ok = (delta > 0U) ? 1U : 0U;

    APP_LOG("[m7_hsem_test] DWT CYCCNT running: %lu (delta=%lu)",
            HSEM_TEST_SHARED->cm7_dwt_ok, HSEM_TEST_SHARED->cm7_dwt_delta);

    /* 3. B1 Raw Timing Baseline on CM7 (using ID 25) */
    HSEM->R[25] = (HSEM_BM_COREID_CPU1 << 8);
    __DMB();

    uint32_t c_start = DWT->CYCCNT;
    volatile uint32_t rlr_val = HSEM->RLR[25];
    uint32_t fast_take_cyc = DWT->CYCCNT - c_start;
    (void)rlr_val;

    c_start = DWT->CYCCNT;
    HSEM->R[25] = (HSEM_BM_COREID_CPU1 << 8);
    uint32_t rel_cyc = DWT->CYCCNT - c_start;
    __DMB();

    c_start = DWT->CYCCNT;
    HSEM->R[25] = (1U << 31) | (HSEM_BM_COREID_CPU1 << 8) | 7U;
    volatile uint32_t r_read = HSEM->R[25];
    uint32_t take_cyc = DWT->CYCCNT - c_start;
    (void)r_read;

    HSEM->R[25] = (HSEM_BM_COREID_CPU1 << 8) | 7U;
    __DMB();

    HSEM_TEST_SHARED->cm7_raw_fast_take_cycles = fast_take_cyc;
    HSEM_TEST_SHARED->cm7_raw_take_cycles = take_cyc;
    HSEM_TEST_SHARED->cm7_raw_release_cycles = rel_cyc;

    APP_LOG("[m7_hsem_test] B1 raw timing (400 MHz): fast_take=%lu cyc, take=%lu cyc, release=%lu cyc",
            fast_take_cyc, take_cyc, rel_cyc);

    /* 4. H0 Characterization on CM7 */
    bool h0_ok = true;
    (void)HSEM_BM_Release(HSEM_TEST_M7_LOCK1, 0U);
    uint32_t rlr = HSEM->RLR[HSEM_TEST_M7_LOCK1];
    uint32_t expected_rlr = (1UL << 31) | (HSEM_BM_COREID_CPU1 << 8);
    if (rlr == expected_rlr)
    {
        uint32_t r = HSEM->R[HSEM_TEST_M7_LOCK1];
        if ((((r >> 8) & 0xFFU) != HSEM_BM_COREID_CPU1) || (HSEM_BM_Release(HSEM_TEST_M7_LOCK1, 0U) != HSEM_BM_OK))
        {
            h0_ok = false;
        }
    }
    else
    {
        h0_ok = false;
        (void)HSEM_BM_Release(HSEM_TEST_M7_LOCK1, 0U);
    }

    HsemBm_Owner_t owner = { 0 };
    if (!HSEM_BM_GetOwner(HSEM_TEST_M7_LOCK1, &owner) || owner.locked || (owner.core_id != 0U))
    {
        h0_ok = false;
    }
    if (HSEM_BM_IsLocked(HSEM_TEST_M7_LOCK1))
    {
        h0_ok = false;
    }

    /* Parameter rejection tests (C6) */
    if (HSEM_BM_FastTake(32U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Take(32U, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Take(HSEM_TEST_M7_LOCK1, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Take(HSEM_TEST_M7_LOCK1, 256U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Release(32U, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Release(HSEM_TEST_M7_LOCK1, 256U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_SetClearKey(0U) != false) h0_ok = false;
    if (HSEM_BM_ReleaseAllOfCore(2U, 0xA5C3U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_ReleaseAllOfCore(HSEM_BM_COREID_CPU1, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_NotifyEnable(0U) != false) h0_ok = false;

    HSEM_TEST_SHARED->h0_m7_pass = h0_ok ? 1U : 0U;
    APP_LOG("[m7_hsem_test] H0 Characterization: %s", h0_ok ? "PASS" : "FAIL");

    /* 5. H3 Release Protection on CM7 */
    bool h3_ok = true;
    if (HSEM_BM_Take(HSEM_TEST_M7_LOCK1, 7U) == HSEM_BM_OK)
    {
        /* Raw release with wrong proc 8 */
        HSEM->R[HSEM_TEST_M7_LOCK1] = (HSEM_BM_COREID_CPU1 << 8) | 8U;
        __DMB();
        uint32_t r = HSEM->R[HSEM_TEST_M7_LOCK1];
        if (((r & (1UL << 31)) == 0U) || ((r & 0xFFU) != 7U))
        {
            h3_ok = false;
        }

        /* HSEM_BM_Release with wrong proc */
        if (HSEM_BM_Release(HSEM_TEST_M7_LOCK1, 8U) != HSEM_BM_ERR_NOT_OWNER)
        {
            h3_ok = false;
        }

        /* Wait for CM4 to be ready to test H3 Case B */
        uint32_t wait_m4_h3 = 0U;
        while ((HSEM_TEST_SHARED->phase != 1U) && (wait_m4_h3++ < 10000000U))
        {
            __NOP();
        }
        if (HSEM_TEST_SHARED->phase != 1U)
        {
            h3_ok = false;
        }

        /* Signal CM4 that CM7 holds lock (phase == 2) */
        HSEM_TEST_SHARED->phase = 2U;
        __DMB();

        /* Wait for CM4 to acknowledge attempt (phase == 3) */
        uint32_t wait_ack = 0U;
        while ((HSEM_TEST_SHARED->phase != 3U) && (wait_ack++ < 10000000U))
        {
            __NOP();
        }
        if (HSEM_TEST_SHARED->phase != 3U)
        {
            h3_ok = false;
        }

        /* Verify lock is STILL held by CM7 proc 7 */
        r = HSEM->R[HSEM_TEST_M7_LOCK1];
        if (((r & (1UL << 31)) == 0U) || ((r & 0xFFU) != 7U))
        {
            h3_ok = false;
        }

        if (HSEM_BM_Release(HSEM_TEST_M7_LOCK1, 7U) != HSEM_BM_OK)
        {
            h3_ok = false;
        }

        HSEM_TEST_SHARED->phase = 4U;
        __DMB();
    }
    else
    {
        h3_ok = false;
    }
    HSEM_TEST_SHARED->h3_m7_pass = h3_ok ? 1U : 0U;
    APP_LOG("[m7_hsem_test] H3 Release Protection: %s", h3_ok ? "PASS" : "FAIL");

    /* 6. H7 Same-Core Context Collision on CM7 (S1) - BM & HAL (B4) */
    bool h7_ok = true;

    /* (a) 1-step collision */
    if (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) == HSEM_BM_OK)
    {
        /* Negative control: raw HAL_HSEM_FastTake from context B reproduces bug S1 (returns HAL_OK) */
        HAL_StatusTypeDef raw_hal = HAL_HSEM_FastTake(HSEM_TEST_ID_LOCK1);
        HSEM_TEST_SHARED->h7_neg_raw_hal = (uint32_t)raw_hal;
        APP_LOG("[m7_hsem_test] H7 negative control: raw HAL_HSEM_FastTake=%d (hazard S1 reproduced: %s)",
                raw_hal, (raw_hal == HAL_OK) ? "YES" : "NO");

        if (raw_hal != HAL_OK)
        {
            /* Hazard S1 was not reproduced: negative control fails */
            h7_ok = false;
        }

        /* Driver BM detects collision: returns BUSY_SELF */
        if (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) != HSEM_BM_BUSY_SELF)
        {
            h7_ok = false;
        }

        /* Driver HAL wrapper detects collision: returns HAL_BUSY */
        if (HSEM_HAL_FastTake(HSEM_TEST_ID_LOCK1) != HAL_BUSY)
        {
            h7_ok = false;
        }

        HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 0U);
    }
    else
    {
        h7_ok = false;
    }

    /* (b) 2-step collision */
    if (HSEM_BM_Take(HSEM_TEST_ID_LOCK2, 1U) == HSEM_BM_OK)
    {
        if (HSEM_BM_Take(HSEM_TEST_ID_LOCK2, 2U) != HSEM_BM_BUSY_SELF)
        {
            h7_ok = false;
        }
        if (HSEM_HAL_Take(HSEM_TEST_ID_LOCK2, 2U) != HAL_BUSY)
        {
            h7_ok = false;
        }
        HSEM_BM_Release(HSEM_TEST_ID_LOCK2, 1U);
    }
    else
    {
        h7_ok = false;
    }

    HSEM_TEST_SHARED->h7_m7_pass = h7_ok ? 1U : 0U;
    APP_LOG("[m7_hsem_test] H7 Context Collision (S1 mitigation): %s", h7_ok ? "PASS" : "FAIL");

    /* 7. H10 Parity on CM7 (BM <-> LL <-> HAL across 14 steps, M4) */
    static H10_StepResult_t bm_res[H10_STEPS];
    static H10_StepResult_t ll_res[H10_STEPS];
    static H10_StepResult_HAL_t hal_res[H10_STEPS];

    RunSequence_BM_M7(bm_res);
    RunSequence_LL_M7(ll_res);
    RunSequence_HAL_M7(hal_res);

    bool h10_bm_ll_ok = CompareSnapshots_M7(bm_res, ll_res, true);
    bool h10_bm_hal_ok = CompareSnapshots_BM_HAL_M7(bm_res, hal_res, true);

    /* Negative control for LL: mutate snapshot to confirm comparison detects mismatch */
    static H10_StepResult_t mutated_ll[H10_STEPS];
    for (uint32_t i = 0U; i < H10_STEPS; i++)
    {
        mutated_ll[i] = ll_res[i];
    }
    mutated_ll[1].snap.R[1] ^= 0x1U;
    bool neg_detected_ll = !CompareSnapshots_M7(bm_res, mutated_ll, false);

    /* Negative control for HAL: mutate snapshot to confirm comparison detects mismatch */
    static H10_StepResult_HAL_t mutated_hal[H10_STEPS];
    for (uint32_t i = 0U; i < H10_STEPS; i++)
    {
        mutated_hal[i] = hal_res[i];
    }
    mutated_hal[1].snap.R[1] ^= 0x1U;
    bool neg_detected_hal = !CompareSnapshots_BM_HAL_M7(bm_res, mutated_hal, false);

    bool h10_all_ok = h10_bm_ll_ok && h10_bm_hal_ok && neg_detected_ll && neg_detected_hal;
    HSEM_TEST_SHARED->h10_m7_pass = h10_all_ok ? 1U : 0U;
    APP_LOG("[m7_hsem_test] H10 Parity (BM <-> LL <-> HAL, 14 steps): %s (LL_match=%d, HAL_match=%d, neg_ll=%d, neg_hal=%d)",
            h10_all_ok ? "PASS" : "FAIL", h10_bm_ll_ok, h10_bm_hal_ok, neg_detected_ll, neg_detected_hal);

    Hsem_ResetTestRegisters_M7();

    /* 8. Synchronize with CM4 for dual-core tests */
    APP_LOG("[m7_hsem_test] Waiting for CM4 ready...");
    uint32_t wait_cm4 = 0;
    while ((HSEM_TEST_SHARED->cm4_ready == 0U) && (wait_cm4++ < 10000000U))
    {
        __NOP();
    }
    if (HSEM_TEST_SHARED->cm4_ready == 0U)
    {
        APP_LOG("[m7_hsem_test] ERROR: CM4 not ready");
        return;
    }

    /* ---- H2: Negative Control (Contention without lock, F0 / M5) ---- */
    APP_LOG("[m7_hsem_test] Starting H2 Negative Control (contention without lock)...");
    HSEM_TEST_SHARED->h2_shared_counter = 0U;
    HSEM_TEST_SHARED->cm7_done = 0U;
    HSEM_TEST_SHARED->cm4_done = 0U;
    __DMB();

    HSEM_TEST_SHARED->go_signal = 1U;
    __DMB();

    /* Part A: Counter RMW without lock */
    uint32_t m7_h2_count = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H2_ITERATIONS; i++)
    {
        uint32_t val = HSEM_TEST_SHARED->h2_shared_counter;
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
        HSEM_TEST_SHARED->h2_shared_counter = val + 1U;
        m7_h2_count++;
    }
    HSEM_TEST_SHARED->h2_m7_count = m7_h2_count;
    __DMB();
    HSEM_TEST_SHARED->cm7_done = 1U;

    while (HSEM_TEST_SHARED->cm4_done != 1U)
    {
        __NOP();
    }

    uint32_t expected_h2 = HSEM_TEST_SHARED->h2_m4_count + HSEM_TEST_SHARED->h2_m7_count;
    uint32_t actual_h2 = HSEM_TEST_SHARED->h2_shared_counter;
    uint32_t lost_h2 = (expected_h2 > actual_h2) ? (expected_h2 - actual_h2) : 0U;
    HSEM_TEST_SHARED->h2_lost_increments = lost_h2;

    APP_LOG("[m7_hsem_test] H2 Part A (counter without lock): N=%lu, m7=%lu, m4=%lu, final=%lu, lost=%lu (%s)",
            (uint32_t)HSEM_TEST_H2_ITERATIONS, m7_h2_count, HSEM_TEST_SHARED->h2_m4_count,
            actual_h2, lost_h2, (lost_h2 > 0U) ? "PASS: data loss reproduced" : "FAIL: 0 losses");

    /* Part B: Torn register read without lock */
    for (uint32_t w = 0U; w < 8U; w++)
    {
        HSEM_TEST_SHARED->h2_shared_reg[w] = 0U;
    }
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->cm7_done = 0U;
    __DMB();

    HSEM_TEST_SHARED->go_signal = 2U;
    __DMB();

    uint32_t h2_torn = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H2_TORN_ITERS; i++)
    {
        uint32_t w0 = HSEM_TEST_SHARED->h2_shared_reg[0];
        uint32_t w1 = HSEM_TEST_SHARED->h2_shared_reg[1];
        uint32_t w2 = HSEM_TEST_SHARED->h2_shared_reg[2];
        uint32_t w3 = HSEM_TEST_SHARED->h2_shared_reg[3];
        uint32_t w4 = HSEM_TEST_SHARED->h2_shared_reg[4];
        uint32_t w5 = HSEM_TEST_SHARED->h2_shared_reg[5];
        uint32_t w6 = HSEM_TEST_SHARED->h2_shared_reg[6];
        uint32_t w7 = HSEM_TEST_SHARED->h2_shared_reg[7];

        if ((w0 != w1) || (w0 != w2) || (w0 != w3) || (w0 != w4) ||
            (w0 != w5) || (w0 != w6) || (w0 != w7))
        {
            h2_torn++;
        }
    }
    HSEM_TEST_SHARED->h2_torn_reads = h2_torn;
    __DMB();
    HSEM_TEST_SHARED->cm7_done = 2U;

    while (HSEM_TEST_SHARED->cm4_done != 2U)
    {
        __NOP();
    }

    APP_LOG("[m7_hsem_test] H2 Part B (torn register without lock): N=%lu, torn_reads=%lu (%s)",
            (uint32_t)HSEM_TEST_H2_TORN_ITERS, h2_torn,
            (h2_torn > 0U) ? "PASS: torn reads reproduced" : "FAIL: 0 torn reads");

    bool h2_ok = (lost_h2 > 0U) && (h2_torn > 0U);
    HSEM_TEST_SHARED->h2_pass = h2_ok ? 1U : 0U;

    /* ---- H1: Mutual Exclusion under HSEM lock (1-step FastTake) ---- */
    APP_LOG("[m7_hsem_test] Starting H1 1-step FastTake (%lu increments per core under lock)...", (uint32_t)HSEM_TEST_H1_ITERATIONS);
    HSEM_TEST_SHARED->h1_shared_counter = 0U;
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->cm7_done = 0U;
    __DMB();

    HSEM_TEST_SHARED->go_signal = 3U;
    __DMB();

    uint32_t m7_h1_count = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H1_ITERATIONS; i++)
    {
        while (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) != HSEM_BM_OK)
        {
            __NOP();
        }
        HSEM_TEST_SHARED->h1_shared_counter++;
        m7_h1_count++;
        HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 0U);
    }
    HSEM_TEST_SHARED->h1_m7_private_count = m7_h1_count;
    __DMB();
    HSEM_TEST_SHARED->cm7_done = 3U;

    while (HSEM_TEST_SHARED->cm4_done != 3U)
    {
        __NOP();
    }

    uint32_t expected_h1 = HSEM_TEST_SHARED->h1_m4_private_count + HSEM_TEST_SHARED->h1_m7_private_count;
    uint32_t actual_h1 = HSEM_TEST_SHARED->h1_shared_counter;
    uint32_t lost_h1 = (expected_h1 > actual_h1) ? (expected_h1 - actual_h1) : 0U;
    bool h1_ok = (lost_h1 == 0U);
    HSEM_TEST_SHARED->h1_pass = h1_ok ? 1U : 0U;

    APP_LOG("[m7_hsem_test] H1 (1-step FastTake): total=%lu, expected=%lu, lost=%lu (%s)",
            actual_h1, expected_h1, lost_h1, h1_ok ? "PASS: exact mutual exclusion" : "FAIL: lost increments");

    /* ---- H1: Mutual Exclusion under HSEM lock (2-step Take) (M5) ---- */
    APP_LOG("[m7_hsem_test] Starting H1 2-step Take (%lu increments per core under lock)...", (uint32_t)HSEM_TEST_H1_2STEP_ITERS);
    HSEM_TEST_SHARED->h1_2step_shared_counter = 0U;
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->cm7_done = 0U;
    __DMB();

    HSEM_TEST_SHARED->go_signal = 4U;
    __DMB();

    uint32_t m7_h1_2step_count = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H1_2STEP_ITERS; i++)
    {
        while (HSEM_BM_Take(HSEM_TEST_ID_LOCK2, 7U) != HSEM_BM_OK)
        {
            __NOP();
        }
        HSEM_TEST_SHARED->h1_2step_shared_counter++;
        m7_h1_2step_count++;
        HSEM_BM_Release(HSEM_TEST_ID_LOCK2, 7U);
    }
    HSEM_TEST_SHARED->h1_2step_m7_private_count = m7_h1_2step_count;
    __DMB();
    HSEM_TEST_SHARED->cm7_done = 4U;

    while (HSEM_TEST_SHARED->cm4_done != 4U)
    {
        __NOP();
    }

    uint32_t expected_h1_2step = HSEM_TEST_SHARED->h1_2step_m4_private_count + HSEM_TEST_SHARED->h1_2step_m7_private_count;
    uint32_t actual_h1_2step = HSEM_TEST_SHARED->h1_2step_shared_counter;
    uint32_t lost_h1_2step = (expected_h1_2step > actual_h1_2step) ? (expected_h1_2step - actual_h1_2step) : 0U;
    bool h1_2step_ok = (lost_h1_2step == 0U);
    HSEM_TEST_SHARED->h1_2step_pass = h1_2step_ok ? 1U : 0U;

    APP_LOG("[m7_hsem_test] H1 (2-step Take): total=%lu, expected=%lu, lost=%lu (%s)",
            actual_h1_2step, expected_h1_2step, lost_h1_2step, h1_2step_ok ? "PASS: exact mutual exclusion" : "FAIL: lost increments");

    /* ---- H1b: Torn Register with Lock ---- */
    APP_LOG("[m7_hsem_test] Starting H1b (%lu reads of 8-word array under lock)...", (uint32_t)HSEM_TEST_H1B_ITERATIONS);
    for (uint32_t w = 0U; w < 8U; w++)
    {
        HSEM_TEST_SHARED->h1b_shared_reg[w] = 0U;
    }
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->cm7_done = 0U;
    __DMB();

    HSEM_TEST_SHARED->go_signal = 5U;
    __DMB();

    uint32_t torn_reads = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H1B_ITERATIONS; i++)
    {
        while (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) != HSEM_BM_OK)
        {
            __NOP();
        }
        uint32_t w0 = HSEM_TEST_SHARED->h1b_shared_reg[0];
        uint32_t w1 = HSEM_TEST_SHARED->h1b_shared_reg[1];
        uint32_t w2 = HSEM_TEST_SHARED->h1b_shared_reg[2];
        uint32_t w3 = HSEM_TEST_SHARED->h1b_shared_reg[3];
        uint32_t w4 = HSEM_TEST_SHARED->h1b_shared_reg[4];
        uint32_t w5 = HSEM_TEST_SHARED->h1b_shared_reg[5];
        uint32_t w6 = HSEM_TEST_SHARED->h1b_shared_reg[6];
        uint32_t w7 = HSEM_TEST_SHARED->h1b_shared_reg[7];
        HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 0U);

        if ((w0 != w1) || (w0 != w2) || (w0 != w3) || (w0 != w4) ||
            (w0 != w5) || (w0 != w6) || (w0 != w7))
        {
            torn_reads++;
        }
    }
    HSEM_TEST_SHARED->h1b_torn_reads = torn_reads;
    __DMB();
    HSEM_TEST_SHARED->cm7_done = 5U;

    while (HSEM_TEST_SHARED->cm4_done != 5U)
    {
        __NOP();
    }

    bool h1b_ok = (torn_reads == 0U);
    HSEM_TEST_SHARED->h1b_pass = h1b_ok ? 1U : 0U;
    APP_LOG("[m7_hsem_test] H1b Result: N=%lu, torn_reads=%lu (%s)",
            (uint32_t)HSEM_TEST_H1B_ITERATIONS, torn_reads,
            h1b_ok ? "PASS: 0 torn reads" : "FAIL: torn reads detected");

    /* ---- H4: Bounded Timeout & Symmetric Case (M5) ---- */
    APP_LOG("[m7_hsem_test] Starting H4 Bounded Timeout...");
    bool h4_ok = true;

    /* Case A: free semaphore -> immediate OK */
    if (HSEM_BM_TakeTimeout(HSEM_TEST_ID_TIMEOUT, 0U, 4000000U) != HSEM_BM_OK)
    {
        h4_ok = false;
    }
    HSEM_BM_Release(HSEM_TEST_ID_TIMEOUT, 0U);

    /* Case B: CM4 holds for 5 ms, CM7 timeout set to 10 ms (4,000,000 cyc @ 400 MHz) */
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->go_signal = 6U;
    __DMB();

    while (HSEM_TEST_SHARED->cm4_done != 6U)
    {
        __NOP();
    }

    uint32_t t_start = HAL_GetTick();
    HsemBm_Status_t st_b = HSEM_BM_TakeTimeout(HSEM_TEST_ID_TIMEOUT, 0U, 4000000U);
    uint32_t t_elapsed_b = HAL_GetTick() - t_start;
    if ((st_b != HSEM_BM_OK) || (t_elapsed_b > 8U))
    {
        h4_ok = false;
    }
    HSEM_BM_Release(HSEM_TEST_ID_TIMEOUT, 0U);

    /* Case C: CM4 holds for 25 ms, CM7 timeout 10 ms (4,000,000 cyc) */
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->go_signal = 7U;
    __DMB();

    while (HSEM_TEST_SHARED->cm4_done != 7U)
    {
        __NOP();
    }

    s_relax_call_count = 0U;
    t_start = HAL_GetTick();
    HsemBm_Status_t st_c = HSEM_BM_TakeTimeoutEx(HSEM_TEST_ID_TIMEOUT, 0U, 4000000U, RelaxHook, (void *)0);
    uint32_t t_elapsed_c = HAL_GetTick() - t_start;

    if ((st_c != HSEM_BM_TIMEOUT) || (t_elapsed_c < 10U) || (t_elapsed_c > 12U) || (s_relax_call_count == 0U))
    {
        h4_ok = false;
    }

    HSEM_TEST_SHARED->h4_measured_ms = t_elapsed_c;
    HSEM_TEST_SHARED->h4_relax_calls = s_relax_call_count;
    HSEM_TEST_SHARED->h4_m7_pass = h4_ok ? 1U : 0U;

    APP_LOG("[m7_hsem_test] H4 CM7 Result: elapsed=%lu ms, relax_calls=%lu, status=%s (%s)",
            t_elapsed_c, s_relax_call_count, (st_c == HSEM_BM_TIMEOUT) ? "TIMEOUT" : "ERR",
            h4_ok ? "PASS" : "FAIL");

    /* Case D: Symmetric H4 (CM7 holds, CM4 waits with timeout) (M5) */
    HSEM_BM_FastTake(HSEM_TEST_ID_TIMEOUT);
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->go_signal = 8U;
    __DMB();

    /* CM7 holds for 5 ms, then releases */
    HAL_Delay(5);
    HSEM_BM_Release(HSEM_TEST_ID_TIMEOUT, 0U);

    while (HSEM_TEST_SHARED->cm4_done != 8U)
    {
        __NOP();
    }

    /* CM7 holds for 25 ms, CM4 timeout is 10 ms */
    HSEM_BM_FastTake(HSEM_TEST_ID_TIMEOUT);
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->go_signal = 9U;
    __DMB();

    HAL_Delay(25);
    HSEM_BM_Release(HSEM_TEST_ID_TIMEOUT, 0U);

    while (HSEM_TEST_SHARED->cm4_done != 9U)
    {
        __NOP();
    }

    bool h4_sym_ok = (HSEM_TEST_SHARED->h4_m4_pass != 0U);
    APP_LOG("[m7_hsem_test] H4 Symmetric (CM4 timeout on CM7 lock): %s", h4_sym_ok ? "PASS" : "FAIL");

    /* ---- H8: Recovery (ReleaseAllOfCore) ---- */
    APP_LOG("[m7_hsem_test] Starting H8 Recovery...");
    bool h8_ok = true;

    /* Precondition R4: Verify CM4 does not hold IDs < 24 */
    uint32_t cm4_mask = HSEM_BM_GetLockedMask(HSEM_BM_COREID_CPU2);
    if ((cm4_mask & 0x00FFFFFFUL) != 0U)
    {
        APP_LOG("[m7_hsem_test] H8 Precondition R4 failed: CM4 holds IDs < 24 (mask=0x%08lX)", cm4_mask);
        h8_ok = false;
    }

    /* CM7 takes ID 28 */
    HSEM_BM_FastTake(HSEM_TEST_ID_RECOV_CM7);

    /* Signal CM4 to take IDs 24..27 */
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->go_signal = 10U;
    __DMB();

    while (HSEM_TEST_SHARED->cm4_done != 10U)
    {
        __NOP();
    }

    /* Verify CM4 holds 24..27 */
    uint32_t m4_held = HSEM_BM_GetLockedMask(HSEM_BM_COREID_CPU2);
    if ((m4_held & 0x0F000000UL) != 0x0F000000UL)
    {
        h8_ok = false;
    }

    /* (a) Try release with WRONG key (0x1234) -> returns ERR_PARAM, nothing released */
    HsemBm_Status_t rel_wrong = HSEM_BM_ReleaseAllOfCore(HSEM_BM_COREID_CPU2, 0x1234U);
    if (rel_wrong != HSEM_BM_ERR_PARAM)
    {
        h8_ok = false;
    }
    if ((HSEM_BM_GetLockedMask(HSEM_BM_COREID_CPU2) & 0x0F000000UL) != 0x0F000000UL)
    {
        h8_ok = false;
    }

    /* (b) Release with CORRECT key */
    HSEM_BM_SetClearKey(HSEM_TEST_CLEAR_KEY);
    HsemBm_Status_t rel_correct = HSEM_BM_ReleaseAllOfCore(HSEM_BM_COREID_CPU2, HSEM_TEST_CLEAR_KEY);
    if (rel_correct != HSEM_BM_OK)
    {
        h8_ok = false;
    }
    if ((HSEM_BM_GetLockedMask(HSEM_BM_COREID_CPU2) & 0x0F000000UL) != 0U)
    {
        h8_ok = false;
    }

    /* Verify CM7 ID 28 is STILL held intact */
    if (!HSEM_BM_IsLocked(HSEM_TEST_ID_RECOV_CM7))
    {
        h8_ok = false;
    }
    HSEM_BM_Release(HSEM_TEST_ID_RECOV_CM7, 0U);

    HSEM_TEST_SHARED->h8_pass = h8_ok ? 1U : 0U;
    APP_LOG("[m7_hsem_test] H8 Result: %s", h8_ok ? "PASS" : "FAIL");

    /* ---- H9: Notifications (NVIC Interrupt-driven, B3) ---- */
    APP_LOG("[m7_hsem_test] Starting H9 Notifications (NVIC interrupt-driven)...");
    bool h9_ok = true;

    /* Negative Control 1: Wait for CM4 to disable IER, then send 5 releases */
    HSEM_TEST_SHARED->go_signal = 11U;
    __DMB();

    while (HSEM_TEST_SHARED->cm4_done != 11U)
    {
        __NOP();
    }

    for (uint32_t r = 0; r < 5U; r++)
    {
        while (HSEM_BM_FastTake(HSEM_TEST_ID_NOTIFY) != HSEM_BM_OK) { __NOP(); }
        HSEM_BM_Release(HSEM_TEST_ID_NOTIFY, 0U);
        for (volatile uint32_t d = 0U; d < 1000U; d++) { __NOP(); }
    }

    HSEM_TEST_SHARED->go_signal = 12U;
    __DMB();

    /* Wait for CM4 to check Neg1 and Neg2 */
    while (HSEM_TEST_SHARED->cm4_done != 12U)
    {
        __NOP();
    }

    APP_LOG("[m7_hsem_test] H9 negative control 1 (IER disabled): CM4 IRQs=%lu (%s)",
            HSEM_TEST_SHARED->h9_neg1_irqs,
            (HSEM_TEST_SHARED->h9_neg1_irqs == 0U) ? "PASS: 0 IRQs" : "FAIL: unexpected IRQ");
    APP_LOG("[m7_hsem_test] H9 negative control 2 (stale flag on enable): CM4 spurious IRQs=%lu (%s)",
            HSEM_TEST_SHARED->h9_neg2_irqs,
            (HSEM_TEST_SHARED->h9_neg2_irqs == 0U) ? "PASS: 0 spurious" : "FAIL: spurious IRQ fired");

    if ((HSEM_TEST_SHARED->h9_neg1_irqs != 0U) || (HSEM_TEST_SHARED->h9_neg2_irqs != 0U))
    {
        h9_ok = false;
    }

    /* Active 500 notifications CM7 -> CM4 */
    HSEM_TEST_SHARED->h9_m4_notifications = 0U;
    HSEM_TEST_SHARED->go_signal = 13U;
    __DMB();

    uint32_t releases_sent = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H9_COUNT; i++)
    {
        while (HSEM_BM_FastTake(HSEM_TEST_ID_NOTIFY) != HSEM_BM_OK)
        {
            __NOP();
        }
        HSEM_BM_Release(HSEM_TEST_ID_NOTIFY, 0U);
        releases_sent++;

        /* Bounded timeout wait for CM4 to receive and acknowledge IRQ */
        uint32_t wait_ack = 0U;
        while ((HSEM_TEST_SHARED->h9_m4_notifications < releases_sent) && (wait_ack < 5000000U))
        {
            wait_ack++;
        }
        if (wait_ack >= 5000000U)
        {
            APP_LOG("[m7_hsem_test] H9 timeout waiting for CM4 IRQ on transfer %lu", releases_sent);
            h9_ok = false;
            break;
        }
    }

    while (HSEM_TEST_SHARED->cm4_done != 13U)
    {
        __NOP();
    }

    APP_LOG("[m7_hsem_test] H9 CM7->CM4 notifications: sent=%lu, cm4_irqs=%lu (%s)",
            releases_sent, HSEM_TEST_SHARED->h9_m4_notifications,
            (releases_sent == HSEM_TEST_H9_COUNT && HSEM_TEST_SHARED->h9_m4_notifications == HSEM_TEST_H9_COUNT) ? "PASS" : "FAIL");

    /* Reverse Active 100 notifications CM4 -> CM7 via HAL (B3) */
    s_m7_irq_count = 0U;
    HSEM_TEST_SHARED->h9_m7_notifications = 0U;

    HSEM_HAL_RegisterCallback(M7_HalCallback, NULL);
    HSEM_HAL_NotifyEnable(1UL << HSEM_TEST_ID_NOTIFY);

    NVIC_ClearPendingIRQ(HSEM1_IRQn);
    NVIC_SetPriority(HSEM1_IRQn, 5);
    NVIC_EnableIRQ(HSEM1_IRQn);

    HSEM_TEST_SHARED->go_signal = 14U;
    __DMB();

    uint32_t wait_m4_xfer = 0U;
    while ((HSEM_TEST_SHARED->cm4_done != 14U) && (wait_m4_xfer < 10000000U))
    {
        HSEM_TEST_SHARED->h9_m7_notifications = s_m7_irq_count;
        wait_m4_xfer++;
    }
    HSEM_TEST_SHARED->h9_m7_notifications = s_m7_irq_count;

    HSEM_HAL_NotifyDisable(1UL << HSEM_TEST_ID_NOTIFY);
    NVIC_DisableIRQ(HSEM1_IRQn);
    NVIC_ClearPendingIRQ(HSEM1_IRQn);

    APP_LOG("[m7_hsem_test] H9 CM4->CM7 HAL notifications: cm7_irqs=%lu/100 (%s)",
            s_m7_irq_count, (s_m7_irq_count == 100U) ? "PASS" : "FAIL");

    if (s_m7_irq_count != 100U)
    {
        h9_ok = false;
    }
    HSEM_TEST_SHARED->h9_m7_pass = h9_ok ? 1U : 0U;

    /* ---- H5: Boot Handshake ---- */
    bool h5_ok = (HSEM_TEST_SHARED->h5_pass != 0U);
    APP_LOG("[m7_hsem_test] H5 Boot Handshake: delay500ms=%lu h5b_pre_magic=%lu h5c_spurious_exti=%lu (woke=%lu reparked=%lu) release=%lu -> %s",
            HSEM_TEST_SHARED->h5_ok,
            HSEM_TEST_SHARED->h5b_ok,
            HSEM_TEST_SHARED->h5c_ok,
            HSEM_TEST_SHARED->h5c_woke,
            HSEM_TEST_SHARED->h5c_reparked,
            HSEM_TEST_SHARED->h5_rel_ok,
            h5_ok ? "PASS" : "FAIL");

    /* ---- Final Conditional Verdict (B4) ---- */
    bool all_tests_passed = h0_ok && h3_ok && h7_ok && h10_all_ok &&
                            h2_ok && h1_ok && h1_2step_ok && h1b_ok &&
                            h4_ok && h4_sym_ok && h8_ok && h9_ok && h5_ok;

    HSEM_TEST_SHARED->all_tests_passed = all_tests_passed ? 1U : 0U;

    APP_LOG("[m7_hsem_test] =================================================");
    if (all_tests_passed)
    {
        APP_LOG("[m7_hsem_test] VERDICT: ALL HSEM SELF-TESTS PASSED (PASS)");
    }
    else
    {
        APP_LOG("[m7_hsem_test] VERDICT: HSEM SELF-TESTS FAILED (FAIL)");
    }
    APP_LOG("[m7_hsem_test] =================================================");
}
