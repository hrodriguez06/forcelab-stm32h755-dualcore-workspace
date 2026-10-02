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
#define TIM_BENCH_VERSION    1U

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
    uint16_t    af1_pa;    /* GPIOA pins the MCU may put in AF1 (PA8 = TIM1_CH1: the TREF
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

/* Bench AF per wired pin (DS12923 table 9, STM32H755_Alternate_Functions.md). */
#define TST_AF_TIM1          1U    /* PE9, PE11, PE13, PE14: TIM1_CH1..CH4 */
#define TST_AF_TIM15         4U    /* PE5, PE6: TIM15_CH1, TIM15_CH2       */
#define TST_AF_MCO1          0U    /* PA8: MCO1 (in AF1, TIM1_CH1: TREF control) */

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
    TST_CMD_DONE
} TstCmd_t;

/* CPU2's T0b(b) refusals. The expected bit pattern is written by hand in
   TST_M4_CLAIMS_EXPECT_*: bit k = case k accepted. CPU1 judges it. */
#define TST_M4_CLAIM_CASES           13U
#define TST_M4_CLAIMS_EXPECT_T0B     0x00000001UL   /* only case 0 (PE5 AF4) in step T0B */
#define TST_M4_CLAIMS_EXPECT_NONE    0x00000000UL   /* nothing between steps              */

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
} TstArea_t;

_Static_assert(sizeof(TstArea_t) <= 0x1000U, "TstArea_t no longer fits its 4 KB slot");

#define TST_AREA   ((TstArea_t *)TST_AREA_ADDR)

#ifdef __cplusplus
}
#endif

#endif /* TIM_SELFTEST_H */
