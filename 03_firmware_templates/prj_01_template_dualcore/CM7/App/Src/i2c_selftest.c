/**
  ******************************************************************************
  * @file    i2c_selftest.c
  * @brief   05_i2c selftest, CPU1 side (preset selftest-i2c), phases F0 and
  *          F1: the bench (AD2, the I2C bus on PB8/PB9, the NUCLEO-F334R8
  *          target) and i2c_bm on both cores.
  *
  *          Prints Rev and build id of both cores, then runs the bench steps
  *          T0a, T0c and T0b with the host (tools/i2c_hw_check.py, which also
  *          talks to the F334, reads the scope, checks the identity of every
  *          image and probe, T0d, and runs its guards on mocks, T0b(a)); then
  *          the F1 steps: F1_NOPIN (T1-T3, T13-T15 without the bus), F1_CM4
  *          (CPU2's I2C1: T1 with BUSY, T4, T5, T6, T7(a), T8, T10, T12, B1),
  *          F1_CM7 (CPU1's I2C4: the same with T7(b) by BDMA) and F1_RECOVER
  *          (T9, and T9(b): the bus held before Init, plan R3-6); then the F2
  *          steps with i2c_ll: F2_NOPIN (T1, T2 parity with i2c_bm), F2_CM4 and
  *          F2_CM7 (T1 with BUSY, T4, T5, T6, T7, T14); then the F3 steps with
  *          i2c_hal, CPU1 only: F3_NOPIN (T1, T2 against i2c_bm and the raw
  *          HAL_I2C_Init, I2C_HAL_Wait with the tick stopped) and F3_CM7 (T1 with
  *          BUSY, T4, T5, T6, T7(b), T14); and prints the global verdict, CPU2's
  *          results included. Plan
  *          and meaning of every test: 01_shared_drivers/05_i2c/IMPLEMENTATION_PLAN.md.
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
  *                step_begin() sets. One raw write is the injected fault of
  *                t0b_cm7_live_s16() (PB9 in AF4 with I2C1 at PE = 0, audit F1
  *                m18); the Fm+ bits are cleared raw by bench_safe() (m20).
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
  *          wait of the test depends on HAL_GetTick(). The HAL tier's own waits
  *          run on it (TIM6 keeps running with the scheduler suspended); the TICK
  *          lines show it advance (F3).
  ******************************************************************************
  */

#include "i2c_selftest.h"
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "i2c_bm.h"
#include "i2c_hal.h"
#include "dma_hal.h"
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
#define CM4_TIMEOUT_MS      30000U        /* a CPU2 command: 65535 bytes at 100 kHz take about 6 s */
#define CM4_READY_MS        5000U
#define STALL_READS         1000U         /* CYCCNT read equal this often in a row: stopped */

/* Every hold of the F334 (T0c and the AT HOLD points) starts after the handshake
   that lets this core go on (audit F2 part 1, B2): the host orders it only then, so
   this core is already watching when the line falls. It waits for the fall up to
   HOLD_WAIT_MS and times the hold from that fall, whatever the host's latency. */
#define HOLD_WAIT_MS        2000U
/* T0c: the F334 holds a line for T0C_HOLD_MS. The watch ends T0C_TAIL_MS after the
   held line rises (the other line must stay high and the held one must not fall
   again), or T0C_LOW_MAX_MS after the fall if it never rises (S21: 500 ms at most). */
#define T0C_HOLD_MS         50U
#define T0C_TAIL_MS         50U
#define T0C_LOW_MAX_MS      500U
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
static bool      s_ll;                    /* phase F2: Init and the transfers through i2c_ll    */
static bool      s_hal;                   /* phase F3: CPU1's Init and transfers through i2c_hal */

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
    /* In the F2 steps every verdict says it is i2c_ll's; in F3, i2c_hal's. */
    const char *tier = s_ll ? "LL " : (s_hal ? "HAL " : "");
    const size_t pre = strlen(tier);
    (void)memcpy(s_line, tier, pre);
    (void)vsnprintf(&s_line[pre], sizeof(s_line) - pre, fmt, ap);
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

/* The Fm+ bits of the bench's instances (SYSCFG_PMCR I2C1FMP bit 0, I2C4FMP bit 3,
   13:122-137) back to 0, each by its own one-bit read-modify-write and only if
   set; readable only with the SYSCFG clock on in a bank (audit F1, m20). */
#define FMP_BENCH_BITS      (SYSCFG_PMCR_I2C1_FMP | SYSCFG_PMCR_I2C4_FMP)

static bool syscfg_clocked(void)
{
    return ((RCC_C1->APB4ENR | RCC_C2->APB4ENR) & RCC_APB4ENR_SYSCFGEN) != 0U;
}

static void fmp_off_raw(void)
{
    if (!syscfg_clocked())
    {
        return;
    }
    if ((SYSCFG->PMCR & SYSCFG_PMCR_I2C1_FMP) != 0U)
    {
        SYSCFG->PMCR &= ~SYSCFG_PMCR_I2C1_FMP;
    }
    if ((SYSCFG->PMCR & SYSCFG_PMCR_I2C4_FMP) != 0U)
    {
        SYSCFG->PMCR &= ~SYSCFG_PMCR_I2C4_FMP;
    }
    (void)SYSCFG->PMCR;
}

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
    fmp_off_raw();

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
    /* The step is withdrawn too (audit F0, m9): CPU2 judges its claims against
       the published step, so after an abort it may claim nothing until DONE. */
    const uint32_t was = s_step;
    publish_step(IST_STEP_NONE);
    s_step = was;                       /* kept for the ABORT line below only */
    LOG("ABORT step=%s reason=%s", (s_step < k_ist_step_count) ? k_ist_steps[s_step].name : "-", reason);
    s_step = IST_STEP_NONE;
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
        st = GPIO_BM_ERR_PARAM;    /* no GPIO output here: MCU_ACK is raw, PB8/PB9 for T9 go through cm7_pins_od() */
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
    { GPIOB, B8, O, 0U, OD, NP },      { GPIOB, B9, O, 0U, PP, NP },      /* GPIO OD: CPU2's step; push-pull */
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
/* Each table's result is a uint32_t, bit k = case k (audit F0, m10). */
_Static_assert(NCASES(k_t0b_refuse) <= 32U, "k_t0b_refuse: more cases than result bits");
_Static_assert(NCASES(k_t0b_accept) <= 32U, "k_t0b_accept: more cases than result bits");
_Static_assert(NCASES(k_cm7_claims) <= 32U, "k_cm7_claims: more cases than result bits");
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

/* Watches PB8, PB9, PF14 and PF15, recording each fall and rise, around one hold
   of line `held` (B2): open before the hold starts, it waits up to HOLD_WAIT_MS for
   the held line to fall, and closes T0C_TAIL_MS after it rises again, or
   T0C_LOW_MAX_MS after the fall if it does not. False if CYCCNT stopped. Inputs
   only: nothing moves. */
static bool watch_bus(Watch_t w[W_N], uint32_t held)
{
    uint32_t level = bus_idr();
    uint32_t fall_at[W_N] = { 0U, 0U, 0U, 0U };
    const uint32_t t0 = DWT->CYCCNT;
    uint32_t last = t0;
    uint32_t same = 0U;
    uint32_t held_fall = 0U;
    uint32_t held_rise = 0U;
    bool held_up = false;             /* the held line rose after its first fall */
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
                    if (k == held)
                    {
                        held_fall = now;
                    }
                }
                w[k].lows++;
                fall_at[k] = now;
                if (k == held)
                {
                    held_up = false;
                }
            }
            else
            {
                w[k].low_cyc += now - fall_at[k];
                if ((k == held) && (w[k].lows != 0U))
                {
                    held_up = true;
                    held_rise = now;
                }
            }
        }
        level = v;
        const bool no_fall = (w[held].lows == 0U) && ((now - t0) >= (HOLD_WAIT_MS * CYC_PER_MS));
        const bool tail_done = held_up && ((now - held_rise) >= (T0C_TAIL_MS * CYC_PER_MS));
        const bool too_long = (w[held].lows != 0U) && !held_up &&
                              ((now - held_fall) >= (T0C_LOW_MAX_MS * CYC_PER_MS));
        if (no_fall || tail_done || too_long)
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
   scope; MCU_ACK drops and this core starts watching the bus; only then does the
   host have the F334 hold `line` low for T0C_HOLD_MS (B2). The watch closes once
   the hold is over (watch_bus()) and the result is printed; the host reads it
   after the F334's HOLD_END. */
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
    const bool ok = watch_bus(w, held);
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
    /* While CPU2 has the pins in AF4: CPU1's AF6 claim is refused by the step's
       owner rule (ist_line_rule(), the bus is CPU2's in T0B), which a claim
       meets before it reads the pins (audit F0, m8). The pin half of S16 is
       then tried on the LIVE registers, PB8/PB9 really in AF4: ist_s16_pins_ok()
       must refuse AF6 on its own, and accept AF4 (the control). */
    const bool cm7_refused = !bench_claim(GPIOB, IST_BUS_MASK, GPIO_BM_MODE_AF, IST_AF_I2C4, GPIO_BM_OTYPE_OD,
                                          GPIO_BM_PULL_NONE);
    const bool rule_refused = !ist_line_rule(0U, s_step, GPIOB, IST_PIN_SCL, GPIO_BM_MODE_AF, IST_AF_I2C4,
                                             GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE);
    verdict(cm7_refused && rule_refused, "T0B CM7 claim of PB8/PB9 AF6 in step T0B (bus = CM4): refused %u, by the "
            "step-owner rule (ist_line_rule refuses %u)", (unsigned)cm7_refused, (unsigned)rule_refused);
    const uint32_t live_moder = GPIOB->MODER;
    const uint32_t live_afrh  = GPIOB->AFR[1];
    const bool pins_af6 = ist_s16_pins_ok(live_moder, live_afrh, IST_AF_I2C4);
    const bool pins_af4 = ist_s16_pins_ok(live_moder, live_afrh, IST_AF_I2C1);
    verdict(!pins_af6 && pins_af4, "T0B S16 pin check on the live GPIOB registers (PB8/PB9 in AF4 by CM4, MODER "
            "0x%08lX AFRH 0x%08lX): AF6 refused %u, AF4 accepted %u (the control)", (unsigned long)live_moder,
            (unsigned long)live_afrh, (unsigned)!pins_af6, (unsigned)pins_af4);
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

/* The live control of S16 in a step of CPU1, through bench_claim() (audit F1, m18;
   F0, m8). PB9 is put in AF4 (I2C1) here by a raw write: the injected fault, the
   only raw write of a bus pin outside bench_safe(). It is done only while I2C1,
   clocked, reads back PE = 0 (then I2C1 drives nothing: PB9 stays an open-drain
   pin let go) and PB9 is open-drain without pull, with interrupts masked, for a
   few microseconds. bench_claim() of PB8 in AF6 must be refused by the pin check
   alone (the step-owner rule accepts it in this step); with PB9 back in input the
   same claim is accepted (the control). Nothing is written for PB8. */
static void t0b_cm7_live_s16(void)
{
    const uint32_t sda_m = 3UL << (IST_PIN_SDA * 2U);
    const uint32_t sda_a = 0xFUL << ((IST_PIN_SDA - 8U) * 4U);
    const bool clk_was = (RCC->APB1LENR & RCC_APB1LENR_I2C1EN) != 0U;       /* CPU1's bank */
    RCC->APB1LENR |= RCC_APB1LENR_I2C1EN;
    (void)RCC->APB1LENR;
    const bool safe = ((I2C1->CR1 & I2C_CR1_PE) == 0U) && ((GPIOB->MODER & sda_m) == 0U) &&
                      ((GPIOB->OTYPER & (1UL << IST_PIN_SDA)) != 0U) && ((GPIOB->PUPDR & sda_m) == 0U);
    bool rule_ok = false;
    bool refused = false;
    bool control = false;
    uint32_t moder = 0U;
    uint32_t afrh = 0U;
    if (safe)
    {
        GpioBm_Section_t gs;                         /* the cross-core GPIO lock, as bench_safe() (O20) */
        const bool locked = (GPIO_BM_SectionEnter(&gs) == GPIO_BM_OK);
        const uint32_t pm = __get_PRIMASK();
        __disable_irq();
        GPIOB->AFR[1] = (GPIOB->AFR[1] & ~sda_a) | (IST_AF_I2C1 << ((IST_PIN_SDA - 8U) * 4U));
        GPIOB->MODER  = (GPIOB->MODER & ~sda_m) | (2UL << (IST_PIN_SDA * 2U));
        moder = GPIOB->MODER;
        afrh  = GPIOB->AFR[1];
        rule_ok = ist_line_rule(0U, s_step, GPIOB, IST_PIN_SCL, GPIO_BM_MODE_AF, IST_AF_I2C4, GPIO_BM_OTYPE_OD,
                                GPIO_BM_PULL_NONE);
        refused = !bench_claim(GPIOB, 1UL << IST_PIN_SCL, GPIO_BM_MODE_AF, IST_AF_I2C4, GPIO_BM_OTYPE_OD,
                               GPIO_BM_PULL_NONE);
        GPIOB->MODER  &= ~sda_m;                     /* input first, then the AF field */
        GPIOB->AFR[1] &= ~sda_a;
        __DSB();
        __set_PRIMASK(pm);
        if (locked)
        {
            GPIO_BM_SectionExit(&gs);
        }
        control = bench_claim(GPIOB, 1UL << IST_PIN_SCL, GPIO_BM_MODE_AF, IST_AF_I2C4, GPIO_BM_OTYPE_OD,
                              GPIO_BM_PULL_NONE);
    }
    if (!clk_was)
    {
        RCC->APB1LENR &= ~RCC_APB1LENR_I2C1EN;
        (void)RCC->APB1LENR;
    }
    const bool back = ((GPIOB->MODER & sda_m) == 0U) && ((GPIOB->AFR[1] & sda_a) == 0U);
    verdict(safe && rule_ok && refused && control && back,
            "T0B S16 live in T0B_CM7 through bench_claim: PB9 put in AF4 (I2C1 PE=0, MODER 0x%08lX AFRH 0x%08lX): "
            "PB8 AF6 refused %u by the pin check (owner rule accepts %u); PB9 back in input %u, the same claim "
            "accepted %u (the control)", (unsigned long)moder, (unsigned long)afrh, (unsigned)refused,
            (unsigned)rule_ok, (unsigned)back, (unsigned)control);
}

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
    t0b_cm7_live_s16();

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
 * Phase F1: i2c_bm (plan T1-T15, B1)
 *
 * The bench state comes from PF14 (S19): with J2 in place it follows the SCL node
 * (1 at rest), without it its 22 kOhm pulls it to 0. With taps the transfers run at
 * 100 and 400 kHz and the scope watches them (T5); without taps at 1 MHz only (D3).
 * The host is told at every point where it acts or measures: an "AT" line, then a
 * handshake (host_at()). Each transfer prints one "X" line, which the host pairs, in
 * order, with the target's RPT line when the line says t=1.
 * ========================================================================================== */

#define T_ADDR          IST_TARGET_ADDR
#define T_NOADDR        IST_NO_TARGET_ADDR
#define F1_BIG          65536U
#define BD_N            4096U
#define SENTINEL        0x5AU
#define PACE_MS         20U          /* the target prints one RPT line in about 13 ms at 115200 */
#define IRQ_PRIO        6U
#define TAPS_SPEEDS     2U

static uint32_t s_cpu_hz;            /* CPU1, SystemCoreClock                                   */
static uint32_t s_k123;              /* rcc_pclk1: I2C1-3 (I2C123SEL = 00, 09:4744-4749)         */
static uint32_t s_k4;                /* rcc_pclk4: I2C4 (I2C4SEL = 00, 09:4868-4873)             */
static uint32_t s_seed;              /* xorshift32 state of the run's seeds                     */
static bool     s_taps;              /* bench state, from PF14                                   */
static uint32_t s_f_main;            /* the functional tests' speed: 400 kHz taps, 1 MHz notaps  */
static uint32_t s_rise_ns, s_fall_ns;/* the bus t_r/t_f given to CalcTiming                      */

static uint8_t s_big[F1_BIG] __attribute__((section(".axi_ram"), aligned(32)));
static uint8_t s_ptrb __attribute__((section(".axi_ram"), aligned(32)));
static uint8_t s_bd_tx[BD_N] __attribute__((section(".d3_ram"), aligned(32)));   /* BDMA: SRAM4 */
static uint8_t s_bd_rx[BD_N] __attribute__((section(".d3_ram"), aligned(32)));

static const uint32_t k_t4_sizes[] = { 1U, 2U, 255U, 256U, 1000U, 65535U };
#define T4_SIZES   ((uint32_t)(sizeof(k_t4_sizes) / sizeof(k_t4_sizes[0])))

static uint32_t next_seed(void)
{
    return ist_xorshift32(&s_seed);
}

static uint32_t ms_cyc(uint32_t ms)
{
    return ms * (s_cpu_hz / 1000U);
}

/* The CYCCNT budget of a transfer of `bytes` bytes at f Hz: 1.6 times its nine bits per
   byte plus 20 ms (the target's own clock stretching, the reloads), within 32 bits. */
static uint32_t budget(uint32_t cpu_hz, uint32_t f, uint32_t bytes)
{
    const uint64_t bus = ((uint64_t)(bytes + 2U) * 9U * (uint64_t)cpu_hz * 16U) / ((uint64_t)f * 10U);
    const uint64_t c = bus + ((uint64_t)cpu_hz / 50U);
    return (c > 0xF0000000ULL) ? 0xF0000000UL : (uint32_t)c;
}

static const char *op_name(uint32_t op)
{
    static const char *const k[4] = { "W", "R", "WR", "P" };
    return (op < 4U) ? k[op] : "?";
}

static const char *mode_name(uint32_t mode)
{
    static const char *const k[5] = { "poll", "it", "dma", "itc", "dmac" };
    return (mode < 5U) ? k[mode] : "?";
}

/* One point where the host acts (the target, the scope): the AT line, then the handshake.
   The host does its part before raising HOST_REQ; this core goes on once it has dropped it. */
