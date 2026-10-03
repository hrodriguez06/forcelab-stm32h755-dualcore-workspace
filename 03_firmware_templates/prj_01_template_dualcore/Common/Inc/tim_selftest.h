/**
  ******************************************************************************
  * @file    tim_selftest.h
  * @brief   03_tim selftest (preset selftest-tim): entry points, the SRAM4
  *          block the two cores coordinate through, the bench step table and
  *          the line rules both cores' bench_claim() apply
  *          (Common/Src/tim_selftest_common.c).
  *
  *          Built only with TIM_DRIVER_SELFTEST. Plan, bench and the meaning
  *          of every test: 01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md;
  *          wiring and rules S1-S10: 01_shared_drivers/00_gpio/README.md
  *          section 8. The host side is tools/tim_hw_check.py.
  ******************************************************************************
  */

#ifndef TIM_SELFTEST_H
#define TIM_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Entry points ---------------------------------------------------------- */
void TIM_Selftest_CM7_PreRelease(void);   /* CPU1, before HSEM_BM_BootReleaseCpu2           */
void TIM_Selftest_CM7_Run(void);          /* CPU1, from Status_Task                         */
void TIM_Selftest_CM4_Run(void);          /* CPU2, after Shared_GpioLockInstall() and
                                             CPU2_PublishBuild()                            */

/* ---- Step table (S4, S7) ------------------------------------------------------ *
 * One table for both cores, so CPU2's bench_claim() judges a claim against the
 * same permissions CPU1 announced. TIM_BENCH_VERSION is the version of the bench
 * protocol: printed in the banner and in every STEP line, and checked by
 * tools/tim_hw_check.py, which keeps its own copy of the step names, their bus
 * owners and their handshakes. Bump it when any of those change.             */
#define TIM_BENCH_VERSION    5U    /* v2: TREF_A + TREF_B (plan R3-3); v3: the F1 steps,
                                      MCO1 on PA8 in T6/T8 (plan R3-2); v4: the F2 steps
                                      T6_TIM1_LL, T6_TIM15_LL and T8_LL (tim_ll); v5: the F3
                                      steps T6_TIM1_HAL, T8_HAL and T9_HAL (tim_hal) */

typedef enum
{
    TST_BUS_NONE = 0,      /* nobody drives PE8..PE15                         */
    TST_BUS_MCU,           /* the MCU may drive the bus pins the step lists   */
    TST_BUS_AD2            /* the AD2 may drive the bus (DIO1/DIO3 only, S11) */
} TstBus_t;

typedef struct
{
    const char *name;
    uint8_t     bus;       /* TstBus_t                                                   */
    uint16_t    out_pe;    /* GPIOE pins the MCU may make GPIO outputs in this step      */
    uint16_t    af_pe;     /* GPIOE pins the MCU may put in their bench AF in this step  */
    uint16_t    af_pa;     /* GPIOA pins ditto (PA8 = MCO1, AF0)                         */
    uint16_t    af1_pa;    /* GPIOA pins the MCU may put in AF1 (PA8 = TIM1_CH1: the TREF_A
                              control, TIM1 stopped, so nothing drives the pin)          */
} TstStep_t;

extern const TstStep_t k_tst_steps[];
extern const uint32_t  k_tst_step_count;

#define TST_STEP_NONE        0xFFFFFFFFUL   /* between steps: nothing wired may be claimed */

/* Index of `name` in k_tst_steps, or TST_STEP_NONE. */
uint32_t tst_step_index(const char *name);

/* ---- Line rules (00_gpio README section 8; 03_tim plan "Uso de las lineas") ---- *
 * True when the pin may become an output (mode GPIO_BM_MODE_OUTPUT) or take
 * alternate function `af` (mode GPIO_BM_MODE_AF) on `core` (0 = CPU1, 1 = CPU2)
 * during step `step` (an index, or TST_STEP_NONE). Pure: reads only its
 * arguments and k_tst_steps. It does NOT cover S12: in a "bus = AD2" step it
 * accepts PE9/PE11 in AF1 only as candidates, and CPU1's bench_claim() must
 * still read TIM1 back before it accepts them.                                  */
bool tst_line_rule(uint32_t core, uint32_t step, const GPIO_TypeDef *port, uint32_t pin,
                   uint32_t mode, uint32_t af);

/* ---- F1 oracles both cores use (Common/Src/tim_selftest_common.c) --------------- *
 * Written by hand from RM0399 rev 4, never derived from tim_bm: the register maps
 * (which registers an instance has and their reset values, T14), the capability
 * table of plan rule 2 (T2 compares GetCaps with it) and the field encodings of the
 * register-image matrix (T2). Each core runs them on the instances it owns.        */
