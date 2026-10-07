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
#include "i2c_bm.h"

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
#define I2C_BENCH_VERSION    2U    /* v1: T0A, T0C, T0B, T0B_CM7, T0B_TIMEOUT (plan phase F0);
                                      v2: + F1_NOPIN, F1_CM4, F1_CM7, F1_RECOVER (phase F1) */

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
    IST_CMD_DONE,
    /* Phase F1 (i2c_bm on I2C1). Arguments in arg0..arg3, results in m4_res[]. */
    IST_CMD_M4_PINS,     /* arg0 = 1: PB8/PB9 claimed and written AF4 open-drain (step F1_CM4);
                            0: bench_safe() of the pins -> res[0] = 1 if done              */
    IST_CMD_M4_INIT,     /* arg0 = TIMINGR, arg1 = analog | dnf << 8, arg2 = timeout_scl_low:
                            EnableClock + I2C_BM_Init(I2C1) -> res[0] status, res[1..6] =
                            CR1, CR2, OAR1, OAR2, TIMINGR, TIMEOUTR, res[7] ISR            */
    IST_CMD_M4_DEINIT,   /* I2C_BM_Deinit(I2C1) -> res[0] status, res[1..7] as M4_INIT     */
    IST_CMD_M4_XFER,     /* arg0 = IST_XF(op, mode, addr), arg1 = n, arg2 = seed (write) or
                            starting pointer of the target (read, write-read), arg3 =
                            max_cycles -> res[] IST_XR_*                                   */
    IST_CMD_M4_T1,       /* T1 refusals on I2C1, nothing initialised -> res[0] cases, res[1]
                            with another status, res[2] snapshot groups unchanged, res[3]
                            control changed, res[4..] the statuses (IST_T1_MAX)            */
    IST_CMD_M4_T2,       /* T2 images of I2C1 over the matrix -> res[0] cases, res[1] bad,
                            res[2] bad Deinit images, res[3] first bad case, res[4] got, res[5]
                            expected, res[6] control detected                             */
    IST_CMD_M4_T13,      /* arg0 = 1: DisableClock(I2C2); 2: EnableClock, DisableClock ->
                            res[0], res[1] statuses, res[2] RCC_C2 APB1LENR after          */
    IST_CMD_M4_FMP,      /* arg0 = 0/1: I2C_BM_SetFastModePlus(I2C1) -> res[0] status, res[1]
                            SYSCFG_PMCR after                                              */
    IST_CMD_M4_T12,      /* arg0 = IST_T12_*, arg1 = count, arg2 = max_cycles of each ->
                            res[0] OK, res[1] stalled, res[2] first other status, res[3]
                            cycles of the slowest                                         */
    IST_CMD_M4_B1,       /* -> res[0] cycles of Init, res[1] Write of 1 byte, res[2..3]
                            XferIrq min/max, res[4] status of the Write                     */
    IST_CMD_M4_BUSY,     /* T1 with BUSY = 1 (the target holds SDA) on I2C1 -> res[0] cases,
                            res[1] with another status, res[2] registers unchanged, res[3]
                            ISR, res[4..] the statuses                                     */
    IST_CMD_M4_REGS      /* -> res[0..7] = CR1, CR2, OAR1, OAR2, TIMINGR, TIMEOUTR, ISR, the
                            I2C1 clock in CPU2's bank                                      */
} IstCmd_t;

/* ---- Phase F1 transfers ---------------------------------------------------------- */
#define IST_OP_WRITE        0U
#define IST_OP_READ         1U
#define IST_OP_WRITEREAD    2U    /* one pointer byte, repeated start, n bytes read          */
#define IST_OP_PROBE        3U    /* I2C_BM_Write of 0 bytes                                  */
#define IST_MODE_POLL       0U
#define IST_MODE_IT         1U
#define IST_MODE_DMA        2U
#define IST_MODE_IT_CTRL    3U    /* T6 control: the interrupt enables cleared by hand        */
#define IST_MODE_DMA_CTRL   4U    /* T7 control: RXDMAEN cleared by hand (reads)              */
#define IST_XF(op, mode, addr)  (((uint32_t)(op)) | ((uint32_t)(mode) << 8) | ((uint32_t)(addr) << 16))
#define IST_XF_OP(x)        ((x) & 0xFFU)
#define IST_XF_MODE(x)      (((x) >> 8) & 0xFFU)
#define IST_XF_ADDR(x)      (((x) >> 16) & 0x7FU)

/* The result of one transfer (CPU2's m4_res[], and CPU1's own). */
#define IST_XR_STATUS       0U    /* I2cBm_Status_t                                          */
#define IST_XR_CRC          1U    /* CRC-32 (zlib) of the bytes written (W) or read (R, WR)   */
#define IST_XR_MISM         2U    /* bytes read that differ from the target's known content  */
#define IST_XR_FIRST_BAD    3U    /* index of the first, 0xFFFFFFFF if none                   */
#define IST_XR_CYCLES       4U    /* CYCCNT of the call (or StartIt to the end)               */
#define IST_XR_DONE_TX      5U    /* I2cBm_Xfer_t.done_tx (IT), else the bytes written        */
#define IST_XR_DONE_RX      6U
#define IST_XR_IRQS         7U    /* event + error IRQs during an IT transfer                 */
#define IST_XR_IRQS_AFTER   8U    /* in the 10 ms after the end (T6)                          */
#define IST_XR_CR1_AFTER    9U    /* CR1 after the end                                        */
#define IST_XR_SENTINEL     10U   /* bytes of the read buffer still holding the sentinel      */
#define IST_XR_ISR_AFTER    11U
#define IST_XR_AUX          12U   /* DMA: the steps' status (0 = all OK)                      */
#define IST_XR_N            13U