static bool host_at(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static bool host_at(const char *fmt, ...)
{
    if (s_bench_abort)
    {
        return false;
    }
    char t[160];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    LOG("AT %s", t);
    return sync0();
}

static void f1_clocks(void)
{
    s_cpu_hz = SystemCoreClock;
    s_k123   = HAL_RCC_GetPCLK1Freq();
    s_k4     = HAL_RCCEx_GetD3PCLK1Freq();
    s_seed   = DWT->CYCCNT | 1U;
    s_taps   = ((GPIOF->IDR >> IST_PIN_TAP_SCL) & 1UL) != 0U;
    s_f_main = s_taps ? 400000U : 1000000U;
    s_rise_ns = s_taps ? 300U : 40U;                    /* taps: the scope measures 250 ns (30-70 %) */
    s_fall_ns = 10U;
    log_long("F1 clocks (HAL, from HSE_VALUE): CPU1 %lu Hz, I2C1-3 kernel %lu Hz, I2C4 kernel %lu Hz; bench state "
             "from PF14: %s; CalcTiming bus t_r %lu ns t_f %lu ns; run seed 0x%08lX", (unsigned long)s_cpu_hz,
             (unsigned long)s_k123, (unsigned long)s_k4, s_taps ? "taps" : "notaps", (unsigned long)s_rise_ns,
             (unsigned long)s_fall_ns, (unsigned long)s_seed);
}

/* TIMINGR of the bench for kernel k and speed f (CalcTiming with the bus figures above),
   with the filters every Init of the bench uses (analog on, DNF 0; T5 tells the host). */
#define BENCH_ANALOG_FILTER   1U
#define BENCH_DNF             0U

static I2cBm_Status_t bench_timing(uint32_t k, uint32_t f, I2cBm_TimingOut_t *o)
{
    const I2cBm_TimingIn_t in = { k, f, s_rise_ns, s_fall_ns, BENCH_ANALOG_FILTER != 0U, BENCH_DNF };
    return I2C_BM_CalcTiming(&in, o);
}

/* ---- CPU2 commands with four arguments ------------------------------------------------- */
static bool cm4_send4(IstCmd_t cmd, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    IstArea_t *a = IST_AREA;
    a->arg1 = a1;
    a->arg2 = a2;
    a->arg3 = a3;
    return cm4_send(cmd, a0);
}

/* ---- I2C4 on CPU1: pins, IRQ, BDMA ------------------------------------------------------- */
static I2cBm_Xfer_t      s_it;
static volatile bool     s_it_ll;           /* the running IT transfer is i2c_ll's (F2) */
static volatile bool     s_it_hal;          /* ... i2c_hal's: the HAL's handlers serve it (F3) */
static volatile bool     s_it_active;
static volatile uint32_t s_it_irqs;
static uint32_t          s_irq_min = 0xFFFFFFFFUL;
static uint32_t          s_irq_max;
static I2C_HandleTypeDef s_hi2c4;           /* F3: the caller's handle of i2c_hal on I2C4 */

static void it_serve(bool er)
{
    s_it_irqs++;
    if (s_it_active && s_it_hal)
    {
        /* F3: the project's handlers call the HAL's (i2c_hal.h, C14); the transfer is
           over when the HAL puts the handle back to READY. */
        if (er)
        {
            HAL_I2C_ER_IRQHandler(&s_hi2c4);
        }
        else
        {
            HAL_I2C_EV_IRQHandler(&s_hi2c4);
        }
        if (s_hi2c4.State == HAL_I2C_STATE_READY)
        {
            s_it_active = false;
        }
    }
    else if (s_it_active)
    {
        const uint32_t t0 = DWT->CYCCNT;
        const bool done = ist_xferirq(s_it_ll, I2C4, &s_it);
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
        I2C4->CR1 &= ~(I2C_CR1_TXIE | I2C_CR1_RXIE | I2C_CR1_TCIE | I2C_CR1_STOPIE | I2C_CR1_NACKIE |
                       I2C_CR1_ERRIE | I2C_CR1_ADDRIE);   /* a stray one: silenced */
    }
}

/* The project's vectors for the test (plan C14), CPU1's NVIC, priority 6. */
void I2C4_EV_IRQHandler(void)
{
    it_serve(false);
}

void I2C4_ER_IRQHandler(void)
{
    it_serve(true);
}

static void nvic_i2c4(bool on)
{
    IRQn_Type ev = I2C4_EV_IRQn;
    IRQn_Type er = I2C4_ER_IRQn;
    (void)I2C_BM_GetIRQn(I2C4, &ev, &er);
    const IRQn_Type q[2] = { ev, er };
    for (uint32_t k = 0U; k < 2U; k++)
    {
        if (on)
        {
            HAL_NVIC_SetPriority(q[k], IRQ_PRIO, 0U);
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

static DMA_HandleTypeDef s_hbd_rx;
static DMA_HandleTypeDef s_hbd_tx;

/* F3: the HAL's DMA path needs the channels' interrupts (HAL_DMA_IRQHandler calls the
   I2C HAL's completion). i2c_bm and i2c_ll poll the I2C instead: their channels keep
   the NVIC untouched (irq_priority -1), so these vectors never run for them. */
void BDMA_Channel0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hbd_rx);
}

void BDMA_Channel1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hbd_tx);
}

static bool bd_init(DMA_HandleTypeDef *h, BDMA_Channel_TypeDef *ch, bool tx, int32_t prio)
{
    uint32_t req = 0U;
    (void)I2C_BM_GetDmaRequest(I2C4, tx, &req);
    const DmaHal_Config_t c = {
        .instance = ch, .request = req, .direction = tx ? DMA_MEMORY_TO_PERIPH : DMA_PERIPH_TO_MEMORY,
        .periph_inc = DMA_PINC_DISABLE, .mem_inc = DMA_MINC_ENABLE, .periph_align = DMA_PDATAALIGN_BYTE,
        .mem_align = DMA_MDATAALIGN_BYTE, .mode = DMA_NORMAL, .priority = DMA_PRIORITY_HIGH,
        .fifo_mode = DMA_FIFOMODE_DISABLE, .fifo_threshold = DMA_FIFO_THRESHOLD_FULL,
        .mem_burst = DMA_MBURST_SINGLE, .periph_burst = DMA_PBURST_SINGLE, .enable_clock = true, .irq_priority = prio };
    return DMA_HAL_Init(h, &c) == HAL_OK;
}

static uint32_t sentinel_left(const uint8_t *b, uint32_t n)
{
    uint32_t left = 0U;
    for (uint32_t i = 0U; i < n; i++)
    {
        if (b[i] == SENTINEL)
        {
            left++;
        }
    }
    return left;
}

/* One transfer on I2C4 (initialised), the twin of CPU2's cmd_xfer(): polling and IT on
   s_big (AXI SRAM, CPU only), DMA on s_bd_tx/s_bd_rx (SRAM4, BDMA, non-cacheable). */
/* F3: a cycle budget of the test as timeout_ms of the HAL tier (at least 2 ms: the tick
   is 1 ms, and a wait of 1 ms can end at once). */
static uint32_t hal_ms(uint32_t max_cycles)
{
    return (max_cycles / (s_cpu_hz / 1000U)) + 2U;
}

/* The last byte of a BDMA read has left RXDR: the channel's count is 0. */
static bool bd_rx_empty(void)
{
    return __HAL_DMA_GET_COUNTER(&s_hbd_rx) == 0U;
}

static void cm7_xfer(uint32_t xf, uint32_t n, uint32_t arg, uint32_t max_cycles, uint32_t r[IST_XR_N])
{
    for (uint32_t k = 0U; k < IST_XR_N; k++)
    {
        r[k] = 0U;
    }
    const uint32_t op = IST_XF_OP(xf);
    const uint32_t mode = IST_XF_MODE(xf);
    const uint32_t addr = IST_XF_ADDR(xf);
    const bool ll = IST_XF_IS_LL(xf);
    const bool hal = IST_XF_IS_HAL(xf);
    const uint32_t tmo_ms = hal_ms(max_cycles);
    const bool dma = (mode == IST_MODE_DMA) || (mode == IST_MODE_DMA_CTRL);
    uint8_t *buf = dma ? ((op == IST_OP_WRITE) ? s_bd_tx : s_bd_rx) : s_big;
    if ((n > (dma ? BD_N : F1_BIG)) || (max_cycles == 0U))
    {
        r[IST_XR_STATUS] = (uint32_t)I2C_BM_ERR_PARAM;
        return;
    }
    const bool rd = (op == IST_OP_READ) || (op == IST_OP_WRITEREAD);
    if (op == IST_OP_WRITE)
    {
        ist_fill(buf, n, arg);
    }
    else
    {
        for (uint32_t i = 0U; i < n; i++)
        {
            buf[i] = SENTINEL;
        }
    }
    s_ptrb = (uint8_t)arg;
    I2cBm_Status_t st = I2C_BM_ERR_PARAM;
    uint32_t done_tx = 0U;
    uint32_t done_rx = 0U;
    uint32_t aux = 0U;
    uint32_t t0 = DWT->CYCCNT;
    if ((mode == IST_MODE_POLL) && hal)
    {
        /* F3: the HAL's blocking calls, bounded by timeout_ms of HAL_GetTick. */
        if (op == IST_OP_WRITE)
        {
            st = I2C_HAL_Write(&s_hi2c4, addr, buf, n, tmo_ms);
        }
        else if (op == IST_OP_READ)
        {
            st = I2C_HAL_Read(&s_hi2c4, addr, buf, n, tmo_ms);
        }
        else if (op == IST_OP_WRITEREAD)
        {
            st = I2C_HAL_WriteRead(&s_hi2c4, addr, &s_ptrb, 1U, buf, n, tmo_ms);
        }
        else
        {
            st = I2C_HAL_Write(&s_hi2c4, addr, NULL, 0U, tmo_ms);
        }
        r[IST_XR_CYCLES] = DWT->CYCCNT - t0;
        const bool ok = st == I2C_BM_OK;
        done_tx = ok ? ((op == IST_OP_WRITE) ? n : ((op == IST_OP_WRITEREAD) ? 1U : 0U)) : 0U;
        done_rx = (ok && rd) ? n : 0U;
    }
    else if (mode == IST_MODE_POLL)
    {
        if (op == IST_OP_WRITE)
        {
            st = ist_write(ll, I2C4, addr, buf, n, max_cycles);
        }
        else if (op == IST_OP_READ)
        {
            st = ist_read(ll, I2C4, addr, buf, n, max_cycles);
        }
        else if (op == IST_OP_WRITEREAD)
        {
            st = ist_writeread(ll, I2C4, addr, &s_ptrb, 1U, buf, n, max_cycles);
        }
        else
        {
            st = ist_write(ll, I2C4, addr, NULL, 0U, max_cycles);
        }
        r[IST_XR_CYCLES] = DWT->CYCCNT - t0;
        done_tx = (op == IST_OP_WRITE) ? n : ((op == IST_OP_WRITEREAD) ? 1U : 0U);
        done_rx = rd ? n : 0U;
    }
    else if ((mode == IST_MODE_IT) || (mode == IST_MODE_IT_CTRL))
    {
        s_it.addr7 = addr;
        s_it.tx = (op == IST_OP_WRITE) ? buf : &s_ptrb;
        s_it.ntx = (op == IST_OP_WRITE) ? n : ((op == IST_OP_WRITEREAD) ? 1U : 0U);
        s_it.rx = buf;
        s_it.nrx = rd ? n : 0U;
        nvic_i2c4(true);
        s_it_irqs = 0U;
        s_it_ll = ll;
        s_it_hal = hal;
        s_it_active = true;
        t0 = DWT->CYCCNT;
        st = hal ? I2C_HAL_StartIt(&s_hi2c4, addr, s_it.tx, s_it.ntx, s_it.rx, s_it.nrx) : ist_startit(ll, I2C4, &s_it);
        if (mode == IST_MODE_IT_CTRL)
        {
            /* T6's control: every enable cleared by hand before the first event. */
            const uint32_t pm = __get_PRIMASK();
            __disable_irq();
            I2C4->CR1 &= ~(I2C_CR1_TXIE | I2C_CR1_RXIE | I2C_CR1_TCIE | I2C_CR1_STOPIE | I2C_CR1_NACKIE | I2C_CR1_ERRIE);
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
                s_stalls++;
                break;
            }
        }
        const bool timed_out = s_it_active;
        s_it_active = false;
        if (st == I2C_BM_OK)
        {
            /* F3: the HAL's verdict and the read-back of its close (I2C_HAL_Wait on a
               handle already READY returns at once). */
            st = timed_out ? I2C_BM_ERR_TIMEOUT : (hal ? I2C_HAL_Wait(&s_hi2c4, 1U) : s_it.status);
        }
        r[IST_XR_CYCLES] = DWT->CYCCNT - t0;
        r[IST_XR_IRQS] = s_it_irqs;
        done_tx = hal ? ((st == I2C_BM_OK) ? s_it.ntx : 0U) : s_it.done_tx;
        done_rx = hal ? ((st == I2C_BM_OK) ? s_it.nrx : 0U) : s_it.done_rx;
        const uint32_t before = s_it_irqs;
        (void)delay_ms(10U);                                 /* T6: 10 ms with no I2C4 IRQ */
        r[IST_XR_IRQS_AFTER] = s_it_irqs - before;
        nvic_i2c4(false);
        if (timed_out)
        {
            /* still PENDING (the HAL: its handle still busy): no progress */
            aux = hal ? ((s_hi2c4.State != HAL_I2C_STATE_READY) ? (uint32_t)I2C_BM_PENDING : 0U) :
                        (uint32_t)s_it.status;
        }
    }
    else if (hal)
    {
        /* F3: the HAL's DMA path. The caller links its BDMA handles (02_dma, interrupts
           on); the I2C interrupts serve the HAL's NACK, TCR and STOP. */
        nvic_i2c4(true);
        s_it_irqs = 0U;
        s_it_hal = true;
        s_it_active = true;
        bool sok;
        I2cBm_Status_t s1;
        if (op == IST_OP_WRITE)
        {
            sok = bd_init(&s_hbd_tx, BDMA_Channel1, true, (int32_t)IRQ_PRIO);
            __HAL_LINKDMA(&s_hi2c4, hdmatx, s_hbd_tx);
            t0 = DWT->CYCCNT;
            s1 = sok ? I2C_HAL_StartDma(&s_hi2c4, addr, buf, NULL, n) : I2C_BM_ERR_HAL;
        }
        else
        {
            sok = bd_init(&s_hbd_rx, BDMA_Channel0, false, (int32_t)IRQ_PRIO);
            __HAL_LINKDMA(&s_hi2c4, hdmarx, s_hbd_rx);
            t0 = DWT->CYCCNT;
            s1 = sok ? I2C_HAL_StartDma(&s_hi2c4, addr, NULL, buf, n) : I2C_BM_ERR_HAL;
            if ((s1 == I2C_BM_OK) && (mode == IST_MODE_DMA_CTRL))
            {
                const uint32_t pm = __get_PRIMASK();         /* T7's control: RXDMAEN cleared by hand */
                __disable_irq();
                I2C4->CR1 &= ~I2C_CR1_RXDMAEN;
                __set_PRIMASK(pm);
            }
        }
        st = (s1 == I2C_BM_OK) ? I2C_HAL_Wait(&s_hi2c4, tmo_ms) : s1;
        r[IST_XR_CYCLES] = DWT->CYCCNT - t0;
        s_it_active = false;
        nvic_i2c4(false);
        r[IST_XR_IRQS] = s_it_irqs;
        if (op == IST_OP_WRITE)
        {
            (void)HAL_DMA_Abort(&s_hbd_tx);
            (void)DMA_HAL_DeInit(&s_hbd_tx);                 /* its NVIC line off again */
            s_hi2c4.hdmatx = NULL;
            done_tx = (st == I2C_BM_OK) ? n : 0U;
        }
        else
        {
            done_rx = n - __HAL_DMA_GET_COUNTER(&s_hbd_rx);
            (void)HAL_DMA_Abort(&s_hbd_rx);
            (void)DMA_HAL_DeInit(&s_hbd_rx);
            s_hi2c4.hdmarx = NULL;
        }
        aux = sok ? 0U : 1U;
    }
    else
    {
        /* BDMA through dma_hal (plan rule 8): the channel first, then DmaStart, WaitEnd,
           the channel aborted, DmaEnd. */
        bool sok;
        if (op == IST_OP_WRITE)
        {
            sok = bd_init(&s_hbd_tx, BDMA_Channel1, true, -1) &&
                  (HAL_DMA_Start(&s_hbd_tx, (uint32_t)&buf[1], (uint32_t)&I2C4->TXDR, n - 1U) == HAL_OK);
            t0 = DWT->CYCCNT;
            const I2cBm_Status_t s1 = ist_dmaw(ll, I2C4, addr, buf, n);
            st = (s1 == I2C_BM_OK) ? ist_waitend(ll, I2C4, n, max_cycles) : s1;
            r[IST_XR_CYCLES] = DWT->CYCCNT - t0;
            (void)HAL_DMA_Abort(&s_hbd_tx);
            (void)DMA_HAL_DeInit(&s_hbd_tx);
            done_tx = n;
        }
        else
        {
            sok = bd_init(&s_hbd_rx, BDMA_Channel0, false, -1) &&
                  (HAL_DMA_Start(&s_hbd_rx, (uint32_t)&I2C4->RXDR, (uint32_t)buf, n) == HAL_OK);
            t0 = DWT->CYCCNT;
            const I2cBm_Status_t s1 = ist_dmar(ll, I2C4, addr, n);
            if ((s1 == I2C_BM_OK) && (mode == IST_MODE_DMA_CTRL))
            {
                const uint32_t pm = __get_PRIMASK();         /* T7's control: RXDMAEN cleared by hand */
                __disable_irq();
                I2C4->CR1 &= ~I2C_CR1_RXDMAEN;
                __set_PRIMASK(pm);
            }
            st = (s1 == I2C_BM_OK) ? ist_waitend(ll, I2C4, n, max_cycles) : s1;
            r[IST_XR_CYCLES] = DWT->CYCCNT - t0;
            if (st == I2C_BM_OK)
            {
                (void)wait_until(bd_rx_empty, 10U);       /* CYCCNT, not the HAL tick (m21) */
            }
            done_rx = n - __HAL_DMA_GET_COUNTER(&s_hbd_rx);
            (void)HAL_DMA_Abort(&s_hbd_rx);
            (void)DMA_HAL_DeInit(&s_hbd_rx);
        }
        const I2cBm_Status_t se = ist_dmaend(ll, I2C4);
        aux = (sok ? 0U : 1U) | ((se == I2C_BM_OK) ? 0U : 2U);
    }
    uint32_t first = 0xFFFFFFFFUL;
    r[IST_XR_STATUS] = (uint32_t)st;
    r[IST_XR_CRC] = (rd || (op == IST_OP_WRITE)) ? ist_crc32(buf, n) : 0U;
    r[IST_XR_MISM] = rd ? ist_check_target(buf, n, arg & 0xFFU, &first) : 0U;
    r[IST_XR_FIRST_BAD] = first;
    r[IST_XR_DONE_TX] = done_tx;
    r[IST_XR_DONE_RX] = done_rx;
    r[IST_XR_CR1_AFTER] = I2C4->CR1;
    r[IST_XR_SENTINEL] = rd ? sentinel_left(buf, n) : 0U;
    r[IST_XR_ISR_AFTER] = I2C4->ISR;
    r[IST_XR_AUX] = aux;
}

/* A transfer on CPU2's I2C1 (m4) or this core's I2C4; r receives IST_XR_*. The bytes of
   a write are regenerated here from the seed for the X line (both cores fill the same). */
static bool xfer(bool m4, uint32_t xf, uint32_t n, uint32_t arg, uint32_t max_cycles, uint32_t r[IST_XR_N])
{
    if (s_ll)
    {
        xf |= IST_XF_TIER_LL;                             /* F2: through i2c_ll, on either core */
    }
    if (s_hal && !m4)
    {
        xf |= IST_XF_TIER_HAL;                            /* F3: through i2c_hal, CPU1 only */
    }
    if (!m4)
    {
        cm7_xfer(xf, n, arg, max_cycles, r);
        return true;
    }
    if (!cm4_send4(IST_CMD_M4_XFER, xf, n, arg, max_cycles))
    {
        return false;
    }
    for (uint32_t k = 0U; k < IST_XR_N; k++)
    {
        r[k] = IST_AREA->m4_res[k];
    }
    return true;
}

/* The name suffix of the tier of the current step: "-LL" (F2), "-HAL" (F3). */
static const char *tier_sfx(void)
{
    return s_ll ? "-LL" : (s_hal ? "-HAL" : "");
}

/* The X line of one transfer: what the host pairs with the target's RPT (t=1). For a
   write of up to 8 bytes, the bytes too (the host compares them with the scope). */
