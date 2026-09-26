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
#define HSEM_TEST_ID_LOCK1          24U
#define HSEM_TEST_ID_LOCK2          25U
#define HSEM_TEST_ID_NOTIFY         26U
#define HSEM_TEST_ID_TIMEOUT        27U
#define HSEM_TEST_ID_RECOV_CM7      28U

#define HSEM_TEST_CLEAR_KEY         0xA5C3U
#define HSEM_TEST_H1_ITERATIONS     1000000UL
#define HSEM_TEST_H1B_ITERATIONS    1000000UL
#define HSEM_TEST_H9_COUNT          1000U

typedef struct __attribute__((aligned(32)))
{
    volatile uint32_t cm7_ready;
    volatile uint32_t cm4_ready;
    volatile uint32_t go_signal;
    volatile uint32_t phase;

    /* H0 Characterization */
    volatile uint32_t h0_m4_pass;
    volatile uint32_t h0_m7_pass;

    /* H1 Mutual Exclusion (1-step and 2-step) */
    volatile uint32_t h1_shared_counter;
    volatile uint32_t h1_m4_private_count;
    volatile uint32_t h1_m7_private_count;

    /* H1b Torn Register with Lock */
    volatile uint32_t h1b_shared_reg[8];
    volatile uint32_t h1b_torn_reads;

    /* H3 Release Protection */
    volatile uint32_t h3_m4_pass;
    volatile uint32_t h3_m7_pass;

    /* H4 Timeout */
    volatile uint32_t h4_m4_pass;
    volatile uint32_t h4_m7_pass;
    volatile uint32_t h4_measured_ms;
    volatile uint32_t h4_relax_calls;

    /* H7 Same-Core Context Collision */
    volatile uint32_t h7_m4_pass;
    volatile uint32_t h7_m7_pass;

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

    /* H9 Notification */
    volatile uint32_t h9_m4_notifications;
    volatile uint32_t h9_m7_notifications;

    /* H10 Parity (BM <-> LL) */
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

    /* H2 Negative Control results from F0 */
    volatile uint32_t h2_lost_increments;
    volatile uint32_t h2_torn_reads;
} HsemTestShared_t;

#define HSEM_TEST_SHARED   ((volatile HsemTestShared_t *)HSEM_TEST_SHARED_ADDR)

#endif /* HSEM_SELFTEST_SHARED_H */
