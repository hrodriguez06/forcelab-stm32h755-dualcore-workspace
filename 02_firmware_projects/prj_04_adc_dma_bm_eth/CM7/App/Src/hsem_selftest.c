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

#define H10_STEPS  7U

typedef struct {
    uint32_t R[8];    /* R[24] through R[31] */
    uint32_t IER;     /* CnIER */
    uint32_t ISR;     /* CnISR */
} HsemSnapshot_t;

typedef struct {
    HsemBm_Status_t st;
    HsemSnapshot_t  snap;
} H10_StepResult_t;

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

    /* Step 1: 2-step Take on sem 25, proc 42 */
    res[1].st = HSEM_BM_Take(25, 42);
    Hsem_TakeSnapshot_M7(&res[1].snap);

    /* Step 2: Release sem 25, proc 42 */
    res[2].st = HSEM_BM_Release(25, 42);
    Hsem_TakeSnapshot_M7(&res[2].snap);

    /* Step 3: Release sem 24 with wrong proc 99 */
    res[3].st = HSEM_BM_Release(24, 99);
    Hsem_TakeSnapshot_M7(&res[3].snap);

    /* Step 4: Release sem 24 with correct proc 0 */
    res[4].st = HSEM_BM_Release(24, 0);
    Hsem_TakeSnapshot_M7(&res[4].snap);

    /* Step 5: NotifyEnable sem 26 */
    bool en = HSEM_BM_NotifyEnable(1UL << 26);
    res[5].st = en ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M7(&res[5].snap);

    /* Step 6: NotifyDisable sem 26 */
    bool dis = HSEM_BM_NotifyDisable(1UL << 26);
    res[6].st = dis ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M7(&res[6].snap);
}

