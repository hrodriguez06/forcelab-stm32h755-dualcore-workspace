/**
  ******************************************************************************
  * @file    spi_selftest.h
  * @brief   06_spi selftest (preset selftest-spi): entry points, the SRAM4
  *          block the two cores coordinate through, the bench step table and
  *          the line rules both cores' bench_claim() apply
  *          (Common/Src/spi_selftest_common.c).
  *
  *          Built only with SPI_DRIVER_SELFTEST. Plan, bench and the meaning
  *          of every test: 01_shared_drivers/06_spi/IMPLEMENTATION_PLAN.md
  *          ("Banco de pruebas", S13 and S14); wiring and rules S1-S10:
  *          01_shared_drivers/00_gpio/README.md section 8. The host side is
  *          tools/spi_hw_check.py.
  ******************************************************************************
  */

#ifndef SPI_SELFTEST_H
#define SPI_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Entry points ---------------------------------------------------------- */
void SPI_Selftest_CM7_PreRelease(void);   /* CPU1, before HSEM_BM_BootReleaseCpu2           */
void SPI_Selftest_CM7_Run(void);          /* CPU1, from Status_Task                         */
void SPI_Selftest_CM4_Run(void);          /* CPU2, after Shared_GpioLockInstall() and
                                             CPU2_PublishBuild()                            */

/* ---- Step table (S4, S7, S13, S14) ------------------------------------------- *
 * One table for both cores, so CPU2's bench_claim() judges a claim against the
 * same permissions CPU1 announced. SPI_BENCH_VERSION is the version of the bench
 * protocol: printed in the banner and in every STEP line, and checked by
 * tools/spi_hw_check.py, which keeps its own copy of the step names, their bus
 * owners and their J1 owners. Bump it when any of those change.              */
#define SPI_BENCH_VERSION    2U    /* v1: T0A, T0C, T0B(b)-(f) (plan phase F0); v2: after the
                                       T0B(d) timeout MCU_ACK stays low until a HOST_REQ
                                       pulse (audit B1) */

typedef enum
{
    SST_BUS_NONE = 0,      /* nobody drives PE8..PE15                              */
    SST_BUS_MCU            /* the MCU may drive the bus pins the step lists (S13)  */
} SstBus_t;                /* no "bus = AD2" step: the SPI bench never lets the AD2
                              drive a line other than HOST_REQ (plan D6)           */

typedef enum
{
    SST_J1_NONE = 0,       /* nobody may take PA5, PA6, PB4, PB5                   */
    SST_J1_CM7,            /* CPU1 may (SPI6, AF8)                                 */
    SST_J1_CM4             /* CPU2 may (SPI1, AF5)                                 */
} SstJ1_t;

typedef struct
{
    const char *name;
    uint8_t     bus;       /* SstBus_t                                                    */
    uint8_t     j1;        /* SstJ1_t: the one core that may take the J1 pins (S14)       */
    uint8_t     j1_af;     /* 1: that core may put them in its SPI AF (AF8 CPU1, AF5 CPU2) */
    uint8_t     pb5_out;   /* 1: that core may make PB5 a GPIO output (T0C only, S14)      */
    uint16_t    out_pe;    /* GPIOE pins the MCU may make GPIO outputs in this step       */
    uint16_t    af5_pe;    /* GPIOE pins the MCU may put in AF5 (SPI4, S13) in this step  */
    uint16_t    af0_pa;    /* GPIOA pins the MCU may put in AF0 (PA8 = MCO1)              */
} SstStep_t;

extern const SstStep_t k_sst_steps[];
extern const uint32_t  k_sst_step_count;

#define SST_STEP_NONE        0xFFFFFFFFUL   /* between steps: nothing wired may be claimed */

/* Index of `name` in k_sst_steps, or SST_STEP_NONE. */
uint32_t sst_step_index(const char *name);

/* ---- Line rules (00_gpio README section 8; 06_spi plan "Uso de las lineas") --- *
 * True when the pin may become an output (mode GPIO_BM_MODE_OUTPUT) or take
 * alternate function `af` (mode GPIO_BM_MODE_AF) on `core` (0 = CPU1, 1 = CPU2)
 * during step `step` (an index, or SST_STEP_NONE). Pure: reads only its
 * arguments and k_sst_steps. Default deny: a pin that is not in the line table
 * of the plan is refused. It does NOT cover the SPI4 half of S13: for PE11..PE14
 * in AF5 it only says the step allows them, and CPU1's bench_claim() must still
 * read SPI4 back (sst_spi_at_rest()) before it accepts; likewise for a J1 pin in
 * AF, each core's bench_claim() reads its own J1 SPI (SPI6 on CPU1, SPI1 on CPU2;
 * plan R2-3).                                                                    */
