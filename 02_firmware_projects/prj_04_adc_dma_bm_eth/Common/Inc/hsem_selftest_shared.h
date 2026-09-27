/**
  ******************************************************************************
  * @file    hsem_selftest_shared.h
  * @brief   Shared definitions for HSEM driver selftest between CM7 and CM4.
  *          Located in SRAM4 (RAM_SHARED) at 0x38009000 (above g_shared).
  ******************************************************************************
  */

#ifndef HSEM_SELFTEST_SHARED_H
#define HSEM_SELFTEST_SHARED_H

#include <stdint.h>
#include <stdbool.h>

#define HSEM_TEST_SHARED_ADDR       (0x38009000UL)

#define HSEM_TEST_ID_START          24U
#define HSEM_TEST_ID_END            31U

/* Disjoint semaphore partitions for concurrent boot tests (H0, H3, H7, H10) */
#define HSEM_TEST_M7_LOCK1          24U
#define HSEM_TEST_M7_LOCK2          25U
#define HSEM_TEST_M7_NOTIFY         26U

#define HSEM_TEST_M4_LOCK1          28U
#define HSEM_TEST_M4_LOCK2          29U
#define HSEM_TEST_M4_NOTIFY         30U

/* Synchronized dual-core test semaphores (gated by go_signal) */
#define HSEM_TEST_ID_LOCK1          24U  /* H1, H1b */
#define HSEM_TEST_ID_LOCK2          25U  /* H1 2-step */
#define HSEM_TEST_ID_NOTIFY         26U  /* H9 */
#define HSEM_TEST_ID_TIMEOUT        27U  /* H4 */
#define HSEM_TEST_ID_RECOV_CM7      28U  /* H8 CM7 canary */

#define HSEM_TEST_CLEAR_KEY         0xA5C3U

#define HSEM_TEST_H2_ITERATIONS     100000UL
#define HSEM_TEST_H2_TORN_ITERS     50000UL
#define HSEM_TEST_H1_ITERATIONS     500000UL
#define HSEM_TEST_H1_2STEP_ITERS    500000UL
#define HSEM_TEST_H1B_ITERATIONS    500000UL
#define HSEM_TEST_H9_COUNT          500U

typedef struct __attribute__((aligned(32)))
{
    volatile uint32_t cm7_ready;
    volatile uint32_t cm4_ready;
    volatile uint32_t go_signal;
    volatile uint32_t phase;

    /* H0 Characterization */
    volatile uint32_t h0_m4_pass;
    volatile uint32_t h0_m7_pass;

    /* H2 Negative Control (F0 / M5) */
    volatile uint32_t h2_shared_counter;
    volatile uint32_t h2_m4_count;
    volatile uint32_t h2_m7_count;
    volatile uint32_t h2_lost_increments;
    volatile uint32_t h2_shared_reg[8];
    volatile uint32_t h2_torn_reads;
    volatile uint32_t h2_pass;

    /* H1 Mutual Exclusion: 1-step (FastTake) */
    volatile uint32_t h1_shared_counter;
    volatile uint32_t h1_m4_private_count;
    volatile uint32_t h1_m7_private_count;
    volatile uint32_t h1_pass;

    /* H1 Mutual Exclusion: 2-step (Take) (M5) */
    volatile uint32_t h1_2step_shared_counter;
    volatile uint32_t h1_2step_m4_private_count;
    volatile uint32_t h1_2step_m7_private_count;
    volatile uint32_t h1_2step_pass;

    /* H1b Torn Register with Lock */
    volatile uint32_t h1b_shared_reg[8];
    volatile uint32_t h1b_torn_reads;
    volatile uint32_t h1b_pass;

    /* H3 Release Protection (including cross-core Case B) */
    volatile uint32_t h3_m4_pass;
    volatile uint32_t h3_m7_pass;
    volatile uint32_t h3_cross_pass;

    /* H4 Timeout (including symmetric CM4 timeout on CM7 lock) */
    volatile uint32_t h4_m4_pass;
    volatile uint32_t h4_m7_pass;
    volatile uint32_t h4_measured_ms;
    volatile uint32_t h4_relax_calls;
    volatile uint32_t h4_symmetric_pass;

    /* H7 Same-Core Context Collision */
    volatile uint32_t h7_m4_pass;
    volatile uint32_t h7_m7_pass;
    volatile uint32_t h7_neg_raw_hal;

    /* H8 Recovery */
    volatile uint32_t h8_pass;
    volatile uint32_t h8_cm4_stopped;

    /* H5 Boot Handshake */
    volatile uint32_t h5_pass;
    volatile uint32_t h5_measured_ms;
    volatile uint32_t h5_ok;
    volatile uint32_t h5b_ok;
    volatile uint32_t h5c_ok;
    volatile uint32_t h5_rel_ok;

    /* H9 Notification (NVIC interrupt-driven) */
    volatile uint32_t h9_m4_notifications;
    volatile uint32_t h9_m7_notifications;
    volatile uint32_t h9_neg1_irqs;
    volatile uint32_t h9_neg2_irqs;
    volatile uint32_t h9_m4_pass;
    volatile uint32_t h9_m7_pass;

    /* H10 Parity (BM <-> LL <-> HAL) */
    volatile uint32_t h10_m4_pass;
    volatile uint32_t h10_m7_pass;

    /* Synchronization completion flags */
    volatile uint32_t cm4_done;
    volatile uint32_t cm7_done;

    /* DWT CYCCNT verification */
    volatile uint32_t cm4_dwt_ok;
    volatile uint32_t cm4_dwt_delta;
    volatile uint32_t cm7_dwt_ok;
    volatile uint32_t cm7_dwt_delta;

    /* B1 Raw Timing Baseline */
    volatile uint32_t cm4_raw_fast_take_cycles;
    volatile uint32_t cm4_raw_take_cycles;
    volatile uint32_t cm4_raw_release_cycles;

    volatile uint32_t cm7_raw_fast_take_cycles;
    volatile uint32_t cm7_raw_take_cycles;
    volatile uint32_t cm7_raw_release_cycles;

    /* Overall verdict computed conditionally */
    volatile uint32_t all_tests_passed;

} HsemTestShared_t;

#define HSEM_TEST_SHARED   ((volatile HsemTestShared_t *)HSEM_TEST_SHARED_ADDR)

#endif /* HSEM_SELFTEST_SHARED_H */
