/**
  ******************************************************************************
  * @file    spi_selftest.c
  * @brief   06_spi selftest, CPU2 side (preset selftest-spi), phases F0 and
  *          F1 (spi_bm on SPI1, CPU2's instance: T1, T2, T4, T6-T9, T14, B1,
  *          and the CPU2 half of T13 on SPI2).
  *
  *          CPU2 has no console. It publishes its Rev, build id and step-table
  *          version in the SRAM4 block (Common/Inc/spi_selftest.h), serves the
  *          commands CPU1 writes there, and CPU1 prints and judges the results.
  *
  *          Electrical safety: the only pins CPU2 may ever configure as AF are
  *          the four J1 pins (PA5, PA6, PB4, PB5) in AF5 (SPI1), and only in a
  *          step whose J1 owner is CPU2; none of them, ever, as a GPIO output
  *          from this core (S14: PA6 and PB4 never; PB5 only in T0C, whose
  *          owner is CPU1). No AD2 line is CPU2's. bench_claim() judges every
  *          output/AF against the step CPU1 published (S4), and a J1 pin goes
  *          to AF only with SPI1 read back at rest, clocked in this core's
  *          bank (spi1_j1_ok(), plan R2-3). bench_safe() puts the four J1 pins
  *          back in input without pull and releases SPI1 in the order of plan
  *          rule 9 (S2, S14). pin_cfg() is the only caller of the GPIO driver
  *          for those pins.
  ******************************************************************************
  */

#include "spi_selftest.h"
#include "spi_bm.h"
#include "dma_bm.h"
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "stm32h7xx_hal.h"

/* CPU1 sends the next command within a minute or so (an F1 step on CPU1 alone,
   SPI4_AD2 or J1_SPI6, prints for tens of seconds at 115200 Bd); five minutes of
   silence mean it stopped (a fault, or a bench abort that skipped the DONE).
   CPU2 then goes to S2 and on to the application loop instead of waiting for
   ever. */
#define SST_CM4_IDLE_TIMEOUT_MS   300000U

#define PA_J1_SCK    5U
#define PA_J1_MISO   6U
#define PB_J1_MISO   4U
#define PB_J1_MOSI   5U

static uint32_t s_af_speed = GPIO_BM_SPEED_LOW;   /* T8 raises it for the J1 pins */

/* SPI1's clock in CPU2's RCC bank (RM0399 9.5.10, 9.7.1). */
static bool spi1_clocked(void)
{
    return (RCC_C2->APB2ENR & RCC_APB2ENR_SPI1EN) != 0U;
}

/* Plan rule 9 (R2-2): IER = 0 -> SPE = 0 keeping SSI -> CFG2 = 0 -> CR1 = 0 ->
   every IFCR flag in one write (the same order as CPU1's spi_release()). */
static void spi_release(SPI_TypeDef *spi)
{
    spi->IER  = 0U;
    spi->CR1  = spi->CR1 & SPI_CR1_SSI;
    spi->CFG2 = 0U;
    spi->CR1  = 0U;
    spi->IFCR = SST_SPI_IFCR_ALL;
    __DSB();
}

/* Raw SPI state for the T0b(f) cases, in the order of CPU1's spi_set(): SSI
   inactive before CFG2, so no master with SSM = 1 ever sees SSI active. */
static void spi_set(SPI_TypeDef *spi, uint32_t cr1, uint32_t cfg2)
{
    spi->IER  = 0U;
    spi->CR1  = SPI_CR1_SSI;
    spi->CFG2 = cfg2;
    spi->CR1  = cr1 & ~SPI_CR1_SPE;
    spi->IFCR = SST_SPI_IFCR_ALL;
    spi->CR1  = cr1;
    __DSB();
}

/* SPI1 on and at its reset values in CPU2's bank; returns whether it was on. */
static bool spi1_on_at_reset(void)
{
    const bool was_on = spi1_clocked();
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;      /* CPU2's bank */
    (void)RCC->APB2ENR;
    spi_set(SPI1, 0U, 0U);
    return was_on;
}

static void spi1_clock_back(bool was_on)
{
    if (!was_on)
    {
        spi_set(SPI1, 0U, 0U);
        RCC->APB2ENR &= ~RCC_APB2ENR_SPI1EN;
        (void)RCC->APB2ENR;
    }
}

/* ---- S2 --------------------------------------------------------------------------- */

/* PA5, PA6, PB4, PB5 in input without pull, AF fields 0; then SPI1, if
   clocked in this bank, released (spi_release()). Raw writes inside the
   cross-core lock (taken if it can be; the writes happen regardless,
   interrupts masked). */
static void bench_safe(void)
{
    GpioBm_Section_t s;
    const bool locked = (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK);
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN;   /* CPU2's bank (RM0399 9.5.10) */
    (void)RCC->AHB4ENR;
    const uint32_t a2 = (3UL << (PA_J1_SCK * 2U)) | (3UL << (PA_J1_MISO * 2U));
    GPIOA->MODER   = GPIOA->MODER & ~a2;
    GPIOA->PUPDR   = GPIOA->PUPDR & ~a2;
    GPIOA->AFR[0] &= ~((0xFUL << (PA_J1_SCK * 4U)) | (0xFUL << (PA_J1_MISO * 4U)));
    const uint32_t b2 = (3UL << (PB_J1_MISO * 2U)) | (3UL << (PB_J1_MOSI * 2U));
    GPIOB->MODER   = GPIOB->MODER & ~b2;
    GPIOB->PUPDR   = GPIOB->PUPDR & ~b2;
    GPIOB->AFR[0] &= ~((0xFUL << (PB_J1_MISO * 4U)) | (0xFUL << (PB_J1_MOSI * 4U)));
    __DSB();
    if (spi1_clocked())
    {
        spi_release(SPI1);
    }
    __set_PRIMASK(pm);
    if (locked)
    {
        GPIO_BM_SectionExit(&s);
    }
}

static void bench_abort(void)
{
    SST_AREA->m4_aborts++;
    bench_safe();
}

/* ---- S4, S14 ------------------------------------------------------------------------- */

/* S14 with plan R2-3: SPI1, CPU2's J1 instance, read back before a J1 pin may
   go to AF (AF5). Reads only. */
static bool spi1_j1_ok(void)
{
    return spi1_clocked() && sst_spi_at_rest(SPI1->CR1, SPI1->CFG2);
}

#define J1_MASK_A   ((1UL << PA_J1_SCK) | (1UL << PA_J1_MISO))
#define J1_MASK_B   ((1UL << PB_J1_MISO) | (1UL << PB_J1_MOSI))

