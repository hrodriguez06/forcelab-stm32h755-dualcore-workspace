/**
  ******************************************************************************
  * @file    i2c_selftest.c
  * @brief   05_i2c selftest, CPU1 side (preset selftest-i2c), phase F0: the
  *          bench (AD2, the I2C bus on PB8/PB9, the NUCLEO-F334R8 target). No
  *          driver code yet (F1-F3).
  *
  *          Prints Rev and build id of both cores, then runs the bench steps
  *          T0a, T0c and T0b with the host (tools/i2c_hw_check.py, which also
  *          talks to the F334, reads the scope, checks the identity of every
  *          image and probe, T0d, and runs its guards on mocks, T0b(a)), and
  *          prints the global verdict, CPU2's results included. Plan and
  *          meaning of every test: 01_shared_drivers/05_i2c/IMPLEMENTATION_PLAN.md.
  *
  *          Electrical safety (no series resistors anywhere on the bench):
  *            S2  bench_safe(): every AD2 line, PA8, PB8/PB9 and PF14/PF15 in
  *                input without pull (PE2 pull-down), MCU_ACK low, I2C2, I2C3
  *                and I2C4 released (CR1 = 0) if clocked in this core's bank.
  *                At start, on any abort, on a timeout and at the end.
  *            S4  bench_claim(): the only way a pin of this selftest becomes an
  *                output or AF, or a bus or tap pin gets a pull (pin_cfg() is
  *                the only caller of the GPIO driver for them). The permission
  *                comes from the CURRENT step of k_ist_steps[], which only
  *                step_begin() sets.
  *            S7  every step is announced and waits for the host before it
  *                moves anything; 10 s without the host -> S2 and abort.
  *            S15 PB8/PB9 never push-pull nor with a pull, on either core;
  *                PF14/PF15 input without pull only (ist_line_rule()).
  *            S16 PB8/PB9 go to AF6 (I2C4) only in a step whose bus owner is
  *                CPU1, with I2C4 read back clocked in this bank and PE = 0,
  *                and both bus pins free of any other AF or output; step_end()
  *                fails a step that leaves them out of input or I2C4 enabled.
  *            S17 the bus levels are printed with every STEP announcement and
  *                read by the host before each step (scope, F334, this IDR).
  *          Every wait is bounded and ends if CYCCNT stops (lesson m1); no
  *          wait depends on HAL_GetTick().
  ******************************************************************************
  */

#include "i2c_selftest.h"
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "app_log.h"
#include "stm32h7xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG(fmt, ...)       APP_LOG("[i2c] " fmt, ##__VA_ARGS__)

#define CYC_PER_MS          400000U       /* CPU1 at 400 MHz nominal (+1.3 % with this board's HSE, N1) */
#define CYC_PER_US          400U
#define SYNC_TIMEOUT_MS     10000U
#define CM4_TIMEOUT_MS      30000U
#define CM4_READY_MS        5000U
#define STALL_READS         1000U         /* CYCCNT read equal this often in a row: stopped */

/* T0c: the window CPU1 watches the bus in while the host has the F334 hold a
   line for T0C_HOLD_MS (the host sends the command after MCU_ACK drops). */
#define T0C_WINDOW_MS       600U
#define T0C_HOLD_MS         50U
#define T0C_HOLD_MIN_US     45000U
#define T0C_HOLD_MAX_US     55000U

/* ---- Bench wiring (00_gpio README section 8; 05_i2c plan "Banco de pruebas") ---------- */
#define PE_REQ              2U        /* HOST_REQ, AD2 -> MCU (DIO12) */
#define PE_DIO8             3U        /* DIO8,  AD2 only              */
#define PE_DIO10            4U        /* DIO10, AD2 only              */
#define PE_DIO9             5U        /* DIO9,  MCU only (unused here)*/
#define PE_DIO11            6U        /* DIO11, MCU only (unused here)*/
#define PE_ACK              7U        /* MCU_ACK, MCU -> AD2 (DIO13)  */
#define PA_MCO              8U        /* DIO14, MCU only (MCO1 unused here) */

static const char *const k_own_name[] = { "NONE", "CM7", "CM4" };

static uint32_t  s_step = IST_STEP_NONE;  /* NONE between steps: only plain inputs */
static bool      s_bench_abort;
static uint32_t  s_fail;                  /* firmware-side verdicts that failed */
static uint32_t  s_stalls;                /* waits that ended because CYCCNT stopped */
static char      s_line[400];             /* verdict()/log_long() text; one task, no reentrancy */

/* ---- Bounded waits (C13, lesson m1) ---------------------------------------------------- */

typedef enum { WAIT_DONE = 0, WAIT_LIMIT, WAIT_STALL } WaitResult_t;

/* Polls `done()` (NULL: never done) for up to `ms` milliseconds of CYCCNT;
   WAIT_STALL when CYCCNT read the same value STALL_READS times in a row. */
static WaitResult_t wait_until(bool (*done)(void), uint32_t ms)
{
    uint32_t last = DWT->CYCCNT;
    uint32_t mark = last;
    uint32_t same = 0U;
    uint32_t elapsed = 0U;
    for (;;)
    {
        if ((done != NULL) && done())
        {
            return WAIT_DONE;
        }
        const uint32_t now = DWT->CYCCNT;
        if (now == last)
        {
            if (++same > STALL_READS)
            {
                s_stalls++;
                return WAIT_STALL;
            }
        }
        else
        {
            same = 0U;
            last = now;
        }
        if ((now - mark) >= CYC_PER_MS)
        {
            mark += CYC_PER_MS;
            if (++elapsed >= ms)
            {
                return WAIT_LIMIT;
            }
        }
    }
}

static bool delay_ms(uint32_t ms)
{
    return wait_until(NULL, ms) == WAIT_LIMIT;
}

/* App_Log() cuts a line at APP_LOG_LINE_MAX (160, CM7/App/Src/app_log.c), so
   s_line is printed in pieces of at most LOG_CHUNK characters, cut at a space,
   the next ones starting with "| ", and `suffix` goes on the last piece (as in
   06_spi). */
#define LOG_CHUNK           120U

static void log_text(const char *suffix)
{
    const char *t = s_line;
    size_t n = strlen(t);
    bool first = true;
    while (n > LOG_CHUNK)
    {
        size_t cut = LOG_CHUNK;
        while ((cut > (LOG_CHUNK / 2U)) && (t[cut] != ' '))
        {
            cut--;
        }
        LOG("%s%.*s", first ? "" : "| ", (int)cut, t);
        t += cut;
        n -= cut;
        while ((n > 0U) && (*t == ' '))
        {
            t++;
            n--;
        }
        first = false;
    }
    LOG("%s%s%s", first ? "" : "| ", t, suffix);
}