#include "tim_bm.h"

enum { TST_K_ADV, TST_K_GP32, TST_K_GP16, TST_K_BASIC, TST_K_T12, TST_K_T1314, TST_K_T15, TST_K_T1617 };

#define TST_RI_N        27U        /* words 0x00..0x68 of TIM_TypeDef                */
#define TST_RI_CR1       0U
#define TST_RI_CR2       1U
#define TST_RI_SMCR      2U
#define TST_RI_DIER      3U
#define TST_RI_SR        4U
#define TST_RI_EGR       5U        /* write-only: never read                         */
#define TST_RI_CCMR1     6U
#define TST_RI_CCMR2     7U
#define TST_RI_CCER      8U
#define TST_RI_CNT       9U
#define TST_RI_PSC      10U
#define TST_RI_ARR      11U
#define TST_RI_RCR      12U
#define TST_RI_CCR1     13U
#define TST_RI_BDTR     17U
#define TST_RI_DCR      18U
#define TST_RI_DMAR     19U        /* reading it is a DMA burst access: never read  */
#define TST_RI_CCMR3    21U
#define TST_RI_AF1      24U
#define TST_RI_AF2      25U
#define TST_RI_TISEL    26U

typedef struct { uint32_t v[TST_RI_N]; } TstImg_t;

/* Kind of an instance (TST_K_*), -1 if it is not a timer. */
int32_t  tst_kind(const TIM_TypeDef *tim);
/* Bit i = the register at offset 4i exists on that kind (RM0399 register maps). */
uint32_t tst_present(int32_t kind);
/* Reset values and the bits compared (0 where the register does not exist). */
void     tst_reset_image(int32_t kind, TstImg_t *value, TstImg_t *mask);
/* Raw read of the instance's registers (EGR, DMAR and the reserved word read as 0). */
void     tst_img_read(const TIM_TypeDef *tim, TstImg_t *img);
/* T14: the instance reads its reset image. On a mismatch: the word index, got, expected. */
bool     tst_reset_ok(const TIM_TypeDef *tim, uint32_t *bad_idx, uint32_t *got, uint32_t *exp);
/* The capability table typed by hand (plan rule 2, RM0399; TIM1/TIM8 TI1SEL as the RM). */
bool     tst_hand_caps(const TIM_TypeDef *tim, TimBm_Caps_t *caps);

/* ---- The tier under test (F2, F3) ------------------------------------------------ *
 * The tests take tim_bm's configurations and values; a tier applies them. BM calls
 * tim_bm. LL translates each value to the constant of stm32h7xx_ll_tim.h with its own
 * table, typed by hand from that header (never from tim_ll.c), and calls tim_ll; a
 * value with no LL constant becomes a word no LL constant has, which tim_ll must
 * refuse. Event masks are the same bits in both (LL_TIM_DIER_* = TIM_BM_EV_*). The HAL
 * tier (F3, CPU1 only: tim_hal on a TIM_HandleTypeDef) is in CM7/App/Src/tim_selftest.c. */
typedef struct
{
    const char *name;                     /* "BM" or "LL"                              */
    const char *tag;                      /* "" or " LL": inserted in the test's lines */
    TimBm_Status_t (*init_tb)(const TimBm_TimeBaseConfig_t *cfg);
    TimBm_Status_t (*start)(TIM_TypeDef *tim);
    TimBm_Status_t (*stop)(TIM_TypeDef *tim);
    TimBm_Status_t (*deinit)(TIM_TypeDef *tim);
    TimBm_Status_t (*enable_irq)(TIM_TypeDef *tim, uint32_t ev_mask);
    TimBm_Status_t (*disable_irq)(TIM_TypeDef *tim, uint32_t ev_mask);
    uint32_t       (*take_flags)(TIM_TypeDef *tim, uint32_t ev_mask);
    uint32_t       (*take_ovr)(TIM_TypeDef *tim, uint32_t ch_mask);
    TimBm_Status_t (*wait_flags)(TIM_TypeDef *tim, uint32_t ev_mask, uint32_t max_cycles, uint32_t *taken);
    TimBm_Status_t (*config_output)(const TimBm_OcConfig_t *cfg);
    TimBm_Status_t (*output_enable)(TIM_TypeDef *tim, uint32_t ch_mask);
    TimBm_Status_t (*output_disable)(TIM_TypeDef *tim, uint32_t ch_mask);
    TimBm_Status_t (*config_input)(const TimBm_IcConfig_t *cfg);
    TimBm_Status_t (*config_pwm_input)(TIM_TypeDef *tim, uint32_t ti, uint32_t edge, uint32_t prescaler,
                                       uint32_t filter, uint32_t tisel);
    TimBm_Status_t (*capture_enable)(TIM_TypeDef *tim, uint32_t ch_mask);
    TimBm_Status_t (*capture_disable)(TIM_TypeDef *tim, uint32_t ch_mask);
    uint32_t       (*read_capture)(TIM_TypeDef *tim, uint32_t channel);
    TimBm_Status_t (*config_master)(TIM_TypeDef *tim, uint32_t mms, bool msm);
    TimBm_Status_t (*config_slave)(TIM_TypeDef *tim, uint32_t sms, uint32_t trigger);
} TstTier_t;