/* All or nothing, against the step CPU1 published in the block. */
static bool bench_claim(const GPIO_TypeDef *port, uint32_t mask, uint32_t mode, uint32_t af)
{
    const uint32_t step = SST_AREA->step;
    if ((mask & 0xFFFFUL) == 0U)
    {
        return false;
    }
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if (((mask & (1UL << p)) != 0U) && !sst_line_rule(1U, step, port, p, mode, af))
        {
            return false;
        }
    }
    if ((mode == GPIO_BM_MODE_AF) &&
        (((port == GPIOA) && ((mask & J1_MASK_A) != 0U)) || ((port == GPIOB) && ((mask & J1_MASK_B) != 0U))))
    {
        return spi1_j1_ok();
    }
    return true;
}

static bool pin_cfg(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af, uint32_t pull)
{
    GpioBm_Status_t st;
    if ((mode == GPIO_BM_MODE_OUTPUT) || (mode == GPIO_BM_MODE_AF))
    {
        if (!bench_claim(port, 1UL << pin, mode, af))
        {
            bench_abort();
            return false;
        }
    }
    if (mode == GPIO_BM_MODE_AF)
    {
        st = GPIO_BM_ConfigAlternateEx(port, pin, af, GPIO_BM_OTYPE_PP, pull, s_af_speed);
    }
    else if (mode == GPIO_BM_MODE_INPUT)
    {
        st = GPIO_BM_ConfigInput(port, pin, pull);
    }
    else
    {
        st = GPIO_BM_ERR_PARAM;    /* CPU2 never makes a pin of this bench a GPIO output */
    }
    if (st != GPIO_BM_OK)
    {
        bench_abort();
        return false;
    }
    return true;
}

/* ---- T0b(b)(f) on CPU2 --------------------------------------------------------------- */

typedef struct { GPIO_TypeDef *port; uint16_t mask; uint8_t mode; uint8_t af; } Claim_t;

#define O  GPIO_BM_MODE_OUTPUT
#define A  GPIO_BM_MODE_AF
/* Case k sets bit k of the result when accepted. Expected, by hand, in
   spi_selftest.h: cases 0..3 in step T0B (J1 = CPU2), none between steps, none
   in step T0B_S14 (J1 = CPU1). They run with SPI1 clocked in CPU2's bank at
   its reset values (run_claims()), so the J1 AF cases depend on the step. */
static const Claim_t k_claims[SST_M4_CLAIM_CASES] = {
    { GPIOA, 1U << PA_J1_SCK, A, 5U },             /*  0 PA5 SPI1_SCK                              */
    { GPIOA, 1U << PA_J1_MISO, A, 5U },            /*  1 PA6 SPI1_MISO (J1-B)                      */
    { GPIOB, 1U << PB_J1_MISO, A, 5U },            /*  2 PB4 SPI1_MISO (the D2 control)            */
    { GPIOB, 1U << PB_J1_MOSI, A, 5U },            /*  3 PB5 SPI1_MOSI (J1-A)                      */
    { GPIOA, 1U << PA_J1_MISO, O, 0U },            /*  4 PA6 output: never (S14)                   */
    { GPIOB, 1U << PB_J1_MISO, O, 0U },            /*  5 PB4 output: never (S14)                   */
    { GPIOB, 1U << PB_J1_MOSI, O, 0U },            /*  6 PB5 output: T0C only, CPU1                */
    { GPIOA, 1U << PA_J1_SCK, O, 0U },             /*  7 PA5 output: never                         */
    { GPIOA, 1U << PA_J1_MISO, A, 8U },            /*  8 PA6 AF8 (SPI6): CPU1's AF                 */
    { GPIOB, 1U << PB_J1_MOSI, A, 8U },            /*  9 PB5 AF8                                   */
    { GPIOB, 1U << PB_J1_MISO, A, 0U },            /* 10 PB4 AF0 (NJTRST): wrong AF                */
    { GPIOA, 1U << 7, A, 5U },                     /* 11 PA7 (RMII_CRS_DV): never                  */
    { GPIOA, 1U << 4, A, 5U },                     /* 12 PA4 (SPI1_NSS): never touched (R4)        */
    { GPIOE, 1U << 12, A, 5U },                    /* 13 PE12 SPI4_SCK: CPU1's bus                 */
    { GPIOE, 1U << 5, A, 5U },                     /* 14 PE5: PE0..PE7 never (S13)                 */
    { GPIOA, 1U << 8, A, 0U },                     /* 15 PA8 MCO1: CPU1's line                     */
    { GPIOE, 1U << 7, O, 0U },                     /* 16 PE7 MCU_ACK                               */
    { GPIOA, (1U << PA_J1_SCK) | (1U << 7), A, 5U },/* 17 mixed: PA5 with PA7                       */
    { GPIOG, 1U << 13, A, 5U },                    /* 18 PG13 (RMII_TXD0): never                   */
};
#undef O
#undef A

/* The configuration registers of GPIOA, GPIOB and GPIOE, read raw: a refused
   claim writes none of them (the same oracle as CPU1's port_snap). */
#define SNAP_REGS   18U
typedef struct { uint32_t r[SNAP_REGS]; } PortSnap_t;

static void port_snap(PortSnap_t *s)
{
    const GPIO_TypeDef *const ports[3] = { GPIOA, GPIOB, GPIOE };
    uint32_t i = 0U;
    for (uint32_t k = 0U; k < 3U; k++)
    {
        s->r[i++] = ports[k]->MODER;
        s->r[i++] = ports[k]->OTYPER;
        s->r[i++] = ports[k]->OSPEEDR;
        s->r[i++] = ports[k]->PUPDR;
        s->r[i++] = ports[k]->AFR[0];
        s->r[i++] = ports[k]->AFR[1];
    }
}

/* Bit k set = register k of the snapshot differs. */
static uint32_t port_snap_diff(const PortSnap_t *a, const PortSnap_t *b)
{
    uint32_t diff = 0U;
    for (uint32_t k = 0U; k < SNAP_REGS; k++)
    {
        if (a->r[k] != b->r[k])
        {
            diff |= 1UL << k;
        }
    }
    return diff;
}