static void xline(const char *test, bool m4, uint32_t f, uint32_t xf, uint32_t n, uint32_t arg, const uint32_t *r,
                  bool target)
{
    char d[24] = "";
    const uint32_t op = IST_XF_OP(xf);
    if ((op == IST_OP_WRITE) && (n <= 8U))
    {
        uint8_t b[8];
        ist_fill(b, n, arg);
        for (uint32_t i = 0U; i < n; i++)
        {
            (void)snprintf(&d[2U * i], 3U, "%02X", b[i]);
        }
    }
    log_long("X %s%s %s f=%lu op=%s mode=%s a=%02lX n=%lu p=%lu st=%lu crc=%08lX mis=%lu bad=%ld cyc=%lu dtx=%lu drx=%lu "
             "irq=%lu irqa=%lu cr1=%08lX sen=%lu isr=%08lX aux=%lu t=%u%s%s", test, tier_sfx(),
             m4 ? "cm4" : "cm7", (unsigned long)f,
             op_name(op), mode_name(IST_XF_MODE(xf)), (unsigned long)IST_XF_ADDR(xf), (unsigned long)n,
             (unsigned long)arg, (unsigned long)r[IST_XR_STATUS], (unsigned long)r[IST_XR_CRC],
             (unsigned long)r[IST_XR_MISM], (long)(int32_t)r[IST_XR_FIRST_BAD], (unsigned long)r[IST_XR_CYCLES],
             (unsigned long)r[IST_XR_DONE_TX], (unsigned long)r[IST_XR_DONE_RX], (unsigned long)r[IST_XR_IRQS],
             (unsigned long)r[IST_XR_IRQS_AFTER], (unsigned long)r[IST_XR_CR1_AFTER], (unsigned long)r[IST_XR_SENTINEL],
             (unsigned long)r[IST_XR_ISR_AFTER], (unsigned long)r[IST_XR_AUX], target ? 1U : 0U,
             (d[0] != '\0') ? " d=" : "", d);
}

/* Init of the instance under test on either core; the image read back. */
static I2cBm_Status_t inst_init(bool m4, uint32_t timingr, uint32_t tmo, uint32_t img[IST_NREG])
{
    if (m4)
    {
        if (!cm4_send4(IST_CMD_M4_INIT, timingr, BENCH_ANALOG_FILTER | (BENCH_DNF << 8), tmo, s_ll ? 1U : 0U))
        {
            return I2C_BM_ERR_HAL;
        }
        for (uint32_t k = 0U; k < IST_NREG; k++)
        {
            img[k] = IST_AREA->m4_res[1U + k];
        }
        return (I2cBm_Status_t)IST_AREA->m4_res[0];
    }
    const I2cBm_Config_t c = { I2C4, timingr, BENCH_ANALOG_FILTER != 0U, BENCH_DNF, tmo };
    (void)I2C_BM_EnableClock(I2C4);
    I2cBm_Status_t st;
    if (s_hal)
    {
        s_hi2c4.Instance = I2C4;                          /* F3: the caller's handle */
        st = I2C_HAL_Init(&s_hi2c4, &c);
    }
    else
    {
        st = ist_init(s_ll, &c);
    }
    ist_regs(I2C4, img);
    return st;
}

/* Deinit, then T14: the reset image of RM0399 50.7 typed in Common (ist_reset_diff_img()). */
static bool inst_deinit(bool m4, uint32_t *diff)
{
    I2cBm_Status_t st;
    uint32_t img[IST_NREG];
    if (m4)
    {
        if (!cm4_send4(IST_CMD_M4_DEINIT, 0U, 0U, 0U, 0U))
        {
            *diff = 0xFFU;
            return false;
        }
        st = (I2cBm_Status_t)IST_AREA->m4_res[0];
        for (uint32_t k = 0U; k < IST_NREG; k++)
        {
            img[k] = IST_AREA->m4_res[1U + k];
        }
    }
    else
    {
        s_hi2c4.Instance = I2C4;
        st = s_hal ? I2C_HAL_Deinit(&s_hi2c4) : I2C_BM_Deinit(I2C4);   /* F3: also the handle to RESET */
        ist_regs(I2C4, img);
    }
    const uint32_t d = ist_reset_diff_img(img);         /* the one typed table of Common (audit F2, m38) */
    *diff = d;
    return (st == I2C_BM_OK) && (d == 0U);
}

/* T14 after every test (audit F1, m24): the Deinit that ends it, judged against the
   reset image of the RM typed in Common (ist_reset_diff_img()). */
static void deinit_judged(bool m4, const char *what)
{
    uint32_t diff = 0U;
    const bool ok = inst_deinit(m4, &diff);
    verdict(ok, "T14 %s after %s: Deinit, registers that differ from the reset image 0x%02lX", m4 ? "I2C1" : "I2C4",
            what, (unsigned long)diff);
}

/* ---- T3: CalcTiming, printed for the host to recompute (C15) -------------------------- */
static uint32_t s_b1_calc_max;

static void t3_one(uint32_t *i, uint32_t k, uint32_t f, uint32_t tr, uint32_t tf, bool af, uint32_t dnf)
{
    const I2cBm_TimingIn_t in = { k, f, tr, tf, af, dnf };
    I2cBm_TimingOut_t o = { 0U, 0U, 0U };
    const uint32_t t0 = DWT->CYCCNT;
    const I2cBm_Status_t st = I2C_BM_CalcTiming(&in, &o);
    const uint32_t dt = DWT->CYCCNT - t0;
    s_b1_calc_max = (dt > s_b1_calc_max) ? dt : s_b1_calc_max;
    LOG("T3 %lu %lu %lu %lu %lu %u %lu %u %08lX %lu %lu", (unsigned long)*i, (unsigned long)k, (unsigned long)f,
        (unsigned long)tr, (unsigned long)tf, (unsigned)af, (unsigned long)dnf, (unsigned)st,
        (unsigned long)((st == I2C_BM_OK) ? o.timingr : 0U), (unsigned long)((st == I2C_BM_OK) ? o.mode : 0U),
        (unsigned long)((st == I2C_BM_OK) ? o.scl_hz_model : 0U));
    (*i)++;
}

static void f1_t3(void)
{
    static const uint32_t ks[] = { 4000000U, 8000000U, 10000000U, 16000000U, 20000000U, 48000000U, 64000000U,
                                   100000000U, 125000000U };
    static const uint32_t fs[] = { 10000U, 100000U, 250000U, 400000U, 1000000U };
    static const uint32_t rf[][2] = { { 100U, 10U }, { 250U, 20U }, { 300U, 300U }, { 120U, 120U } };
    uint32_t n = 0U;
    for (uint32_t a = 0U; a < (sizeof(ks) / sizeof(ks[0])); a++)
    {
        for (uint32_t b = 0U; b < (sizeof(fs) / sizeof(fs[0])); b++)
        {
            for (uint32_t c = 0U; c < (sizeof(rf) / sizeof(rf[0])); c++)
            {
                for (uint32_t d = 0U; d < 2U; d++)
                {
                    t3_one(&n, ks[a], fs[b], rf[c][0], rf[c][1], d != 0U, 0U);
                }
            }
        }
    }
    /* The digital filter, the limits of each rule, and the parameters. */
    static const uint32_t dnfs[] = { 1U, 2U, 4U, 8U, 15U };
    for (uint32_t d = 0U; d < (sizeof(dnfs) / sizeof(dnfs[0])); d++)
    {
        t3_one(&n, 100000000U, 100000U, 250U, 20U, true, dnfs[d]);
        t3_one(&n, 100000000U, 400000U, 250U, 20U, false, dnfs[d]);
        t3_one(&n, 100000000U, 1000000U, 40U, 10U, true, dnfs[d]);
        t3_one(&n, 20000000U, 1000000U, 120U, 120U, true, dnfs[d]);
    }
    static const uint32_t edge[][5] = {
        { 4000000U, 100000U, 1000U, 300U, 1U }, { 3999999U, 100000U, 100U, 10U, 0U },
        { 10000000U, 400000U, 300U, 300U, 1U }, { 9999999U, 400000U, 100U, 10U, 0U },
        { 20000000U, 1000000U, 120U, 120U, 1U }, { 19999999U, 1000000U, 100U, 10U, 0U },
        { 100000000U, 100001U, 250U, 20U, 1U }, { 100000000U, 400001U, 120U, 20U, 1U },
        { 100000000U, 99900U, 250U, 20U, 1U }, { 101250000U, 400000U, 200U, 10U, 1U },
        { 64000000U, 400000U, 200U, 10U, 1U }, { 64000000U, 1000000U, 40U, 10U, 1U },
        { 100000000U, 1U, 0U, 0U, 0U }, { 4294967295U, 1000000U, 0U, 0U, 0U }, { 4294967295U, 10000U, 1000U, 300U, 1U },
        { 100000000U, 100000U, 1001U, 20U, 1U }, { 100000000U, 400000U, 301U, 20U, 1U },
        { 100000000U, 1000000U, 121U, 20U, 1U }, { 100000000U, 1000000U, 40U, 121U, 1U },
        { 0U, 100000U, 100U, 10U, 1U }, { 100000000U, 0U, 100U, 10U, 1U }, { 100000000U, 1000001U, 100U, 10U, 1U },
    };
    for (uint32_t e = 0U; e < (sizeof(edge) / sizeof(edge[0])); e++)
    {
        t3_one(&n, edge[e][0], edge[e][1], edge[e][2], edge[e][3], edge[e][4] != 0U, 0U);
    }
    t3_one(&n, 100000000U, 100000U, 250U, 20U, true, 16U);
    LOG("T3 done: %lu CalcTiming vectors (the host recomputes them); the slowest took %lu cycles",
        (unsigned long)n, (unsigned long)s_b1_calc_max);
}

/* ---- T1 and T2: the Common code on CPU1's instances and, through CPU2, on I2C1 ----------- */
static void t1_print(const char *who, const IstT1Result_t *t)
{
    for (uint32_t i = 0U; (i < t->cases) && (i < IST_T1_MAX); i++)
    {
        if (t->got[i] != t->exp[i])
        {
            LOG("T1 %s case %lu: status %u, expected %u", who, (unsigned long)i, (unsigned)t->got[i],
                (unsigned)t->exp[i]);
        }
    }
}

static void f1_t1(void)
{
    static IstT1Result_t t;
    I2cBm_TimingOut_t o = { 0U, 0U, 0U };
    (void)bench_timing(s_k123, 100000U, &o);
    (void)I2C_BM_EnableClock(I2C2);
    ist_t1(I2C2, o.timingr, s_ll, &t);
    t1_print("CM7 I2C2", &t);
    verdict((t.bad == 0U) && (t.cases == (s_ll ? IST_T1_LL_CASES : IST_T1_CASES)) && (t.groups_same == 3U) &&
            (t.control_changed == 1U),
            "T1 CM7 refusals on I2C2: %lu cases (62, 63 with i2c_ll), %lu with another status; instance, RCC_C1/C2 APB1LENR/APB4ENR, "
            "D2CCIP2R, D3CCIPR, SYSCFG_PMCR unchanged in the 2 groups: 0x%lX (0x3); control (a valid Init) changes "
            "them %lu (Init %lu)", (unsigned long)t.cases, (unsigned long)t.bad, (unsigned long)t.groups_same,
            (unsigned long)t.control_changed, (unsigned long)t.init_status);
    (void)I2C_BM_DisableClock(I2C2);

    if (cm4_send4(IST_CMD_M4_T1, o.timingr, s_ll ? 1U : 0U, 0U, 0U))
    {
        const volatile uint32_t *r = IST_AREA->m4_res;
        verdict((r[1] == 0U) && (r[0] == (s_ll ? IST_T1_LL_CASES : IST_T1_CASES)) && (r[2] == 3U) && (r[3] == 1U),
                "T1 CM4 refusals on I2C1 (the same Common cases): %lu cases (62, 63 with i2c_ll), %lu with another status; photo unchanged "
                "in the 2 groups 0x%lX (0x3); control changes it %lu", (unsigned long)r[0], (unsigned long)r[1],
                (unsigned long)r[2], (unsigned long)r[3]);
    }
}

static void t2_print(const char *who, const IstT2Result_t *t)
{
    verdict((t->bad == 0U) && (t->bad_deinit == 0U) && (t->bad_status == 0U) && (t->control == t->cases) &&
            (t->cases == 120U),
            "T2 %s images over TIMINGR x filter x DNF x timeout: %lu cases, %lu differ from the RM fields typed by hand "
            "(first case<<4|reg 0x%lX got 0x%08lX expected 0x%08lX), %lu Deinit not at the reset image, %lu Init "
            "neither OK nor ERR_BUS; control (expected with DNF + 1, the same comparison) told apart in %lu cases "
            "(all)", who, (unsigned long)t->cases,
            (unsigned long)t->bad, (unsigned long)t->first_bad, (unsigned long)t->got, (unsigned long)t->exp,
            (unsigned long)t->bad_deinit, (unsigned long)t->bad_status, (unsigned long)t->control);
}

static void f1_t2(void)
{
    static const struct { I2C_TypeDef *i2c; const char *name; } inst[3] = {
        { I2C2, "CM7 I2C2" }, { I2C3, "CM7 I2C3" }, { I2C4, "CM7 I2C4" } };
    for (uint32_t i = 0U; i < 3U; i++)
    {
        IstT2Result_t t;
        (void)I2C_BM_EnableClock(inst[i].i2c);
        ist_t2(inst[i].i2c, &t);
        t2_print(inst[i].name, &t);
        if (inst[i].i2c != I2C4)
        {
            (void)I2C_BM_DisableClock(inst[i].i2c);
        }
    }
    if (cm4_send4(IST_CMD_M4_T2, 0U, 0U, 0U, 0U))
    {
        const volatile uint32_t *r = IST_AREA->m4_res;
        IstT2Result_t t = { r[0], r[1], r[2], r[7], r[3], r[4], r[5], r[6] };
        t2_print("CM4 I2C1", &t);
    }
}

/* Silicon facts of the table against the CMSIS/HAL constants, and the kernel field
   against RCC read directly (C1, plan rule 2). */
static void f1_caps(void)
{
    I2C_TypeDef *const inst[4] = { I2C1, I2C2, I2C3, I2C4 };
    static const IRQn_Type ev[4] = { I2C1_EV_IRQn, I2C2_EV_IRQn, I2C3_EV_IRQn, I2C4_EV_IRQn };
    static const IRQn_Type er[4] = { I2C1_ER_IRQn, I2C2_ER_IRQn, I2C3_ER_IRQn, I2C4_ER_IRQn };
    static const uint32_t rx[4] = { DMA_REQUEST_I2C1_RX, DMA_REQUEST_I2C2_RX, DMA_REQUEST_I2C3_RX, BDMA_REQUEST_I2C4_RX };
    static const uint32_t tx[4] = { DMA_REQUEST_I2C1_TX, DMA_REQUEST_I2C2_TX, DMA_REQUEST_I2C3_TX, BDMA_REQUEST_I2C4_TX };
    const uint32_t d2 = RCC->D2CCIP2R;
    const uint32_t d3 = RCC->D3CCIPR;
    /* Field positions typed from RM0399 9.7.20 (I2C123SEL 13:12) and 9.7.21 (I2C4SEL 9:8). */
    const uint32_t raw[4] = { (d2 >> 12) & 3U, (d2 >> 12) & 3U, (d2 >> 12) & 3U, (d3 >> 8) & 3U };
    uint32_t bad = 0U;
    for (uint32_t i = 0U; i < 4U; i++)
    {
        I2cBm_Caps_t c = { 0xFFU, 0xFFU };
        IRQn_Type e = (IRQn_Type)0;
        IRQn_Type f = (IRQn_Type)0;
        uint32_t qr = 0U;
        uint32_t qt = 0U;
        uint32_t ks = 9U;
        const bool ok = I2C_BM_IsValidInstance(inst[i]) && (I2C_BM_GetCaps(inst[i], &c) == I2C_BM_OK) &&
                        (I2C_BM_GetIRQn(inst[i], &e, &f) == I2C_BM_OK) &&
                        (I2C_BM_GetDmaRequest(inst[i], false, &qr) == I2C_BM_OK) &&
                        (I2C_BM_GetDmaRequest(inst[i], true, &qt) == I2C_BM_OK) &&
                        (I2C_BM_GetKernelSource(inst[i], &ks) == I2C_BM_OK);
        const bool facts = (e == ev[i]) && (f == er[i]) && (qr == rx[i]) && (qt == tx[i]) &&
                           (c.dmamux == ((i == 3U) ? 2U : 1U)) &&
                           (c.kernel_select == ((i == 3U) ? I2C_BM_KSEL_I2C4 : I2C_BM_KSEL_I2C123)) &&
                           (ks == raw[i]) && (ks == 0U);
        if (!ok || !facts)
        {
            bad |= 1UL << i;
        }
    }
    const bool not_inst = !I2C_BM_IsValidInstance((const I2C_TypeDef *)((uint32_t)I2C1 + 4U)) &&
                          !I2C_BM_IsValidInstance((const I2C_TypeDef *)USART3) && !I2C_BM_IsValidInstance(NULL);
    verdict((bad == 0U) && not_inst,
            "T1/T2 capability table of I2C1..4 against stm32h755xx.h (IRQn EV/ER), stm32h7xx_hal_dma.h (DMAMUX1 "
            "33/34 35/36 73/74, DMAMUX2 13/14) and RCC read directly (I2C123SEL %lu, I2C4SEL %lu, 0 in prj_01): "
            "rows that differ 0x%lX; I2C1+4, USART3 and NULL not instances %u", (unsigned long)raw[0],
            (unsigned long)raw[3], (unsigned long)bad, (unsigned)not_inst);
}

/* SetFastModePlus: one bit of SYSCFG_PMCR, the rest unchanged, restored (C10, rule 10). */
static void f1_fmp(void)
{
    const uint32_t before = SYSCFG->PMCR;
    const I2cBm_Status_t s1 = I2C_BM_SetFastModePlus(I2C2, true);
    const uint32_t on = SYSCFG->PMCR;
    const I2cBm_Status_t s2 = I2C_BM_SetFastModePlus(I2C2, false);
    const uint32_t off = SYSCFG->PMCR;
    verdict((s1 == I2C_BM_OK) && (s2 == I2C_BM_OK) && (on == (before | SYSCFG_PMCR_I2C2_FMP)) &&
            (off == (before & ~SYSCFG_PMCR_I2C2_FMP)) && (off == before),
            "SetFastModePlus(I2C2): SYSCFG_PMCR 0x%08lX -> on 0x%08lX (bit 1 only) -> off 0x%08lX (as before), "
            "statuses %u %u", (unsigned long)before, (unsigned long)on, (unsigned long)off, (unsigned)s1, (unsigned)s2);
}

/* T13: CPU2's DisableClock(I2C2) while CPU1's bank enables it (RM0399 9.5.10). The
   oracle is whether a register write lands: an unclocked I2C ignores it. */
static void f1_t13(void)
{
    I2cBm_TimingOut_t o = { 0U, 0U, 0U };
    (void)bench_timing(s_k123, 100000U, &o);
    const I2cBm_Config_t c = { I2C2, o.timingr, true, 0U, 0U };
    (void)I2C_BM_EnableClock(I2C2);
    (void)I2C_BM_Deinit(I2C2);
    I2C2->TIMINGR = o.timingr;                             /* PE = 0: written directly */
    const uint32_t c1on = RCC_C1->APB1LENR & RCC_APB1LENR_I2C2EN;
    bool m1 = false;
    bool m2 = false;
    uint32_t r1 = 0U;
    uint32_t r2 = 0U;
    if (cm4_send4(IST_CMD_M4_T13, 1U, 0U, 0U, 0U))
    {
        m1 = IST_AREA->m4_res[1] == (uint32_t)I2C_BM_OK;
        r1 = IST_AREA->m4_res[2];
    }
    I2C2->TIMEOUTR = 0x00000123UL;
    const bool land1 = (I2C2->TIMEOUTR == 0x00000123UL) && (I2C2->TIMINGR == o.timingr);
    if (cm4_send4(IST_CMD_M4_T13, 2U, 0U, 0U, 0U))
    {
        m2 = (IST_AREA->m4_res[0] == (uint32_t)I2C_BM_OK) && (IST_AREA->m4_res[1] == (uint32_t)I2C_BM_OK);
        r2 = IST_AREA->m4_res[2];
    }
    I2C2->TIMEOUTR = 0x00000456UL;
    const bool land2 = (I2C2->TIMEOUTR == 0x00000456UL);
    /* The control: CPU1 disables its bank too; nobody enables I2C2; a write is lost. */
    const I2cBm_Status_t dc = I2C_BM_DisableClock(I2C2);
    const uint32_t both = (RCC_C1->APB1LENR | RCC_C2->APB1LENR) & RCC_APB1LENR_I2C2EN;
    I2C2->TIMINGR = 0x10101010UL;
    (void)I2C_BM_EnableClock(I2C2);
    const uint32_t after = I2C2->TIMINGR;
    (void)I2C_BM_Init(&c);
    (void)I2C_BM_Deinit(I2C2);
    (void)I2C_BM_DisableClock(I2C2);
    verdict(m1 && land1 && ((r1 & RCC_APB1LENR_I2C2EN) == 0U) && (c1on != 0U),
            "T13 CPU2 DisableClock(I2C2) (RCC_C2 APB1LENR I2C2EN %lu) while RCC_C1 enables it (%lu): a TIMEOUTR write "
            "still lands %u", (unsigned long)((r1 & RCC_APB1LENR_I2C2EN) != 0U), (unsigned long)(c1on != 0U),
            (unsigned)land1);
    verdict(m2 && land2 && ((r2 & RCC_APB1LENR_I2C2EN) == 0U),
            "T13 CPU2 EnableClock + DisableClock(I2C2): RCC_C2 I2C2EN %lu after; a write still lands %u",
            (unsigned long)((r2 & RCC_APB1LENR_I2C2EN) != 0U), (unsigned)land2);
    verdict((dc == I2C_BM_OK) && (both == 0U) && (after == o.timingr),
            "T13 control: CPU1 DisableClock(I2C2) %u, both banks off; TIMINGR 0x10101010 written then, read with the "
            "clock back: 0x%08lX (the earlier 0x%08lX: the write was lost)", (unsigned)dc, (unsigned long)after,
            (unsigned long)o.timingr);
}

