/**
  ******************************************************************************
  * @file    i2c_selftest.h
  * @brief   05_i2c selftest (preset selftest-i2c): entry points, the SRAM4
  *          block the two cores coordinate through, the bench step table and
  *          the line rules both cores' bench_claim() apply
  *          (Common/Src/i2c_selftest_common.c).
  *
  *          Built only with I2C_DRIVER_SELFTEST. Plan, bench and the meaning
  *          of every test: 01_shared_drivers/05_i2c/IMPLEMENTATION_PLAN.md
  *          ("Banco de pruebas", S15-S21); wiring of the AD2 and rules S1-S10:
  *          01_shared_drivers/00_gpio/README.md section 8. The host side is
  *          tools/i2c_hw_check.py; the I2C target is the NUCLEO-F334R8 running
  *          tools/i2c_target_f334 (workspace root).
  ******************************************************************************
  */

#ifndef I2C_SELFTEST_H
#define I2C_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Entry points ---------------------------------------------------------- */
void I2C_Selftest_CM7_PreRelease(void);   /* CPU1, before HSEM_BM_BootReleaseCpu2           */
void I2C_Selftest_CM7_Run(void);          /* CPU1, from Status_Task                         */
void I2C_Selftest_CM4_Run(void);          /* CPU2, after Shared_GpioLockInstall() and
                                             CPU2_PublishBuild()                            */

/* ---- Step table (S4, S7, S15, S16) ------------------------------------------- *
 * One table for both cores, so CPU2's bench_claim() judges a claim against the
 * same permissions CPU1 announced. I2C_BENCH_VERSION is the version of the bench
 * protocol: printed in the banner and in every STEP line, and checked by
 * tools/i2c_hw_check.py, which keeps its own copy of the step names and their
 * bus-pin owners. Bump it when any of those change. From v1 on, a step that
 * needs the host to read a state holds it until a HOST_REQ pulse
 * (sync_ack_low(), 06_spi lesson B1).                                         */
#define I2C_BENCH_VERSION    1U    /* v1: T0A, T0C, T0B, T0B_CM7, T0B_TIMEOUT (plan phase F0) */

typedef enum
{
    IST_OWN_NONE = 0,      /* nobody may take PB8/PB9 out of input                   */
    IST_OWN_CM7,           /* CPU1 may (I2C4, AF6), and no other core                */
    IST_OWN_CM4            /* CPU2 may (I2C1, AF4), and no other core                */
} IstOwner_t;

typedef struct
{
    const char *name;
    uint8_t     owner;     /* IstOwner_t: the one core that may take PB8/PB9 (S16)        */
    uint8_t     af;        /* 1: that core may put them in its I2C AF, open-drain, no pull */
    uint8_t     od_out;    /* 1: that core may make them GPIO open-drain outputs (bus
                              recovery, F1); 0 in every F0 step                           */
} IstStep_t;

extern const IstStep_t k_ist_steps[];
extern const uint32_t  k_ist_step_count;

#define IST_STEP_NONE        0xFFFFFFFFUL   /* between steps: nothing wired may be claimed */

/* Index of `name` in k_ist_steps, or IST_STEP_NONE. */
uint32_t ist_step_index(const char *name);

/* ---- Line rules (S4, S13, S14 inherited; S15, S16) ---------------------------- *
 * True when the pin may be configured with this mode (GPIO_BM_MODE_*), alternate
 * function, output type (GPIO_BM_OTYPE_*) and pull (GPIO_BM_PULL_*) on `core`
 * (0 = CPU1, 1 = CPU2) during step `step` (an index, or IST_STEP_NONE). Pure:
 * reads only its arguments and k_ist_steps.
 *   PB8/PB9 (the bus): never a pull and never push-pull (S15); input always;
 *     AF only on the step owner's core, in its I2C AF (CPU1 AF6 = I2C4, CPU2
 *     AF4 = I2C1); GPIO open-drain output only where od_out = 1 (S16).
 *   PF14/PF15 (scope taps on the bus through J2/J3, and I2C4's AF4 pins):
 *     input without pull only (S15).
 *   Any other pin: input (with any pull: T0A pulls the AD2 lines), never an
 *     output or AF in this bench (S4; S13: SPI4 never; S14: J1 never).
 * It does NOT read the hardware: each core's bench_claim() must still check
 * its own I2C instance (clocked in its RCC bank, PE = 0) and the present state
 * of both bus pins (ist_s16_pins_ok()) before it accepts an AF claim.          */