static void run_claims(SstClaimResult_t *r)
{
    const bool spi1_was_on = spi1_on_at_reset();
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    uint32_t acc = 0U;
    r->step_seen = SST_AREA->step;
    for (uint32_t k = 0U; k < SST_M4_CLAIM_CASES; k++)
    {
        if (bench_claim(k_claims[k].port, k_claims[k].mask, k_claims[k].mode, k_claims[k].af))
        {
            acc |= 1UL << k;
        }
    }
    r->accepted     = acc;
    r->cases        = SST_M4_CLAIM_CASES;
    port_snap(&after);
    r->regs_changed = port_snap_diff(&before, &after);
    spi1_clock_back(spi1_was_on);
}

/* T0b(f), plan R2-3: the SPI1 states of SST_SPI_CASES (the same table as
   CPU1's k_spi6_cases), each against a claim of PA6 in AF5 in step T0B (J1 =
   CPU2). The J1 pins stay in input, so no state reaches a pin (AFCNTR = 1 only
   with MASTER = 1, RM0399 53:3462). */
typedef struct { uint32_t cr1, cfg2, clk; } SpiCase_t;
static const SpiCase_t k_spi1_cases[SST_SPI_CASES] = {
    { SST_SPI_SSI_INACTIVE | SPI_CR1_SPE, SST_SPI_MASTER_SOFT,                   1U },   /* 0 SPE = 1          */
    { SST_SPI_SSI_INACTIVE,               SST_SPI_MASTER_SOFT | SPI_CFG2_AFCNTR, 1U },   /* 1 AFCNTR = 1       */
    { SST_SPI_SSI_INACTIVE,               SST_SPI_MASTER_SOFT | SPI_CFG2_IOSWP,  1U },   /* 2 IOSWP = 1        */
    { SST_SPI_SSI_INACTIVE,               SPI_CFG2_SSM | SPI_CFG2_CPOL,          1U },   /* 3 slave, CFG2 != 0 */
    { SST_SPI_SSI_INACTIVE,               SST_SPI_MASTER_SOFT,                   0U },   /* 4 clock off        */
    { SST_SPI_SSI_INACTIVE,               SST_SPI_MASTER_SOFT,                   1U },   /* 5 control: master  */
    { 0U,                                 0U,                                    1U },   /* 6 control: reset   */
};

static void spi1_cases(SstSpiCaseResult_t *r)
{
    const bool was_on = spi1_on_at_reset();
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    uint32_t acc = 0U;
    r->spe_seen = 0U;
    for (uint32_t k = 0U; k < SST_SPI_CASES; k++)
    {
        RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
        (void)RCC->APB2ENR;
        spi_set(SPI1, k_spi1_cases[k].cr1, k_spi1_cases[k].cfg2);
        if ((k_spi1_cases[k].cr1 & SPI_CR1_SPE) != 0U)
        {
            r->spe_seen = ((SPI1->CR1 & SPI_CR1_SPE) != 0U) ? 1U : 0U;
        }
        if (k_spi1_cases[k].clk == 0U)
        {
            RCC->APB2ENR &= ~RCC_APB2ENR_SPI1EN;
            (void)RCC->APB2ENR;
        }
        if (bench_claim(GPIOA, 1UL << PA_J1_MISO, GPIO_BM_MODE_AF, SST_AF_SPI1))
        {
            acc |= 1UL << k;
        }
        RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
        (void)RCC->APB2ENR;
        spi_set(SPI1, 0U, 0U);
    }
    port_snap(&after);
    r->cases        = SST_SPI_CASES;
    r->accepted     = acc;
    r->regs_changed = port_snap_diff(&before, &after);
    r->cr1_after    = SPI1->CR1;
    r->cfg2_after   = SPI1->CFG2;
    spi1_clock_back(was_on);
    r->clk_same     = (spi1_clocked() == was_on) ? 1U : 0U;
}

/* MODER/PUPDR fields of PA5 | PA6 << 2 | PB4 << 4 | PB5 << 6. */
static uint32_t j1_field2(uint32_t ra, uint32_t rb)
{
    return ((ra >> (PA_J1_SCK * 2U)) & 0xFUL) | (((rb >> (PB_J1_MISO * 2U)) & 0xFUL) << 4);
}

/* PA5 to AF5 through the claim (allowed in step T0B, whose J1 owner is CPU2;
   PA5 is unwired, CN7-10, and SPI1 is clocked at its reset values, so nothing
   drives it), then SPI1 a master at rest (SSI, MASTER|SSM; SPE = 0, AFCNTR =
   0: still nothing driven), PA6 pull-down and PB4 pull-up as inputs; then
   bench_safe() must bring the four J1 pins back to input without pull and
   AF0, and release SPI1 (CFG2 = 0, CR1 = 0, no MODF: plan R2-3). */
static void run_safe(SstSafeResult_t *r)
{
    const bool spi1_was_on = spi1_on_at_reset();
    r->claim_ok = pin_cfg(GPIOA, PA_J1_SCK, GPIO_BM_MODE_AF, SST_AF_SPI1, GPIO_BM_PULL_NONE) ? 1U : 0U;
    spi_set(SPI1, SST_SPI_SSI_INACTIVE, SST_SPI_MASTER_SOFT);
    r->spi1_cr1  = SPI1->CR1;
    r->spi1_cfg2 = SPI1->CFG2;
    r->af_moder = (GPIOA->MODER >> (PA_J1_SCK * 2U)) & 3UL;
    r->af_afr   = (GPIOA->AFR[0] >> (PA_J1_SCK * 4U)) & 0xFUL;
    (void)pin_cfg(GPIOA, PA_J1_MISO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_DOWN);
    (void)pin_cfg(GPIOB, PB_J1_MISO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_UP);
    r->pull_pupdr = ((GPIOA->PUPDR >> (PA_J1_MISO * 2U)) & 3UL) | (((GPIOB->PUPDR >> (PB_J1_MISO * 2U)) & 3UL) << 2);
    bench_safe();
    r->safe_moder = j1_field2(GPIOA->MODER, GPIOB->MODER);
    r->safe_pupdr = j1_field2(GPIOA->PUPDR, GPIOB->PUPDR);
    r->safe_afr   = ((GPIOA->AFR[0] >> (PA_J1_SCK * 4U)) & 0xFFUL) | (((GPIOB->AFR[0] >> (PB_J1_MISO * 4U)) & 0xFFUL) << 8);
    r->spi1_cr1_safe  = SPI1->CR1;
    r->spi1_cfg2_safe = SPI1->CFG2;
    r->spi1_sr_safe   = SPI1->SR;
    spi1_clock_back(spi1_was_on);
    r->spi1_clk_same  = (spi1_clocked() == spi1_was_on) ? 1U : 0U;
}