/* T15 (lesson E2): I2C2 has no pin in AF on this bench. A Write must end, bounded,
   and Deinit give the reset image. Control: I2C4 with its pins (step F1_CM7). */
static void f1_t15(void)
{
    I2cBm_TimingOut_t o = { 0U, 0U, 0U };
    (void)bench_timing(s_k123, 100000U, &o);
    const I2cBm_Config_t c = { I2C2, o.timingr, true, 0U, 0U };
    (void)I2C_BM_EnableClock(I2C2);
    const I2cBm_Status_t si = I2C_BM_Init(&c);
    const uint32_t isr0 = I2C2->ISR;
    uint8_t b[2] = { 0x11U, 0x22U };
    const uint32_t max = ms_cyc(5U);
    uint32_t t0 = DWT->CYCCNT;
    const I2cBm_Status_t sw = (si == I2C_BM_OK) ? I2C_BM_Write(I2C2, T_ADDR, b, 2U, max) : si;
    const uint32_t cw = DWT->CYCCNT - t0;
    uint32_t img[IST_NREG];
    ist_regs(I2C2, img);
    t0 = DWT->CYCCNT;
    const I2cBm_Status_t sr = (si == I2C_BM_OK) ? I2C_BM_Read(I2C2, T_ADDR, b, 2U, max) : si;
    const uint32_t cr = DWT->CYCCNT - t0;
    const I2cBm_Status_t sd = I2C_BM_Deinit(I2C2);
    const uint32_t diff = ist_reset_diff(I2C2);
    (void)I2C_BM_DisableClock(I2C2);
    log_long("T15 I2C2 without pins: Init %u, ISR after Init 0x%08lX; Write of 2 bytes %u in %lu cycles; registers "
             "after: CR1 0x%08lX CR2 0x%08lX ISR 0x%08lX; Read of 2 bytes %u in %lu cycles", (unsigned)si,
             (unsigned long)isr0, (unsigned)sw, (unsigned long)cw, (unsigned long)img[0], (unsigned long)img[1],
             (unsigned long)img[6], (unsigned)sr, (unsigned long)cr);
    const bool bounded = (cw <= (max + ms_cyc(1U))) && (cr <= (max + ms_cyc(1U)));
    verdict(((sw == I2C_BM_ERR_TIMEOUT) || (sw == I2C_BM_ERR_BUS)) && ((sr == I2C_BM_ERR_TIMEOUT) || (sr == I2C_BM_ERR_BUS)) &&
            bounded && (sd == I2C_BM_OK) && (diff == 0U),
            "T15 I2C2 without pins: Write %u, Read %u (ERR_TIMEOUT %u or ERR_BUS %u), each within max_cycles + 1 ms "
            "(%lu, %lu <= %lu); Deinit %u, registers that differ from reset 0x%02lX", (unsigned)sw, (unsigned)sr,
            (unsigned)I2C_BM_ERR_TIMEOUT, (unsigned)I2C_BM_ERR_BUS, (unsigned long)cw, (unsigned long)cr,
            (unsigned long)(max + ms_cyc(1U)), (unsigned)sd, (unsigned long)diff);
}

/* T14's control: an instance left initialised is told apart from the reset image. */
static void f1_t14_control(void)
{
    I2cBm_TimingOut_t o = { 0U, 0U, 0U };
    (void)bench_timing(s_k123, 100000U, &o);
    const I2cBm_Config_t c = { I2C3, o.timingr, true, 0U, 0U };
    (void)I2C_BM_EnableClock(I2C3);
    (void)I2C_BM_Init(&c);
    const uint32_t d1 = ist_reset_diff(I2C3);
    const I2cBm_Status_t sd = I2C_BM_Deinit(I2C3);
    const uint32_t d2 = ist_reset_diff(I2C3);
    (void)I2C_BM_DisableClock(I2C3);
    verdict((d1 != 0U) && (sd == I2C_BM_OK) && (d2 == 0U),
            "T14 control: I2C3 initialised and not deinitialised differs from the reset image in 0x%02lX (CR1, "
            "TIMINGR); after Deinit %u: 0x%02lX", (unsigned long)d1, (unsigned)sd, (unsigned long)d2);
}

/* The CPU clock against the AD2's (plan N1): MCU_ACK high for exactly CLKREF_CYC CYCCNT
   cycles, interrupts masked; the host times it with the logic analyzer. */
#define CLKREF_CYC   40000000UL       /* 100 ms at 400 MHz nominal */

static void f1_clkref(void)
{
    if (!host_at("CLKREF cyc=%lu cpu=%lu", (unsigned long)CLKREF_CYC, (unsigned long)s_cpu_hz))
    {
        return;
    }
    (void)delay_ms(50U);
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    const uint32_t t0 = DWT->CYCCNT;
    uint32_t last = t0;
    uint32_t same = 0U;
    bool stalled = false;
    ack(true);
    while ((DWT->CYCCNT - t0) < CLKREF_CYC)          /* masked: also ends if CYCCNT stops (m21) */
    {
        const uint32_t now = DWT->CYCCNT;
        same = (now == last) ? (same + 1U) : 0U;
        last = now;
        if (same > STALL_READS)
        {
            stalled = true;
            break;
        }
    }
    ack(false);
    __set_PRIMASK(pm);
    if (stalled)
    {
        s_stalls++;
    }
    LOG("CLKREF done%s", stalled ? " (CYCCNT stopped)" : "");
}

static void step_f1_nopin(void)
{
    if (!step_begin("F1_NOPIN"))
    {
        return;
    }
    f1_clocks();
    f1_clkref();
    f1_t3();
    f1_caps();
    f1_t1();
    f1_t2();
    f1_fmp();
    f1_t13();
    f1_t15();
    f1_t14_control();
    if (!host_at("F1_NOPIN end"))
    {
        return;
    }
    step_end();
}

/* ---- Transfers on the bus (steps F1_CM4 and F1_CM7) ------------------------------------- */

static uint32_t core_hz(bool m4)
{
    return m4 ? (s_cpu_hz / 2U) : s_cpu_hz;              /* CPU2 at 200 MHz (CM7 main.c) */
}

static bool pace(void)
{
    return delay_ms(PACE_MS);
}

/* One transfer: done, printed, paced. ok_st is the status it must end with. */
static bool run1(const char *test, bool m4, uint32_t f, uint32_t xf, uint32_t n, uint32_t arg, bool target,
                 I2cBm_Status_t ok_st, uint32_t *bad, uint32_t r[IST_XR_N])
{
    const uint32_t op = IST_XF_OP(xf);
    const uint32_t bytes = n + ((op == IST_OP_WRITEREAD) ? 2U : 1U);
    if (!xfer(m4, xf, n, arg, budget(core_hz(m4), f, bytes), r))
    {
        bench_abort("CM4 transfer command");
        return false;
    }
    xline(test, m4, f, xf, n, arg, r, target);
    const bool rd = (op == IST_OP_READ) || (op == IST_OP_WRITEREAD);
    if ((r[IST_XR_STATUS] != (uint32_t)ok_st) || (rd && (ok_st == I2C_BM_OK) && (r[IST_XR_MISM] != 0U)))
    {
        (*bad)++;
    }
    return pace();
}

/* T4 at one speed on one core (CPU2's I2C1 or CPU1's I2C4): reads from the target's
   known content, write-reads, the address probes, writes; the control (a returned byte
   inverted by the target) last. The host pairs every X line with t=1 with an RPT. */
static void t4_speed(bool m4, uint32_t f, uint32_t timingr)
{
    const char *core = m4 ? "CM4" : "CM7";
    uint32_t r[IST_XR_N];
    uint32_t bad = 0U;
    uint32_t p = 0U;
    if (!host_at("F334 INIT"))
    {
        return;
    }
    for (uint32_t i = 0U; i < T4_SIZES; i++)
    {
        const uint32_t n = k_t4_sizes[i];
        if (!run1("T4", m4, f, IST_XF(IST_OP_READ, IST_MODE_POLL, T_ADDR), n, p, true, I2C_BM_OK, &bad, r))
        {
            return;
        }
        p = (p + n) & 0xFFU;                              /* the target's pointer moves by n */
    }
    for (uint32_t i = 0U; i < T4_SIZES; i++)
    {
        const uint32_t ptr = (0x10U * (i + 1U)) & 0xFFU;
        if (!run1("T4", m4, f, IST_XF(IST_OP_WRITEREAD, IST_MODE_POLL, T_ADDR), k_t4_sizes[i], ptr, true, I2C_BM_OK,
                  &bad, r))
        {
            return;
        }
    }
    if (!run1("T4", m4, f, IST_XF(IST_OP_PROBE, IST_MODE_POLL, T_ADDR), 0U, 0U, true, I2C_BM_OK, &bad, r) ||
        !run1("T4", m4, f, IST_XF(IST_OP_PROBE, IST_MODE_POLL, T_NOADDR), 0U, 0U, false, I2C_BM_ERR_NACK, &bad, r))
    {
        return;
    }
    for (uint32_t i = 0U; i < T4_SIZES; i++)
    {
        if (!run1("T4", m4, f, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), k_t4_sizes[i], next_seed(), true,
                  I2C_BM_OK, &bad, r))
        {
            return;
        }
    }
    verdict(bad == 0U, "T4 %s at %lu Hz (TIMINGR 0x%08lX): %lu reads, %lu write-reads, 2 probes, %lu writes of 1..65535 "
            "bytes: %lu with another status or a byte that differs from the target's content", core, (unsigned long)f,
            (unsigned long)timingr, (unsigned long)T4_SIZES, (unsigned long)T4_SIZES, (unsigned long)T4_SIZES,
            (unsigned long)bad);
    /* The control: the target inverts the 6th byte it returns; the check sees it. */
    if (!host_at("F334 INIT") || !host_at("F334 CORRUPT 5"))
    {
        return;
    }
    uint32_t cbad = 0U;
    if (!run1("T4C", m4, f, IST_XF(IST_OP_READ, IST_MODE_POLL, T_ADDR), 16U, 0U, true, I2C_BM_OK, &cbad, r))
    {
        return;
    }
    verdict((r[IST_XR_STATUS] == (uint32_t)I2C_BM_OK) && (r[IST_XR_MISM] == 1U) && (r[IST_XR_FIRST_BAD] == 5U),
            "T4 %s control at %lu Hz: target armed CORRUPT 5, read of 16: %lu byte differs, at index %ld (5)", core,
            (unsigned long)f, (unsigned long)r[IST_XR_MISM], (long)(int32_t)r[IST_XR_FIRST_BAD]);
}

/* T5 (taps): short transfers, each one captured by the scope; the host decodes it
   and judges the bytes, the conditions and the times against TIMINGR. */
static void t5_speed(bool m4, uint32_t f, uint32_t timingr, uint32_t k, uint32_t model)
{
    uint32_t r[IST_XR_N];
    uint32_t bad = 0U;
    if (!host_at("F334 INIT"))
    {
        return;
    }
    /* The reads first, while the target holds its known content and pointer 0; the
       writes (random bytes from a seed, the first one the pointer) after. */
    struct { uint32_t op; uint32_t addr; uint32_t n; uint32_t arg; bool target; I2cBm_Status_t st; bool probe_first; } c[5] = {
        { IST_OP_READ, T_ADDR, 2U, 0U, true, I2C_BM_OK, false },
        { IST_OP_WRITEREAD, T_ADDR, 1U, 0x07U, true, I2C_BM_OK, false },
        { IST_OP_WRITE, T_ADDR, 1U, 0x00000020UL, true, I2C_BM_OK, true },     /* probe + 1 byte: tBUF  */
        { IST_OP_WRITE, T_ADDR, 2U, 0x0000C310UL, true, I2C_BM_OK, false },
        { IST_OP_PROBE, T_NOADDR, 0U, 0U, false, I2C_BM_ERR_NACK, false },    /* NACK and STOP (T8(a)) */
    };
    for (uint32_t i = 0U; i < 5U; i++)
    {
        if (!host_at("SCOPE T5%s k=%lu f=%lu kernel=%lu timingr=%08lX model=%lu rate=%lu trig=sda af=%u dnf=%u",
                     tier_sfx(), (unsigned long)i, (unsigned long)f, (unsigned long)k, (unsigned long)timingr,
                     (unsigned long)model, (unsigned long)((f <= 100000U) ? 12500000U : 25000000U),
                     (unsigned)BENCH_ANALOG_FILTER, (unsigned)BENCH_DNF))
        {
            return;
        }
        /* The probe and the write back to back, both in one capture: their X lines after. */
        uint32_t rp[IST_XR_N];
        if (c[i].probe_first &&
            !xfer(m4, IST_XF(IST_OP_PROBE, IST_MODE_POLL, T_ADDR), 0U, 0U, budget(core_hz(m4), f, 1U), rp))
        {
            return;
        }
        if (!xfer(m4, IST_XF(c[i].op, IST_MODE_POLL, c[i].addr), c[i].n, c[i].arg, budget(core_hz(m4), f, 4U), r))
        {
            return;
        }
        if (c[i].probe_first)
        {
            xline("T5", m4, f, IST_XF(IST_OP_PROBE, IST_MODE_POLL, T_ADDR), 0U, 0U, rp, true);
            bad += (rp[IST_XR_STATUS] != (uint32_t)I2C_BM_OK) ? 1U : 0U;
        }
        xline("T5", m4, f, IST_XF(c[i].op, IST_MODE_POLL, c[i].addr), c[i].n, c[i].arg, r, c[i].target);
        bad += ((r[IST_XR_STATUS] != (uint32_t)c[i].st) ||
                (((c[i].op == IST_OP_READ) || (c[i].op == IST_OP_WRITEREAD)) && (r[IST_XR_MISM] != 0U))) ? 1U : 0U;
        if (!pace())
        {
            return;
        }
    }
    if (!host_at("SCOPE end"))
    {
        return;
    }
    verdict(bad == 0U, "T5 %s at %lu Hz: 5 captured transfers (probe + write 1, write 2, read 2, write-read 1+1, probe "
            "0x43) ended as expected on the H755 side: %lu did not (the host judges the bus)", m4 ? "CM4" : "CM7",
            (unsigned long)f, (unsigned long)bad);
}

/* The bench speeds of this session, one entry each: TIMINGR and its model. */
static uint32_t speeds(uint32_t f_out[TAPS_SPEEDS])
{
    if (s_taps)
    {
        f_out[0] = 100000U;
        f_out[1] = 400000U;
        return 2U;
    }
    f_out[0] = 400000U;                                   /* also without taps: O1 (taps/notaps) */
    f_out[1] = 1000000U;                                  /* S19, D3: only without taps */
    return 2U;
}

/* Fm+ (SYSCFG_PMCR.I2CxFMP) of the instance under test, and the target's FMP. */
static bool fmp(bool m4, bool on)
{
    I2cBm_Status_t st;
    if (m4)
    {
        if (!cm4_send4(IST_CMD_M4_FMP, on ? 1U : 0U, 0U, 0U, 0U))
        {
            return false;
        }
        st = (I2cBm_Status_t)IST_AREA->m4_res[0];
    }
    else
    {
        st = I2C_BM_SetFastModePlus(I2C4, on);
    }
    LOG("Fm+ %s %s: SetFastModePlus %u, SYSCFG_PMCR 0x%08lX", m4 ? "I2C1" : "I2C4", on ? "on" : "off", (unsigned)st,
        (unsigned long)SYSCFG->PMCR);
    return (st == I2C_BM_OK) && host_at("F334 FMP %u", on ? 1U : 0U);
}

/* At the end of every bus step: the Fm+ bits must be 0 again, put back by the
   step itself; if not they are cleared here and the step fails (audit F1, m20).
   Every abort clears them too (bench_safe()). */
static void fmp_judge(const char *step)
{
    const uint32_t left = syscfg_clocked() ? (SYSCFG->PMCR & FMP_BENCH_BITS) : 0U;
    fmp_off_raw();
    const uint32_t now = syscfg_clocked() ? (SYSCFG->PMCR & FMP_BENCH_BITS) : 0U;
    verdict(syscfg_clocked() && (left == 0U) && (now == 0U),
            "Fm+ after %s: SYSCFG_PMCR I2C1FMP|I2C4FMP 0x%lX at the end of the step (0: put back by the step), 0x%lX "
            "after the check", step, (unsigned long)left, (unsigned long)now);
}

/* T4 and T5 at every speed of the session. */
static void t4_t5(bool m4)
{
    uint32_t fl[TAPS_SPEEDS];
    const uint32_t nf = speeds(fl);
    const uint32_t k = m4 ? s_k123 : s_k4;
    for (uint32_t i = 0U; (i < nf) && !s_bench_abort; i++)
    {
        I2cBm_TimingOut_t o = { 0U, 0U, 0U };
        const I2cBm_Status_t sc = bench_timing(k, fl[i], &o);
        uint32_t img[IST_NREG];
        const bool fm = fl[i] > 400000U;
        if (fm && !fmp(m4, true))
        {
            bench_abort("Fm+");
            return;
        }
        const I2cBm_Status_t si = (sc == I2C_BM_OK) ? inst_init(m4, o.timingr, 0U, img) : sc;
        LOG("TIMING %s f=%lu kernel=%lu timingr=%08lX model=%lu calc=%u init=%u", m4 ? "CM4" : "CM7",
            (unsigned long)fl[i], (unsigned long)k, (unsigned long)o.timingr, (unsigned long)o.scl_hz_model,
            (unsigned)sc, (unsigned)si);
        if (si != I2C_BM_OK)
        {
            verdict(false, "Init of %s at %lu Hz: %u", m4 ? "I2C1" : "I2C4", (unsigned long)fl[i], (unsigned)si);
            bench_abort("Init");
            return;
        }
        t4_speed(m4, fl[i], o.timingr);
        if (s_taps)
        {
            t5_speed(m4, fl[i], o.timingr, k, o.scl_hz_model);
        }
        uint32_t diff = 0U;
        const bool d_ok = inst_deinit(m4, &diff);
        verdict(d_ok, "T14 %s after T4/T5 at %lu Hz: Deinit, registers that differ from the reset image 0x%02lX",
                m4 ? "I2C1" : "I2C4", (unsigned long)fl[i], (unsigned long)diff);
        if (fm && !fmp(m4, false))
        {
            bench_abort("Fm+ off");
            return;
        }
    }
}

