/**
  ******************************************************************************
  * @file    hsem_selftest.c
  * @brief   HSEM driver self-test implementation for Cortex-M4 (F1 Selftest).
  ******************************************************************************
  */

#include "hsem_selftest.h"
#include "hsem_selftest_shared.h"
#include "hsem_bm.h"
#include "hsem_ll.h"
#include "board_def.h"
#include "shared_memory.h"
#include "stm32h7xx.h"
#include "stm32h7xx_hal_hsem.h"
#include <string.h>
#include <stdbool.h>

#define ST_MAX_LINES 30
static char s_lines[ST_MAX_LINES][SHARED_MBOX_SIZE];
static uint32_t s_line_count = 0U;
static uint32_t s_line_len = 0U;
static uint32_t s_line_next = 0U;

static void Line_Begin(void)
{
    s_line_len = 0U;
    if (s_line_count < ST_MAX_LINES)
    {
        s_lines[s_line_count][0] = '\0';
    }
}

static void Line_End(void)
{
    if (s_line_count < ST_MAX_LINES)
    {
        s_line_count++;
    }
}

static void Line_Str(const char *s)
{
    if (s_line_count >= ST_MAX_LINES) return;
    char *dst = s_lines[s_line_count];
    while ((*s != '\0') && (s_line_len < (SHARED_MBOX_SIZE - 1U)))
    {
        dst[s_line_len++] = *s++;
    }
    dst[s_line_len] = '\0';
}

static void Line_U32(uint32_t v)
{
    char tmp[11];
    uint32_t n = 0U;
    do
    {
        tmp[n++] = (char)('0' + (int)(v % 10U));
        v /= 10U;
    } while ((v != 0U) && (n < sizeof(tmp) - 1U));

    while (n > 0U)
    {
        char c = tmp[--n];
        if (s_line_count < ST_MAX_LINES && s_line_len < (SHARED_MBOX_SIZE - 1U))
        {
            s_lines[s_line_count][s_line_len++] = c;
            s_lines[s_line_count][s_line_len] = '\0';
        }
    }
}

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

static void Hsem_TakeSnapshot_M4(HsemSnapshot_t *s)
{
    for (uint32_t i = 0; i < 8; i++)
    {
        s->R[i] = HSEM->R[24 + i];
    }
    s->IER = HSEM->C2IER;
    s->ISR = HSEM->C2ISR;
}

static void Hsem_ResetTestRegisters_M4(void)
{
    (void)HSEM_BM_Release(28, 0);
    (void)HSEM_BM_Release(28, 99);
    (void)HSEM_BM_Release(29, 42);
    (void)HSEM_BM_Release(29, 43);
    (void)HSEM_BM_Release(30, 0);
    HSEM->C2IER = 0U;
    HSEM->C2ICR = 0xFFFFFFFFUL;
    __DSB();
    __ISB();
}