/* ==========================================================================================
 * Phase F1: spi_bm on SPI1 (CPU2's instance), and the SPI2 half of T13. CPU1 orders,
 * prints and judges; every result goes to m4_res[].
 * ========================================================================================== */

static uint32_t s_cpu_hz;          /* CPU2, SystemCoreClock (HAL_Init: the D2 clock)            */
static uint32_t s_k1;              /* SPI1's kernel, pll1_q_ck (SPI123SEL = 000, RM0399 09:4653) */

static void f1_clocks(void)
{
    PLL1_ClocksTypeDef p1;
    HAL_RCCEx_GetPLL1ClockFreq(&p1);
    s_cpu_hz = SystemCoreClock;
    s_k1     = p1.PLL1_Q_Frequency;
}

static void res_clear(void)
{
    for (uint32_t i = 0U; i < 16U; i++)
    {
        SST_AREA->m4_res[i] = 0U;
    }
}

static void delay_cyc(uint32_t n)
{
    const uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < n)
    {
    }
}

static uint32_t guard_of(uint32_t mbr)
{
    uint32_t g = 0U;
    if ((SPI_BM_CalcGuardCycles(s_cpu_hz, s_k1, mbr, &g) != SPI_BM_OK) || (g == 0U))
    {
        g = 1U;
    }
    return g;
}

/* Four times the bus time plus 10 ms (the same bound as CPU1's). */
static uint32_t budget_of(uint32_t mbr, uint32_t bits, uint32_t frames)
{
    const uint64_t sck = ((uint64_t)s_cpu_hz << (mbr + 1U)) / s_k1;
    const uint64_t c = ((uint64_t)frames * (bits + 2U) * sck * 4U) + ((uint64_t)(s_cpu_hz / 100U));
    return (c > 0xF0000000ULL) ? 0xF0000000UL : (uint32_t)c;
}

static SpiBm_Config_t mkcfg(uint32_t mbr, uint32_t mode, uint32_t bits, bool lsb, uint32_t dir, uint32_t thr)
{
    const SpiBm_Config_t c = { SPI1, mbr, mode, bits, lsb, dir, thr, SPI_BM_NSS_SOFT, false, 0U, 0U, false };
    return c;
}

static uint32_t thr_max(uint32_t bits)
{
    const uint32_t bpf = (bits <= 8U) ? 1U : ((bits <= 16U) ? 2U : ((bits <= 24U) ? 3U : 4U));
    return (16U / bpf) / 2U;               /* half of SPI1's 16-byte FIFO (53:56, 53:3394) */
}

/* Buffers in SRAM2 (D2: CPU2's RAM and DMA2's reach; CPU2 has no data cache). */
#define M4_MAXF      4096U
static uint32_t s_tx[M4_MAXF] __attribute__((aligned(32)));
static uint32_t s_rx[M4_MAXF] __attribute__((aligned(32)));

/* ---- Registers against RM0399's reset image (T14), and the T1 snapshot ----------------- */
#define NREG          8U
static void spi_regs(const SPI_TypeDef *s, uint32_t *r)
{
    r[0] = s->CR1;
    r[1] = s->CR2;
    r[2] = s->CFG1;
    r[3] = s->CFG2;
    r[4] = s->IER;
    r[5] = s->CRCPOLY;
    r[6] = s->UDRDR;
    r[7] = s->I2SCFGR;
}

/* Typed from RM0399 53.11 (CFG1 53:3309, CRCPOLY 53:3815; the rest 0). */
static const uint32_t k_reset_regs[NREG] = { 0U, 0U, 0x00070007UL, 0U, 0U, 0x00000107UL, 0U, 0U };

static uint32_t reset_diff(const SPI_TypeDef *s)
{
    uint32_t r[NREG];
    spi_regs(s, r);
    uint32_t d = 0U;
    for (uint32_t k = 0U; k < NREG; k++)
    {
        if (r[k] != k_reset_regs[k])
        {
            d |= 1UL << k;
        }
    }
    return d;
}

#define T1_SNAP      (NREG + 6U)
static void t1_snap(uint32_t *r)
{
    spi_regs(SPI1, r);
    r[NREG + 0U] = RCC_C1->APB1LENR;
    r[NREG + 1U] = RCC_C1->APB2ENR;
    r[NREG + 2U] = RCC_C1->APB4ENR;
    r[NREG + 3U] = RCC_C2->APB1LENR;
    r[NREG + 4U] = RCC_C2->APB2ENR;
    r[NREG + 5U] = RCC_C2->APB4ENR;
}

static bool snap_eq(const uint32_t *a, const uint32_t *b)
{
    for (uint32_t i = 0U; i < T1_SNAP; i++)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}

/* ---- T1 on SPI1: the refusals, with the expected status of each case by hand ---------- */
static uint32_t s_t1_n;
static uint32_t s_t1_bad;

static void t1(SpiBm_Status_t got, SpiBm_Status_t exp)
{
    volatile uint32_t *r = SST_AREA->m4_res;
    if ((got != exp) && (s_t1_bad == 0U))
    {
        r[4] = s_t1_n;
        r[5] = (uint32_t)got;
        r[6] = (uint32_t)exp;
    }
    if (got != exp)
    {
        s_t1_bad++;
    }
    s_t1_n++;
}