/* The main speed's TIMINGR for this core's instance, and Init. */
static bool main_init(bool m4, uint32_t tmo, uint32_t *timingr)
{
    I2cBm_TimingOut_t o = { 0U, 0U, 0U };
    const I2cBm_Status_t sc = bench_timing(m4 ? s_k123 : s_k4, s_f_main, &o);
    uint32_t img[IST_NREG];
    const I2cBm_Status_t si = (sc == I2C_BM_OK) ? inst_init(m4, o.timingr, tmo, img) : sc;
    *timingr = o.timingr;
    if (si != I2C_BM_OK)
    {
        verdict(false, "Init of %s at %lu Hz, timeout %lu: %u", m4 ? "I2C1" : "I2C4", (unsigned long)s_f_main,
                (unsigned long)tmo, (unsigned)si);
        bench_abort("Init");
        return false;
    }
    return true;
}

/* T6: by interrupt; the control clears the enables by hand. */
static void t6(bool m4)
{
    static const uint32_t ns[4] = { 1U, 2U, 256U, 1000U };
    static const uint32_t ops[3] = { IST_OP_WRITE, IST_OP_READ, IST_OP_WRITEREAD };
    uint32_t r[IST_XR_N];
    uint32_t bad = 0U;
    uint32_t irq_left = 0U;
    uint32_t ie_left = 0U;
    const uint32_t f = s_f_main;
    if (!host_at("F334 INIT"))
    {
        return;
    }
    uint32_t p = 0U;
    for (uint32_t o = 0U; o < 3U; o++)
    {
        for (uint32_t i = 0U; i < 4U; i++)
        {
            const uint32_t arg = (ops[o] == IST_OP_WRITE) ? next_seed() : ((ops[o] == IST_OP_READ) ? p : 0x40U);
            if (!run1("T6", m4, f, IST_XF(ops[o], IST_MODE_IT, T_ADDR), ns[i], arg, true, I2C_BM_OK, &bad, r))
            {
                return;
            }
            irq_left += r[IST_XR_IRQS_AFTER];
            ie_left |= r[IST_XR_CR1_AFTER] & 0xFEU;     /* every interrupt enable of CR1 */
            if (ops[o] == IST_OP_READ)
            {
                p = (p + ns[i]) & 0xFFU;
            }
            if (ops[o] == IST_OP_WRITE)
            {
                /* writes change the target's registers: back to the known content */
                if (!host_at("F334 INIT"))
                {
                    return;
                }
                p = 0U;
            }
        }
    }
    verdict((bad == 0U) && (irq_left == 0U) && (ie_left == 0U),
            "T6 %s by interrupt at %lu Hz: write, read, write-read of 1, 2, 256, 1000 bytes: %lu with another status or "
            "data; interrupt enables left in CR1 0x%02lX (0); IRQs in the 10 ms after the end %lu (0)",
            m4 ? "CM4" : "CM7", (unsigned long)f, (unsigned long)bad, (unsigned long)ie_left, (unsigned long)irq_left);
    /* The control: the enables cleared before the first event: no progress, the wait
       ends by its budget; the instance is reset (the master held SCL low), and the
       target cuts its transfer after 500 ms without progress. */
    const uint32_t max = ms_cyc(5U) / (m4 ? 2U : 1U);
    if (!xfer(m4, IST_XF(IST_OP_WRITE, IST_MODE_IT_CTRL, T_ADDR), 2U, next_seed(), max, r))
    {
        return;
    }
    xline("T6C", m4, f, IST_XF(IST_OP_WRITE, IST_MODE_IT_CTRL, T_ADDR), 2U, 0U, r, true);
    uint32_t img[IST_NREG];
    uint32_t tr = 0U;
    deinit_judged(m4, "the T6 control");
    (void)delay_ms(600U);
    const bool re = main_init(m4, 0U, &tr);
    (void)img;
    verdict((r[IST_XR_STATUS] == (uint32_t)I2C_BM_ERR_TIMEOUT) && (r[IST_XR_AUX] == (uint32_t)I2C_BM_PENDING) &&
            (r[IST_XR_IRQS] == 0U) && re,
            "T6 %s control: enables cleared by hand after StartIt: status still PENDING (%lu), the wait ended by its "
            "budget (%lu = ERR_TIMEOUT), %lu IRQs; Deinit, 600 ms (the target's cut), Init again %u", m4 ? "CM4" : "CM7",
            (unsigned long)r[IST_XR_AUX], (unsigned long)r[IST_XR_STATUS], (unsigned long)r[IST_XR_IRQS], (unsigned)re);
}

/* T7: by DMA (CPU2: DMA2 streams through dma_bm; CPU1: BDMA through dma_hal, SRAM4). */
static void t7(bool m4)
{
    static const uint32_t ns[4] = { 2U, 255U, 256U, 4096U };
    uint32_t r[IST_XR_N];
    uint32_t bad = 0U;
    uint32_t aux = 0U;
    const uint32_t f = s_f_main;
    if (!host_at("F334 INIT"))
    {
        return;
    }
    uint32_t p = 0U;
    for (uint32_t i = 0U; i < 4U; i++)
    {
        if (!run1("T7", m4, f, IST_XF(IST_OP_READ, IST_MODE_DMA, T_ADDR), ns[i], p, true, I2C_BM_OK, &bad, r))
        {
            return;
        }
        aux |= r[IST_XR_AUX];
        p = (p + ns[i]) & 0xFFU;
    }
    for (uint32_t i = 0U; i < 4U; i++)
    {
        if (!run1("T7", m4, f, IST_XF(IST_OP_WRITE, IST_MODE_DMA, T_ADDR), ns[i], next_seed(), true, I2C_BM_OK, &bad, r))
        {
            return;
        }
        aux |= r[IST_XR_AUX];
    }
    verdict((bad == 0U) && (aux == 0U), "T7%s %s by %s at %lu Hz: read and write of 2, 255, 256, 4096 bytes: %lu with "
            "another status or data; DMA steps not OK 0x%lX", m4 ? "(a)" : "(b)", m4 ? "CM4 I2C1" : "CM7 I2C4",
            m4 ? "DMA2 (dma_bm)" : "BDMA (dma_hal, .d3_ram)", (unsigned long)f, (unsigned long)bad, (unsigned long)aux);
    /* The control: RXDMAEN cleared by hand: nobody reads RXDR, the buffer keeps its
       sentinel and WaitEnd ends by its budget (software reset). */
    if (!host_at("F334 INIT"))
    {
        return;
    }
    const uint32_t max = ms_cyc(5U) / (m4 ? 2U : 1U);
    if (!xfer(m4, IST_XF(IST_OP_READ, IST_MODE_DMA_CTRL, T_ADDR), 64U, 0U, max, r))
    {
        return;
    }
    xline("T7C", m4, f, IST_XF(IST_OP_READ, IST_MODE_DMA_CTRL, T_ADDR), 64U, 0U, r, true);
    (void)delay_ms(600U);                                 /* the target's 500 ms cut */
    verdict((r[IST_XR_STATUS] == (uint32_t)I2C_BM_ERR_TIMEOUT) && (r[IST_XR_SENTINEL] == 64U),
            "T7%s control: RXDMAEN cleared by hand, read of 64: WaitEnd %lu (ERR_TIMEOUT %u), bytes still holding the "
            "sentinel %lu (64)", m4 ? "(a)" : "(b)", (unsigned long)r[IST_XR_STATUS], (unsigned)I2C_BM_ERR_TIMEOUT,
            (unsigned long)r[IST_XR_SENTINEL]);
}

static bool wait_pin(uint32_t pin, uint32_t level, uint32_t ms)
{
    const uint32_t t0 = DWT->CYCCNT;
    uint32_t last = t0;
    uint32_t same = 0U;
    while (((GPIOB->IDR >> pin) & 1UL) != level)
    {
        const uint32_t now = DWT->CYCCNT;
        same = (now == last) ? (same + 1U) : 0U;
        last = now;
        if ((same > STALL_READS) || ((now - t0) >= ms_cyc(ms)))
        {
            return false;
        }
    }
    return true;
}

/* T8: (a) no target at 0x43; (b) the target NACKs data byte 3 of a write of 8. */
static void t8(bool m4)
{
    uint32_t r[IST_XR_N];
    uint32_t bad = 0U;
    const uint32_t f = s_f_main;
    if (!run1("T8A", m4, f, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_NOADDR), 1U, 0x0000005AUL, false, I2C_BM_ERR_NACK,
              &bad, r))
    {
        return;
    }
    const uint32_t busy_a = r[IST_XR_ISR_AFTER] & I2C_ISR_BUSY;
    if (!run1("T8A", m4, f, IST_XF(IST_OP_PROBE, IST_MODE_IT, T_NOADDR), 0U, 0U, false, I2C_BM_ERR_NACK, &bad, r))
    {
        return;
    }
    const uint32_t busy_b = r[IST_XR_ISR_AFTER] & I2C_ISR_BUSY;
    if (!host_at("F334 INIT"))
    {
        return;
    }
    if (!run1("T8A", m4, f, IST_XF(IST_OP_READ, IST_MODE_POLL, T_ADDR), 4U, 0U, true, I2C_BM_OK, &bad, r))
    {
        return;
    }
    verdict((bad == 0U) && (busy_a == 0U) && (busy_b == 0U),
            "T8(a) %s address 0x43 (no target): Write and IT probe ERR_NACK, BUSY after %lu %lu (0: the STOP was sent); "
            "the next transfer to 0x42 OK: %lu failed", m4 ? "CM4" : "CM7", (unsigned long)(busy_a != 0U),
            (unsigned long)(busy_b != 0U), (unsigned long)bad);
    /* (b), with the scope (taps) on the NACKed write. */
    uint32_t bb = 0U;
    if (!host_at("F334 NACK 3"))
    {
        return;
    }
    if (s_taps && !host_at("SCOPE T8 k=0 f=%lu kernel=%lu timingr=0 model=0 rate=%lu trig=sda", (unsigned long)f,
                           (unsigned long)(m4 ? s_k123 : s_k4), (unsigned long)((f <= 100000U) ? 20000000U : 50000000U)))
    {
        return;
    }
    if (!run1("T8B", m4, f, IST_XF(IST_OP_WRITE, IST_MODE_IT, T_ADDR), 8U, 0x0000A55AUL, true, I2C_BM_ERR_NACK, &bb, r))
    {
        return;
    }
    const uint32_t dtx = r[IST_XR_DONE_TX];
    if (s_taps && !host_at("SCOPE end"))
    {
        return;
    }
    if (!host_at("F334 NACK 3"))
    {
        return;
    }
    if (!run1("T8B", m4, f, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 8U, 0x0000A55AUL, true, I2C_BM_ERR_NACK, &bb, r))
    {
        return;
    }
    /* The control: the same write with nothing armed ends well. */
    if (!run1("T8B", m4, f, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 8U, 0x0000A55AUL, true, I2C_BM_OK, &bb, r))
    {
        return;
    }
    verdict((bb == 0U) && (dtx == 3U),
            "T8(b) %s the target NACKs data byte 3 of a write of 8: IT and polling ERR_NACK, bytes acknowledged by the "
            "driver's count %lu (3); control without NACK armed OK: %lu failed", m4 ? "CM4" : "CM7",
            (unsigned long)dtx, (unsigned long)bb);
}

/* T10: (a) the target stretches SCL 200 us at the address and after every byte; (b) SCL
   held 50 ms, with the SCL-low timeout of ~10 ms, and without it (the control). */
static void t10(bool m4)
{
    uint32_t r[IST_XR_N];
    uint32_t bad = 0U;
    const uint32_t f = s_f_main;
    uint32_t tr = 0U;
    if (!host_at("F334 INIT"))
    {
        return;
    }
    if (!run1("T10A0", m4, f, IST_XF(IST_OP_READ, IST_MODE_POLL, T_ADDR), 4U, 0U, true, I2C_BM_OK, &bad, r))
    {
        return;
    }
    const uint32_t c_r0 = r[IST_XR_CYCLES];
    if (!run1("T10A0", m4, f, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 4U, 0x00004010UL, true, I2C_BM_OK, &bad, r))
    {
        return;
    }
    const uint32_t c_w0 = r[IST_XR_CYCLES];
    if (!host_at("F334 INIT") || !host_at("F334 STRETCH ADDR 200") || !host_at("F334 STRETCH BYTE 200"))
    {
        return;
    }
    if (s_taps && !host_at("SCOPE T10 k=0 f=%lu kernel=0 timingr=0 model=0 rate=4000000 trig=sda", (unsigned long)f))
    {
        return;
    }
    if (!run1("T10A", m4, f, IST_XF(IST_OP_READ, IST_MODE_POLL, T_ADDR), 4U, 0U, true, I2C_BM_OK, &bad, r))
    {
        return;
    }
    const uint32_t c_r1 = r[IST_XR_CYCLES];
    if (s_taps && !host_at("SCOPE end"))
    {
        return;
    }
    if (!host_at("F334 STRETCH ADDR 200") || !host_at("F334 STRETCH BYTE 200"))
    {
        return;
    }
    if (s_taps && !host_at("SCOPE T10 k=1 f=%lu kernel=0 timingr=0 model=0 rate=4000000 trig=sda", (unsigned long)f))
    {
        return;
    }
    if (!run1("T10A", m4, f, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 4U, 0x00004010UL, true, I2C_BM_OK, &bad, r))
    {
        return;
    }
    const uint32_t c_w1 = r[IST_XR_CYCLES];
    if (s_taps && !host_at("SCOPE end"))
    {
        return;
    }
    /* At least 200 us at the address and after each of the 4 data bytes (5 x 200 us):
       the write's and the read's own stretches (byte control after each byte). */
    const uint32_t min_extra = (5U * 200U * core_hz(m4)) / 1000000U;
    verdict((bad == 0U) && (c_w1 >= (c_w0 + min_extra)) && (c_r1 >= (c_r0 + min_extra)),
            "T10(a) %s target stretching 200 us at the address and per byte: write and read of 4 OK with the data; "
            "CYCCNT write %lu -> %lu, read %lu -> %lu cycles (each >= +%lu: 5 x 200 us)", m4 ? "CM4" : "CM7",
            (unsigned long)c_w0, (unsigned long)c_w1, (unsigned long)c_r0, (unsigned long)c_r1,
            (unsigned long)min_extra);
    /* (b): TIMEOUTA for ~10 ms at the kernel clock: (TIMEOUTA + 1) x 2048 / kernel. */
    const uint32_t k = m4 ? s_k123 : s_k4;
    const uint32_t tmo = (uint32_t)(((uint64_t)k / 100U + 2047U) / 2048U);   /* 10 ms */
    deinit_judged(m4, "T10(a)");
    if (!main_init(m4, tmo, &tr) || !host_at("RPT lenient"))     /* the target is off while it holds SCL */
    {
        return;
    }
    /* With SCL held low the TIMEOUT fires again every TIMEOUTA (silicon fact, audit F1
       part 3, O22): the Write ends at the first multiple of TIMEOUTA, counted from the
       SCL fall, after its own START, n = ceil((start - fall) / TIMEOUTA). The host
       orders the hold after the handshake (B2) and CPU1 times the fall itself, waiting
       for it (B1): CPU2's Write starts as soon as CPU1 has seen it (n = 1); CPU1's
       starts on purpose 1.5 x TIMEOUTA after it, so that its first expiry ends nothing
       and the second ends the Write (n = 2). Neither depends on the host's latency. */
    const uint32_t nwant = m4 ? 1U : 2U;                  /* the n each START is placed for (B2) */
    if (s_taps && !host_at("SCOPE T10B k=0 f=%lu kernel=%lu timingr=0 model=0 rate=100000 trig=scl tmo=%lu nwant=%lu",
                           (unsigned long)f, (unsigned long)k, (unsigned long)tmo, (unsigned long)nwant))
    {
        return;
    }
    const uint32_t tmo_cyc = (uint32_t)(((uint64_t)tmo * 2048U * s_cpu_hz) / k);   /* TIMEOUTA in CPU1 cycles */
    if (!host_at("HOLD SCL 50"))
    {
        return;
    }
    const bool low_b = wait_pin(IST_PIN_SCL, 0U, HOLD_WAIT_MS);
    const uint32_t fall_b = DWT->CYCCNT;
    if (low_b && !m4)
    {
        const uint32_t until = (tmo_cyc / 2U) * 3U;       /* 1.5 x TIMEOUTA after the fall */
        uint32_t last = DWT->CYCCNT;
        uint32_t same = 0U;
        while ((DWT->CYCCNT - fall_b) < until)
        {
            const uint32_t now = DWT->CYCCNT;
            same = (now == last) ? (same + 1U) : 0U;
            last = now;
            if (same > STALL_READS)
            {
                s_stalls++;
                break;
            }
        }
    }
    const uint32_t start_b = DWT->CYCCNT;
    if (!xfer(m4, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 2U, 0x00002010UL, ms_cyc(100U) / (m4 ? 2U : 1U), r))
    {
        return;
    }
    const uint32_t end_b = DWT->CYCCNT;                     /* before the X line (UART) */
    xline("T10B", m4, f, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 2U, 0x00002010UL, r, true);
    const uint32_t st_b = r[IST_XR_STATUS];
    (void)wait_pin(IST_PIN_SCL, 1U, 100U);
    if (!host_at("HOLD_END"))
    {
        return;
    }
    if (s_taps && !host_at("SCOPE end"))
    {
        return;
    }
    deinit_judged(m4, "T10(b)");
    if (!main_init(m4, 0U, &tr) || !host_at("HOLD SCL 50"))
    {
        return;
    }
    const bool low_c = wait_pin(IST_PIN_SCL, 0U, HOLD_WAIT_MS);   /* the 30 ms are counted from the fall (B2) */
    const uint32_t seen_c = DWT->CYCCNT;
    if (!xfer(m4, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 2U, 0x00002010UL, ms_cyc(30U) / (m4 ? 2U : 1U), r))
    {
        return;
    }
    const uint32_t cy_c = DWT->CYCCNT - seen_c;             /* before the X line (UART) */
    xline("T10BC", m4, f, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 2U, 0x00002010UL, r, true);
    const uint32_t st_c = r[IST_XR_STATUS];
    (void)wait_pin(IST_PIN_SCL, 1U, 100U);
    if (!host_at("HOLD_END"))
    {
        return;
    }
    (void)delay_ms(600U);                                 /* any transfer the target began is cut */
    if (!host_at("RPT strict"))
    {
        return;
    }
    const uint32_t cpu_us = s_cpu_hz / 1000000U;
    const uint32_t us_start = (start_b - fall_b) / cpu_us;  /* CPU1's CYCCNT, from the SCL fall */
    const uint32_t us_end = (end_b - fall_b) / cpu_us;
    const uint32_t us_tmo = tmo_cyc / cpu_us;
    const uint32_t us_tick = (uint32_t)((2048ULL * 1000000U) / k);             /* a first tick cut short */
    const uint32_t n_b = ((start_b - fall_b) / tmo_cyc) + 1U;                   /* first multiple after START */
    const uint32_t lo = (n_b * us_tmo) - us_tick;
    const uint32_t hi = (n_b * us_tmo) + 1000U;
    verdict(low_b && (st_b == (uint32_t)I2C_BM_ERR_BUS) && (n_b == nwant) && (us_end >= lo) && (us_end <= hi),
            "T10(b) %s SCL held 50 ms by the target, timeout_scl_low %lu (TIMEOUTA %lu us at %lu Hz): from the SCL "
            "fall (seen %u, waited for after the handshake), Write started at %lu us, so n = %lu (%lu wanted: %s); it "
            "ended %lu us after the fall with %lu (ERR_BUS %u, by TIMEOUT, then the software reset), n x TIMEOUTA "
            "%lu..%lu", m4 ? "CM4" : "CM7", (unsigned long)tmo, (unsigned long)us_tmo, (unsigned long)k,
            (unsigned)low_b, (unsigned long)us_start, (unsigned long)n_b, (unsigned long)nwant,
            m4 ? "started at once" : "started after 1.5 x TIMEOUTA, the first expiry ends nothing",
            (unsigned long)us_end, (unsigned long)st_b, (unsigned)I2C_BM_ERR_BUS, (unsigned long)lo, (unsigned long)hi);
    const uint32_t ms_c = cy_c / (s_cpu_hz / 1000U);
    verdict(low_c && (st_c == (uint32_t)I2C_BM_ERR_TIMEOUT) && (ms_c >= 29U) && (ms_c <= 33U),
            "T10(b) %s control without timeout_scl_low: Write %lu (ERR_TIMEOUT %u) %lu ms after SCL went low "
            "(max_cycles 30 ms)",
            m4 ? "CM4" : "CM7", (unsigned long)st_c, (unsigned)I2C_BM_ERR_TIMEOUT, (unsigned long)ms_c);
    deinit_judged(m4, "the T10(b) control");
    (void)main_init(m4, 0U, &tr);
}