static void log_long(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_long(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(s_line, sizeof(s_line), fmt, ap);
    va_end(ap);
    log_text("");
}

static void verdict(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void verdict(bool ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(s_line, sizeof(s_line), fmt, ap);
    va_end(ap);
    if (!ok)
    {
        s_fail++;
    }
    log_text(ok ? " -> PASS" : " -> FAIL");
}

static void publish_step(uint32_t idx)
{
    s_step = idx;
    IST_AREA->step = idx;
    __DMB();
}

/* ---- I2C instances of CPU1 (plan decision 4: I2C2, I2C3, I2C4) -------------------------- */

/* Clocks in CPU1's RCC bank (RM0399 9.7.1): the bank this core writes; RCC_C1
   reads it whichever core runs the code. */
static bool i2c4_clocked(void)
{
    return (RCC_C1->APB4ENR & RCC_APB4ENR_I2C4EN) != 0U;
}

static bool i2c2_clocked(void)
{
    return (RCC_C1->APB1LENR & RCC_APB1LENR_I2C2EN) != 0U;
}

static bool i2c3_clocked(void)
{
    return (RCC_C1->APB1LENR & RCC_APB1LENR_I2C3EN) != 0U;
}

static void i2c4_clock(bool on)
{
    if (on)
    {
        RCC->APB4ENR |= RCC_APB4ENR_I2C4EN;      /* CPU1's bank */
    }
    else
    {
        RCC->APB4ENR &= ~RCC_APB4ENR_I2C4EN;
    }
    (void)RCC->APB4ENR;
}

/* CR1 = 0: IE, DMA and PE off. PE = 0 must last 3 APB cycles (50:441-445):
   read back. With PE = 0 the I2C lets go of SCL and SDA (50:429-431). */
static void i2c_release(I2C_TypeDef *i2c)
{
    i2c->CR1 = 0U;
    for (uint32_t i = 0U; i < 3U; i++)
    {
        (void)i2c->CR1;
    }
    __DSB();
}

/* ---- S2: safe state ------------------------------------------------------------------ */

/* Every AD2 line, PA8, the bus pins and the scope taps in input without pull
   (PE2 with pull-down, so HOST_REQ reads 0 while the AD2 does not drive it),
   their AF fields at 0, PB8/PB9 with the open-drain type, MCU_ACK driven low.
   PE0 (plan R4) and PE1 (LD2) are never touched, nor the J1 pins of 06_spi
   (nothing in this image takes them). Pins first; then I2C2, I2C3 and I2C4,
   each if clocked in this bank, at CR1 = 0. Raw writes: the safe state must
   not depend on code under test. The cross-core lock is asked for, and the
   writes happen even if it is refused, with this core's interrupts masked. */
static void bench_safe(void)
{
    GpioBm_Section_t s;
    const bool locked = (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK);
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();

    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOEEN | RCC_AHB4ENR_GPIOFEN;
    (void)RCC->AHB4ENR;

    const uint32_t e_pins = 0xFFFCUL & ~(1UL << PE_ACK);             /* PE2..PE6, PE8..PE15 */
    uint32_t mask2 = 0U;
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if ((e_pins & (1UL << p)) != 0U)
        {
            mask2 |= 3UL << (p * 2U);
        }
    }
    GPIOE->MODER   = GPIOE->MODER & ~mask2;
    GPIOE->PUPDR   = (GPIOE->PUPDR & ~mask2) | (2UL << (PE_REQ * 2U));
    GPIOE->AFR[0] &= 0x000000FFUL;                                    /* PE2..PE7 AF0 */
    GPIOE->AFR[1]  = 0U;                                              /* PE8..PE15 AF0 */

    GPIOE->BSRR   = 1UL << (PE_ACK + 16U);                            /* level first */
    GPIOE->OTYPER = GPIOE->OTYPER & ~(1UL << PE_ACK);
    GPIOE->PUPDR  = GPIOE->PUPDR & ~(3UL << (PE_ACK * 2U));
    GPIOE->MODER  = (GPIOE->MODER & ~(3UL << (PE_ACK * 2U))) | (1UL << (PE_ACK * 2U));

    GPIOA->MODER   = GPIOA->MODER & ~(3UL << (PA_MCO * 2U));
    GPIOA->PUPDR   = GPIOA->PUPDR & ~(3UL << (PA_MCO * 2U));
    GPIOA->AFR[1] &= ~(0xFUL << ((PA_MCO - 8U) * 4U));

    const uint32_t b2 = (3UL << (IST_PIN_SCL * 2U)) | (3UL << (IST_PIN_SDA * 2U));
    GPIOB->MODER   = GPIOB->MODER & ~b2;
    GPIOB->PUPDR   = GPIOB->PUPDR & ~b2;
    GPIOB->OTYPER  = GPIOB->OTYPER | IST_BUS_MASK;
    GPIOB->AFR[1] &= ~((0xFUL << ((IST_PIN_SCL - 8U) * 4U)) | (0xFUL << ((IST_PIN_SDA - 8U) * 4U)));

    const uint32_t f2 = (3UL << (IST_PIN_TAP_SCL * 2U)) | (3UL << (IST_PIN_TAP_SDA * 2U));
    GPIOF->MODER   = GPIOF->MODER & ~f2;
    GPIOF->PUPDR   = GPIOF->PUPDR & ~f2;
    GPIOF->AFR[1] &= ~((0xFUL << ((IST_PIN_TAP_SCL - 8U) * 4U)) | (0xFUL << ((IST_PIN_TAP_SDA - 8U) * 4U)));
    __DSB();

    if (i2c2_clocked())
    {
        i2c_release(I2C2);
    }
    if (i2c3_clocked())
    {
        i2c_release(I2C3);
    }
    if (i2c4_clocked())
    {
        i2c_release(I2C4);
    }

    __set_PRIMASK(pm);
    if (locked)
    {
        GPIO_BM_SectionExit(&s);
    }
}

static void bench_abort(const char *reason)
{
    if (!s_bench_abort)
    {
        s_bench_abort = true;
        s_fail++;
    }
    bench_safe();
    LOG("ABORT step=%s reason=%s", (s_step < k_ist_step_count) ? k_ist_steps[s_step].name : "-", reason);
}

/* ---- S4 + S15 + S16: the claim --------------------------------------------------------- */

static bool is_s15_pin(const GPIO_TypeDef *port, uint32_t pin)
{
    return ((port == GPIOB) && ((pin == IST_PIN_SCL) || (pin == IST_PIN_SDA))) ||
           ((port == GPIOF) && ((pin == IST_PIN_TAP_SCL) || (pin == IST_PIN_TAP_SDA)));
}

/* S16 with the instance read (lesson R2-3): I2C4 clocked in this bank (an
   unclocked I2C4 cannot be read back at all) and PE = 0. Reads only. */
static bool i2c4_at_rest(void)
{
    return i2c4_clocked() && ((I2C4->CR1 & I2C_CR1_PE) == 0U);
}

/* All or nothing: every pin of the mask is judged before anything is written. */
static bool bench_claim(const GPIO_TypeDef *port, uint32_t mask, uint32_t mode, uint32_t af,
                        uint32_t otype, uint32_t pull)
{
    if ((mask & 0xFFFFUL) == 0U)
    {
        return false;
    }
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if (((mask & (1UL << p)) != 0U) && !ist_line_rule(0U, s_step, port, p, mode, af, otype, pull))
        {
            return false;
        }
    }
    if ((port == GPIOB) && ((mask & IST_BUS_MASK) != 0U) && (mode != GPIO_BM_MODE_INPUT))
    {
        return i2c4_at_rest() && ist_s16_pins_ok(GPIOB->MODER, GPIOB->AFR[1], af);
    }
    return true;
}

/* The only path to the GPIO driver for a pin of this selftest. An output or an
   AF, or a pull on a bus or tap pin, is claimed first. */