static void cmd_t1(void)
{
    volatile uint32_t *r = SST_AREA->m4_res;
    uint32_t a[T1_SNAP];
    uint32_t b[T1_SNAP];
    uint32_t same = 0U;
    uint32_t v = 0U;
    SpiBm_Caps_t caps;
    const SpiBm_Config_t c = mkcfg(7U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    SpiBm_Config_t k;
    res_clear();
    s_t1_n = 0U;
    s_t1_bad = 0U;
    (void)SPI_BM_EnableClock(SPI1);
    (void)SPI_BM_Deinit(SPI1);

    /* 1. parameters, nothing initialised */
    t1_snap(a);
    t1(SPI_BM_CheckConfig(NULL), SPI_BM_ERR_PARAM);
    k = c; k.spi = (SPI_TypeDef *)((uint32_t)SPI1 + 4U);      t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.data_bits = 3U;                                    t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.data_bits = 33U;                                   t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.mode = 4U;                                         t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.mbr = 8U;                                          t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.direction = 2U;                                    t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.nss = 3U;                                          t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.mssi = 1U;                                         t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.fifo_threshold = 0U;                               t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.data_bits = 32U; k.fifo_threshold = 3U;            t1(SPI_BM_Init(&k), SPI_BM_ERR_RANGE);
    k = c; k.data_bits = 24U; k.fifo_threshold = 3U;            t1(SPI_BM_Init(&k), SPI_BM_ERR_RANGE);
    t1(SPI_BM_GetCaps(NULL, &caps), SPI_BM_ERR_PARAM);
    t1(SPI_BM_CalcPrescaler(100U, 1U, &v, NULL), SPI_BM_ERR_PARAM);
    t1(SPI_BM_GetKernelSource(SPI1, NULL), SPI_BM_ERR_PARAM);
    t1(SPI_BM_TransferStartIt(SPI1, NULL), SPI_BM_ERR_PARAM);
    t1_snap(b);
    same |= snap_eq(a, b) ? 1U : 0U;

    /* 2. SPI1 transmit only, 32-bit: the transfer's own parameters */
    k = c; k.direction = SPI_BM_DIR_TX_ONLY; k.data_bits = 32U;
    const SpiBm_Status_t i2 = SPI_BM_Init(&k);
    SpiBm_Xfer_t x = { s_tx, NULL, 1U, 0U, 0U, 0U, SPI_BM_OK };
    t1_snap(a);
    t1(SPI_BM_Transfer(SPI1, s_tx, NULL, 0U, 1U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Transfer(SPI1, s_tx, NULL, 65536U, 1U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Transfer(SPI1, NULL, NULL, 1U, 1U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Transfer(SPI1, s_tx, NULL, 1U, 0U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Transfer(SPI1, s_tx, NULL, 1U, 1U, 0U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Transfer(SPI1, s_tx, s_rx, 1U, 1U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Transfer(SPI1, (const uint8_t *)s_tx + 2U, NULL, 1U, 1U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_TransferStartIt(SPI1, &x), SPI_BM_ERR_PARAM);
    t1(SPI_BM_DmaPrepare(SPI1, 1U, true), SPI_BM_ERR_PARAM);
    t1(SPI_BM_DmaPrepare(SPI1, 0U, false), SPI_BM_ERR_PARAM);
    t1(SPI_BM_DmaEnd(SPI1, 0U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_WaitEnd(SPI1, 0U), SPI_BM_ERR_PARAM);
    t1_snap(b);
    same |= snap_eq(a, b) ? 2U : 0U;
    (void)SPI_BM_Deinit(SPI1);

    /* 3. SPE = 1 by hand (the J1 pins are in input in this step) */
    k = c;
    const SpiBm_Status_t i3 = SPI_BM_Init(&k);
    SPI1->CR1 |= SPI_CR1_SPE;
    const uint32_t spe = SPI1->CR1 & SPI_CR1_SPE;
    t1_snap(a);
    t1(SPI_BM_Init(&k), SPI_BM_ERR_BUSY);
    t1(SPI_BM_CheckConfig(&k), SPI_BM_ERR_BUSY);
    t1(SPI_BM_DmaPrepare(SPI1, 1U, false), SPI_BM_ERR_BUSY);
    t1(SPI_BM_DmaStart(SPI1), SPI_BM_ERR_BUSY);
    t1(SPI_BM_Transfer(SPI1, s_tx, s_rx, 1U, 1U, 1000U), SPI_BM_ERR_BUSY);
    t1_snap(b);
    same |= snap_eq(a, b) ? 4U : 0U;
    SPI1->CR1 &= ~SPI_CR1_SPE;

    /* 4. AFCNTR = 1 in force: another CPOL or SSIOP */
    k = c; k.keep_pins = true;
    const SpiBm_Status_t i4 = SPI_BM_Init(&k);
    t1_snap(a);
    k.mode = 3U;                                     t1(SPI_BM_Init(&k), SPI_BM_ERR_BUSY);
    k.mode = 0U; k.nss_active_high = true;           t1(SPI_BM_Init(&k), SPI_BM_ERR_BUSY);
    t1_snap(b);
    same |= snap_eq(a, b) ? 8U : 0U;
    (void)SPI_BM_Deinit(SPI1);

    /* the control: a valid Init changes the snapshot */
    t1_snap(a);
    k = c;
    const SpiBm_Status_t w = SPI_BM_Init(&k);
    t1_snap(b);
    const bool ctrl = !snap_eq(a, b);
    (void)SPI_BM_Deinit(SPI1);

    r[0] = s_t1_n;
    r[1] = s_t1_bad + (((i2 != SPI_BM_OK) || (i3 != SPI_BM_OK) || (i4 != SPI_BM_OK) || (spe == 0U)) ? 1U : 0U);
    r[2] = same;
    r[3] = (ctrl && (w == SPI_BM_OK)) ? 1U : 0U;
}

/* ---- T2 on SPI1: register images against sst_t2_expect() ------------------------------- */
static void cmd_t2(void)
{
    volatile uint32_t *r = SST_AREA->m4_res;
    static const uint32_t bits_list[4] = { 4U, 8U, 16U, 32U };
    uint32_t n = 0U;
    uint32_t bad = 0U;
    uint32_t bad_d = 0U;
    uint32_t bad_r = 0U;
    res_clear();
    (void)SPI_BM_EnableClock(SPI1);
    (void)SPI_BM_Deinit(SPI1);
    for (uint32_t mbr = 0U; mbr <= 7U; mbr++)
    {
        for (uint32_t mode = 0U; mode <= 3U; mode++)
        {
            for (uint32_t ib = 0U; ib < 4U; ib++)
            {
                const uint32_t bits = bits_list[ib];
                const uint32_t thr[2] = { 1U, thr_max(bits) };
                for (uint32_t it = 0U; it < 2U; it++)
                {
                    for (uint32_t lsb = 0U; lsb < 2U; lsb++)
                    {
                        for (uint32_t dir = 0U; dir < 2U; dir++)
                        {
                            for (uint32_t nss = 0U; nss < 3U; nss++)
                            {
                                for (uint32_t op = 0U; op < 2U; op++)
                                {
                                    for (uint32_t mi = 0U; mi < 4U; mi++)
                                    {
                                        const uint32_t mssi = ((mi & 1U) != 0U) ? 15U : 0U;
                                        const uint32_t midi = ((mi & 2U) != 0U) ? 15U : 0U;
                                        if ((nss == SPI_BM_NSS_SOFT) && (mssi != 0U))
                                        {
                                            continue;
                                        }
                                        for (uint32_t keep = 0U; keep < 2U; keep++)
                                        {
                                            const SpiBm_Config_t c = { SPI1, mbr, mode, bits, lsb != 0U, dir, thr[it],
                                                                       nss, op != 0U, mssi, midi, keep != 0U };
                                            uint32_t e1 = 0U;
                                            uint32_t e2 = 0U;
                                            uint32_t ecr = 0U;
                                            sst_t2_expect(&c, &e1, &e2, &ecr);
                                            const SpiBm_Status_t st = SPI_BM_Init(&c);
                                            const uint32_t g2 = SPI1->CFG2;
                                            if ((st != SPI_BM_OK) || (SPI1->CFG1 != e1) || (g2 != e2) ||
                                                (SPI1->CR1 != ecr))
                                            {
                                                if (bad == 0U)
                                                {
                                                    r[5] = n;
                                                    r[6] = g2;
                                                    r[7] = e2;
                                                }
                                                bad++;
                                            }
                                            if ((SPI1->I2SCFGR != 0U) || (SPI1->CRCPOLY != 0x00000107UL) ||
                                                (SPI1->UDRDR != 0U))
                                            {
                                                bad_r++;
                                            }
                                            if (SPI_BM_Deinit(SPI1) != SPI_BM_OK)
                                            {
                                                bad_d++;
                                            }
                                            n++;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    uint32_t e1 = 0U;
    uint32_t e2 = 0U;
    uint32_t ecr = 0U;
    const SpiBm_Config_t c0 = mkcfg(0U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    sst_t2_expect(&c0, &e1, &e2, &ecr);
    (void)SPI_BM_Init(&c0);
    r[4] = (SPI1->CFG2 != (e2 ^ (1UL << 22))) ? 1U : 0U;
    (void)SPI_BM_Deinit(SPI1);
    r[0] = n;
    r[1] = bad;
    r[2] = bad_d;
    r[3] = bad_r;
}

/* ---- J1 pins of SPI1 (AF5): the claim reads SPI1 back at rest first (S14, R2-3) ------- */
static bool j1_af(uint32_t port_pin_a, GPIO_TypeDef *port, uint32_t pull)
{
    return pin_cfg(port, port_pin_a, GPIO_BM_MODE_AF, SST_AF_SPI1, pull);
}

static void cmd_j1pins(uint32_t what, uint32_t cpol, uint32_t speed)
{
    volatile uint32_t *r = SST_AREA->m4_res;
    res_clear();
    (void)SPI_BM_EnableClock(SPI1);
    (void)SPI_BM_Deinit(SPI1);
    bool ok;
    if (what == SST_J1P_RELEASE)
    {
        ok = pin_cfg(GPIOA, PA_J1_SCK, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE) &&
             pin_cfg(GPIOA, PA_J1_MISO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE) &&
             pin_cfg(GPIOB, PB_J1_MISO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE) &&
             pin_cfg(GPIOB, PB_J1_MOSI, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE);
    }
    else
    {
        s_af_speed = speed;
        ok = j1_af(PA_J1_SCK, GPIOA, (cpol != 0U) ? GPIO_BM_PULL_UP : GPIO_BM_PULL_DOWN) &&
             j1_af(PB_J1_MOSI, GPIOB, GPIO_BM_PULL_NONE);
        /* one MISO pin in AF at a time: the other goes to input first */
        if (ok && (what == SST_J1P_LOOP))
        {
            ok = pin_cfg(GPIOB, PB_J1_MISO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE) &&
                 j1_af(PA_J1_MISO, GPIOA, GPIO_BM_PULL_NONE);
        }
        else if (ok)
        {
            ok = pin_cfg(GPIOA, PA_J1_MISO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE) &&
                 j1_af(PB_J1_MISO, GPIOB, (what == SST_J1P_PB4_UP) ? GPIO_BM_PULL_UP : GPIO_BM_PULL_DOWN);
        }
        s_af_speed = GPIO_BM_SPEED_LOW;
    }
    r[0] = ok ? 1U : 0U;
    r[1] = ((GPIOA->MODER >> (PA_J1_SCK * 2U)) & 0xFUL) | (((GPIOB->MODER >> (PB_J1_MISO * 2U)) & 0xFUL) << 4);
}

/* ---- Transfers on SPI1: polling, interrupt, DMA2 ----------------------------------- */
static SpiBm_Xfer_t      s_it;
static volatile bool     s_it_active;
static volatile uint32_t s_it_irqs;
static volatile uint32_t s_it_spurious;
static uint32_t          s_b1_irq_min = 0xFFFFFFFFUL;
static uint32_t          s_b1_irq_max;

/* The project's vector for the test (plan C14), CPU2's NVIC, priority 6. */
void SPI1_IRQHandler(void)
{
    s_it_irqs++;
    if (s_it_active)
    {
        const uint32_t t0 = DWT->CYCCNT;
        const bool done = SPI_BM_TransferIrq(SPI1, &s_it);
        const uint32_t dt = DWT->CYCCNT - t0;
        if (dt < s_b1_irq_min)
        {
            s_b1_irq_min = dt;
        }
        if (dt > s_b1_irq_max)
        {
            s_b1_irq_max = dt;
        }
        if (done)
        {
            s_it_active = false;
        }
    }
    else
    {
        s_it_spurious++;
        SPI1->IER = 0U;
    }
}

static void nvic_spi1(bool on)
{
    IRQn_Type irq = SPI1_IRQn;
    (void)SPI_BM_GetIRQn(SPI1, &irq);
    if (on)
    {
        HAL_NVIC_SetPriority(irq, 6U, 0U);
        NVIC_ClearPendingIRQ(irq);
        HAL_NVIC_EnableIRQ(irq);
    }
    else
    {
        HAL_NVIC_DisableIRQ(irq);
        NVIC_ClearPendingIRQ(irq);
    }
}

#define M4_RX_STREAM   DMA2_Stream0
#define M4_TX_STREAM   DMA2_Stream1

static bool m4_stream(DMA_Stream_TypeDef *s, bool tx, uint32_t es, void *buf, uint32_t n)
{
    uint32_t req = 0U;
    (void)SPI_BM_GetDmaRequest(SPI1, tx, &req);
    const uint32_t sz = (es == 1U) ? DMA_BM_DATA_SIZE_BYTE : ((es == 2U) ? DMA_BM_DATA_SIZE_HALFWORD : DMA_BM_DATA_SIZE_WORD);
    const DmaBm_ConfigEx_t d = {
        .base = { .stream = s, .request_id = req, .direction = tx ? DMA_BM_DIR_MEM_TO_PERIPH : DMA_BM_DIR_PERIPH_TO_MEM,
                  .periph_data_size = sz, .mem_data_size = sz, .periph_inc = false, .mem_inc = true,
                  .circular = false, .priority = DMA_BM_PRIORITY_HIGH, .enable_it_ht = false, .enable_it_tc = false,
                  .enable_it_te = false, .enable_it_dme = false, .enable_it_fe = false, .enable_trbuff = false,
                  .enable_fifo = false, .fifo_threshold = DMA_BM_FIFO_TH_1_2,
                  .periph_addr = tx ? (uint32_t)&SPI1->TXDR : (uint32_t)&SPI1->RXDR, .mem_addr = (uint32_t)buf,
                  .buffer_length = n },
        .mem1_addr = 0U, .mem_burst = DMA_BM_BURST_SINGLE, .periph_burst = DMA_BM_BURST_SINGLE,
        .periph_flow_ctrl = false, .fail_if_busy = true, .stop_spin_limit = 0U };
    return DMA_BM_InitStreamEx(&d) && DMA_BM_StartEx(s);
}

static void cmd_xfer(uint32_t xc, uint32_t seed, uint32_t n, uint32_t xm)
{
    volatile uint32_t *r = SST_AREA->m4_res;
    res_clear();
    const uint32_t bits = SST_XCFG_BITS(xc);
    const uint32_t mbr = SST_XCFG_MBR(xc);
    const uint32_t es = sst_elem_bytes(bits);
    if ((n == 0U) || (n > M4_MAXF))
    {
        r[0] = (uint32_t)SPI_BM_ERR_PARAM;
        return;
    }
    const SpiBm_Config_t c = mkcfg(mbr, SST_XCFG_MODE(xc), bits, SST_XCFG_LSB(xc) != 0U, SST_XCFG_DIR(xc),
                                   SST_XCFG_THR(xc));
    sst_fill(s_tx, es, n, bits, seed);
    sst_fill_const(s_rx, es, n, 0xA5A5A5A5UL);
    (void)SPI_BM_EnableClock(SPI1);
    const SpiBm_Status_t si = SPI_BM_Init(&c);
    const uint32_t g = guard_of(mbr);
    const uint32_t bud = budget_of(mbr, bits, n);
    SpiBm_Status_t st = SPI_BM_ERR_PARAM;
    uint32_t irqs = 0U;
    uint32_t after = 0U;
    uint32_t t0 = DWT->CYCCNT;
    if (xm == SST_XM_POLL)
    {
        st = SPI_BM_Transfer(SPI1, s_tx, s_rx, n, g, bud);
    }
    else if (xm == SST_XM_IT)
    {
        nvic_spi1(true);
        s_it.tx = s_tx;
        s_it.rx = s_rx;
        s_it.frames = n;
        s_it.guard_cycles = g;
        s_it_irqs = 0U;
        s_it_active = true;
        t0 = DWT->CYCCNT;
        st = SPI_BM_TransferStartIt(SPI1, &s_it);
        if (st != SPI_BM_OK)
        {
            s_it_active = false;
        }
        while (s_it_active && ((DWT->CYCCNT - t0) < bud))
        {
        }
        if (st == SPI_BM_OK)
        {
            st = s_it_active ? SPI_BM_ERR_TIMEOUT : s_it.status;
        }
        s_it_active = false;
        irqs = s_it_irqs;
        delay_cyc(s_cpu_hz / 100U);                 /* T6(c): 10 ms with no SPI1 IRQ */
        after = s_it_irqs - irqs;
        if ((SPI1->CR1 & SPI_CR1_SPE) != 0U)
        {
            after |= 0x80000000UL;
        }
        nvic_spi1(false);
    }
    else
    {
        const bool rxdma = (xm == SST_XM_DMA);
        const SpiBm_Status_t sp = SPI_BM_DmaPrepare(SPI1, n, rxdma);
        bool dok = true;
        if (rxdma)
        {
            dok = m4_stream(M4_RX_STREAM, false, es, s_rx, n);
        }
        dok = dok && m4_stream(M4_TX_STREAM, true, es, s_tx, n);
        t0 = DWT->CYCCNT;
        const SpiBm_Status_t ss = SPI_BM_DmaStart(SPI1);
        st = SPI_BM_WaitEnd(SPI1, bud);
        if (rxdma)
        {
            const uint32_t w0 = DWT->CYCCNT;          /* the last frames leave the RxFIFO */
            while ((DMA_BM_GetRemaining(M4_RX_STREAM) != 0U) && ((DWT->CYCCNT - w0) < (s_cpu_hz / 1000U)))
            {
            }
        }
        (void)DMA_BM_StopEx(M4_TX_STREAM, 0U);       /* 1. the streams */
        if (rxdma)
        {
            (void)DMA_BM_StopEx(M4_RX_STREAM, 0U);
        }
        const SpiBm_Status_t se = SPI_BM_DmaEnd(SPI1, g);   /* 2. SPI, 3. DMA enables */
        (void)DMA_BM_DeinitStream(M4_TX_STREAM);
        if (rxdma)
        {
            (void)DMA_BM_DeinitStream(M4_RX_STREAM);
        }
        if ((st == SPI_BM_OK) && ((sp != SPI_BM_OK) || !dok || (ss != SPI_BM_OK) || (se != SPI_BM_OK)))
        {
            st = SPI_BM_ERR_HAL;
        }
        uint32_t left = 0U;
        for (uint32_t i = 0U; i < n; i++)
        {
            if (sst_get(s_rx, es, i) == (0xA5A5A5A5UL & sst_mask(es * 8U)))
            {
                left++;
            }
        }
        r[8] = left;
    }
    r[4] = DWT->CYCCNT - t0;
    r[7] = (uint32_t)SPI_BM_Deinit(SPI1);
    r[0] = (uint32_t)st;
    r[1] = (SST_XCFG_DIR(xc) == SPI_BM_DIR_FULL_DUPLEX) ? sst_mismatch(s_tx, s_rx, es, n, bits) : 0U;
    r[2] = sst_crc32_frames(s_rx, es, n, bits);
    r[3] = sst_coverage(s_tx, es, n, bits);
    r[5] = irqs;
    r[6] = (uint32_t)si;
    r[9] = after;
}

/* T6(c)'s control: TXPIE = 1 by hand with SPE = 0 (TXP = 1 at reset, 53:3597). */
static void cmd_t6c(void)
{
    volatile uint32_t *r = SST_AREA->m4_res;
    res_clear();
    const SpiBm_Config_t c = mkcfg(5U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    (void)SPI_BM_EnableClock(SPI1);
    (void)SPI_BM_Init(&c);
    nvic_spi1(true);
    s_it_active = false;
    s_it_spurious = 0U;
    SPI1->IER = SPI_IER_TXPIE;
    delay_cyc(s_cpu_hz / 1000U);
    r[0] = s_it_spurious;
    nvic_spi1(false);
    (void)SPI_BM_Deinit(SPI1);
}

/* T9 on SPI1: t(1000) and t(2000) frames, transmit only. The J1 pins are in AF5
   (SST_J1P_LOOP): without its SCK routed, SPI1 never reaches EOT. */
static void cmd_t9(uint32_t mbr, uint32_t bits)
{
    volatile uint32_t *r = SST_AREA->m4_res;
    res_clear();
    const SpiBm_Config_t c = mkcfg(mbr, 0U, bits, false, SPI_BM_DIR_TX_ONLY, 1U);
    sst_fill(s_tx, sst_elem_bytes(bits), 2000U, bits, 0x1234567U);
    (void)SPI_BM_EnableClock(SPI1);
    uint32_t mask = 0U;
    for (uint32_t k = 0U; k < 2U; k++)
    {
        const uint32_t n = (k + 1U) * 1000U;
        (void)SPI_BM_Init(&c);
        const uint32_t t0 = DWT->CYCCNT;
        const SpiBm_Status_t st = SPI_BM_Transfer(SPI1, s_tx, NULL, n, guard_of(mbr), budget_of(mbr, bits, n));
        r[k] = DWT->CYCCNT - t0;
        (void)SPI_BM_Deinit(SPI1);
        if (st != SPI_BM_OK)
        {
            mask |= 1UL << k;
        }
    }
    r[2] = s_cpu_hz;
    r[3] = mask;
}

/* T13: SPI2's clock in CPU2's bank while CPU1's DMA keeps SPI2 sending. */
static void cmd_t13(uint32_t what)
{
    volatile uint32_t *r = SST_AREA->m4_res;
    res_clear();
    if (what == 1U)
    {
        r[0] = (uint32_t)SPI_BM_DisableClock(SPI2);
        r[1] = (uint32_t)SPI_BM_OK;
    }
    else
    {
        r[0] = (uint32_t)SPI_BM_EnableClock(SPI2);
        r[1] = (uint32_t)SPI_BM_DisableClock(SPI2);
    }
    r[2] = RCC_C2->APB1LENR;
}

static void cmd_t14(void)
{
    volatile uint32_t *r = SST_AREA->m4_res;
    res_clear();
    (void)SPI_BM_EnableClock(SPI1);
    r[0] = (uint32_t)SPI_BM_Deinit(SPI1);
    r[1] = reset_diff(SPI1);
}

/* B1 on CPU2: a cold Init, a 1-frame Transfer at /2 (TX only, J1 pins in AF5), and the
   TransferIrq cycles seen by the T6 transfers. */
static void cmd_b1(void)
{
    volatile uint32_t *r = SST_AREA->m4_res;
    res_clear();
    const SpiBm_Config_t c = mkcfg(0U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U);
    (void)SPI_BM_EnableClock(SPI1);
    (void)SPI_BM_Deinit(SPI1);
    s_tx[0] = 0x5AU;
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    uint32_t t0 = DWT->CYCCNT;
    (void)SPI_BM_Init(&c);
    r[0] = DWT->CYCCNT - t0;
    t0 = DWT->CYCCNT;
    (void)SPI_BM_Transfer(SPI1, s_tx, NULL, 1U, guard_of(0U), budget_of(0U, 8U, 1U));
    r[1] = DWT->CYCCNT - t0;
    __set_PRIMASK(pm);
    (void)SPI_BM_Deinit(SPI1);
    r[2] = s_b1_irq_min;
    r[3] = s_b1_irq_max;
}

static void copy_str(volatile char *dst, uint32_t n, const char *src)
{
    uint32_t i = 0U;
    for (; (i + 1U < n) && (src[i] != '\0'); i++)
    {
        dst[i] = src[i];
    }
    for (; i < n; i++)
    {
        dst[i] = '\0';
    }
}

void SPI_Selftest_CM4_Run(void)
{
    SstArea_t *a = SST_AREA;

    /* CPU1 opened the session before releasing this core. */
    if (a->magic != SST_AREA_MAGIC)
    {
        return;
    }
    const uint32_t session = a->session;
    bench_safe();
    f1_clocks();
    copy_str(a->m4_rev, sizeof(a->m4_rev), FW_GIT_REV);
    copy_str(a->m4_build_id, sizeof(a->m4_build_id), FW_BUILD_ID);
    a->m4_version = SPI_BENCH_VERSION;
    uint32_t last_seq = a->cmd_seq;
    __DMB();
    a->m4_ready = session;

    for (;;)
    {
        const uint32_t t0 = HAL_GetTick();
        while (a->cmd_seq == last_seq)
        {
            if ((HAL_GetTick() - t0) > SST_CM4_IDLE_TIMEOUT_MS)
            {
                a->m4_timeout = 1U;
                bench_safe();
                return;
            }
        }
        __DMB();
        last_seq = a->cmd_seq;
        const uint32_t cmd = a->cmd;
        const uint32_t a0  = a->arg0;
        const uint32_t a1  = a->arg1;
        const uint32_t a2  = a->arg2;
        const uint32_t a3  = a->arg3;

        switch (cmd)
        {
        case SST_CMD_CLAIMS:
            if (a0 < SST_M4_SLOTS)
            {
                run_claims(&a->m4_claims[a0]);
            }
            break;
        case SST_CMD_SAFE:
            run_safe(&a->m4_safe);
            break;
        case SST_CMD_SPI1:
            spi1_cases(&a->m4_spi1);
            break;
        case SST_CMD_T1:
            cmd_t1();
            break;
        case SST_CMD_T2:
            cmd_t2();
            break;
        case SST_CMD_J1PINS:
            cmd_j1pins(a0, a1, a2);
            break;
        case SST_CMD_XFER:
            cmd_xfer(a0, a1, a2, a3);
            break;
        case SST_CMD_T6C:
            cmd_t6c();
            break;
        case SST_CMD_T9:
            cmd_t9(a0, a1);
            break;
        case SST_CMD_T13:
            cmd_t13(a0);
            break;
        case SST_CMD_T14:
            cmd_t14();
            break;
        case SST_CMD_B1:
            cmd_b1();
            break;
        case SST_CMD_DONE:
            bench_safe();
            __DMB();
            a->ack_seq = last_seq;
            return;
        default:
            break;
        }
        __DMB();
        a->ack_seq = last_seq;
    }
}