/* F3: T1 with BUSY = 1 through i2c_hal on I2C4 (initialised, its handle READY): the
   transfers of ist_t1_busy() with the HAL tier's calls, each refused by i2c_bm's check
   (ERR_BUS) before the HAL. The DMA handles are linked only so that the refusal is
   i2c_bm's; no stream is started. */
static void hal_t1_busy(IstT1Result_t *r)
{
    uint32_t a[IST_T1_PHOTO];
    uint32_t b[IST_T1_PHOTO];
    uint8_t buf[4] = { 0U, 1U, 2U, 3U };
    I2C_HandleTypeDef *h = &s_hi2c4;
    r->cases = 0U;
    r->bad = 0U;
    r->groups_same = 0U;
    r->isr = I2C4->ISR;
    h->hdmatx = &s_hbd_tx;
    h->hdmarx = &s_hbd_rx;
    ist_t1_photo(I2C4, a);
    ist_t1_case(r, I2C_BM_CheckTransfer(I2C4, T_ADDR, 1U, 0U), I2C_BM_ERR_BUS);
    ist_t1_case(r, I2C_HAL_Write(h, T_ADDR, buf, 1U, 100U), I2C_BM_ERR_BUS);
    ist_t1_case(r, I2C_HAL_Write(h, T_ADDR, NULL, 0U, 100U), I2C_BM_ERR_BUS);
    ist_t1_case(r, I2C_HAL_Read(h, T_ADDR, buf, 2U, 100U), I2C_BM_ERR_BUS);
    ist_t1_case(r, I2C_HAL_WriteRead(h, T_ADDR, buf, 1U, buf, 2U, 100U), I2C_BM_ERR_BUS);
    ist_t1_case(r, I2C_HAL_StartIt(h, T_ADDR, buf, 2U, NULL, 0U), I2C_BM_ERR_BUS);
    ist_t1_case(r, I2C_HAL_StartDma(h, T_ADDR, buf, NULL, 2U), I2C_BM_ERR_BUS);
    ist_t1_case(r, I2C_HAL_StartDma(h, T_ADDR, NULL, buf, 2U), I2C_BM_ERR_BUS);
    ist_t1_photo(I2C4, b);
    h->hdmatx = NULL;
    h->hdmarx = NULL;
    r->groups_same = (ist_t1_photo_eq(a, b) && (h->State == HAL_I2C_STATE_READY)) ? 1U : 0U;
}

/* T1 with BUSY = 1: the target holds SDA low (a START on the bus); every transfer is
   refused with ERR_BUS and nothing is written. */
static void t1_busy(bool m4)
{
    if (!host_at("HOLD SDA 100"))
    {
        return;
    }
    const bool low = wait_pin(IST_PIN_SDA, 0U, HOLD_WAIT_MS);     /* the hold starts after the handshake (B2) */
    IstT1Result_t t;
    uint32_t cases;
    uint32_t bad;
    uint32_t same;
    uint32_t isr;
    if (m4)
    {
        if (!cm4_send4(IST_CMD_M4_BUSY, s_ll ? 1U : 0U, 0U, 0U, 0U))
        {
            return;
        }
        cases = IST_AREA->m4_res[0];
        bad = IST_AREA->m4_res[1];
        same = IST_AREA->m4_res[2];
        isr = IST_AREA->m4_res[3];
    }
    else
    {
        if (s_hal)
        {
            hal_t1_busy(&t);
        }
        else
        {
            ist_t1_busy(I2C4, s_ll, &t);
        }
        t1_print("CM7 BUSY", &t);
        cases = t.cases;
        bad = t.bad;
        same = t.groups_same;
        isr = t.isr;
    }
    (void)wait_pin(IST_PIN_SDA, 1U, 150U);
    if (!host_at("HOLD_END"))
    {
        return;
    }
    verdict(low && ((isr & I2C_ISR_BUSY) != 0U) && (bad == 0U) && (same == 1U) && (cases == IST_T1_BUSY_CASES),
            "T1 %s with BUSY = 1 (target holding SDA; ISR 0x%08lX): %lu transfers refused (8), %lu with another status than "
            "ERR_BUS; registers and RCC unchanged %lu", m4 ? "CM4 I2C1" : "CM7 I2C4", (unsigned long)isr,
            (unsigned long)cases, (unsigned long)bad, (unsigned long)same);
}

/* B1: cycles of Init, of a Write of 1 byte, of XferIrq (min/max). */
static void b1(bool m4, uint32_t timingr)
{
    uint32_t ci;
    uint32_t cw;
    uint32_t qmin;
    uint32_t qmax;
    uint32_t sw;
    if (m4)
    {
        if (!cm4_send4(IST_CMD_M4_B1, timingr, 0U, 0U, 0U))
        {
            return;
        }
        ci = IST_AREA->m4_res[0];
        cw = IST_AREA->m4_res[1];
        qmin = IST_AREA->m4_res[2];
        qmax = IST_AREA->m4_res[3];
        sw = IST_AREA->m4_res[4];
    }
    else
    {
        const I2cBm_Config_t c = { I2C4, timingr, true, 0U, 0U };
        uint8_t one = 0x80U;
        (void)I2C_BM_Deinit(I2C4);
        uint32_t t0 = DWT->CYCCNT;
        (void)I2C_BM_Init(&c);
        ci = DWT->CYCCNT - t0;
        t0 = DWT->CYCCNT;
        sw = (uint32_t)I2C_BM_Write(I2C4, T_ADDR, &one, 1U, ms_cyc(10U));
        cw = DWT->CYCCNT - t0;
        s_irq_min = 0xFFFFFFFFUL;
        s_irq_max = 0U;
        uint32_t r[IST_XR_N];
        cm7_xfer(IST_XF(IST_OP_WRITE, IST_MODE_IT, T_ADDR), 16U, 0x1234567UL, ms_cyc(10U), r);
        qmin = s_irq_min;
        qmax = s_irq_max;
    }
    LOG("B1 %s at %lu Hz: I2C_BM_Init %lu cycles, I2C_BM_Write of 1 byte %lu cycles (status %lu), I2C_BM_XferIrq "
        "%lu..%lu cycles per call (IT write of 16)", m4 ? "CM4 (200 MHz)" : "CM7 (400 MHz)", (unsigned long)s_f_main,
        (unsigned long)ci, (unsigned long)cw, (unsigned long)sw, (unsigned long)qmin, (unsigned long)qmax);
    (void)pace();
}

/* ---- T12 (ES0445 2.19.6) on CPU2's I2C1, the kernel clock on the HSI ------------------- */
#define T12_COUNT   1000U

static bool rcc_cr_set(uint32_t bit, bool on)
{
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    RCC->CR = on ? (RCC->CR | bit) : (RCC->CR & ~bit);
    const uint32_t rb = RCC->CR;
    __set_PRIMASK(pm);
    return ((rb & bit) != 0U) == on;
}

static bool hsi_ready(void)
{
    return (RCC->CR & RCC_CR_HSIRDY) != 0U;
}

/* I2C123SEL [13:12] of RCC_D2CCIP2R (09:4744-4749): 00 pclk1, 10 hsi_ker_ck. Only
   in T12, read back (plan C10, instructions 3.5). */
static bool i2c123sel_set(uint32_t field)
{
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    RCC->D2CCIP2R = (RCC->D2CCIP2R & ~RCC_D2CCIP2R_I2C123SEL_Msk) | (field << RCC_D2CCIP2R_I2C123SEL_Pos);
    const uint32_t rb = RCC->D2CCIP2R;
    __set_PRIMASK(pm);
    return ((rb & RCC_D2CCIP2R_I2C123SEL_Msk) >> RCC_D2CCIP2R_I2C123SEL_Pos) == field;
}

static void t12(void)
{
    static const char *const k_name[4] = { "poll, driver (first byte preloaded)", "DMA, driver (preloaded)",
                                           "poll, by hand without preload", "DMA, by hand without preload" };
    const uint32_t cr_was = RCC->CR & RCC_CR_HSION;
    const uint32_t d2_was = RCC->D2CCIP2R;
    const uint32_t hsidiv = (RCC->CR & RCC_CR_HSIDIV_Msk) >> RCC_CR_HSIDIV_Pos;
    deinit_judged(true, "the tests before T12 (I2C1 off the PCLK kernel)");
    /* HSION written only if it was 0 (plan R3-4, audit F1 m19). */
    const bool hsi_wrote = (cr_was == 0U);
    bool ok = (!hsi_wrote || rcc_cr_set(RCC_CR_HSION, true)) && (wait_until(hsi_ready, 5U) == WAIT_DONE) &&
              (hsidiv == 0U);
    ok = ok && i2c123sel_set(2U);
    const uint32_t k = 64000000U;                        /* hsi_ker_ck, HSIDIV = /1 (09:3581-3586) */
    I2cBm_TimingOut_t o = { 0U, 0U, 0U };
    uint32_t img[IST_NREG];
    ok = ok && (bench_timing(k, s_f_main, &o) == I2C_BM_OK) && (inst_init(true, o.timingr, 0U, img) == I2C_BM_OK);
    log_long("T12 kernel: RCC_CR HSION was %lu (written only if 0), HSIDIV %lu (0: 64 MHz), HSIRDY %lu; RCC_D2CCIP2R 0x%08lX -> 0x%08lX "
             "(I2C123SEL = 10, HSI); APB1 %lu Hz / kernel %lu Hz = %lu.%02lu (ES0445 2.19.6 window 1.5..3); I2C1 "
             "TIMINGR 0x%08lX at %lu Hz", (unsigned long)(cr_was != 0U), (unsigned long)hsidiv,
             (unsigned long)hsi_ready(), (unsigned long)d2_was, (unsigned long)RCC->D2CCIP2R, (unsigned long)s_k123,
             (unsigned long)k, (unsigned long)(s_k123 / k), (unsigned long)(((s_k123 % k) * 100U) / k),
             (unsigned long)o.timingr, (unsigned long)s_f_main);
    uint32_t res[4][4];
    bool sent = ok;
    for (uint32_t v = 0U; (v < 4U) && sent; v++)
    {
        sent = host_at("F334 STATUS t12=%lu", (unsigned long)v) &&
               cm4_send4(IST_CMD_M4_T12, v, T12_COUNT, budget(s_cpu_hz / 2U, s_f_main, 3U) + ms_cyc(1U) / 2U, 0U);
        for (uint32_t i = 0U; (i < 4U) && sent; i++)
        {
            res[v][i] = IST_AREA->m4_res[i];
        }
        if (sent)
        {
            LOG("T12 %s: %lu of %lu OK, %lu stalled (ERR_TIMEOUT, then PE = 0), first other status 0x%lX, slowest "
                "%lu cycles", k_name[v], (unsigned long)res[v][0], (unsigned long)T12_COUNT, (unsigned long)res[v][1],
                (unsigned long)res[v][2], (unsigned long)res[v][3]);
        }
        (void)delay_ms(500U);                            /* the target's reports drain */
    }
    const bool st_ok = sent && host_at("F334 STATUS t12=4");
    /* Back: I2C1 off the HSI, then the HSI as it was; each restored on its own and
       read back, so one failing does not skip the other (R3-4, m19). */
    deinit_judged(true, "T12 (I2C1 off the HSI kernel)");
    const bool sel_back = i2c123sel_set((d2_was & RCC_D2CCIP2R_I2C123SEL_Msk) >> RCC_D2CCIP2R_I2C123SEL_Pos) &&
                          (RCC->D2CCIP2R == d2_was);
    const bool hsi_back = !hsi_wrote || rcc_cr_set(RCC_CR_HSION, false);
    const bool back = sel_back && hsi_back && ((RCC->CR & RCC_CR_HSION) == cr_was);
    verdict(ok && sent && st_ok && (res[0][0] == T12_COUNT) && (res[1][0] == T12_COUNT),
            "T12 2.19.6 with the kernel on the HSI: driver poll %lu/%lu and DMA %lu/%lu writes of 2 bytes OK (0 stalls)",
            (unsigned long)(sent ? res[0][0] : 0U), (unsigned long)T12_COUNT, (unsigned long)(sent ? res[1][0] : 0U),
            (unsigned long)T12_COUNT);
    if (sent)
    {
        const uint32_t man = res[2][1] + res[3][1];
        LOG("T12 control (decision 6): without the preload, %lu stalls by hand in polling and %lu with DMA in %lu each: "
            "%s", (unsigned long)res[2][1], (unsigned long)res[3][1], (unsigned long)T12_COUNT,
            (man != 0U) ? "reproduced" : "no reproducido (not reproduced)");
    }
    verdict(back, "T12 restore: RCC_D2CCIP2R 0x%08lX (0x%08lX before, restored %u), RCC_CR HSION %lu (%lu before; "
            "written %u, restored %u)", (unsigned long)RCC->D2CCIP2R, (unsigned long)d2_was, (unsigned)sel_back,
            (unsigned long)((RCC->CR & RCC_CR_HSION) != 0U), (unsigned long)(cr_was != 0U), (unsigned)hsi_wrote,
            (unsigned)hsi_back);
}

/* ---- The bus steps -------------------------------------------------------------------- */

/* PB8/PB9 to AF6 (I2C4) through the claim (S15, S16). */
static bool cm7_pins_af(void)
{
    (void)I2C_BM_EnableClock(I2C4);
    i2c_release(I2C4);
    return pin_cfg(GPIOB, IST_PIN_SCL, GPIO_BM_MODE_AF, IST_AF_I2C4, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE) &&
           pin_cfg(GPIOB, IST_PIN_SDA, GPIO_BM_MODE_AF, IST_AF_I2C4, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE);
}

static bool cm7_pins_input(void)
{
    return pin_cfg(GPIOB, IST_PIN_SCL, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE) &&
           pin_cfg(GPIOB, IST_PIN_SDA, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE);
}

/* T9: PB8/PB9 GPIO open-drain, released (ODR = 1 first), claimed together (S16 refuses
   a second output pin while the first already is one); step F1_RECOVER only (od_out). */
static bool cm7_pins_od(void)
{
    if (!bench_claim(GPIOB, IST_BUS_MASK, GPIO_BM_MODE_OUTPUT, 0U, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE))
    {
        bench_abort("claim refused");
        return false;
    }
    const bool ok = (GPIO_BM_ConfigOutput(GPIOB, IST_PIN_SCL, GPIO_BM_OTYPE_OD, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 1U) ==
                     GPIO_BM_OK) &&
                    (GPIO_BM_ConfigOutput(GPIOB, IST_PIN_SDA, GPIO_BM_OTYPE_OD, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 1U) ==
                     GPIO_BM_OK);
    if (!ok)
    {
        bench_abort("config");
    }
    return ok;
}

static void bus_write(void *ctx, bool high, uint32_t pin)
{
    (void)ctx;
    GPIOB->BSRR = high ? (1UL << pin) : (1UL << (pin + 16U));
    __DSB();
}

static void rec_scl_w(void *ctx, bool high) { bus_write(ctx, high, IST_PIN_SCL); }
static void rec_sda_w(void *ctx, bool high) { bus_write(ctx, high, IST_PIN_SDA); }
static bool rec_scl_r(void *ctx) { (void)ctx; return ((GPIOB->IDR >> IST_PIN_SCL) & 1UL) != 0U; }
static bool rec_sda_r(void *ctx) { (void)ctx; return ((GPIOB->IDR >> IST_PIN_SDA) & 1UL) != 0U; }

static void t9(void)
{
    uint32_t tr = 0U;
    uint32_t r[IST_XR_N];
    uint8_t b[2] = { 0x30U, 0x31U };
    if (!cm7_pins_af() || !main_init(false, 0U, &tr))
    {
        return;
    }
    /* The scope first: arming it takes longer than the 300 ms hold. It triggers on the
       target's release of SDA (its 5th SCL edge, inside RecoverBus), with 3/4 of the
       buffer before it: the capture is placed by the recovery itself, not by when the
       host orders the hold (audit F2 part 1, B3). */
    if (s_taps && !host_at("SCOPE T9 k=0 f=0 kernel=0 timingr=0 model=0 rate=2000000 trig=sdar"))
    {
        return;
    }
    if (!host_at("HOLD SDA 300 EDGES 5"))
    {
        return;
    }
    const bool low = wait_pin(IST_PIN_SDA, 0U, HOLD_WAIT_MS);       /* the hold starts after the handshake (B2) */
    (void)delay_ms(1U);
    const uint32_t isr = I2C4->ISR;
    const uint32_t t0 = DWT->CYCCNT;
    const I2cBm_Status_t sw = I2C_BM_Write(I2C4, T_ADDR, b, 2U, ms_cyc(5U));
    const uint32_t cw = DWT->CYCCNT - t0;
    /* The recovery: PE = 0 (Deinit), the pins to input, then GPIO open-drain. The
       Deinit is judged after the recovery: a verdict line here (blocking UART) would
       delay it by ms. */
    uint32_t d_diff = 0U;
    const bool d_ok = inst_deinit(false, &d_diff);
    bool ok = cm7_pins_input() && delay_ms(1U) && cm7_pins_od();
    if (!ok)
    {
        return;
    }
    const I2cBm_Recovery_t rec = { rec_scl_w, rec_sda_w, rec_scl_r, rec_sda_r, NULL, 5U * CYC_PER_US };
    uint32_t pulses = 0xFFU;
    const I2cBm_Status_t sr = I2C_BM_RecoverBus(&rec, ms_cyc(10U), &pulses);
    const uint32_t lv = bus_idr() & 3U;
    if (s_taps && !host_at("SCOPE end"))
    {
        return;
    }
    ok = cm7_pins_input() && delay_ms(1U) && cm7_pins_af() && main_init(false, 0U, &tr);
    if (!ok || !host_at("HOLD_END"))
    {
        return;
    }
    if (!xfer(false, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 2U, 0x00009012UL, budget(s_cpu_hz, s_f_main, 3U), r))
    {
        return;
    }
    xline("T9", false, s_f_main, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 2U, 0x00009012UL, r, true);
    verdict(d_ok, "T14 I2C4 after the T9 Write on the held bus: Deinit, registers that differ from the reset image "
            "0x%02lX", (unsigned long)d_diff);
    verdict(low && ((isr & I2C_ISR_BUSY) != 0U) && ((sw == I2C_BM_ERR_BUS) || (sw == I2C_BM_ERR_TIMEOUT)) &&
            (cw <= ms_cyc(6U)) && (sr == I2C_BM_OK) && (pulses == 5U) && (lv == 3U) &&
            (r[IST_XR_STATUS] == (uint32_t)I2C_BM_OK),
            "T9 the target holds SDA until 5 SCL edges: I2C4 ISR 0x%08lX (BUSY), Write %u in %lu cycles (ERR_BUS %u or "
            "ERR_TIMEOUT %u, bounded); PB8/PB9 GPIO open-drain, RecoverBus %u, pulses %lu (5), PB8/PB9 after %lu (3); "
            "AF6 and Init again, Write %lu (OK)", (unsigned long)isr, (unsigned)sw, (unsigned long)cw,
            (unsigned)I2C_BM_ERR_BUS, (unsigned)I2C_BM_ERR_TIMEOUT, (unsigned)sr, (unsigned long)pulses,
            (unsigned long)lv, (unsigned long)r[IST_XR_STATUS]);
    /* The control: no RecoverBus; SDA stays low until the target's own limit. */
    if (!host_at("HOLD SDA 100"))
    {
        return;
    }
    const bool low_c = wait_pin(IST_PIN_SDA, 0U, HOLD_WAIT_MS);     /* timed from the fall (B2) */
    const uint32_t t1 = DWT->CYCCNT;
    const bool high_c = wait_pin(IST_PIN_SDA, 1U, 300U);
    const uint32_t ms = (DWT->CYCCNT - t1) / (s_cpu_hz / 1000U);
    if (!host_at("HOLD_END"))
    {
        return;
    }
    verdict(low_c && high_c && (ms >= 95U) && (ms <= 110U),
            "T9 control without RecoverBus: SDA low %lu ms (the target's 100 ms limit), then released %u",
            (unsigned long)ms, (unsigned)high_c);
    deinit_judged(false, "T9");
    (void)cm7_pins_input();
}