extern const TstTier_t k_tst_tier_bm;
extern const TstTier_t k_tst_tier_ll;

/* T2: one configuration of the matrix, handed to the core's hook after the driver
   applied it (CPU1 rebuilds it through the raw HAL and lists the differences). */
enum { TST_T2_TB = 0, TST_T2_OC, TST_T2_IC, TST_T2_MS, TST_T2_SL, TST_T2_CLASSES };
typedef struct
{
    uint32_t               cls;
    TimBm_TimeBaseConfig_t tb;
    TimBm_OcConfig_t       oc;
    TimBm_IcConfig_t       ic;
    uint32_t               mms;
    bool                   msm;
    uint32_t               sms;
    uint32_t               trg;
} TstT2Cfg_t;
typedef void (*TstT2Hook_t)(const TstT2Cfg_t *cfg, const TstImg_t *bm);

typedef struct
{
    uint32_t configs;          /* configurations applied                         */
    uint32_t mismatches;       /* images that differ from the hand encoding      */
    uint32_t calls_bad;        /* driver calls that did not return TIM_BM_OK     */
    uint32_t first_cfg;        /* first mismatch: configuration index, word, got, expected */
    uint32_t first_idx;
    uint32_t first_got;
    uint32_t first_exp;
    uint32_t first_cls;
    uint32_t ctrl_detected;    /* the image altered on purpose was flagged        */
    uint32_t per_class[TST_T2_CLASSES];
    /* Parity (F2, tier != BM): the same configuration applied again through tim_bm, and
       every word but CNT compared bit for bit with the tier's image.               */
    uint32_t par_configs;
    uint32_t par_mismatches;
    uint32_t par_first_cfg;
    uint32_t par_first_idx;
    uint32_t par_first_tier;   /* the word in the tier's image                      */
    uint32_t par_first_bm;     /* ... and in tim_bm's                               */
    uint32_t par_ctrl;         /* a tim_bm image altered on purpose was flagged     */
} TstT2Result_t;

/* Runs the whole matrix on `tim` (owned by the calling core, clock on) through `tier`,
   Deinit before each configuration and at the end. `hook` may be NULL. With a tier
   other than BM, each image is also compared with tim_bm's (parity).            */
void tst_t2_run(TIM_TypeDef *tim, const TstTier_t *tier, TstT2Hook_t hook, TstT2Result_t *res);

/* T6 points, one table for TIM1 (CPU1) and TIM15 (CPU2); the host keeps its own copy
   and checks each announcement. Edge-aligned: period (PSC+1)(ARR+1) ticks, high CCR
   (PSC+1) (PWM1); center 1: period 2 ARR (PSC+1), high 2 CCR (PSC+1). P8 is TIM1 only. */
typedef struct { uint16_t psc; uint16_t arr; uint8_t mode; uint16_t ccr; uint8_t low; uint8_t center; } TstT6Point_t;
#define TST_T6_POINTS   9U
extern const TstT6Point_t k_tst_t6[TST_T6_POINTS];
/* Deinit, time base, the channel, OutputEnable, Start, through `tier`. */
TimBm_Status_t tst_t6_apply(const TstTier_t *tier, TIM_TypeDef *tim, uint32_t ch, const TstT6Point_t *p);

/* Bench AF per wired pin (DS12923 table 9, STM32H755_Alternate_Functions.md). */
#define TST_AF_TIM1          1U    /* PE9, PE11, PE13, PE14: TIM1_CH1..CH4 */
#define TST_AF_TIM15         4U    /* PE5, PE6: TIM15_CH1, TIM15_CH2       */
#define TST_AF_MCO1          0U    /* PA8: MCO1 (in AF1, TIM1_CH1: TREF_A control) */