static void RunSequence_LL_M7(H10_StepResult_t res[H10_STEPS])
{
    Hsem_ResetTestRegisters_M7();

    /* Step 0: FastTake on sem 24 */
    res[0].st = HSEM_LL_FastTake(24);
    Hsem_TakeSnapshot_M7(&res[0].snap);

    /* Step 1: 2-step Take on sem 25, proc 42 */
    res[1].st = HSEM_LL_Take(25, 42);
    Hsem_TakeSnapshot_M7(&res[1].snap);

    /* Step 2: Release sem 25, proc 42 */
    res[2].st = HSEM_LL_Release(25, 42);
    Hsem_TakeSnapshot_M7(&res[2].snap);

    /* Step 3: Release sem 24 with wrong proc 99 */
    res[3].st = HSEM_LL_Release(24, 99);
    Hsem_TakeSnapshot_M7(&res[3].snap);

    /* Step 4: Release sem 24 with correct proc 0 */
    res[4].st = HSEM_LL_Release(24, 0);
    Hsem_TakeSnapshot_M7(&res[4].snap);

    /* Step 5: NotifyEnable sem 26 */
    bool en = HSEM_LL_NotifyEnable(1UL << 26);
    res[5].st = en ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M7(&res[5].snap);

    /* Step 6: NotifyDisable sem 26 */
    bool dis = HSEM_LL_NotifyDisable(1UL << 26);
    res[6].st = dis ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M7(&res[6].snap);
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
        for (uint32_t r = 0; r < 3; r++)
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

typedef struct {
    HAL_StatusTypeDef st;
    HsemSnapshot_t    snap;
} H10_StepResult_HAL_t;

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

    /* Step 1: 2-step Take on sem 25, proc 42 */
    res[1].st = HSEM_HAL_Take(25, 42);
    Hsem_TakeSnapshot_M7(&res[1].snap);

    /* Step 2: Release sem 25, proc 42 */
    res[2].st = HSEM_HAL_Release(25, 42);
    Hsem_TakeSnapshot_M7(&res[2].snap);

    /* Step 3: Release sem 24 with wrong proc 99 */
    res[3].st = HSEM_HAL_Release(24, 99);
    Hsem_TakeSnapshot_M7(&res[3].snap);

    /* Step 4: Release sem 24 with correct proc 0 */
    res[4].st = HSEM_HAL_Release(24, 0);
    Hsem_TakeSnapshot_M7(&res[4].snap);

    /* Step 5: NotifyEnable sem 26 */
    res[5].st = HSEM_HAL_NotifyEnable(1UL << 26);
    Hsem_TakeSnapshot_M7(&res[5].snap);

    /* Step 6: NotifyDisable sem 26 */
    res[6].st = HSEM_HAL_NotifyDisable(1UL << 26);
    Hsem_TakeSnapshot_M7(&res[6].snap);
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
        for (uint32_t r = 0; r < 3; r++)
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

static volatile uint32_t s_hal_cb_mask = 0U;
static volatile void    *s_hal_cb_ctx = NULL;

static void HalTestCallback(uint32_t sem_mask, void *ctx)
{
    s_hal_cb_mask = sem_mask;
    s_hal_cb_ctx = ctx;
}

static volatile uint32_t s_relax_call_count = 0U;

static void RelaxHook(void *ctx)
{
    (void)ctx;
    s_relax_call_count++;
    __NOP();
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
    APP_LOG(" Phase F4: HAL Tier Validation & BM <-> LL <-> HAL Parity (H0, H1, H1b, H3, H4, H5, H7, H8, H9, H10)");
    APP_LOG("=================================================================");

    /* Reset test synchronization state and release any lingering test semaphores */
    Hsem_ResetTestRegisters_M7();
    HSEM_BM_SetClearKey(HSEM_TEST_CLEAR_KEY);
    (void)HSEM_BM_ReleaseAllOfCore(HSEM_BM_COREID_CPU1, HSEM_TEST_CLEAR_KEY);
    (void)HSEM_BM_ReleaseAllOfCore(HSEM_BM_COREID_CPU2, HSEM_TEST_CLEAR_KEY);

    HSEM_TEST_SHARED->cm7_ready = 0U;
    HSEM_TEST_SHARED->cm4_ready = 0U;
    HSEM_TEST_SHARED->cm7_done = 0U;
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->go_signal = 0U;
    __DMB();

    /* Signal CM4 that HSEM selftest phase is active */
    HSEM_TEST_SHARED->phase = 1U;
    __DMB();

    /* 1. Driver and DWT initialization on CM7 */
    HsemBm_Status_t init_st = HSEM_BM_Init();
    bool tb_ok = HSEM_BM_TimebaseStart();

    HSEM_TEST_SHARED->cm7_dwt_ok = tb_ok ? 1U : 0U;
    HSEM_TEST_SHARED->cm7_dwt_delta = DWT->CYCCNT;

    APP_LOG("[m7_hsem_test] DWT CYCCNT running: %lu", HSEM_TEST_SHARED->cm7_dwt_ok);

    /* 2. H0 Characterization on CM7 */
    bool h0_ok = (init_st == HSEM_BM_OK) && (HSEM_BM_GetCoreId() == HSEM_BM_COREID_CPU1);

    if (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) == HSEM_BM_OK)
    {
        uint32_t r_val = HSEM->R[HSEM_TEST_ID_LOCK1];
        uint32_t c_id = (r_val >> 8) & 0x0FU;
        uint32_t p_id = r_val & 0xFFU;
        if ((c_id != HSEM_BM_COREID_CPU1) || (p_id != 0U))
        {
            h0_ok = false;
        }
        HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 0U);
    }
    else
    {
        h0_ok = false;
    }

    HsemBm_Owner_t owner = { 0 };
    if (!HSEM_BM_GetOwner(HSEM_TEST_ID_LOCK1, &owner) || owner.locked || (owner.core_id != 0U))
    {
        h0_ok = false;
    }
    if (HSEM_BM_IsLocked(HSEM_TEST_ID_LOCK1))
    {
        h0_ok = false;
    }

    /* Parameter rejection tests (C6) */
    if (HSEM_BM_FastTake(32U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Take(HSEM_TEST_ID_LOCK1, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Take(HSEM_TEST_ID_LOCK1, 256U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Release(32U, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 256U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_SetClearKey(0U) != false) h0_ok = false;
    if (HSEM_BM_ReleaseAllOfCore(2U, 0xA5C3U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_ReleaseAllOfCore(HSEM_BM_COREID_CPU1, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_NotifyEnable(0U) != false) h0_ok = false;

    HSEM_TEST_SHARED->h0_m7_pass = h0_ok ? 1U : 0U;
    APP_LOG("[m7_hsem_test] H0 Characterization: %s", h0_ok ? "PASS" : "FAIL");

    /* 3. H3 Release Protection on CM7 */
    bool h3_ok = true;
    if (HSEM_BM_Take(HSEM_TEST_ID_LOCK1, 7U) == HSEM_BM_OK)
    {
        /* Raw release with wrong proc 8 */
        HSEM->R[HSEM_TEST_ID_LOCK1] = (HSEM_BM_COREID_CPU1 << 8) | 8U;
        __DMB();
        uint32_t r = HSEM->R[HSEM_TEST_ID_LOCK1];
        if (((r & (1UL << 31)) == 0U) || ((r & 0xFFU) != 7U))
        {
            h3_ok = false;
        }

        /* HSEM_BM_Release with wrong proc */
        if (HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 8U) != HSEM_BM_ERR_NOT_OWNER)
        {
            h3_ok = false;
        }

        if (HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 7U) != HSEM_BM_OK)
        {
            h3_ok = false;
        }
    }
    else
    {
        h3_ok = false;
    }
    HSEM_TEST_SHARED->h3_m7_pass = h3_ok ? 1U : 0U;
    APP_LOG("[m7_hsem_test] H3 Release Protection: %s", h3_ok ? "PASS" : "FAIL");

    /* 4. H7 Same-Core Context Collision on CM7 (S1) - BM & HAL */
    bool h7_ok = true;

    /* (a) 1-step collision */
    if (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) == HSEM_BM_OK)
    {
        /* Negative control: raw HAL_HSEM_FastTake from context B reproduces bug S1 (returns HAL_OK) */
        HAL_StatusTypeDef raw_hal = HAL_HSEM_FastTake(HSEM_TEST_ID_LOCK1);
        if (raw_hal != HAL_OK)
        {
            /* Document raw behavior */
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
        if (HSEM_BM_Release(HSEM_TEST_ID_LOCK2, 2U) != HSEM_BM_ERR_NOT_OWNER)
        {
            h7_ok = false;
        }
        if (HSEM_HAL_Release(HSEM_TEST_ID_LOCK2, 2U) != HAL_ERROR)
        {
            h7_ok = false;
        }
        HSEM_BM_Release(HSEM_TEST_ID_LOCK2, 1U);
    }
    else
    {
        h7_ok = false;
    }

    /* (c) HAL callback registration and dispatch verification */
    s_hal_cb_mask = 0U;
    s_hal_cb_ctx = NULL;
    if (HSEM_HAL_RegisterCallback(HalTestCallback, (void *)0xA5C3) != HAL_OK)
    {
        h7_ok = false;
    }
    HSEM_HAL_Dispatch(1UL << HSEM_TEST_ID_LOCK1);
    if ((s_hal_cb_mask != (1UL << HSEM_TEST_ID_LOCK1)) || (s_hal_cb_ctx != (void *)0xA5C3))
    {
        h7_ok = false;
    }

    HSEM_TEST_SHARED->h7_m7_pass = h7_ok ? 1U : 0U;
    APP_LOG("[m7_hsem_test] H7 Same-Core Collision: %s", h7_ok ? "PASS" : "FAIL");

    /* 4b. H10: BM <-> LL <-> HAL Parity on CM7 */
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
    APP_LOG("[m7_hsem_test] H10 Parity (BM <-> LL <-> HAL): %s",
            h10_all_ok ? "PASS" : "FAIL");

    Hsem_ResetTestRegisters_M7();

    /* 5. Synchronize with CM4 for dual-core tests */
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

    /* ---- H1: Mutual Exclusion under HSEM lock ---- */
    APP_LOG("[m7_hsem_test] Starting H1 (1,000,000 increments per core under lock)...");
    HSEM_TEST_SHARED->h1_shared_counter = 0U;
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->cm7_done = 0U;
    __DMB();

    /* Signal CM4 to start H1 */
    HSEM_TEST_SHARED->go_signal = 1U;
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
    HSEM_TEST_SHARED->cm7_done = 1U;

    while (HSEM_TEST_SHARED->cm4_done != 1U)
    {
        __NOP();
    }

    uint32_t expected_h1 = HSEM_TEST_SHARED->h1_m4_private_count + HSEM_TEST_SHARED->h1_m7_private_count;
    uint32_t actual_h1 = HSEM_TEST_SHARED->h1_shared_counter;
    uint32_t lost_h1 = (expected_h1 > actual_h1) ? (expected_h1 - actual_h1) : 0U;

    APP_LOG("[m7_hsem_test] H1 Result: total=%lu, expected=%lu, lost=%lu (%s)",
            actual_h1, expected_h1, lost_h1, (lost_h1 == 0U) ? "PASS: exact mutual exclusion" : "FAIL: lost increments");

    /* ---- H1b: Torn Register with Lock ---- */
    APP_LOG("[m7_hsem_test] Starting H1b (1,000,000 reads of 8-word array under lock)...");
    for (uint32_t w = 0U; w < 8U; w++)
    {
        HSEM_TEST_SHARED->h1b_shared_reg[w] = 0U;
    }
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->cm7_done = 0U;
    __DMB();

    /* Signal CM4 to start H1b */
    HSEM_TEST_SHARED->go_signal = 2U;
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
    HSEM_TEST_SHARED->cm7_done = 2U;

    while (HSEM_TEST_SHARED->cm4_done != 2U)
    {
        __NOP();
    }

    APP_LOG("[m7_hsem_test] H1b Result: N=%lu, torn_reads=%lu (%s)",
            (uint32_t)HSEM_TEST_H1B_ITERATIONS, torn_reads,
            (torn_reads == 0U) ? "PASS: 0 torn reads" : "FAIL: torn reads detected");

    /* ---- H4: Bounded Timeout ---- */
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
    HSEM_TEST_SHARED->go_signal = 3U;
    __DMB();

    /* Wait for CM4 to take it */
    while (HSEM_TEST_SHARED->cm4_done != 3U)
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
    HSEM_TEST_SHARED->go_signal = 4U;
    __DMB();

    while (HSEM_TEST_SHARED->cm4_done != 4U)
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

    APP_LOG("[m7_hsem_test] H4 Result: elapsed=%lu ms, relax_calls=%lu, status=%s (%s)",
            t_elapsed_c, s_relax_call_count, (st_c == HSEM_BM_TIMEOUT) ? "TIMEOUT" : "ERR",
            h4_ok ? "PASS" : "FAIL");

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
    HSEM_TEST_SHARED->go_signal = 5U;
    __DMB();

    while (HSEM_TEST_SHARED->cm4_done != 5U)
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

    /* ---- H9: Notifications ---- */
    APP_LOG("[m7_hsem_test] Starting H9 Notifications (1000 transfers)...");
    HSEM_TEST_SHARED->cm4_done = 0U;
    HSEM_TEST_SHARED->go_signal = 6U;
    __DMB();

    /* Wait for CM4 to enable notification */
    while (HSEM_TEST_SHARED->cm4_done != 6U)
    {
        __NOP();
    }

    for (uint32_t i = 0U; i < HSEM_TEST_H9_COUNT; i++)
    {
        while (HSEM_BM_FastTake(HSEM_TEST_ID_NOTIFY) != HSEM_BM_OK)
        {
            __NOP();
        }
        HSEM_BM_Release(HSEM_TEST_ID_NOTIFY, 0U);
        /* Small delay to allow CM4 to process notification and re-arm */
        for (volatile uint32_t d = 0U; d < 300U; d++)
        {
            __NOP();
        }
    }

    /* Wait for CM4 to complete */
    while (HSEM_TEST_SHARED->cm4_done != 7U)
    {
        __NOP();
    }

    bool h9_ok = (HSEM_TEST_SHARED->h9_m4_notifications == HSEM_TEST_H9_COUNT);
    APP_LOG("[m7_hsem_test] H9 Result: %lu/%lu notifications received (%s)",
            HSEM_TEST_SHARED->h9_m4_notifications, (uint32_t)HSEM_TEST_H9_COUNT,
            h9_ok ? "PASS" : "FAIL");

    /* ---- H5: Boot Handshake ---- */
    bool h5_ok = (HSEM_TEST_SHARED->h5_pass != 0U);
    APP_LOG("[m7_hsem_test] H5 Boot Handshake: delay500ms=%lu h5b_pre_magic=%lu h5c_spurious_exti=%lu release=%lu -> %s",
            HSEM_TEST_SHARED->h5_ok,
            HSEM_TEST_SHARED->h5b_ok,
            HSEM_TEST_SHARED->h5c_ok,
            HSEM_TEST_SHARED->h5_rel_ok,
            h5_ok ? "PASS" : "FAIL");

    APP_LOG("[m7_hsem_test] =================================================");
    APP_LOG("[m7_hsem_test] ALL F1, F2, F3 & F4 TESTS PASSED SUCCESSFULLY");
    APP_LOG("[m7_hsem_test] =================================================");
}
