/**
  ******************************************************************************
  * @file    hsem_selftest.c
  * @brief   HSEM driver self-test implementation for Cortex-M7.
  ******************************************************************************
  */

#include "hsem_selftest.h"
#include "hsem_selftest_shared.h"
#include "board_def.h"
#include "shared_memory.h"
#include "app_log.h"
#include "stm32h7xx.h"

void HSEM_SelfTest_CM7_Run(void)
{
    APP_LOG(" ");
    APP_LOG("=================================================================");
#if defined(FW_GIT_REV)
    APP_LOG(" STM32H755 HSEM Driver Self-Test - Rev %s", FW_GIT_REV);
#else
    APP_LOG(" STM32H755 HSEM Driver Self-Test");
#endif
    APP_LOG(" Phase F0 Base: DWT verification, B1 baseline & H2 negative control");
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
    HSEM->R[25] = (3U << 8); /* CM7 core_id = 3 */
    __DMB();

    /* FastTake: 1-step read of RLR */
    uint32_t c_start = DWT->CYCCNT;
    volatile uint32_t rlr_val = HSEM->RLR[25];
    uint32_t fast_take_cyc = DWT->CYCCNT - c_start;
    (void)rlr_val;

    /* Release */
    c_start = DWT->CYCCNT;
    HSEM->R[25] = (3U << 8);
    uint32_t rel_cyc = DWT->CYCCNT - c_start;
    __DMB();

    /* 2-step Take */
    c_start = DWT->CYCCNT;
    HSEM->R[25] = (1U << 31) | (3U << 8) | 7U; /* LOCK | COREID 3 | PROCID 7 */
    volatile uint32_t r_read = HSEM->R[25];
    uint32_t take_cyc = DWT->CYCCNT - c_start;
    (void)r_read;

    /* Release */
    HSEM->R[25] = (3U << 8) | 7U;
    __DMB();

    HSEM_TEST_SHARED->cm7_raw_fast_take_cycles = fast_take_cyc;
    HSEM_TEST_SHARED->cm7_raw_take_cycles = take_cyc;
    HSEM_TEST_SHARED->cm7_raw_release_cycles = rel_cyc;

    APP_LOG("[m7_hsem_test] B1 raw timing (400 MHz): fast_take=%lu cyc, take=%lu cyc, release=%lu cyc",
            fast_take_cyc, take_cyc, rel_cyc);

    /* 4. H2 Negative Control */
    APP_LOG("[m7_hsem_test] Starting H2 Negative Control (contention without lock)...");

    /* Wait for CM4 to be ready */
    uint32_t wait_cm4 = 0;
    while ((HSEM_TEST_SHARED->cm4_ready == 0U) && (wait_cm4++ < 10000000U))
    {
        __NOP();
    }

    if (HSEM_TEST_SHARED->cm4_ready == 0U)
    {
        APP_LOG("[m7_hsem_test] ERROR: CM4 not ready for H2 test");
        return;
    }

    /* Part A: Counter RMW without lock */
    HSEM_TEST_SHARED->shared_counter = 0U;
    HSEM_TEST_SHARED->cm7_done = 0U;
    HSEM_TEST_SHARED->cm4_done = 0U;
    __DMB();

    HSEM_TEST_SHARED->cm7_ready = 1U;
    HSEM_TEST_SHARED->go_signal = 1U;
    __DMB();

    uint32_t m7_count = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H2_ITERATIONS; i++)
    {
        uint32_t val = HSEM_TEST_SHARED->shared_counter;
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
        HSEM_TEST_SHARED->shared_counter = val + 1U;
        m7_count++;
    }
    HSEM_TEST_SHARED->m7_h2_count = m7_count;
    __DMB();
    HSEM_TEST_SHARED->cm7_done = 1U;

    /* Wait for CM4 to finish Part A */
    while (HSEM_TEST_SHARED->cm4_done != 1U)
    {
        __NOP();
    }

    uint32_t expected = HSEM_TEST_SHARED->m4_h2_count + HSEM_TEST_SHARED->m7_h2_count;
    uint32_t actual = HSEM_TEST_SHARED->shared_counter;
    uint32_t lost = (expected > actual) ? (expected - actual) : 0U;
    HSEM_TEST_SHARED->h2_lost_increments = lost;

    APP_LOG("[m7_hsem_test] H2 Part A (counter without lock): N=%lu, m7=%lu, m4=%lu, final=%lu, lost=%lu (%s)",
            (uint32_t)HSEM_TEST_H2_ITERATIONS, m7_count, HSEM_TEST_SHARED->m4_h2_count,
            actual, lost, (lost > 0U) ? "PASS: data loss reproduced" : "FAIL: 0 losses");

    /* Part B: Torn register without lock */
    for (uint32_t w = 0U; w < 8U; w++)
    {
        HSEM_TEST_SHARED->shared_reg[w] = 0U;
    }
    __DMB();

    /* Signal CM4 to start Part B */
    HSEM_TEST_SHARED->go_signal = 2U;
    __DMB();

    uint32_t torn_reads = 0U;
    for (uint32_t i = 0U; i < HSEM_TEST_H2_TORN_ITERS; i++)
    {
        uint32_t w0 = HSEM_TEST_SHARED->shared_reg[0];
        uint32_t w1 = HSEM_TEST_SHARED->shared_reg[1];
        uint32_t w2 = HSEM_TEST_SHARED->shared_reg[2];
        uint32_t w3 = HSEM_TEST_SHARED->shared_reg[3];
        uint32_t w4 = HSEM_TEST_SHARED->shared_reg[4];
        uint32_t w5 = HSEM_TEST_SHARED->shared_reg[5];
        uint32_t w6 = HSEM_TEST_SHARED->shared_reg[6];
        uint32_t w7 = HSEM_TEST_SHARED->shared_reg[7];

        if ((w0 != w1) || (w0 != w2) || (w0 != w3) || (w0 != w4) ||
            (w0 != w5) || (w0 != w6) || (w0 != w7))
        {
            torn_reads++;
        }
    }
    HSEM_TEST_SHARED->h2_torn_reads = torn_reads;
    __DMB();
    HSEM_TEST_SHARED->cm7_done = 2U;

    while (HSEM_TEST_SHARED->cm4_done != 2U)
    {
        __NOP();
    }

    APP_LOG("[m7_hsem_test] H2 Part B (torn register without lock): N=%lu, torn_reads=%lu (%s)",
            (uint32_t)HSEM_TEST_H2_TORN_ITERS, torn_reads,
            (torn_reads > 0U) ? "PASS: torn reads reproduced" : "FAIL: 0 torn reads");

    APP_LOG("[m7_hsem_test] F0 Base verification COMPLETE");
}
