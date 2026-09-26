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
#include <string.h>

#define ST_MAX_LINES 20
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

static void Hsem_TakeSnapshot_M4(HsemSnapshot_t *s)
{
    for (uint32_t i = 0; i < 8; i++)
    {
        s->R[i] = HSEM->R[24 + i];
    }
    s->IER = HSEM->C2IER;
    s->ISR = HSEM->C2ISR;
}

static void RunSequence_BM_M4(H10_StepResult_t res[H10_STEPS])
{
    (void)HSEM_BM_Release(24, 0);
    (void)HSEM_BM_Release(25, 42);
    (void)HSEM_BM_NotifyDisable(0xFFFFFFFFUL);

    /* Step 0: FastTake on sem 24 */
    res[0].st = HSEM_BM_FastTake(24);
    Hsem_TakeSnapshot_M4(&res[0].snap);

    /* Step 1: 2-step Take on sem 25, proc 42 */
    res[1].st = HSEM_BM_Take(25, 42);
    Hsem_TakeSnapshot_M4(&res[1].snap);

    /* Step 2: Release sem 25, proc 42 */
    res[2].st = HSEM_BM_Release(25, 42);
    Hsem_TakeSnapshot_M4(&res[2].snap);

    /* Step 3: Release sem 24 with wrong proc 99 */
    res[3].st = HSEM_BM_Release(24, 99);
    Hsem_TakeSnapshot_M4(&res[3].snap);

    /* Step 4: Release sem 24 with correct proc 0 */
    res[4].st = HSEM_BM_Release(24, 0);
    Hsem_TakeSnapshot_M4(&res[4].snap);

    /* Step 5: NotifyEnable sem 26 */
    bool en = HSEM_BM_NotifyEnable(1UL << 26);
    res[5].st = en ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M4(&res[5].snap);

    /* Step 6: NotifyDisable sem 26 */
    bool dis = HSEM_BM_NotifyDisable(1UL << 26);
    res[6].st = dis ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M4(&res[6].snap);
}

