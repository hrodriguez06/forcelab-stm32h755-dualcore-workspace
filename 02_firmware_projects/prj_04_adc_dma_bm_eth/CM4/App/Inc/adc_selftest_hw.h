/**
  ******************************************************************************
  * @file    adc_selftest_hw.h
  * @brief   Hardware tests T2, T3, T4 and T6 of the shared ADC driver, BM and
  *          LL tiers, run on CPU2. Built only with -DADC_DRIVER_SELFTEST=ON.
  *
  *          What each test proves, and its pass criterion, is in
  *          01_shared_drivers/07_adc/PLAN.md section 3.3 (phase 5):
  *
  *            T2   ADC3 converts VREFINT at 16, 14, 12, 10 and 8 bits, plus the
  *                 two optimised modes on Rev V. The code must scale as
  *                 2^(16-N), and CFGR.RES must read back the encoding for this
  *                 silicon revision (8-bit is 111b on Rev V, 100b on Rev Y).
  *            T3   ADC1 + DMA1_Stream0 triggered by TIM3 at
  *                 BOARD_ADC_SAMPLING_RATE_HZ for one second through InitEx:
  *                 HT and TC at 488.3 per second (+-1 %), no error of any kind.
  *            T3b  ADC3 sequence {VREFINT, TEMPSENSOR} through DMA1_Stream1:
  *                 ComputeStatsChannel must separate the two interleaved
  *                 channels. This is the multi-channel path (F3) with DMA.
  *            T4   The DMA stream is stopped under a running ADC: the overrun
  *                 must land in adc_overrun_count and not in dma_error_count.
  *            T6   ADC2 enabled, then ADC1 asks for another prescaler: InitEx
  *                 must refuse, and leave ADC12_COMMON->CCR and ADC2 untouched.
  *
  *          No interrupt is used. ADC_SelfTestHw_Run() runs before
  *          ADC_Acquisition_Init(), while DMA1_Stream0_IRQn and ADC_IRQn are
  *          still disabled in the NVIC, and polls the flags, calling the
  *          driver's own IRQ handlers by hand. It leaves every ADC, DMA stream
  *          and TIM3 as it found them, so the application starts clean.
  *
  *          Results go two ways: g_adc_selftest_hw, in this core's SRAM2, for
  *          a debugger; and one line per free mailbox slot for the CPU1
  *          console, posted by ADC_SelfTestHw_Poll() from the super-loop so
  *          the boot does not wait for CPU1 to print.
  ******************************************************************************
  */

#ifndef ADC_SELFTEST_HW_H
#define ADC_SELFTEST_HW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_STHW_TIER_BM        0U
#define ADC_STHW_TIER_LL        1U
#define ADC_STHW_TIERS          2U
#define ADC_STHW_RES_POINTS     7U   /* 16, 14, 12, 10, 8, 14-opt, 12-opt */

/* One bit per test and tier in AdcSelfTestHw_Results_t.pass */
#define ADC_STHW_PASS_T2(tier)  (1UL << (0U + (tier)))
#define ADC_STHW_PASS_T3(tier)  (1UL << (2U + (tier)))
#define ADC_STHW_PASS_T3B(tier) (1UL << (4U + (tier)))
#define ADC_STHW_PASS_T4(tier)  (1UL << (6U + (tier)))
#define ADC_STHW_PASS_T6(tier)  (1UL << (8U + (tier)))
#define ADC_STHW_PASS_G5        (1UL << 10U)
#define ADC_STHW_PASS_ALL       0x7FFUL

typedef struct
{
    uint32_t done;                 /* 0xADC0FEED once Run() has finished     */
    uint32_t pass;                 /* ADC_STHW_PASS_* bits                   */
    uint32_t rev_id;               /* DBGMCU REV_ID the run saw              */

    /* T2: VREFINT on ADC3, 16-sample average, per resolution point */
    uint16_t t2_raw[ADC_STHW_TIERS][ADC_STHW_RES_POINTS];      /* 0 = skipped/failed */
    uint8_t  t2_res_code[ADC_STHW_TIERS][ADC_STHW_RES_POINTS]; /* CFGR.RES read back */

    /* T3 (index = tier) */
    uint32_t t3_elapsed_ms[ADC_STHW_TIERS];
    uint32_t t3_ht[ADC_STHW_TIERS];
    uint32_t t3_tc[ADC_STHW_TIERS];
    uint32_t t3_dma_err[ADC_STHW_TIERS];
    uint32_t t3_adc_ovr[ADC_STHW_TIERS];
    uint32_t t3_half_ovr[ADC_STHW_TIERS];
    uint16_t t3_avg[ADC_STHW_TIERS];

    /* T3b: last averages of sequence rank 0 (VREFINT) and rank 1 (TEMPSENSOR) */
    uint32_t t3b_halves[ADC_STHW_TIERS];
    uint16_t t3b_avg[ADC_STHW_TIERS][2];

    /* T4 */
    uint32_t t4_adc_ovr[ADC_STHW_TIERS];
    uint32_t t4_dma_err[ADC_STHW_TIERS];

    /* T6 */
    bool     t6_adc2_up[ADC_STHW_TIERS];
    bool     t6_rejected[ADC_STHW_TIERS];
    bool     t6_accepted[ADC_STHW_TIERS];
    bool     t6_adc2_still_on[ADC_STHW_TIERS];
    uint32_t t6_ccr_before[ADC_STHW_TIERS];
    uint32_t t6_ccr_after[ADC_STHW_TIERS];

    /* G5: register comparison between BM and LL */
    bool     g5_match;
    uint32_t g5_diff_mask;
} AdcSelfTestHw_Results_t;

extern volatile AdcSelfTestHw_Results_t g_adc_selftest_hw;

/**
  * @brief  Runs T2, T3, T3b, T4 and T6 on the BM and LL tiers. Blocking, about
  *         2.5 s. Call once, after the calibration has been injected and
  *         before ADC_Acquisition_Init().
  */
void ADC_SelfTestHw_Run(void);

/**
  * @brief  Posts the next result line to g_shared.m4_to_m7 if CPU1 has already
  *         printed the previous one. Non-blocking; call from the super-loop.
  */
void ADC_SelfTestHw_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* ADC_SELFTEST_HW_H */