bool sst_line_rule(uint32_t core, uint32_t step, const GPIO_TypeDef *port, uint32_t pin,
                   uint32_t mode, uint32_t af);

/* ---- The SPI behind a claimed pin (S13, S14 with plan R2-3) --------------------- *
 * True when an SPI whose pins are about to go to AF takes no control of them:
 * SPE = 0 and AFCNTR = 0 (RM0399 53:3456-3464), IOSWP = 0 (no MISO/MOSI swap),
 * and a master (MASTER = 1) or at its reset value (CFG2 = 0). A slave with a
 * CFG2 != 0 is refused: it is what a mode fault leaves (MASTER cleared,
 * 53:1464-1476), and a slave drives MISO once enabled. Pure: the caller reads
 * CR1 and CFG2, after checking the SPI's clock in its own RCC bank. Used for
 * SPI4 on PE11..PE14 (CPU1), SPI6 on J1 (CPU1) and SPI1 on J1 (CPU2).         */
bool sst_spi_at_rest(uint32_t cr1, uint32_t cfg2);

/* Every flag of SPI_IFCR, written once (IFCR is write-only). */
#define SST_SPI_IFCR_ALL   (SPI_IFCR_EOTC | SPI_IFCR_TXTFC | SPI_IFCR_UDRC | SPI_IFCR_OVRC | SPI_IFCR_CRCEC | \
                            SPI_IFCR_TIFREC | SPI_IFCR_MODFC | SPI_IFCR_TSERFC | SPI_IFCR_SUSPC)

/* The raw SPI states the selftest writes (F0 has no driver): a master with SS
   from SSI, and SSI inactive for SSIOP = 0 (RM0399 53:566-567). */
#define SST_SPI_MASTER_SOFT  (SPI_CFG2_MASTER | SPI_CFG2_SSM)
#define SST_SPI_SSI_INACTIVE SPI_CR1_SSI

/* The SPI states T0b(f) puts the J1 owner in (SPI6 on CPU1, SPI1 on CPU2), one
   case each; bit k of the result = case k accepted. Cases 0..4 must be
   refused (SPE = 1, AFCNTR = 1, IOSWP = 1, slave with CFG2 != 0, clock off in
   the core's bank), cases 5..6 accepted (master at rest, CFG2 = 0). */
#define SST_SPI_CASES           7U
#define SST_SPI_CASES_EXPECT    0x00000060UL

/* Bench AF per pin (DS12923 table 9, 00_docs_stm32h7/STM32H755_Alternate_Functions.md). */
#define SST_AF_SPI4          5U    /* PE11 NSS, PE12 SCK, PE13 MISO, PE14 MOSI          */
#define SST_AF_SPI1          5U    /* PA5 SCK, PA6/PB4 MISO, PB5 MOSI: CPU2 (SPI1)      */
#define SST_AF_SPI6          8U    /* PA5 SCK, PA6/PB4 MISO, PB5 MOSI: CPU1 (SPI6)      */
#define SST_AF_MCO1          0U    /* PA8                                              */

/* ---- SRAM4 block (outside g_shared, fixed address) ------------------------------ *
 * 0x3800C000, 4 KB: inside the RAM_SHARED region of both linker scripts
 * (0x38008000, 32 KB) and inside MPU region 7 (0x38000000, 64 KB, non-cacheable
 * on CPU1). Above g_shared (0x38008000), the 00_gpio block (0x3800A000) and the
 * 03_tim block (0x3800B000), 4 KB each, so no two selftests share a byte. Fixed
 * rather than linked, so both images agree on it without depending on object
 * order.                                                                         */
#define SST_AREA_ADDR        0x3800C000UL
#define SST_AREA_MAGIC       0x53504930UL   /* "SPI0" */

typedef enum
{
    SST_CMD_NONE = 0,
    SST_CMD_CLAIMS,      /* arg0 = slot of m4_claims[] (SST_M4_SLOT_*)                          */
    SST_CMD_SAFE,        /* claim PA5 AF5 (step T0B), write it, SPI1 a master at rest, then
                            CPU2's bench_safe()                                                    */
    SST_CMD_SPI1,        /* T0b(f): the SPI1 states of SST_SPI_CASES against a claim of PA6 AF5 */
    SST_CMD_DONE
} SstCmd_t;

/* CPU2's claim cases (T0b(b) and (f)). The expected bit patterns are written by
   hand in SST_M4_CLAIMS_EXPECT_*: bit k = case k accepted. CPU1 judges them. */