static void RunSequence_LL_M4(H10_StepResult_t res[H10_STEPS])
{
    (void)HSEM_LL_Release(24, 0);
    (void)HSEM_LL_Release(25, 42);
    (void)HSEM_LL_NotifyDisable(0xFFFFFFFFUL);

    /* Step 0: FastTake on sem 24 */
    res[0].st = HSEM_LL_FastTake(24);
    Hsem_TakeSnapshot_M4(&res[0].snap);

    /* Step 1: 2-step Take on sem 25, proc 42 */
    res[1].st = HSEM_LL_Take(25, 42);
    Hsem_TakeSnapshot_M4(&res[1].snap);

    /* Step 2: Release sem 25, proc 42 */
    res[2].st = HSEM_LL_Release(25, 42);
    Hsem_TakeSnapshot_M4(&res[2].snap);

    /* Step 3: Release sem 24 with wrong proc 99 */
    res[3].st = HSEM_LL_Release(24, 99);
    Hsem_TakeSnapshot_M4(&res[3].snap);

    /* Step 4: Release sem 24 with correct proc 0 */
    res[4].st = HSEM_LL_Release(24, 0);
    Hsem_TakeSnapshot_M4(&res[4].snap);

    /* Step 5: NotifyEnable sem 26 */
    bool en = HSEM_LL_NotifyEnable(1UL << 26);
    res[5].st = en ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M4(&res[5].snap);

    /* Step 6: NotifyDisable sem 26 */
    bool dis = HSEM_LL_NotifyDisable(1UL << 26);
    res[6].st = dis ? HSEM_BM_OK : HSEM_BM_ERR_PARAM;
    Hsem_TakeSnapshot_M4(&res[6].snap);
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

void HSEM_SelfTest_CM4_Run(void)
{
    /* 1. Driver and DWT initialization */
    HsemBm_Status_t init_st = HSEM_BM_Init();
    bool tb_ok = HSEM_BM_TimebaseStart();

    HSEM_TEST_SHARED->cm4_dwt_ok = tb_ok ? 1U : 0U;
    HSEM_TEST_SHARED->cm4_dwt_delta = DWT->CYCCNT;

    /* 2. H0 Characterization on CM4 */
    bool h0_ok = (init_st == HSEM_BM_OK) && (HSEM_BM_GetCoreId() == HSEM_BM_COREID_CPU2);

    /* Test 1-step take characterization */
    if (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) == HSEM_BM_OK)
    {
        uint32_t r_val = HSEM->R[HSEM_TEST_ID_LOCK1];
        uint32_t c_id = (r_val >> 8) & 0x0FU;
        uint32_t p_id = r_val & 0xFFU;
        if ((c_id != HSEM_BM_COREID_CPU2) || (p_id != 0U))
        {
            h0_ok = false;
        }
        HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 0U);
    }
    else
    {
        h0_ok = false;
    }

    /* Test GetOwner on free semaphore does not take it (C11) */
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
    if (HSEM_BM_ReleaseAllOfCore(HSEM_BM_COREID_CPU2, 0U) != HSEM_BM_ERR_PARAM) h0_ok = false;
    if (HSEM_BM_NotifyEnable(0U) != false) h0_ok = false;

    HSEM_TEST_SHARED->h0_m4_pass = h0_ok ? 1U : 0U;

    /* 3. H3: Release Protection on CM4 */
    bool h3_ok = true;
    /* 2-step with proc 7 */
    if (HSEM_BM_Take(HSEM_TEST_ID_LOCK1, 7U) == HSEM_BM_OK)
    {
        /* (a) Raw release attempt with wrong proc 8 */
        HSEM->R[HSEM_TEST_ID_LOCK1] = (HSEM_BM_COREID_CPU2 << 8) | 8U;
        __DMB();
        /* Verify still held by proc 7 */
        uint32_t r = HSEM->R[HSEM_TEST_ID_LOCK1];
        if (((r & (1UL << 31)) == 0U) || ((r & 0xFFU) != 7U))
        {
            h3_ok = false;
        }

        /* (d) HSEM_BM_Release with wrong proc */
        if (HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 8U) != HSEM_BM_ERR_NOT_OWNER)
        {
            h3_ok = false;
        }

        /* Release correctly */
        if (HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 7U) != HSEM_BM_OK)
        {
            h3_ok = false;
        }
    }
    else
    {
        h3_ok = false;
    }

    /* 1-step (proc 0) */
    if (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) == HSEM_BM_OK)
    {
        /* Raw release with proc 5 */
        HSEM->R[HSEM_TEST_ID_LOCK1] = (HSEM_BM_COREID_CPU2 << 8) | 5U;
        __DMB();
        if (!HSEM_BM_IsLocked(HSEM_TEST_ID_LOCK1))
        {
            h3_ok = false;
        }
        HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 0U);
    }
    else
    {
        h3_ok = false;
    }
    HSEM_TEST_SHARED->h3_m4_pass = h3_ok ? 1U : 0U;

    /* 4. H7: Same-Core Context Collision on CM4 (S1) */
    bool h7_ok = true;
    if (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) == HSEM_BM_OK)
    {
        /* Context B on same core attempts FastTake */
        if (HSEM_BM_FastTake(HSEM_TEST_ID_LOCK1) != HSEM_BM_BUSY_SELF)
        {
            h7_ok = false;
        }
        HSEM_BM_Release(HSEM_TEST_ID_LOCK1, 0U);
    }
    else
    {
        h7_ok = false;
    }

    if (HSEM_BM_Take(HSEM_TEST_ID_LOCK2, 1U) == HSEM_BM_OK)
    {
        /* Context B on same core attempts Take with proc 2 */
        if (HSEM_BM_Take(HSEM_TEST_ID_LOCK2, 2U) != HSEM_BM_BUSY_SELF)
        {
            h7_ok = false;
        }
        if (HSEM_BM_Release(HSEM_TEST_ID_LOCK2, 2U) != HSEM_BM_ERR_NOT_OWNER)
        {
            h7_ok = false;
        }
        HSEM_BM_Release(HSEM_TEST_ID_LOCK2, 1U);
    }
    else
    {
        h7_ok = false;
    }
    HSEM_TEST_SHARED->h7_m4_pass = h7_ok ? 1U : 0U;

    /* 5. H10: BM <-> LL Parity on CM4 */
    H10_StepResult_t bm_res[H10_STEPS];
    H10_StepResult_t ll_res[H10_STEPS];
    RunSequence_BM_M4(bm_res);
    RunSequence_LL_M4(ll_res);

    bool h10_ok = CompareSnapshots_M4(bm_res, ll_res);

    /* Negative control: mutate a snapshot to confirm comparison detects mismatch */
    H10_StepResult_t mutated_res[H10_STEPS];
    for (uint32_t i = 0U; i < H10_STEPS; i++)
    {
        mutated_res[i] = ll_res[i];
    }
    mutated_res[1].snap.R[1] ^= 0x1U;
    bool neg_detected = !CompareSnapshots_M4(bm_res, mutated_res);

    HSEM_TEST_SHARED->h10_m4_pass = (h10_ok && neg_detected) ? 1U : 0U;

    /* 6. Synchronize with CM7 for dual-core tests */
    HSEM_TEST_SHARED->cm4_ready = 1U;
    __DMB();

    /* Wait for CM7 to signal start */
    while (HSEM_TEST_SHARED->go_signal != 1U)
    {
        __NOP();
    }

    /* ---- H1 Phase 1: 1-step FastTake mutual exclusion ---- */
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
    HSEM_TEST_SHARED->cm4_done = 1U;

    /* Wait for CM7 to move to Phase 2 */
    while (HSEM_TEST_SHARED->go_signal != 2U)
    {
        __NOP();
    }

    /* ---- H1b: Torn Register with Lock ---- */
    /* CM4 continuously writes 8 words under lock */
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
    HSEM_TEST_SHARED->cm4_done = 2U;

    /* Wait for CM7 to move to Phase 3: H4 Timeout */
    while (HSEM_TEST_SHARED->go_signal != 3U)
    {
        __NOP();
    }

    /* ---- H4: Bounded Timeout cooperation ---- */
    /* CM4 takes ID_TIMEOUT and holds it for 5 ms, then releases */
    HSEM_BM_FastTake(HSEM_TEST_ID_TIMEOUT);
    HSEM_TEST_SHARED->cm4_done = 3U;
    __DMB();

    /* Hold for 5 ms */
    HAL_Delay(5);
    HSEM_BM_Release(HSEM_TEST_ID_TIMEOUT, 0U);

    /* Wait for CM7 step C */
    while (HSEM_TEST_SHARED->go_signal != 4U)
    {
        __NOP();
    }

    /* CM4 takes ID_TIMEOUT and holds it for 25 ms (CM7 times out at 10 ms) */
    HSEM_BM_FastTake(HSEM_TEST_ID_TIMEOUT);
    HSEM_TEST_SHARED->cm4_done = 4U;
    __DMB();

    HAL_Delay(25);
    HSEM_BM_Release(HSEM_TEST_ID_TIMEOUT, 0U);

    /* Wait for CM7 to move to Phase 5: H8 Recovery */
    while (HSEM_TEST_SHARED->go_signal != 5U)
    {
        __NOP();
    }

    /* ---- H8: Recovery ---- */
    /* CM4 takes IDs 24..27 and stops */
    HSEM_BM_FastTake(24U);
    HSEM_BM_FastTake(25U);
    HSEM_BM_FastTake(26U);
    HSEM_BM_FastTake(27U);
    HSEM_TEST_SHARED->cm4_done = 5U;
    __DMB();

    /* Wait for CM7 to perform recovery */
    while (HSEM_TEST_SHARED->go_signal != 6U)
    {
        __NOP();
    }

    /* ---- H9: Notifications ---- */
    /* CM4 subscribes to ID 26 notifications */
    HSEM_BM_NotifyEnable(1UL << HSEM_TEST_ID_NOTIFY);
    HSEM_TEST_SHARED->cm4_done = 6U;
    __DMB();

    uint32_t notifications_seen = 0U;
    while (notifications_seen < HSEM_TEST_H9_COUNT)
    {
        uint32_t mask = HSEM_BM_NotifyTake();
        if ((mask & (1UL << HSEM_TEST_ID_NOTIFY)) != 0U)
        {
            notifications_seen++;
            /* Re-arm for next */
            HSEM_BM_NotifyEnable(1UL << HSEM_TEST_ID_NOTIFY);
        }
    }
    HSEM_TEST_SHARED->h9_m4_notifications = notifications_seen;
    HSEM_BM_NotifyDisable(1UL << HSEM_TEST_ID_NOTIFY);
    HSEM_TEST_SHARED->cm4_done = 7U;
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
    Line_Str(" H9 notifications=");
    Line_U32(notifications_seen);
    Line_Str(" H10 parity=");
    Line_Str(HSEM_TEST_SHARED->h10_m4_pass ? "PASS" : "FAIL");
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