/* ---- SRAM4 block (outside g_shared, fixed address) ------------------------------ *
 * 0x3800B000, 4 KB: inside the RAM_SHARED region of both linker scripts
 * (0x38008000, 32 KB) and inside MPU region 7 (0x38000000, 64 KB, non-cacheable
 * on CPU1). Above g_shared (0x38008000, under 256 B) and above the 00_gpio block
 * (0x3800A000, 4 KB), so the two selftests never share a byte. Fixed rather than
 * linked, so both images agree on it without depending on object order.        */
#define TST_AREA_ADDR        0x3800B000UL
#define TST_AREA_MAGIC       0x54494D30UL   /* "TIM0" */

typedef enum
{
    TST_CMD_NONE = 0,
    TST_CMD_CLAIMS,      /* arg0 = 0: during step T0B; 1: between steps (nothing allowed) */
    TST_CMD_SAFE,        /* claim PE5 AF4 (step T0B), write it, then CPU2's bench_safe()   */
    TST_CMD_DONE,
    /* F1 (tim_bm on CPU2: TIM15, and TIM2's RCC bit in CPU2's bank for T16) and F2
       (tim_ll on CPU2: arg0 = TST_TIER_LL for T1, T2, T5, T12 and B1; bit TST_T6_LL of
       arg0 for T6). Each writes m4_res[]; the layout is the TST_R_* list below.    */
    TST_CMD_T1,          /* TIM15 refusals and their snapshot                              */
    TST_CMD_T2,          /* TIM15 register-image matrix against the hand table             */
    TST_CMD_T5,          /* TIM15 time base against CPU2's CYCCNT                          */
    TST_CMD_T6,          /* arg0 = point index, TST_T6_CONTROL or TST_T6_OFF (PE5/PE6)     */
    TST_CMD_T10D_ARM,    /* TIM15 captures TRC = ITR0 (TIM1 TRGO)                          */
    TST_CMD_T10D_READ,   /* read the captures armed above, then Deinit                     */
    TST_CMD_T12,         /* TIM15 update interrupts, spurious IRQ, control                  */
    TST_CMD_T16,         /* arg0 = 0: DisableClock(TIM2); 1: EnableClock then DisableClock  */
    TST_CMD_B1           /* cycles of TakeFlags, OutputEnable, ConfigOutput, InitTimeBase   */
} TstCmd_t;

#define TST_TIER_BM          0U
#define TST_TIER_LL          1U
#define TST_T6_CONTROL       0xFEU   /* TIM15 CC1E = CC2E = 0: pins follow the pull-down */
#define TST_T6_OFF           0xFFU   /* Deinit, PE5/PE6 back to input                    */
#define TST_T6_LL            (1UL << 16)   /* T6 through tim_ll                          */

/* CPU2's T0b(b) refusals. The expected bit pattern is written by hand in
   TST_M4_CLAIMS_EXPECT_*: bit k = case k accepted. CPU1 judges it. */
#define TST_M4_CLAIM_CASES           13U
#define TST_M4_CLAIMS_EXPECT_T0B     0x00000001UL   /* only case 0 (PE5 AF4) in step T0B */
#define TST_M4_CLAIMS_EXPECT_NONE    0x00000000UL   /* nothing between steps              */
/* O1: PE5 input -> AF4, no pull, low speed, push-pull changes GPIOE MODER (6) and
   AFR[0] (10) of the snapshot, and nothing else. */
#define TST_M4_SAFE_CHANGED          ((1UL << 6) | (1UL << 10))

/* ---- F1 results of CPU2 (m4_res[]) --------------------------------------------- */
#define TST_M4_RES_WORDS     96U
/* TST_CMD_T1 */
#define TST_R_T1_CASES       0U    /* refusals evaluated                                */
#define TST_R_T1_BAD         1U    /* bit k: case k returned another status             */
#define TST_R_T1_TOUCHED     2U    /* bit k: case k changed the TIM15/RCC snapshot      */
#define TST_R_T1_CTRL_ST     3U    /* control: status of the valid call                 */
#define TST_R_T1_CTRL_DIFF   4U    /* control: snapshot registers it changed            */
#define TST_R_T1_CTRL_EXP    5U    /* control: the registers it must change             */
#define TST_R_T1_STATUS0     8U    /* [8 + k]: status of case k                         */
/* TST_CMD_T2 */
#define TST_R_T2_CONFIGS     0U
#define TST_R_T2_MISMATCH    1U
#define TST_R_T2_FIRST_CFG   2U    /* index of the first mismatch                        */
#define TST_R_T2_FIRST_REG   3U    /* register offset, got, expected                     */
#define TST_R_T2_FIRST_GOT   4U
#define TST_R_T2_FIRST_EXP   5U
#define TST_R_T2_CTRL        6U    /* 1: the altered image was detected                  */
#define TST_R_T2_CALLS_BAD   7U    /* driver calls that did not return OK                */
#define TST_R_T2_PAR_CONFIGS 8U    /* F2, tim_ll: images compared with tim_bm's          */
#define TST_R_T2_PAR_MISMATCH 9U
#define TST_R_T2_PAR_FIRST_CFG 10U
#define TST_R_T2_PAR_FIRST_REG 11U
#define TST_R_T2_PAR_FIRST_LL 12U
#define TST_R_T2_PAR_FIRST_BM 13U
#define TST_R_T2_PAR_CTRL    14U   /* 1: the altered tim_bm image was detected           */
/* TST_CMD_T5: [0..5] (a) ticks, updates, cycles; (b) ticks, updates, cycles;
   [6] GetKernelClockHz; [7] its status; [8] T14 after it; [9] HAL_RCC_GetHCLKFreq */