bool ist_line_rule(uint32_t core, uint32_t step, const GPIO_TypeDef *port, uint32_t pin,
                   uint32_t mode, uint32_t af, uint32_t otype, uint32_t pull);

/* S16, the pin half: true when neither PB8 nor PB9 is configured for anything
   but `af` right now: each is input, analog (the reset state), or AF `af`. A
   pin in another AF, or a GPIO output, belongs to someone else. Pure: the
   caller passes GPIOB MODER and AFR[1]. */
bool ist_s16_pins_ok(uint32_t moder, uint32_t afrh, uint32_t af);

/* Bench AF per pin (DS12923 table 9, 00_docs_stm32h7/STM32H755_Alternate_Functions.md). */
#define IST_AF_I2C1          4U    /* PB8 I2C1_SCL, PB9 I2C1_SDA: CPU2                  */
#define IST_AF_I2C4          6U    /* PB8 I2C4_SCL, PB9 I2C4_SDA: CPU1                  */
#define IST_PIN_SCL          8U    /* PB8, CN7-2 (D15)                                  */
#define IST_PIN_SDA          9U    /* PB9, CN7-4 (D14)                                  */
#define IST_PIN_TAP_SCL      14U   /* PF14, CN9-19: scope 1+, 22 kOhm to GND, via J2    */
#define IST_PIN_TAP_SDA      15U   /* PF15, CN9-21: scope 2+, 22 kOhm to 3V3, via J3    */
#define IST_BUS_MASK         ((1UL << IST_PIN_SCL) | (1UL << IST_PIN_SDA))

/* The S16 pin-state cases both cores run on ist_s16_pins_ok() with register
   images written by hand (PB9 in AF6 while PB8 is claimed in AF4, ...): bit k =
   case k accepted. Cases 0..3 must be refused, 4..6 accepted. */
#define IST_S16_CASES          7U
#define IST_S16_CASES_EXPECT   0x00000070UL
uint32_t ist_s16_cases(uint32_t af, uint32_t other_af);   /* bit k = case k accepted */

/* The I2C instance states T0b puts the AF owner in (I2C1 on CPU2, I2C4 on
   CPU1): bit k = case k accepted. Cases 0..1 must be refused (PE = 1, clock
   off in the core's bank), cases 2..3 accepted (PE = 0 with ANFOFF, reset). */
#define IST_I2C_CASES          4U
#define IST_I2C_CASES_EXPECT   0x0000000CUL

/* ---- SRAM4 block (outside g_shared, fixed address) ------------------------------ *
 * 0x3800D000, 4 KB: inside the RAM_SHARED region of both linker scripts
 * (0x38008000, 32 KB) and inside MPU region 7 (0x38000000, 64 KB, non-cacheable
 * on CPU1). Above g_shared (0x38008000) and the blocks of 00_gpio (0x3800A000),
 * 03_tim (0x3800B000) and 06_spi (0x3800C000), 4 KB each, so no two selftests
 * share a byte. Fixed rather than linked, so both images agree on it without
 * depending on object order.                                                     */
#define IST_AREA_ADDR        0x3800D000UL
#define IST_AREA_MAGIC       0x49324330UL   /* "I2C0" */

typedef enum
{
    IST_CMD_NONE = 0,
    IST_CMD_CLAIMS,      /* arg0 = slot of m4_claims[] (IST_M4_SLOT_*)                         */
    IST_CMD_I2C1,        /* T0b: the I2C1 states of IST_I2C_CASES against a claim of PB8 AF4 */
    IST_CMD_S16,         /* T0b: the S16 pin-state cases on ist_s16_pins_ok()                */
    IST_CMD_AF_ON,       /* T0b: PB8/PB9 claimed and written AF4 open-drain, I2C1 PE = 0     */
    IST_CMD_SAFE,        /* T0b: CPU2's bench_safe(), then a photo                            */
    IST_CMD_DONE
} IstCmd_t;

/* CPU2's claim cases (T0b). The expected bit patterns are written by hand in
   IST_M4_CLAIMS_EXPECT_*: bit k = case k accepted. CPU1 judges them. */
