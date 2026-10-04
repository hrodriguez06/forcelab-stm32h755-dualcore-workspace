/**
  ******************************************************************************
  * @file    spi_selftest.c
  * @brief   06_spi selftest, CPU2 side (preset selftest-spi), phase F0.
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
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "stm32h7xx_hal.h"

/* CPU1 sends the next command within seconds; a minute of silence means it
   stopped (a fault, or a bench abort that skipped the DONE). CPU2 then goes to
   S2 and on to the application loop instead of waiting for ever. */
#define SST_CM4_IDLE_TIMEOUT_MS   60000U

#define PA_J1_SCK    5U
#define PA_J1_MISO   6U
#define PB_J1_MISO   4U
#define PB_J1_MOSI   5U

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
        st = GPIO_BM_ConfigAlternateEx(port, pin, af, GPIO_BM_OTYPE_PP, pull, GPIO_BM_SPEED_LOW);
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