/* T9(b), plan R3-6 (audit F1, M2): the target holds SDA BEFORE Init. ISR.BUSY is set
   only by a START seen with PE = 1 (50:2897-2900), so Init returns OK with the bus
   held, and the transfer then ends, bounded by its max_cycles, with the code the
   silicon gives: recorded, and judged to be ERR_ARLO, ERR_BUS or ERR_TIMEOUT. Then
   the recovery of T9 (RecoverBus gives the pulses the target still needs of its 5
   edges: the refused transfer may have clocked some), Init and a transfer that
   works. The control is T9 itself: the hold after Init gives ERR_BUS. */
#define T9B_BUDGET_MS   2U

static void t9b(void)
{
    uint32_t r[IST_XR_N];
    uint32_t img[IST_NREG];
    uint8_t b[2] = { 0x32U, 0x33U };
    I2cBm_TimingOut_t o = { 0U, 0U, 0U };
    (void)bench_timing(s_k4, s_f_main, &o);
    if (!cm7_pins_af())                                   /* AF6 with I2C4 at PE = 0: nothing drives */
    {
        return;
    }
    /* The scope triggers on the target's release of SDA (its 5th SCL edge), with 3/4
       of the 8.2 ms buffer (6.1 ms) before it: the refused Write and RecoverBus both
       come before the release, and the span from the Write's start to the end of
       RecoverBus is this core's own code, measured below and printed after it (B3). */
    if (s_taps && !host_at("SCOPE T9B k=0 f=%lu kernel=0 timingr=0 model=0 rate=1000000 trig=sdar",
                           (unsigned long)s_f_main))
    {
        return;
    }
    if (!host_at("HOLD SDA 300 EDGES 5"))
    {
        return;
    }
    const bool low = wait_pin(IST_PIN_SDA, 0U, HOLD_WAIT_MS);       /* the hold starts after the handshake (B2) */
    (void)delay_ms(1U);
    const I2cBm_Status_t si = inst_init(false, o.timingr, 0U, img);   /* the hold is already there */
    const uint32_t isr = I2C4->ISR;
    const uint32_t chk = (uint32_t)I2C_BM_CheckTransfer(I2C4, T_ADDR, 2U, 0U);
    const uint32_t lim = ms_cyc(T9B_BUDGET_MS);
    const uint32_t t0 = DWT->CYCCNT;
    const I2cBm_Status_t sw = I2C_BM_Write(I2C4, T_ADDR, b, 2U, lim);
    const uint32_t cw = DWT->CYCCNT - t0;
    const uint32_t lv0 = bus_idr() & 3U;
    uint32_t d_diff = 0U;
    const bool d_ok = inst_deinit(false, &d_diff);       /* judged after the recovery (blocking UART) */
    if (!cm7_pins_input() || !delay_ms(1U) || !cm7_pins_od())
    {
        return;
    }
    const I2cBm_Recovery_t rec = { rec_scl_w, rec_sda_w, rec_scl_r, rec_sda_r, NULL, 5U * CYC_PER_US };
    uint32_t pulses = 0xFFU;
    const I2cBm_Status_t sr = I2C_BM_RecoverBus(&rec, ms_cyc(10U), &pulses);
    const uint32_t span = DWT->CYCCNT - t0;              /* the Write's start to RecoverBus's end */
    const uint32_t lv = bus_idr() & 3U;
    if (s_taps && !host_at("SCOPE end"))
    {
        return;
    }
    uint32_t tr = 0U;
    if (!cm7_pins_input() || !delay_ms(1U) || !cm7_pins_af() || !main_init(false, 0U, &tr) || !host_at("HOLD_END"))
    {
        return;
    }
    if (!xfer(false, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 2U, 0x00009B12UL, budget(s_cpu_hz, s_f_main, 3U), r))
    {
        return;
    }
    xline("T9B", false, s_f_main, IST_XF(IST_OP_WRITE, IST_MODE_POLL, T_ADDR), 2U, 0x00009B12UL, r, true);
    verdict(d_ok, "T14 I2C4 after the T9(b) Write on the bus held before Init: Deinit, registers that differ from "
            "the reset image 0x%02lX", (unsigned long)d_diff);
    const bool in_set = (sw == I2C_BM_ERR_ARLO) || (sw == I2C_BM_ERR_BUS) || (sw == I2C_BM_ERR_TIMEOUT);
    const bool bounded = cw <= (lim + ms_cyc(1U));
    /* After the refused Write (audit F1 part 2, m34): SCL released (high); SDA still
       held by the target if the Write gave it fewer than its 5 edges, and then
       RecoverBus must give at least one pulse; SDA already free means no pulse at
       all. The host judges pulses + the Write's SCL edges = the target's EDGES. */
    const bool sda_low0 = (lv0 & 2U) == 0U;
    const bool pulses_ok = (pulses <= 5U) && (sda_low0 ? (pulses >= 1U) : (pulses == 0U));
    LOG("T9(b) span: from the Write's start to the end of RecoverBus %lu us (CYCCNT)",
        (unsigned long)(span / (s_cpu_hz / 1000000U)));
    verdict(low && (si == I2C_BM_OK) && ((isr & I2C_ISR_BUSY) == 0U) && (chk == (uint32_t)I2C_BM_OK) && in_set &&
            bounded && ((lv0 & 1U) != 0U) && (sr == I2C_BM_OK) && pulses_ok && (lv == 3U) &&
            (r[IST_XR_STATUS] == (uint32_t)I2C_BM_OK),
            "T9(b) the target holds SDA from before Init: Init %u (OK), I2C4 ISR 0x%08lX (BUSY 0: no START seen), "
            "CheckTransfer %lu (OK); Write %u in %lu cycles (one of ERR_ARLO %u, ERR_BUS %u, ERR_TIMEOUT %u; budget "
            "%lu + 1 ms), PB8/PB9 after it %lu (SCL high; SDA low: pulses 1..5, SDA high: 0); RecoverBus %u, pulses %lu, "
            "PB8/PB9 after %lu (3); AF6 and Init again, Write %lu (OK)", (unsigned)si, (unsigned long)isr,
            (unsigned long)chk, (unsigned)sw, (unsigned long)cw, (unsigned)I2C_BM_ERR_ARLO, (unsigned)I2C_BM_ERR_BUS,
            (unsigned)I2C_BM_ERR_TIMEOUT, (unsigned long)lim, (unsigned long)lv0, (unsigned)sr,
            (unsigned long)pulses, (unsigned long)lv, (unsigned long)r[IST_XR_STATUS]);
    deinit_judged(false, "T9(b)");
    (void)cm7_pins_input();
}

static void step_f1_cm4(void)
{
    if (!step_begin("F1_CM4"))
    {
        return;
    }
    uint32_t tr = 0U;
    if (!cm4_send4(IST_CMD_M4_PINS, 1U, 0U, 0U, 0U) || (IST_AREA->m4_res[0] != 1U))
    {
        bench_abort("CM4 pins");
        return;
    }
    t4_t5(true);
    const bool fm = s_f_main > 400000U;
    const bool fm_on = !s_bench_abort && fm && fmp(true, true);
    if (!s_bench_abort && (!fm || fm_on) && main_init(true, 0U, &tr))
    {
        t1_busy(true);
        t6(true);
        t7(true);
        t8(true);
        t10(true);
        if (host_at("RPT lenient"))                     /* B1 and T12 print no X line */
        {
            b1(true, tr);
            t12();
            (void)host_at("RPT strict");
        }
        deinit_judged(true, "the F1_CM4 tests at the main speed");
    }
    if (fm_on)
    {
        (void)fmp(true, false);
    }
    (void)cm4_send4(IST_CMD_M4_PINS, 0U, 0U, 0U, 0U);
    fmp_judge("F1_CM4");
    if (!host_at("F1_CM4 end"))
    {
        return;
    }
    step_end();
}

static void step_f1_cm7(void)
{
    if (!step_begin("F1_CM7"))
    {
        return;
    }
    /* S16: the other instance had the pins until the last step. */
    if (!delay_ms(2U) || !cm7_pins_af())
    {
        bench_abort("CM7 pins");
        return;
    }
    uint32_t tr = 0U;
    t4_t5(false);
    const bool fm = s_f_main > 400000U;
    const bool fm_on = !s_bench_abort && fm && fmp(false, true);
    if (!s_bench_abort && (!fm || fm_on) && main_init(false, 0U, &tr))
    {
        t1_busy(false);
        t6(false);
        t7(false);
        t8(false);
        t10(false);
        if (host_at("RPT lenient"))
        {
            b1(false, tr);
            (void)delay_ms(100U);
            (void)host_at("RPT strict");
        }
        deinit_judged(false, "the F1_CM7 tests at the main speed");
    }
    if (fm_on)
    {
        (void)fmp(false, false);
    }
    (void)cm7_pins_input();
    fmp_judge("F1_CM7");
    if (!host_at("F1_CM7 end"))
    {
        return;
    }
    step_end();
}

static void step_f1_recover(void)
{
    if (!step_begin("F1_RECOVER"))
    {
        return;
    }
    const bool fm = s_f_main > 400000U;
    if (!delay_ms(2U) || (fm && !fmp(false, true)))
    {
        bench_abort("F1_RECOVER");
        return;
    }
    t9();
    t9b();
    if (fm)
    {
        (void)fmp(false, false);
    }
    (void)cm7_pins_input();
    fmp_judge("F1_RECOVER");
    if (!host_at("F1_RECOVER end"))
    {
        return;
    }
    step_end();
}

/* ---- Phase F2: i2c_ll ------------------------------------------------------------------ */

/* T2 with i2c_ll: the LL image of every case of the matrix against the RM fields and,
   bit for bit, against i2c_bm's (plan T2, F2 column), on I2C2, I2C3, I2C4 and CPU2's
   I2C1. */
static void t2ll_print(const char *who, const IstT2LlResult_t *t)
{
    verdict((t->cases == 120U) && (t->bad_rm == 0U) && (t->bad_parity == 0U) && (t->bad_status == 0U) &&
            (t->bad_deinit == 0U) && (t->control == t->cases),
            "T2 %s images over TIMINGR x filter x DNF x timeout: %lu cases; against the RM fields %lu differ; against "
            "the image of i2c_bm for the same configuration %lu differ (first case<<4|reg 0x%lX: LL 0x%08lX BM "
            "0x%08lX); %lu Init statuses not OK/ERR_BUS or not BM's, %lu Deinit not at reset; control (BM's image "
            "with DNF + 1, the same parity comparison) told apart in %lu cases (all)", who, (unsigned long)t->cases,
            (unsigned long)t->bad_rm, (unsigned long)t->bad_parity, (unsigned long)t->first_bad,
            (unsigned long)t->got, (unsigned long)t->exp, (unsigned long)t->bad_status, (unsigned long)t->bad_deinit,
            (unsigned long)t->control);
}

static void f2_t2(void)
{
    static const struct { I2C_TypeDef *i2c; const char *name; } inst[3] = {
        { I2C2, "CM7 I2C2" }, { I2C3, "CM7 I2C3" }, { I2C4, "CM7 I2C4" } };
    for (uint32_t i = 0U; i < 3U; i++)
    {
        IstT2LlResult_t t;
        (void)I2C_BM_EnableClock(inst[i].i2c);
        ist_t2_ll(inst[i].i2c, &t);
        t2ll_print(inst[i].name, &t);
        if (inst[i].i2c != I2C4)
        {
            (void)I2C_BM_DisableClock(inst[i].i2c);
        }
    }
    if (cm4_send4(IST_CMD_M4_T2LL, 0U, 0U, 0U, 0U))
    {
        const volatile uint32_t *r = IST_AREA->m4_res;
        const IstT2LlResult_t t = { r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8] };
        t2ll_print("CM4 I2C1", &t);
    }
}

static void step_f2_nopin(void)
{
    if (!step_begin("F2_NOPIN"))
    {
        return;
    }
    s_ll = true;
    LOG("F2: from here the configuration and the transfers go through i2c_ll (verdicts prefixed LL)");
    f1_t1();                                              /* T1 with i2c_ll: the same refusals + 1 */
    f2_t2();
    s_ll = false;
    if (!host_at("F2_NOPIN end"))
    {
        return;
    }
    step_end();
}

/* The bus tests of F2 on one core: T4 (T5 with taps), T1 with BUSY, T6, T7 and the
   Deinit of each (T14), with i2c_ll; the rest of F1 (T8-T10, T12, B1) stays on i2c_bm. */
static void f2_bus(bool m4, const char *step)
{
    uint32_t tr = 0U;
    s_ll = true;
    t4_t5(m4);
    const bool fm = s_f_main > 400000U;
    const bool fm_on = !s_bench_abort && fm && fmp(m4, true);
    if (!s_bench_abort && (!fm || fm_on) && main_init(m4, 0U, &tr))
    {
        t1_busy(m4);
        t6(m4);
        t7(m4);
        deinit_judged(m4, m4 ? "the F2_CM4 tests at the main speed" : "the F2_CM7 tests at the main speed");
    }
    if (fm_on)
    {
        (void)fmp(m4, false);
    }
    s_ll = false;
    fmp_judge(step);
}

static void step_f2_cm4(void)
{
    if (!step_begin("F2_CM4"))
    {
        return;
    }
    if (!cm4_send4(IST_CMD_M4_PINS, 1U, 0U, 0U, 0U) || (IST_AREA->m4_res[0] != 1U))
    {
        bench_abort("CM4 pins");
        return;
    }
    f2_bus(true, "F2_CM4");
    (void)cm4_send4(IST_CMD_M4_PINS, 0U, 0U, 0U, 0U);
    if (!host_at("F2_CM4 end"))
    {
        return;
    }
    step_end();
}

static void step_f2_cm7(void)
{
    if (!step_begin("F2_CM7"))
    {
        return;
    }
    if (!delay_ms(2U) || !cm7_pins_af())                 /* S16: CPU2 had the pins until the last step */
    {
        bench_abort("CM7 pins");
        return;
    }
    f2_bus(false, "F2_CM7");
    (void)cm7_pins_input();
    if (!host_at("F2_CM7 end"))
    {
        return;
    }
    step_end();
}

/* ---- Phase F3: i2c_hal (CPU1 only) ------------------------------------------------------ */

static I2C_HandleTypeDef s_hh;              /* F3_NOPIN: the handle of I2C2 / I2C3 / I2C4 */

/* HAL_GetTick() through the session: the host checks the TICK lines rise (plan F3). */
static void tick_line(const char *where)
{
    LOG("TICK %s %lu", where, (unsigned long)HAL_GetTick());
}

static void hal_handle(I2C_HandleTypeDef *h, I2C_TypeDef *i2c)
{
    (void)memset(h, 0, sizeof(*h));
    h->Instance = i2c;
}

/* T1 with i2c_hal on I2C2 (no pins): its own refusals, each status written by hand,
   with the photo unchanged in both groups: ist_t1_photo() and the handle's State
   (audit F3 part 1, m49). CalcTiming, the clock, GetCaps, RecoverBus... are i2c_bm's
   in every tier (F1, T1). The control: a valid I2C_HAL_Init changes the photo. Each
   I2C_HAL_Deinit of the test is judged: OK and the reset image (T14, m48). */
#define IST_T1_HAL_CASES    48U
#define HAL_PHOTO           (IST_T1_PHOTO + 1U)

static void hal_photo(const I2C_HandleTypeDef *h, uint32_t s[HAL_PHOTO])
{
    ist_t1_photo(h->Instance, s);
    s[IST_T1_PHOTO] = (uint32_t)h->State;
}

static bool hal_photo_eq(const uint32_t a[HAL_PHOTO], const uint32_t b[HAL_PHOTO])
{
    return ist_t1_photo_eq(a, b) && (a[IST_T1_PHOTO] == b[IST_T1_PHOTO]);
}

/* T14 of the HAL tier: I2C_HAL_Deinit OK, the handle in RESET and the reset image. */
static bool hal_deinit_ok(I2C_HandleTypeDef *h)
{
    const I2cBm_Status_t st = I2C_HAL_Deinit(h);
    return (st == I2C_BM_OK) && (h->State == HAL_I2C_STATE_RESET) && (ist_reset_diff(h->Instance) == 0U);
}