#define IST_M4_CLAIM_CASES           20U
#define IST_M4_SLOT_T0B              0U             /* step T0B: the bus belongs to CPU2      */
#define IST_M4_SLOT_CM7              1U             /* step T0B_CM7: it belongs to CPU1       */
#define IST_M4_SLOT_NONE             2U             /* between steps                          */
#define IST_M4_SLOTS                 3U
#define IST_M4_CLAIMS_EXPECT_T0B     0x00080007UL   /* cases 0..2 (AF4 OD) and 19 (input)     */
#define IST_M4_CLAIMS_EXPECT_CM7     0x00080000UL   /* only the plain input (case 19)         */
#define IST_M4_CLAIMS_EXPECT_NONE    0x00080000UL   /* idem                                   */

typedef struct
{
    uint32_t cases;           /* claims evaluated                                   */
    uint32_t accepted;        /* bit k = case k accepted                            */
    uint32_t regs_changed;    /* bit k: register k of the GPIOA/GPIOB/GPIOE/GPIOF
                                 snapshot (MODER, OTYPER, OSPEEDR, PUPDR, AFR[0],
                                 AFR[1] per port) differs                            */
    uint32_t step_seen;       /* the step index CPU2 judged against                 */
} IstClaimResult_t;

typedef struct
{
    uint32_t cases;           /* IST_I2C_CASES                                       */
    uint32_t accepted;        /* bit k = case k accepted                             */
    uint32_t pe_seen;         /* PE read back 1 in the PE = 1 case                   */
    uint32_t regs_changed;    /* GPIO snapshot registers that differ                 */
    uint32_t cr1_after;       /* I2C1 CR1 at the end (0)                             */
    uint32_t clk_same;        /* its clock in CPU2's bank as before                  */
} IstI2cCaseResult_t;

typedef struct
{
    uint32_t cases;           /* IST_S16_CASES                                       */
    uint32_t accepted;
} IstS16Result_t;

/* A photo of the two bus pins: MODER, OTYPER, PUPDR and AFR fields of PB8 in
   the low bits and of PB9 above them (2/1/2/4 bits per pin), IDR of both (bit 0
   PB8, bit 1 PB9), and the owner instance (CR1, ISR). */
typedef struct
{
    uint32_t claim_ok;
    uint32_t moder;           /* PB8 | PB9 << 2                                      */
    uint32_t otyper;          /* PB8 | PB9 << 1                                      */
    uint32_t pupdr;           /* PB8 | PB9 << 2                                      */
    uint32_t afr;             /* PB8 | PB9 << 4                                      */
    uint32_t idr;             /* PB8 | PB9 << 1                                      */
    uint32_t i2c_cr1;
    uint32_t i2c_isr;
    uint32_t clk_same;        /* SAFE: I2C1 clock in CPU2's bank as before T0b       */
} IstPinPhoto_t;

typedef struct
{
    volatile uint32_t magic;
    volatile uint32_t session;
    volatile uint32_t version;         /* I2C_BENCH_VERSION of CPU1's table           */
    volatile uint32_t step;            /* current step index, IST_STEP_NONE between   */

    volatile char     m4_rev[24];      /* FW_GIT_REV of the CPU2 image                */
    volatile char     m4_build_id[24]; /* FW_BUILD_ID of the CPU2 image               */
    volatile uint32_t m4_version;      /* I2C_BENCH_VERSION of CPU2's table           */
    volatile uint32_t m4_ready;        /* = session once CPU2 serves commands         */

    volatile uint32_t cmd;
    volatile uint32_t arg0;
    volatile uint32_t cmd_seq;         /* CPU1 increments after writing cmd/args      */
    volatile uint32_t ack_seq;         /* CPU2 copies cmd_seq when done               */
    volatile uint32_t m4_timeout;      /* CPU2 gave up waiting for a command          */
    volatile uint32_t m4_aborts;       /* CPU2 bench_abort() calls                    */
    volatile uint32_t m4_stalls;       /* CPU2 waits ended because CYCCNT stopped     */

    /* CPU2 results */
    IstClaimResult_t   m4_claims[IST_M4_SLOTS];
    IstI2cCaseResult_t m4_i2c1;
    IstS16Result_t     m4_s16;
    IstPinPhoto_t      m4_af;          /* after IST_CMD_AF_ON                          */
    IstPinPhoto_t      m4_safe;        /* after IST_CMD_SAFE                           */
} IstArea_t;

_Static_assert(sizeof(IstArea_t) <= 0x1000U, "IstArea_t no longer fits its 4 KB slot");

#define IST_AREA   ((IstArea_t *)IST_AREA_ADDR)

#ifdef __cplusplus
}
#endif

#endif /* I2C_SELFTEST_H */
