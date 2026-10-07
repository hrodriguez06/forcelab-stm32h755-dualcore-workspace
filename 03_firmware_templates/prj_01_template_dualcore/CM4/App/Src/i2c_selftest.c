/**
  ******************************************************************************
  * @file    i2c_selftest.c
  * @brief   05_i2c selftest, CPU2 side (preset selftest-i2c), phases F0 and F1.
  *
  *          CPU2 has no console. It publishes its Rev, build id and step-table
  *          version in the SRAM4 block (Common/Inc/i2c_selftest.h), serves the
  *          commands CPU1 writes there, and CPU1 prints and judges the results.
  *          F1: i2c_bm on I2C1 (CPU2's instance, plan decision 4): its pins
  *          (AF4, step F1_CM4 only), Init/Deinit, transfers by polling, by
  *          interrupt (the vectors below, priority 6) and by DMA2 through dma_bm,
  *          and T1, T2, T12, T13 and B1.
  *
  *          Electrical safety: the only pins CPU2 ever takes out of input are
  *          the two bus pins PB8/PB9, in AF4 (I2C1) open-drain without pull,
  *          and only in a step whose bus owner is CPU2 (S15, S16). bench_claim()
  *          judges every AF claim, and every pull on a bus or tap pin, against
  *          the step CPU1 published (ist_line_rule(), S4), then reads I2C1
  *          (clocked in this core's RCC bank, PE = 0) and the present state of
  *          both bus pins (ist_s16_pins_ok()). pin_cfg() is the only caller of
  *          the GPIO driver for the bus pins. bench_safe() puts PB8/PB9 back in
  *          input (open-drain type, no pull, AF0) and I2C1, if clocked in this
  *          bank, at CR1 = 0 (PE = 0 releases SCL and SDA, RM0399 50:429-431).
  *          Every wait is bounded and ends if CYCCNT stops (lesson m1).
  ******************************************************************************
  */

#include "i2c_selftest.h"
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "i2c_bm.h"
#include "dma_bm.h"
#include "stm32h7xx_hal.h"

/* CPU1 sends the next command within seconds; a minute of silence means it
   stopped (a fault, or a bench abort that skipped the DONE). CPU2 then goes to
   S2 and on to the application loop instead of waiting for ever. */
#define IST_CM4_IDLE_TIMEOUT_MS   120000U
#define CYC_PER_MS_CM4            200000U     /* CPU2 at 200 MHz (CM7/Core/Src/main.c) */
#define STALL_READS               1000U

/* ---- Bounded waits (C13, lesson m1) ---------------------------------------------------- */

/* Polls until `done()` or `ms` milliseconds of CYCCNT; false on the limit or
   when CYCCNT read the same value STALL_READS times in a row (counted). */
static bool wait_until(bool (*done)(void), uint32_t ms)
{
    uint32_t last = DWT->CYCCNT;
    uint32_t mark = last;
    uint32_t same = 0U;
    uint32_t elapsed = 0U;
    for (;;)
    {
        if ((done != NULL) && done())
        {
            return true;
        }
        const uint32_t now = DWT->CYCCNT;
        if (now == last)
        {
            if (++same > STALL_READS)
            {
                IST_AREA->m4_stalls++;
                return false;
            }
        }
        else
        {
            same = 0U;
            last = now;
        }
        if ((now - mark) >= CYC_PER_MS_CM4)
        {
            mark += CYC_PER_MS_CM4;
            if (++elapsed >= ms)
            {
                return false;
            }
        }
    }
}

/* ---- I2C1, CPU2's instance -------------------------------------------------------------- */

static bool i2c1_clocked(void)
{
    return (RCC_C2->APB1LENR & RCC_APB1LENR_I2C1EN) != 0U;   /* CPU2's bank (RM0399 9.7.1) */
}

/* CR1 = 0: IE, DMA and PE off. PE = 0 must last 3 APB cycles (50:441-445):
   read back. */
static void i2c_release(I2C_TypeDef *i2c)
{
    i2c->CR1 = 0U;
    for (uint32_t i = 0U; i < 3U; i++)
    {
        (void)i2c->CR1;
    }
    __DSB();
}

static void i2c1_clock(bool on)
{
    if (on)
    {
        RCC->APB1LENR |= RCC_APB1LENR_I2C1EN;     /* CPU2's bank */
    }
    else
    {
        RCC->APB1LENR &= ~RCC_APB1LENR_I2C1EN;
    }
    (void)RCC->APB1LENR;
}

/* ---- S2 ------------------------------------------------------------------------------- */

/* PB8, PB9 in input without pull, open-drain type, AF0; then I2C1, if clocked
   in this bank, released. Raw writes inside the cross-core lock (taken if it
   can be; the writes happen regardless, interrupts masked). */
static void bench_safe(void)
{
    GpioBm_Section_t s;
    const bool locked = (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK);
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOBEN;          /* CPU2's bank (RM0399 9.5.10) */
    (void)RCC->AHB4ENR;
    const uint32_t m2 = (3UL << (IST_PIN_SCL * 2U)) | (3UL << (IST_PIN_SDA * 2U));
    GPIOB->MODER   = GPIOB->MODER & ~m2;
    GPIOB->PUPDR   = GPIOB->PUPDR & ~m2;
    GPIOB->OTYPER  = GPIOB->OTYPER | IST_BUS_MASK;
    GPIOB->AFR[1] &= ~((0xFUL << ((IST_PIN_SCL - 8U) * 4U)) | (0xFUL << ((IST_PIN_SDA - 8U) * 4U)));
    __DSB();
    if (i2c1_clocked())
    {
        i2c_release(I2C1);
    }
    __set_PRIMASK(pm);
    if (locked)
    {
        GPIO_BM_SectionExit(&s);
    }
}

