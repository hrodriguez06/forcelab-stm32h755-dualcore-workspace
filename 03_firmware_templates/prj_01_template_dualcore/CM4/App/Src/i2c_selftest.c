/**
  ******************************************************************************
  * @file    i2c_selftest.c
  * @brief   05_i2c selftest, CPU2 side (preset selftest-i2c), phase F0.
  *
  *          CPU2 has no console. It publishes its Rev, build id and step-table
  *          version in the SRAM4 block (Common/Inc/i2c_selftest.h), serves the
  *          commands CPU1 writes there, and CPU1 prints and judges the results.
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
#include "stm32h7xx_hal.h"

/* CPU1 sends the next command within seconds; a minute of silence means it
   stopped (a fault, or a bench abort that skipped the DONE). CPU2 then goes to
   S2 and on to the application loop instead of waiting for ever. */
#define IST_CM4_IDLE_TIMEOUT_MS   60000U
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
        case IST_CMD_DONE:
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