static void RunSequence_BM_M4(H10_StepResult_t res[H10_STEPS])
{
    Hsem_ResetTestRegisters_M4();

    /* Step 0: FastTake on sem 28 */
    res[0].st = HSEM_BM_FastTake(28);
    Hsem_TakeSnapshot_M4(&res[0].snap);

    /* Step 1: Re-entrant FastTake on sem 28 -> BUSY_SELF */
    res[1].st = HSEM_BM_FastTake(28);
    Hsem_TakeSnapshot_M4(&res[1].snap);

    /* Step 2: Re-entrant TakeTimeoutEx on sem 28 -> BUSY_SELF (M1) */
    res[2].st = HSEM_BM_TakeTimeoutEx(28, 0, 1000U, NULL, NULL);
    Hsem_TakeSnapshot_M4(&res[2].snap);

    /* Step 3: 2-step Take on sem 29, proc 42 */
    res[3].st = HSEM_BM_Take(29, 42);
    Hsem_TakeSnapshot_M4(&res[3].snap);

    /* Step 4: Re-entrant Take on sem 29, proc 43 -> BUSY_SELF */
    res[4].st = HSEM_BM_Take(29, 43);
    Hsem_TakeSnapshot_M4(&res[4].snap);

    /* Step 5: Release sem 29 with wrong proc 99 -> ERR_NOT_OWNER */
    res[5].st = HSEM_BM_Release(29, 99);
    Hsem_TakeSnapshot_M4(&res[5].snap);

    /* Step 6: Release sem 29 with correct proc 42 -> OK */
    res[6].st = HSEM_BM_Release(29, 42);
    Hsem_TakeSnapshot_M4(&res[6].snap);

    /* Step 7: Release sem 28 with correct proc 0 -> OK */
    res[7].st = HSEM_BM_Release(28, 0);
    Hsem_TakeSnapshot_M4(&res[7].snap);

    /* Step 8: Parameter error FastTake sem 32 -> ERR_PARAM */
    res[8].st = HSEM_BM_FastTake(32);
    Hsem_TakeSnapshot_M4(&res[8].snap);

    /* Step 9: Parameter error Take proc 256 -> ERR_PARAM */
    res[9].st = HSEM_BM_Take(28, 256);
    Hsem_TakeSnapshot_M4(&res[9].snap);

    /* Step 10: Parameter error Release sem 32 -> ERR_PARAM */
    res[10].st = HSEM_BM_Release(32, 0);
    Hsem_TakeSnapshot_M4(&res[10].snap);

    /* Step 11: NotifyEnable sem 30 */
    bool en = HSEM_BM_NotifyEnable(1UL << 30);
    res[11].st = en ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M4(&res[11].snap);

    /* Step 12: NotifyDisable sem 30 */
    bool dis = HSEM_BM_NotifyDisable(1UL << 30);
    res[12].st = dis ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M4(&res[12].snap);
}

static void RunSequence_LL_M4(H10_StepResult_t res[H10_STEPS])
{
    Hsem_ResetTestRegisters_M4();

    /* Step 0: FastTake on sem 28 */
    res[0].st = (HsemBm_Status_t)HSEM_LL_FastTake(28);
    Hsem_TakeSnapshot_M4(&res[0].snap);

    /* Step 1: Re-entrant FastTake on sem 28 -> BUSY_SELF */
    res[1].st = (HsemBm_Status_t)HSEM_LL_FastTake(28);
    Hsem_TakeSnapshot_M4(&res[1].snap);

    /* Step 2: Re-entrant TakeTimeoutEx on sem 28 -> BUSY_SELF (M1) */
    res[2].st = (HsemBm_Status_t)HSEM_LL_TakeTimeoutEx(28, 0, 1000U, NULL, NULL);
    Hsem_TakeSnapshot_M4(&res[2].snap);

    /* Step 3: 2-step Take on sem 29, proc 42 */
    res[3].st = (HsemBm_Status_t)HSEM_LL_Take(29, 42);
    Hsem_TakeSnapshot_M4(&res[3].snap);

    /* Step 4: Re-entrant Take on sem 29, proc 43 -> BUSY_SELF */
    res[4].st = (HsemBm_Status_t)HSEM_LL_Take(29, 43);
    Hsem_TakeSnapshot_M4(&res[4].snap);

    /* Step 5: Release sem 29 with wrong proc 99 -> ERR_NOT_OWNER */
    res[5].st = (HsemBm_Status_t)HSEM_LL_Release(29, 99);
    Hsem_TakeSnapshot_M4(&res[5].snap);

    /* Step 6: Release sem 29 with correct proc 42 -> OK */
    res[6].st = (HsemBm_Status_t)HSEM_LL_Release(29, 42);
    Hsem_TakeSnapshot_M4(&res[6].snap);

    /* Step 7: Release sem 28 with correct proc 0 -> OK */
    res[7].st = (HsemBm_Status_t)HSEM_LL_Release(28, 0);
    Hsem_TakeSnapshot_M4(&res[7].snap);

    /* Step 8: Parameter error FastTake sem 32 -> ERR_PARAM */
    res[8].st = (HsemBm_Status_t)HSEM_LL_FastTake(32);
    Hsem_TakeSnapshot_M4(&res[8].snap);

    /* Step 9: Parameter error Take proc 256 -> ERR_PARAM */
    res[9].st = (HsemBm_Status_t)HSEM_LL_Take(28, 256);
    Hsem_TakeSnapshot_M4(&res[9].snap);

    /* Step 10: Parameter error Release sem 32 -> ERR_PARAM */
    res[10].st = (HsemBm_Status_t)HSEM_LL_Release(32, 0);
    Hsem_TakeSnapshot_M4(&res[10].snap);

    /* Step 11: NotifyEnable sem 30 */
    bool en = HSEM_LL_NotifyEnable(1UL << 30);
    res[11].st = en ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M4(&res[11].snap);

    /* Step 12: NotifyDisable sem 30 */
    bool dis = HSEM_LL_NotifyDisable(1UL << 30);
    res[12].st = dis ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M4(&res[12].snap);
}