static bool pin_cfg(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af, uint32_t otype, uint32_t pull)
{
    GpioBm_Status_t st;
    if ((mode != GPIO_BM_MODE_INPUT) || ((pull != GPIO_BM_PULL_NONE) && is_s15_pin(port, pin)))
    {
        if (!bench_claim(port, 1UL << pin, mode, af, otype, pull))
        {
            bench_abort("claim refused");
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
        st = GPIO_BM_ERR_PARAM;    /* no GPIO output in this F0 selftest (MCU_ACK is raw) */
    }
    if (st != GPIO_BM_OK)
    {
        LOG("gpio_bm config of pin %lu failed, status %lu", (unsigned long)pin, (unsigned long)st);
        bench_abort("config");
        return false;
    }
    return true;
}

/* ---- Handshake (S7) --------------------------------------------------------------------- */

static void ack(bool high)
{
    GPIOE->BSRR = high ? (1UL << PE_ACK) : (1UL << (PE_ACK + 16U));
    __DSB();
}

static bool req_high(void)
{
    return ((GPIOE->IDR >> PE_REQ) & 1UL) != 0UL;
}

static bool req_low(void)
{
    return ((GPIOE->IDR >> PE_REQ) & 1UL) == 0UL;
}

static bool wait_req(bool high)
{
    return wait_until(high ? req_high : req_low, SYNC_TIMEOUT_MS) == WAIT_DONE;
}

/* One four-phase handshake: the MCU state is ready when ACK rises; the host
   acts or measures, then raises REQ; ACK drops and the MCU waits for REQ to
   drop. */
static bool sync0(void)
{
    if (s_bench_abort)
    {
        return false;
    }
    ack(true);
    bool ok = wait_req(true);
    ack(false);
    if (ok)
    {
        ok = wait_req(false);
    }
    if (!ok)
    {
        bench_abort("host timeout");
    }
    return ok;
}

/* MCU_ACK stays low and the MCU goes on only once the host has raised and
   dropped HOST_REQ: the host reads a state the MCU holds, so its check does
   not depend on the order in which console lines and DIO reads arrive (06_spi
   audit B1; protocol v1 of this bench). */
static bool sync_ack_low(void)
{
    if (s_bench_abort)
    {
        return false;
    }
    bool ok = wait_req(true);
    if (ok)
    {
        ok = wait_req(false);
    }
    if (!ok)
    {
        bench_abort("host timeout");
    }
    return ok;
}

/* Bus and taps as this core reads them: bit 0 PB8, 1 PB9, 2 PF14, 3 PF15. */
static uint32_t bus_idr(void)
{
    const uint32_t b = GPIOB->IDR;
    const uint32_t f = GPIOF->IDR;
    return ((b >> IST_PIN_SCL) & 1UL) | (((b >> IST_PIN_SDA) & 1UL) << 1) |
           (((f >> IST_PIN_TAP_SCL) & 1UL) << 2) | (((f >> IST_PIN_TAP_SDA) & 1UL) << 3);
}

static bool step_begin(const char *name)
{
    if (s_bench_abort)
    {
        return false;
    }
    const uint32_t idx = ist_step_index(name);
    if (idx == IST_STEP_NONE)
    {
        bench_abort("unknown step");
        return false;
    }
    publish_step(idx);
    const uint32_t v = bus_idr();
    LOG("STEP %s owner=%s v=%u pb8=%lu pb9=%lu pf14=%lu pf15=%lu", k_ist_steps[idx].name,
        k_own_name[k_ist_steps[idx].owner], (unsigned)I2C_BENCH_VERSION, (unsigned long)(v & 1U),
        (unsigned long)((v >> 1) & 1U), (unsigned long)((v >> 2) & 1U), (unsigned long)((v >> 3) & 1U));
    return sync0();   /* nothing moves until the host has checked the announcement and the levels */
}

/* S16: a step hands the bus back with PB8/PB9 in input (or analog) and I2C4
   disabled. Checked here, then forced anyway. */
static void step_end(void)
{
    if (s_step < k_ist_step_count)
    {
        const uint32_t m8 = (GPIOB->MODER >> (IST_PIN_SCL * 2U)) & 3UL;
        const uint32_t m9 = (GPIOB->MODER >> (IST_PIN_SDA * 2U)) & 3UL;
        const bool pins_free = ((m8 == GPIO_BM_MODE_INPUT) || (m8 == GPIO_BM_MODE_ANALOG)) &&
                               ((m9 == GPIO_BM_MODE_INPUT) || (m9 == GPIO_BM_MODE_ANALOG));
        const bool i2c4_off = !i2c4_clocked() || ((I2C4->CR1 & I2C_CR1_PE) == 0U);
        if (!pins_free || !i2c4_off)
        {
            verdict(false, "step %s ended with PB8 MODER %lu, PB9 MODER %lu, I2C4 PE %u", k_ist_steps[s_step].name,
                    (unsigned long)m8, (unsigned long)m9, (unsigned)!i2c4_off);
            bench_abort("bus pins or I2C4 not released at the end of the step");
        }
    }
    publish_step(IST_STEP_NONE);
}

/* ==========================================================================================
 * CPU2 commands
 * ========================================================================================== */

static uint32_t s_cm4_seq;

static bool cm4_acked(void)
{
    return IST_AREA->ack_seq == s_cm4_seq;
}

static bool cm4_send(IstCmd_t cmd, uint32_t a0)
{
    IstArea_t *a = IST_AREA;
    a->cmd  = (uint32_t)cmd;
    a->arg0 = a0;
    __DMB();
    s_cm4_seq = a->cmd_seq + 1U;
    a->cmd_seq = s_cm4_seq;
    __DMB();
    if (wait_until(cm4_acked, CM4_TIMEOUT_MS) != WAIT_DONE)
    {
        LOG("CM4 did not answer command %lu", (unsigned long)cmd);
        s_fail++;
        return false;
    }
    __DMB();
    return true;
}

/* CPU2's claim cases in one slot, against the bit pattern written by hand. */
static void cm4_claims(uint32_t slot, uint32_t expect, uint32_t step_expected, const char *what)
{
    if (cm4_send(IST_CMD_CLAIMS, slot))
    {
        const IstClaimResult_t *r = &IST_AREA->m4_claims[slot];
        verdict((r->cases == IST_M4_CLAIM_CASES) && (r->accepted == expect) && (r->regs_changed == 0U) &&
                (r->step_seen == step_expected),
                "T0B CM4 claim %s: %lu cases, accepted 0x%05lX (0x%05lX), GPIO regs changed 0x%06lX",
                what, (unsigned long)r->cases, (unsigned long)r->accepted, (unsigned long)expect,
                (unsigned long)r->regs_changed);
    }
}

/* ==========================================================================================
 * Claim tables and the "nothing written" oracle
 * ========================================================================================== */

/* Configuration registers of the ports a claim can name: a refused claim writes none. */
#define SNAP_PORTS   4U
typedef struct { uint32_t r[SNAP_PORTS * 6U]; } PortSnap_t;

static void port_snap(PortSnap_t *s)
{
    const GPIO_TypeDef *const ports[SNAP_PORTS] = { GPIOA, GPIOB, GPIOE, GPIOF };
    uint32_t i = 0U;
    for (uint32_t k = 0U; k < SNAP_PORTS; k++)
    {
        s->r[i++] = ports[k]->MODER;
        s->r[i++] = ports[k]->OTYPER;
        s->r[i++] = ports[k]->OSPEEDR;
        s->r[i++] = ports[k]->PUPDR;
        s->r[i++] = ports[k]->AFR[0];
        s->r[i++] = ports[k]->AFR[1];
    }
}

static bool port_snap_eq(const PortSnap_t *a, const PortSnap_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

typedef struct { GPIO_TypeDef *port; uint16_t mask; uint8_t mode, af, otype, pull; } Claim_t;

/* Bit i set = case i was accepted. */
static uint32_t run_claims(const Claim_t *c, uint32_t n)
{
    uint32_t acc = 0U;
    for (uint32_t i = 0U; i < n; i++)
    {
        if (bench_claim(c[i].port, c[i].mask, c[i].mode, c[i].af, c[i].otype, c[i].pull))
        {
            acc |= 1UL << i;
        }
    }
    return acc;
}

#define NCASES(t)   ((uint32_t)(sizeof(t) / sizeof((t)[0])))
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
#define F14 (1U << IST_PIN_TAP_SCL)
#define F15 (1U << IST_PIN_TAP_SDA)

/* T0b, CPU1 in step T0B (bus owner CPU2): every case must be refused. */
static const Claim_t k_t0b_refuse[] = {
    { GPIOB, B8, A, 6U, OD, NP },      { GPIOB, B9, A, 6U, OD, NP },      /* AF6, but the bus is CPU2's (S16) */
    { GPIOB, B8 | B9, A, 6U, OD, NP }, { GPIOB, B8, A, 4U, OD, NP },      /* both; CPU2's AF4 on CPU1         */
    { GPIOB, B8, O, 0U, OD, NP },      { GPIOB, B9, O, 0U, PP, NP },      /* GPIO OD (od_out 0); push-pull    */
    { GPIOB, B8, I, 0U, OD, PU },      { GPIOB, B9, I, 0U, OD, PD },      /* inputs with a pull (S15)         */
    { GPIOF, F14, A, 4U, OD, NP },     { GPIOF, F15, A, 4U, OD, NP },     /* I2C4 on the taps (S15)           */
    { GPIOF, F14, O, 0U, PP, NP },     { GPIOF, F15, I, 0U, OD, PU },     /* tap output; tap pull (S15)       */
    { GPIOE, 1U << 0, O, 0U, PP, NP }, { GPIOE, 1U << 1, O, 0U, PP, NP }, /* PE0 (R4); PE1 LD2                */
    { GPIOE, 1U << PE_REQ, O, 0U, PP, NP }, { GPIOE, 1U << PE_DIO8, O, 0U, PP, NP }, /* HOST_REQ; AD2 only (S4) */
    { GPIOE, 1U << PE_ACK, O, 0U, PP, NP }, { GPIOE, 1U << 8, O, 0U, PP, NP },       /* MCU_ACK; AD2 bus (S4)  */
    { GPIOE, 1U << 12, A, 5U, PP, NP }, { GPIOE, 1U << 11, A, 5U, PP, NP },          /* SPI4 (S13)             */
    { GPIOA, 1U << PA_MCO, A, 0U, PP, NP }, { GPIOA, 1U << 5, A, 8U, PP, NP },       /* MCO1; J1 SPI6 (S14)    */
    { GPIOA, 1U << 6, O, 0U, PP, NP }, { GPIOB, 1U << 5, O, 0U, PP, NP }, /* J1 (S14)                         */
    { GPIOB, 1U << 4, A, 8U, PP, NP }, { GPIOC, 1U << 13, O, 0U, PP, NP },/* J1 (S14); PC13 (R4)              */
    { GPIOA, 1U << 13, A, 0U, PP, NP }, { GPIOA, 1U << 4, A, 5U, PP, NP },/* SWDIO; PA4 (R4)                  */
    { GPIOA, 1U << 7, A, 11U, PP, NP }, { GPIOG, 1U << 13, A, 11U, PP, NP }, /* RMII_CRS_DV; RMII_TXD0        */
    { GPIOB, 1U << 0, O, 0U, PP, NP }, { GPIOB, B8 | (1U << 6), A, 6U, OD, NP }, /* LD1; PB8 with PB6        */
};

/* T0b, CPU1 in step T0B (bus owner CPU2): the controls, all accepted. */
static const Claim_t k_t0b_accept[] = {
    { GPIOB, B8, I, 0U, OD, NP }, { GPIOF, F14 | F15, I, 0U, OD, NP },    /* plain inputs                     */
    { GPIOE, 1U << 8, I, 0U, PP, PU },                                    /* an AD2 line pulled (T0A)         */
};

/* T0b, CPU1 in T0B_CM7 (bus owner CPU1, I2C4 clocked at its reset values):
   cases 0..2 and 14 accepted, the rest refused. */
static const Claim_t k_cm7_claims[] = {
    { GPIOB, B8, A, 6U, OD, NP },      { GPIOB, B9, A, 6U, OD, NP },      /*  0, 1 I2C4_SCL, I2C4_SDA         */
    { GPIOB, B8 | B9, A, 6U, OD, NP },                                    /*  2 both                          */
    { GPIOB, B8, A, 6U, PP, NP },      { GPIOB, B9, A, 6U, OD, PU },      /*  3 push-pull; 4 pull-up (S15)    */
    { GPIOB, B8, A, 6U, OD, PD },      { GPIOB, B8, A, 4U, OD, NP },      /*  5 pull-down; 6 CPU2's AF4       */
    { GPIOB, B9, A, 5U, OD, NP },      { GPIOB, B8, O, 0U, OD, NP },      /*  7 SPI2_NSS; 8 GPIO OD           */
    { GPIOB, B8, I, 0U, OD, PU },      { GPIOF, F14, A, 4U, OD, NP },     /*  9 pull-up; 10 PF14 I2C4_SCL     */
    { GPIOF, F15, A, 4U, OD, NP },     { GPIOF, F14, I, 0U, OD, PD },     /* 11 PF15 I2C4_SDA; 12 tap pull    */
    { GPIOF, F15, O, 0U, OD, NP },     { GPIOB, B8, I, 0U, OD, NP },      /* 13 tap output; 14 plain input    */
};
#define CM7_CLAIMS_EXPECT   0x00004007UL
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
#undef F14
#undef F15

/* ==========================================================================================
 * Bench steps
 * ========================================================================================== */

/* The 13 lines T0a pulls, in DIO order: DIO0..DIO7 (PE8..PE15), DIO8 (PE3),
   DIO9 (PE5), DIO10 (PE4), DIO11 (PE6), DIO14 (PA8). HOST_REQ (DIO12) and
   MCU_ACK (DIO13) are exercised by every handshake. */
typedef struct { GPIO_TypeDef *port; uint8_t pin; uint8_t dio; } Line_t;
static const Line_t k_lines[] = {
    { GPIOE, 8U, 0U }, { GPIOE, 9U, 1U }, { GPIOE, 10U, 2U }, { GPIOE, 11U, 3U },
    { GPIOE, 12U, 4U }, { GPIOE, 13U, 5U }, { GPIOE, 14U, 6U }, { GPIOE, 15U, 7U },
    { GPIOE, PE_DIO8, 8U }, { GPIOE, PE_DIO9, 9U }, { GPIOE, PE_DIO10, 10U }, { GPIOE, PE_DIO11, 11U },
    { GPIOA, PA_MCO, 14U },
};
#define N_LINES      (sizeof(k_lines) / sizeof(k_lines[0]))
#define T0A_CTRL_K   9U     /* the control pulls DIO9 (PE5) the wrong way */

/* Bit k of the result = line k reads 1 on the MCU. */
static uint32_t lines_idr(void)
{
    uint32_t v = 0U;
    for (uint32_t k = 0U; k < N_LINES; k++)
    {
        if (((k_lines[k].port->IDR >> k_lines[k].pin) & 1UL) != 0U)
        {
            v |= 1UL << k;
        }
    }
    return v;
}

/* Pull-up on the lines of `up_mask` (bit k = line k), pull-down on the rest. */
static bool lines_pull(uint32_t up_mask)
{
    for (uint32_t k = 0U; k < N_LINES; k++)
    {
        const uint32_t pull = ((up_mask & (1UL << k)) != 0U) ? GPIO_BM_PULL_UP : GPIO_BM_PULL_DOWN;
        if (!pin_cfg(k_lines[k].port, k_lines[k].pin, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_OTYPE_PP, pull))
        {
            return false;
        }
    }
    return true;
}

/* T0a (as in 06_spi): every line that is not the handshake follows both MCU
   pulls, then a walking 1 (one line up, the others down: two lines shorted
   together, or a line on the wrong DIO, show up), then the control: all up
   except DIO9 pulled down on purpose, which the same check must flag. */
static void step_t0a(void)
{
    const uint32_t all = (1UL << N_LINES) - 1U;
    if (!step_begin("T0A"))
    {
        return;
    }
    uint32_t up = 0U;
    uint32_t dn = 0U;
    uint32_t walk_bad = 0U;
    for (uint32_t phase = 0U; phase < (2U + N_LINES); phase++)
    {
        const uint32_t pattern = (phase == 0U) ? all : ((phase == 1U) ? 0U : (1UL << (phase - 2U)));
        if (!lines_pull(pattern) || !delay_ms(1U))
        {
            return;
        }
        const uint32_t v = lines_idr();
        if (phase == 0U)
        {
            up = v;
        }
        else if (phase == 1U)
        {
            dn = v;
        }
        else if (v != pattern)
        {
            walk_bad |= 1UL << (phase - 2U);
        }
        if (!sync0())
        {
            return;
        }
    }
    if (!lines_pull(all & ~(1UL << T0A_CTRL_K)) || !delay_ms(1U))
    {
        return;
    }
    const uint32_t ctrl = lines_idr();
    if (!sync0())
    {
        return;
    }
    for (uint32_t k = 0U; k < N_LINES; k++)
    {
        (void)pin_cfg(k_lines[k].port, k_lines[k].pin, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE);
    }
    verdict((up == all) && (dn == 0U), "T0A MCU IDR of the 13 lines, pull-up 0x%04lX (0x%04lX), pull-down 0x%04lX (0x0000)",
            (unsigned long)up, (unsigned long)all, (unsigned long)dn);
    verdict(walk_bad == 0U, "T0A walking 1 on 13 lines, MCU IDR: lines that differ 0x%04lX", (unsigned long)walk_bad);
    verdict((ctrl ^ all) == (1UL << T0A_CTRL_K),
            "T0A control: DIO9 (PE5) pulled down on purpose, MCU check flags 0x%04lX (0x%04lX expected)",
            (unsigned long)(ctrl ^ all), (unsigned long)(1UL << T0A_CTRL_K));
    step_end();
}

/* ---- T0c: the bus at rest and the F334's holds, seen by this core ------------------------ */

#define W_PB8   0U
#define W_PB9   1U
#define W_PF14  2U
#define W_PF15  3U
#define W_N     4U

typedef struct
{
    uint32_t lows;           /* falling edges seen (low intervals)          */
    uint32_t low_cyc;        /* CYCCNT cycles spent low, all intervals      */
    uint32_t first_cyc;      /* first fall, cycles after the window opened  */
    bool     low_at_end;
} Watch_t;

/* Watches PB8, PB9, PF14 and PF15 for `ms` milliseconds of CYCCNT, recording
   each fall and rise; false if CYCCNT stopped. Inputs only: nothing moves. */
static bool watch_bus(Watch_t w[W_N], uint32_t ms)
{
    uint32_t level = bus_idr();
    uint32_t fall_at[W_N] = { 0U, 0U, 0U, 0U };
    const uint32_t t0 = DWT->CYCCNT;
    uint32_t last = t0;
    uint32_t same = 0U;
    for (uint32_t k = 0U; k < W_N; k++)
    {
        w[k].lows = 0U;
        w[k].low_cyc = 0U;
        w[k].first_cyc = 0U;
        w[k].low_at_end = false;
        fall_at[k] = t0;              /* a line already low at the start counts from t0 */
    }
    for (;;)
    {
        const uint32_t now = DWT->CYCCNT;
        const uint32_t v = bus_idr();
        if (now == last)
        {
            if (++same > STALL_READS)
            {
                s_stalls++;
                return false;
            }
        }
        else
        {
            same = 0U;
            last = now;
        }
        const uint32_t changed = v ^ level;
        for (uint32_t k = 0U; (changed != 0U) && (k < W_N); k++)
        {
            if ((changed & (1UL << k)) == 0U)
            {
                continue;
            }
            if ((v & (1UL << k)) == 0U)
            {
                if (w[k].lows == 0U)
                {
                    w[k].first_cyc = now - t0;
                }
                w[k].lows++;
                fall_at[k] = now;
            }
            else
            {
                w[k].low_cyc += now - fall_at[k];
            }
        }
        level = v;
        if ((now - t0) >= (ms * CYC_PER_MS))
        {
            for (uint32_t k = 0U; k < W_N; k++)
            {
                if ((level & (1UL << k)) == 0U)
                {
                    w[k].low_cyc += now - fall_at[k];
                    w[k].low_at_end = true;
                }
            }
            return true;
        }
    }
}

static uint32_t cyc_to_us(uint32_t cyc)
{
    return cyc / CYC_PER_US;
}

/* One hold: MCU_ACK rises (ready), the host raises HOST_REQ after arming the
   scope; MCU_ACK drops and this core watches the bus for T0C_WINDOW_MS while
   the host has the F334 hold `line` low for T0C_HOLD_MS. The window then
   closes and the result is printed; the host reads it in the next handshake. */
static void t0c_hold(const char *line, uint32_t held, uint32_t other)
{
    Watch_t w[W_N];
    ack(true);
    if (!wait_req(true))
    {
        ack(false);
        bench_abort("host timeout");
        return;
    }
    ack(false);
    const bool ok = watch_bus(w, T0C_WINDOW_MS);
    if (!ok)
    {
        bench_abort("CYCCNT stopped");
        return;
    }
    if (!wait_req(false))
    {
        bench_abort("host timeout");
        return;
    }
    static const char *const k_name[W_N] = { "PB8", "PB9", "PF14", "PF15" };
    /* Every pin: falls, time low, first fall after the window opened, low at the end. */
    int n = snprintf(s_line, sizeof(s_line), "T0C hold %s:", line);
    for (uint32_t k = 0U; (k < W_N) && (n > 0) && ((size_t)n < sizeof(s_line)); k++)
    {
        n += snprintf(&s_line[n], sizeof(s_line) - (size_t)n, " %s lows=%lu low_us=%lu first_us=%lu end_low=%u;",
                      k_name[k], (unsigned long)w[k].lows, (unsigned long)cyc_to_us(w[k].low_cyc),
                      (unsigned long)cyc_to_us(w[k].first_cyc), (unsigned)w[k].low_at_end);
    }
    log_text("");
    const uint32_t us = cyc_to_us(w[held].low_cyc);
    verdict((w[held].lows == 1U) && !w[held].low_at_end && (us >= T0C_HOLD_MIN_US) && (us <= T0C_HOLD_MAX_US) &&
            (w[other].lows == 0U),
            "T0C hold %s seen by CM7: %s low once for %lu us (%lu..%lu, CYCCNT at 400 MHz nominal), %s never low "
            "(%lu falls)", line, k_name[held], (unsigned long)us, (unsigned long)T0C_HOLD_MIN_US,
            (unsigned long)T0C_HOLD_MAX_US, k_name[other], (unsigned long)w[other].lows);
}

/* T0c (S17, S19), at the start of every session, CPU1 alone, nothing driven by
   the H755. (1) Levels at rest: PB8, PB9, PF14, PF15 in input without pull;
   the host reads the scope and the F334 in the same handshake. (2) The F334
   holds SDA, then SCL, for 50 ms: this core, the F334 and (with taps) the
   scope must all see it. (3) Levels at rest again. */
static void step_t0c(void)
{
    if (!step_begin("T0C"))
    {
        return;
    }
    const uint32_t v0 = bus_idr();
    LOG("T0C rest pb8=%lu pb9=%lu pf14=%lu pf15=%lu", (unsigned long)(v0 & 1U), (unsigned long)((v0 >> 1) & 1U),
        (unsigned long)((v0 >> 2) & 1U), (unsigned long)((v0 >> 3) & 1U));
    verdict((v0 & 3U) == 3U, "T0C bus at rest seen by CM7: PB8 %lu PB9 %lu (1, 1)", (unsigned long)(v0 & 1U),
            (unsigned long)((v0 >> 1) & 1U));
    if (!sync0())                       /* host: scope levels, F334 LEVELS, S17/S19 */
    {
        return;
    }
    t0c_hold("SDA", W_PB9, W_PB8);
    if (!sync0())                       /* host: F334 HOLD_END and the scope capture judged */
    {
        return;
    }
    t0c_hold("SCL", W_PB8, W_PB9);
    if (!sync0())
    {
        return;
    }
    const uint32_t v1 = bus_idr();
    LOG("T0C rest after pb8=%lu pb9=%lu pf14=%lu pf15=%lu", (unsigned long)(v1 & 1U), (unsigned long)((v1 >> 1) & 1U),
        (unsigned long)((v1 >> 2) & 1U), (unsigned long)((v1 >> 3) & 1U));
    verdict((v1 & 3U) == 3U, "T0C bus at rest after the holds, CM7: PB8 %lu PB9 %lu (1, 1)", (unsigned long)(v1 & 1U),
            (unsigned long)((v1 >> 1) & 1U));
    if (!sync0())                       /* host: levels again (S17 after the step) */
    {
        return;
    }
    step_end();
}

/* ---- T0b ----------------------------------------------------------------------------------- */

/* A photo of the two bus pins and of I2C4 (see IstPinPhoto_t). */
static void photo(IstPinPhoto_t *p)
{
    const uint32_t s8 = IST_PIN_SCL;
    const uint32_t s9 = IST_PIN_SDA;
    p->moder  = ((GPIOB->MODER >> (s8 * 2U)) & 3UL) | (((GPIOB->MODER >> (s9 * 2U)) & 3UL) << 2);
    p->otyper = ((GPIOB->OTYPER >> s8) & 1UL) | (((GPIOB->OTYPER >> s9) & 1UL) << 1);
    p->pupdr  = ((GPIOB->PUPDR >> (s8 * 2U)) & 3UL) | (((GPIOB->PUPDR >> (s9 * 2U)) & 3UL) << 2);
    p->afr    = ((GPIOB->AFR[1] >> ((s8 - 8U) * 4U)) & 0xFUL) | (((GPIOB->AFR[1] >> ((s9 - 8U) * 4U)) & 0xFUL) << 4);
    p->idr    = ((GPIOB->IDR >> s8) & 1UL) | (((GPIOB->IDR >> s9) & 1UL) << 1);
    p->i2c_cr1 = i2c4_clocked() ? I2C4->CR1 : 0xFFFFFFFFUL;
    p->i2c_isr = i2c4_clocked() ? I2C4->ISR : 0xFFFFFFFFUL;
}

static void log_photo(const char *what, const IstPinPhoto_t *p, const char *inst)
{
    log_long("T0B photo %s: MODER PB8|PB9<<2=0x%lX OTYPER=0x%lX PUPDR=0x%lX AFR PB8|PB9<<4=0x%02lX IDR=0x%lX "
             "%s CR1=0x%08lX ISR=0x%08lX", what, (unsigned long)p->moder, (unsigned long)p->otyper,
             (unsigned long)p->pupdr, (unsigned long)p->afr, (unsigned long)p->idr, inst,
             (unsigned long)p->i2c_cr1, (unsigned long)p->i2c_isr);
}

/* T0b in step T0B (bus owner CPU2): CPU1 refused everywhere it should be;
   CPU2's claims, its I2C1 states and the S16 pin cases; then CPU2 writes the
   accepted case (PB8/PB9 AF4 open-drain, I2C1 at PE = 0) and the host reads
   the bus levels while the step holds that state; then CPU2's bench_safe(). */
static void step_t0b(void)
{
    IstArea_t *a = IST_AREA;
    if (!step_begin("T0B"))
    {
        return;
    }
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    const uint32_t bad = run_claims(k_t0b_refuse, NCASES(k_t0b_refuse));
    const uint32_t good = run_claims(k_t0b_accept, NCASES(k_t0b_accept));
    port_snap(&after);
    const bool untouched = port_snap_eq(&before, &after);
    verdict((bad == 0U) && (good == ((1UL << NCASES(k_t0b_accept)) - 1U)) && untouched,
            "T0B CM7 claim in T0B (bus = CM4): %lu refusals (S15 AF/pull/push-pull, S16 owner, S4/S13/S14, R4), "
            "accepted by mistake 0x%08lX; %lu controls (plain inputs, a pulled AD2 line) accepted 0x%lX; GPIO regs "
            "untouched %u", (unsigned long)NCASES(k_t0b_refuse), (unsigned long)bad,
            (unsigned long)NCASES(k_t0b_accept), (unsigned long)good, (unsigned)untouched);

    cm4_claims(IST_M4_SLOT_T0B, IST_M4_CLAIMS_EXPECT_T0B, s_step, "in T0B (bus = CM4)");
    if (cm4_send(IST_CMD_I2C1, 0U))
    {
        const IstI2cCaseResult_t *r = &a->m4_i2c1;
        verdict((r->cases == IST_I2C_CASES) && (r->accepted == IST_I2C_CASES_EXPECT) && (r->pe_seen == 1U) &&
                (r->regs_changed == 0U) && (r->cr1_after == 0U) && (r->clk_same == 1U),
                "T0B S16 CM4, PB8 AF4 against I2C1: PE=1 (read back %lu) and clock off refused, PE=0 with ANFOFF "
                "and reset accepted: accepted 0x%lX (0x%lX), GPIO regs changed 0x%06lX, I2C1 CR1 after 0x%08lX, "
                "clock as before %lu", (unsigned long)r->pe_seen, (unsigned long)r->accepted,
                (unsigned long)IST_I2C_CASES_EXPECT, (unsigned long)r->regs_changed, (unsigned long)r->cr1_after,
                (unsigned long)r->clk_same);
    }
    if (cm4_send(IST_CMD_S16, 0U))
    {
        verdict((a->m4_s16.cases == IST_S16_CASES) && (a->m4_s16.accepted == IST_S16_CASES_EXPECT),
                "T0B S16 pin state, CM4 (AF4): PB9 in AF6, PB9 output, PB8 in AF6, PB8 output refused; both input, "
                "both analog, both AF4 accepted: accepted 0x%02lX (0x%02lX)", (unsigned long)a->m4_s16.accepted,
                (unsigned long)IST_S16_CASES_EXPECT);
    }

    /* The accepted case, written: PB8/PB9 AF4 open-drain on CPU2, I2C1 at PE = 0. */
    if (!cm4_send(IST_CMD_AF_ON, 0U))
    {
        bench_abort("CM4 AF_ON");
        return;
    }
    const IstPinPhoto_t *p = &a->m4_af;
    log_photo("CM4 AF4", p, "I2C1");
    const bool af_ok = (p->claim_ok == 1U) && (p->moder == 0xAU) && (p->otyper == 0x3U) && (p->pupdr == 0U) &&
                       (p->afr == 0x44U) && (p->i2c_cr1 == 0U) && ((p->i2c_isr & I2C_ISR_BUSY) == 0U);
    const uint32_t lv = bus_idr();
    verdict(af_ok && (p->idr == 3U) && ((lv & 3U) == 3U),
            "T0B CM4 writes PB8/PB9 AF4 open-drain no pull with I2C1 at PE=0: claim %lu, MODER 0x%lX (0xA) OTYPER "
            "0x%lX (0x3) PUPDR 0x%lX (0) AFR 0x%02lX (0x44), I2C1 CR1 0x%08lX BUSY %lu; bus IDR CM4 0x%lX CM7 0x%lX "
            "(0x3: released)", (unsigned long)p->claim_ok, (unsigned long)p->moder, (unsigned long)p->otyper,
            (unsigned long)p->pupdr, (unsigned long)p->afr, (unsigned long)p->i2c_cr1,
            (unsigned long)((p->i2c_isr & I2C_ISR_BUSY) != 0U), (unsigned long)p->idr, (unsigned long)(lv & 3U));
    /* While CPU2 has the pins in AF4, CPU1's own AF6 claim must still be refused. */
    const bool cm7_refused = !bench_claim(GPIOB, IST_BUS_MASK, GPIO_BM_MODE_AF, IST_AF_I2C4, GPIO_BM_OTYPE_OD,
                                          GPIO_BM_PULL_NONE);
    verdict(cm7_refused, "T0B CM7 claim of PB8/PB9 AF6 while CM4 holds them in AF4: refused %u", (unsigned)cm7_refused);
    LOG("T0B levels with CM4 AF4 pb8=%lu pb9=%lu", (unsigned long)(lv & 1U), (unsigned long)((lv >> 1) & 1U));
    const bool host_ok = sync0();        /* host: bus levels (S17) with the pins in AF4 at PE = 0 */

    if (cm4_send(IST_CMD_SAFE, 0U))
    {
        const IstPinPhoto_t *q = &a->m4_safe;
        log_photo("CM4 after bench_safe", q, "I2C1");
        verdict((q->moder == 0U) && (q->otyper == 0x3U) && (q->pupdr == 0U) && (q->afr == 0U) && (q->i2c_cr1 == 0U) &&
                (q->clk_same == 1U),
                "T0B CM4 bench_safe: MODER 0x%lX (0: input) OTYPER 0x%lX (0x3) PUPDR 0x%lX AFR 0x%02lX (0), I2C1 CR1 "
                "0x%08lX (0), clock as before %lu", (unsigned long)q->moder, (unsigned long)q->otyper,
                (unsigned long)q->pupdr, (unsigned long)q->afr, (unsigned long)q->i2c_cr1, (unsigned long)q->clk_same);
    }
    else
    {
        bench_safe();
    }
    if (!host_ok || !sync0())            /* host: levels again after the release */
    {
        return;
    }
    step_end();
    cm4_claims(IST_M4_SLOT_NONE, IST_M4_CLAIMS_EXPECT_NONE, IST_STEP_NONE, "between steps");
}

/* T0b in step T0B_CM7 (bus owner CPU1): CPU1's claims with I2C4 clocked at its
   reset values, the I2C4 states, the S16 pin cases; then CPU1 writes PB8/PB9
   AF6 open-drain with I2C4 at PE = 0 and the host reads the levels; then
   bench_safe(). CPU2 is refused everything here. */
typedef struct { uint32_t cr1; uint32_t clk; const char *what; } I2cCase_t;
static const I2cCase_t k_i2c4_cases[IST_I2C_CASES] = {
    { I2C_CR1_PE,     1U, "PE=1" },
    { 0U,             0U, "clock off in CPU1's bank" },
    { I2C_CR1_ANFOFF, 1U, "PE=0 with ANFOFF" },
    { 0U,             1U, "reset" },
};

static void step_t0b_cm7(void)
{
    if (!step_begin("T0B_CM7"))
    {
        return;
    }
    /* S16: the other instance had the pins until the last step; >= 1 ms with
       them in input before this one takes them. */
    if (!delay_ms(2U))
    {
        bench_abort("CYCCNT stopped");
        return;
    }
    const bool clk_was_on = i2c4_clocked();
    i2c4_clock(true);
    i2c_release(I2C4);

    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    const uint32_t acc = run_claims(k_cm7_claims, NCASES(k_cm7_claims));
    uint32_t i2c_acc = 0U;
    uint32_t pe_seen = 0U;
    for (uint32_t k = 0U; k < IST_I2C_CASES; k++)
    {
        i2c4_clock(true);
        i2c_release(I2C4);
        I2C4->CR1 = k_i2c4_cases[k].cr1;
        if ((k_i2c4_cases[k].cr1 & I2C_CR1_PE) != 0U)
        {
            pe_seen = ((I2C4->CR1 & I2C_CR1_PE) != 0U) ? 1U : 0U;   /* the case is a case only if PE is 1 */
        }
        if (k_i2c4_cases[k].clk == 0U)
        {
            i2c4_clock(false);
        }
        if (bench_claim(GPIOB, 1UL << IST_PIN_SCL, GPIO_BM_MODE_AF, IST_AF_I2C4, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE))
        {
            i2c_acc |= 1UL << k;
        }
        i2c4_clock(true);
        i2c_release(I2C4);
    }
    port_snap(&after);
    const bool untouched = port_snap_eq(&before, &after);
    verdict((acc == CM7_CLAIMS_EXPECT) && untouched,
            "T0B CM7 claim in T0B_CM7 (bus = CM7, I2C4 at reset): %lu cases, accepted 0x%04lX (0x%04lX: AF6 OD no "
            "pull on PB8, PB9, both; a plain input), GPIO regs untouched %u", (unsigned long)NCASES(k_cm7_claims),
            (unsigned long)acc, (unsigned long)CM7_CLAIMS_EXPECT, (unsigned)untouched);
    verdict((i2c_acc == IST_I2C_CASES_EXPECT) && (pe_seen == 1U) && (I2C4->CR1 == 0U),
            "T0B S16 CM7, PB8 AF6 against I2C4: PE=1 (read back %lu) and clock off refused, PE=0 with ANFOFF and "
            "reset accepted: accepted 0x%lX (0x%lX), I2C4 CR1 after 0x%08lX", (unsigned long)pe_seen,
            (unsigned long)i2c_acc, (unsigned long)IST_I2C_CASES_EXPECT, (unsigned long)I2C4->CR1);
    const uint32_t s16 = ist_s16_cases(IST_AF_I2C4, IST_AF_I2C1);
    verdict(s16 == IST_S16_CASES_EXPECT,
            "T0B S16 pin state, CM7 (AF6): PB9 in AF4, PB9 output, PB8 in AF4, PB8 output refused; both input, both "
            "analog, both AF6 accepted: accepted 0x%02lX (0x%02lX)", (unsigned long)s16,
            (unsigned long)IST_S16_CASES_EXPECT);

    /* The accepted case, written: PB8/PB9 AF6 open-drain on CPU1, I2C4 at PE = 0. */
    IstPinPhoto_t p;
    const bool claimed = pin_cfg(GPIOB, IST_PIN_SCL, GPIO_BM_MODE_AF, IST_AF_I2C4, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE) &&
                         pin_cfg(GPIOB, IST_PIN_SDA, GPIO_BM_MODE_AF, IST_AF_I2C4, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE);
    photo(&p);
    log_photo("CM7 AF6", &p, "I2C4");
    verdict(claimed && (p.moder == 0xAU) && (p.otyper == 0x3U) && (p.pupdr == 0U) && (p.afr == 0x66U) &&
            (p.i2c_cr1 == 0U) && ((p.i2c_isr & I2C_ISR_BUSY) == 0U) && (p.idr == 3U),
            "T0B CM7 writes PB8/PB9 AF6 open-drain no pull with I2C4 at PE=0: claim %u, MODER 0x%lX (0xA) OTYPER 0x%lX "
            "(0x3) PUPDR 0x%lX (0) AFR 0x%02lX (0x66), I2C4 CR1 0x%08lX BUSY %lu, bus IDR 0x%lX (0x3: released)",
            (unsigned)claimed, (unsigned long)p.moder, (unsigned long)p.otyper, (unsigned long)p.pupdr,
            (unsigned long)p.afr, (unsigned long)p.i2c_cr1, (unsigned long)((p.i2c_isr & I2C_ISR_BUSY) != 0U),
            (unsigned long)p.idr);
    LOG("T0B levels with CM7 AF6 pb8=%lu pb9=%lu", (unsigned long)(p.idr & 1U), (unsigned long)((p.idr >> 1) & 1U));
    const bool host_ok = !s_bench_abort && sync0();      /* host: bus levels with the pins in AF6 */

    bench_safe();
    photo(&p);
    log_photo("CM7 after bench_safe", &p, "I2C4");
    if (!clk_was_on)
    {
        i2c4_clock(false);
    }
    verdict((p.moder == 0U) && (p.otyper == 0x3U) && (p.pupdr == 0U) && (p.afr == 0U) && (p.i2c_cr1 == 0U) &&
            (i2c4_clocked() == clk_was_on),
            "T0B CM7 bench_safe: MODER 0x%lX (0: input) OTYPER 0x%lX (0x3) PUPDR 0x%lX AFR 0x%02lX (0), I2C4 CR1 "
            "0x%08lX (0); I2C4 clock as before %u", (unsigned long)p.moder, (unsigned long)p.otyper,
            (unsigned long)p.pupdr, (unsigned long)p.afr, (unsigned long)p.i2c_cr1, (unsigned)clk_was_on);

    cm4_claims(IST_M4_SLOT_CM7, IST_M4_CLAIMS_EXPECT_CM7, s_step, "in T0B_CM7 (bus = CM7)");
    if (!host_ok || !sync0())            /* host: levels again after the release */
    {
        return;
    }
    step_end();
}

/* T0b(d): the host does not answer; the MCU must time out into S2, and wait
   with MCU_ACK low until the host has seen it (sync_ack_low()). */
static void step_t0b_timeout(void)
{
    if (!step_begin("T0B_TIMEOUT"))
    {
        return;
    }
    ack(true);
    const bool answered = wait_req(true);
    bench_safe();
    const uint32_t e_moder = GPIOE->MODER & 0xFFFFFFF0UL;         /* PE2..PE15 */
    const uint32_t expect  = 1UL << (PE_ACK * 2U);                /* only MCU_ACK is an output */
    const uint32_t bus_m   = (GPIOB->MODER >> (IST_PIN_SCL * 2U)) & 0xFUL;
    const uint32_t tap_m   = (GPIOF->MODER >> (IST_PIN_TAP_SCL * 2U)) & 0xFUL;
    const bool safe = (e_moder == expect) && (bus_m == 0U) && (tap_m == 0U) &&
                      (((GPIOA->MODER >> (PA_MCO * 2U)) & 3UL) == 0U) &&
                      (((GPIOE->ODR >> PE_ACK) & 1UL) == 0U) && (GPIOE->AFR[1] == 0U);
    verdict(!answered && safe, "T0B(d) no HOST_REQ in 10 s -> bench_safe: GPIOE MODER[31:4]=0x%08lX (0x%08lX), AFRH=0x%08lX, "
            "PB8|PB9 MODER 0x%lX (0), PF14|PF15 MODER 0x%lX (0)", (unsigned long)e_moder, (unsigned long)expect,
            (unsigned long)GPIOE->AFR[1], (unsigned long)bus_m, (unsigned long)tap_m);
    LOG("T0B timeout handled");
    if (!sync_ack_low())   /* host: MCU_ACK low, then a HOST_REQ pulse */
    {
        return;
    }
    step_end();
}

/* ==========================================================================================
 * Entry points
 * ========================================================================================== */

void I2C_Selftest_CM7_PreRelease(void)
{
    IstArea_t *a = IST_AREA;
    memset((void *)a, 0, sizeof(*a));
    a->session = DWT->CYCCNT ^ 0x12C00000UL;
    a->version = I2C_BENCH_VERSION;
    a->step    = IST_STEP_NONE;
    __DMB();
    a->magic = IST_AREA_MAGIC;
    __DSB();
}

static void copy_area_str(char *dst, const volatile char *src, uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++)
    {
        dst[i] = src[i];
    }
    dst[n - 1U] = '\0';
}

static bool cm4_ready(void)
{
    return IST_AREA->m4_ready == IST_AREA->session;
}

void I2C_Selftest_CM7_Run(void)
{
    IstArea_t *a = IST_AREA;
    char rev4[sizeof(a->m4_rev)];
    char id4[sizeof(a->m4_build_id)];

    vTaskSuspendAll();   /* no task switch for the whole test; the HAL tick (TIM6) keeps running */
    publish_step(IST_STEP_NONE);
    bench_safe();

    LOG("CM7 Rev %s", FW_GIT_REV);
    LOG("CM7 Build id %s", FW_BUILD_ID);
    const bool m4_ok = (wait_until(cm4_ready, CM4_READY_MS) == WAIT_DONE);
    copy_area_str(rev4, a->m4_rev, sizeof(rev4));
    copy_area_str(id4, a->m4_build_id, sizeof(id4));
    LOG("CM4 Rev %s", m4_ok ? rev4 : "(CPU2 not ready)");
    LOG("CM4 Build id %s", m4_ok ? id4 : "(CPU2 not ready)");
    if (!m4_ok || (a->m4_version != I2C_BENCH_VERSION))
    {
        s_fail++;
    }
    LOG("I2C_BENCH v%u idle", (unsigned)I2C_BENCH_VERSION);
    /* DBGMCU is only read (plan: never written). */
    LOG("silicon REV_ID=0x%04lX, test block at 0x%08lX, CM4 table v%lu, steps %lu",
        (unsigned long)((DBGMCU->IDCODE & DBGMCU_IDCODE_REV_ID_Msk) >> DBGMCU_IDCODE_REV_ID_Pos),
        (unsigned long)IST_AREA_ADDR, (unsigned long)a->m4_version, (unsigned long)k_ist_step_count);

    step_t0a();
    step_t0c();
    step_t0b();
    step_t0b_cm7();
    step_t0b_timeout();
    if (step_begin("DONE"))
    {
        step_end();
    }
    bench_safe();
    verdict(!s_bench_abort, "bench completed without abort");

    (void)cm4_send(IST_CMD_DONE, 0U);
    verdict((a->m4_timeout == 0U) && (a->m4_aborts == 0U) && (a->m4_stalls == 0U) && (s_stalls == 0U),
            "CM4 finished: timeout %lu, aborts %lu; CYCCNT stall exits CM4 %lu, CM7 %lu",
            (unsigned long)a->m4_timeout, (unsigned long)a->m4_aborts, (unsigned long)a->m4_stalls,
            (unsigned long)s_stalls);
    LOG("Status_Task stack high-water mark %lu words", (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    LOG("FW VERDICT=%s failures=%lu", (s_fail == 0U) ? "PASS" : "FAIL", (unsigned long)s_fail);

    (void)xTaskResumeAll();
}
