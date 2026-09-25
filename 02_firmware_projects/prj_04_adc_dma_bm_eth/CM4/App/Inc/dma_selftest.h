/**
  ******************************************************************************
  * @file    dma_selftest.h
  * @brief   Bare-metal and LL DMA driver self-test for Cortex-M4 (prj_04).
  *          Tests T1-T7 described in 01_shared_drivers/02_dma/README.md section 9.
  ******************************************************************************
  */

#ifndef DMA_SELFTEST_H
#define DMA_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t pass_mask;
    uint32_t t1_passed;
    uint32_t t2_passed;
    uint32_t t3_passed;
    uint32_t t4_tc_count;
    uint32_t t4_ht_count;
    uint32_t t5_passed;
    uint32_t t6_passed;
    uint32_t t7_observed_te;
} DmaSelfTest_Results_t;

extern DmaSelfTest_Results_t g_dma_selftest_results;

/**
  * @brief  Runs DMA self-tests on CM4 before ADC acquisition starts.
  */
void DMA_SelfTest_CM4_Run(void);

/**
  * @brief  Polls and posts formatted test result lines to CPU1 mailbox.
  */
void DMA_SelfTest_CM4_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* DMA_SELFTEST_H */