static bool CompareSnapshots_M4(const H10_StepResult_t bm[H10_STEPS],
                               const H10_StepResult_t ll[H10_STEPS])
{
    for (uint32_t i = 0; i < H10_STEPS; i++)
    {
        if (bm[i].st != ll[i].st)
        {
            return false;
        }
        for (uint32_t r = 0; r < 8; r++)
        {
            if (bm[i].snap.R[r] != ll[i].snap.R[r])
            {
                return false;
            }
        }
        if (bm[i].snap.IER != ll[i].snap.IER)
        {
            return false;
        }
        if (bm[i].snap.ISR != ll[i].snap.ISR)
        {
            return false;
        }
    }
    return true;
}

/* NVIC ISR state on CM4 */
static volatile uint32_t s_m4_irq_count = 0U;

void HSEM_SelfTest_CM4_IRQHandler(void)
{
    uint32_t mask = HSEM_BM_NotifyTake();
    if ((mask & (1UL << HSEM_TEST_ID_NOTIFY)) != 0U)
    {
        s_m4_irq_count++;
    }
}

static volatile uint32_t s_m4_relax_call_count = 0U;
static void M4_RelaxHook(void *ctx)
{
    (void)ctx;
    s_m4_relax_call_count++;
}

void HSEM_SelfTest_CM4_Run(void)
{
    /* 1. Ensure HSEM clock is enabled on CM4 */
    __HAL_RCC_HSEM_CLK_ENABLE();
    (void)RCC->AHB4ENR; /* Readback */

    /* 2. DWT CYCCNT initialization on CM4 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    uint32_t t0 = DWT->CYCCNT;
    for (volatile uint32_t k = 0; k < 500U; k++)
    {
        __NOP();
    }
    uint32_t t1 = DWT->CYCCNT;
    uint32_t delta = t1 - t0;

    HSEM_TEST_SHARED->cm4_dwt_delta = delta;
    HSEM_TEST_SHARED->cm4_dwt_ok = (delta > 0U) ? 1U : 0U;

    /* 3. B1 Raw Timing Baseline on CM4 (using ID 29) */
    HSEM->R[29] = (HSEM_BM_COREID_CPU2 << 8);
    __DMB();

    uint32_t c_start = DWT->CYCCNT;
    volatile uint32_t rlr_val = HSEM->RLR[29];
    uint32_t fast_take_cyc = DWT->CYCCNT - c_start;
    (void)rlr_val;

    c_start = DWT->CYCCNT;
    HSEM->R[29] = (HSEM_BM_COREID_CPU2 << 8);
    uint32_t rel_cyc = DWT->CYCCNT - c_start;
    __DMB();

    c_start = DWT->CYCCNT;
    HSEM->R[29] = (1U << 31) | (HSEM_BM_COREID_CPU2 << 8) | 7U;
    volatile uint32_t r_read = HSEM->R[29];
    uint32_t take_cyc = DWT->CYCCNT - c_start;
    (void)r_read;

    HSEM->R[29] = (HSEM_BM_COREID_CPU2 << 8) | 7U;
    __DMB();

    HSEM_TEST_SHARED->cm4_raw_fast_take_cycles = fast_take_cyc;
    HSEM_TEST_SHARED->cm4_raw_take_cycles = take_cyc;
    HSEM_TEST_SHARED->cm4_raw_release_cycles = rel_cyc;

    /* 4. H0 Characterization on CM4 */
    bool h0_ok = true;
    (void)HSEM_BM_Release(HSEM_TEST_M4_LOCK1, 0U);
    uint32_t rlr = HSEM->RLR[HSEM_TEST_M4_LOCK1];
    uint32_t expected_rlr = (1UL << 31) | (HSEM_BM_COREID_CPU2 << 8);
    if (rlr == expected_rlr)
    {
        uint32_t r = HSEM->R[HSEM_TEST_M4_LOCK1];
        if ((((r >> 8) & 0xFFU) != HSEM_BM_COREID_CPU2) || (HSEM_BM_Release(HSEM_TEST_M4_LOCK1, 0U) != HSEM_BM_OK))
        {
            h0_ok = false;
        }
    }
    else
    {
        h0_ok = false;
        (void)HSEM_BM_Release(HSEM_TEST_M4_LOCK1, 0U);
    }

    HsemBm_Owner_t owner = { 0 };
    if (!HSEM_BM_GetOwner(HSEM_TEST_M4_LOCK1, &owner) || owner.locked || (owner.core_id != 0U))
    {
        h0_ok = false;
    }
    if (HSEM_BM_IsLocked(HSEM_TEST_M4_LOCK1))
    {
        h0_ok = false;
    }

    /* Parameter rejection tests (C6) */
    if (HSEM_BM_FastTake(32U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Take(32U, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Take(HSEM_TEST_M4_LOCK1, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Take(HSEM_TEST_M4_LOCK1, 256U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Release(32U, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_Release(HSEM_TEST_M4_LOCK1, 256U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_SetClearKey(0U) != false) h0_ok = false;
    if (HSEM_BM_ReleaseAllOfCore(2U, 0xA5C3U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_ReleaseAllOfCore(HSEM_BM_COREID_CPU2, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_NotifyEnable(0U) != false) h0_ok = false;

    HSEM_TEST_SHARED->h0_m4_pass = h0_ok ? 1U : 0U;

    /* 5. H3 Release Protection on CM4 */
    bool h3_ok = true;
    if (HSEM_BM_Take(HSEM_TEST_M4_LOCK1, 7U) == HSEM_BM_OK)
    {
        /* Wrong proc release */
        if (HSEM_BM_Release(HSEM_TEST_M4_LOCK1, 8U) != HSEM_BM_ERR_NOT_OWNER)
        {
            h3_ok = false;
        }
        /* Correct proc release */
        if (HSEM_BM_Release(HSEM_TEST_M4_LOCK1, 7U) != HSEM_BM_OK)
        {
            h3_ok = false;
        }
    }
    else
    {
        h3_ok = false;
    }

    /* Case B: Attempt release of CM7-held lock HSEM_TEST_M7_LOCK1 */
    /* Signal CM7 that CM4 is ready for Case B (phase == 1) */
    HSEM_TEST_SHARED->phase = 1U;
    __DMB();

    /* Wait for CM7 to hold the lock (phase == 2) */
    uint32_t w_cm7 = 0U;
    while ((HSEM_TEST_SHARED->phase != 2U) && (w_cm7++ < 20000000U))
    {
        __NOP();
    }

    if (HSEM_TEST_SHARED->phase == 2U)
    {
        /* Attempt to release CM7's lock from CM4 */
        if (HSEM_BM_Release(HSEM_TEST_M7_LOCK1, 0U) != HSEM_BM_ERR_NOT_OWNER)
        {
            h3_ok = false;
        }
        HSEM_TEST_SHARED->phase = 3U;
        __DMB();

        /* Wait for CM7 to finish and release (phase == 4) */
        uint32_t w_fin = 0U;
        while ((HSEM_TEST_SHARED->phase != 4U) && (w_fin++ < 10000000U))
        {
            __NOP();
        }
    }
    else
    {
        h3_ok = false;
    }

    HSEM_TEST_SHARED->h3_m4_pass = h3_ok ? 1U : 0U;

    /* 6. H7 Same-Core Context Collision on CM4 (S1) */
    bool h7_ok = true;
    if (HSEM_BM_FastTake(HSEM_TEST_M4_LOCK1) == HSEM_BM_OK)
    {
        /* Negative control: raw HAL_HSEM_FastTake from context B reproduces bug S1 (returns HAL_OK) */
        HAL_StatusTypeDef raw_hal = HAL_HSEM_FastTake(HSEM_TEST_M4_LOCK1);
        HSEM_TEST_SHARED->h7_neg_raw_hal = (uint32_t)raw_hal;
        if (raw_hal != HAL_OK)
        {
            /* Hazard S1 was not reproduced: negative control fails */
            h7_ok = false;
        }

        /* Driver BM detects collision: returns BUSY_SELF */
        if (HSEM_BM_FastTake(HSEM_TEST_M4_LOCK1) != HSEM_BM_BUSY_SELF)
        {
            h7_ok = false;
        }

        HSEM_BM_Release(HSEM_TEST_M4_LOCK1, 0U);
    }
    else
    {
        h7_ok = false;
    }

    if (HSEM_BM_Take(HSEM_TEST_M4_LOCK2, 1U) == HSEM_BM_OK)
    {
        if (HSEM_BM_Take(HSEM_TEST_M4_LOCK2, 2U) != HSEM_BM_BUSY_SELF)
        {
            h7_ok = false;
        }
        HSEM_BM_Release(HSEM_TEST_M4_LOCK2, 1U);
    }
    else
    {
        h7_ok = false;
    }
    HSEM_TEST_SHARED->h7_m4_pass = h7_ok ? 1U : 0U;

    /* 7. H10 Parity on CM4 (BM vs LL across 14 steps) */
    static H10_StepResult_t bm_res[H10_STEPS];
    static H10_StepResult_t ll_res[H10_STEPS];
    RunSequence_BM_M4(bm_res);
    RunSequence_LL_M4(ll_res);

    bool h10_bm_ll_ok = CompareSnapshots_M4(bm_res, ll_res);

    /* Negative control: mutate snapshot to confirm comparison detects mismatch */
    static H10_StepResult_t mutated_ll[H10_STEPS];
    for (uint32_t i = 0U; i < H10_STEPS; i++)
    {
        mutated_ll[i] = ll_res[i];
    }
    mutated_ll[1].snap.R[1] ^= 0x1U;
    bool neg_detected_ll = !CompareSnapshots_M4(bm_res, mutated_ll);

    bool h10_all_ok = h10_bm_ll_ok && neg_detected_ll;
    HSEM_TEST_SHARED->h10_m4_pass = h10_all_ok ? 1U : 0U;

    Hsem_ResetTestRegisters_M4();

    /* 8. Signal CM4 ready for dual-core tests */
    HSEM_TEST_SHARED->cm4_ready = 1U;
    __DMB();

    /* Wait for CM7 to signal start of H2 Negative Control */
    while (HSEM_TEST_SHARED->go_signal != 1U)
    {
        __NOP();
    }

    /* ---- H2 Part A: Counter RMW without lock ---- */
    uint32_t m4_h2_count = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H2_ITERATIONS; i++)
    {
        uint32_t val = HSEM_TEST_SHARED->h2_shared_counter;
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
        HSEM_TEST_SHARED->h2_shared_counter = val + 1U;
        m4_h2_count++;
    }
    HSEM_TEST_SHARED->h2_m4_count = m4_h2_count;
    __DMB();
    HSEM_TEST_SHARED->cm4_done = 1U;

    /* Wait for CM7 to start H2 Part B */
    while (HSEM_TEST_SHARED->go_signal != 2U)
    {
        __NOP();
    }

    /* ---- H2 Part B: Torn register write without lock ---- */
    for (uint32_t i = 0U; i < HSEM_TEST_H2_TORN_ITERS; i++)
    {
        uint32_t v = i + 1U;
        for (uint32_t w = 0U; w < 8U; w++)
        {
            HSEM_TEST_SHARED->h2_shared_reg[w] = v;
        }
    }
    __DMB();
    HSEM_TEST_SHARED->cm4_done = 2U;

    /* Wait for CM7 to start H1 1-step */
    while (HSEM_TEST_SHARED->go_signal != 3U)
    {
        __NOP();
    }

    /* ---- H1: 1-step FastTake mutual exclusion ---- */
    uint32_t m4_h1_count = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H1_ITERATIONS; i++)
    {
        while (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) != HSEM_BM_OK)
        {
            __NOP();
        }
        HSEM_TEST_SHARED->h1_shared_counter++;
        m4_h1_count++;
        HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 0U);
    }
    HSEM_TEST_SHARED->h1_m4_private_count = m4_h1_count;
    __DMB();
    HSEM_TEST_SHARED->cm4_done = 3U;

    /* Wait for CM7 to start H1 2-step */
    while (HSEM_TEST_SHARED->go_signal != 4U)
    {
        __NOP();
    }

    /* ---- H1: 2-step Take mutual exclusion (M5) ---- */
    uint32_t m4_h1_2step_count = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H1_2STEP_ITERS; i++)
    {
        while (HSEM_BM_Take(HSEM_TEST_ID_LOCK2, 42U) != HSEM_BM_OK)
        {
            __NOP();
        }
        HSEM_TEST_SHARED->h1_2step_shared_counter++;
        m4_h1_2step_count++;
        HSEM_BM_Release(HSEM_TEST_ID_LOCK2, 42U);
    }
    HSEM_TEST_SHARED->h1_2step_m4_private_count = m4_h1_2step_count;
    __DMB();
    HSEM_TEST_SHARED->cm4_done = 4U;

    /* Wait for CM7 to start H1b */
    while (HSEM_TEST_SHARED->go_signal != 5U)
    {
        __NOP();
    }

    /* ---- H1b: Torn Register with Lock ---- */
    for (uint32_t i = 0U; i < HSEM_TEST_H1B_ITERATIONS; i++)
    {
        while (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) != HSEM_BM_OK)
        {
            __NOP();
        }
        uint32_t v = i + 1U;
        for (uint32_t w = 0U; w < 8U; w++)
        {
            HSEM_TEST_SHARED->h1b_shared_reg[w] = v;
        }
        HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 0U);
    }
    __DMB();
    HSEM_TEST_SHARED->cm4_done = 5U;

    /* Wait for CM7 to start H4 */
    while (HSEM_TEST_SHARED->go_signal != 6U)
    {
        __NOP();
    }

    /* ---- H4: Bounded Timeout cooperation ---- */
    /* CM4 takes ID_TIMEOUT and holds it for 5 ms, then releases */
    HSEM_BM_FastTake(HSEM_TEST_ID_TIMEOUT);
    HSEM_TEST_SHARED->cm4_done = 6U;
    __DMB();

    HAL_Delay(5);
    HSEM_BM_Release(HSEM_TEST_ID_TIMEOUT, 0U);

    /* Wait for CM7 step C */
    while (HSEM_TEST_SHARED->go_signal != 7U)
    {
        __NOP();
    }

    /* CM4 takes ID_TIMEOUT and holds it for 25 ms (CM7 times out at 10 ms) */
    HSEM_BM_FastTake(HSEM_TEST_ID_TIMEOUT);
    HSEM_TEST_SHARED->cm4_done = 7U;
    __DMB();

    HAL_Delay(25);
    HSEM_BM_Release(HSEM_TEST_ID_TIMEOUT, 0U);

    /* ---- H4 Symmetric: CM4 waiting on CM7 lock (M5) ---- */
    while (HSEM_TEST_SHARED->go_signal != 8U)
    {
        __NOP();
    }

    bool h4_sym_ok = true;
    /* CM7 holds for 5 ms, CM4 timeout set to 10 ms (2,000,000 cyc @ 200 MHz) */
    uint32_t t_start_m4 = HAL_GetTick();
    HsemBm_Status_t st_sym_b = HSEM_BM_TakeTimeout(HSEM_TEST_ID_TIMEOUT, 0U, 2000000U);
    uint32_t t_elapsed_m4_b = HAL_GetTick() - t_start_m4;
    if ((st_sym_b != HSEM_BM_OK) || (t_elapsed_m4_b > 8U))
    {
        h4_sym_ok = false;
    }
    HSEM_BM_Release(HSEM_TEST_ID_TIMEOUT, 0U);

    HSEM_TEST_SHARED->cm4_done = 8U;
    __DMB();

    /* Wait for CM7 to start 25 ms hold */
    while (HSEM_TEST_SHARED->go_signal != 9U)
    {
        __NOP();
    }

    s_m4_relax_call_count = 0U;
    t_start_m4 = HAL_GetTick();
    HsemBm_Status_t st_sym_c = HSEM_BM_TakeTimeoutEx(HSEM_TEST_ID_TIMEOUT, 0U, 2000000U, M4_RelaxHook, NULL);
    uint32_t t_elapsed_m4_c = HAL_GetTick() - t_start_m4;
    if ((st_sym_c != HSEM_BM_TIMEOUT) || (t_elapsed_m4_c < 10U) || (t_elapsed_m4_c > 12U) || (s_m4_relax_call_count == 0U))
    {
        h4_sym_ok = false;
    }

    HSEM_TEST_SHARED->h4_m4_pass = h4_sym_ok ? 1U : 0U;
    HSEM_TEST_SHARED->h4_symmetric_pass = h4_sym_ok ? 1U : 0U;
    HSEM_TEST_SHARED->cm4_done = 9U;
    __DMB();

    /* Wait for CM7 to move to Phase H8: Recovery */
    while (HSEM_TEST_SHARED->go_signal != 10U)
    {
        __NOP();
    }

    /* ---- H8: Recovery ---- */
    /* CM4 takes IDs 24..27 and stops */
    HSEM_BM_FastTake(24U);
    HSEM_BM_FastTake(25U);
    HSEM_BM_FastTake(26U);
    HSEM_BM_FastTake(27U);
    HSEM_TEST_SHARED->cm4_done = 10U;
    __DMB();

    /* Wait for CM7 to perform recovery */
    while (HSEM_TEST_SHARED->go_signal != 11U)
    {
        __NOP();
    }

    /* ---- H9: Notifications (NVIC Interrupt-driven, B3) ---- */
    bool h9_m4_ok = true;

    /* Negative Control 1: IER disabled -> 0 IRQs */
    HSEM_BM_NotifyDisable(1UL << HSEM_TEST_ID_NOTIFY);
    s_m4_irq_count = 0U;
    NVIC_ClearPendingIRQ(HSEM2_IRQn);
    NVIC_SetPriority(HSEM2_IRQn, 5);
    NVIC_EnableIRQ(HSEM2_IRQn);

    HSEM_TEST_SHARED->cm4_done = 11U;
    __DMB();

    /* Wait for CM7 to perform 5 releases with IER disabled */
    while (HSEM_TEST_SHARED->go_signal != 12U)
    {
        __NOP();
    }

    HSEM_TEST_SHARED->h9_neg1_irqs = s_m4_irq_count;
    if (s_m4_irq_count != 0U)
    {
        h9_m4_ok = false;
    }

    /* Negative Control 2: Stale flag cleared on enable */
    /* Enable notification on ID_NOTIFY: NotifyEnable clears ICR first, then sets IER */
    HSEM_BM_NotifyEnable(1UL << HSEM_TEST_ID_NOTIFY);
    for (volatile uint32_t d = 0U; d < 10000U; d++) { __NOP(); }

    HSEM_TEST_SHARED->h9_neg2_irqs = s_m4_irq_count;
    if (s_m4_irq_count != 0U)
    {
        h9_m4_ok = false;
    }

    HSEM_TEST_SHARED->cm4_done = 12U;
    __DMB();

    /* Wait for CM7 to begin active 500-transfer H9 test */
    while (HSEM_TEST_SHARED->go_signal != 13U)
    {
        __NOP();
    }

    /* CM4 receives 500 notifications via HSEM2_IRQHandler */
    uint32_t prev_irq = 0U;
    uint32_t wait_timeout = 0U;
    while ((s_m4_irq_count < HSEM_TEST_H9_COUNT) && (wait_timeout < 10000000U))
    {
        if (s_m4_irq_count != prev_irq)
        {
            prev_irq = s_m4_irq_count;
            HSEM_TEST_SHARED->h9_m4_notifications = prev_irq;
            wait_timeout = 0U;
        }
        wait_timeout++;
    }
    HSEM_TEST_SHARED->h9_m4_notifications = s_m4_irq_count;
    if (s_m4_irq_count != HSEM_TEST_H9_COUNT)
    {
        h9_m4_ok = false;
    }

    HSEM_BM_NotifyDisable(1UL << HSEM_TEST_ID_NOTIFY);
    NVIC_DisableIRQ(HSEM2_IRQn);
    NVIC_ClearPendingIRQ(HSEM2_IRQn);

    HSEM_TEST_SHARED->cm4_done = 13U;
    __DMB();

    /* Reverse Notification: CM4 sends 100 releases to CM7 for HAL notification test */
    while (HSEM_TEST_SHARED->go_signal != 14U)
    {
        __NOP();
    }

    for (uint32_t i = 0U; i < 100U; i++)
    {
        while (HSEM_BM_FastTake(HSEM_TEST_ID_NOTIFY) != HSEM_BM_OK)
        {
            __NOP();
        }
        HSEM_BM_Release(HSEM_TEST_ID_NOTIFY, 0U);

        uint32_t wait_m7 = 0U;
        while ((HSEM_TEST_SHARED->h9_m7_notifications <= i) && (wait_m7 < 1000000U))
        {
            wait_m7++;
        }
        if (wait_m7 >= 1000000U)
        {
            h9_m4_ok = false;
            break;
        }
    }

    HSEM_TEST_SHARED->h9_m4_pass = h9_m4_ok ? 1U : 0U;
    HSEM_TEST_SHARED->cm4_done = 14U;
    __DMB();

    /* Format CM4 summary lines */
    Line_Begin();
    Line_Str("banner rev=");
#if defined(FW_GIT_REV)
    Line_Str(FW_GIT_REV);
#else
    Line_Str("unknown");
#endif
    Line_Str(" core=CM4 HSEM_BM/LL");
    Line_End();

    Line_Begin();
    Line_Str("H0 characterization=");
    Line_Str(h0_ok ? "PASS" : "FAIL");
    Line_Str(" H3 release_prot=");
    Line_Str(h3_ok ? "PASS" : "FAIL");
    Line_End();

    Line_Begin();
    Line_Str("H7 same_core=");
    Line_Str(h7_ok ? "PASS" : "FAIL");
    Line_Str(" H7 neg_raw_hal=");
    Line_U32(HSEM_TEST_SHARED->h7_neg_raw_hal);
    Line_End();

    Line_Begin();
    Line_Str("H10 parity=");
    Line_Str(h10_all_ok ? "PASS" : "FAIL");
    Line_Str(" H4 symmetric=");
    Line_Str(h4_sym_ok ? "PASS" : "FAIL");
    Line_End();

    Line_Begin();
    Line_Str("H9 irqs=");
    Line_U32(s_m4_irq_count);
    Line_Str(" H9 neg1=");
    Line_U32(HSEM_TEST_SHARED->h9_neg1_irqs);
    Line_Str(" H9 neg2=");
    Line_U32(HSEM_TEST_SHARED->h9_neg2_irqs);
    Line_Str(" H9 status=");
    Line_Str(h9_m4_ok ? "PASS" : "FAIL");
    Line_End();

    bool cm4_all_ok = h0_ok && h3_ok && h7_ok && h10_all_ok && h4_sym_ok && h9_m4_ok;
    Line_Begin();
    Line_Str("CM4 VERDICT=");
    Line_Str(cm4_all_ok ? "PASS" : "FAIL");
    Line_End();
}

void HSEM_SelfTest_CM4_Poll(void)
{
    if (s_line_next >= s_line_count) return;

    SHARED_LOCK(HSEM_ID_M4_TO_M7_MBOX);
    if (g_shared.m4_to_m7_len == 0U)
    {
        uint32_t len = (uint32_t)strlen(s_lines[s_line_next]);
        memcpy((void *)g_shared.m4_to_m7, s_lines[s_line_next], len + 1U);
        g_shared.m4_to_m7_len = len;
        s_line_next++;
    }
    SHARED_UNLOCK(HSEM_ID_M4_TO_M7_MBOX);
}