/* TST_CMD_T6: [0] status of the configuration (0 = OK), [1] T14 after OFF          */
/* TST_CMD_T10D_*: [0] arm status, [1] captures taken, [2..5] CCR1 values, [6] T14  */
/* TST_CMD_T12: [0] update IRQs in the window, [1] window cycles, [2] spurious IRQs
   (b), [3] spurious IRQs, control (b), [4] IRQs with UIE = 0, [5] T14, [6] IRQn ok */
/* TST_CMD_T16: [0] status, [1] RCC_C1->APB1LENR, [2] RCC_C2->APB1LENR (after), [3]
   RCC_C2->APB1LENR between EnableClock and DisableClock (arg0 = 1)                 */
/* TST_CMD_B1: [0..3] cycles of TakeFlags, OutputEnable, ConfigOutput, InitTimeBase */

typedef struct
{
    uint32_t cases;           /* claims evaluated                                   */
    uint32_t accepted;        /* bit k = case k accepted                            */
    uint32_t regs_changed;    /* bit k: register k of the GPIOA/GPIOE snapshot (MODER,
                                 OTYPER, OSPEEDR, PUPDR, AFR[0], AFR[1]) differs        */
    uint32_t step_seen;       /* the step index CPU2 judged against                 */
} TstClaimResult_t;

typedef struct
{
    uint32_t claim_ok;        /* PE5 AF4 accepted in step T0B                    */
    uint32_t ctrl_changed;    /* audit O1: snapshot registers the valid write changed
                                 (bit k as regs_changed), expected TST_M4_SAFE_CHANGED */
    uint32_t af_moder;        /* GPIOE MODER field of PE5 after the write (2 = AF) */
    uint32_t af_afr;          /* GPIOE AFR field of PE5 after the write          */
    uint32_t safe_moder;      /* ... after CPU2's bench_safe() (0 = input)        */
    uint32_t safe_afr;
    uint32_t safe_pupdr;
    uint32_t pe6_moder;       /* PE6 after bench_safe()                          */
} TstSafeResult_t;

typedef struct
{
    volatile uint32_t magic;
    volatile uint32_t session;
    volatile uint32_t version;         /* TIM_BENCH_VERSION of CPU1's table           */
    volatile uint32_t step;            /* current step index, TST_STEP_NONE between   */

    volatile char     m4_rev[24];      /* FW_GIT_REV of the CPU2 image                */
    volatile char     m4_build_id[24]; /* FW_BUILD_ID of the CPU2 image               */
    volatile uint32_t m4_version;      /* TIM_BENCH_VERSION of CPU2's table           */
    volatile uint32_t m4_ready;        /* = session once CPU2 serves commands         */

    volatile uint32_t cmd;
    volatile uint32_t arg0;
    volatile uint32_t arg1;
    volatile uint32_t cmd_seq;         /* CPU1 increments after writing cmd/args      */
    volatile uint32_t ack_seq;         /* CPU2 copies cmd_seq when done               */
    volatile uint32_t m4_timeout;      /* CPU2 gave up waiting for a command          */
    volatile uint32_t m4_aborts;       /* CPU2 bench_abort() calls                    */

    /* CPU2 results */
    TstClaimResult_t  m4_claims[2];    /* [0] step T0B, [1] between steps             */
    TstSafeResult_t   m4_safe;
    volatile uint32_t m4_res[TST_M4_RES_WORDS];   /* F1 commands (TST_R_* above)   */
} TstArea_t;

_Static_assert(sizeof(TstArea_t) <= 0x1000U, "TstArea_t no longer fits its 4 KB slot");

#define TST_AREA   ((TstArea_t *)TST_AREA_ADDR)

#ifdef __cplusplus
}
#endif

#endif /* TIM_SELFTEST_H */
