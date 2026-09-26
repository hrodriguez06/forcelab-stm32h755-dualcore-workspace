/**
  ******************************************************************************
  * @file    hsem_selftest.c
  * @brief   HSEM driver self-test implementation for Cortex-M4.
  ******************************************************************************
  */

#include "hsem_selftest.h"
#include "hsem_selftest_shared.h"
#include "board_def.h"
#include "shared_memory.h"
#include "stm32h7xx.h"
#include <string.h>

#define ST_MAX_LINES 16
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

void HSEM_SelfTest_CM4_Run(void)
{
    /* 1. Ensure HSEM clock is enabled */
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

    /* 3. B1: Raw timing baseline on CM4 (using ID 24) */
    /* Release any hold on ID 24 first */
    HSEM->R[24] = (1U << 8); /* CM4 core_id = 1 */
    __DMB();

    /* FastTake: 1-step read of RLR */
    uint32_t c_start = DWT->CYCCNT;
    volatile uint32_t rlr_val = HSEM->RLR[24];
    uint32_t fast_take_cyc = DWT->CYCCNT - c_start;
    (void)rlr_val;

    /* Release */
    c_start = DWT->CYCCNT;
    HSEM->R[24] = (1U << 8);
    uint32_t rel_cyc = DWT->CYCCNT - c_start;
    __DMB();

    /* 2-step Take */
    c_start = DWT->CYCCNT;
    HSEM->R[24] = (1U << 31) | (1U << 8) | 7U; /* LOCK | COREID 1 | PROCID 7 */
    volatile uint32_t r_read = HSEM->R[24];
    uint32_t take_cyc = DWT->CYCCNT - c_start;
    (void)r_read;

    /* Release */
    HSEM->R[24] = (1U << 8) | 7U;
    __DMB();

    HSEM_TEST_SHARED->cm4_raw_fast_take_cycles = fast_take_cyc;
    HSEM_TEST_SHARED->cm4_raw_take_cycles = take_cyc;
    HSEM_TEST_SHARED->cm4_raw_release_cycles = rel_cyc;

    /* 4. H2 Negative Control: Concurrent contention without lock */
    HSEM_TEST_SHARED->cm4_ready = 1U;
    __DMB();

    /* Wait for CM7 to signal start of H2 Part A */
    while (HSEM_TEST_SHARED->go_signal != 1U)
    {
        __NOP();
    }

    /* Part A: Increment shared_counter without lock */
    uint32_t m4_count = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H2_ITERATIONS; i++)
    {
        uint32_t val = HSEM_TEST_SHARED->shared_counter;
        /* Small delay window to force bus collisions */
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
        HSEM_TEST_SHARED->shared_counter = val + 1U;
        m4_count++;
    }
    HSEM_TEST_SHARED->m4_h2_count = m4_count;
    __DMB();
    HSEM_TEST_SHARED->cm4_done = 1U;

    /* Wait for CM7 to signal start of H2 Part B */
    while (HSEM_TEST_SHARED->go_signal != 2U)
    {
        __NOP();
    }

    /* Part B: Continuously write 8-word registers without lock */
    for (uint32_t i = 0U; i < HSEM_TEST_H2_TORN_ITERS; i++)
    {
        uint32_t v = i + 1U;
        for (uint32_t w = 0U; w < 8U; w++)
        {
            HSEM_TEST_SHARED->shared_reg[w] = v;
        }
    }
    __DMB();
    HSEM_TEST_SHARED->cm4_done = 2U;

    /* 5. Format CM4 selftest lines */
    Line_Begin();
    Line_Str("banner rev=");
#if defined(FW_GIT_REV)
    Line_Str(FW_GIT_REV);
#else
    Line_Str("unknown");
#endif
    Line_Str(" core=CM4");
    Line_End();

    Line_Begin();
    Line_Str("DWT CYCCNT running=");
    Line_U32(HSEM_TEST_SHARED->cm4_dwt_ok);
    Line_Str(" delta=");
    Line_U32(HSEM_TEST_SHARED->cm4_dwt_delta);
    Line_End();

    Line_Begin();
    Line_Str("B1 raw_fast_take=");
    Line_U32(fast_take_cyc);
    Line_Str(" raw_take=");
    Line_U32(take_cyc);
    Line_Str(" raw_release=");
    Line_U32(rel_cyc);
    Line_End();

    Line_Begin();
    Line_Str("H2 m4_count=");
    Line_U32(m4_count);
    Line_Str(" done=2");
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
