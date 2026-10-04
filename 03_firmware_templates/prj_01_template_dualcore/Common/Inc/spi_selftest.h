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
#include "spi_bm.h"
#include "spi_ll.h"

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
#define SPI_BENCH_VERSION    4U    /* v1: T0A, T0C, T0B(b)-(f) (plan phase F0); v2: after the
                                       T0B(d) timeout MCU_ACK stays low until a HOST_REQ
                                       pulse (audit B1); v3: the spi_bm steps F1_NOPIN,
                                       J1_SPI6, J1_SPI1 and SPI4_AD2 (phase F1); v4: the
                                       spi_ll steps F2_NOPIN, J1_SPI6_LL, J1_SPI1_LL and
                                       SPI4_AD2_LL (phase F2) */

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

/* ---- Data pattern and oracles (pure; plan "Plan de pruebas") ------------------- *
 * Frame i of a transfer is the (i + 1)-th output of xorshift32 (x ^= x << 13,
 * x ^= x >> 17, x ^= x << 5) from `seed`, masked to `bits`. Buffers hold one
 * element per frame of `es` bytes (1, 2 or 4). The digest is the CRC-32 of zlib
 * (reflected 0xEDB88320, init and final XOR 0xFFFFFFFF) over each masked frame
 * as 4 little-endian bytes; the host recomputes all of it from the seed.         */
uint32_t sst_mask(uint32_t bits);
uint32_t sst_elem_bytes(uint32_t bits);
void     sst_fill(void *buf, uint32_t es, uint32_t frames, uint32_t bits, uint32_t seed);
void     sst_fill_const(void *buf, uint32_t es, uint32_t frames, uint32_t value);
uint32_t sst_get(const void *buf, uint32_t es, uint32_t i);
uint32_t sst_crc32_frames(const void *buf, uint32_t es, uint32_t frames, uint32_t bits);
uint32_t sst_mismatch(const void *a, const void *b, uint32_t es, uint32_t frames, uint32_t bits);
uint32_t sst_coverage(const void *buf, uint32_t es, uint32_t frames, uint32_t bits);

/* T2's oracle: CFG1, CFG2 and CR1 of RM0399 53.11.1-53.11.4 for `cfg`, every field
   placed with its bit position typed from the RM text, not from CMSIS or the driver. */
void     sst_t2_expect(const SpiBm_Config_t *cfg, uint32_t *cfg1, uint32_t *cfg2, uint32_t *cr1);

/* F2: the spi_ll configuration of a spi_bm one, from the LL constants written by hand
   in the test (not from spi_ll.c, whose translation T1 and T2 test). */
void     sst_ll_of(const SpiBm_Config_t *b, SpiLl_Config_t *l);

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
    SST_CMD_DONE,
    /* Phase F1 (spi_bm on SPI1, CPU2's instance; plus SPI2 for T13) */
    SST_CMD_T1,          /* T1 refusals on SPI1; arg0 = 0 spi_bm, 1 spi_ll -> m4_res             */
    SST_CMD_T2,          /* T2 register images of SPI1; arg0 = 0 spi_bm, 1 spi_ll: images against
                            the RM table, and spi_ll against spi_bm bit for bit -> m4_res         */
    SST_CMD_J1PINS,      /* arg0 = SST_J1P_*, arg1 = CPOL (pull of SCK), arg2 = GPIO speed       */
    SST_CMD_XFER,        /* arg0 = SST_XCFG(...), arg1 = seed, arg2 = frames, arg3 = SST_XM_*
                            -> m4_res (SstXferRes)                                               */
    SST_CMD_T6C,         /* T6(c): arg0 = 0: SPI1 IRQs in 10 ms after the end; 1: the control
                            (TXPIE = 1 with SPE = 0)                    -> m4_res[0] = IRQs      */
    SST_CMD_T9,          /* arg0 = mbr, arg1 = data bits: SPI1 TX only  -> m4_res[0..1] = t1000, t2000 */
    SST_CMD_T13,         /* arg0 = 1: DisableClock(SPI2); 2: EnableClock then DisableClock(SPI2)
                            -> m4_res[0..2] = statuses, RCC_C2 APB1LENR after                    */
    SST_CMD_T14,         /* SPI_BM_Deinit(SPI1) and its registers       -> m4_res[0..9]          */
    SST_CMD_B1           /* cycles of Init, Transfer of 1 frame, TransferIrq; arg0 = tier
                            -> m4_res[0..5]                                                      */
} SstCmd_t;