#define IST_T12_POLL_DRIVER  0U   /* I2C_BM_Write of 2 bytes (first byte preloaded)          */
#define IST_T12_DMA_DRIVER   1U   /* I2C_BM_DmaStartWrite of 2 bytes                          */
#define IST_T12_POLL_MANUAL  2U   /* by hand, no preload: START, then TXDR at each TXIS       */
#define IST_T12_DMA_MANUAL   3U   /* by hand, no preload: TXDMAEN, START, the stream sends 2  */

#define IST_T1_MAX           80U
#define IST_T1_CASES         62U   /* ist_t1()'s cases (O15: judged exactly)    */
#define IST_T1_BUSY_CASES    8U    /* ist_t1_busy()'s transfers                 */

/* The target (tools/i2c_target_f334): 7-bit address, and a free address. */
#define IST_TARGET_ADDR      0x42U
#define IST_NO_TARGET_ADDR   0x43U
/* Its registers after INIT: reg[i] = i ^ 0xA5; a read returns them from the pointer on. */
#define IST_TARGET_REG(i)    ((uint8_t)(((i) & 0xFFU) ^ 0xA5U))

/* ---- Data (both cores) ------------------------------------------------------------ */
uint32_t ist_xorshift32(uint32_t *state);                     /* state != 0                */
void     ist_fill(uint8_t *buf, uint32_t n, uint32_t seed);    /* xorshift32 bytes          */
uint32_t ist_crc32(const uint8_t *buf, uint32_t n);           /* zlib.crc32 of the bytes   */
/* Bytes of buf[0..n-1] that differ from the target's content from pointer p0;
   *first = index of the first (0xFFFFFFFF if none). */
uint32_t ist_check_target(const uint8_t *buf, uint32_t n, uint32_t p0, uint32_t *first);

/* ---- T1, T2, T14 on any instance, from either core (phase F1) ------------------- */
#define IST_NREG             7U    /* CR1, CR2, OAR1, OAR2, TIMINGR, TIMEOUTR, ISR          */
void     ist_regs(const I2C_TypeDef *i2c, uint32_t r[IST_NREG]);
/* Bit k set: register k differs from the reset image of RM0399 50.7 (T14). */
uint32_t ist_reset_diff(const I2C_TypeDef *i2c);

typedef struct
{
    uint32_t cases;
    uint32_t bad;              /* cases with another status than the one written by hand  */
    uint32_t groups_same;      /* bit g: the photo of group g + 1 unchanged                */
    uint32_t control_changed;  /* a valid Init changed the photo                           */
    uint32_t init_status;      /* of that Init                                             */
    uint32_t isr;              /* ist_t1_busy: ISR before the refusals                    */
    uint8_t  got[IST_T1_MAX];
    uint8_t  exp[IST_T1_MAX];
} IstT1Result_t;
/* The refusals of T1 on `i2c`, clocked in this core's bank; `timingr` valid for it. */
void     ist_t1(I2C_TypeDef *i2c, uint32_t timingr, IstT1Result_t *r);
/* T1 with ISR.BUSY = 1: every transfer ERR_BUS, nothing written. */
void     ist_t1_busy(I2C_TypeDef *i2c, IstT1Result_t *r);

typedef struct
{
    uint32_t cases;            /* TIMINGR x filters x DNF x timeout                        */
    uint32_t bad;              /* images that differ from the RM fields typed by hand     */
    uint32_t bad_deinit;       /* Deinit not OK or not the reset image                    */
    uint32_t bad_status;       /* Init neither OK nor ERR_BUS                              */
    uint32_t first_bad;        /* case << 4 | register, 0xFFFFFFFF if none                 */
    uint32_t got, exp;
    uint32_t control;          /* cases whose image differs from an altered expected one
                                  (DNF + 1 mod 16) through the same comparison: all of them */
} IstT2Result_t;
void     ist_t2(I2C_TypeDef *i2c, IstT2Result_t *r);

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
    volatile uint32_t arg1;            /* phase F1                                    */
    volatile uint32_t arg2;
    volatile uint32_t arg3;
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
    volatile uint32_t  m4_res[96];     /* phase F1: the result of the last command     */
} IstArea_t;

_Static_assert(sizeof(IstArea_t) <= 0x1000U, "IstArea_t no longer fits its 4 KB slot");

#define IST_AREA   ((IstArea_t *)IST_AREA_ADDR)

#ifdef __cplusplus
}
#endif

#endif /* I2C_SELFTEST_H */
