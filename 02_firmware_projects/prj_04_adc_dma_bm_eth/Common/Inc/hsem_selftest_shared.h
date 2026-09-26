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
#define HSEM_TEST_H2_ITERATIONS     100000U
#define HSEM_TEST_H2_TORN_ITERS     50000U

typedef struct __attribute__((aligned(32)))
{
    volatile uint32_t cm7_ready;
    volatile uint32_t cm4_ready;
    volatile uint32_t go_signal;
    volatile uint32_t phase;

    /* H2 Negative Control: Concurrent counter without lock */
    volatile uint32_t shared_counter;
    volatile uint32_t m4_h2_count;
    volatile uint32_t m7_h2_count;
    volatile uint32_t h2_lost_increments;

    /* H2 Negative Control: Torn register read without lock */
    volatile uint32_t shared_reg[8];
    volatile uint32_t h2_torn_reads;

    /* Synchronization completion flags */
    volatile uint32_t cm4_done;
    volatile uint32_t cm7_done;

    /* DWT CYCCNT verification */
    volatile uint32_t cm4_dwt_ok;
    volatile uint32_t cm4_dwt_delta;
    volatile uint32_t cm7_dwt_ok;
    volatile uint32_t cm7_dwt_delta;

    /* B1 Raw Timing Baseline (in CPU cycles) */
    volatile uint32_t cm4_raw_fast_take_cycles;
    volatile uint32_t cm4_raw_take_cycles;
    volatile uint32_t cm4_raw_release_cycles;

    volatile uint32_t cm7_raw_fast_take_cycles;
    volatile uint32_t cm7_raw_take_cycles;
    volatile uint32_t cm7_raw_release_cycles;

    /* B1 Retention Times in prj_04 */
    volatile uint32_t hsem3_max_retention_cycles;
    volatile uint32_t hsem4_max_retention_cycles;
} HsemTestShared_t;

#define HSEM_TEST_SHARED   ((volatile HsemTestShared_t *)HSEM_TEST_SHARED_ADDR)

#endif /* HSEM_SELFTEST_SHARED_H */