static void bench_abort(void)
{
    IST_AREA->m4_aborts++;
    bench_safe();
}

/* ---- S4, S15, S16 ---------------------------------------------------------------------- */

static bool is_s15_pin(const GPIO_TypeDef *port, uint32_t pin)
{
    return ((port == GPIOB) && ((pin == IST_PIN_SCL) || (pin == IST_PIN_SDA))) ||
           ((port == GPIOF) && ((pin == IST_PIN_TAP_SCL) || (pin == IST_PIN_TAP_SDA)));
}

/* All or nothing, against the step CPU1 published in the block; for a bus pin
   in AF, I2C1 read back (clocked in this bank, PE = 0) and both bus pins free
   of any other AF or output (S16). Reads only. */
static bool bench_claim(const GPIO_TypeDef *port, uint32_t mask, uint32_t mode, uint32_t af,
                        uint32_t otype, uint32_t pull)
{
    const uint32_t step = IST_AREA->step;
    if ((mask & 0xFFFFUL) == 0U)
    {
        return false;
    }
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if (((mask & (1UL << p)) != 0U) && !ist_line_rule(1U, step, port, p, mode, af, otype, pull))
        {
            return false;
        }
    }
    if ((port == GPIOB) && ((mask & IST_BUS_MASK) != 0U) && (mode != GPIO_BM_MODE_INPUT))
    {
        return i2c1_clocked() && ((I2C1->CR1 & I2C_CR1_PE) == 0U) &&
               ist_s16_pins_ok(GPIOB->MODER, GPIOB->AFR[1], af);
    }
    return true;
}

/* The only path to the GPIO driver for a pin of this selftest. An AF, an
   output, or a pull on a bus or tap pin is claimed first. */
static bool pin_cfg(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af, uint32_t otype, uint32_t pull)
{
    GpioBm_Status_t st;
    if ((mode != GPIO_BM_MODE_INPUT) || ((pull != GPIO_BM_PULL_NONE) && is_s15_pin(port, pin)))
    {
        if (!bench_claim(port, 1UL << pin, mode, af, otype, pull))
        {
            bench_abort();
            return false;
        }
    }
    if (mode == GPIO_BM_MODE_AF)
    {
        st = GPIO_BM_ConfigAlternateEx(port, pin, af, otype, pull, GPIO_BM_SPEED_LOW);
    }
    else if (mode == GPIO_BM_MODE_INPUT)
    {
        st = GPIO_BM_ConfigInput(port, pin, pull);
    }
    else
    {
        st = GPIO_BM_ERR_PARAM;    /* CPU2 makes no pin of this bench a GPIO output in F0 */
    }
    if (st != GPIO_BM_OK)
    {
        bench_abort();
        return false;
    }
    return true;
}

/* ---- T0b on CPU2 ------------------------------------------------------------------------ */

typedef struct { GPIO_TypeDef *port; uint16_t mask; uint8_t mode, af, otype, pull; } Claim_t;

#define I  GPIO_BM_MODE_INPUT
#define O  GPIO_BM_MODE_OUTPUT
#define A  GPIO_BM_MODE_AF
#define OD GPIO_BM_OTYPE_OD
#define PP GPIO_BM_OTYPE_PP
#define NP GPIO_BM_PULL_NONE
#define PU GPIO_BM_PULL_UP
#define PD GPIO_BM_PULL_DOWN
#define B8 (1U << IST_PIN_SCL)
#define B9 (1U << IST_PIN_SDA)
/* Case k sets bit k of the result when accepted. Expected, by hand, in
   i2c_selftest.h: cases 0..2 and 19 in step T0B (bus = CPU2), only 19 in
   T0B_CM7 and between steps. They run with I2C1 clocked in CPU2's bank at its
   reset values (run_claims()), so the AF4 cases depend on the step alone. */
static const Claim_t k_claims[IST_M4_CLAIM_CASES] = {
    { GPIOB, B8, A, 4U, OD, NP },               /*  0 PB8 I2C1_SCL                             */
    { GPIOB, B9, A, 4U, OD, NP },               /*  1 PB9 I2C1_SDA                             */
    { GPIOB, B8 | B9, A, 4U, OD, NP },          /*  2 both                                     */
    { GPIOB, B8, A, 4U, PP, NP },               /*  3 push-pull (S15)                          */
    { GPIOB, B8, A, 4U, OD, PU },               /*  4 pull-up (S15)                            */
    { GPIOB, B9, A, 4U, OD, PD },               /*  5 pull-down (S15)                          */
    { GPIOB, B8, A, 6U, OD, NP },               /*  6 AF6, CPU1's I2C4 (S16)                   */
    { GPIOB, B8, A, 1U, OD, NP },               /*  7 AF1 TIM16_CH1                            */
    { GPIOB, B8, O, 0U, OD, NP },               /*  8 GPIO open-drain: recovery steps only     */
    { GPIOB, B9, O, 0U, PP, NP },               /*  9 GPIO push-pull (S15)                     */
    { GPIOB, B9, I, 0U, OD, PU },               /* 10 input with pull-up (S15)                 */
    { GPIOF, 1U << IST_PIN_TAP_SCL, A, 4U, OD, NP },   /* 11 PF14 I2C4_SCL: taps input only    */
    { GPIOF, 1U << IST_PIN_TAP_SDA, I, 0U, OD, PD },   /* 12 PF15 pull-down (S15)              */
    { GPIOF, 1U << IST_PIN_TAP_SCL, O, 0U, OD, NP },   /* 13 PF14 output                       */
    { GPIOE, 1U << 7, O, 0U, PP, NP },          /* 14 PE7 MCU_ACK (S4)                         */
    { GPIOE, 1U << 12, A, 5U, PP, NP },         /* 15 PE12 SPI4_SCK (S13)                      */
    { GPIOB, 1U << 5, A, 5U, PP, NP },          /* 16 PB5 SPI1_MOSI, J1 (S14)                  */
    { GPIOA, 1U << 5, O, 0U, PP, NP },          /* 17 PA5, J1 (S14)                            */
    { GPIOB, B8 | (1U << 7), A, 4U, OD, NP },   /* 18 PB8 with PB7 (I2C1_SDA elsewhere)        */
    { GPIOB, B8, I, 0U, OD, NP },               /* 19 control: plain input                     */
};
_Static_assert(IST_M4_CLAIM_CASES <= 32U, "k_claims: more cases than result bits (audit F0, m10)");
#undef I
#undef O
#undef A
#undef OD
#undef PP
#undef NP
#undef PU
#undef PD
#undef B8
#undef B9