#define SST_M4_CLAIM_CASES           19U
#define SST_M4_SLOT_T0B              0U             /* step T0B: J1 belongs to CPU2      */
#define SST_M4_SLOT_NONE             1U             /* between steps                     */
#define SST_M4_SLOT_S14              2U             /* step T0B_S14: J1 belongs to CPU1  */
#define SST_M4_SLOTS                 3U
#define SST_M4_CLAIMS_EXPECT_T0B     0x0000000FUL   /* cases 0..3: PA5, PA6, PB4, PB5 in AF5 */
#define SST_M4_CLAIMS_EXPECT_NONE    0x00000000UL   /* nothing between steps                 */
#define SST_M4_CLAIMS_EXPECT_S14     0x00000000UL   /* nothing in CPU1's J1 step             */

typedef struct
{
    uint32_t cases;           /* claims evaluated                                   */
    uint32_t accepted;        /* bit k = case k accepted                            */
    uint32_t regs_changed;    /* bit k: register k of the GPIOA/GPIOB/GPIOE snapshot
                                 (MODER, OTYPER, OSPEEDR, PUPDR, AFR[0], AFR[1]) differs */
    uint32_t step_seen;       /* the step index CPU2 judged against                 */
} SstClaimResult_t;

typedef struct
{
    uint32_t claim_ok;        /* PA5 AF5 accepted and written in step T0B            */
    uint32_t af_moder;        /* GPIOA MODER field of PA5 after the write (2 = AF)  */
    uint32_t af_afr;          /* GPIOA AFR field of PA5 after the write (5)         */
    uint32_t pull_pupdr;      /* PUPDR fields of PA6 | PB4 << 2 before the safe state
                                 (PA6 pull-down 2, PB4 pull-up 1: 0x6)              */
    uint32_t safe_moder;      /* MODER fields of PA5 | PA6 << 2 | PB4 << 4 | PB5 << 6
                                 after CPU2's bench_safe() (0: all input)            */
    uint32_t safe_pupdr;      /* PUPDR fields, same packing (0: no pull)             */
    uint32_t safe_afr;        /* AFR fields of PA5 | PA6 << 4 | PB4 << 8 | PB5 << 12 */
    uint32_t spi1_cr1;        /* SPI1 CR1, CFG2 before bench_safe() (SSI, MASTER|SSM)  */
    uint32_t spi1_cfg2;
    uint32_t spi1_cr1_safe;   /* ... and after it (0, 0), with SR (MODF = 0)           */
    uint32_t spi1_cfg2_safe;
    uint32_t spi1_sr_safe;
    uint32_t spi1_clk_same;   /* SPI1 clock in CPU2's bank as before the command      */
} SstSafeResult_t;

typedef struct
{
    uint32_t cases;           /* SST_SPI_CASES                                       */
    uint32_t accepted;        /* bit k = case k accepted                             */
    uint32_t spe_seen;        /* SPE read back 1 in the SPE = 1 case                 */
    uint32_t regs_changed;    /* GPIO snapshot registers that differ                 */
    uint32_t cr1_after;       /* the SPI at the end (0, 0)                           */
    uint32_t cfg2_after;
    uint32_t clk_same;        /* its clock in the core's bank as before              */
} SstSpiCaseResult_t;

typedef struct
{
    volatile uint32_t magic;
    volatile uint32_t session;
    volatile uint32_t version;         /* SPI_BENCH_VERSION of CPU1's table           */
    volatile uint32_t step;            /* current step index, SST_STEP_NONE between   */

    volatile char     m4_rev[24];      /* FW_GIT_REV of the CPU2 image                */
    volatile char     m4_build_id[24]; /* FW_BUILD_ID of the CPU2 image               */
    volatile uint32_t m4_version;      /* SPI_BENCH_VERSION of CPU2's table           */
    volatile uint32_t m4_ready;        /* = session once CPU2 serves commands         */

    volatile uint32_t cmd;
    volatile uint32_t arg0;
    volatile uint32_t arg1;
    volatile uint32_t cmd_seq;         /* CPU1 increments after writing cmd/args      */
    volatile uint32_t ack_seq;         /* CPU2 copies cmd_seq when done               */
    volatile uint32_t m4_timeout;      /* CPU2 gave up waiting for a command          */
    volatile uint32_t m4_aborts;       /* CPU2 bench_abort() calls                    */

    /* CPU2 results */
    SstClaimResult_t  m4_claims[SST_M4_SLOTS];
    SstSafeResult_t   m4_safe;
    SstSpiCaseResult_t m4_spi1;
} SstArea_t;

_Static_assert(sizeof(SstArea_t) <= 0x1000U, "SstArea_t no longer fits its 4 KB slot");

#define SST_AREA   ((SstArea_t *)SST_AREA_ADDR)

#ifdef __cplusplus
}
#endif

#endif /* SPI_SELFTEST_H */