static void f3_t1(void)
{
    static IstT1Result_t t;
    uint32_t a[HAL_PHOTO];
    uint32_t b[HAL_PHOTO];
    uint8_t buf[4] = { 0U, 1U, 2U, 3U };
    I2cBm_TimingOut_t o = { 0U, 0U, 0U };
    (void)bench_timing(s_k123, 100000U, &o);
    (void)I2C_BM_EnableClock(I2C2);
    I2C_HandleTypeDef *h = &s_hh;
    hal_handle(h, I2C2);
    const I2cBm_Config_t c = { I2C2, o.timingr, true, 0U, 0U };
    I2cBm_Config_t k;
    t.cases = 0U;
    t.bad = 0U;
    t.groups_same = 0U;

    /* 1. The instance at reset. */
    (void)I2C_BM_Deinit(I2C2);
    hal_photo(h, a);
    ist_t1_case(&t, I2C_HAL_Init(NULL, &c), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_Init(h, NULL), I2C_BM_ERR_PARAM);
    h->Instance = (I2C_TypeDef *)USART3;                    ist_t1_case(&t, I2C_HAL_Init(h, &c), I2C_BM_ERR_PARAM);
    h->Instance = I2C3;                                     ist_t1_case(&t, I2C_HAL_Init(h, &c), I2C_BM_ERR_PARAM);
    h->Instance = I2C2;
    k = c; k.dnf = 16U;                                     ist_t1_case(&t, I2C_HAL_Init(h, &k), I2C_BM_ERR_PARAM);
    k = c; k.timingr = o.timingr | 0x01000000UL;            ist_t1_case(&t, I2C_HAL_Init(h, &k), I2C_BM_ERR_PARAM);
    k = c; k.timeout_scl_low = 10U;                         ist_t1_case(&t, I2C_HAL_Init(h, &k), I2C_BM_ERR_UNSUPPORTED);
    k = c; k.timeout_scl_low = 4097U;                       ist_t1_case(&t, I2C_HAL_Init(h, &k), I2C_BM_ERR_UNSUPPORTED);
    ist_t1_case(&t, I2C_HAL_Write(NULL, T_ADDR, buf, 1U, 10U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_Write(h, T_ADDR, buf, 1U, 0U), I2C_BM_ERR_PARAM);          /* timeout_ms 0 */
    /* M4: HAL_MAX_DELAY would leave the HAL's waits unbounded. */
    ist_t1_case(&t, I2C_HAL_Write(h, T_ADDR, buf, 1U, HAL_MAX_DELAY), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_Read(h, T_ADDR, buf, 1U, HAL_MAX_DELAY), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_WriteRead(h, T_ADDR, buf, 1U, buf, 1U, HAL_MAX_DELAY), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_Wait(h, HAL_MAX_DELAY), I2C_BM_ERR_PARAM);
    /* m40: the tier's limit before i2c_bm's check, which would say ERR_BUSY (PE = 0). */
    ist_t1_case(&t, I2C_HAL_WriteRead(h, T_ADDR, buf, 3U, buf, 1U, 10U), I2C_BM_ERR_UNSUPPORTED);
    ist_t1_case(&t, I2C_HAL_StartIt(h, T_ADDR, buf, 3U, buf, 1U), I2C_BM_ERR_UNSUPPORTED);
    ist_t1_case(&t, I2C_HAL_Write(h, T_ADDR, buf, 1U, 10U), I2C_BM_ERR_BUSY);          /* PE = 0 */
    ist_t1_case(&t, I2C_HAL_Read(h, T_ADDR, buf, 1U, 10U), I2C_BM_ERR_BUSY);
    ist_t1_case(&t, I2C_HAL_WriteRead(h, T_ADDR, buf, 1U, buf, 1U, 10U), I2C_BM_ERR_BUSY);
    ist_t1_case(&t, I2C_HAL_Read(NULL, T_ADDR, buf, 1U, 10U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_WriteRead(NULL, T_ADDR, buf, 1U, buf, 1U, 10U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_StartIt(NULL, T_ADDR, buf, 1U, NULL, 0U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_StartIt(h, T_ADDR, buf, 1U, NULL, 0U), I2C_BM_ERR_BUSY);
    ist_t1_case(&t, I2C_HAL_StartDma(h, T_ADDR, buf, NULL, 2U), I2C_BM_ERR_PARAM);      /* no hdmatx */
    ist_t1_case(&t, I2C_HAL_StartDma(h, T_ADDR, NULL, buf, 2U), I2C_BM_ERR_PARAM);      /* no hdmarx */
    ist_t1_case(&t, I2C_HAL_Wait(NULL, 10U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_Wait(h, 0U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_Deinit(NULL), I2C_BM_ERR_PARAM);
    h->Instance = (I2C_TypeDef *)USART3;                    ist_t1_case(&t, I2C_HAL_Deinit(h), I2C_BM_ERR_PARAM);
    h->Instance = I2C2;
    hal_photo(h, b);
    t.groups_same |= hal_photo_eq(a, b) ? 1U : 0U;

    /* 2. Initialised (PE = 1, handle READY): what the hardware would ignore, the transfers'
       own fields, this tier's limits, and a handle that is not READY. */
    const I2cBm_Status_t i2 = I2C_HAL_Init(h, &c);
    hal_photo(h, a);
    k = c; k.timingr = o.timingr ^ 0x00000001UL;            ist_t1_case(&t, I2C_HAL_Init(h, &k), I2C_BM_ERR_BUSY);
    k = c; k.dnf = 1U;                                      ist_t1_case(&t, I2C_HAL_Init(h, &k), I2C_BM_ERR_BUSY);
    ist_t1_case(&t, I2C_HAL_Write(h, 0x07U, buf, 1U, 10U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_Write(h, 0x78U, buf, 1U, 10U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_Write(h, T_ADDR, buf, 65536UL, 10U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_Write(h, T_ADDR, NULL, 1U, 10U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_Write(h, T_ADDR, buf, 1U, HAL_MAX_DELAY), I2C_BM_ERR_PARAM);   /* M4, initialised */
    ist_t1_case(&t, I2C_HAL_Read(h, T_ADDR, buf, 0U, 10U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_WriteRead(h, T_ADDR, buf, 0U, buf, 1U, 10U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_WriteRead(h, T_ADDR, buf, 1U, buf, 0U, 10U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_WriteRead(h, T_ADDR, buf, 3U, buf, 1U, 10U), I2C_BM_ERR_UNSUPPORTED);
    ist_t1_case(&t, I2C_HAL_StartIt(h, T_ADDR, NULL, 1U, NULL, 0U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_StartIt(h, T_ADDR, buf, 3U, buf, 1U), I2C_BM_ERR_UNSUPPORTED);
    h->hdmatx = &s_hbd_tx;                                  /* linked, never started: refused first */
    h->hdmarx = &s_hbd_rx;
    ist_t1_case(&t, I2C_HAL_StartDma(h, T_ADDR, NULL, NULL, 2U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_StartDma(h, T_ADDR, buf, buf, 2U), I2C_BM_ERR_PARAM);
    ist_t1_case(&t, I2C_HAL_StartDma(h, T_ADDR, buf, NULL, 1U), I2C_BM_ERR_PARAM);      /* n < 2 */
    ist_t1_case(&t, I2C_HAL_StartDma(h, T_ADDR, NULL, buf, 0U), I2C_BM_ERR_PARAM);
    h->hdmatx = NULL;
    h->hdmarx = NULL;
    h->State = HAL_I2C_STATE_BUSY_TX;                       /* a transfer running, as the HAL marks it */
    ist_t1_case(&t, I2C_HAL_Write(h, T_ADDR, buf, 1U, 10U), I2C_BM_ERR_BUSY);
    ist_t1_case(&t, I2C_HAL_StartIt(h, T_ADDR, buf, 1U, NULL, 0U), I2C_BM_ERR_BUSY);
    h->State = HAL_I2C_STATE_READY;
    hal_photo(h, b);
    t.groups_same |= hal_photo_eq(a, b) ? 2U : 0U;
    const bool d1 = hal_deinit_ok(h);

    /* The control: a valid Init changes the photo. */
    hal_photo(h, a);
    const I2cBm_Status_t ic = I2C_HAL_Init(h, &c);
    hal_photo(h, b);
    t.control_changed = (!hal_photo_eq(a, b) && ((ic == I2C_BM_OK) || (ic == I2C_BM_ERR_BUS)) &&
                         ((i2 == I2C_BM_OK) || (i2 == I2C_BM_ERR_BUS))) ? 1U : 0U;
    t.init_status = (uint32_t)ic;
    const bool d2 = hal_deinit_ok(h);
    (void)I2C_BM_DisableClock(I2C2);
    t1_print("CM7 I2C2 HAL", &t);
    verdict((t.bad == 0U) && (t.cases == IST_T1_HAL_CASES) && (t.groups_same == 3U) && (t.control_changed == 1U) &&
            d1 && d2,
            "T1 CM7 refusals of i2c_hal on I2C2: %lu cases (%u), %lu with another status; instance, RCC_C1/C2 "
            "APB1LENR/APB4ENR, D2CCIP2R, D3CCIPR, SYSCFG_PMCR and the handle's State unchanged in the 2 groups: "
            "0x%lX (0x3); control (a valid I2C_HAL_Init) changes them %lu (Init %lu); T14: both I2C_HAL_Deinit OK "
            "with the reset image %u %u", (unsigned long)t.cases, (unsigned)IST_T1_HAL_CASES,
            (unsigned long)t.bad, (unsigned long)t.groups_same, (unsigned long)t.control_changed,
            (unsigned long)t.init_status, (unsigned)d1, (unsigned)d2);
}

/* The raw HAL, as the test's own reference (plan T2, F3): HAL_I2C_Init and the two filter
   calls with the Init fields written here by hand, not taken from i2c_hal.c. */
static HAL_StatusTypeDef hal_raw_init(I2C_HandleTypeDef *h, I2C_TypeDef *i2c, uint32_t timingr, bool analog,
                                      uint32_t dnf)
{
    hal_handle(h, i2c);                                     /* State RESET */
    h->Init.Timing           = timingr;
    h->Init.OwnAddress1      = 0U;
    h->Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    h->Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    h->Init.OwnAddress2      = 0U;
    h->Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    h->Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    h->Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
    HAL_StatusTypeDef hs = HAL_I2C_Init(h);
    if (hs == HAL_OK)
    {
        hs = HAL_I2CEx_ConfigAnalogFilter(h, analog ? I2C_ANALOGFILTER_ENABLE : I2C_ANALOGFILTER_DISABLE);
    }
    if (hs == HAL_OK)
    {
        hs = HAL_I2CEx_ConfigDigitalFilter(h, dnf);
    }
    return hs;
}

/* The differences by design between the HAL's image and i2c_bm's (i2c_hal.h, T2),
   typed here: OAR1.OA1EN (HAL_I2C_Init l. 600) and CR2.AUTOEND (l. 619; its NACK is
   written with PE = 0, which holds it cleared, RM0399 50:2621-2623). */
#define T2H_OAR1_HAL    I2C_OAR1_OA1EN
#define T2H_CR2_HAL     I2C_CR2_AUTOEND
#define T2H_DESIGN_REGS ((1UL << 1) | (1UL << 2))          /* CR2, OAR1 in ist_regs() order */

typedef struct
{
    uint32_t cases;
    uint32_t hal_cases;        /* timeout 0: initialised by the HAL                     */
    uint32_t refused;          /* timeout != 0: ERR_UNSUPPORTED with the reset image     */
    uint32_t bad_raw;          /* HAL images that differ from the raw HAL_I2C_Init's     */
    uint32_t bad_bm;           /* ... from i2c_bm's with the two design differences      */
    uint32_t bad_rm;           /* ... from the RM fields with the two design differences */
    uint32_t regs_vs_bm;       /* OR of the registers where HAL and BM differ            */
    uint32_t bad_status;
    uint32_t bad_deinit;
    uint32_t control;          /* told apart from the raw HAL image with DNF + 1: all    */
} T2HalResult_t;

static void f3_t2_one(I2C_TypeDef *i2c, T2HalResult_t *r)
{
    (void)memset(r, 0, sizeof(*r));
    I2cBm_Config_t c;
    uint32_t e[IST_NREG];
    for (uint32_t idx = 0U; ist_t2_case(i2c, idx, &c, e); idx++)
    {
        uint32_t g_bm[IST_NREG];
        uint32_t g_h[IST_NREG];
        uint32_t g_raw[IST_NREG];
        uint32_t g_raw2[IST_NREG];
        I2C_HandleTypeDef h2;
        const I2cBm_Status_t s_bm = I2C_BM_Init(&c);
        ist_regs(i2c, g_bm);
        bool d_ok = (I2C_BM_Deinit(i2c) == I2C_BM_OK) && (ist_reset_diff(i2c) == 0U);
        hal_handle(&s_hh, i2c);
        const I2cBm_Status_t s_h = I2C_HAL_Init(&s_hh, &c);
        ist_regs(i2c, g_h);
        d_ok = d_ok && (I2C_HAL_Deinit(&s_hh) == I2C_BM_OK) && (ist_reset_diff(i2c) == 0U);
        if (c.timeout_scl_low != 0U)
        {
            /* Refused before any write: the image is still the reset one. */
            if ((s_h == I2C_BM_ERR_UNSUPPORTED) && (ist_reset_diff_img(g_h) == 0U))
            {
                r->refused++;
            }
            else
            {
                r->bad_status++;
            }
        }
        else
        {
            r->hal_cases++;
            const HAL_StatusTypeDef h1 = hal_raw_init(&h2, i2c, c.timingr, c.analog_filter, c.dnf);
            ist_regs(i2c, g_raw);
            d_ok = d_ok && (I2C_BM_Deinit(i2c) == I2C_BM_OK) && (ist_reset_diff(i2c) == 0U);
            const HAL_StatusTypeDef h2s = hal_raw_init(&h2, i2c, c.timingr, c.analog_filter, (c.dnf + 1U) & 15U);
            ist_regs(i2c, g_raw2);
            d_ok = d_ok && (I2C_BM_Deinit(i2c) == I2C_BM_OK) && (ist_reset_diff(i2c) == 0U);
            if ((h1 != HAL_OK) || (h2s != HAL_OK) || (s_h != s_bm) || ((s_bm != I2C_BM_OK) && (s_bm != I2C_BM_ERR_BUS)))
            {
                r->bad_status++;
            }
            r->bad_raw += ist_t2_differs(g_h, g_raw) ? 1U : 0U;
            for (uint32_t k = 0U; k < 6U; k++)
            {
                r->regs_vs_bm |= (g_h[k] != g_bm[k]) ? (1UL << k) : 0U;
            }
            uint32_t adj[IST_NREG];
            for (uint32_t k = 0U; k < IST_NREG; k++)
            {
                adj[k] = g_bm[k];
            }
            adj[1] |= T2H_CR2_HAL;
            adj[2] |= T2H_OAR1_HAL;
            r->bad_bm += ist_t2_differs(g_h, adj) ? 1U : 0U;
            e[1] |= T2H_CR2_HAL;
            e[2] |= T2H_OAR1_HAL;
            r->bad_rm += ist_t2_differs(g_h, e) ? 1U : 0U;
            r->control += ist_t2_differs(g_h, g_raw2) ? 1U : 0U;
        }
        r->bad_deinit += d_ok ? 0U : 1U;
        r->cases++;
    }
}

static void f3_t2(void)
{
    static const struct { I2C_TypeDef *i2c; const char *name; } inst[3] = {
        { I2C2, "CM7 I2C2" }, { I2C3, "CM7 I2C3" }, { I2C4, "CM7 I2C4" } };
    for (uint32_t i = 0U; i < 3U; i++)
    {
        T2HalResult_t t;
        (void)I2C_BM_EnableClock(inst[i].i2c);
        f3_t2_one(inst[i].i2c, &t);
        verdict((t.cases == IST_T2_CASES) && (t.hal_cases == 30U) && (t.refused == 90U) && (t.bad_raw == 0U) &&
                (t.bad_bm == 0U) && (t.bad_rm == 0U) && (t.regs_vs_bm == T2H_DESIGN_REGS) && (t.bad_status == 0U) &&
                (t.bad_deinit == 0U) && (t.control == t.hal_cases),
                "T2 %s: %lu cases; timeout != 0 refused with the reset image %lu (90); of %lu initialised, "
                "against the raw HAL_I2C_Init %lu differ; against BM's + OAR1.OA1EN, CR2.AUTOEND (by design) %lu, "
                "registers that differ from BM's 0x%02lX (0x06: CR2, OAR1); against the RM fields + the same %lu; "
                "statuses %lu, Deinit %lu; control (raw HAL with DNF + 1) told apart in %lu (all)",
                inst[i].name, (unsigned long)t.cases, (unsigned long)t.refused, (unsigned long)t.hal_cases,
                (unsigned long)t.bad_raw, (unsigned long)t.bad_bm, (unsigned long)t.regs_vs_bm,
                (unsigned long)t.bad_rm, (unsigned long)t.bad_status, (unsigned long)t.bad_deinit,
                (unsigned long)t.control);
        if (inst[i].i2c != I2C4)
        {
            (void)I2C_BM_DisableClock(inst[i].i2c);
        }
    }
}

/* C13 in i2c_hal (lesson m4): I2C_HAL_Wait on a handle marked busy by hand (no register
   written) with PRIMASK set, so the tick cannot advance: it must end by its count of
   polls with the same HAL_GetTick(), not hang. Control: the same wait with the tick
   running ends by its 10 ms. I2C2, no pins. */
static void f3_wait_stall(void)
{
    I2cBm_TimingOut_t o = { 0U, 0U, 0U };
    (void)bench_timing(s_k123, 100000U, &o);
    (void)I2C_BM_EnableClock(I2C2);
    hal_handle(&s_hh, I2C2);
    const I2cBm_Config_t c = { I2C2, o.timingr, true, 0U, 0U };
    const I2cBm_Status_t si = I2C_HAL_Init(&s_hh, &c);
    s_hh.State = HAL_I2C_STATE_BUSY_TX;
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    const uint32_t k0 = HAL_GetTick();
    uint32_t c0 = DWT->CYCCNT;
    const I2cBm_Status_t s1 = I2C_HAL_Wait(&s_hh, 10U);
    const uint32_t cy1 = DWT->CYCCNT - c0;
    const uint32_t k1 = HAL_GetTick();
    __set_PRIMASK(pm);
    c0 = DWT->CYCCNT;
    const I2cBm_Status_t s2 = I2C_HAL_Wait(&s_hh, 10U);
    const uint32_t cy2 = DWT->CYCCNT - c0;
    s_hh.State = HAL_I2C_STATE_READY;
    const bool d_ok = hal_deinit_ok(&s_hh);                 /* T14 (m48) */
    (void)I2C_BM_DisableClock(I2C2);
    const uint32_t two_ticks = 2U * ms_cyc(1U);
    /* The control's lower bound (m47): a wait of 10 ticks can end a little after 9 tick
       periods (it starts anywhere in the first one): 8.5 ms of CYCCNT, not 9. */
    const uint32_t lo2 = ms_cyc(17U) / 2U;
    verdict((si == I2C_BM_OK) && (s1 == I2C_BM_ERR_TIMEOUT) && (k1 == k0) && (cy1 >= two_ticks) &&
            (cy1 <= ms_cyc(500U)) && (s2 == I2C_BM_ERR_TIMEOUT) && (cy2 >= lo2) && (cy2 <= ms_cyc(12U)) && d_ok,
            "I2C_HAL_Wait(10 ms) on a busy handle with PRIMASK set (the tick stopped): %u (ERR_TIMEOUT %u) after %lu "
            "cycles (>= 2 tick periods, %lu), HAL_GetTick() %lu -> %lu (unchanged); control with the tick running: %u "
            "after %lu cycles (8.5..12 ms); T14: I2C_HAL_Deinit OK with the reset image %u", (unsigned)s1,
            (unsigned)I2C_BM_ERR_TIMEOUT, (unsigned long)cy1, (unsigned long)two_ticks, (unsigned long)k0,
            (unsigned long)k1, (unsigned)s2, (unsigned long)cy2, (unsigned)d_ok);
}

static void step_f3_nopin(void)
{
    if (!step_begin("F3_NOPIN"))
    {
        return;
    }
    s_hal = true;
    LOG("F3: from here CPU1's configuration and transfers go through i2c_hal (verdicts prefixed HAL)");
    tick_line("F3_NOPIN start");
    f3_t1();
    tick_line("after T1");
    f3_t2();
    tick_line("after T2");
    f3_wait_stall();
    tick_line("F3_NOPIN end");
    s_hal = false;
    if (!host_at("F3_NOPIN end"))
    {
        return;
    }
    step_end();
}

/* The bus tests of F3 on CPU1's I2C4 through i2c_hal: T4 (T5 with taps), T1 with BUSY,
   T6, T7(b) and the Deinit of each (T14). */
static void step_f3_cm7(void)
{
    if (!step_begin("F3_CM7"))
    {
        return;
    }
    if (!delay_ms(2U) || !cm7_pins_af())
    {
        bench_abort("CM7 pins");
        return;
    }
    uint32_t tr = 0U;
    s_hal = true;
    tick_line("F3_CM7 start");
    t4_t5(false);
    tick_line("after T4/T5");
    const bool fm = s_f_main > 400000U;
    const bool fm_on = !s_bench_abort && fm && fmp(false, true);
    if (!s_bench_abort && (!fm || fm_on) && main_init(false, 0U, &tr))
    {
        t1_busy(false);
        t6(false);
        tick_line("after T6");
        t7(false);
        tick_line("after T7(b)");
        deinit_judged(false, "the F3_CM7 tests at the main speed");
    }
    if (fm_on)
    {
        (void)fmp(false, false);
    }
    s_hal = false;
    fmp_judge("F3_CM7");
    (void)cm7_pins_input();
    tick_line("F3_CM7 end");
    if (!host_at("F3_CM7 end"))
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
    step_f1_nopin();
    step_f1_cm4();
    step_f1_cm7();
    step_f1_recover();
    step_f2_nopin();
    step_f2_cm4();
    step_f2_cm7();
    step_f3_nopin();
    step_f3_cm7();
    if (step_begin("DONE"))
    {
        tick_line("DONE");
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