/* The configuration registers of GPIOA, GPIOB, GPIOE and GPIOF, read raw: a
   refused claim writes none of them (the same oracle as CPU1's port_snap). */
#define SNAP_REGS   24U
typedef struct { uint32_t r[SNAP_REGS]; } PortSnap_t;

static void port_snap(PortSnap_t *s)
{
    const GPIO_TypeDef *const ports[4] = { GPIOA, GPIOB, GPIOE, GPIOF };
    uint32_t i = 0U;
    for (uint32_t k = 0U; k < 4U; k++)
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

/* I2C1 clocked in this bank at its reset values; returns whether it was on. */
static bool i2c1_on_at_reset(void)
{
    const bool was_on = i2c1_clocked();
    i2c1_clock(true);
    i2c_release(I2C1);
    return was_on;
}

static void i2c1_clock_back(bool was_on)
{
    if (!was_on)
    {
        i2c_release(I2C1);
        i2c1_clock(false);
    }
}

static void run_claims(IstClaimResult_t *r)
{
    const bool was_on = i2c1_on_at_reset();
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    uint32_t acc = 0U;
    r->step_seen = IST_AREA->step;
    for (uint32_t k = 0U; k < IST_M4_CLAIM_CASES; k++)
    {
        const Claim_t *c = &k_claims[k];
        if (bench_claim(c->port, c->mask, c->mode, c->af, c->otype, c->pull))
        {
            acc |= 1UL << k;
        }
    }
    port_snap(&after);
    r->cases        = IST_M4_CLAIM_CASES;
    r->accepted     = acc;
    r->regs_changed = port_snap_diff(&before, &after);
    i2c1_clock_back(was_on);
}

/* T0b, S16 with the instance read (lesson R2-3): the I2C1 states of
   IST_I2C_CASES against a claim of PB8 in AF4, in step T0B. The bus pins stay
   in input, so no state reaches a pin. */
static void i2c1_cases(IstI2cCaseResult_t *r)
{
    const bool was_on = i2c1_on_at_reset();
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    uint32_t acc = 0U;
    r->pe_seen = 0U;
    for (uint32_t k = 0U; k < IST_I2C_CASES; k++)
    {
        i2c1_clock(true);
        i2c_release(I2C1);
        if (k == 0U)
        {
            I2C1->CR1 = I2C_CR1_PE;                       /* PE = 1: refuse */
            r->pe_seen = ((I2C1->CR1 & I2C_CR1_PE) != 0U) ? 1U : 0U;
        }
        else if (k == 1U)
        {
            i2c1_clock(false);                            /* clock off in this bank: refuse */
        }
        else if (k == 2U)
        {
            I2C1->CR1 = I2C_CR1_ANFOFF;                   /* PE = 0, other bits set: accept */
        }
        else
        {
            /* reset values: accept */
        }
        if (bench_claim(GPIOB, 1UL << IST_PIN_SCL, GPIO_BM_MODE_AF, IST_AF_I2C1, GPIO_BM_OTYPE_OD,
                        GPIO_BM_PULL_NONE))
        {
            acc |= 1UL << k;
        }
        i2c1_clock(true);
        i2c_release(I2C1);
    }
    port_snap(&after);
    r->cases        = IST_I2C_CASES;
    r->accepted     = acc;
    r->regs_changed = port_snap_diff(&before, &after);
    r->cr1_after    = I2C1->CR1;
    i2c1_clock_back(was_on);
    r->clk_same     = (i2c1_clocked() == was_on) ? 1U : 0U;
}

static void photo(IstPinPhoto_t *p)
{
    const uint32_t s8 = IST_PIN_SCL;
    const uint32_t s9 = IST_PIN_SDA;
    p->moder  = ((GPIOB->MODER >> (s8 * 2U)) & 3UL) | (((GPIOB->MODER >> (s9 * 2U)) & 3UL) << 2);
    p->otyper = ((GPIOB->OTYPER >> s8) & 1UL) | (((GPIOB->OTYPER >> s9) & 1UL) << 1);
    p->pupdr  = ((GPIOB->PUPDR >> (s8 * 2U)) & 3UL) | (((GPIOB->PUPDR >> (s9 * 2U)) & 3UL) << 2);
    p->afr    = ((GPIOB->AFR[1] >> ((s8 - 8U) * 4U)) & 0xFUL) | (((GPIOB->AFR[1] >> ((s9 - 8U) * 4U)) & 0xFUL) << 4);
    p->idr    = ((GPIOB->IDR >> s8) & 1UL) | (((GPIOB->IDR >> s9) & 1UL) << 1);
    p->i2c_cr1 = i2c1_clocked() ? I2C1->CR1 : 0xFFFFFFFFUL;
    p->i2c_isr = i2c1_clocked() ? I2C1->ISR : 0xFFFFFFFFUL;
}

static bool s_af_clk_was_on;

/* T0b, the accepted case written (step T0B, bus = CPU2): I2C1 clocked in this
   bank at its reset values (PE = 0, so it lets go of SCL and SDA, 50:429-431),
   then PB8 and PB9 to AF4 open-drain without pull, through the claim. Nothing
   drives the bus: CPU1 holds the step while the host reads the levels. */
static void run_af_on(IstPinPhoto_t *p)
{
    s_af_clk_was_on = i2c1_on_at_reset();
    p->claim_ok = (pin_cfg(GPIOB, IST_PIN_SCL, GPIO_BM_MODE_AF, IST_AF_I2C1, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE) &&
                   pin_cfg(GPIOB, IST_PIN_SDA, GPIO_BM_MODE_AF, IST_AF_I2C1, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE))
                  ? 1U : 0U;
    photo(p);
    p->clk_same = 1U;
}

/* CPU2's bench_safe() from the AF4 state of run_af_on(), then a photo; then
   the I2C1 clock back to what it was before T0b. */
static void run_safe(IstPinPhoto_t *p)
{
    bench_safe();
    photo(p);
    i2c1_clock_back(s_af_clk_was_on);
    p->clk_same = (i2c1_clocked() == s_af_clk_was_on) ? 1U : 0U;
    p->claim_ok = 1U;
}

/* ==========================================================================================
 * Phase F1: i2c_bm on I2C1, served for CPU1 (which prints and judges)
 * ========================================================================================== */

#define M4_BUF_N        65536U
#define M4_DMA_TX       DMA2_Stream1      /* DMAMUX1 request i2c1_tx_dma (34)       */
#define M4_DMA_RX       DMA2_Stream0      /* i2c1_rx_dma (33)                        */
#define M4_SENTINEL     0x5AU
#define M4_IRQ_PRIO     6U

/* SRAM2 (RAM_D2_M4): DMA1/DMA2 reach it; CPU2 has no data cache. */
static uint8_t s_buf[M4_BUF_N] __attribute__((aligned(32)));
static uint8_t s_ptr_byte __attribute__((aligned(32)));

static volatile uint32_t *res(void)
{
    return IST_AREA->m4_res;
}

static void res_clear(void)
{
    for (uint32_t i = 0U; i < (sizeof(IST_AREA->m4_res) / sizeof(IST_AREA->m4_res[0])); i++)
    {
        IST_AREA->m4_res[i] = 0U;
    }
}

/* Cycles without CYCCNT stopping (lesson m1): false if it stopped. */
static bool delay_cyc(uint32_t n)
{
    const uint32_t t0 = DWT->CYCCNT;
    uint32_t last = t0;
    uint32_t same = 0U;
    while ((DWT->CYCCNT - t0) < n)
    {
        const uint32_t now = DWT->CYCCNT;
        same = (now == last) ? (same + 1U) : 0U;
        last = now;
        if (same > STALL_READS)
        {
            IST_AREA->m4_stalls++;
            return false;
        }
    }
    return true;
}

static void regs_to(volatile uint32_t *r)
{
    uint32_t g[IST_NREG];
    ist_regs(I2C1, g);
    for (uint32_t k = 0U; k < IST_NREG; k++)
    {
        r[k] = g[k];
    }
}

/* PB8/PB9 in AF4 open-drain without pull through the claim (step F1_CM4, I2C1
   clocked in this bank at PE = 0), or back to input. */
static void cmd_pins(uint32_t on)
{
    volatile uint32_t *r = res();
    res_clear();
    if (on != 0U)
    {
        i2c1_clock(true);
        i2c_release(I2C1);
        r[0] = (pin_cfg(GPIOB, IST_PIN_SCL, GPIO_BM_MODE_AF, IST_AF_I2C1, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE) &&
                pin_cfg(GPIOB, IST_PIN_SDA, GPIO_BM_MODE_AF, IST_AF_I2C1, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE)) ? 1U : 0U;
    }
    else
    {
        bench_safe();
        r[0] = 1U;
    }
}

static void cmd_init(uint32_t timingr, uint32_t filt, uint32_t tmo)
{
    volatile uint32_t *r = res();
    res_clear();
    const I2cBm_Config_t c = { I2C1, timingr, (filt & 1U) != 0U, (filt >> 8) & 0xFFU, tmo };
    (void)I2C_BM_EnableClock(I2C1);
    r[0] = (uint32_t)I2C_BM_Init(&c);
    regs_to(&r[1]);
}

static void cmd_deinit(void)
{
    volatile uint32_t *r = res();
    res_clear();
    r[0] = (uint32_t)I2C_BM_Deinit(I2C1);
    regs_to(&r[1]);
}

static void cmd_regs(void)
{
    volatile uint32_t *r = res();
    res_clear();
    regs_to(r);
    r[7] = i2c1_clocked() ? 1U : 0U;
}

/* ---- Interrupt transfers: the project's vectors for the test (C14), priority 6 ---------- */
static I2cBm_Xfer_t      s_it;
static volatile bool     s_it_active;
static volatile uint32_t s_it_irqs;
static uint32_t          s_irq_min = 0xFFFFFFFFUL;
static uint32_t          s_irq_max;

static void it_serve(void)
{
    s_it_irqs++;
    if (s_it_active)
    {
        const uint32_t t0 = DWT->CYCCNT;
        const bool done = I2C_BM_XferIrq(I2C1, &s_it);
        const uint32_t dt = DWT->CYCCNT - t0;
        s_irq_min = (dt < s_irq_min) ? dt : s_irq_min;
        s_irq_max = (dt > s_irq_max) ? dt : s_irq_max;
        if (done)
        {
            s_it_active = false;
        }
    }
    else
    {
        I2C1->CR1 &= ~(I2C_CR1_TXIE | I2C_CR1_RXIE | I2C_CR1_TCIE | I2C_CR1_STOPIE | I2C_CR1_NACKIE |
                       I2C_CR1_ERRIE | I2C_CR1_ADDRIE);   /* a stray one: silenced */
    }
}

void I2C1_EV_IRQHandler(void)
{
    it_serve();
}

void I2C1_ER_IRQHandler(void)
{
    it_serve();
}

static void nvic_i2c1(bool on)
{
    IRQn_Type ev = I2C1_EV_IRQn;
    IRQn_Type er = I2C1_ER_IRQn;
    (void)I2C_BM_GetIRQn(I2C1, &ev, &er);
    const IRQn_Type q[2] = { ev, er };
    for (uint32_t k = 0U; k < 2U; k++)
    {
        if (on)
        {
            HAL_NVIC_SetPriority(q[k], M4_IRQ_PRIO, 0U);
            NVIC_ClearPendingIRQ(q[k]);
            HAL_NVIC_EnableIRQ(q[k]);
        }
        else
        {
            HAL_NVIC_DisableIRQ(q[k]);
            NVIC_ClearPendingIRQ(q[k]);
        }
    }
}

/* ---- DMA1/DMA2 streams of the caller (02_dma) --------------------------------------------- */
static bool m4_stream(DMA_Stream_TypeDef *s, bool tx, uint8_t *mem, uint32_t n)
{
    uint32_t req = 0U;
    (void)I2C_BM_GetDmaRequest(I2C1, tx, &req);
    const DmaBm_ConfigEx_t d = {
        .base = { .stream = s, .request_id = req, .direction = tx ? DMA_BM_DIR_MEM_TO_PERIPH : DMA_BM_DIR_PERIPH_TO_MEM,
                  .periph_data_size = DMA_BM_DATA_SIZE_BYTE, .mem_data_size = DMA_BM_DATA_SIZE_BYTE,
                  .periph_inc = false, .mem_inc = true, .circular = false, .priority = DMA_BM_PRIORITY_HIGH,
                  .enable_it_ht = false, .enable_it_tc = false, .enable_it_te = false, .enable_it_dme = false,
                  .enable_it_fe = false, .enable_trbuff = false, .enable_fifo = false,
                  .fifo_threshold = DMA_BM_FIFO_TH_1_2,
                  .periph_addr = tx ? (uint32_t)&I2C1->TXDR : (uint32_t)&I2C1->RXDR, .mem_addr = (uint32_t)mem,
                  .buffer_length = n },
        .mem1_addr = 0U, .mem_burst = DMA_BM_BURST_SINGLE, .periph_burst = DMA_BM_BURST_SINGLE,
        .periph_flow_ctrl = false, .fail_if_busy = true, .stop_spin_limit = 0U };
    return DMA_BM_InitStreamEx(&d) && DMA_BM_StartEx(s);
}

static void m4_stream_end(DMA_Stream_TypeDef *s)
{
    (void)DMA_BM_StopEx(s, 0U);
    (void)DMA_BM_DeinitStream(s);
}

static uint32_t sentinel_left(uint32_t n)
{
    uint32_t left = 0U;
    for (uint32_t i = 0U; i < n; i++)
    {
        if (s_buf[i] == M4_SENTINEL)
        {
            left++;
        }
    }
    return left;
}

/* One transfer on I2C1, initialised by M4_INIT. Writes: s_buf[0..n-1] from the
   seed (the first byte is the target's pointer); reads and write-reads: the
   target's known content from pointer `p` (IST_TARGET_REG). */
static void cmd_xfer(uint32_t xf, uint32_t n, uint32_t arg, uint32_t max_cycles)
{
    volatile uint32_t *r = res();
    res_clear();
    const uint32_t op = IST_XF_OP(xf);
    const uint32_t mode = IST_XF_MODE(xf);
    const uint32_t addr = IST_XF_ADDR(xf);
    if ((n > M4_BUF_N) || (max_cycles == 0U))
    {
        r[IST_XR_STATUS] = (uint32_t)I2C_BM_ERR_PARAM;
        return;
    }
    const bool rd = (op == IST_OP_READ) || (op == IST_OP_WRITEREAD);
    if (op == IST_OP_WRITE)
    {
        ist_fill(s_buf, n, arg);
    }
    else
    {
        for (uint32_t i = 0U; i < n; i++)
        {
            s_buf[i] = M4_SENTINEL;
        }
    }
    s_ptr_byte = (uint8_t)arg;
    I2cBm_Status_t st = I2C_BM_ERR_PARAM;
    uint32_t done_tx = 0U;
    uint32_t done_rx = 0U;
    uint32_t aux = 0U;
    uint32_t t0 = DWT->CYCCNT;
    if (mode == IST_MODE_POLL)
    {
        if (op == IST_OP_WRITE)
        {
            st = I2C_BM_Write(I2C1, addr, s_buf, n, max_cycles);
        }
        else if (op == IST_OP_READ)
        {
            st = I2C_BM_Read(I2C1, addr, s_buf, n, max_cycles);
        }
        else if (op == IST_OP_WRITEREAD)
        {
            st = I2C_BM_WriteRead(I2C1, addr, &s_ptr_byte, 1U, s_buf, n, max_cycles);
        }
        else
        {
            st = I2C_BM_Write(I2C1, addr, NULL, 0U, max_cycles);
        }
        done_tx = (op == IST_OP_WRITE) ? n : ((op == IST_OP_WRITEREAD) ? 1U : 0U);
        done_rx = rd ? n : 0U;
    }
    else if ((mode == IST_MODE_IT) || (mode == IST_MODE_IT_CTRL))
    {
        s_it.addr7 = addr;
        s_it.tx = (op == IST_OP_WRITE) ? s_buf : &s_ptr_byte;
        s_it.ntx = (op == IST_OP_WRITE) ? n : ((op == IST_OP_WRITEREAD) ? 1U : 0U);
        s_it.rx = s_buf;
        s_it.nrx = rd ? n : 0U;
        nvic_i2c1(true);
        s_it_irqs = 0U;
        s_it_active = true;
        t0 = DWT->CYCCNT;
        st = I2C_BM_StartIt(I2C1, &s_it);
        if (mode == IST_MODE_IT_CTRL)
        {
            /* T6's control: every enable cleared by hand before the first event. */
            const uint32_t pm = __get_PRIMASK();
            __disable_irq();
            I2C1->CR1 &= ~(I2C_CR1_TXIE | I2C_CR1_RXIE | I2C_CR1_TCIE | I2C_CR1_STOPIE | I2C_CR1_NACKIE | I2C_CR1_ERRIE);
            __set_PRIMASK(pm);
        }
        if (st != I2C_BM_OK)
        {
            s_it_active = false;
        }
        uint32_t last = DWT->CYCCNT;
        uint32_t same = 0U;
        while (s_it_active && ((DWT->CYCCNT - t0) < max_cycles))
        {
            const uint32_t now = DWT->CYCCNT;
            same = (now == last) ? (same + 1U) : 0U;
            last = now;
            if (same > STALL_READS)
            {
                IST_AREA->m4_stalls++;
                break;
            }
        }
        const bool timed_out = s_it_active;
        s_it_active = false;
        if (st == I2C_BM_OK)
        {
            st = timed_out ? I2C_BM_ERR_TIMEOUT : s_it.status;
        }
        r[IST_XR_IRQS] = s_it_irqs;
        done_tx = s_it.done_tx;
        done_rx = s_it.done_rx;
        r[IST_XR_CYCLES] = DWT->CYCCNT - t0;
        const uint32_t before = s_it_irqs;
        (void)delay_cyc(SystemCoreClock / 100U);          /* T6: 10 ms with no I2C1 IRQ */
        r[IST_XR_IRQS_AFTER] = s_it_irqs - before;
        nvic_i2c1(false);
        if (timed_out)
        {
            aux = (uint32_t)s_it.status;                   /* still PENDING: no progress */
        }
    }
    else
    {
        /* DMA (plan rule 8): the stream first, then DmaStart, WaitEnd, the stream
           stopped, DmaEnd. */
        bool sok;
        if (op == IST_OP_WRITE)
        {
            sok = m4_stream(M4_DMA_TX, true, &s_buf[1], n - 1U);
            t0 = DWT->CYCCNT;
            const I2cBm_Status_t s1 = I2C_BM_DmaStartWrite(I2C1, addr, s_buf, n);
            st = (s1 == I2C_BM_OK) ? I2C_BM_WaitEnd(I2C1, n, max_cycles) : s1;
            m4_stream_end(M4_DMA_TX);
            done_tx = n;
        }
        else
        {
            sok = m4_stream(M4_DMA_RX, false, s_buf, n);
            t0 = DWT->CYCCNT;
            const I2cBm_Status_t s1 = I2C_BM_DmaStartRead(I2C1, addr, n);
            if ((s1 == I2C_BM_OK) && (mode == IST_MODE_DMA_CTRL))
            {
                const uint32_t pm = __get_PRIMASK();      /* T7's control: RXDMAEN cleared by hand */
                __disable_irq();
                I2C1->CR1 &= ~I2C_CR1_RXDMAEN;
                __set_PRIMASK(pm);
            }
            st = (s1 == I2C_BM_OK) ? I2C_BM_WaitEnd(I2C1, n, max_cycles) : s1;
            const uint32_t w0 = DWT->CYCCNT;               /* the last byte leaves RXDR */
            while ((DMA_BM_GetRemaining(M4_DMA_RX) != 0U) && ((DWT->CYCCNT - w0) < (SystemCoreClock / 1000U)))
            {
            }
            done_rx = n - DMA_BM_GetRemaining(M4_DMA_RX);
            m4_stream_end(M4_DMA_RX);
        }
        const I2cBm_Status_t se = I2C_BM_DmaEnd(I2C1);
        aux = (sok ? 0U : 1U) | ((se == I2C_BM_OK) ? 0U : 2U);
        r[IST_XR_CYCLES] = DWT->CYCCNT - t0;
    }
    if ((mode == IST_MODE_POLL))
    {
        r[IST_XR_CYCLES] = DWT->CYCCNT - t0;
    }
    uint32_t first = 0xFFFFFFFFUL;
    r[IST_XR_STATUS] = (uint32_t)st;
    r[IST_XR_CRC] = rd ? ist_crc32(s_buf, n) : ((op == IST_OP_WRITE) ? ist_crc32(s_buf, n) : 0U);
    r[IST_XR_MISM] = rd ? ist_check_target(s_buf, n, arg & 0xFFU, &first) : 0U;
    r[IST_XR_FIRST_BAD] = first;
    r[IST_XR_DONE_TX] = done_tx;
    r[IST_XR_DONE_RX] = done_rx;
    r[IST_XR_CR1_AFTER] = I2C1->CR1;
    r[IST_XR_SENTINEL] = rd ? sentinel_left(n) : 0U;
    r[IST_XR_ISR_AFTER] = I2C1->ISR;
    r[IST_XR_AUX] = aux;
}

static void cmd_t1(void)
{
    volatile uint32_t *r = res();
    static IstT1Result_t t;
    res_clear();
    (void)I2C_BM_EnableClock(I2C1);
    ist_t1(I2C1, IST_AREA->arg0, &t);
    r[0] = t.cases;
    r[1] = t.bad;
    r[2] = t.groups_same;
    r[3] = t.control_changed;
    for (uint32_t i = 0U; (i < t.cases) && (i < IST_T1_MAX); i++)
    {
        r[4U + (i / 4U)] |= ((uint32_t)t.got[i] & 0xFFU) << (8U * (i % 4U));
    }
}

static void cmd_busy(void)
{
    volatile uint32_t *r = res();
    static IstT1Result_t t;
    res_clear();
    ist_t1_busy(I2C1, &t);
    r[0] = t.cases;
    r[1] = t.bad;
    r[2] = t.groups_same;
    r[3] = t.isr;
    for (uint32_t i = 0U; (i < t.cases) && (i < IST_T1_MAX); i++)
    {
        r[4U + (i / 4U)] |= ((uint32_t)t.got[i] & 0xFFU) << (8U * (i % 4U));
    }
}

static void cmd_t2(void)
{
    volatile uint32_t *r = res();
    IstT2Result_t t;
    res_clear();
    (void)I2C_BM_EnableClock(I2C1);
    ist_t2(I2C1, &t);
    r[0] = t.cases;
    r[1] = t.bad;
    r[2] = t.bad_deinit;
    r[3] = t.first_bad;
    r[4] = t.got;
    r[5] = t.exp;
    r[6] = t.control;
    r[7] = t.bad_status;
}

/* T13: this core's bank only (RM0399 9.5.10), I2C2 enabled in CPU1's bank. */
static void cmd_t13(uint32_t what)
{
    volatile uint32_t *r = res();
    res_clear();
    if (what == 2U)
    {
        r[0] = (uint32_t)I2C_BM_EnableClock(I2C2);
    }
    r[1] = (uint32_t)I2C_BM_DisableClock(I2C2);
    r[2] = RCC_C2->APB1LENR;
}

static void cmd_fmp(uint32_t on)
{
    volatile uint32_t *r = res();
    res_clear();
    r[0] = (uint32_t)I2C_BM_SetFastModePlus(I2C1, on != 0U);
    r[1] = SYSCFG->PMCR;
}

/* T12 (ES0445 2.19.6): `count` writes of 2 bytes, I2C1 initialised on the HSI kernel
   clock by CPU1 (APB1 / kernel within 1.5..3). The driver preloads the first byte;
   the manual variants write CR2 themselves without it, the control of the erratum. */
static I2cBm_Status_t t12_manual(bool dma, uint32_t max_cycles)
{
    I2C1->ICR = 0x3F38U;                                 /* every flag of ICR (50:2979-3027) */
    I2C1->ISR = I2C_ISR_TXE;
    if (dma)
    {
        if (!m4_stream(M4_DMA_TX, true, s_buf, 2U))
        {
            return I2C_BM_ERR_HAL;
        }
        I2C1->CR1 |= I2C_CR1_TXDMAEN;
    }
    I2C1->CR2 = ((uint32_t)IST_TARGET_ADDR << 1) | (2UL << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND | I2C_CR2_START;
    uint32_t sent = 0U;
    const uint32_t t0 = DWT->CYCCNT;
    I2cBm_Status_t st = I2C_BM_ERR_TIMEOUT;
    while ((DWT->CYCCNT - t0) < max_cycles)
    {
        const uint32_t isr = I2C1->ISR;
        if (!dma && ((isr & I2C_ISR_TXIS) != 0U) && (sent < 2U))
        {
            I2C1->TXDR = s_buf[sent];                     /* as fast as the CPU can */
            sent++;
        }
        if ((isr & I2C_ISR_NACKF) != 0U)
        {
            st = I2C_BM_ERR_NACK;
            break;
        }
        if ((isr & I2C_ISR_STOPF) != 0U)
        {
            st = I2C_BM_OK;
            break;
        }
    }
    if (dma)
    {
        m4_stream_end(M4_DMA_TX);
        I2C1->CR1 &= ~I2C_CR1_TXDMAEN;
    }
    if (st == I2C_BM_ERR_TIMEOUT)
    {
        /* Stalled: only PE = 0 releases it (ES0445 2.19.6). */
        I2C1->CR1 &= ~I2C_CR1_PE;
        for (uint32_t i = 0U; (i < 1000U) && ((I2C1->CR1 & I2C_CR1_PE) != 0U); i++)
        {
        }
        (void)I2C1->CR1;
        I2C1->CR1 |= I2C_CR1_PE;
    }
    I2C1->ICR = 0x3F38U;
    I2C1->CR2 = 0U;
    return st;
}

static void cmd_t12(uint32_t what, uint32_t count, uint32_t max_cycles)
{
    volatile uint32_t *r = res();
    res_clear();
    uint32_t ok = 0U;
    uint32_t stalled = 0U;
    uint32_t other = 0U;
    uint32_t slow = 0U;
    s_buf[0] = 0xF0U;                                    /* the target's pointer: 0xF0 */
    for (uint32_t k = 0U; k < count; k++)
    {
        s_buf[1] = (uint8_t)k;
        const uint32_t t0 = DWT->CYCCNT;
        I2cBm_Status_t st;
        if (what == IST_T12_POLL_DRIVER)
        {
            st = I2C_BM_Write(I2C1, IST_TARGET_ADDR, s_buf, 2U, max_cycles);
        }
        else if (what == IST_T12_DMA_DRIVER)
        {
            const bool sok = m4_stream(M4_DMA_TX, true, &s_buf[1], 1U);
            st = I2C_BM_DmaStartWrite(I2C1, IST_TARGET_ADDR, s_buf, 2U);
            if (st == I2C_BM_OK)
            {
                st = I2C_BM_WaitEnd(I2C1, 2U, max_cycles);
            }
            m4_stream_end(M4_DMA_TX);
            (void)I2C_BM_DmaEnd(I2C1);
            if (!sok)
            {
                st = I2C_BM_ERR_HAL;
            }
        }
        else
        {
            st = t12_manual(what == IST_T12_DMA_MANUAL, max_cycles);
        }
        const uint32_t dt = DWT->CYCCNT - t0;
        slow = (dt > slow) ? dt : slow;
        if (st == I2C_BM_OK)
        {
            ok++;
        }
        else if (st == I2C_BM_ERR_TIMEOUT)
        {
            stalled++;
        }
        else if (other == 0U)
        {
            other = 0x100U | (uint32_t)st;
        }
        (void)delay_cyc(SystemCoreClock / 20000U);       /* 50 us between transfers */
    }
    r[0] = ok;
    r[1] = stalled;
    r[2] = other;
    r[3] = slow;
}

/* B1: cycles of Init, of a Write of 1 byte, and of each XferIrq call of an IT write. */
static void cmd_b1(uint32_t timingr)
{
    volatile uint32_t *r = res();
    res_clear();
    const I2cBm_Config_t c = { I2C1, timingr, true, 0U, 0U };
    (void)I2C_BM_Deinit(I2C1);
    uint32_t t0 = DWT->CYCCNT;
    (void)I2C_BM_Init(&c);
    r[0] = DWT->CYCCNT - t0;
    s_buf[0] = 0x80U;
    t0 = DWT->CYCCNT;
    r[4] = (uint32_t)I2C_BM_Write(I2C1, IST_TARGET_ADDR, s_buf, 1U, SystemCoreClock / 100U);
    r[1] = DWT->CYCCNT - t0;
    s_irq_min = 0xFFFFFFFFUL;
    s_irq_max = 0U;
    ist_fill(&s_buf[1], 15U, 0x1234567U);
    s_it.addr7 = IST_TARGET_ADDR;
    s_it.tx = s_buf;
    s_it.ntx = 16U;
    s_it.rx = NULL;
    s_it.nrx = 0U;
    nvic_i2c1(true);
    s_it_active = true;
    t0 = DWT->CYCCNT;
    if (I2C_BM_StartIt(I2C1, &s_it) != I2C_BM_OK)
    {
        s_it_active = false;
    }
    while (s_it_active && ((DWT->CYCCNT - t0) < (SystemCoreClock / 100U)))
    {
    }
    s_it_active = false;
    nvic_i2c1(false);
    r[2] = s_irq_min;
    r[3] = s_irq_max;
    r[5] = (uint32_t)s_it.status;
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

static uint32_t s_last_seq;

static bool cmd_arrived(void)
{
    return IST_AREA->cmd_seq != s_last_seq;
}

void I2C_Selftest_CM4_Run(void)
{
    IstArea_t *a = IST_AREA;

    /* CPU1 opened the session before releasing this core. */
    if (a->magic != IST_AREA_MAGIC)
    {
        return;
    }
    const uint32_t session = a->session;
    bench_safe();
    copy_str(a->m4_rev, sizeof(a->m4_rev), FW_GIT_REV);
    copy_str(a->m4_build_id, sizeof(a->m4_build_id), FW_BUILD_ID);
    a->m4_version = I2C_BENCH_VERSION;
    s_last_seq = a->cmd_seq;
    __DMB();
    a->m4_ready = session;

    for (;;)
    {
        if (!wait_until(cmd_arrived, IST_CM4_IDLE_TIMEOUT_MS))
        {
            a->m4_timeout = 1U;
            bench_safe();
            return;
        }
        __DMB();
        s_last_seq = a->cmd_seq;
        const uint32_t cmd = a->cmd;
        const uint32_t a0  = a->arg0;
        const uint32_t a1  = a->arg1;
        const uint32_t a2  = a->arg2;
        const uint32_t a3  = a->arg3;

        switch (cmd)
        {
        case IST_CMD_CLAIMS:
            if (a0 < IST_M4_SLOTS)
            {
                run_claims(&a->m4_claims[a0]);
            }
            break;
        case IST_CMD_I2C1:
            i2c1_cases(&a->m4_i2c1);
            break;
        case IST_CMD_S16:
            a->m4_s16.cases    = IST_S16_CASES;
            a->m4_s16.accepted = ist_s16_cases(IST_AF_I2C1, IST_AF_I2C4);
            break;
        case IST_CMD_AF_ON:
            run_af_on(&a->m4_af);
            break;
        case IST_CMD_SAFE:
            run_safe(&a->m4_safe);
            break;
        case IST_CMD_M4_PINS:
            cmd_pins(a0);
            break;
        case IST_CMD_M4_INIT:
            cmd_init(a0, a1, a2);
            break;
        case IST_CMD_M4_DEINIT:
            cmd_deinit();
            break;
        case IST_CMD_M4_XFER:
            cmd_xfer(a0, a1, a2, a3);
            break;
        case IST_CMD_M4_T1:
            cmd_t1();
            break;
        case IST_CMD_M4_T2:
            cmd_t2();
            break;
        case IST_CMD_M4_T13:
            cmd_t13(a0);
            break;
        case IST_CMD_M4_FMP:
            cmd_fmp(a0);
            break;
        case IST_CMD_M4_T12:
            cmd_t12(a0, a1, a2);
            break;
        case IST_CMD_M4_B1:
            cmd_b1(a0);
            break;
        case IST_CMD_M4_BUSY:
            cmd_busy();
            break;
        case IST_CMD_M4_REGS:
            cmd_regs();
            break;
        case IST_CMD_DONE:
            (void)I2C_BM_Deinit(I2C1);
            bench_safe();
            __DMB();
            a->ack_seq = s_last_seq;
            return;
        default:
            break;
        }
        __DMB();
        a->ack_seq = s_last_seq;
    }
}
