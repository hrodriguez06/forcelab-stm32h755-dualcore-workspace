/**
  ******************************************************************************
  * @file    adc_selftest_hw.h
  * @brief   Hardware tests T2, T3, T4 and T6 of the shared ADC driver, HAL
  *          tier, run on CPU2. Built only with -DADC_DRIVER_SELFTEST=ON.
  *
  *          What each test proves, and its pass criterion, is in
  *          01_shared_drivers/07_adc/README.md section 9.1:
  *
  *            T2   ADC3 converts VREFINT at 16, 14, 12, 10 and 8 bits plus the
  *                 two optimised modes. The code must scale as 2^(16-N) and
  *                 CFGR.RES must read back this revision's encoding.
  *            T3   ADC1 + DMA1_Stream0 triggered by TIM3 at
  *                 BOARD_ADC_SAMPLING_RATE_HZ for one second through
  *                 ADC_HAL_InitEx: HT and TC at 488.3 per second (+-1 %), no
  *                 error of any kind.
  *            T3b  ADC3 sequence {VREFINT, TEMPSENSOR} through DMA1_Stream1:
  *                 ADC_HAL_ComputeStatsChannel must separate the two ranks.
  *            T4   The DMA stream is stopped under a running ADC: the overrun
  *                 must land in adc_overrun_count and not in dma_error_count
  *                 (finding A5: HAL reports both through ErrorCallback).
  *            T6   ADC2 enabled, then ADC1 asks for another prescaler:
  *                 ADC_HAL_InitEx must refuse and leave ADC12_COMMON->CCR and
  *                 ADC2 untouched. The line also reports the BOOST ADC1 ended
  *                 up with against the one its real clock needs.
  *
  *          No interrupt is used: ADC_SelfTestHw_Run() runs before
  *          ADC_Acquisition_Init() and calls the HAL IRQ handlers by hand. It
  *          leaves ADC1/2/3, DMA1_Stream0/1 and TIM3 as it found them.
  ******************************************************************************
  */

#ifndef ADC_SELFTEST_HW_H
#define ADC_SELFTEST_HW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_STHW_RES_POINTS     7U   /* 16, 14, 12, 10, 8, 14-opt, 12-opt */

#define ADC_STHW_PASS_T2        (1UL << 0)
#define ADC_STHW_PASS_T3        (1UL << 1)
#define ADC_STHW_PASS_T3B       (1UL << 2)
#define ADC_STHW_PASS_T4        (1UL << 3)
#define ADC_STHW_PASS_T6        (1UL << 4)
#define ADC_STHW_PASS_ALL       0x1FUL

typedef struct
{
    uint32_t done;                 /* 0xADC0FEED once Run() has finished     */
    uint32_t pass;                 /* ADC_STHW_PASS_* bits                   */
    uint32_t rev_id;               /* DBGMCU REV_ID the run saw              */

    uint16_t t2_raw[ADC_STHW_RES_POINTS];       /* 0 = skipped/failed */
    uint8_t  t2_res_code[ADC_STHW_RES_POINTS];  /* CFGR.RES read back */

    uint32_t t3_elapsed_ms;
    uint32_t t3_ht;
    uint32_t t3_tc;
    uint32_t t3_dma_err;
    uint32_t t3_adc_ovr;
    uint32_t t3_half_ovr;
    uint16_t t3_avg;

    uint32_t t3b_halves;
    uint16_t t3b_avg[2];

    uint32_t t4_adc_ovr;
    uint32_t t4_dma_err;

    bool     t6_adc2_up;
    bool     t6_rejected;
    bool     t6_accepted;
    bool     t6_adc2_still_on;
    uint32_t t6_ccr_before;
    uint32_t t6_ccr_after;
    uint32_t t6_boost;             /* ADC1 CR.BOOST after the conflicting Init */
    uint32_t t6_boost_needed;      /* BOOST the clock actually in CCR needs     */
} AdcSelfTestHw_Results_t;

extern volatile AdcSelfTestHw_Results_t g_adc_selftest_hw;

/**
  * @brief  Runs T2, T3, T3b, T4 and T6 on the HAL tier. Blocking, about 1.5 s.
  *         Call once, after the calibration has been injected and before
  *         ADC_Acquisition_Init().
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