/* SST_CMD_J1PINS: the four J1 pins of SPI1 (AF5), CPU2's J1 steps only (S14). */
#define SST_J1P_RELEASE      0U   /* the four in input without pull                             */
#define SST_J1P_LOOP         1U   /* PA5 SCK, PB5 MOSI, PA6 MISO in AF5; PB4 input               */
#define SST_J1P_PB4_UP       2U   /* MISO moved to PB4 (AF5, pull-up), PA6 input (plan D2)       */
#define SST_J1P_PB4_DOWN     3U   /* the same with pull-down                                     */

/* SST_CMD_XFER: one transfer on SPI1 (Init, transfer, Deinit). */
#define SST_XM_POLL          0U   /* SPI_BM_Transfer                                             */
#define SST_XM_IT            1U   /* SPI_BM_TransferStartIt + the test's SPI1_IRQHandler         */
#define SST_XM_DMA           2U   /* DMA2 streams 0 (RX) and 1 (TX), the DMA driver's BM tier    */
#define SST_XM_DMA_NORX      3U   /* the T7 control: RXDMAEN = 0                                 */
#define SST_XCFG(mode, bits, lsb, thr, mbr, dir) \
    ((uint32_t)(mode) | ((uint32_t)(bits) << 2) | ((uint32_t)(lsb) << 8) | ((uint32_t)(thr) << 9) | \
     ((uint32_t)(mbr) << 14) | ((uint32_t)(dir) << 17))
#define SST_XCFG_MODE(c)     ((c) & 3U)
#define SST_XCFG_BITS(c)     (((c) >> 2) & 0x3FU)
#define SST_XCFG_LSB(c)      (((c) >> 8) & 1U)
#define SST_XCFG_THR(c)      (((c) >> 9) & 0x1FU)
#define SST_XCFG_MBR(c)      (((c) >> 14) & 7U)
#define SST_XCFG_DIR(c)      (((c) >> 17) & 1U)
#define SST_XCFG_LL          (1UL << 18)       /* F2: the transfer through spi_ll       */
#define SST_XCFG_IS_LL(c)    (((c) >> 18) & 1U)

/* The result of one transfer, in m4_res[] for SST_CMD_XFER. */
typedef struct
{
    uint32_t status;          /* SpiBm_Status_t of the transfer                      */
    uint32_t mism;            /* frames of RX that differ from TX, masked to the size */
    uint32_t crc;             /* sst_crc32_frames() of what was received             */
    uint32_t cov;             /* bits that took both values in TX (sst_coverage)     */
    uint32_t cycles;          /* CYCCNT of the transfer call(s)                      */
    uint32_t irqs;            /* SPI1 interrupts (IT), from the start to the end     */
    uint32_t init_st;         /* SPI_BM_Init                                          */
    uint32_t deinit_st;       /* SPI_BM_Deinit                                        */
    uint32_t sentinel_left;   /* DMA control: RX words still holding the sentinel     */
} SstXferRes_t;

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
    volatile uint32_t arg2;
    volatile uint32_t arg3;

    /* CPU2 results */
    SstClaimResult_t  m4_claims[SST_M4_SLOTS];
    SstSafeResult_t   m4_safe;
    SstSpiCaseResult_t m4_spi1;
    volatile uint32_t m4_res[160];     /* phase F1: the result of the last command     */
} SstArea_t;

_Static_assert(sizeof(SstArea_t) <= 0x1000U, "SstArea_t no longer fits its 4 KB slot");

#define SST_AREA   ((SstArea_t *)SST_AREA_ADDR)

#ifdef __cplusplus
}
#endif

#endif /* SPI_SELFTEST_H */
