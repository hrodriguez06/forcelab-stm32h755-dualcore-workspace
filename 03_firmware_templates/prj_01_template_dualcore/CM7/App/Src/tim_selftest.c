/**
  ******************************************************************************
  * @file    tim_selftest.c
  * @brief   03_tim selftest, CPU1 side (preset selftest-tim): the Analog
  *          Discovery 2 bench (F0), the tests of tim_bm (F1), of tim_ll (F2) and
  *          of tim_hal (F3).
  *
  *          Prints Rev and build id of both cores, then runs the bench steps
  *          T0a, TREF_A and T0b(b)(c)(d)(f)(g) with the host
  *          (tools/tim_hw_check.py, which runs T0b(a)(e) on a mock DLL), the
  *          internal F1 tests (T1-T5, T7, T10, T12-T16, B1; CPU2's part through
  *          the SRAM4 block), the F1 bench steps T6_TIM1 (T6, T11, T13a),
  *          T6_TIM15, T8 (T8, T13b) and T9, the internal F2 tests through tim_ll
  *          (T1, T2 with the parity against tim_bm, T5, T10(a)(b), T12, T14, B1) and
  *          the F2 bench steps T6_TIM1_LL, T6_TIM15_LL and T8_LL, the internal F3
  *          tests through tim_hal (T1, T2 with the parity against tim_bm and the HAL's
  *          by-design differences, T5, T14, B1) and the F3 bench steps T6_TIM1_HAL,
  *          T8_HAL and T9_HAL, with HAL_GetTick (TIM6) checked against CYCCNT before,
  *          between and after them, then TREF_B, and prints the global verdict,
  *          CPU2's results included. Plan and meaning of every test:
  *          01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md.
  *
  *          Electrical safety (no series resistors on the bench):
  *            S2  bench_safe(): every wired pin in input, MCU_ACK low, MCO1
  *                restored. At start, on any abort, on a timeout and at the end.
  *            S4  bench_claim(): the only way a wired pin becomes an output or
  *                AF (pin_cfg() is the only caller of the GPIO driver for a
  *                wired pin). The permission comes from the CURRENT step of
  *                k_tst_steps[], which only step_begin() sets.
  *            S5  buschk(): the bus is shown released (both pulls followed) in
  *                the same step, before either side drives it.
  *            S7  every step is announced and waits for the host before it
  *                moves anything; 10 s without the host -> S2 and abort.
  *            S12 in a "bus = AD2" step bench_claim() accepts AF only on PE9
  *                and PE11, in AF1, after reading TIM1 back as inputs with no
  *                output path (tim1_s12_ok()).
  ******************************************************************************
  */

#include "tim_selftest.h"
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "tim_bm.h"
#include "tim_ll.h"
#include "tim_hal.h"
#include "dma_bm.h"
#include "app_log.h"
#include "stm32h7xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG(fmt, ...)       APP_LOG("[tim] " fmt, ##__VA_ARGS__)

#define CYC_PER_MS          400000U
#define SYNC_TIMEOUT_CYC    4000000000UL          /* 10 s at 400 MHz (CYCCNT wraps at 10.7 s) */
#define CM4_TIMEOUT_MS      30000U
#define CM4_READY_MS        5000U

/* ---- Bench wiring (00_gpio README section 8) ------------------------------------- */
#define PE_REQ              2U        /* HOST_REQ, AD2 -> MCU (DIO12) */
#define PE_DIO8             3U        /* DIO8,  AD2 only              */
#define PE_DIO10            4U        /* DIO10, AD2 only              */
#define PE_T15C1            5U        /* DIO9,  MCU only (CPU2)       */
#define PE_T15C2            6U        /* DIO11, MCU only (CPU2)       */
#define PE_ACK              7U        /* MCU_ACK, MCU -> AD2 (DIO13)  */
#define BUS_SHIFT           8U        /* PE8..PE15 = DIO0..DIO7       */
#define BUS_MASK            0xFF00UL
#define PE_CAP1             9U        /* DIO1: TIM1_CH1 (AF1)         */
#define PE_CAP2             11U       /* DIO3: TIM1_CH2 (AF1)         */
#define PA_MCO              8U        /* DIO14, MCU only: MCO1 (AF0)  */
#define PF_SCOPE1           14U
#define PF_SCOPE2           15U

static const char *const k_bus_name[] = { "NONE", "MCU", "AD2" };

static uint32_t  s_step = TST_STEP_NONE;  /* NONE between steps: nothing wired may be driven */
static bool      s_bench_abort;
static uint32_t  s_fail;                  /* firmware-side verdicts that failed */
static uint32_t  s_mco_saved;             /* RCC_CFGR MCO1 fields before TREF   */
static bool      s_mco_changed;
static char      s_line[512];             /* verdict() text; one task, no reentrancy */

/* ---- Small helpers ------------------------------------------------------------------ */

static void delay_cycles(uint32_t n)
{
    const uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < n)
    {
    }
}

static void delay_ms(uint32_t ms)
{
    for (uint32_t i = 0U; i < ms; i++)
    {
        delay_cycles(CYC_PER_MS);
    }
}

/* App_Log cuts a line at 160 characters (app_log.c), so a longer text goes out in
   pieces: the first as it is, the rest after "  ... ", and the suffix on the last. */
#define LINE_PIECE   130U

static void emit(const char *text, const char *suffix)
{
    const size_t n = strlen(text);
    if (n <= LINE_PIECE)
    {
        LOG("%s%s", text, suffix);
        return;
    }
    LOG("%.*s", (int)LINE_PIECE, text);
    for (size_t i = LINE_PIECE; i < n; i += LINE_PIECE)
    {
        const bool last = (i + LINE_PIECE) >= n;
        LOG("  ... %.*s%s", (int)LINE_PIECE, &text[i], last ? suffix : "");
    }
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
    emit(s_line, ok ? " -> PASS" : " -> FAIL");
}

/* LOG for texts that may pass 160 characters. */
static void logw(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logw(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(s_line, sizeof(s_line), fmt, ap);
    va_end(ap);
    emit(s_line, "");
}

static void publish_step(uint32_t idx)
{
    s_step = idx;
    TST_AREA->step = idx;
    __DMB();
}

/* ---- S2: safe state ------------------------------------------------------------------ */

static void mco_restore(void)
{
    if (s_mco_changed)
    {
        MODIFY_REG(RCC->CFGR, RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE, s_mco_saved);
        s_mco_changed = false;
    }
}

/* Every wired pin in input without pull (PE2 with pull-down, so HOST_REQ reads
   0 while the AD2 does not drive it), the AF fields of the wired pins at 0,
   MCU_ACK driven low; PE5/PE6 too, although CPU2 configures them. PE0 (plan
   R4) and PE1 (LD2, board_def.h) are never touched. Raw writes: the safe
   state must not depend on code under test. The cross-core lock is asked
   for, and the writes happen even if it is refused, with this core's
   interrupts masked: safety first. */
static void bench_safe(void)
{
    GpioBm_Section_t s;
    const bool locked = (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK);
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();

    mco_restore();
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOEEN | RCC_AHB4ENR_GPIOFEN;
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

    const uint32_t f2 = (3UL << (PF_SCOPE1 * 2U)) | (3UL << (PF_SCOPE2 * 2U));
    GPIOF->MODER = GPIOF->MODER & ~f2;
    GPIOF->PUPDR = GPIOF->PUPDR & ~f2;

    GPIOA->MODER   = GPIOA->MODER & ~(3UL << (PA_MCO * 2U));
    GPIOA->PUPDR   = GPIOA->PUPDR & ~(3UL << (PA_MCO * 2U));
    GPIOA->AFR[1] &= ~(0xFUL << ((PA_MCO - 8U) * 4U));
    __DSB();

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
    LOG("ABORT step=%s reason=%s", (s_step < k_tst_step_count) ? k_tst_steps[s_step].name : "-", reason);
}

/* ---- S4 + S12: the claim --------------------------------------------------------------- */

/* S12: TIM1 read back before PE9/PE11 may go to AF1 while the AD2 drives. The
   claimed channels are inputs (CCxS != 00), and no TIM1 output path exists:
   CC1NE = CC2NE = CC3NE = 0, CC3E = CC4E = 0, MOE = OSSI = OSSR = 0 (tables
   354 and 359 of RM0399: with MOE = OSSI = 0 the outputs are not driven). An
   unclocked TIM1 reads 0, so CCxS = 00 refuses it. Reads only. */
static bool tim1_s12_ok(uint32_t mask)
{
    const uint32_t ccmr1 = TIM1->CCMR1;
    const uint32_t ccer  = TIM1->CCER;
    const uint32_t bdtr  = TIM1->BDTR;
    if (((mask & (1UL << PE_CAP1)) != 0U) && ((ccmr1 & TIM_CCMR1_CC1S) == 0U))
    {
        return false;
    }
    if (((mask & (1UL << PE_CAP2)) != 0U) && ((ccmr1 & TIM_CCMR1_CC2S) == 0U))
    {
        return false;
    }
    if ((ccer & (TIM_CCER_CC1NE | TIM_CCER_CC2NE | TIM_CCER_CC3NE | TIM_CCER_CC3E | TIM_CCER_CC4E)) != 0U)
    {
        return false;
    }
    return (bdtr & (TIM_BDTR_MOE | TIM_BDTR_OSSI | TIM_BDTR_OSSR)) == 0U;
}

/* All or nothing: every pin of the mask is judged before anything is written. */
static bool bench_claim(const GPIO_TypeDef *port, uint32_t mask, uint32_t mode, uint32_t af)
{
    if ((mask & 0xFFFFUL) == 0U)
    {
        return false;
    }
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if (((mask & (1UL << p)) != 0U) && !tst_line_rule(0U, s_step, port, p, mode, af))
        {
            return false;
        }
    }
    const bool ad2_drives = (s_step < k_tst_step_count) && (k_tst_steps[s_step].bus == (uint8_t)TST_BUS_AD2);
    if ((port == GPIOE) && (mode == GPIO_BM_MODE_AF) && ((mask & BUS_MASK) != 0U) && ad2_drives)
    {
        return tim1_s12_ok(mask);
    }
    return true;
}

/* The only path to the GPIO driver for a pin of this selftest. Inputs need no
   claim (nothing is driven); an output or an AF is claimed first. */
static bool pin_cfg(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af, uint32_t pull, uint32_t level)
{
    GpioBm_Status_t st;
    if ((mode == GPIO_BM_MODE_OUTPUT) || (mode == GPIO_BM_MODE_AF))
    {
        if (!bench_claim(port, 1UL << pin, mode, af))
        {
            bench_abort("claim refused");
            return false;
        }
    }
    if (mode == GPIO_BM_MODE_OUTPUT)
    {
        st = GPIO_BM_ConfigOutput(port, pin, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, pull, level);
    }
    else if (mode == GPIO_BM_MODE_AF)
    {
        st = GPIO_BM_ConfigAlternateEx(port, pin, af, GPIO_BM_OTYPE_PP, pull, GPIO_BM_SPEED_LOW);
    }
    else
    {
        st = GPIO_BM_ConfigInput(port, pin, pull);
    }
    if (st != GPIO_BM_OK)
    {
        LOG("gpio_bm config of pin %lu failed, status %lu", (unsigned long)pin, (unsigned long)st);
        bench_abort("config");
        return false;
    }
    return true;
}

static bool pins_input(GPIO_TypeDef *port, uint32_t mask, uint32_t pull)
{
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if (((mask & (1UL << p)) != 0U) && !pin_cfg(port, p, GPIO_BM_MODE_INPUT, 0U, pull, 0U))
        {
            return false;
        }
    }
    return true;
}

/* ---- Handshake (S7) --------------------------------------------------------------------- */

static void ack(bool high)
{
    GPIOE->BSRR = high ? (1UL << PE_ACK) : (1UL << (PE_ACK + 16U));
    __DSB();
}

static bool wait_req(bool high, uint32_t timeout_cyc)
{
    const uint32_t t0 = DWT->CYCCNT;
    while ((((GPIOE->IDR >> PE_REQ) & 1UL) != 0UL) != high)
    {
        if ((DWT->CYCCNT - t0) >= timeout_cyc)
        {
            return false;
        }
    }
    return true;
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
    bool ok = wait_req(true, SYNC_TIMEOUT_CYC);
    ack(false);
    if (ok)
    {
        ok = wait_req(false, SYNC_TIMEOUT_CYC);
    }
    if (!ok)
    {
        bench_abort("host timeout");
    }
    return ok;
}

/* TREF cross-check bursts (plan R3-1); tick_burst() is defined further down. */
#define TICK_BURST_LONG       16U   /* at the banner and at the verdict  */
#define TICK_BURST_STEP       4U    /* at the start of every step        */
#define TICK_BURST_STEP_MS    50U
#define TICK_BURST_START_MS   20U
static void tick_burst(const char *label, uint32_t n);

static bool step_begin(const char *name)
{
    if (s_bench_abort)
    {
        return false;
    }
    const uint32_t idx = tst_step_index(name);
    if (idx == TST_STEP_NONE)
    {
        bench_abort("unknown step");
        return false;
    }
    tick_burst(k_tst_steps[idx].name, TICK_BURST_STEP);   /* x of TREF: one burst per step */
    publish_step(idx);
    LOG("STEP %s bus=%s v=%u", k_tst_steps[idx].name, k_bus_name[k_tst_steps[idx].bus], (unsigned)TIM_BENCH_VERSION);
    return sync0();   /* nothing moves until the host has checked the announcement */
}

static void step_end(void)
{
    publish_step(TST_STEP_NONE);
}

/* ---- S5: the bus is released, shown electrically --------------------------------------- */

static uint32_t bus_idr(void)
{
    return (GPIOE->IDR >> BUS_SHIFT) & 0xFFUL;
}

/* Pull of every bus pin; the pins in `af_mask` stay in AF1 (claimed again). */
static bool bus_pull(uint32_t pull, uint32_t af_mask)
{
    for (uint32_t p = BUS_SHIFT; p < 16U; p++)
    {
        const bool af = (af_mask & (1UL << p)) != 0U;
        if (!pin_cfg(GPIOE, p, af ? GPIO_BM_MODE_AF : GPIO_BM_MODE_INPUT, af ? TST_AF_TIM1 : 0U, pull, 0U))
        {
            return false;
        }
    }
    return true;
}

/* Both MCU pulls, each followed by a host check of the same lines; `af_mask`
   keeps those pins in AF (S12). Aborts if the MCU itself does not read them. */
static bool buschk_af(uint32_t af_mask, uint32_t *up_out, uint32_t *dn_out)
{
    if (!bus_pull(GPIO_BM_PULL_UP, af_mask))
    {
        return false;
    }
    delay_ms(1U);
    const uint32_t up = bus_idr();
    if (!sync0())
    {
        return false;
    }
    if (!bus_pull(GPIO_BM_PULL_DOWN, af_mask))
    {
        return false;
    }
    delay_ms(1U);
    const uint32_t dn = bus_idr();
    if (!sync0())
    {
        return false;
    }
    if (up_out != NULL)
    {
        *up_out = up;
    }
    if (dn_out != NULL)
    {
        *dn_out = dn;
    }
    if ((up != 0xFFU) || (dn != 0x00U))
    {
        LOG("BUSCHK idr up=0x%02lX down=0x%02lX", (unsigned long)up, (unsigned long)dn);
        bench_abort("bus not released");
        return false;
    }
    return true;
}

static bool buschk(void)
{
    if (!buschk_af(0U, NULL, NULL))
    {
        return false;
    }
    return pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
}

/* ==========================================================================================
 * CPU2 commands
 * ========================================================================================== */

static bool cm4_send(TstCmd_t cmd, uint32_t a0)
{
    TstArea_t *a = TST_AREA;
    a->cmd  = (uint32_t)cmd;
    a->arg0 = a0;
    a->arg1 = 0U;
    __DMB();
    const uint32_t seq = a->cmd_seq + 1U;
    a->cmd_seq = seq;
    __DMB();
    const uint32_t t0 = HAL_GetTick();
    while (a->ack_seq != seq)
    {
        if ((HAL_GetTick() - t0) > CM4_TIMEOUT_MS)
        {
            LOG("CM4 did not answer command %lu", (unsigned long)cmd);
            s_fail++;
            return false;
        }
    }
    __DMB();
    return true;
}

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
    { GPIOE, PE_DIO8, 8U }, { GPIOE, PE_T15C1, 9U }, { GPIOE, PE_DIO10, 10U }, { GPIOE, PE_T15C2, 11U },
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
        if (!pin_cfg(k_lines[k].port, k_lines[k].pin, GPIO_BM_MODE_INPUT, 0U, pull, 0U))
        {
            return false;
        }
    }
    return true;
}

/* T0a: every line that is not the handshake follows both MCU pulls (00_gpio
   T0a), then a walking 1 (one line up, the others down: two lines shorted
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
        if (!lines_pull(pattern))
        {
            return;
        }
        delay_ms(1U);
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
    if (!lines_pull(all & ~(1UL << T0A_CTRL_K)))
    {
        return;
    }
    delay_ms(1U);
    const uint32_t ctrl = lines_idr();
    if (!sync0())
    {
        return;
    }
    (void)lines_pull(0U);
    for (uint32_t k = 0U; k < N_LINES; k++)
    {
        (void)pin_cfg(k_lines[k].port, k_lines[k].pin, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U);
    }
    verdict((up == all) && (dn == 0U), "T0A MCU IDR of the 13 lines, pull-up 0x%04lX (0x%04lX), pull-down 0x%04lX (0x0000)",
            (unsigned long)up, (unsigned long)all, (unsigned long)dn);
    verdict(walk_bad == 0U, "T0A walking 1 on 13 lines, MCU IDR: lines that differ 0x%04lX", (unsigned long)walk_bad);
    verdict((ctrl ^ all) == (1UL << T0A_CTRL_K),
            "T0A control: DIO9 (PE5) pulled down on purpose, MCU check flags 0x%04lX (0x%04lX expected)",
            (unsigned long)(ctrl ^ all), (unsigned long)(1UL << T0A_CTRL_K));
    step_end();
}

/* TREF_A / TREF_B (plan R3-1): MCO1 = HSE/8 on PA8 (AF0) for the AD2 to
   measure; the host takes its captures inside one handshake. TREF_A, after
   T0a, also runs the control (R2-1): MCO1 left at HSE/8 in the RCC, PA8 moved
   to AF1 (TIM1_CH1) with TIM1 unclocked in both RCC banks, so MCO1 no longer
   reaches the pin and the AD2 must see no edge (the T10 control of 00_gpio).
   MCO1PRE = 0000 is not a control on this silicon: it passes HSE undivided
   (docs/tim_f0_bench_d01061e.txt). TREF_B, the last step before DONE, has no
   control. Each group saves RCC_CFGR and restores it with read-back; the
   first ms after each change is not shown to the host (RM0399 9.5.4:
   changing MCO1 may glitch). */
static void step_tref(const char *name, bool control)
{
    if (!step_begin(name))
    {
        return;
    }
    const uint32_t cfgr_before = RCC->CFGR;
    s_mco_saved   = cfgr_before & (RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE);
    s_mco_changed = true;
    /* MCO1SEL = 010 (hse_ck), MCO1PRE = 1000 (/8). */
    MODIFY_REG(RCC->CFGR, RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE, RCC_CFGR_MCO1_1 | RCC_CFGR_MCO1PRE_3);
    const uint32_t mco_on = RCC->CFGR & (RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE);
    if (!pin_cfg(GPIOA, PA_MCO, GPIO_BM_MODE_AF, TST_AF_MCO1, GPIO_BM_PULL_NONE, 0U))
    {
        return;
    }
    delay_ms(2U);
    LOG("%s MCO1 = HSE/8 on PA8 AF0, %lu Hz expected (HSE_VALUE/8), RCC_CFGR=0x%08lX",
        name, (unsigned long)(HSE_VALUE / 8U), (unsigned long)RCC->CFGR);
    if (!sync0())          /* host: frequency on DIO14 */
    {
        return;
    }
    uint32_t mco_ctrl = mco_on;
    if (control)
    {
        /* An unclocked TIM1 cannot run: its enable bit in both RCC banks, raw. */
        const uint32_t tim1_c1 = RCC_C1->APB2ENR & RCC_APB2ENR_TIM1EN;
        const uint32_t tim1_c2 = RCC_C2->APB2ENR & RCC_APB2ENR_TIM1EN;
        if (!pin_cfg(GPIOA, PA_MCO, GPIO_BM_MODE_AF, TST_AF_TIM1, GPIO_BM_PULL_NONE, 0U))
        {
            return;
        }
        mco_ctrl = RCC->CFGR & (RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE);
        const uint32_t af_ctrl = (GPIOA->AFR[1] >> ((PA_MCO - 8U) * 4U)) & 0xFUL;
        delay_ms(2U);
        LOG("%s control PA8 AF%lu (TIM1_CH1), TIM1EN C1=%lu C2=%lu, RCC_CFGR=0x%08lX", name,
            (unsigned long)af_ctrl, (unsigned long)tim1_c1, (unsigned long)tim1_c2, (unsigned long)RCC->CFGR);
        if (!sync0())      /* host: no edge on DIO14 */
        {
            return;
        }
        verdict((af_ctrl == TST_AF_TIM1) && (tim1_c1 == 0U) && (tim1_c2 == 0U),
                "%s control: PA8 AFR=%lu (TIM1_CH1), TIM1 clock off in both banks (C1=%lu C2=%lu)", name,
                (unsigned long)af_ctrl, (unsigned long)tim1_c1, (unsigned long)tim1_c2);
    }
    if (!pin_cfg(GPIOA, PA_MCO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U))
    {
        return;
    }
    mco_restore();
    const uint32_t cfgr_after = RCC->CFGR;
    verdict((mco_on == (RCC_CFGR_MCO1_1 | RCC_CFGR_MCO1PRE_3)) && (mco_ctrl == mco_on),
            "%s MCO1 fields read back 0x%08lX (HSE/8)%s", name, (unsigned long)mco_on,
            control ? ", unchanged during the control" : "");
    verdict(cfgr_after == cfgr_before, "%s RCC_CFGR restored: before 0x%08lX after 0x%08lX", name,
            (unsigned long)cfgr_before, (unsigned long)cfgr_after);
    step_end();
}

/* Configuration registers of the wired ports: a refused claim writes none. */
typedef struct { uint32_t r[18]; } PortSnap_t;

static void port_snap(PortSnap_t *s)
{
    const GPIO_TypeDef *const ports[3] = { GPIOA, GPIOE, GPIOF };
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

static bool port_snap_eq(const PortSnap_t *a, const PortSnap_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* Bit k = word k of the snapshot differs: GPIOA MODER 0 .. AFR[1] 5, GPIOE 6..11,
   GPIOF 12..17. */
static uint32_t port_snap_diff(const PortSnap_t *a, const PortSnap_t *b)
{
    uint32_t d = 0U;
    for (uint32_t k = 0U; k < 18U; k++)
    {
        if (a->r[k] != b->r[k])
        {
            d |= 1UL << k;
        }
    }
    return d;
}

typedef struct { GPIO_TypeDef *port; uint16_t mask; uint8_t mode; uint8_t af; } Claim_t;

#define O  GPIO_BM_MODE_OUTPUT
#define A  GPIO_BM_MODE_AF
/* T0b(b), CPU1, step T0B (bus = NONE): every case must be refused. */
static const Claim_t k_t0b_refuse[] = {
    { GPIOE, 1U << 0, O, 0U },          { GPIOE, 1U << 1, O, 0U },          /* PE0 (R4); LD2    */
    { GPIOE, 1U << PE_REQ, O, 0U },     { GPIOE, 1U << PE_DIO8, A, 4U },    /* HOST_REQ, BKIN   */
    { GPIOE, 1U << PE_DIO10, A, 4U },                                       /* TIM15_CH1N       */
    { GPIOE, 1U << PE_ACK, O, 0U },     { GPIOE, 1U << PE_ACK, A, 1U },     /* MCU_ACK, ETR     */
    { GPIOE, 1U << 8, A, 1U },          { GPIOE, 1U << 10, A, 1U },         /* CH1N, CH2N       */
    { GPIOE, 1U << 12, A, 1U },         { GPIOE, 1U << 15, A, 1U },         /* CH3N, BKIN       */
    { GPIOE, 1U << PE_CAP1, A, 1U },    { GPIOE, 1U << 13, A, 1U },         /* bus = NONE       */
    { GPIOE, 1U << PE_CAP1, O, 0U },    { GPIOE, 1U << PE_T15C1, A, 4U },   /* bus; CPU2's pin  */
    { GPIOA, 1U << PA_MCO, A, 1U },     { GPIOA, 1U << PA_MCO, O, 0U },     /* TIM1_CH1; output */
    { GPIOF, 1U << PF_SCOPE1, O, 0U },  { GPIOC, 1U << 13, O, 0U },         /* scope; PC13      */
    { GPIOA, (1U << PA_MCO) | (1U << 13), A, 0U },                          /* mixed with SWD   */
};
#undef O
#undef A

/* T0b(b)(c)(d): the claim refuses what it must on both cores, the host's
   released-bus check catches a driven bus, a missing host sends the MCU to S2. */
static void step_t0b(void)
{
    TstArea_t *a = TST_AREA;
    if (!step_begin("T0B"))
    {
        return;
    }
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    uint32_t wrongly_accepted = 0U;
    for (uint32_t i = 0U; i < (sizeof(k_t0b_refuse) / sizeof(k_t0b_refuse[0])); i++)
    {
        const Claim_t *c = &k_t0b_refuse[i];
        if (bench_claim(c->port, c->mask, c->mode, c->af))
        {
            wrongly_accepted |= 1UL << i;
        }
    }
    const bool accepted = bench_claim(GPIOA, 1UL << PA_MCO, GPIO_BM_MODE_AF, TST_AF_MCO1);   /* the control */
    port_snap(&after);
    verdict((wrongly_accepted == 0U) && accepted && port_snap_eq(&before, &after),
            "T0B(b) CM7 claim: %u refusals, accepted by mistake 0x%08lX; PA8 AF0 accepted (control) %u; regs untouched %u",
            (unsigned)(sizeof(k_t0b_refuse) / sizeof(k_t0b_refuse[0])), (unsigned long)wrongly_accepted,
            (unsigned)accepted, (unsigned)port_snap_eq(&before, &after));
    /* Sensitivity of the snapshot (audit O1): a valid write must change exactly the words
       it writes. PA8 input -> AF0, no pull, low speed, push-pull: only GPIOA MODER. */
    {
        PortSnap_t s0;
        PortSnap_t s1;
        port_snap(&s0);
        /* input -> AF: MODER always; AFR[1] too when PA8's AF field is not already 0 (the
           TREF_A control leaves it at AF1: an input keeps its AF field) */
        const bool afr_set = ((s0.r[5] >> ((PA_MCO - 8U) * 4U)) & 0xFUL) != 0U;
        const uint32_t exp = 0x00001U | (afr_set ? 0x00020U : 0U);
        const bool wrote = pin_cfg(GPIOA, PA_MCO, GPIO_BM_MODE_AF, TST_AF_MCO1, GPIO_BM_PULL_NONE, 0U);
        port_snap(&s1);
        (void)pin_cfg(GPIOA, PA_MCO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U);
        const uint32_t d = port_snap_diff(&s0, &s1);
        verdict(wrote && (d == exp), "T0B(b) CM7 control (O1): PA8 to AF0 changes snapshot words 0x%05lX (0x%05lX: "
                "GPIOA MODER%s)", (unsigned long)d, (unsigned long)exp, afr_set ? ", AFR[1]" : " only");
    }

    if (cm4_send(TST_CMD_CLAIMS, 0U))
    {
        const TstClaimResult_t *r = &a->m4_claims[0];
        verdict((r->cases == TST_M4_CLAIM_CASES) && (r->accepted == TST_M4_CLAIMS_EXPECT_T0B) &&
                (r->regs_changed == 0U) && (r->step_seen == s_step),
                "T0B(b) CM4 claim in T0B: %lu cases, accepted 0x%04lX (0x%04lX: PE5 AF4 only), regs changed 0x%03lX",
                (unsigned long)r->cases, (unsigned long)r->accepted, (unsigned long)TST_M4_CLAIMS_EXPECT_T0B,
                (unsigned long)r->regs_changed);
    }
    if (cm4_send(TST_CMD_SAFE, 0U))
    {
        const TstSafeResult_t *r = &a->m4_safe;
        verdict((r->claim_ok == 1U) && (r->af_moder == 2U) && (r->af_afr == TST_AF_TIM15) && (r->safe_moder == 0U) &&
                (r->safe_afr == 0U) && (r->safe_pupdr == 0U) && (r->pe6_moder == 0U),
                "T0B(b) CM4 bench_safe: PE5 AF4 MODER=%lu AFR=%lu, after safe MODER=%lu AFR=%lu PUPDR=%lu, PE6 MODER=%lu",
                (unsigned long)r->af_moder, (unsigned long)r->af_afr, (unsigned long)r->safe_moder,
                (unsigned long)r->safe_afr, (unsigned long)r->safe_pupdr, (unsigned long)r->pe6_moder);
        verdict(r->ctrl_changed == TST_M4_SAFE_CHANGED, "T0B(b) CM4 control (O1): PE5 to AF4 changes snapshot words "
                "0x%03lX (0x%03lX: GPIOE MODER, AFR[0])", (unsigned long)r->ctrl_changed,
                (unsigned long)TST_M4_SAFE_CHANGED);
    }
    if (!sync0())
    {
        return;
    }
    step_end();
    if (cm4_send(TST_CMD_CLAIMS, 1U))
    {
        const TstClaimResult_t *r = &a->m4_claims[1];
        verdict((r->cases == TST_M4_CLAIM_CASES) && (r->accepted == TST_M4_CLAIMS_EXPECT_NONE) &&
                (r->regs_changed == 0U) && (r->step_seen == TST_STEP_NONE),
                "T0B(b) CM4 claim between steps: accepted 0x%04lX (0x0000), regs changed 0x%03lX",
                (unsigned long)r->accepted, (unsigned long)r->regs_changed);
    }

    /* (c): the MCU drives 0x5A on purpose while the AD2 only listens. The host
       runs its normal released-bus check, which must fail here. */
    if (!step_begin("T0B_S5NEG") || !buschk())   /* S5: released first, then driven */
    {
        return;
    }
    delay_ms(1U);
    for (uint32_t p = BUS_SHIFT; p < 16U; p++)
    {
        if (!pin_cfg(GPIOE, p, GPIO_BM_MODE_OUTPUT, 0U, GPIO_BM_PULL_NONE, (0x5AUL >> (p - BUS_SHIFT)) & 1UL))
        {
            return;
        }
    }
    const uint32_t driven = bus_idr();
    (void)sync0();       /* host: released-bus check, pull-up phase   */
    (void)sync0();       /* host: released-bus check, pull-down phase */
    (void)pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
    uint32_t moder_bus = GPIOE->MODER >> (BUS_SHIFT * 2U);
    (void)buschk();      /* and now it really is released */
    verdict((driven == 0x5AU) && (moder_bus == 0U) && !s_bench_abort,
            "T0B(c) bus driven to 0x%02lX during the host check, then released (MODER bus 0x%04lX)",
            (unsigned long)driven, (unsigned long)moder_bus);
    step_end();

    /* (d): the host does not answer; the MCU must time out into S2. */
    if (!step_begin("T0B_TIMEOUT"))
    {
        return;
    }
    ack(true);
    const bool answered = wait_req(true, SYNC_TIMEOUT_CYC);
    bench_safe();
    const uint32_t e_moder = GPIOE->MODER & 0xFFFFFFF0UL;         /* PE2..PE15 */
    const uint32_t expect  = 1UL << (PE_ACK * 2U);                /* only MCU_ACK is an output */
    const bool safe = (e_moder == expect) &&
                      (((GPIOF->MODER >> (PF_SCOPE1 * 2U)) & 0xFUL) == 0U) &&
                      (((GPIOA->MODER >> (PA_MCO * 2U)) & 3UL) == 0U) &&
                      (((GPIOE->ODR >> PE_ACK) & 1UL) == 0U) && (GPIOE->AFR[1] == 0U);
    verdict(!answered && safe, "T0B(d) no HOST_REQ in 10 s -> bench_safe: GPIOE MODER[31:4]=0x%08lX (0x%08lX), AFRH=0x%08lX",
            (unsigned long)e_moder, (unsigned long)expect, (unsigned long)GPIOE->AFR[1]);
    LOG("T0B timeout handled");
    step_end();
}

/* T0b(f): real release of Digital Out. The AD2 runs DIO1 (PE9) at 1 MHz and
   stops it with Idle = Zet: the bus must then follow both MCU pulls. Control:
   the same with Idle = Low, so DIO1 stays driven low while the MCU pulls up;
   the host's S5 check must fail on DIO1. The MCU only ever has inputs here (no
   claim in this step: k_tst_steps), so there is no contention. */
static void step_t0b_dout(void)
{
    if (!step_begin("T0B_DOUT") || !buschk())
    {
        return;
    }
    uint32_t v[4];
    for (uint32_t run = 0U; run < 2U; run++)       /* 0: Idle = Zet, 1: Idle = Low (control) */
    {
        if (!pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_DOWN) || !sync0())   /* host: Digital Out 1 MHz, stop */
        {
            return;
        }
        if (!pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_UP))
        {
            return;
        }
        delay_ms(1U);
        v[run * 2U] = bus_idr();
        if (!sync0() || !pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_DOWN))   /* host: S5 check, pull-up */
        {
            return;
        }
        delay_ms(1U);
        v[run * 2U + 1U] = bus_idr();
        if (!sync0())                                                      /* host: S5 check, pull-down */
        {
            return;
        }
    }
    if (!pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE) || !sync0())       /* host: S2 */
    {
        return;
    }
    const bool released = buschk();                                       /* after S2: released again */
    verdict((v[0] == 0xFFU) && (v[1] == 0x00U), "T0B(f) Idle=Zet after 1 MHz on DIO1: MCU IDR bus up 0x%02lX down 0x%02lX",
            (unsigned long)v[0], (unsigned long)v[1]);
    verdict((v[2] == 0xFDU) && (v[3] == 0x00U),
            "T0B(f) control Idle=Low: MCU IDR bus up 0x%02lX (0xFD: PE9 held low) down 0x%02lX",
            (unsigned long)v[2], (unsigned long)v[3]);
    verdict(released, "T0B(f) after the host's S2 the bus follows both pulls again");
    step_end();
}

/* TIM1 registers S12 reads, for the logs and the "nothing written" check. */
typedef struct { uint32_t cr1, ccmr1, ccer, bdtr; } Tim1Snap_t;

static void tim1_snap(Tim1Snap_t *s)
{
    s->cr1   = TIM1->CR1;
    s->ccmr1 = TIM1->CCMR1;
    s->ccer  = TIM1->CCER;
    s->bdtr  = TIM1->BDTR;
}

/* One S12 case: TIM1 set up raw (the selftest, not a driver: F0 has none), then
   the claim. CEN stays 0 and CCER never enables an output: these settings are
   never seen by a pin, because a refused claim writes nothing. */
static bool s12_case(uint32_t ccmr1, uint32_t ccer, uint32_t bdtr, uint32_t mask, uint32_t af)
{
    TIM1->CCER  = 0U;
    TIM1->CCMR1 = ccmr1;
    TIM1->BDTR  = bdtr;
    TIM1->CCER  = ccer;
    __DSB();
    return bench_claim(GPIOE, mask, GPIO_BM_MODE_AF, af);
}

typedef struct { uint32_t ccmr1, ccer, bdtr, mask, af; const char *what; } S12Case_t;

#define S12_CAP   ((1UL << PE_CAP1) | (1UL << PE_CAP2))
#define S12_IN2   (TIM_CCMR1_CC1S_0 | TIM_CCMR1_CC2S_0)     /* CC1S = CC2S = 01: TI1, TI2 */

/* T0b(g): every case must be refused (the TIM1 values are set raw, CEN = 0). */
static const S12Case_t k_s12_cases[] = {
    { 0U,               0U,             0U,            S12_CAP,        1U, "CC1S=CC2S=00 (outputs)" },
    { TIM_CCMR1_CC2S_0, 0U,             0U,            1UL << PE_CAP1, 1U, "CC1S=00" },
    { TIM_CCMR1_CC1S_0, 0U,             0U,            1UL << PE_CAP2, 1U, "CC2S=00" },
    { S12_IN2,          0U,             TIM_BDTR_MOE,  S12_CAP,        1U, "MOE=1" },
    { S12_IN2,          0U,             TIM_BDTR_OSSI, S12_CAP,        1U, "OSSI=1" },
    { S12_IN2,          0U,             TIM_BDTR_OSSR, S12_CAP,        1U, "OSSR=1" },
    { S12_IN2,          TIM_CCER_CC1NE, 0U,            S12_CAP,        1U, "CC1NE=1" },
    { S12_IN2,          TIM_CCER_CC2NE, 0U,            S12_CAP,        1U, "CC2NE=1" },
    { S12_IN2,          TIM_CCER_CC3E,  0U,            S12_CAP,        1U, "CC3E=1" },
    { S12_IN2,          TIM_CCER_CC4E,  0U,            S12_CAP,        1U, "CC4E=1" },
    { S12_IN2,          0U,             0U,            S12_CAP,        2U, "AF2" },
    { S12_IN2,          0U,             0U,            1UL << 13,      1U, "PE13" },
    { S12_IN2,          0U,             0U,            1UL << 8,       1U, "PE8 (CH1N)" },
};

/* T0b(g): S12. In a "bus = AD2" step, PE9/PE11 go to AF1 only with TIM1 read
   back as inputs with no output path; then the S5 check runs with the pins in
   AF (RM0399 12.3.12: the pulls stay active and IDR is sampled in AF). */
static void step_t0b_s12(void)
{
    const uint32_t cap = S12_CAP;
    const uint32_t in2 = S12_IN2;
    if (!step_begin("T0B_S12") || !buschk())
    {
        return;
    }
    const bool clk_was_on = (RCC->APB2ENR & RCC_APB2ENR_TIM1EN) != 0U;
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;      /* CPU1's bank (RM0399 9.5.10) */
    (void)RCC->APB2ENR;
    TIM1->CR1 = 0U;

    PortSnap_t g0;
    PortSnap_t g1;
    port_snap(&g0);
    uint32_t wrongly_accepted = 0U;
    for (uint32_t i = 0U; i < (sizeof(k_s12_cases) / sizeof(k_s12_cases[0])); i++)
    {
        if (s12_case(k_s12_cases[i].ccmr1, k_s12_cases[i].ccer, k_s12_cases[i].bdtr, k_s12_cases[i].mask, k_s12_cases[i].af))
        {
            wrongly_accepted |= 1UL << i;
            LOG("T0B(g) case %lu (%s) accepted", (unsigned long)i, k_s12_cases[i].what);
        }
    }
    port_snap(&g1);
    const bool untouched = port_snap_eq(&g0, &g1);
    LOG("T0B(g) cases: CCxS=00 x3, MOE, OSSI, OSSR, CC1NE, CC2NE, CC3E, CC4E, AF2, PE13 AF1, PE8 AF1");
    verdict((wrongly_accepted == 0U) && untouched,
            "T0B(g) S12 claim: %u cases refused, accepted by mistake 0x%04lX, GPIO regs untouched %u",
            (unsigned)(sizeof(k_s12_cases) / sizeof(k_s12_cases[0])), (unsigned long)wrongly_accepted, (unsigned)untouched);

    /* The control: the right configuration is accepted, so the guard tells them apart. */
    Tim1Snap_t t;
    const bool ok_claim = s12_case(in2, 0U, 0U, cap, 1U);
    tim1_snap(&t);
    LOG("T0B(g) TIM1 CR1=0x%04lX CCMR1=0x%08lX CCER=0x%04lX BDTR=0x%08lX", (unsigned long)t.cr1,
        (unsigned long)t.ccmr1, (unsigned long)t.ccer, (unsigned long)t.bdtr);
    verdict(ok_claim, "T0B(g) control: CC1S=CC2S=01, CCER=0, BDTR=0 -> claim PE9|PE11 AF1 accepted");
    if (!ok_claim)
    {
        bench_abort("S12 control refused");   /* the host expects the S5-in-AF handshakes next */
    }
    uint32_t up = 0U;
    uint32_t dn = 0U;
    bool s5_af = false;
    if (ok_claim && pin_cfg(GPIOE, PE_CAP1, GPIO_BM_MODE_AF, TST_AF_TIM1, GPIO_BM_PULL_NONE, 0U) &&
        pin_cfg(GPIOE, PE_CAP2, GPIO_BM_MODE_AF, TST_AF_TIM1, GPIO_BM_PULL_NONE, 0U))
    {
        LOG("T0B(g) GPIOE MODER=0x%08lX AFRH=0x%08lX (PE9, PE11 in AF1)", (unsigned long)GPIOE->MODER,
            (unsigned long)GPIOE->AFR[1]);
        s5_af = buschk_af(cap, &up, &dn);     /* host: S5 check with PE9/PE11 in AF */
    }
    const uint32_t moder_af = (GPIOE->MODER >> (PE_CAP1 * 2U)) & 3UL;
    (void)pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
    TIM1->CCER  = 0U;
    TIM1->CCMR1 = 0U;
    TIM1->BDTR  = 0U;
    TIM1->CR1   = 0U;
    tim1_snap(&t);
    if (!clk_was_on)
    {
        RCC->APB2ENR &= ~RCC_APB2ENR_TIM1EN;
        (void)RCC->APB2ENR;
    }
    verdict(s5_af && (moder_af == 2U) && (up == 0xFFU) && (dn == 0x00U),
            "T0B(g) S5 with PE9/PE11 in AF1 (MODER=%lu): MCU IDR bus up 0x%02lX down 0x%02lX",
            (unsigned long)moder_af, (unsigned long)up, (unsigned long)dn);
    verdict((t.cr1 | t.ccmr1 | t.ccer | t.bdtr) == 0U,
            "T0B(g) TIM1 back to reset values (CR1 CCMR1 CCER BDTR = 0), clock as before (%u)", (unsigned)clk_was_on);
    (void)buschk();
    step_end();
}

/* ==========================================================================================
 * F1: tim_bm (plan "Plan de pruebas", the F1 rows)
 *
 * No oracle calls the driver to judge the driver: registers are read raw and compared
 * with tables typed by hand from RM0399 (Common/Src/tim_selftest_common.c and below),
 * CYCCNT counts time, IDR samples pins, the AD2 measures the bench lines
 * (tools/tim_hw_check.py) and the host re-does T3's arithmetic. TIM6, the HAL tick, is
 * only ever passed to GetKernelClockHz, GetIRQn and GetDmaRequest, which read; TIM15 is
 * CPU2's and is tested there (TST_CMD_*). Every test ends with Deinit and the T14 check.
 * ========================================================================================== */

#define T_CLK_HZ          200000000UL   /* rcc_timx_ker_ck of prj_01 (table 59): the T5(e) oracle */
#define CYC_PER_TICK      2U            /* CPU1 at 400 MHz, the timers at 200 MHz, one PLL1       */
#define TEST_IRQ_PRIO     6U            /* plan "Selftest": the test ISRs, below TIM6 (0)          */

typedef struct { TIM_TypeDef *tim; const char *name; } TimRef_t;

/* CPU1's instances (plan "Selftest"), TIM6 left out. */
static const TimRef_t k_tims[] = {
    { TIM1, "TIM1" }, { TIM2, "TIM2" }, { TIM3, "TIM3" }, { TIM4, "TIM4" }, { TIM5, "TIM5" },
    { TIM7, "TIM7" }, { TIM8, "TIM8" }, { TIM12, "TIM12" }, { TIM13, "TIM13" }, { TIM14, "TIM14" },
    { TIM16, "TIM16" }, { TIM17, "TIM17" },
};
#define N_TIMS   (sizeof(k_tims) / sizeof(k_tims[0]))

/* Every instance, for the read-only checks (GetCaps, GetIRQn, GetDmaRequest, GetKernelClockHz). */
static const TimRef_t k_all_tims[] = {
    { TIM1, "TIM1" }, { TIM2, "TIM2" }, { TIM3, "TIM3" }, { TIM4, "TIM4" }, { TIM5, "TIM5" },
    { TIM6, "TIM6" }, { TIM7, "TIM7" }, { TIM8, "TIM8" }, { TIM12, "TIM12" }, { TIM13, "TIM13" },
    { TIM14, "TIM14" }, { TIM15, "TIM15" }, { TIM16, "TIM16" }, { TIM17, "TIM17" },
};
#define N_ALL    (sizeof(k_all_tims) / sizeof(k_all_tims[0]))

/* F2: the tier the shared tests go through: k_tst_tier_bm for F1's tests, k_tst_tier_ll
   for F2's. Its tag (" LL") goes into their lines, so a BM line reads as in F1. */
static const TstTier_t *s_tr = &k_tst_tier_bm;

/* The bench step of the current tier: "T8", "T8_LL", "T8_HAL". One buffer: each step
   asks once, at its start. */
static const char *tier_step(const char *bm_name)
{
    static char buf[24];
    if (s_tr == &k_tst_tier_bm)
    {
        return bm_name;
    }
    (void)snprintf(buf, sizeof(buf), "%s_%s", bm_name, s_tr->name);
    return buf;
}

static const char *tim_name(const TIM_TypeDef *t)
{
    for (uint32_t i = 0U; i < N_ALL; i++)
    {
        if (k_all_tims[i].tim == t)
        {
            return k_all_tims[i].name;
        }
    }
    return "?";
}

/* RCC enable bit of an instance in a core's bank, from the CMSIS names (RM0399 9.7.37 and
   9.7.40); *apb2 tells APB1LENR from APB2ENR. The oracle of T16 and of the clock restore. */
static uint32_t rcc_en_bit(const TIM_TypeDef *t, bool *apb2)
{
    *apb2 = (t == TIM1) || (t == TIM8) || (t == TIM15) || (t == TIM16) || (t == TIM17);
    if (t == TIM1)  { return RCC_APB2ENR_TIM1EN; }
    if (t == TIM8)  { return RCC_APB2ENR_TIM8EN; }
    if (t == TIM15) { return RCC_APB2ENR_TIM15EN; }
    if (t == TIM16) { return RCC_APB2ENR_TIM16EN; }
    if (t == TIM17) { return RCC_APB2ENR_TIM17EN; }
    if (t == TIM2)  { return RCC_APB1LENR_TIM2EN; }
    if (t == TIM3)  { return RCC_APB1LENR_TIM3EN; }
    if (t == TIM4)  { return RCC_APB1LENR_TIM4EN; }
    if (t == TIM5)  { return RCC_APB1LENR_TIM5EN; }
    if (t == TIM7)  { return RCC_APB1LENR_TIM7EN; }
    if (t == TIM12) { return RCC_APB1LENR_TIM12EN; }
    if (t == TIM13) { return RCC_APB1LENR_TIM13EN; }
    if (t == TIM14) { return RCC_APB1LENR_TIM14EN; }
    return 0U;
}

/* ---- T14: Deinit, then the reset image of RM0399 read raw ----------------------------- */

static uint32_t s_t14_n;
static uint32_t s_t14_bad;

static bool t14(TIM_TypeDef *t, const char *after)
{
    uint32_t idx = 0U;
    uint32_t got = 0U;
    uint32_t exp = 0U;
    const TimBm_Status_t st = s_tr->deinit(t);
    const bool ok = (st == TIM_BM_OK) && tst_reset_ok(t, &idx, &got, &exp);
    s_t14_n++;
    if (!ok)
    {
        s_t14_bad++;
        logw("T14%s %s after %s: Deinit status %u, offset 0x%02lX reads 0x%08lX (reset 0x%08lX)", s_tr->tag, tim_name(t), after,
            (unsigned)st, (unsigned long)(idx * 4U), (unsigned long)got, (unsigned long)exp);
    }
    return ok;
}

/* The F1 tests turn on the clock of every CPU1 instance (CPU1's bank) and give back the
   bank as it was. */
static uint32_t s_apb1l_saved;
static uint32_t s_apb2_saved;

static void f1_clocks_on(void)
{
    s_apb1l_saved = RCC_C1->APB1LENR;
    s_apb2_saved  = RCC_C1->APB2ENR;
    for (uint32_t i = 0U; i < N_TIMS; i++)
    {
        if (TIM_BM_EnableClock(k_tims[i].tim) != TIM_BM_OK)
        {
            LOG("F1 EnableClock(%s) failed", k_tims[i].name);
            s_fail++;
        }
    }
}

static void f1_clocks_restore(void)
{
    for (uint32_t i = 0U; i < N_TIMS; i++)
    {
        bool apb2 = false;
        const uint32_t bit = rcc_en_bit(k_tims[i].tim, &apb2);
        const uint32_t was = apb2 ? s_apb2_saved : s_apb1l_saved;
        (void)TIM_BM_Deinit(k_tims[i].tim);
        if ((was & bit) == 0U)
        {
            (void)TIM_BM_DisableClock(k_tims[i].tim);
        }
    }
    verdict(((RCC_C1->APB1LENR ^ s_apb1l_saved) & 0x1FFU) == 0U && ((RCC_C1->APB2ENR ^ s_apb2_saved) & 0x70003U) == 0U,
            "F1 CPU1 timer clocks given back: APB1LENR 0x%08lX (before 0x%08lX), APB2ENR 0x%08lX (before 0x%08lX)",
            (unsigned long)RCC_C1->APB1LENR, (unsigned long)s_apb1l_saved, (unsigned long)RCC_C1->APB2ENR,
            (unsigned long)s_apb2_saved);
}

/* ==========================================================================================
 * T3: the pure functions, printed for the host to recompute (plan "Reglas de calculo")
 * ========================================================================================== */

static uint32_t s_lcg = 0x5EED0003UL;

static uint32_t lcg(void)
{
    s_lcg = (s_lcg * 1664525UL) + 1013904223UL;
    return s_lcg;
}

static uint32_t s_t3_worst;      /* B1: worst CalcTimeBase, cycles */
static uint32_t s_t3_ct;
static uint32_t s_t3_dc;
static uint32_t s_t3_kc;

static void t3_ct(uint32_t clk, uint32_t target, uint32_t arr_max)
{
    TimBm_TimeBase_t tb = { 0U, 0U, 0U, 0 };
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    const uint32_t c0 = DWT->CYCCNT;
    const TimBm_Status_t st = TIM_BM_CalcTimeBase(clk, target, arr_max, &tb);
    const uint32_t cyc = DWT->CYCCNT - c0;
    __set_PRIMASK(pm);
    if (cyc > s_t3_worst)
    {
        s_t3_worst = cyc;
    }
    LOG("T3 CT %lu %lu %lu %lu %u %lu %lu 0x%08lX%08lX %ld %lu", (unsigned long)s_t3_ct, (unsigned long)clk,
        (unsigned long)target, (unsigned long)arr_max, (unsigned)st, (unsigned long)tb.psc, (unsigned long)tb.arr,
        (unsigned long)(tb.ticks >> 32), (unsigned long)(tb.ticks & 0xFFFFFFFFULL), (long)tb.error_ppm,
        (unsigned long)cyc);
    s_t3_ct++;
}

static void t3_dc(uint32_t arr, uint32_t bits, uint32_t duty)
{
    uint32_t ccr = 0xFFFFFFFFUL;
    const TimBm_Status_t st = TIM_BM_DutyToCompare(arr, bits, duty, &ccr);
    LOG("T3 DC %lu %lu %lu %lu %u %lu", (unsigned long)s_t3_dc, (unsigned long)arr, (unsigned long)bits,
        (unsigned long)duty, (unsigned)st, (unsigned long)((st == TIM_BM_OK) ? ccr : 0U));
    s_t3_dc++;
}

static void t3_kc(uint32_t hclk, uint32_t d2ppre, uint32_t timpre)
{
    uint32_t hz = 0U;
    const TimBm_Status_t st = TIM_BM_KernelClockFromFields(hclk, d2ppre, timpre, &hz);
    LOG("T3 KC %lu %lu %lu %lu %u %lu", (unsigned long)s_t3_kc, (unsigned long)hclk, (unsigned long)d2ppre,
        (unsigned long)timpre, (unsigned)st, (unsigned long)hz);
    s_t3_kc++;
}

static void test_t3(void)
{
    /* The limits of the plan's T3 row: target = clk/2; the vector that gives psc = 65535
       and the ones just past it (ERR_RANGE); exact divisors; primes; 16- and 32-bit arr_max;
       n < 2; the ERR_PARAM cases. */
    static const uint32_t k_ct[][3] = {
        { 200000000UL, 100000000UL, 0xFFFFUL },     { 200000000UL, 100000001UL, 0xFFFFUL },
        { 200000000UL, 0UL, 0xFFFFUL },             { 200000000UL, 1000UL, 0UL },
        { 131072000UL, 1000UL, 1UL },               { 131072001UL, 1000UL, 1UL },
        { 131072000UL, 999UL, 1UL },                { 4294967295UL, 1UL, 0xFFFFUL },
        { 4294967295UL, 1UL, 0xFFFEUL },            { 200000000UL, 1000000UL, 0xFFFFUL },
        { 200000000UL, 1000UL, 0xFFFFUL },          { 200000000UL, 1UL, 0xFFFFFFFFUL },
        { 200000000UL, 1UL, 0xFFFFUL },             { 1000UL, 400UL, 1UL },
        { 1000UL, 300UL, 1UL },                     { 200000000UL, 7919UL, 0xFFFFUL },
        { 200000000UL, 104729UL, 0xFFFFUL },        { 199999991UL, 3UL, 0xFFFFUL },
        { 199999991UL, 65521UL, 0xFFFFFFFFUL },     { 202600000UL, 10000UL, 0xFFFFUL },
        { 64000000UL, 44100UL, 0xFFFFUL },          { 240000000UL, 48000UL, 0xFFFFUL },
        { 100000000UL, 33333333UL, 0xFFFFUL },      { 3UL, 1UL, 0xFFFFUL },
        { 2UL, 1UL, 1UL },                          { 0UL, 0UL, 1UL },
        { 4294967295UL, 2147483647UL, 0xFFFFFFFFUL }, { 4294967295UL, 2147483647UL, 1UL },
        { 200000000UL, 3UL, 0xFFFFUL },             { 200000000UL, 50000UL, 199UL },
        { 200000000UL, 1000000UL, 199UL },          { 200000000UL, 1000001UL, 199UL },
        { 200000000UL, 999999UL, 199UL },           { 1UL, 1UL, 0xFFFFUL },
    };
    for (uint32_t i = 0U; i < (sizeof(k_ct) / sizeof(k_ct[0])); i++)
    {
        t3_ct(k_ct[i][0], k_ct[i][1], k_ct[i][2]);
    }
    for (uint32_t i = 0U; i < 200U; i++)
    {
        const uint32_t clk = lcg();
        const uint32_t half = clk / 2U;
        uint32_t target = (half == 0U) ? 1U : (1U + (lcg() % half));
        if ((i % 10U) == 9U)
        {
            target = half - (lcg() % 3U);                    /* near clk/2 */
        }
        const uint32_t r = lcg() % 4U;
        const uint32_t arr_max = (r == 0U) ? 0xFFFFUL : ((r == 1U) ? 0xFFFFFFFFUL : ((r == 2U) ? (1U + (lcg() % 0xFFFFU)) : (lcg() | 1U)));
        t3_ct(clk, target, arr_max);
    }

    static const uint32_t k_dc[][3] = {
        { 199U, 16U, 0U },         { 199U, 16U, 10000U },     { 199U, 16U, 5000U },      { 199U, 16U, 2500U },
        { 199U, 16U, 10001U },     { 0xFFFFU, 16U, 10000U },  { 0xFFFEU, 16U, 10000U },  { 0xFFFFFFFFUL, 32U, 10000U },
        { 0xFFFFFFFEUL, 32U, 10000U }, { 0x10000U, 16U, 5000U }, { 100U, 8U, 50U },      { 0U, 16U, 10000U },
        { 0U, 16U, 4999U },        { 0U, 16U, 5000U },        { 1U, 16U, 2500U },        { 0xFFFFU, 16U, 9999U },
        { 0xFFFFFFFFUL, 32U, 9999U }, { 999U, 16U, 1U },      { 999U, 16U, 4U },         { 999U, 16U, 5U },
    };
    for (uint32_t i = 0U; i < (sizeof(k_dc) / sizeof(k_dc[0])); i++)
    {
        t3_dc(k_dc[i][0], k_dc[i][1], k_dc[i][2]);
    }
    for (uint32_t i = 0U; i < 50U; i++)
    {
        const bool w32 = (lcg() & 1U) != 0U;
        const uint32_t arr = w32 ? lcg() : (lcg() & 0xFFFFU);
        t3_dc(arr, w32 ? 32U : 16U, lcg() % 10001U);
    }

    /* The 16 rows of table 59 for two bus clocks, and the two ERR_PARAM cases. */
    for (uint32_t h = 0U; h < 2U; h++)
    {
        const uint32_t hclk = (h == 0U) ? 200000000UL : 199999999UL;
        for (uint32_t timpre = 0U; timpre < 2U; timpre++)
        {
            for (uint32_t d = 0U; d < 8U; d++)
            {
                t3_kc(hclk, d, timpre);
            }
        }
    }
    t3_kc(200000000UL, 8U, 0U);
    t3_kc(200000000UL, 0U, 2U);
    logw("T3 sent: %lu CalcTimeBase, %lu DutyToCompare, %lu KernelClockFromFields vectors (the host recomputes them)",
        (unsigned long)s_t3_ct, (unsigned long)s_t3_dc, (unsigned long)s_t3_kc);
    logw("B1 CM7 CalcTimeBase worst case over the T3 sweep: %lu cycles", (unsigned long)s_t3_worst);
}

/* ==========================================================================================
 * T1: refusals, with the registers and the RCC banks read before and after
 * ========================================================================================== */

#define T1_NT   6U
static TIM_TypeDef *const k_t1_tims[T1_NT] = { TIM1, TIM2, TIM4, TIM7, TIM12, TIM17 };

typedef struct
{
    TstImg_t im[T1_NT];
    uint32_t rcc[6];       /* RCC_C1 APB1LENR, APB1HENR, APB2ENR, RCC_C2 the same */
} T1Snap_t;

static T1Snap_t s_t1_b;
static T1Snap_t s_t1_a;

static void t1_snap(T1Snap_t *s)
{
    for (uint32_t k = 0U; k < T1_NT; k++)
    {
        tst_img_read(k_t1_tims[k], &s->im[k]);
    }
    s->rcc[0] = RCC_C1->APB1LENR;
    s->rcc[1] = RCC_C1->APB1HENR;
    s->rcc[2] = RCC_C1->APB2ENR;
    s->rcc[3] = RCC_C2->APB1LENR;
    s->rcc[4] = RCC_C2->APB1HENR;
    s->rcc[5] = RCC_C2->APB2ENR;
}

/* Changed words: tim[k] bit i = word i of timer k, *rcc bit j = RCC word j. With
   `running`, TIM2's CNT and SR are not compared (it counts on purpose). */
static void t1_diff(uint32_t tim[T1_NT], uint32_t *rcc, bool running)
{
    *rcc = 0U;
    for (uint32_t j = 0U; j < 6U; j++)
    {
        if (s_t1_a.rcc[j] != s_t1_b.rcc[j])
        {
            *rcc |= 1UL << j;
        }
    }
    for (uint32_t k = 0U; k < T1_NT; k++)
    {
        tim[k] = 0U;
        for (uint32_t i = 0U; i < TST_RI_N; i++)
        {
            if (running && (k == 1U) && ((i == TST_RI_CNT) || (i == TST_RI_SR)))
            {
                continue;
            }
            if (s_t1_a.im[k].v[i] != s_t1_b.im[k].v[i])
            {
                tim[k] |= 1UL << i;
            }
        }
    }
}

typedef struct { const char *what; uint8_t expect; uint8_t running; } T1Case_t;

static const T1Case_t k_t1[] = {
    /*  0 */ { "InitTimeBase(NULL)",                                   TIM_BM_ERR_PARAM,       0U },
    /*  1 */ { "InitTimeBase(USART3)",                                 TIM_BM_ERR_PARAM,       0U },
    /*  2 */ { "InitTimeBase(TIM1 + 4)",                               TIM_BM_ERR_PARAM,       0U },
    /*  3 */ { "EnableClock(USART3)",                                  TIM_BM_ERR_PARAM,       0U },
    /*  4 */ { "EnableClock(TIM1 + 4)",                                TIM_BM_ERR_PARAM,       0U },
    /*  5 */ { "ConfigOutput(TIM2 channel 0)",                         TIM_BM_ERR_PARAM,       0U },
    /*  6 */ { "ConfigOutput(TIM2 channel 5)",                         TIM_BM_ERR_PARAM,       0U },
    /*  7 */ { "ConfigOutput(TIM12 channel 3)",                        TIM_BM_ERR_UNSUPPORTED, 0U },
    /*  8 */ { "ConfigOutput(TIM7 channel 1): PWM on TIM7",            TIM_BM_ERR_UNSUPPORTED, 0U },
    /*  9 */ { "InitTimeBase(TIM2 ARR = 0)",                           TIM_BM_ERR_PARAM,       0U },
    /* 10 */ { "InitTimeBase(TIM4 ARR = 0x10000, 16 bits)",            TIM_BM_ERR_PARAM,       0U },
    /* 11 */ { "InitTimeBase(TIM2 PSC = 65536)",                       TIM_BM_ERR_PARAM,       0U },
    /* 12 */ { "InitTimeBase(TIM17 repetition = 0x100, 8-bit RCR)",    TIM_BM_ERR_PARAM,       0U },
    /* 13 */ { "InitTimeBase(TIM12 center 1)",                         TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 14 */ { "ConfigSlave(TIM4 trigger ITR5)",                       TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 15 */ { "ConfigSlave(TIM2 trigger 19: no ETRF value)",          TIM_BM_ERR_PARAM,       0U },
    /* 16 */ { "ConfigSlave(TIM4 trigger 7: ETRF's TS code = ITR7)",   TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 17 */ { "ConfigInput(TIM17 TI1SEL = 0001, reserved)",           TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 18 */ { "2.16.1 ConfigMaster(TIM2 MSM) with OPM, reset+trigger", TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 19 */ { "2.16.1 ConfigSlave(TIM2 reset+trigger) with OPM, MSM", TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 20 */ { "2.16.1 InitTimeBase(TIM2 OPM) with MSM, reset+trigger", TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 21 */ { "ConfigOutput(TIM2 CH1) with CC1E = 1",                 TIM_BM_ERR_BUSY,        0U },
    /* 22 */ { "ConfigInput(TIM2 CH2) with CC2E = 1",                  TIM_BM_ERR_BUSY,        0U },
    /* 23 */ { "InitTimeBase(TIM2) with CEN = 1",                      TIM_BM_ERR_BUSY,        1U },
    /* 24 */ { "ConfigSlave(TIM2 trigger) with SMS = gated",           TIM_BM_ERR_BUSY,        0U },
    /* 25 */ { "DutyToCompare(duty 10001)",                            TIM_BM_ERR_PARAM,       0U },
    /* 26 */ { "DutyToCompare(100 %, ARR 0xFFFF, 16 bits)",            TIM_BM_ERR_RANGE,       0U },
    /* 27 */ { "EnableDmaRequest(TIM4 CC4: no DMAMUX1 request)",       TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 28 */ { "OutputEnable(TIM2 CH1) on an input",                   TIM_BM_ERR_PARAM,       0U },
    /* 29 */ { "CaptureEnable(TIM2 CH1) on an output",                 TIM_BM_ERR_PARAM,       0U },
    /* 30 */ { "EnableIrq(TIM13 TRIGGER)",                             TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 31 */ { "SetPeriod(TIM2, 0)",                                   TIM_BM_ERR_PARAM,       0U },
    /* 32 */ { "ConfigMaster(TIM12 OC3REF > MMS 5)",                   TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 33 */ { "ConfigMaster(TIM13: no TRGO)",                         TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 34 */ { "ConfigPwmInput(TIM13: one channel, no slave)",         TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 35 */ { "ConfigSlave(TIM2 gated on TI1F_ED)",                   TIM_BM_ERR_PARAM,       0U },
    /* 36 */ { "WaitFlags(TIM2, taken NULL)",                          TIM_BM_ERR_PARAM,       0U },
    /* 37 */ { "KernelClockFromFields(D2PPRE = 8)",                    TIM_BM_ERR_PARAM,       0U },
    /* 38 */ { "EnableIrq(TIM2, bit 5 outside TIM_BM_EV_*)",           TIM_BM_ERR_PARAM,       0U },
};
#define T1_CASES   (sizeof(k_t1) / sizeof(k_t1[0]))

static TimBm_TimeBaseConfig_t tb_cfg(TIM_TypeDef *t, uint32_t psc, uint32_t arr, uint32_t mode, bool opm)
{
    TimBm_TimeBaseConfig_t c = { t, psc, arr, mode, 0U, false, opm, false };
    return c;
}

static TimBm_Status_t t1_case(uint32_t k)
{
    TimBm_TimeBaseConfig_t tb = tb_cfg(TIM2, 0U, 999U, TIM_BM_COUNT_UP, false);
    TimBm_OcConfig_t oc = { TIM2, 1U, TIM_BM_OC_PWM1, 100U, false, true };
    TimBm_IcConfig_t ic = { TIM2, 2U, TIM_BM_IC_DIRECT, TIM_BM_EDGE_RISING, 1U, 0U, 0U };
    TimBm_Status_t st = TIM_BM_OK;
    uint32_t taken = 0U;
    uint32_t v = 0U;
#define T1_CALL(expr)  do { t1_snap(&s_t1_b); st = (expr); t1_snap(&s_t1_a); } while (0)
    switch (k)
    {
    case 0:  T1_CALL(TIM_BM_InitTimeBase(NULL)); break;
    case 1:  tb.tim = (TIM_TypeDef *)USART3; T1_CALL(TIM_BM_InitTimeBase(&tb)); break;
    case 2:  tb.tim = (TIM_TypeDef *)(TIM1_BASE + 4U); T1_CALL(TIM_BM_InitTimeBase(&tb)); break;
    case 3:  T1_CALL(TIM_BM_EnableClock((TIM_TypeDef *)USART3)); break;
    case 4:  T1_CALL(TIM_BM_EnableClock((TIM_TypeDef *)(TIM1_BASE + 4U))); break;
    case 5:  oc.channel = 0U; T1_CALL(TIM_BM_ConfigOutput(&oc)); break;
    case 6:  oc.channel = 5U; T1_CALL(TIM_BM_ConfigOutput(&oc)); break;
    case 7:  oc.tim = TIM12; oc.channel = 3U; T1_CALL(TIM_BM_ConfigOutput(&oc)); break;
    case 8:  oc.tim = TIM7; T1_CALL(TIM_BM_ConfigOutput(&oc)); break;
    case 9:  tb.arr = 0U; T1_CALL(TIM_BM_InitTimeBase(&tb)); break;
    case 10: tb.tim = TIM4; tb.arr = 0x10000U; T1_CALL(TIM_BM_InitTimeBase(&tb)); break;
    case 11: tb.psc = 65536U; T1_CALL(TIM_BM_InitTimeBase(&tb)); break;
    case 12: tb.tim = TIM17; tb.repetition = 0x100U; T1_CALL(TIM_BM_InitTimeBase(&tb)); break;
    case 13: tb.tim = TIM12; tb.count_mode = TIM_BM_COUNT_CENTER1; T1_CALL(TIM_BM_InitTimeBase(&tb)); break;
    case 14: T1_CALL(TIM_BM_ConfigSlave(TIM4, TIM_BM_SMS_TRIGGER, TIM_BM_TRG_ITR(5))); break;
    case 15: T1_CALL(TIM_BM_ConfigSlave(TIM2, TIM_BM_SMS_TRIGGER, 19U)); break;
    case 16: T1_CALL(TIM_BM_ConfigSlave(TIM4, TIM_BM_SMS_TRIGGER, 7U)); break;
    case 17:
        ic.tim = TIM17; ic.channel = 1U; ic.tisel = 1U;
        T1_CALL(TIM_BM_ConfigInput(&ic));
        break;
    case 18:
        tb.one_pulse = true;
        (void)TIM_BM_InitTimeBase(&tb);
        (void)TIM_BM_ConfigSlave(TIM2, TIM_BM_SMS_RESET_TRIGGER, TIM_BM_TRG_ITR(0));
        T1_CALL(TIM_BM_ConfigMaster(TIM2, TIM_BM_MMS_RESET, true));
        break;
    case 19:
        tb.one_pulse = true;
        (void)TIM_BM_InitTimeBase(&tb);
        (void)TIM_BM_ConfigMaster(TIM2, TIM_BM_MMS_RESET, true);
        T1_CALL(TIM_BM_ConfigSlave(TIM2, TIM_BM_SMS_RESET_TRIGGER, TIM_BM_TRG_ITR(0)));
        break;
    case 20:
        (void)TIM_BM_InitTimeBase(&tb);
        (void)TIM_BM_ConfigMaster(TIM2, TIM_BM_MMS_RESET, true);
        (void)TIM_BM_ConfigSlave(TIM2, TIM_BM_SMS_RESET_TRIGGER, TIM_BM_TRG_ITR(0));
        tb.one_pulse = true;
        T1_CALL(TIM_BM_InitTimeBase(&tb));
        break;
    case 21:
        (void)TIM_BM_ConfigOutput(&oc);
        (void)TIM_BM_OutputEnable(TIM2, TIM_BM_CH(1));
        T1_CALL(TIM_BM_ConfigOutput(&oc));
        break;
    case 22:
        (void)TIM_BM_ConfigInput(&ic);
        (void)TIM_BM_CaptureEnable(TIM2, TIM_BM_CH(2));
        T1_CALL(TIM_BM_ConfigInput(&ic));
        break;
    case 23:
        (void)TIM_BM_InitTimeBase(&tb);
        (void)TIM_BM_Start(TIM2);
        T1_CALL(TIM_BM_InitTimeBase(&tb));
        break;
    case 24:
        (void)TIM_BM_ConfigSlave(TIM2, TIM_BM_SMS_GATED, TIM_BM_TRG_ITR(0));
        T1_CALL(TIM_BM_ConfigSlave(TIM2, TIM_BM_SMS_TRIGGER, TIM_BM_TRG_ITR(1)));
        break;
    case 25: T1_CALL(TIM_BM_DutyToCompare(199U, 16U, 10001U, &v)); break;
    case 26: T1_CALL(TIM_BM_DutyToCompare(0xFFFFU, 16U, 10000U, &v)); break;
    case 27: T1_CALL(TIM_BM_EnableDmaRequest(TIM4, TIM_BM_EV_CC4)); break;
    case 28:
        ic.channel = 1U;
        (void)TIM_BM_ConfigInput(&ic);
        T1_CALL(TIM_BM_OutputEnable(TIM2, TIM_BM_CH(1)));
        break;
    case 29:
        (void)TIM_BM_ConfigOutput(&oc);
        T1_CALL(TIM_BM_CaptureEnable(TIM2, TIM_BM_CH(1)));
        break;
    case 30: T1_CALL(TIM_BM_EnableIrq(TIM13, TIM_BM_EV_TRIGGER)); break;
    case 31: T1_CALL(TIM_BM_SetPeriod(TIM2, 0U)); break;
    case 32: T1_CALL(TIM_BM_ConfigMaster(TIM12, TIM_BM_MMS_OC3REF, false)); break;
    case 33: T1_CALL(TIM_BM_ConfigMaster(TIM13, TIM_BM_MMS_UPDATE, false)); break;
    case 34: T1_CALL(TIM_BM_ConfigPwmInput(TIM13, 1U, TIM_BM_EDGE_RISING, 1U, 0U, 0U)); break;
    case 35: T1_CALL(TIM_BM_ConfigSlave(TIM2, TIM_BM_SMS_GATED, TIM_BM_TRG_TI1F_ED)); break;
    case 36: T1_CALL(TIM_BM_WaitFlags(TIM2, TIM_BM_EV_UPDATE, 1000U, NULL)); break;
    case 37: T1_CALL(TIM_BM_KernelClockFromFields(200000000UL, 8U, 0U, &v)); break;
    case 38: T1_CALL(TIM_BM_EnableIrq(TIM2, 1UL << 5)); break;
    default: break;
    }
#undef T1_CALL
    (void)taken;
    return st;
}

static void test_t1(void)
{
    uint64_t bad = 0U;
    uint64_t touched = 0U;
    for (uint32_t k = 0U; k < T1_CASES; k++)
    {
        uint32_t tm[T1_NT];
        uint32_t rcc = 0U;
        const TimBm_Status_t st = t1_case(k);
        t1_diff(tm, &rcc, k_t1[k].running != 0U);
        bool same = (rcc == 0U);
        for (uint32_t j = 0U; j < T1_NT; j++)
        {
            same = same && (tm[j] == 0U);
        }
        if (st != (TimBm_Status_t)k_t1[k].expect)
        {
            bad |= 1ULL << k;
        }
        if (!same)
        {
            touched |= 1ULL << k;
        }
        LOG("T1 %lu %s: status %u (expected %u), registers and RCC banks unchanged %u", (unsigned long)k, k_t1[k].what,
            (unsigned)st, (unsigned)k_t1[k].expect, (unsigned)same);
        for (uint32_t j = 0U; j < T1_NT; j++)
        {
            (void)TIM_BM_Deinit(k_t1_tims[j]);
        }
    }
    verdict((bad == 0U) && (touched == 0U),
            "T1 CM7 %u refusals: wrong status 0x%08lX%08lX, snapshot changed 0x%08lX%08lX", (unsigned)T1_CASES,
            (unsigned long)(bad >> 32), (unsigned long)(bad & 0xFFFFFFFFU), (unsigned long)(touched >> 32),
            (unsigned long)(touched & 0xFFFFFFFFU));

    /* Sensitivity (audit O1): valid calls change the snapshot, exactly in the expected
       words, so an unchanged snapshot above means something. */
    uint32_t tm[T1_NT];
    uint32_t rcc = 0U;
    TimBm_OcConfig_t oc = { TIM2, 1U, TIM_BM_OC_PWM1, 100U, false, true };
    t1_snap(&s_t1_b);
    const TimBm_Status_t st1 = TIM_BM_ConfigOutput(&oc);
    t1_snap(&s_t1_a);
    t1_diff(tm, &rcc, false);
    const uint32_t exp_tim2 = (1UL << TST_RI_CCMR1) | (1UL << TST_RI_CCR1);
    const bool others_same = (tm[0] | tm[2] | tm[3] | tm[4] | tm[5] | rcc) == 0U;
    verdict((st1 == TIM_BM_OK) && (tm[1] == exp_tim2) && others_same,
            "T1 control (O1): ConfigOutput(TIM2 CH1 PWM1 CCR 100 preload) status %u changes TIM2 words 0x%07lX "
            "(0x%07lX: CCMR1, CCR1), other timers and RCC 0x%lX (0)", (unsigned)st1, (unsigned long)tm[1],
            (unsigned long)exp_tim2, (unsigned long)(tm[0] | tm[2] | tm[3] | tm[4] | tm[5] | rcc));
    (void)TIM_BM_Deinit(TIM2);

    (void)TIM_BM_DisableClock(TIM13);
    t1_snap(&s_t1_b);
    const TimBm_Status_t st2 = TIM_BM_EnableClock(TIM13);
    t1_snap(&s_t1_a);
    t1_diff(tm, &rcc, false);
    const bool tims_same = (tm[0] | tm[1] | tm[2] | tm[3] | tm[4] | tm[5]) == 0U;
    verdict((st2 == TIM_BM_OK) && (rcc == 0x1U) && tims_same,
            "T1 control (O1): EnableClock(TIM13) status %u changes RCC words 0x%02lX (0x01: RCC_C1 APB1LENR only), "
            "timer words unchanged %u", (unsigned)st2, (unsigned long)rcc, (unsigned)tims_same);
}

/* ==========================================================================================
 * T2: register images against the hand encodings; on CPU1 also against the raw HAL
 * ========================================================================================== */

static TIM_HandleTypeDef s_h;
static uint32_t s_hal_diff[N_TIMS][TST_T2_CLASSES][TST_RI_N];
static uint32_t s_hal_err;
static uint32_t s_hal_cur;

static const char *const k_cls_name[TST_T2_CLASSES] = { "time base", "output compare", "input capture", "master", "slave" };

static void hal_base(TIM_TypeDef *t, uint32_t psc, uint32_t arr, uint32_t mode, uint32_t rep, bool arpe)
{
    static const uint32_t k_cm[5] = { TIM_COUNTERMODE_UP, TIM_COUNTERMODE_DOWN, TIM_COUNTERMODE_CENTERALIGNED1,
                                      TIM_COUNTERMODE_CENTERALIGNED2, TIM_COUNTERMODE_CENTERALIGNED3 };
    memset(&s_h, 0, sizeof(s_h));
    s_h.Instance               = t;
    s_h.Init.Prescaler         = psc;
    s_h.Init.CounterMode       = k_cm[mode];
    s_h.Init.Period            = arr;
    s_h.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_h.Init.RepetitionCounter = rep;
    s_h.Init.AutoReloadPreload = arpe ? TIM_AUTORELOAD_PRELOAD_ENABLE : TIM_AUTORELOAD_PRELOAD_DISABLE;
}

static uint32_t hal_channel(uint32_t ch)
{
    return (ch - 1U) * 4U;     /* TIM_CHANNEL_1..4 = 0x0, 0x4, 0x8, 0xC */
}

/* The same intent through the raw HAL (HAL_TIM_* without the driver), then the bits in
   which its image differs from the driver's. */
static void hal_hook(const TstT2Cfg_t *c, const TstImg_t *bm)
{
    static const uint32_t k_ocm[8] = { TIM_OCMODE_TIMING, TIM_OCMODE_ACTIVE, TIM_OCMODE_INACTIVE, TIM_OCMODE_TOGGLE,
                                       TIM_OCMODE_FORCED_INACTIVE, TIM_OCMODE_FORCED_ACTIVE, TIM_OCMODE_PWM1,
                                       TIM_OCMODE_PWM2 };
    static const uint32_t k_sel[4] = { 0U, TIM_ICSELECTION_DIRECTTI, TIM_ICSELECTION_INDIRECTTI, TIM_ICSELECTION_TRC };
    static const uint32_t k_trg[19] = { TIM_TS_ITR0, TIM_TS_ITR1, TIM_TS_ITR2, TIM_TS_ITR3, TIM_TS_ITR4, TIM_TS_ITR5,
                                        TIM_TS_ITR6, TIM_TS_ITR7, TIM_TS_ITR8, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                                        TIM_TS_TI1F_ED, TIM_TS_TI1FP1, TIM_TS_TI2FP2 };
    TIM_TypeDef *t = k_tims[s_hal_cur].tim;
    TimBm_Caps_t caps;
    (void)tst_hand_caps(t, &caps);
    const uint32_t cmax = (caps.counter_bits == 32U) ? 0xFFFFFFFFUL : 0xFFFFUL;
    HAL_StatusTypeDef hs = HAL_OK;
    (void)TIM_BM_Deinit(t);
    switch (c->cls)
    {
    case TST_T2_TB:
        hal_base(t, c->tb.psc, c->tb.arr, c->tb.count_mode, c->tb.repetition, c->tb.arr_preload);
        hs = c->tb.one_pulse ? HAL_TIM_OnePulse_Init(&s_h, TIM_OPMODE_SINGLE) : HAL_TIM_Base_Init(&s_h);
        if (c->tb.urs_overflow_only)
        {
            __HAL_TIM_URS_ENABLE(&s_h);
        }
        break;
    case TST_T2_OC:
    {
        const bool pwm = c->oc.mode >= TIM_BM_OC_PWM1;
        TIM_OC_InitTypeDef oc = { 0 };
        hal_base(t, 0U, cmax, 0U, 0U, false);
        hs = pwm ? HAL_TIM_PWM_Init(&s_h) : HAL_TIM_OC_Init(&s_h);
        oc.OCMode       = k_ocm[c->oc.mode];
        oc.Pulse        = c->oc.compare;
        oc.OCPolarity   = c->oc.active_low ? TIM_OCPOLARITY_LOW : TIM_OCPOLARITY_HIGH;
        oc.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
        oc.OCFastMode   = TIM_OCFAST_DISABLE;
        oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
        oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
        if (hs == HAL_OK)
        {
            hs = pwm ? HAL_TIM_PWM_ConfigChannel(&s_h, &oc, hal_channel(c->oc.channel))
                     : HAL_TIM_OC_ConfigChannel(&s_h, &oc, hal_channel(c->oc.channel));
        }
        if ((hs == HAL_OK) && c->oc.preload && !pwm)
        {
            __HAL_TIM_ENABLE_OCxPRELOAD(&s_h, hal_channel(c->oc.channel));
        }
        break;
    }
    case TST_T2_IC:
    {
        TIM_IC_InitTypeDef ic = { 0 };
        hal_base(t, 0U, cmax, 0U, 0U, false);
        hs = HAL_TIM_IC_Init(&s_h);
        ic.ICPolarity  = (c->ic.edge == TIM_BM_EDGE_RISING) ? TIM_ICPOLARITY_RISING
                         : ((c->ic.edge == TIM_BM_EDGE_FALLING) ? TIM_ICPOLARITY_FALLING : TIM_ICPOLARITY_BOTHEDGE);
        ic.ICSelection = k_sel[c->ic.selection];
        ic.ICPrescaler = (c->ic.prescaler == 8U) ? TIM_ICPSC_DIV8 : ((c->ic.prescaler == 4U) ? TIM_ICPSC_DIV4
                         : ((c->ic.prescaler == 2U) ? TIM_ICPSC_DIV2 : TIM_ICPSC_DIV1));
        ic.ICFilter    = c->ic.filter;
        if (hs == HAL_OK)
        {
            hs = HAL_TIM_IC_ConfigChannel(&s_h, &ic, hal_channel(c->ic.channel));
        }
        if (hs == HAL_OK)
        {
            hs = HAL_TIMEx_TISelection(&s_h, c->ic.tisel << (8U * (c->ic.channel - 1U)), hal_channel(c->ic.channel));
        }
        break;
    }
    case TST_T2_MS:
    {
        TIM_MasterConfigTypeDef mc = { 0 };
        hal_base(t, 0U, cmax, 0U, 0U, false);
        hs = HAL_TIM_Base_Init(&s_h);
        mc.MasterOutputTrigger  = c->mms << 4;
        mc.MasterOutputTrigger2 = 0U;
        mc.MasterSlaveMode      = c->msm ? TIM_MASTERSLAVEMODE_ENABLE : TIM_MASTERSLAVEMODE_DISABLE;
        if (hs == HAL_OK)
        {
            hs = HAL_TIMEx_MasterConfigSynchronization(&s_h, &mc);
        }
        break;
    }
    default:   /* TST_T2_SL */
    {
        static const uint32_t k_sm[9] = { TIM_SLAVEMODE_DISABLE, 0U, 0U, 0U, TIM_SLAVEMODE_RESET, TIM_SLAVEMODE_GATED,
                                          TIM_SLAVEMODE_TRIGGER, TIM_SLAVEMODE_EXTERNAL1,
                                          TIM_SLAVEMODE_COMBINED_RESETTRIGGER };
        TIM_SlaveConfigTypeDef sc = { 0 };
        hal_base(t, 0U, cmax, 0U, 0U, false);
        hs = HAL_TIM_Base_Init(&s_h);
        sc.SlaveMode        = k_sm[c->sms];
        sc.InputTrigger     = k_trg[c->trg];
        sc.TriggerPolarity  = TIM_TRIGGERPOLARITY_RISING;
        sc.TriggerPrescaler = TIM_TRIGGERPRESCALER_DIV1;
        sc.TriggerFilter    = 0U;
        if (hs == HAL_OK)
        {
            hs = HAL_TIM_SlaveConfigSynchro(&s_h, &sc);
        }
        break;
    }
    }
    if (hs != HAL_OK)
    {
        s_hal_err++;
    }
    static TstImg_t hal;       /* static: the Status_Task stack is 512 words */
    tst_img_read(t, &hal);
    for (uint32_t i = 0U; i < TST_RI_N; i++)
    {
        if ((i != TST_RI_CNT) && (i != TST_RI_SR))
        {
            s_hal_diff[s_hal_cur][c->cls][i] |= hal.v[i] ^ bm->v[i];
        }
    }
}

static const char *const k_ri_name[TST_RI_N] = {
    "CR1", "CR2", "SMCR", "DIER", "SR", "EGR", "CCMR1", "CCMR2", "CCER", "CNT", "PSC", "ARR", "RCR", "CCR1",
    "CCR2", "CCR3", "CCR4", "BDTR", "DCR", "DMAR", "res", "CCMR3", "CCR5", "CCR6", "AF1", "AF2", "TISEL",
};

static void test_t2(void)
{
    /* The capability table of the driver against the one typed in the test (plan rule 2). */
    uint32_t caps_bad = 0U;
    for (uint32_t i = 0U; i < N_ALL; i++)
    {
        TimBm_Caps_t got;
        TimBm_Caps_t exp;
        const TimBm_Status_t st = TIM_BM_GetCaps(k_all_tims[i].tim, &got);
        (void)tst_hand_caps(k_all_tims[i].tim, &exp);
        const bool same = (st == TIM_BM_OK) && (got.counter_bits == exp.counter_bits) && (got.channels == exp.channels) &&
                          (got.itr_mask == exp.itr_mask) && (got.mms_max == exp.mms_max) &&
                          (got.up_down_center == exp.up_down_center) && (got.repetition == exp.repetition) &&
                          (got.bdtr == exp.bdtr) && (got.slave_mode == exp.slave_mode) &&
                          (got.master_mode == exp.master_mode) && (got.tisel_mask[0] == exp.tisel_mask[0]) &&
                          (got.tisel_mask[1] == exp.tisel_mask[1]) && (got.tisel_mask[2] == exp.tisel_mask[2]) &&
                          (got.tisel_mask[3] == exp.tisel_mask[3]);
        if (!same)
        {
            caps_bad |= 1UL << i;
            logw("T2 caps %s: bits %u ch %u itr 0x%03X mms %u udc %u rep %u bdtr %u slave %u master %u tisel %X/%X/%X/%X",
                k_all_tims[i].name, got.counter_bits, got.channels, got.itr_mask, got.mms_max, got.up_down_center,
                got.repetition, got.bdtr, got.slave_mode, got.master_mode, got.tisel_mask[0], got.tisel_mask[1],
                got.tisel_mask[2], got.tisel_mask[3]);
        }
    }
    verdict(caps_bad == 0U, "T2 GetCaps of the 14 instances equals the hand table (RM0399): differing 0x%04lX",
            (unsigned long)caps_bad);

    memset(s_hal_diff, 0, sizeof(s_hal_diff));
    s_hal_err = 0U;
    uint32_t total = 0U;
    uint32_t bad = 0U;
    for (uint32_t i = 0U; i < N_TIMS; i++)
    {
        TstT2Result_t r;
        s_hal_cur = i;
        tst_t2_run(k_tims[i].tim, &k_tst_tier_bm, hal_hook, &r);
        total += r.configs;
        const bool ok = (r.mismatches == 0U) && (r.calls_bad == 0U) && (r.ctrl_detected == 1U) && (r.configs > 0U);
        if (!ok)
        {
            bad |= 1UL << i;
        }
        logw("T2 %s: %lu images (time base %lu, OC %lu, IC %lu, master %lu, slave %lu), mismatches %lu, calls not OK %lu, "
            "altered image flagged %lu%s", k_tims[i].name, (unsigned long)r.configs, (unsigned long)r.per_class[0],
            (unsigned long)r.per_class[1], (unsigned long)r.per_class[2], (unsigned long)r.per_class[3],
            (unsigned long)r.per_class[4], (unsigned long)r.mismatches, (unsigned long)r.calls_bad,
            (unsigned long)r.ctrl_detected, ok ? "" : " <-");
        if (r.mismatches != 0U)
        {
            logw("T2 %s first mismatch: %s image %lu, %s reads 0x%08lX, hand encoding 0x%08lX", k_tims[i].name,
                k_cls_name[r.first_cls], (unsigned long)r.first_cfg, k_ri_name[r.first_idx], (unsigned long)r.first_got,
                (unsigned long)r.first_exp);
        }
        (void)t14(k_tims[i].tim, "T2");
    }
    verdict(bad == 0U, "T2 CM7 register images: %lu configurations on 12 instances, instances failing 0x%04lX "
            "(each control: an expected image altered on purpose is flagged)", (unsigned long)total, (unsigned long)bad);

    /* The raw HAL: every bit in which its image differed from the driver's, per instance,
       class and register (the report explains each from stm32h7xx_hal_tim.c). */
    uint32_t lines = 0U;
    for (uint32_t i = 0U; i < N_TIMS; i++)
    {
        for (uint32_t c = 0U; c < TST_T2_CLASSES; c++)
        {
            for (uint32_t w = 0U; w < TST_RI_N; w++)
            {
                if (s_hal_diff[i][c][w] != 0U)
                {
                    logw("T2 HAL %s %s: %s bits 0x%08lX differ from tim_bm", k_tims[i].name, k_cls_name[c], k_ri_name[w],
                        (unsigned long)s_hal_diff[i][c][w]);
                    lines++;
                }
            }
        }
    }
    logw("T2 HAL raw images: %lu configurations, %lu HAL calls not HAL_OK, %lu differing (instance, class, register) "
        "entries listed above", (unsigned long)total, (unsigned long)s_hal_err, (unsigned long)lines);
}

/* ==========================================================================================
 * T5: time base against CPU1's CYCCNT (2 cycles per timer tick)
 * ========================================================================================== */

/* Updates over >= 100 ms of CYCCNT, from the first to the last one. err: against `ticks`
   timer ticks per update; ctrl: the same measurement against `ctrl_ticks`. */
static bool t5_measure(TIM_TypeDef *t, const TimBm_TimeBaseConfig_t *cfg, uint32_t ticks, uint32_t ctrl_ticks,
                       const char *what)
{
    const TimBm_Status_t st = s_tr->init_tb(cfg);
    uint32_t n = 0U;
    uint32_t first = 0U;
    uint32_t last = 0U;
    (void)s_tr->start(t);
    const uint32_t t0 = DWT->CYCCNT;
    const uint32_t window = 100U * CYC_PER_MS;
    while ((DWT->CYCCNT - t0) < (window + (8U * ticks * CYC_PER_TICK) + CYC_PER_MS))
    {
        if (s_tr->take_flags(t, TIM_BM_EV_UPDATE) != 0U)
        {
            const uint32_t now = DWT->CYCCNT;
            if (n == 0U)
            {
                first = now;
            }
            last = now;
            n++;
            if ((last - first) >= window)
            {
                break;
            }
        }
    }
    (void)s_tr->stop(t);
    bool ok = (st == TIM_BM_OK) && (n >= 3U);
    int32_t err = 0;
    int32_t ctrl = 0;
    if (ok)
    {
        const int64_t span = (int64_t)(uint32_t)(last - first);
        const int64_t exp  = (int64_t)(n - 1U) * ticks * CYC_PER_TICK;
        const int64_t expc = (int64_t)(n - 1U) * ctrl_ticks * CYC_PER_TICK;
        err  = (int32_t)(((span - exp) * 1000000) / exp);
        ctrl = (int32_t)(((span - expc) * 1000000) / expc);
        ok = (err >= -100) && (err <= 100) && ((ctrl < -100) || (ctrl > 100));
    }
    verdict(ok, "T5%s %s %s: %lu updates over %lu cycles, %lu ticks each: %+ld ppm (|e| <= 100); control %lu ticks: "
            "%+ld ppm", s_tr->tag, tim_name(t), what, (unsigned long)n, (unsigned long)(last - first), (unsigned long)ticks, (long)err,
            (unsigned long)ctrl_ticks, (long)ctrl);
    (void)t14(t, "T5");
    return ok;
}

static void test_t5(void)
{
    for (uint32_t i = 0U; i < N_TIMS; i++)
    {
        /* (a) PSC + 1 = 2, ARR + 1 = 500: 1000 ticks; control PSC + 2 */
        const TimBm_TimeBaseConfig_t c = tb_cfg(k_tims[i].tim, 1U, 499U, TIM_BM_COUNT_UP, false);
        (void)t5_measure(k_tims[i].tim, &c, 1000U, 1500U, "(a) up, PSC=1 ARR=499");
    }
    TIM_TypeDef *const rcr_tims[4] = { TIM1, TIM8, TIM16, TIM17 };
    for (uint32_t i = 0U; i < 4U; i++)
    {
        /* (b) RCR = 3: one update every 4 periods */
        TimBm_TimeBaseConfig_t c = tb_cfg(rcr_tims[i], 1U, 499U, TIM_BM_COUNT_UP, false);
        c.repetition = 3U;
        (void)t5_measure(rcr_tims[i], &c, 4000U, 1000U, "(b) RCR=3");
    }
    TIM_TypeDef *const w32[2] = { TIM2, TIM5 };
    for (uint32_t i = 0U; i < 2U; i++)
    {
        /* (c) ARR + 1 = 0x20000: a 16-bit counter would give half the period */
        const TimBm_TimeBaseConfig_t c = tb_cfg(w32[i], 0U, 0x1FFFFU, TIM_BM_COUNT_UP, false);
        (void)t5_measure(w32[i], &c, 0x20000U, 0x10000U, "(c) 32 bits, ARR=0x1FFFF");
    }
    TIM_TypeDef *const dc[2] = { TIM1, TIM2 };
    for (uint32_t i = 0U; i < 2U; i++)
    {
        /* (d) down: (ARR + 1)(PSC + 1); center 1: the counter period is 2 ARR ticks. TIM1
           with RCR = 1 updates once per period; TIM2 (no RCR) at both turns, ARR apart. */
        const TimBm_TimeBaseConfig_t d = tb_cfg(dc[i], 1U, 499U, TIM_BM_COUNT_DOWN, false);
        (void)t5_measure(dc[i], &d, 1000U, 1500U, "(d) down");
        TimBm_TimeBaseConfig_t c = tb_cfg(dc[i], 1U, 500U, TIM_BM_COUNT_CENTER1, false);
        c.repetition = (dc[i] == TIM1) ? 1U : 0U;
        const uint32_t per_update = (dc[i] == TIM1) ? (2U * 500U * 2U) : (500U * 2U);
        const uint32_t edge_like  = (dc[i] == TIM1) ? (2U * 501U * 2U) : (501U * 2U);
        (void)t5_measure(dc[i], &c, per_update, edge_like,
                         (dc[i] == TIM1) ? "(d) center 1, ARR=500, RCR=1: period 2 ARR" : "(d) center 1, ARR=500: update every ARR, period 2 ARR");
    }
    if (s_tr != &k_tst_tier_bm)
    {
        return;    /* (e) GetKernelClockHz takes no LL constant: tim_ll has none */
    }
    /* (e) the kernel clock of every instance from table 59 */
    const uint32_t hclk = HAL_RCC_GetHCLKFreq();
    uint32_t bad = 0U;
    for (uint32_t i = 0U; i < N_ALL; i++)
    {
        uint32_t hz = 0U;
        const TimBm_Status_t st = TIM_BM_GetKernelClockHz(k_all_tims[i].tim, hclk, &hz);
        if ((st != TIM_BM_OK) || (hz != T_CLK_HZ))
        {
            bad |= 1UL << i;
            logw("T5(e) %s: status %u, %lu Hz", k_all_tims[i].name, (unsigned)st, (unsigned long)hz);
        }
    }
    verdict(bad == 0U, "T5(e) GetKernelClockHz(hclk1 = %lu Hz) = %lu Hz on all 14 instances (D2PPRE1=%lu D2PPRE2=%lu "
            "TIMPRE=%lu): differing 0x%04lX", (unsigned long)hclk, (unsigned long)T_CLK_HZ,
            (unsigned long)((RCC->D2CFGR >> 4) & 7U), (unsigned long)((RCC->D2CFGR >> 8) & 7U),
            (unsigned long)((RCC->CFGR >> 15) & 1U), (unsigned long)bad);
}

/* ==========================================================================================
 * T7: PWM on the unwired pins, IDR sampled against CYCCNT
 * ========================================================================================== */

typedef struct { uint32_t rises; uint32_t falls; uint32_t t_rise[16]; uint32_t t_fall[16]; uint32_t max_gap; uint32_t level; } Edges_t;

/* Samples the pin for `cycles`: each sample reads CYCCNT and IDR with PRIMASK set (a pair
   the HAL tick cannot split); between samples interrupts run, and the largest gap is kept. */
static void sample_pin(const GPIO_TypeDef *port, uint32_t pin, uint32_t cycles, Edges_t *e)
{
    memset(e, 0, sizeof(*e));
    uint32_t prev_t = DWT->CYCCNT;
    uint32_t prev = (port->IDR >> pin) & 1UL;
    e->level = prev;
    const uint32_t t0 = prev_t;
    while ((prev_t - t0) < cycles)
    {
        const uint32_t pm = __get_PRIMASK();
        __disable_irq();
        const uint32_t now = DWT->CYCCNT;
        const uint32_t v = (port->IDR >> pin) & 1UL;
        __set_PRIMASK(pm);
        if ((now - prev_t) > e->max_gap)
        {
            e->max_gap = now - prev_t;
        }
        if ((v != 0U) && (prev == 0U) && (e->rises < 16U))
        {
            e->t_rise[e->rises++] = now;
        }
        else if ((v == 0U) && (prev != 0U) && (e->falls < 16U))
        {
            e->t_fall[e->falls++] = now;
        }
        prev = v;
        prev_t = now;
    }
}

static void t7_one(TIM_TypeDef *t, GPIO_TypeDef *port, uint32_t pin, uint32_t af, const char *pname)
{
    Edges_t e;
    if (!pin_cfg(port, pin, GPIO_BM_MODE_AF, af, GPIO_BM_PULL_DOWN, 0U))
    {
        return;
    }
    TimBm_TimeBaseConfig_t c = tb_cfg(t, 199U, 999U, TIM_BM_COUNT_UP, false);   /* 200 MHz / 200 / 1000 = 1 kHz */
    const TimBm_OcConfig_t oc = { t, 1U, TIM_BM_OC_PWM1, 250U, false, true };   /* 25 %                         */
    const bool cfg_ok = (TIM_BM_InitTimeBase(&c) == TIM_BM_OK) && (TIM_BM_ConfigOutput(&oc) == TIM_BM_OK) &&
                        (TIM_BM_OutputEnable(t, TIM_BM_CH(1)) == TIM_BM_OK) && (TIM_BM_Start(t) == TIM_BM_OK);
    sample_pin(port, pin, 12U * CYC_PER_MS, &e);
    /* period: mean of the rise-to-rise intervals; high: mean of rise-to-next-fall */
    uint32_t per_ok = 0U;
    int64_t per_sum = 0;
    int64_t hi_sum = 0;
    for (uint32_t k = 1U; k < e.rises; k++)
    {
        per_sum += (int64_t)(uint32_t)(e.t_rise[k] - e.t_rise[k - 1U]);
        per_ok++;
    }
    uint32_t hi_n = 0U;
    for (uint32_t k = 0U; k < e.rises; k++)
    {
        for (uint32_t f = 0U; f < e.falls; f++)
        {
            const uint32_t d = e.t_fall[f] - e.t_rise[k];
            if ((d > 0U) && (d < 400000U))
            {
                hi_sum += d;
                hi_n++;
                break;
            }
        }
    }
    const int64_t exp_per = 1000LL * 200LL * CYC_PER_TICK;         /* 200000 ticks = 400000 cycles */
    const int32_t per_ppm = (per_ok != 0U) ? (int32_t)(((per_sum / per_ok) - exp_per) * 1000000LL / exp_per) : 0;
    const int32_t duty_x100 = ((per_ok != 0U) && (hi_n != 0U)) ? (int32_t)((hi_sum / hi_n) * 10000LL / (per_sum / per_ok)) : 0;
    const bool ok = cfg_ok && (per_ok >= 10U) && (per_ppm >= -5000) && (per_ppm <= 5000) && (duty_x100 >= 2450) &&
                    (duty_x100 <= 2550);
    (void)TIM_BM_Stop(t);
    (void)TIM_BM_OutputDisable(t, TIM_BM_CH(1));
    /* control: CCxE = 0, the pin follows its pull-down */
    Edges_t z;
    (void)TIM_BM_Start(t);
    sample_pin(port, pin, 12U * CYC_PER_MS, &z);
    (void)TIM_BM_Stop(t);
    const bool ctrl = (z.rises == 0U) && (z.falls == 0U) && (z.level == 0U);
    verdict(ok && ctrl, "T7 %s on %s (1 kHz, 25 %%): %lu periods, period %+ld ppm (|e| <= 5000), duty %ld.%02ld %% "
            "(25 +-0.5), largest sampling gap %lu cycles; control CC1E=0: %lu edges, level %lu", tim_name(t), pname,
            (unsigned long)per_ok, (long)per_ppm, (long)(duty_x100 / 100), (long)(duty_x100 % 100),
            (unsigned long)e.max_gap, (unsigned long)(z.rises + z.falls), (unsigned long)z.level);
    (void)pin_cfg(port, pin, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_DOWN, 0U);
    (void)t14(t, "T7");
}

static void test_t7(void)
{
    t7_one(TIM13, GPIOF, 8U, 9U, "PF8 AF9");
    t7_one(TIM14, GPIOF, 9U, 9U, "PF9 AF9");
    t7_one(TIM16, GPIOF, 6U, 1U, "PF6 AF1");
    t7_one(TIM17, GPIOF, 7U, 1U, "PF7 AF1");
    for (uint32_t p = 6U; p <= 9U; p++)
    {
        (void)pin_cfg(GPIOF, p, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U);
    }
}

/* ==========================================================================================
 * T10: master / slave, the slave's CNT and CCR read raw
 * ========================================================================================== */

/* Counts `n` updates of `t` by polling UIF (the period must leave the loop time), then
   stops it. Bounded by `max_cycles`. */
static uint32_t run_updates(TIM_TypeDef *t, uint32_t n, uint32_t max_cycles)
{
    uint32_t k = 0U;
    const uint32_t t0 = DWT->CYCCNT;
    while ((k < n) && ((DWT->CYCCNT - t0) < max_cycles))
    {
        if (s_tr->take_flags(t, TIM_BM_EV_UPDATE) != 0U)
        {
            k++;
        }
    }
    (void)s_tr->stop(t);
    return k;
}

static void t10a(bool control)
{
    /* TIM3 (MMS = update, period 2000 ticks) -> TIM2 external clock 1 on ITR2 (table 358).
       Control: ITR1 = TIM8, which is stopped. */
    TimBm_TimeBaseConfig_t s = tb_cfg(TIM2, 0U, 0xFFFFFFFFUL, TIM_BM_COUNT_UP, false);
    TimBm_TimeBaseConfig_t m = tb_cfg(TIM3, 0U, 1999U, TIM_BM_COUNT_UP, false);
    bool ok = (s_tr->init_tb(&s) == TIM_BM_OK) && (s_tr->init_tb(&m) == TIM_BM_OK) &&
              (s_tr->config_master(TIM3, TIM_BM_MMS_UPDATE, false) == TIM_BM_OK) &&
              (s_tr->config_slave(TIM2, TIM_BM_SMS_EXT_CLOCK1, TIM_BM_TRG_ITR(control ? 1U : 2U)) == TIM_BM_OK) &&
              (s_tr->start(TIM2) == TIM_BM_OK) && (s_tr->start(TIM3) == TIM_BM_OK);
    const uint32_t k = run_updates(TIM3, 1000U, 100U * CYC_PER_MS);
    const uint32_t cnt = TIM2->CNT;
    (void)s_tr->stop(TIM2);
    if (!control)
    {
        verdict(ok && (k == 1000U) && (cnt == 1000U), "T10%s(a) TIM3 update -> TIM2 external clock 1 (ITR2): %lu TIM3 updates, "
                "TIM2 CNT = %lu (1000 exactly)", s_tr->tag, (unsigned long)k, (unsigned long)cnt);
    }
    else
    {
        verdict(ok && (k == 1000U) && (cnt == 0U), "T10%s(a) control: TIM2 on ITR1 (TIM8, stopped): TIM2 CNT = %lu (0)",
                s_tr->tag, (unsigned long)cnt);
    }
    (void)t14(TIM2, "T10(a)");
    (void)t14(TIM3, "T10(a)");
}

/* Gated: the master's OC1REF (PWM1, CCR high ticks per period) gates `slave`. OC1REF is
   first set by the master's first update (it stays low before it: the first dev run gave
   CNT = (N - 1) CCR), so the N periods are counted from that update, and the master stops
   in the low part of the period after the N-th, with the gate closed. */
static void t10_gated(TIM_TypeDef *master, uint32_t mms, TIM_TypeDef *slave, uint32_t itr, uint32_t n, uint32_t ccr,
                      const char *what, bool control)
{
    TimBm_TimeBaseConfig_t ms = tb_cfg(master, 0U, 999U, TIM_BM_COUNT_UP, false);
    TimBm_TimeBaseConfig_t sl = tb_cfg(slave, 0U, 0xFFFFU, TIM_BM_COUNT_UP, false);
    const TimBm_OcConfig_t oc = { master, 1U, TIM_BM_OC_PWM1, ccr, false, true };
    bool ok = (s_tr->init_tb(&ms) == TIM_BM_OK) && (s_tr->config_output(&oc) == TIM_BM_OK) &&
              (s_tr->init_tb(&sl) == TIM_BM_OK) &&
              (s_tr->config_slave(slave, TIM_BM_SMS_GATED, TIM_BM_TRG_ITR(itr)) == TIM_BM_OK);
    if (mms != 0xFFU)
    {
        ok = ok && (s_tr->config_master(master, mms, false) == TIM_BM_OK);
    }
    else
    {
        /* TIM13/TIM14 have no TRGO: table 361 wires their OC1 to ITR2/ITR3. CC1E on, the
           pin (PF8/PF9) is not in AF, so nothing is driven. */
        ok = ok && (s_tr->output_enable(master, TIM_BM_CH(1)) == TIM_BM_OK);
    }
    ok = ok && (s_tr->start(slave) == TIM_BM_OK) && (s_tr->start(master) == TIM_BM_OK);
    uint32_t k = 0U;
    const uint32_t t0 = DWT->CYCCNT;
    while ((k < n) && ((DWT->CYCCNT - t0) < (50U * CYC_PER_MS)))
    {
        if (s_tr->take_flags(master, TIM_BM_EV_UPDATE) != 0U)
        {
            k++;
        }
    }
    while (((master->CNT) < (ccr + 20U)) && ((DWT->CYCCNT - t0) < (60U * CYC_PER_MS)))
    {
    }
    (void)s_tr->stop(master);
    const uint32_t cnt = slave->CNT;
    (void)s_tr->stop(slave);
    const int32_t diff = (int32_t)cnt - (int32_t)(n * ccr);
    if (!control)
    {
        verdict(ok && (k == n) && (diff >= -(int32_t)n) && (diff <= (int32_t)n),
                "T10%s%s: %lu periods of %lu high ticks, slave CNT = %lu (N CCR = %lu, |diff| <= N)", s_tr->tag, what,
                (unsigned long)n,
                (unsigned long)ccr, (unsigned long)cnt, (unsigned long)(n * ccr));
    }
    else
    {
        verdict(ok && (cnt == 0U), "T10%s%s control: the slave on an ITR whose master is stopped: CNT = %lu (0)",
                s_tr->tag, what, (unsigned long)cnt);
    }
    (void)t14(master, "T10");
    (void)t14(slave, "T10");
}

static void t10c(bool control)
{
    /* TIM1 TRGO (update) -> TIM8 trigger mode on ITR0 (table 353): TIM8 starts by itself.
       Control: ITR3 = TIM5, stopped. */
    TimBm_TimeBaseConfig_t m = tb_cfg(TIM1, 0U, 1999U, TIM_BM_COUNT_UP, false);
    TimBm_TimeBaseConfig_t s = tb_cfg(TIM8, 0U, 0xFFFFU, TIM_BM_COUNT_UP, false);
    bool ok = (TIM_BM_InitTimeBase(&m) == TIM_BM_OK) && (TIM_BM_ConfigMaster(TIM1, TIM_BM_MMS_UPDATE, false) == TIM_BM_OK) &&
              (TIM_BM_InitTimeBase(&s) == TIM_BM_OK) &&
              (TIM_BM_ConfigSlave(TIM8, TIM_BM_SMS_TRIGGER, TIM_BM_TRG_ITR(control ? 3U : 0U)) == TIM_BM_OK);
    const uint32_t cen_before = TIM8->CR1 & TIM_CR1_CEN;
    (void)TIM_BM_Start(TIM1);
    const uint32_t k = run_updates(TIM1, 1U, CYC_PER_MS);
    const uint32_t cen_after = TIM8->CR1 & TIM_CR1_CEN;
    (void)TIM_BM_Stop(TIM8);
    if (!control)
    {
        verdict(ok && (k == 1U) && (cen_before == 0U) && (cen_after != 0U),
                "T10(c) TIM1 TRGO -> TIM8 trigger (ITR0): TIM8 CEN %lu before, %lu after the first TIM1 update",
                (unsigned long)cen_before, (unsigned long)(cen_after != 0U));
    }
    else
    {
        verdict(ok && (cen_after == 0U), "T10(c) control: TIM8 on ITR3 (TIM5, stopped): CEN %lu after (0)",
                (unsigned long)(cen_after != 0U));
    }
    (void)t14(TIM1, "T10(c)");
    (void)t14(TIM8, "T10(c)");
}

/* (d) TRC capture of a master's TRGO: two captures, difference = master period. */
static void t10d_one(TIM_TypeDef *slave, uint32_t itr, TIM_TypeDef *master, bool control)
{
    const uint32_t period = 1000U;
    TimBm_TimeBaseConfig_t m = tb_cfg(master, 0U, period - 1U, TIM_BM_COUNT_UP, false);
    TimBm_TimeBaseConfig_t s = tb_cfg(slave, 0U, (slave == TIM2 || slave == TIM5) ? 0xFFFFFFFFUL : 0xFFFFU,
                                      TIM_BM_COUNT_UP, false);
    const TimBm_IcConfig_t ic = { slave, 1U, TIM_BM_IC_TRC, TIM_BM_EDGE_RISING, 1U, 0U, 0U };
    bool ok = (TIM_BM_InitTimeBase(&s) == TIM_BM_OK) &&
              (TIM_BM_ConfigSlave(slave, TIM_BM_SMS_DISABLED, TIM_BM_TRG_ITR(itr)) == TIM_BM_OK) &&
              (TIM_BM_ConfigInput(&ic) == TIM_BM_OK) && (TIM_BM_CaptureEnable(slave, TIM_BM_CH(1)) == TIM_BM_OK) &&
              (TIM_BM_Start(slave) == TIM_BM_OK);
    if (!control)
    {
        ok = ok && (TIM_BM_InitTimeBase(&m) == TIM_BM_OK) &&
             (TIM_BM_ConfigMaster(master, TIM_BM_MMS_UPDATE, false) == TIM_BM_OK) && (TIM_BM_Start(master) == TIM_BM_OK);
    }
    uint32_t cap[3] = { 0U, 0U, 0U };
    uint32_t n = 0U;
    const uint32_t t0 = DWT->CYCCNT;
    while ((n < 3U) && ((DWT->CYCCNT - t0) < CYC_PER_MS))
    {
        if (TIM_BM_TakeFlags(slave, TIM_BM_EV_CC1) != 0U)
        {
            cap[n++] = TIM_BM_ReadCapture(slave, 1U);
        }
    }
    (void)TIM_BM_Stop(master);
    (void)TIM_BM_Stop(slave);
    const uint32_t mask = (slave == TIM2 || slave == TIM5) ? 0xFFFFFFFFUL : 0xFFFFUL;
    const uint32_t d = (cap[2] - cap[1]) & mask;
    if (!control)
    {
        verdict(ok && (n == 3U) && (d >= (period - 1U)) && (d <= (period + 1U)),
                "T10(d) %s captures TRC = ITR%lu (%s TRGO, period %lu ticks): difference %lu (+-1)", tim_name(slave),
                (unsigned long)itr, tim_name(master), (unsigned long)period, (unsigned long)d);
    }
    else
    {
        verdict(ok && (n == 0U), "T10(d) control: %s on ITR%lu with %s stopped: %lu captures (0)", tim_name(slave),
                (unsigned long)itr, tim_name(master), (unsigned long)n);
    }
    (void)t14(slave, "T10(d)");
    (void)t14(master, "T10(d)");
}

static void test_t10(void)
{
    t10a(false);
    t10a(true);
    /* (b) TIM2 (MMS = OC1REF) -> TIM4 gated on ITR1 (table 358): CNT = N CCR. Control: ITR3 = TIM8, stopped. */
    t10_gated(TIM2, TIM_BM_MMS_OC1REF, TIM4, 1U, 100U, 200U, "(b) TIM2 OC1REF -> TIM4 gated (ITR1)", false);
    t10_gated(TIM2, TIM_BM_MMS_OC1REF, TIM4, 3U, 100U, 200U, "(b)", true);
    t10c(false);
    t10c(true);
    /* (d) every TRC slave of CPU1 with TIM1 as master (ITR0), TIM12 with TIM4 (ITR0). TIM15
       is CPU2's: T10D_ARM/READ. */
    t10d_one(TIM2, 0U, TIM1, false);
    t10d_one(TIM3, 0U, TIM1, false);
    t10d_one(TIM4, 0U, TIM1, false);
    t10d_one(TIM5, 0U, TIM1, false);
    t10d_one(TIM8, 0U, TIM1, false);
    t10d_one(TIM12, 0U, TIM4, false);
    t10d_one(TIM2, 0U, TIM1, true);
    {
        TstArea_t *a = TST_AREA;
        TimBm_TimeBaseConfig_t m = tb_cfg(TIM1, 0U, 999U, TIM_BM_COUNT_UP, false);
        const bool arm = cm4_send(TST_CMD_T10D_ARM, 0U) && (a->m4_res[0] == TIM_BM_OK);
        const bool mst = (TIM_BM_InitTimeBase(&m) == TIM_BM_OK) &&
                         (TIM_BM_ConfigMaster(TIM1, TIM_BM_MMS_UPDATE, false) == TIM_BM_OK) &&
                         (TIM_BM_Start(TIM1) == TIM_BM_OK);
        const bool rd = cm4_send(TST_CMD_T10D_READ, 0U);
        (void)TIM_BM_Stop(TIM1);
        const uint32_t d = (a->m4_res[4] - a->m4_res[3]) & 0xFFFFU;
        verdict(arm && mst && rd && (a->m4_res[1] >= 3U) && (d >= 999U) && (d <= 1001U) && (a->m4_res[6] == 1U),
                "T10(d) TIM15 (CM4) captures TRC = ITR0 (TIM1 TRGO, period 1000 ticks): %lu captures, difference %lu "
                "(+-1), CM4 T14 %lu", (unsigned long)a->m4_res[1], (unsigned long)d, (unsigned long)a->m4_res[6]);
        (void)t14(TIM1, "T10(d) TIM15");
    }
    /* (e) TIM13 / TIM14 OC1REF -> TIM12 gated on ITR2 / ITR3 (table 361). */
    t10_gated(TIM13, 0xFFU, TIM12, 2U, 100U, 200U, "(e) TIM13 OC1 -> TIM12 gated (ITR2)", false);
    t10_gated(TIM14, 0xFFU, TIM12, 3U, 100U, 200U, "(e) TIM14 OC1 -> TIM12 gated (ITR3)", false);
}

/* ==========================================================================================
 * T12: interrupts. The test ISRs: TakeFlags of their own timer, a counter each.
 * ========================================================================================== */

enum { IQ_T1UP, IQ_T1CC, IQ_T2, IQ_T3, IQ_T4, IQ_T5, IQ_T7, IQ_T8UP, IQ_T13, IQ_T12, IQ_T14, IQ_T8CC, IQ_T16, IQ_T17,
       IQ_SPUR, IQ_N };
static volatile uint32_t s_irq[IQ_N];
static volatile uint32_t s_t15_on;    /* TIM7's vector serves T15 instead of T12 */
static void t15_isr(void);

static void count_irq(TIM_TypeDef *t, uint32_t ev, uint32_t idx)
{
    if (s_tr->take_flags(t, ev) != 0U)
    {
        s_irq[idx]++;
    }
    else
    {
        s_irq[IQ_SPUR]++;
    }
}

void TIM1_UP_IRQHandler(void)  { count_irq(TIM1, TIM_BM_EV_UPDATE, IQ_T1UP); }
void TIM1_CC_IRQHandler(void)  { count_irq(TIM1, TIM_BM_EV_CC1, IQ_T1CC); }
void TIM2_IRQHandler(void)     { count_irq(TIM2, TIM_BM_EV_UPDATE, IQ_T2); }
void TIM3_IRQHandler(void)     { count_irq(TIM3, TIM_BM_EV_UPDATE, IQ_T3); }
void TIM4_IRQHandler(void)     { count_irq(TIM4, TIM_BM_EV_UPDATE, IQ_T4); }
void TIM5_IRQHandler(void)     { count_irq(TIM5, TIM_BM_EV_UPDATE, IQ_T5); }
void TIM8_CC_IRQHandler(void)  { count_irq(TIM8, TIM_BM_EV_CC1, IQ_T8CC); }
void TIM16_IRQHandler(void)    { count_irq(TIM16, TIM_BM_EV_UPDATE, IQ_T16); }
void TIM17_IRQHandler(void)    { count_irq(TIM17, TIM_BM_EV_UPDATE, IQ_T17); }

void TIM7_IRQHandler(void)
{
    if (s_t15_on != 0U)
    {
        t15_isr();
    }
    else
    {
        count_irq(TIM7, TIM_BM_EV_UPDATE, IQ_T7);
    }
}

/* Shared vectors: the ISR tells the instances apart with TakeFlags. */
void TIM8_UP_TIM13_IRQHandler(void)
{
    uint32_t n = 0U;
    if (s_tr->take_flags(TIM8, TIM_BM_EV_UPDATE) != 0U)  { s_irq[IQ_T8UP]++; n++; }
    if (s_tr->take_flags(TIM13, TIM_BM_EV_UPDATE) != 0U) { s_irq[IQ_T13]++; n++; }
    if (n == 0U) { s_irq[IQ_SPUR]++; }
}

void TIM8_BRK_TIM12_IRQHandler(void)
{
    if (s_tr->take_flags(TIM12, TIM_BM_EV_UPDATE) != 0U) { s_irq[IQ_T12]++; } else { s_irq[IQ_SPUR]++; }
}

void TIM8_TRG_COM_TIM14_IRQHandler(void)
{
    if (s_tr->take_flags(TIM14, TIM_BM_EV_UPDATE) != 0U) { s_irq[IQ_T14]++; } else { s_irq[IQ_SPUR]++; }
}

/* The vectors, by hand from stm32h755xx.h (the T12(c) oracle). */
typedef struct { TIM_TypeDef *tim; uint32_t ev; IRQn_Type irq; uint32_t idx; } IrqRow_t;
static const IrqRow_t k_irq_rows[] = {
    { TIM1, TIM_BM_EV_UPDATE, TIM1_UP_IRQn, IQ_T1UP },          { TIM1, TIM_BM_EV_CC1, TIM1_CC_IRQn, IQ_T1CC },
    { TIM2, TIM_BM_EV_UPDATE, TIM2_IRQn, IQ_T2 },               { TIM3, TIM_BM_EV_UPDATE, TIM3_IRQn, IQ_T3 },
    { TIM4, TIM_BM_EV_UPDATE, TIM4_IRQn, IQ_T4 },               { TIM5, TIM_BM_EV_UPDATE, TIM5_IRQn, IQ_T5 },
    { TIM7, TIM_BM_EV_UPDATE, TIM7_IRQn, IQ_T7 },               { TIM8, TIM_BM_EV_UPDATE, TIM8_UP_TIM13_IRQn, IQ_T8UP },
    { TIM13, TIM_BM_EV_UPDATE, TIM8_UP_TIM13_IRQn, IQ_T13 },    { TIM12, TIM_BM_EV_UPDATE, TIM8_BRK_TIM12_IRQn, IQ_T12 },
    { TIM14, TIM_BM_EV_UPDATE, TIM8_TRG_COM_TIM14_IRQn, IQ_T14 }, { TIM8, TIM_BM_EV_CC1, TIM8_CC_IRQn, IQ_T8CC },
    { TIM16, TIM_BM_EV_UPDATE, TIM16_IRQn, IQ_T16 },            { TIM17, TIM_BM_EV_UPDATE, TIM17_IRQn, IQ_T17 },
};
#define N_IRQ_ROWS   (sizeof(k_irq_rows) / sizeof(k_irq_rows[0]))

static void irq_on(IRQn_Type irq)
{
    NVIC_SetPriority(irq, TEST_IRQ_PRIO);
    NVIC_ClearPendingIRQ(irq);
    NVIC_EnableIRQ(irq);
}

static void irq_off(IRQn_Type irq)
{
    NVIC_DisableIRQ(irq);
    NVIC_ClearPendingIRQ(irq);
}

/* 10 kHz: PSC + 1 = 200, ARR + 1 = 100. A CC row uses CC1 in frozen mode at CCR1 = 50. */
static bool t12_arm(const IrqRow_t *r)
{
    TimBm_TimeBaseConfig_t c = tb_cfg(r->tim, 199U, 99U, TIM_BM_COUNT_UP, false);
    bool ok = s_tr->init_tb(&c) == TIM_BM_OK;
    if (r->ev == TIM_BM_EV_CC1)
    {
        const TimBm_OcConfig_t oc = { r->tim, 1U, TIM_BM_OC_FROZEN, 50U, false, false };
        ok = ok && (s_tr->config_output(&oc) == TIM_BM_OK);
    }
    return ok && (s_tr->enable_irq(r->tim, r->ev) == TIM_BM_OK);
}

/* Runs the rows of `set` (bit k = row k) together for 100 ms. */
static void t12_group(uint32_t set, const char *what)
{
    bool ok = true;
    for (uint32_t k = 0U; k < IQ_N; k++)
    {
        s_irq[k] = 0U;
    }
    for (uint32_t k = 0U; k < N_IRQ_ROWS; k++)
    {
        if ((set & (1UL << k)) != 0U)
        {
            ok = ok && t12_arm(&k_irq_rows[k]);
            irq_on(k_irq_rows[k].irq);
        }
    }
    for (uint32_t k = 0U; k < N_IRQ_ROWS; k++)
    {
        if ((set & (1UL << k)) != 0U)
        {
            (void)s_tr->start(k_irq_rows[k].tim);
        }
    }
    delay_cycles(100U * CYC_PER_MS);
    for (uint32_t k = 0U; k < N_IRQ_ROWS; k++)
    {
        if ((set & (1UL << k)) != 0U)
        {
            (void)s_tr->stop(k_irq_rows[k].tim);
        }
    }
    for (uint32_t k = 0U; k < N_IRQ_ROWS; k++)
    {
        if ((set & (1UL << k)) != 0U)
        {
            irq_off(k_irq_rows[k].irq);
            (void)s_tr->disable_irq(k_irq_rows[k].tim, k_irq_rows[k].ev);
        }
    }
    /* each counter of the set at 1000 +-1, every other counter at 0 */
    for (uint32_t k = 0U; k < N_IRQ_ROWS; k++)
    {
        const uint32_t n = s_irq[k_irq_rows[k].idx];
        const bool in = (set & (1UL << k)) != 0U;
        if (in ? ((n < 999U) || (n > 1001U)) : (n != 0U))
        {
            ok = false;
        }
    }
    char buf[96];
    int len = 0;
    for (uint32_t k = 0U; (k < N_IRQ_ROWS) && (len < 80); k++)
    {
        if ((set & (1UL << k)) != 0U)
        {
            len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " %s=%lu", tim_name(k_irq_rows[k].tim),
                            (unsigned long)s_irq[k_irq_rows[k].idx]);
        }
    }
    verdict(ok && (s_irq[IQ_SPUR] == 0U), "T12%s(a) %s, 10 kHz for 100 ms:%s (1000 +-1), others 0, ISR calls that took "
            "nothing %lu", s_tr->tag, what, buf, (unsigned long)s_irq[IQ_SPUR]);
    for (uint32_t k = 0U; k < N_IRQ_ROWS; k++)
    {
        if ((set & (1UL << k)) != 0U)
        {
            (void)t14(k_irq_rows[k].tim, "T12");
        }
    }
}

static void test_t12(void)
{
    t12_group(1UL << 0, "TIM1_UP");
    t12_group(1UL << 1, "TIM1_CC (CC1)");
    t12_group(1UL << 2, "TIM2");
    t12_group(1UL << 3, "TIM3");
    t12_group(1UL << 4, "TIM4");
    t12_group(1UL << 5, "TIM5");
    t12_group(1UL << 6, "TIM7");
    t12_group(1UL << 7, "TIM8_UP_TIM13 (TIM8 alone)");
    t12_group(1UL << 8, "TIM8_UP_TIM13 (TIM13 alone)");
    t12_group(1UL << 9, "TIM8_BRK_TIM12 (TIM12 alone)");
    t12_group(1UL << 10, "TIM8_TRG_COM_TIM14 (TIM14 alone)");
    t12_group(1UL << 11, "TIM8_CC (CC1 alone)");
    t12_group((1UL << 7) | (1UL << 8) | (1UL << 9) | (1UL << 10) | (1UL << 11),
              "TIM8 UP/CC with TIM12, TIM13, TIM14 together");
    t12_group(1UL << 12, "TIM16");
    t12_group(1UL << 13, "TIM17");

    /* (b) no spurious IRQ: EnableIrq(UPDATE) right after InitTimeBase, no Start, 10 ms. */
    s_irq[IQ_T2] = 0U;
    s_irq[IQ_SPUR] = 0U;
    TimBm_TimeBaseConfig_t c = tb_cfg(TIM2, 199U, 99U, TIM_BM_COUNT_UP, false);
    (void)s_tr->init_tb(&c);
    (void)s_tr->enable_irq(TIM2, TIM_BM_EV_UPDATE);
    irq_on(TIM2_IRQn);
    delay_ms(10U);
    const uint32_t spur = s_irq[IQ_T2];
    irq_off(TIM2_IRQn);
    (void)t14(TIM2, "T12(b)");
    /* control: initialised by hand, UG with URS = 0 and UIF left set -> one IRQ */
    s_irq[IQ_T2] = 0U;
    TIM2->PSC = 199U;
    TIM2->ARR = 99U;
    TIM2->EGR = TIM_EGR_UG;
    (void)s_tr->enable_irq(TIM2, TIM_BM_EV_UPDATE);
    irq_on(TIM2_IRQn);
    delay_ms(10U);
    const uint32_t spur_ctrl = s_irq[IQ_T2];
    irq_off(TIM2_IRQn);
    (void)t14(TIM2, "T12(b) control");
    verdict((spur == 0U) && (spur_ctrl == 1U), "T12%s(b) TIM2 EnableIrq(UPDATE) after InitTimeBase, no Start: %lu IRQ in 10 ms "
            "(0); control by hand (UG with URS = 0, UIF not cleared): %lu IRQ (1)", s_tr->tag, (unsigned long)spur,
            (unsigned long)spur_ctrl);
    /* control of (a): DIER.UIE = 0 with the timer running and the vector enabled */
    s_irq[IQ_T2] = 0U;
    (void)s_tr->init_tb(&c);
    irq_on(TIM2_IRQn);
    (void)s_tr->start(TIM2);
    delay_ms(10U);
    (void)s_tr->stop(TIM2);
    irq_off(TIM2_IRQn);
    verdict(s_irq[IQ_T2] == 0U, "T12%s(a) control: TIM2 at 10 kHz for 10 ms with UIE = 0: %lu IRQ (0)",
            s_tr->tag, (unsigned long)s_irq[IQ_T2]);
    (void)t14(TIM2, "T12 control");

    /* (c) GetIRQn against stm32h755xx.h, every instance and event (UNSUPPORTED where none) */
    typedef struct { TIM_TypeDef *t; int16_t up, cc, trg; } IrqHand_t;
    static const IrqHand_t k_hand[] = {
        { TIM1, TIM1_UP_IRQn, TIM1_CC_IRQn, TIM1_TRG_COM_IRQn },   { TIM2, TIM2_IRQn, TIM2_IRQn, TIM2_IRQn },
        { TIM3, TIM3_IRQn, TIM3_IRQn, TIM3_IRQn },                 { TIM4, TIM4_IRQn, TIM4_IRQn, TIM4_IRQn },
        { TIM5, TIM5_IRQn, TIM5_IRQn, TIM5_IRQn },                 { TIM6, TIM6_DAC_IRQn, -1, -1 },
        { TIM7, TIM7_IRQn, -1, -1 },
        { TIM8, TIM8_UP_TIM13_IRQn, TIM8_CC_IRQn, TIM8_TRG_COM_TIM14_IRQn },
        { TIM12, TIM8_BRK_TIM12_IRQn, TIM8_BRK_TIM12_IRQn, TIM8_BRK_TIM12_IRQn },
        { TIM13, TIM8_UP_TIM13_IRQn, TIM8_UP_TIM13_IRQn, -1 },     { TIM14, TIM8_TRG_COM_TIM14_IRQn, TIM8_TRG_COM_TIM14_IRQn, -1 },
        { TIM15, TIM15_IRQn, TIM15_IRQn, TIM15_IRQn },             { TIM16, TIM16_IRQn, TIM16_IRQn, -1 },
        { TIM17, TIM17_IRQn, TIM17_IRQn, -1 },
    };
    uint32_t checked = 0U;
    uint32_t bad = 0U;
    for (uint32_t i = 0U; i < (sizeof(k_hand) / sizeof(k_hand[0])); i++)
    {
        TimBm_Caps_t caps;
        (void)tst_hand_caps(k_hand[i].t, &caps);
        for (uint32_t b = 0U; b <= 6U; b++)
        {
            if (b == 5U)
            {
                continue;
            }
            const uint32_t ev = 1UL << b;
            int32_t exp;
            if (b == 0U)      { exp = k_hand[i].up; }
            else if (b == 6U) { exp = k_hand[i].trg; }
            else              { exp = (b <= caps.channels) ? k_hand[i].cc : -1; }
            IRQn_Type got = (IRQn_Type)0;
            const TimBm_Status_t st = (s_tr == &k_tst_tier_ll) ? TIM_LL_GetIRQn(k_hand[i].t, ev, &got)
                                                               : TIM_BM_GetIRQn(k_hand[i].t, ev, &got);
            const bool ok = (exp < 0) ? (st == TIM_BM_ERR_UNSUPPORTED) : ((st == TIM_BM_OK) && ((int32_t)got == exp));
            checked++;
            if (!ok)
            {
                bad++;
                logw("T12%s(c) %s event 0x%02lX: status %u IRQn %d, hand table %ld", s_tr->tag, tim_name(k_hand[i].t),
                    (unsigned long)ev, (unsigned)st, (int)got, (long)exp);
            }
        }
    }
    verdict(bad == 0U, "T12%s(c) GetIRQn against stm32h755xx.h: %lu (instance, event) pairs, %lu differ",
            s_tr->tag, (unsigned long)checked, (unsigned long)bad);
}

/* ==========================================================================================
 * T13(c): GetDmaRequest against DMA_REQUEST_TIM* of stm32h7xx_hal_dma.h (table 126)
 * ========================================================================================== */

static void test_t13c(void)
{
    typedef struct { TIM_TypeDef *t; uint32_t up, cc1, cc2, cc3, cc4; } DmaHand_t;
    static const DmaHand_t k_hand[] = {
        { TIM1, DMA_REQUEST_TIM1_UP, DMA_REQUEST_TIM1_CH1, DMA_REQUEST_TIM1_CH2, DMA_REQUEST_TIM1_CH3, DMA_REQUEST_TIM1_CH4 },
        { TIM2, DMA_REQUEST_TIM2_UP, DMA_REQUEST_TIM2_CH1, DMA_REQUEST_TIM2_CH2, DMA_REQUEST_TIM2_CH3, DMA_REQUEST_TIM2_CH4 },
        { TIM3, DMA_REQUEST_TIM3_UP, DMA_REQUEST_TIM3_CH1, DMA_REQUEST_TIM3_CH2, DMA_REQUEST_TIM3_CH3, DMA_REQUEST_TIM3_CH4 },
        { TIM4, DMA_REQUEST_TIM4_UP, DMA_REQUEST_TIM4_CH1, DMA_REQUEST_TIM4_CH2, DMA_REQUEST_TIM4_CH3, 0U },
        { TIM5, DMA_REQUEST_TIM5_UP, DMA_REQUEST_TIM5_CH1, DMA_REQUEST_TIM5_CH2, DMA_REQUEST_TIM5_CH3, DMA_REQUEST_TIM5_CH4 },
        { TIM6, DMA_REQUEST_TIM6_UP, 0U, 0U, 0U, 0U },
        { TIM7, DMA_REQUEST_TIM7_UP, 0U, 0U, 0U, 0U },
        { TIM8, DMA_REQUEST_TIM8_UP, DMA_REQUEST_TIM8_CH1, DMA_REQUEST_TIM8_CH2, DMA_REQUEST_TIM8_CH3, DMA_REQUEST_TIM8_CH4 },
        { TIM12, 0U, 0U, 0U, 0U, 0U }, { TIM13, 0U, 0U, 0U, 0U, 0U }, { TIM14, 0U, 0U, 0U, 0U, 0U },
        { TIM15, DMA_REQUEST_TIM15_UP, DMA_REQUEST_TIM15_CH1, 0U, 0U, 0U },
        { TIM16, DMA_REQUEST_TIM16_UP, DMA_REQUEST_TIM16_CH1, 0U, 0U, 0U },
        { TIM17, DMA_REQUEST_TIM17_UP, DMA_REQUEST_TIM17_CH1, 0U, 0U, 0U },
    };
    uint32_t checked = 0U;
    uint32_t bad = 0U;
    for (uint32_t i = 0U; i < (sizeof(k_hand) / sizeof(k_hand[0])); i++)
    {
        const uint32_t exp[5] = { k_hand[i].up, k_hand[i].cc1, k_hand[i].cc2, k_hand[i].cc3, k_hand[i].cc4 };
        for (uint32_t b = 0U; b < 5U; b++)
        {
            uint32_t id = 0xFFFFFFFFUL;
            const TimBm_Status_t st = TIM_BM_GetDmaRequest(k_hand[i].t, 1UL << b, &id);
            const bool ok = (exp[b] == 0U) ? (st == TIM_BM_ERR_UNSUPPORTED) : ((st == TIM_BM_OK) && (id == exp[b]));
            checked++;
            if (!ok)
            {
                bad++;
                logw("T13(c) %s event 0x%02lX: status %u id %lu, DMA_REQUEST_* %lu", tim_name(k_hand[i].t),
                    (unsigned long)(1UL << b), (unsigned)st, (unsigned long)id, (unsigned long)exp[b]);
            }
        }
    }
    verdict(bad == 0U, "T13(c) GetDmaRequest against DMA_REQUEST_TIM* (stm32h7xx_hal_dma.h): %lu pairs, %lu differ",
            (unsigned long)checked, (unsigned long)bad);
}

/* ==========================================================================================
 * T14 control: a timer left configured on purpose must be flagged
 * ========================================================================================== */

static void test_t14_control(void)
{
    TimBm_TimeBaseConfig_t c = tb_cfg(TIM2, 5U, 1234U, TIM_BM_COUNT_UP, false);
    (void)TIM_BM_InitTimeBase(&c);
    uint32_t idx = 0U;
    uint32_t got = 0U;
    uint32_t exp = 0U;
    const bool flagged = !tst_reset_ok(TIM2, &idx, &got, &exp);
    const bool after = t14(TIM2, "T14 control");
    verdict(flagged && after, "T14 control: TIM2 left with PSC = 5, ARR = 1234 is flagged (%s reads 0x%08lX, reset "
            "0x%08lX); after Deinit the check passes %u", k_ri_name[idx], (unsigned long)got, (unsigned long)exp,
            (unsigned)after);
}

/* ==========================================================================================
 * T15: concurrency on one core (C10). The thread toggles CC2IE of TIM2 while a TIM7 ISR at
 * 50 kHz toggles CC3IE. A lost update: the ISR finds CC3IE other than it last wrote.
 * ========================================================================================== */

static volatile uint32_t s_t15_driver;   /* 1: both sides through tim_bm; 0: the control's hand RMW */
static volatile uint32_t s_t15_in_rmw;
static volatile uint32_t s_t15_isr;
static volatile uint32_t s_t15_lost;
static volatile uint32_t s_t15_overlap;
static volatile uint32_t s_t15_cc3;      /* CC3IE as the ISR last wrote it */

static void t15_isr(void)
{
    (void)TIM_BM_TakeFlags(TIM7, TIM_BM_EV_UPDATE);
    s_t15_isr++;
    if (s_t15_in_rmw != 0U)
    {
        s_t15_overlap++;
    }
    const uint32_t now = ((TIM2->DIER & TIM_DIER_CC3IE) != 0U) ? 1U : 0U;
    if (now != s_t15_cc3)
    {
        s_t15_lost++;
    }
    if (s_t15_driver != 0U)
    {
        (void)((s_t15_cc3 != 0U) ? TIM_BM_DisableIrq(TIM2, TIM_BM_EV_CC3) : TIM_BM_EnableIrq(TIM2, TIM_BM_EV_CC3));
    }
    else
    {
        TIM2->DIER = (s_t15_cc3 != 0U) ? (TIM2->DIER & ~TIM_DIER_CC3IE) : (TIM2->DIER | TIM_DIER_CC3IE);
    }
    s_t15_cc3 = (s_t15_cc3 != 0U) ? 0U : 1U;
}

#define T15_ITER        1000000UL
#define T15_RMW_WINDOW  24U        /* cycles between the read and the write in the control */

static void t15_run(bool driver)
{
    s_t15_driver = driver ? 1U : 0U;
    s_t15_isr = 0U;
    s_t15_lost = 0U;
    s_t15_overlap = 0U;
    s_t15_cc3 = 0U;
    TIM2->DIER = 0U;
    TimBm_TimeBaseConfig_t c = tb_cfg(TIM7, 0U, 3999U, TIM_BM_COUNT_UP, false);   /* 200 MHz / 4000 = 50 kHz */
    (void)TIM_BM_InitTimeBase(&c);
    (void)TIM_BM_EnableIrq(TIM7, TIM_BM_EV_UPDATE);
    s_t15_on = 1U;
    irq_on(TIM7_IRQn);
    (void)TIM_BM_Start(TIM7);
    for (uint32_t i = 0U; i < T15_ITER; i++)
    {
        if (driver)
        {
            (void)TIM_BM_EnableIrq(TIM2, TIM_BM_EV_CC2);
            (void)TIM_BM_DisableIrq(TIM2, TIM_BM_EV_CC2);
        }
        else
        {
            uint32_t d = TIM2->DIER;
            s_t15_in_rmw = 1U;
            delay_cycles(T15_RMW_WINDOW);
            TIM2->DIER = d | TIM_DIER_CC2IE;
            s_t15_in_rmw = 0U;
            d = TIM2->DIER;
            s_t15_in_rmw = 1U;
            delay_cycles(T15_RMW_WINDOW);
            TIM2->DIER = d & ~TIM_DIER_CC2IE;
            s_t15_in_rmw = 0U;
        }
    }
    (void)TIM_BM_Stop(TIM7);
    irq_off(TIM7_IRQn);
    s_t15_on = 0U;
    /* the last ISR's write must still be there (one more check of the final state) */
    const uint32_t final_cc3 = ((TIM2->DIER & TIM_DIER_CC3IE) != 0U) ? 1U : 0U;
    if (final_cc3 != s_t15_cc3)
    {
        s_t15_lost++;
    }
    const uint32_t final_dier = TIM2->DIER;
    if (driver)
    {
        verdict((s_t15_lost == 0U) && (s_t15_isr > 1000U) && ((final_dier & TIM_DIER_CC2IE) == 0U),
                "T15 tim_bm: %lu x Enable/DisableIrq(TIM2, CC2) against %lu TIM7 ISRs toggling CC3: %lu lost updates (0), "
                "final DIER 0x%04lX", (unsigned long)T15_ITER, (unsigned long)s_t15_isr, (unsigned long)s_t15_lost,
                (unsigned long)final_dier);
    }
    else
    {
        verdict((s_t15_lost > 0U) && (s_t15_overlap > 0U),
                "T15 control (RMW by hand, no PRIMASK, %u-cycle window): %lu ISRs, %lu inside the thread's RMW window, "
                "%lu lost updates (> 0)", (unsigned)T15_RMW_WINDOW, (unsigned long)s_t15_isr,
                (unsigned long)s_t15_overlap, (unsigned long)s_t15_lost);
    }
    TIM2->DIER = 0U;
    (void)t14(TIM7, "T15");
    (void)t14(TIM2, "T15");
}

static void test_t15(void)
{
    t15_run(true);
    t15_run(false);
}

/* ==========================================================================================
 * T16: the RCC banks of the two cores (RM0399 9.5.10)
 * ========================================================================================== */

static bool tim2_counting(void)
{
    const uint32_t a = TIM2->CNT;
    delay_cycles(2000U);
    return TIM2->CNT != a;
}

static void test_t16(void)
{
    TstArea_t *a = TST_AREA;
    TimBm_TimeBaseConfig_t c = tb_cfg(TIM2, 0U, 0xFFFFFFFFUL, TIM_BM_COUNT_UP, false);
    const bool on = (TIM_BM_EnableClock(TIM2) == TIM_BM_OK) && (TIM_BM_InitTimeBase(&c) == TIM_BM_OK) &&
                    (TIM_BM_Start(TIM2) == TIM_BM_OK);
    const bool run0 = tim2_counting();
    const bool s1 = cm4_send(TST_CMD_T16, 0U) && (a->m4_res[0] == TIM_BM_OK);
    const uint32_t c1_1 = RCC_C1->APB1LENR & RCC_APB1LENR_TIM2EN;
    const uint32_t c2_1 = RCC_C2->APB1LENR & RCC_APB1LENR_TIM2EN;
    const bool run1 = tim2_counting();
    const bool s2 = cm4_send(TST_CMD_T16, 1U) && (a->m4_res[0] == TIM_BM_OK);
    const uint32_t c2_mid = a->m4_res[3] & RCC_APB1LENR_TIM2EN;   /* CPU2's bank after its EnableClock */
    const uint32_t c2_2 = RCC_C2->APB1LENR & RCC_APB1LENR_TIM2EN;
    const bool run2 = tim2_counting();
    (void)TIM_BM_DisableClock(TIM2);
    const uint32_t c1_3 = RCC_C1->APB1LENR & RCC_APB1LENR_TIM2EN;
    const bool run3 = tim2_counting();
    verdict(on && run0 && s1 && (c1_1 != 0U) && (c2_1 == 0U) && run1,
            "T16 CM4 DisableClock(TIM2) while CM7 enables it: RCC_C1 bit %lu, RCC_C2 bit %lu, TIM2 still counting %u",
            (unsigned long)(c1_1 != 0U), (unsigned long)(c2_1 != 0U), (unsigned)run1);
    verdict(s2 && (c2_mid != 0U) && (c2_2 == 0U) && run2,
            "T16 CM4 EnableClock then DisableClock(TIM2): RCC_C2 bit %lu then %lu, TIM2 still counting %u",
            (unsigned long)(c2_mid != 0U), (unsigned long)(c2_2 != 0U), (unsigned)run2);
    verdict((c1_3 == 0U) && !run3, "T16 control: CM7 DisableClock(TIM2) with RCC_C2 clear: RCC_C1 bit %lu, TIM2 counting %u (0)",
            (unsigned long)(c1_3 != 0U), (unsigned)run3);
    (void)TIM_BM_EnableClock(TIM2);
    (void)t14(TIM2, "T16");
}

/* ==========================================================================================
 * B1: cycles of the plan's functions on CPU1 (CPU2: TST_CMD_B1)
 * ========================================================================================== */

static uint32_t cycles_of(TimBm_Status_t (*fn)(void))
{
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    const uint32_t c0 = DWT->CYCCNT;
    (void)fn();
    const uint32_t c = DWT->CYCCNT - c0;
    __set_PRIMASK(pm);
    return c;
}

static TimBm_Status_t b1_take(void)  { (void)TIM_BM_TakeFlags(TIM2, TIM_BM_EV_UPDATE); return TIM_BM_OK; }
static TimBm_Status_t b1_oe(void)    { return TIM_BM_OutputEnable(TIM2, TIM_BM_CH(1)); }
static TimBm_Status_t b1_oc(void)
{
    const TimBm_OcConfig_t oc = { TIM2, 1U, TIM_BM_OC_PWM1, 100U, false, true };
    return TIM_BM_ConfigOutput(&oc);
}
static TimBm_Status_t b1_tb(void)
{
    const TimBm_TimeBaseConfig_t c = { TIM2, 0U, 999U, TIM_BM_COUNT_UP, 0U, true, false, false };
    return TIM_BM_InitTimeBase(&c);
}

static void test_b1(void)
{
    TstArea_t *a = TST_AREA;
    const uint32_t tb = cycles_of(b1_tb);
    const uint32_t oc = cycles_of(b1_oc);
    const uint32_t oe = cycles_of(b1_oe);
    TIM2->SR = 0U;
    const uint32_t tk = cycles_of(b1_take);
    (void)t14(TIM2, "B1");
    logw("B1 CM7 cycles (400 MHz): TakeFlags %lu, OutputEnable %lu, ConfigOutput %lu, InitTimeBase %lu",
        (unsigned long)tk, (unsigned long)oe, (unsigned long)oc, (unsigned long)tb);
    if (cm4_send(TST_CMD_B1, 0U))
    {
        logw("B1 CM4 cycles (200 MHz, TIM15): TakeFlags %lu, OutputEnable %lu, ConfigOutput %lu, InitTimeBase %lu",
            (unsigned long)a->m4_res[0], (unsigned long)a->m4_res[1], (unsigned long)a->m4_res[2],
            (unsigned long)a->m4_res[3]);
    }
}

/* ==========================================================================================
 * CPU2's F1 tests (TIM15), judged here
 * ========================================================================================== */

static void test_cm4(void)
{
    TstArea_t *a = TST_AREA;
    if (cm4_send(TST_CMD_T1, 0U))
    {
        verdict((a->m4_res[TST_R_T1_BAD] == 0U) && (a->m4_res[TST_R_T1_TOUCHED] == 0U) && (a->m4_res[TST_R_T1_CASES] > 0U),
                "T1 CM4 TIM15: %lu refusals, wrong status 0x%04lX, snapshot changed 0x%04lX", (unsigned long)a->m4_res[TST_R_T1_CASES],
                (unsigned long)a->m4_res[TST_R_T1_BAD], (unsigned long)a->m4_res[TST_R_T1_TOUCHED]);
        for (uint32_t k = 0U; k < a->m4_res[TST_R_T1_CASES]; k++)
        {
            LOG("T1 CM4 case %lu: status %lu", (unsigned long)k, (unsigned long)a->m4_res[TST_R_T1_STATUS0 + k]);
        }
        verdict((a->m4_res[TST_R_T1_CTRL_ST] == TIM_BM_OK) && (a->m4_res[TST_R_T1_CTRL_DIFF] == a->m4_res[TST_R_T1_CTRL_EXP]),
                "T1 CM4 control (O1): ConfigOutput(TIM15 CH1) changes TIM15 words 0x%07lX (0x%07lX: CCMR1, CCR1), status %lu",
                (unsigned long)a->m4_res[TST_R_T1_CTRL_DIFF], (unsigned long)a->m4_res[TST_R_T1_CTRL_EXP],
                (unsigned long)a->m4_res[TST_R_T1_CTRL_ST]);
    }
    if (cm4_send(TST_CMD_T2, 0U))
    {
        verdict((a->m4_res[TST_R_T2_MISMATCH] == 0U) && (a->m4_res[TST_R_T2_CALLS_BAD] == 0U) &&
                (a->m4_res[TST_R_T2_CTRL] == 1U) && (a->m4_res[TST_R_T2_CONFIGS] > 0U),
                "T2 CM4 TIM15: %lu images, mismatches %lu (first: image %lu word 0x%02lX reads 0x%08lX, hand 0x%08lX), "
                "calls not OK %lu, altered image flagged %lu", (unsigned long)a->m4_res[TST_R_T2_CONFIGS],
                (unsigned long)a->m4_res[TST_R_T2_MISMATCH], (unsigned long)a->m4_res[TST_R_T2_FIRST_CFG],
                (unsigned long)(a->m4_res[TST_R_T2_FIRST_REG] * 4U), (unsigned long)a->m4_res[TST_R_T2_FIRST_GOT],
                (unsigned long)a->m4_res[TST_R_T2_FIRST_EXP], (unsigned long)a->m4_res[TST_R_T2_CALLS_BAD],
                (unsigned long)a->m4_res[TST_R_T2_CTRL]);
    }
    if (cm4_send(TST_CMD_T5, 0U))
    {
        /* CPU2 at 200 MHz: 1 cycle per tick */
        for (uint32_t m = 0U; m < 2U; m++)
        {
            const uint32_t ticks = a->m4_res[3U * m];
            const uint32_t n = a->m4_res[3U * m + 1U];
            const uint32_t span = a->m4_res[3U * m + 2U];
            const int64_t exp = (int64_t)(n - 1U) * ticks;
            const int64_t expc = (int64_t)(n - 1U) * ((m == 0U) ? 1500U : 1000U);
            const int32_t err = (n >= 3U) ? (int32_t)((((int64_t)span - exp) * 1000000) / exp) : 0;
            const int32_t ctrl = (n >= 3U) ? (int32_t)((((int64_t)span - expc) * 1000000) / expc) : 0;
            verdict((n >= 3U) && (err >= -100) && (err <= 100) && ((ctrl < -100) || (ctrl > 100)),
                    "T5 TIM15 (CM4) %s: %lu updates over %lu cycles, %lu ticks each: %+ld ppm (|e| <= 100); control: %+ld ppm",
                    (m == 0U) ? "(a) up, PSC=1 ARR=499" : "(b) RCR=3", (unsigned long)n, (unsigned long)span,
                    (unsigned long)ticks, (long)err, (long)ctrl);
        }
        verdict((a->m4_res[7] == TIM_BM_OK) && (a->m4_res[6] == T_CLK_HZ),
                "T5(e) CM4 GetKernelClockHz(TIM15, hclk %lu) = %lu Hz, status %lu", (unsigned long)a->m4_res[9],
                (unsigned long)a->m4_res[6], (unsigned long)a->m4_res[7]);
        verdict(a->m4_res[8] == 1U, "T14 CM4 TIM15 after T5: %lu", (unsigned long)a->m4_res[8]);
    }
    if (cm4_send(TST_CMD_T12, 0U))
    {
        verdict((a->m4_res[0] >= 999U) && (a->m4_res[0] <= 1001U) && (a->m4_res[2] == 0U) && (a->m4_res[3] == 1U) &&
                (a->m4_res[4] == 0U) && (a->m4_res[6] == 1U),
                "T12 TIM15 (CM4): (a) %lu update IRQs in %lu cycles (1000 +-1); (b) %lu spurious (0), control by hand %lu (1); "
                "UIE = 0: %lu (0); GetIRQn = TIM15_IRQn %lu", (unsigned long)a->m4_res[0], (unsigned long)a->m4_res[1],
                (unsigned long)a->m4_res[2], (unsigned long)a->m4_res[3], (unsigned long)a->m4_res[4],
                (unsigned long)a->m4_res[6]);
        verdict(a->m4_res[5] == 1U, "T14 CM4 TIM15 after T12: %lu", (unsigned long)a->m4_res[5]);
    }
}

/* The internal F1 tests, between the T0b steps and the AD2 steps. No wired pin moves. */
static void f1_internal(void)
{
    f1_clocks_on();
    test_t3();
    test_t1();
    test_t2();
    test_t14_control();
    test_t5();
    test_t7();
    test_t10();
    test_t12();
    test_t13c();
    test_t15();
    test_t16();
    test_b1();
    test_cm4();
}

/* ==========================================================================================
 * F2: tim_ll (plan "Plan de pruebas", the F2 rows: T1, T2, T5, T6, T8, T10(a)(b), T12, T14)
 *
 * T1 calls tim_ll with LL constants, its own table. T2, T5, T10, T12 and T14 are F1's tests
 * run again through the LL tier (s_tr), with the same oracles; T2 adds the parity against
 * tim_bm's image. T6 and T8 are the bench steps T6_TIM1_LL, T6_TIM15_LL and T8_LL.
 * ========================================================================================== */

static const T1Case_t k_t1_ll[] = {
    /*  0 */ { "InitTimeBase(NULL)",                                         TIM_BM_ERR_PARAM,       0U },
    /*  1 */ { "InitTimeBase(USART3)",                                       TIM_BM_ERR_PARAM,       0U },
    /*  2 */ { "InitTimeBase(TIM1 + 4)",                                     TIM_BM_ERR_PARAM,       0U },
    /*  3 */ { "Start(TIM1 + 4)",                                            TIM_BM_ERR_PARAM,       0U },
    /*  4 */ { "ConfigOutput(TIM2 channel 0x0002: no LL constant)",          TIM_BM_ERR_PARAM,       0U },
    /*  5 */ { "ConfigOutput(TIM2 LL_TIM_CHANNEL_CH1N)",                     TIM_BM_ERR_PARAM,       0U },
    /*  6 */ { "ConfigOutput(TIM12 LL_TIM_CHANNEL_CH3)",                     TIM_BM_ERR_UNSUPPORTED, 0U },
    /*  7 */ { "ConfigOutput(TIM7 CH1): PWM on TIM7",                        TIM_BM_ERR_UNSUPPORTED, 0U },
    /*  8 */ { "ConfigOutput(TIM2 LL_TIM_OCMODE_RETRIG_OPM1: outside the API)", TIM_BM_ERR_PARAM,    0U },
    /*  9 */ { "InitTimeBase(TIM2 ARR = 0)",                                 TIM_BM_ERR_PARAM,       0U },
    /* 10 */ { "InitTimeBase(TIM4 ARR = 0x10000, 16 bits)",                  TIM_BM_ERR_PARAM,       0U },
    /* 11 */ { "InitTimeBase(TIM2 PSC = 65536)",                             TIM_BM_ERR_PARAM,       0U },
    /* 12 */ { "InitTimeBase(TIM17 repetition = 0x100, 8-bit RCR)",          TIM_BM_ERR_PARAM,       0U },
    /* 13 */ { "InitTimeBase(TIM12 LL_TIM_COUNTERMODE_CENTER_DOWN)",         TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 14 */ { "InitTimeBase(TIM2 counter mode 0x0008: no LL constant)",     TIM_BM_ERR_PARAM,       0U },
    /* 15 */ { "ConfigSlave(TIM4 TRIGGER, LL_TIM_TS_ITR5)",                  TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 16 */ { "ConfigSlave(TIM2 TRIGGER, LL_TIM_TS_ETRF)",                  TIM_BM_ERR_PARAM,       0U },
    /* 17 */ { "ConfigSlave(TIM2 TRIGGER, LL_TIM_TS_ITR9: not on this device)", TIM_BM_ERR_PARAM,    0U },
    /* 18 */ { "ConfigSlave(TIM2 LL_TIM_CLOCKSOURCE_EXT_MODE2: outside the API)", TIM_BM_ERR_PARAM,  0U },
    /* 19 */ { "ConfigSlave(TIM2 GATED on LL_TIM_TS_TI1F_ED)",               TIM_BM_ERR_PARAM,       0U },
    /* 20 */ { "ConfigInput(TIM17 LL_TIM_TIM17_TI1_RMP_SPDIF_FS: 0001, reserved)", TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 21 */ { "ConfigInput(TIM2 CH2 filter 0x00000001: no LL constant)",    TIM_BM_ERR_PARAM,       0U },
    /* 22 */ { "ConfigInput(TIM2 CH2 polarity CC1NP alone: no LL constant)", TIM_BM_ERR_PARAM,       0U },
    /* 23 */ { "ConfigInput(TIM2 CH2 remap in the TI1 field)",               TIM_BM_ERR_PARAM,       0U },
    /* 24 */ { "ConfigInput(TIM13 TRC, unknown filter): capability first (C6)", TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 25 */ { "2.16.1 ConfigMaster(TIM2 MSM) with OPM, reset+trigger",      TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 26 */ { "2.16.1 ConfigSlave(TIM2 COMBINED_RESETTRIGGER) with OPM, MSM", TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 27 */ { "2.16.1 InitTimeBase(TIM2 OPM) with MSM, reset+trigger",      TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 28 */ { "ConfigOutput(TIM2 CH1) with CC1E = 1",                       TIM_BM_ERR_BUSY,        0U },
    /* 29 */ { "ConfigInput(TIM2 CH2) with CC2E = 1",                        TIM_BM_ERR_BUSY,        0U },
    /* 30 */ { "InitTimeBase(TIM2) with CEN = 1",                            TIM_BM_ERR_BUSY,        1U },
    /* 31 */ { "ConfigSlave(TIM2 trigger) with SMS = gated",                 TIM_BM_ERR_BUSY,        0U },
    /* 32 */ { "EnableDmaRequest(TIM4 LL_TIM_DIER_CC4IE: no DMAMUX1 request)", TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 33 */ { "OutputEnable(TIM2 CH1) on an input",                         TIM_BM_ERR_PARAM,       0U },
    /* 34 */ { "CaptureEnable(TIM2 CH1) on an output",                       TIM_BM_ERR_PARAM,       0U },
    /* 35 */ { "OutputEnable(TIM2 LL_TIM_CHANNEL_CH1N)",                     TIM_BM_ERR_PARAM,       0U },
    /* 36 */ { "EnableIrq(TIM13 LL_TIM_DIER_TIE)",                           TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 37 */ { "EnableIrq(TIM2 LL_TIM_DIER_COMIE: outside the API)",         TIM_BM_ERR_PARAM,       0U },
    /* 38 */ { "SetPeriod(TIM2, 0)",                                         TIM_BM_ERR_PARAM,       0U },
    /* 39 */ { "ConfigMaster(TIM12 LL_TIM_TRGO_OC3REF > MMS 5)",             TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 40 */ { "ConfigMaster(TIM13: no TRGO)",                               TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 41 */ { "ConfigMaster(TIM2 LL_TIM_TRGO2_UPDATE: TRGO2, outside the API)", TIM_BM_ERR_PARAM,   0U },
    /* 42 */ { "ConfigPwmInput(TIM13: one channel, no slave)",               TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 43 */ { "ConfigPwmInput(TIM1 LL_TIM_CHANNEL_CH3)",                    TIM_BM_ERR_PARAM,       0U },
    /* 44 */ { "ConfigPwmInput(TIM1 CH1 LL_TIM_IC_POLARITY_BOTHEDGE)",       TIM_BM_ERR_PARAM,       0U },
    /* 45 */ { "WaitFlags(TIM2, taken NULL)",                                TIM_BM_ERR_PARAM,       0U },
    /* 46 */ { "GenerateEvent(TIM2 LL_TIM_DIER_BIE: outside the API)",       TIM_BM_ERR_PARAM,       0U },
};
#define T1_LL_CASES   (sizeof(k_t1_ll) / sizeof(k_t1_ll[0]))

static TimBm_Status_t t1_ll_case(uint32_t k)
{
    TimLl_TimeBaseConfig_t tb = { TIM2, 0U, LL_TIM_COUNTERMODE_UP, 999U, 0U, false, false, false };
    TimLl_OcConfig_t oc = { TIM2, LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_PWM1, 100U, false, true };
    TimLl_IcConfig_t ic = { TIM2, LL_TIM_CHANNEL_CH2, LL_TIM_ACTIVEINPUT_DIRECTTI, LL_TIM_IC_POLARITY_RISING,
                            LL_TIM_ICPSC_DIV1, LL_TIM_IC_FILTER_FDIV1, 0U };
    TimBm_Status_t st = TIM_BM_OK;
#define T1_CALL(expr)  do { t1_snap(&s_t1_b); st = (expr); t1_snap(&s_t1_a); } while (0)
    switch (k)
    {
    case 0:  T1_CALL(TIM_LL_InitTimeBase(NULL)); break;
    case 1:  tb.tim = (TIM_TypeDef *)USART3; T1_CALL(TIM_LL_InitTimeBase(&tb)); break;
    case 2:  tb.tim = (TIM_TypeDef *)(TIM1_BASE + 4U); T1_CALL(TIM_LL_InitTimeBase(&tb)); break;
    case 3:  T1_CALL(TIM_LL_Start((TIM_TypeDef *)(TIM1_BASE + 4U))); break;
    case 4:  oc.channel = 0x0002U; T1_CALL(TIM_LL_ConfigOutput(&oc)); break;
    case 5:  oc.channel = LL_TIM_CHANNEL_CH1N; T1_CALL(TIM_LL_ConfigOutput(&oc)); break;
    case 6:  oc.tim = TIM12; oc.channel = LL_TIM_CHANNEL_CH3; T1_CALL(TIM_LL_ConfigOutput(&oc)); break;
    case 7:  oc.tim = TIM7; T1_CALL(TIM_LL_ConfigOutput(&oc)); break;
    case 8:  oc.oc_mode = LL_TIM_OCMODE_RETRIG_OPM1; T1_CALL(TIM_LL_ConfigOutput(&oc)); break;
    case 9:  tb.autoreload = 0U; T1_CALL(TIM_LL_InitTimeBase(&tb)); break;
    case 10: tb.tim = TIM4; tb.autoreload = 0x10000U; T1_CALL(TIM_LL_InitTimeBase(&tb)); break;
    case 11: tb.prescaler = 65536U; T1_CALL(TIM_LL_InitTimeBase(&tb)); break;
    case 12: tb.tim = TIM17; tb.repetition = 0x100U; T1_CALL(TIM_LL_InitTimeBase(&tb)); break;
    case 13: tb.tim = TIM12; tb.counter_mode = LL_TIM_COUNTERMODE_CENTER_DOWN; T1_CALL(TIM_LL_InitTimeBase(&tb)); break;
    case 14: tb.counter_mode = 0x0008U; T1_CALL(TIM_LL_InitTimeBase(&tb)); break;
    case 15: T1_CALL(TIM_LL_ConfigSlave(TIM4, LL_TIM_SLAVEMODE_TRIGGER, LL_TIM_TS_ITR5)); break;
    case 16: T1_CALL(TIM_LL_ConfigSlave(TIM2, LL_TIM_SLAVEMODE_TRIGGER, LL_TIM_TS_ETRF)); break;
    case 17: T1_CALL(TIM_LL_ConfigSlave(TIM2, LL_TIM_SLAVEMODE_TRIGGER, LL_TIM_TS_ITR9)); break;
    case 18: T1_CALL(TIM_LL_ConfigSlave(TIM2, LL_TIM_CLOCKSOURCE_EXT_MODE2, LL_TIM_TS_ITR0)); break;
    case 19: T1_CALL(TIM_LL_ConfigSlave(TIM2, LL_TIM_SLAVEMODE_GATED, LL_TIM_TS_TI1F_ED)); break;
    case 20:
        ic.tim = TIM17; ic.channel = LL_TIM_CHANNEL_CH1; ic.remap = LL_TIM_TIM17_TI1_RMP_SPDIF_FS;
        T1_CALL(TIM_LL_ConfigInput(&ic));
        break;
    case 21: ic.filter = 0x00000001U; T1_CALL(TIM_LL_ConfigInput(&ic)); break;
    case 22: ic.polarity = TIM_CCER_CC1NP; T1_CALL(TIM_LL_ConfigInput(&ic)); break;
    case 23: ic.remap = TIM_TISEL_TI1SEL_0; T1_CALL(TIM_LL_ConfigInput(&ic)); break;
    case 24:
        ic.tim = TIM13; ic.channel = LL_TIM_CHANNEL_CH1; ic.active_input = LL_TIM_ACTIVEINPUT_TRC; ic.filter = 0x1U;
        T1_CALL(TIM_LL_ConfigInput(&ic));
        break;
    case 25:
        tb.one_pulse = true;
        (void)TIM_LL_InitTimeBase(&tb);
        (void)TIM_LL_ConfigSlave(TIM2, LL_TIM_SLAVEMODE_COMBINED_RESETTRIGGER, LL_TIM_TS_ITR0);
        T1_CALL(TIM_LL_ConfigMaster(TIM2, LL_TIM_TRGO_RESET, true));
        break;
    case 26:
        tb.one_pulse = true;
        (void)TIM_LL_InitTimeBase(&tb);
        (void)TIM_LL_ConfigMaster(TIM2, LL_TIM_TRGO_RESET, true);
        T1_CALL(TIM_LL_ConfigSlave(TIM2, LL_TIM_SLAVEMODE_COMBINED_RESETTRIGGER, LL_TIM_TS_ITR0));
        break;
    case 27:
        (void)TIM_LL_InitTimeBase(&tb);
        (void)TIM_LL_ConfigMaster(TIM2, LL_TIM_TRGO_RESET, true);
        (void)TIM_LL_ConfigSlave(TIM2, LL_TIM_SLAVEMODE_COMBINED_RESETTRIGGER, LL_TIM_TS_ITR0);
        tb.one_pulse = true;
        T1_CALL(TIM_LL_InitTimeBase(&tb));
        break;
    case 28:
        (void)TIM_LL_ConfigOutput(&oc);
        (void)TIM_LL_OutputEnable(TIM2, LL_TIM_CHANNEL_CH1);
        T1_CALL(TIM_LL_ConfigOutput(&oc));
        break;
    case 29:
        (void)TIM_LL_ConfigInput(&ic);
        (void)TIM_LL_CaptureEnable(TIM2, LL_TIM_CHANNEL_CH2);
        T1_CALL(TIM_LL_ConfigInput(&ic));
        break;
    case 30:
        (void)TIM_LL_InitTimeBase(&tb);
        (void)TIM_LL_Start(TIM2);
        T1_CALL(TIM_LL_InitTimeBase(&tb));
        break;
    case 31:
        (void)TIM_LL_ConfigSlave(TIM2, LL_TIM_SLAVEMODE_GATED, LL_TIM_TS_ITR0);
        T1_CALL(TIM_LL_ConfigSlave(TIM2, LL_TIM_SLAVEMODE_TRIGGER, LL_TIM_TS_ITR1));
        break;
    case 32: T1_CALL(TIM_LL_EnableDmaRequest(TIM4, LL_TIM_DIER_CC4IE)); break;
    case 33:
        ic.channel = LL_TIM_CHANNEL_CH1;
        (void)TIM_LL_ConfigInput(&ic);
        T1_CALL(TIM_LL_OutputEnable(TIM2, LL_TIM_CHANNEL_CH1));
        break;
    case 34:
        (void)TIM_LL_ConfigOutput(&oc);
        T1_CALL(TIM_LL_CaptureEnable(TIM2, LL_TIM_CHANNEL_CH1));
        break;
    case 35: T1_CALL(TIM_LL_OutputEnable(TIM2, LL_TIM_CHANNEL_CH1N)); break;
    case 36: T1_CALL(TIM_LL_EnableIrq(TIM13, LL_TIM_DIER_TIE)); break;
    case 37: T1_CALL(TIM_LL_EnableIrq(TIM2, LL_TIM_DIER_COMIE)); break;
    case 38: T1_CALL(TIM_LL_SetPeriod(TIM2, 0U)); break;
    case 39: T1_CALL(TIM_LL_ConfigMaster(TIM12, LL_TIM_TRGO_OC3REF, false)); break;
    case 40: T1_CALL(TIM_LL_ConfigMaster(TIM13, LL_TIM_TRGO_UPDATE, false)); break;
    case 41: T1_CALL(TIM_LL_ConfigMaster(TIM2, LL_TIM_TRGO2_UPDATE, false)); break;
    case 42:
        T1_CALL(TIM_LL_ConfigPwmInput(TIM13, LL_TIM_CHANNEL_CH1, LL_TIM_IC_POLARITY_RISING, LL_TIM_ICPSC_DIV1,
                                      LL_TIM_IC_FILTER_FDIV1, 0U));
        break;
    case 43:
        T1_CALL(TIM_LL_ConfigPwmInput(TIM1, LL_TIM_CHANNEL_CH3, LL_TIM_IC_POLARITY_RISING, LL_TIM_ICPSC_DIV1,
                                      LL_TIM_IC_FILTER_FDIV1, 0U));
        break;
    case 44:
        T1_CALL(TIM_LL_ConfigPwmInput(TIM1, LL_TIM_CHANNEL_CH1, LL_TIM_IC_POLARITY_BOTHEDGE, LL_TIM_ICPSC_DIV1,
                                      LL_TIM_IC_FILTER_FDIV1, 0U));
        break;
    case 45: T1_CALL(TIM_LL_WaitFlags(TIM2, LL_TIM_DIER_UIE, 1000U, NULL)); break;
    default: T1_CALL(TIM_LL_GenerateEvent(TIM2, LL_TIM_DIER_BIE)); break;
    }
#undef T1_CALL
    return st;
}

static void test_t1_ll(void)
{
    uint64_t bad = 0U;
    uint64_t touched = 0U;
    for (uint32_t k = 0U; k < T1_LL_CASES; k++)
    {
        uint32_t tm[T1_NT];
        uint32_t rcc = 0U;
        const TimBm_Status_t st = t1_ll_case(k);
        t1_diff(tm, &rcc, k_t1_ll[k].running != 0U);
        bool same = (rcc == 0U);
        for (uint32_t j = 0U; j < T1_NT; j++)
        {
            same = same && (tm[j] == 0U);
        }
        if (st != (TimBm_Status_t)k_t1_ll[k].expect)
        {
            bad |= 1ULL << k;
        }
        if (!same)
        {
            touched |= 1ULL << k;
        }
        LOG("T1 LL %lu %s: status %u (expected %u), registers and RCC banks unchanged %u", (unsigned long)k,
            k_t1_ll[k].what, (unsigned)st, (unsigned)k_t1_ll[k].expect, (unsigned)same);
        for (uint32_t j = 0U; j < T1_NT; j++)
        {
            (void)TIM_LL_Deinit(k_t1_tims[j]);
        }
        (void)TIM_LL_Deinit(TIM13);
    }
    verdict((bad == 0U) && (touched == 0U),
            "T1 LL CM7 %u refusals: wrong status 0x%08lX%08lX, snapshot changed 0x%08lX%08lX", (unsigned)T1_LL_CASES,
            (unsigned long)(bad >> 32), (unsigned long)(bad & 0xFFFFFFFFU), (unsigned long)(touched >> 32),
            (unsigned long)(touched & 0xFFFFFFFFU));

    /* O1: the valid LL call changes exactly CCMR1 and CCR1 of TIM2 */
    uint32_t tm[T1_NT];
    uint32_t rcc = 0U;
    const TimLl_OcConfig_t oc = { TIM2, LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_PWM1, 100U, false, true };
    t1_snap(&s_t1_b);
    const TimBm_Status_t st1 = TIM_LL_ConfigOutput(&oc);
    t1_snap(&s_t1_a);
    t1_diff(tm, &rcc, false);
    const uint32_t exp_tim2 = (1UL << TST_RI_CCMR1) | (1UL << TST_RI_CCR1);
    const uint32_t others = tm[0] | tm[2] | tm[3] | tm[4] | tm[5] | rcc;
    verdict((st1 == TIM_BM_OK) && (tm[1] == exp_tim2) && (others == 0U),
            "T1 LL control (O1): ConfigOutput(TIM2 LL_TIM_CHANNEL_CH1 LL_TIM_OCMODE_PWM1 CCR 100 preload) status %u "
            "changes TIM2 words 0x%07lX (0x%07lX: CCMR1, CCR1), other timers and RCC 0x%lX (0)", (unsigned)st1,
            (unsigned long)tm[1], (unsigned long)exp_tim2, (unsigned long)others);
    (void)TIM_LL_Deinit(TIM2);
}

static void test_t2_ll(void)
{
    uint32_t total = 0U;
    uint32_t par = 0U;
    uint32_t bad = 0U;
    for (uint32_t i = 0U; i < N_TIMS; i++)
    {
        TstT2Result_t r;
        tst_t2_run(k_tims[i].tim, &k_tst_tier_ll, NULL, &r);
        total += r.configs;
        par += r.par_configs;
        const bool ok = (r.mismatches == 0U) && (r.calls_bad == 0U) && (r.ctrl_detected == 1U) && (r.configs > 0U) &&
                        (r.par_configs == r.configs) && (r.par_mismatches == 0U) && (r.par_ctrl == 1U);
        if (!ok)
        {
            bad |= 1UL << i;
        }
        logw("T2 LL %s: %lu images (time base %lu, OC %lu, IC %lu, master %lu, slave %lu), hand mismatches %lu, calls "
            "not OK %lu, altered image flagged %lu; parity with tim_bm: %lu compared, %lu differ, altered tim_bm image "
            "flagged %lu%s", k_tims[i].name, (unsigned long)r.configs, (unsigned long)r.per_class[0],
            (unsigned long)r.per_class[1], (unsigned long)r.per_class[2], (unsigned long)r.per_class[3],
            (unsigned long)r.per_class[4], (unsigned long)r.mismatches, (unsigned long)r.calls_bad,
            (unsigned long)r.ctrl_detected, (unsigned long)r.par_configs, (unsigned long)r.par_mismatches,
            (unsigned long)r.par_ctrl, ok ? "" : " <-");
        if (r.mismatches != 0U)
        {
            logw("T2 LL %s first hand mismatch: %s image %lu, %s reads 0x%08lX, hand encoding 0x%08lX", k_tims[i].name,
                k_cls_name[r.first_cls], (unsigned long)r.first_cfg, k_ri_name[r.first_idx], (unsigned long)r.first_got,
                (unsigned long)r.first_exp);
        }
        if (r.par_mismatches != 0U)
        {
            logw("T2 LL %s first parity difference: image %lu, %s tim_ll 0x%08lX, tim_bm 0x%08lX", k_tims[i].name,
                (unsigned long)r.par_first_cfg, k_ri_name[r.par_first_idx], (unsigned long)r.par_first_tier,
                (unsigned long)r.par_first_bm);
        }
        (void)t14(k_tims[i].tim, "T2");
    }
    verdict(bad == 0U, "T2 LL CM7 register images: %lu configurations on 12 instances against the hand encoding, %lu "
            "compared bit for bit with tim_bm's (parity), instances failing 0x%04lX", (unsigned long)total,
            (unsigned long)par, (unsigned long)bad);
}

static void test_t10_ll(void)
{
    t10a(false);
    t10a(true);
    t10_gated(TIM2, TIM_BM_MMS_OC1REF, TIM4, 1U, 100U, 200U, "(b) TIM2 OC1REF -> TIM4 gated (ITR1)", false);
    t10_gated(TIM2, TIM_BM_MMS_OC1REF, TIM4, 3U, 100U, 200U, "(b)", true);
}

static TimBm_Status_t b1l_take(void)  { (void)TIM_LL_TakeFlags(TIM2, LL_TIM_DIER_UIE); return TIM_BM_OK; }
static TimBm_Status_t b1l_oe(void)    { return TIM_LL_OutputEnable(TIM2, LL_TIM_CHANNEL_CH1); }
static TimBm_Status_t b1l_oc(void)
{
    const TimLl_OcConfig_t oc = { TIM2, LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_PWM1, 100U, false, true };
    return TIM_LL_ConfigOutput(&oc);
}
static TimBm_Status_t b1l_tb(void)
{
    const TimLl_TimeBaseConfig_t c = { TIM2, 0U, LL_TIM_COUNTERMODE_UP, 999U, 0U, true, false, false };
    return TIM_LL_InitTimeBase(&c);
}

static void test_b1_ll(void)
{
    TstArea_t *a = TST_AREA;
    const uint32_t tb = cycles_of(b1l_tb);
    const uint32_t oc = cycles_of(b1l_oc);
    const uint32_t oe = cycles_of(b1l_oe);
    TIM2->SR = 0U;
    const uint32_t tk = cycles_of(b1l_take);
    (void)t14(TIM2, "B1");
    logw("B1 LL CM7 cycles (400 MHz): TakeFlags %lu, OutputEnable %lu, ConfigOutput %lu, InitTimeBase %lu",
        (unsigned long)tk, (unsigned long)oe, (unsigned long)oc, (unsigned long)tb);
    if (cm4_send(TST_CMD_B1, TST_TIER_LL))
    {
        logw("B1 LL CM4 cycles (200 MHz, TIM15): TakeFlags %lu, OutputEnable %lu, ConfigOutput %lu, InitTimeBase %lu",
            (unsigned long)a->m4_res[0], (unsigned long)a->m4_res[1], (unsigned long)a->m4_res[2],
            (unsigned long)a->m4_res[3]);
    }
}

/* CPU2's F2 tests (TIM15 through tim_ll), judged here as F1's. */
static void test_cm4_ll(void)
{
    TstArea_t *a = TST_AREA;
    if (cm4_send(TST_CMD_T1, TST_TIER_LL))
    {
        verdict((a->m4_res[TST_R_T1_BAD] == 0U) && (a->m4_res[TST_R_T1_TOUCHED] == 0U) && (a->m4_res[TST_R_T1_CASES] > 0U),
                "T1 LL CM4 TIM15: %lu refusals, wrong status 0x%04lX, snapshot changed 0x%04lX",
                (unsigned long)a->m4_res[TST_R_T1_CASES], (unsigned long)a->m4_res[TST_R_T1_BAD],
                (unsigned long)a->m4_res[TST_R_T1_TOUCHED]);
        for (uint32_t k = 0U; k < a->m4_res[TST_R_T1_CASES]; k++)
        {
            LOG("T1 LL CM4 case %lu: status %lu", (unsigned long)k, (unsigned long)a->m4_res[TST_R_T1_STATUS0 + k]);
        }
        verdict((a->m4_res[TST_R_T1_CTRL_ST] == TIM_BM_OK) && (a->m4_res[TST_R_T1_CTRL_DIFF] == a->m4_res[TST_R_T1_CTRL_EXP]),
                "T1 LL CM4 control (O1): ConfigOutput(TIM15 LL_TIM_CHANNEL_CH1) changes TIM15 words 0x%07lX (0x%07lX: "
                "CCMR1, CCR1), status %lu", (unsigned long)a->m4_res[TST_R_T1_CTRL_DIFF],
                (unsigned long)a->m4_res[TST_R_T1_CTRL_EXP], (unsigned long)a->m4_res[TST_R_T1_CTRL_ST]);
    }
    if (cm4_send(TST_CMD_T2, TST_TIER_LL))
    {
        const volatile uint32_t *r = a->m4_res;
        verdict((r[TST_R_T2_MISMATCH] == 0U) && (r[TST_R_T2_CALLS_BAD] == 0U) && (r[TST_R_T2_CTRL] == 1U) &&
                (r[TST_R_T2_CONFIGS] > 0U) && (r[TST_R_T2_PAR_CONFIGS] == r[TST_R_T2_CONFIGS]) &&
                (r[TST_R_T2_PAR_MISMATCH] == 0U) && (r[TST_R_T2_PAR_CTRL] == 1U),
                "T2 LL CM4 TIM15: %lu images, hand mismatches %lu, calls not OK %lu, altered image flagged %lu; parity with "
                "tim_bm: %lu compared, %lu differ (first: image %lu word 0x%02lX tim_ll 0x%08lX tim_bm 0x%08lX), altered "
                "tim_bm image flagged %lu", (unsigned long)r[TST_R_T2_CONFIGS], (unsigned long)r[TST_R_T2_MISMATCH],
                (unsigned long)r[TST_R_T2_CALLS_BAD], (unsigned long)r[TST_R_T2_CTRL], (unsigned long)r[TST_R_T2_PAR_CONFIGS],
                (unsigned long)r[TST_R_T2_PAR_MISMATCH], (unsigned long)r[TST_R_T2_PAR_FIRST_CFG],
                (unsigned long)(r[TST_R_T2_PAR_FIRST_REG] * 4U), (unsigned long)r[TST_R_T2_PAR_FIRST_LL],
                (unsigned long)r[TST_R_T2_PAR_FIRST_BM], (unsigned long)r[TST_R_T2_PAR_CTRL]);
    }
    if (cm4_send(TST_CMD_T5, TST_TIER_LL))
    {
        for (uint32_t m = 0U; m < 2U; m++)
        {
            const uint32_t ticks = a->m4_res[3U * m];
            const uint32_t n = a->m4_res[3U * m + 1U];
            const uint32_t span = a->m4_res[3U * m + 2U];
            const int64_t exp = (int64_t)(n - 1U) * ticks;
            const int64_t expc = (int64_t)(n - 1U) * ((m == 0U) ? 1500U : 1000U);
            const int32_t err = (n >= 3U) ? (int32_t)((((int64_t)span - exp) * 1000000) / exp) : 0;
            const int32_t ctrl = (n >= 3U) ? (int32_t)((((int64_t)span - expc) * 1000000) / expc) : 0;
            verdict((n >= 3U) && (err >= -100) && (err <= 100) && ((ctrl < -100) || (ctrl > 100)),
                    "T5 LL TIM15 (CM4) %s: %lu updates over %lu cycles, %lu ticks each: %+ld ppm (|e| <= 100); control: "
                    "%+ld ppm", (m == 0U) ? "(a) up, PSC=1 ARR=499" : "(b) RCR=3", (unsigned long)n, (unsigned long)span,
                    (unsigned long)ticks, (long)err, (long)ctrl);
        }
        verdict(a->m4_res[8] == 1U, "T14 LL CM4 TIM15 after T5: %lu", (unsigned long)a->m4_res[8]);
    }
    if (cm4_send(TST_CMD_T12, TST_TIER_LL))
    {
        verdict((a->m4_res[0] >= 999U) && (a->m4_res[0] <= 1001U) && (a->m4_res[2] == 0U) && (a->m4_res[3] == 1U) &&
                (a->m4_res[4] == 0U) && (a->m4_res[6] == 1U),
                "T12 LL TIM15 (CM4): (a) %lu update IRQs in %lu cycles (1000 +-1); (b) %lu spurious (0), control by hand "
                "%lu (1); UIE = 0: %lu (0); GetIRQn = TIM15_IRQn %lu", (unsigned long)a->m4_res[0],
                (unsigned long)a->m4_res[1], (unsigned long)a->m4_res[2], (unsigned long)a->m4_res[3],
                (unsigned long)a->m4_res[4], (unsigned long)a->m4_res[6]);
        verdict(a->m4_res[5] == 1U, "T14 LL CM4 TIM15 after T12: %lu", (unsigned long)a->m4_res[5]);
    }
}

/* The internal F2 tests, after F1's and before the AD2 steps. No wired pin moves. */
static void f2_internal(void)
{
    s_tr = &k_tst_tier_ll;
    test_t1_ll();
    test_t2_ll();
    test_t5();
    test_t10_ll();
    test_t12();
    test_b1_ll();
    test_cm4_ll();
    s_tr = &k_tst_tier_bm;
}

/* ==========================================================================================
 * F3: tim_hal (plan "Plan de pruebas", the F3 rows: T1, T2, T5, T6, T8, T9, T14; B1). CPU1
 * only: CPU2's HAL configuration has no HAL_TIM_MODULE_ENABLED (plan decision 5).
 *
 * tim_hal works on a TIM_HandleTypeDef: the HAL tier keeps one per CPU1 instance (s_hh) and
 * passes it to tim_hal. tim_hal has no counter start/stop and no capture disable: for those
 * the tier calls the HAL on the same handle (HAL_TIM_Base_Start, HAL_TIM_Base_Stop,
 * HAL_TIM_IC_Stop), so the handle states stay the HAL's own. The flags, the interrupt
 * enables and the capture registers go through tim_bm, as in the other tiers' oracles.
 * T1 calls tim_hal by hand; T2, T5 and T14 are F1's tests through the HAL tier, and T2 adds
 * the parity against tim_bm and the HAL's by-design differences. T6, T8 and T9 are the bench
 * steps T6_TIM1_HAL, T8_HAL and T9_HAL. TIM6 is CPU1's HAL tick: HAL_GetTick is checked
 * against CYCCNT before, between and after the HAL tests, and TIM6's registers are only read.
 * ========================================================================================== */

static TIM_HandleTypeDef s_hh[N_TIMS];
static TIM_HandleTypeDef s_hx;            /* any other address */

static TIM_HandleTypeDef *hh(TIM_TypeDef *t)
{
    for (uint32_t i = 0U; i < N_TIMS; i++)
    {
        if (k_tims[i].tim == t)
        {
            s_hh[i].Instance = t;
            return &s_hh[i];
        }
    }
    s_hx.Instance = t;
    return &s_hx;
}

static TimBm_Status_t hal_st(HAL_StatusTypeDef hs)
{
    return (hs == HAL_OK) ? TIM_BM_OK : TIM_BM_ERR_HAL;
}

static TimBm_Status_t hal_init_tb(const TimBm_TimeBaseConfig_t *c)
{
    return TIM_HAL_InitTimeBase(hh(c->tim), c);
}

static TimBm_Status_t hal_start(TIM_TypeDef *t)  { return hal_st(HAL_TIM_Base_Start(hh(t))); }
static TimBm_Status_t hal_stop(TIM_TypeDef *t)   { return hal_st(HAL_TIM_Base_Stop(hh(t))); }
static TimBm_Status_t hal_deinit(TIM_TypeDef *t) { return TIM_HAL_Deinit(hh(t)); }

static TimBm_Status_t hal_config_output(const TimBm_OcConfig_t *c)
{
    return TIM_HAL_ConfigOutput(hh(c->tim), c);
}

/* fn on every channel of a TIM_BM_CH() mask, in channel order, up to the first error. */
static TimBm_Status_t hal_each(TIM_TypeDef *t, uint32_t m, TimBm_Status_t (*fn)(TIM_HandleTypeDef *, uint32_t))
{
    TimBm_Status_t st = (m == 0U) ? TIM_BM_ERR_PARAM : TIM_BM_OK;
    for (uint32_t ch = 1U; (ch <= 4U) && (st == TIM_BM_OK); ch++)
    {
        if ((m & TIM_BM_CH(ch)) != 0U)
        {
            st = fn(hh(t), ch);
        }
    }
    return st;
}

static TimBm_Status_t hal_output_enable(TIM_TypeDef *t, uint32_t m)  { return hal_each(t, m, TIM_HAL_StartPwm); }
static TimBm_Status_t hal_output_disable(TIM_TypeDef *t, uint32_t m) { return hal_each(t, m, TIM_HAL_StopPwm); }
static TimBm_Status_t hal_capture_enable(TIM_TypeDef *t, uint32_t m) { return hal_each(t, m, TIM_HAL_StartCapture); }

static TimBm_Status_t hal_capture_disable(TIM_TypeDef *t, uint32_t m)
{
    TimBm_Status_t st = (m == 0U) ? TIM_BM_ERR_PARAM : TIM_BM_OK;
    for (uint32_t ch = 1U; (ch <= 4U) && (st == TIM_BM_OK); ch++)
    {
        if ((m & TIM_BM_CH(ch)) != 0U)
        {
            st = hal_st(HAL_TIM_IC_Stop(hh(t), (ch - 1U) * 4U));
        }
    }
    return st;
}

static TimBm_Status_t hal_config_input(const TimBm_IcConfig_t *c)
{
    return TIM_HAL_ConfigInput(hh(c->tim), c);
}

/* PWM input the HAL way (RM0399 40.3.8): TIti direct, the other channel indirect on the
   opposite edge, then the slave controller in reset mode on TIxFPx. */
static TimBm_Status_t hal_config_pwm_input(TIM_TypeDef *tim, uint32_t ti, uint32_t edge, uint32_t prescaler,
                                           uint32_t filter, uint32_t tisel)
{
    const TimBm_IcConfig_t a = { tim, ti, TIM_BM_IC_DIRECT, edge, prescaler, filter, tisel };
    const TimBm_IcConfig_t b = { tim, (ti == 1U) ? 2U : 1U, TIM_BM_IC_INDIRECT,
                                 (edge == TIM_BM_EDGE_RISING) ? TIM_BM_EDGE_FALLING : TIM_BM_EDGE_RISING,
                                 prescaler, filter, 0U };
    TimBm_Status_t st = TIM_HAL_ConfigInput(hh(tim), &a);
    if (st == TIM_BM_OK)
    {
        st = TIM_HAL_ConfigInput(hh(tim), &b);
    }
    if (st == TIM_BM_OK)
    {
        st = TIM_HAL_ConfigSlave(hh(tim), TIM_BM_SMS_RESET, (ti == 1U) ? TIM_BM_TRG_TI1FP1 : TIM_BM_TRG_TI2FP2);
    }
    return st;
}

static uint32_t hal_read_capture(TIM_TypeDef *t, uint32_t channel)
{
    return TIM_BM_ReadCapture(t, channel);
}

static TimBm_Status_t hal_config_master(TIM_TypeDef *t, uint32_t mms, bool msm)
{
    return TIM_HAL_ConfigMaster(hh(t), mms, msm);
}

static TimBm_Status_t hal_config_slave(TIM_TypeDef *t, uint32_t sms, uint32_t trigger)
{
    return TIM_HAL_ConfigSlave(hh(t), sms, trigger);
}

static const TstTier_t k_tier_hal = {
    "HAL", " HAL",
    hal_init_tb, hal_start, hal_stop, hal_deinit, TIM_BM_EnableIrq, TIM_BM_DisableIrq,
    TIM_BM_TakeFlags, TIM_BM_TakeOvercapture, TIM_BM_WaitFlags, hal_config_output, hal_output_enable,
    hal_output_disable, hal_config_input, hal_config_pwm_input, hal_capture_enable, hal_capture_disable,
    hal_read_capture, hal_config_master, hal_config_slave,
};

/* ---- TIM6, the HAL tick of CPU1 (plan F3 gate, A22) ------------------------------------ */

/* Over 100 ms of CYCCNT (both clocks from PLL1), HAL_GetTick must advance 100 +-1 ms. */
static void hal_tick_check(const char *when)
{
    const uint32_t c0 = DWT->CYCCNT;
    const uint32_t t0 = HAL_GetTick();
    while ((DWT->CYCCNT - c0) < (100U * CYC_PER_MS))
    {
    }
    const uint32_t d = HAL_GetTick() - t0;
    verdict((d >= 99U) && (d <= 101U), "TIM6 tick %s: HAL_GetTick advanced %lu ms in 100 ms of CYCCNT (100 +-1)", when,
            (unsigned long)d);
}

typedef struct { uint32_t cr1, cr2, dier, psc, arr; } Tim6Snap_t;
static Tim6Snap_t s_tim6_f3;

static void tim6_snap(Tim6Snap_t *s)           /* reads only */
{
    s->cr1  = TIM6->CR1;
    s->cr2  = TIM6->CR2;
    s->dier = TIM6->DIER;
    s->psc  = TIM6->PSC;
    s->arr  = TIM6->ARR;
}

/* ---- T1 through tim_hal --------------------------------------------------------------- */

static const T1Case_t k_t1_hal[] = {
    /*  0 */ { "InitTimeBase(h NULL)",                                         TIM_BM_ERR_PARAM,       0U },
    /*  1 */ { "InitTimeBase(cfg NULL)",                                       TIM_BM_ERR_PARAM,       0U },
    /*  2 */ { "InitTimeBase(h->Instance USART3)",                             TIM_BM_ERR_PARAM,       0U },
    /*  3 */ { "InitTimeBase(h->Instance TIM1 + 4)",                           TIM_BM_ERR_PARAM,       0U },
    /*  4 */ { "InitTimeBase(h->Instance TIM2, cfg->tim TIM4)",                TIM_BM_ERR_PARAM,       0U },
    /*  5 */ { "ConfigOutput(h->Instance TIM2, cfg->tim TIM4)",                TIM_BM_ERR_PARAM,       0U },
    /*  6 */ { "ConfigInput(h->Instance TIM2, cfg->tim TIM4)",                 TIM_BM_ERR_PARAM,       0U },
    /*  7 */ { "ConfigOutput(cfg NULL)",                                       TIM_BM_ERR_PARAM,       0U },
    /*  8 */ { "StartPwm(h NULL)",                                             TIM_BM_ERR_PARAM,       0U },
    /*  9 */ { "StartCapture(h->Instance TIM1 + 4)",                           TIM_BM_ERR_PARAM,       0U },
    /* 10 */ { "ConfigMaster(h->Instance USART3)",                             TIM_BM_ERR_PARAM,       0U },
    /* 11 */ { "ConfigSlave(h NULL)",                                          TIM_BM_ERR_PARAM,       0U },
    /* 12 */ { "Deinit(h NULL)",                                               TIM_BM_ERR_PARAM,       0U },
    /* 13 */ { "Deinit(h->Instance USART3)",                                   TIM_BM_ERR_PARAM,       0U },
    /* 14 */ { "ConfigOutput(TIM2 channel 0)",                                 TIM_BM_ERR_PARAM,       0U },
    /* 15 */ { "ConfigOutput(TIM2 channel 5)",                                 TIM_BM_ERR_PARAM,       0U },
    /* 16 */ { "ConfigOutput(TIM12 channel 3)",                                TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 17 */ { "ConfigOutput(TIM7 channel 1): PWM on TIM7",                    TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 18 */ { "InitTimeBase(TIM2 ARR = 0)",                                   TIM_BM_ERR_PARAM,       0U },
    /* 19 */ { "InitTimeBase(TIM4 ARR = 0x10000, 16 bits)",                    TIM_BM_ERR_PARAM,       0U },
    /* 20 */ { "InitTimeBase(TIM2 PSC = 65536)",                               TIM_BM_ERR_PARAM,       0U },
    /* 21 */ { "InitTimeBase(TIM17 repetition = 0x100, 8-bit RCR)",            TIM_BM_ERR_PARAM,       0U },
    /* 22 */ { "InitTimeBase(TIM12 center 1)",                                 TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 23 */ { "ConfigSlave(TIM4 trigger ITR5)",                               TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 24 */ { "ConfigSlave(TIM2 trigger 19: no ETRF value)",                  TIM_BM_ERR_PARAM,       0U },
    /* 25 */ { "ConfigSlave(TIM2 gated on TI1F_ED)",                           TIM_BM_ERR_PARAM,       0U },
    /* 26 */ { "ConfigInput(TIM17 TI1SEL = 0001, reserved)",                   TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 27 */ { "2.16.1 ConfigMaster(TIM2 MSM) with OPM, reset+trigger",        TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 28 */ { "2.16.1 ConfigSlave(TIM2 reset+trigger) with OPM, MSM",         TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 29 */ { "2.16.1 InitTimeBase(TIM2 OPM) with MSM, reset+trigger",        TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 30 */ { "ConfigOutput(TIM2 CH1) with CC1E = 1 (StartPwm)",              TIM_BM_ERR_BUSY,        1U },
    /* 31 */ { "ConfigInput(TIM2 CH2) with CC2E = 1 (StartCapture)",           TIM_BM_ERR_BUSY,        1U },
    /* 32 */ { "InitTimeBase(TIM2) with CEN = 1",                              TIM_BM_ERR_BUSY,        1U },
    /* 33 */ { "ConfigSlave(TIM2 trigger) with SMS = gated",                   TIM_BM_ERR_BUSY,        0U },
    /* 34 */ { "StartPwm(TIM2 CH1) on an input",                               TIM_BM_ERR_PARAM,       0U },
    /* 35 */ { "StartCapture(TIM2 CH1) on an output",                          TIM_BM_ERR_PARAM,       0U },
    /* 36 */ { "StartPwm(TIM2 channel 0)",                                     TIM_BM_ERR_PARAM,       0U },
    /* 37 */ { "StopPwm(TIM2 channel 5)",                                      TIM_BM_ERR_PARAM,       0U },
    /* 38 */ { "StartCapture(TIM7 CH1): no channel",                           TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 39 */ { "ConfigMaster(TIM12 OC3REF > MMS 5)",                           TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 40 */ { "ConfigMaster(TIM13: no TRGO)",                                 TIM_BM_ERR_UNSUPPORTED, 0U },
    /* 41 */ { "StartPwm(TIM2 CH1) again: HAL_TIM_PWM_Start refuses (channel BUSY)", TIM_BM_ERR_HAL,  1U },
};
#define T1_HAL_CASES   (sizeof(k_t1_hal) / sizeof(k_t1_hal[0]))

static TIM_HandleTypeDef  s_t1_hb;      /* the case's handle before and after the call */
static TIM_HandleTypeDef  s_t1_ha;
static TIM_HandleTypeDef  s_t1_bad;     /* a handle on something that is not a timer */
static TIM_HandleTypeDef *s_t1_hp;      /* the case's handle, NULL if it has none */

static TimBm_Status_t t1_hal_case(uint32_t k)
{
    TimBm_TimeBaseConfig_t tb = tb_cfg(TIM2, 0U, 999U, TIM_BM_COUNT_UP, false);
    TimBm_OcConfig_t oc = { TIM2, 1U, TIM_BM_OC_PWM1, 100U, false, true };
    TimBm_IcConfig_t ic = { TIM2, 2U, TIM_BM_IC_DIRECT, TIM_BM_EDGE_RISING, 1U, 0U, 0U };
    TIM_HandleTypeDef *const h2 = hh(TIM2);
    TimBm_Status_t st = TIM_BM_OK;
    memset(&s_t1_bad, 0, sizeof(s_t1_bad));
#define T1H_CALL(hp, expr)                                                                   \
    do {                                                                                     \
        s_t1_hp = (hp);                                                                      \
        if (s_t1_hp != NULL) { memcpy(&s_t1_hb, s_t1_hp, sizeof(s_t1_hb)); }                 \
        t1_snap(&s_t1_b);                                                                    \
        st = (expr);                                                                         \
        t1_snap(&s_t1_a);                                                                    \
        if (s_t1_hp != NULL) { memcpy(&s_t1_ha, s_t1_hp, sizeof(s_t1_ha)); }                 \
    } while (0)
    switch (k)
    {
    case 0:  T1H_CALL(NULL, TIM_HAL_InitTimeBase(NULL, &tb)); break;
    case 1:  T1H_CALL(h2, TIM_HAL_InitTimeBase(h2, NULL)); break;
    case 2:
        s_t1_bad.Instance = (TIM_TypeDef *)USART3; tb.tim = s_t1_bad.Instance;
        T1H_CALL(&s_t1_bad, TIM_HAL_InitTimeBase(&s_t1_bad, &tb));
        break;
    case 3:
        s_t1_bad.Instance = (TIM_TypeDef *)(TIM1_BASE + 4U); tb.tim = s_t1_bad.Instance;
        T1H_CALL(&s_t1_bad, TIM_HAL_InitTimeBase(&s_t1_bad, &tb));
        break;
    case 4:  tb.tim = TIM4; T1H_CALL(h2, TIM_HAL_InitTimeBase(h2, &tb)); break;
    case 5:  oc.tim = TIM4; T1H_CALL(h2, TIM_HAL_ConfigOutput(h2, &oc)); break;
    case 6:  ic.tim = TIM4; T1H_CALL(h2, TIM_HAL_ConfigInput(h2, &ic)); break;
    case 7:  T1H_CALL(h2, TIM_HAL_ConfigOutput(h2, NULL)); break;
    case 8:  T1H_CALL(NULL, TIM_HAL_StartPwm(NULL, 1U)); break;
    case 9:
        s_t1_bad.Instance = (TIM_TypeDef *)(TIM1_BASE + 4U);
        T1H_CALL(&s_t1_bad, TIM_HAL_StartCapture(&s_t1_bad, 1U));
        break;
    case 10:
        s_t1_bad.Instance = (TIM_TypeDef *)USART3;
        T1H_CALL(&s_t1_bad, TIM_HAL_ConfigMaster(&s_t1_bad, TIM_BM_MMS_UPDATE, false));
        break;
    case 11: T1H_CALL(NULL, TIM_HAL_ConfigSlave(NULL, TIM_BM_SMS_RESET, TIM_BM_TRG_ITR(0))); break;
    case 12: T1H_CALL(NULL, TIM_HAL_Deinit(NULL)); break;
    case 13:
        s_t1_bad.Instance = (TIM_TypeDef *)USART3;
        T1H_CALL(&s_t1_bad, TIM_HAL_Deinit(&s_t1_bad));
        break;
    case 14: oc.channel = 0U; T1H_CALL(h2, TIM_HAL_ConfigOutput(h2, &oc)); break;
    case 15: oc.channel = 5U; T1H_CALL(h2, TIM_HAL_ConfigOutput(h2, &oc)); break;
    case 16: oc.tim = TIM12; oc.channel = 3U; T1H_CALL(hh(TIM12), TIM_HAL_ConfigOutput(hh(TIM12), &oc)); break;
    case 17: oc.tim = TIM7; T1H_CALL(hh(TIM7), TIM_HAL_ConfigOutput(hh(TIM7), &oc)); break;
    case 18: tb.arr = 0U; T1H_CALL(h2, TIM_HAL_InitTimeBase(h2, &tb)); break;
    case 19: tb.tim = TIM4; tb.arr = 0x10000U; T1H_CALL(hh(TIM4), TIM_HAL_InitTimeBase(hh(TIM4), &tb)); break;
    case 20: tb.psc = 65536U; T1H_CALL(h2, TIM_HAL_InitTimeBase(h2, &tb)); break;
    case 21: tb.tim = TIM17; tb.repetition = 0x100U; T1H_CALL(hh(TIM17), TIM_HAL_InitTimeBase(hh(TIM17), &tb)); break;
    case 22:
        tb.tim = TIM12; tb.count_mode = TIM_BM_COUNT_CENTER1;
        T1H_CALL(hh(TIM12), TIM_HAL_InitTimeBase(hh(TIM12), &tb));
        break;
    case 23: T1H_CALL(hh(TIM4), TIM_HAL_ConfigSlave(hh(TIM4), TIM_BM_SMS_TRIGGER, TIM_BM_TRG_ITR(5))); break;
    case 24: T1H_CALL(h2, TIM_HAL_ConfigSlave(h2, TIM_BM_SMS_TRIGGER, 19U)); break;
    case 25: T1H_CALL(h2, TIM_HAL_ConfigSlave(h2, TIM_BM_SMS_GATED, TIM_BM_TRG_TI1F_ED)); break;
    case 26:
        ic.tim = TIM17; ic.channel = 1U; ic.tisel = 1U;
        T1H_CALL(hh(TIM17), TIM_HAL_ConfigInput(hh(TIM17), &ic));
        break;
    case 27:
        tb.one_pulse = true;
        (void)TIM_HAL_InitTimeBase(h2, &tb);
        (void)TIM_HAL_ConfigSlave(h2, TIM_BM_SMS_RESET_TRIGGER, TIM_BM_TRG_ITR(0));
        T1H_CALL(h2, TIM_HAL_ConfigMaster(h2, TIM_BM_MMS_RESET, true));
        break;
    case 28:
        tb.one_pulse = true;
        (void)TIM_HAL_InitTimeBase(h2, &tb);
        (void)TIM_HAL_ConfigMaster(h2, TIM_BM_MMS_RESET, true);
        T1H_CALL(h2, TIM_HAL_ConfigSlave(h2, TIM_BM_SMS_RESET_TRIGGER, TIM_BM_TRG_ITR(0)));
        break;
    case 29:
        (void)TIM_HAL_InitTimeBase(h2, &tb);
        (void)TIM_HAL_ConfigMaster(h2, TIM_BM_MMS_RESET, true);
        (void)TIM_HAL_ConfigSlave(h2, TIM_BM_SMS_RESET_TRIGGER, TIM_BM_TRG_ITR(0));
        tb.one_pulse = true;
        T1H_CALL(h2, TIM_HAL_InitTimeBase(h2, &tb));
        break;
    case 30:
        (void)TIM_HAL_InitTimeBase(h2, &tb);
        (void)TIM_HAL_ConfigOutput(h2, &oc);
        (void)TIM_HAL_StartPwm(h2, 1U);
        T1H_CALL(h2, TIM_HAL_ConfigOutput(h2, &oc));
        break;
    case 31:
        (void)TIM_HAL_InitTimeBase(h2, &tb);
        (void)TIM_HAL_ConfigInput(h2, &ic);
        (void)TIM_HAL_StartCapture(h2, 2U);
        T1H_CALL(h2, TIM_HAL_ConfigInput(h2, &ic));
        break;
    case 32:
        (void)TIM_HAL_InitTimeBase(h2, &tb);
        (void)TIM_BM_Start(TIM2);              /* tim_hal has no Start of the time base */
        T1H_CALL(h2, TIM_HAL_InitTimeBase(h2, &tb));
        break;
    case 33:
        (void)TIM_HAL_ConfigSlave(h2, TIM_BM_SMS_GATED, TIM_BM_TRG_ITR(0));
        T1H_CALL(h2, TIM_HAL_ConfigSlave(h2, TIM_BM_SMS_TRIGGER, TIM_BM_TRG_ITR(1)));
        break;
    case 34:
        ic.channel = 1U;
        (void)TIM_HAL_ConfigInput(h2, &ic);
        T1H_CALL(h2, TIM_HAL_StartPwm(h2, 1U));
        break;
    case 35:
        (void)TIM_HAL_ConfigOutput(h2, &oc);
        T1H_CALL(h2, TIM_HAL_StartCapture(h2, 1U));
        break;
    case 36: T1H_CALL(h2, TIM_HAL_StartPwm(h2, 0U)); break;
    case 37: T1H_CALL(h2, TIM_HAL_StopPwm(h2, 5U)); break;
    case 38: T1H_CALL(hh(TIM7), TIM_HAL_StartCapture(hh(TIM7), 1U)); break;
    case 39: T1H_CALL(hh(TIM12), TIM_HAL_ConfigMaster(hh(TIM12), TIM_BM_MMS_OC3REF, false)); break;
    case 40: T1H_CALL(hh(TIM13), TIM_HAL_ConfigMaster(hh(TIM13), TIM_BM_MMS_UPDATE, false)); break;
    default:
        (void)TIM_HAL_InitTimeBase(h2, &tb);
        (void)TIM_HAL_ConfigOutput(h2, &oc);
        (void)TIM_HAL_StartPwm(h2, 1U);
        T1H_CALL(h2, TIM_HAL_StartPwm(h2, 1U));
        break;
    }
#undef T1H_CALL
    return st;
}

static void test_t1_hal(void)
{
    uint64_t bad = 0U;
    uint64_t touched = 0U;
    uint64_t hchg = 0U;
    for (uint32_t k = 0U; k < T1_HAL_CASES; k++)
    {
        uint32_t tm[T1_NT];
        uint32_t rcc = 0U;
        const TimBm_Status_t st = t1_hal_case(k);
        t1_diff(tm, &rcc, k_t1_hal[k].running != 0U);
        bool same = (rcc == 0U);
        for (uint32_t j = 0U; j < T1_NT; j++)
        {
            same = same && (tm[j] == 0U);
        }
        const bool hsame = (s_t1_hp == NULL) || (memcmp(&s_t1_hb, &s_t1_ha, sizeof(s_t1_hb)) == 0);
        if (st != (TimBm_Status_t)k_t1_hal[k].expect)
        {
            bad |= 1ULL << k;
        }
        if (!same)
        {
            touched |= 1ULL << k;
        }
        if (!hsame)
        {
            hchg |= 1ULL << k;
        }
        LOG("T1 HAL %lu %s: status %u (expected %u), registers and RCC banks unchanged %u, handle unchanged %u",
            (unsigned long)k, k_t1_hal[k].what, (unsigned)st, (unsigned)k_t1_hal[k].expect, (unsigned)same,
            (unsigned)hsame);
        for (uint32_t j = 0U; j < T1_NT; j++)
        {
            (void)TIM_HAL_Deinit(hh(k_t1_tims[j]));
        }
        (void)TIM_HAL_Deinit(hh(TIM13));
    }
    verdict((bad == 0U) && (touched == 0U) && (hchg == 0U),
            "T1 HAL CM7 %u refusals: wrong status 0x%08lX%08lX, snapshot changed 0x%08lX%08lX, handle changed "
            "0x%08lX%08lX", (unsigned)T1_HAL_CASES, (unsigned long)(bad >> 32), (unsigned long)(bad & 0xFFFFFFFFU),
            (unsigned long)(touched >> 32), (unsigned long)(touched & 0xFFFFFFFFU), (unsigned long)(hchg >> 32),
            (unsigned long)(hchg & 0xFFFFFFFFU));

    /* O1: the valid call changes exactly CCMR1 and CCR1 of TIM2 */
    uint32_t tm[T1_NT];
    uint32_t rcc = 0U;
    TIM_HandleTypeDef *const h = hh(TIM2);
    const TimBm_OcConfig_t oc = { TIM2, 1U, TIM_BM_OC_PWM1, 100U, false, true };
    t1_snap(&s_t1_b);
    const TimBm_Status_t st1 = TIM_HAL_ConfigOutput(h, &oc);
    t1_snap(&s_t1_a);
    t1_diff(tm, &rcc, false);
    const uint32_t exp_tim2 = (1UL << TST_RI_CCMR1) | (1UL << TST_RI_CCR1);
    const uint32_t others = tm[0] | tm[2] | tm[3] | tm[4] | tm[5] | rcc;
    verdict((st1 == TIM_BM_OK) && (tm[1] == exp_tim2) && (others == 0U),
            "T1 HAL control (O1): ConfigOutput(TIM2 CH1 PWM1 CCR 100 preload) status %u changes TIM2 words 0x%07lX "
            "(0x%07lX: CCMR1, CCR1), other timers and RCC 0x%lX (0)", (unsigned)st1, (unsigned long)tm[1],
            (unsigned long)exp_tim2, (unsigned long)others);
    (void)TIM_HAL_Deinit(h);

    /* The handle: the states the HAL keeps in it, through tim_hal. */
    const TimBm_TimeBaseConfig_t tb = tb_cfg(TIM2, 0U, 999U, TIM_BM_COUNT_UP, false);
    const TimBm_Status_t sa = TIM_HAL_InitTimeBase(h, &tb);
    const uint32_t s_init = h->State;
    bool ch_ready = true;
    for (uint32_t i = 0U; i < 4U; i++)
    {
        ch_ready = ch_ready && (h->ChannelState[i] == HAL_TIM_CHANNEL_STATE_READY) &&
                   (h->ChannelNState[i] == HAL_TIM_CHANNEL_STATE_READY);
    }
    const TimBm_Status_t sb = TIM_HAL_ConfigOutput(h, &oc);
    const TimBm_Status_t sc = TIM_HAL_StartPwm(h, 1U);
    const uint32_t c_on = h->ChannelState[0];
    const TimBm_Status_t sd = TIM_HAL_StopPwm(h, 1U);
    const uint32_t c_off = h->ChannelState[0];
    const TimBm_Status_t se = TIM_HAL_Deinit(h);
    bool reset = (h->State == HAL_TIM_STATE_RESET) && (h->DMABurstState == HAL_DMA_BURST_STATE_RESET) &&
                 (h->Lock == HAL_UNLOCKED);
    for (uint32_t i = 0U; i < 4U; i++)
    {
        reset = reset && (h->ChannelState[i] == HAL_TIM_CHANNEL_STATE_RESET) &&
                (h->ChannelNState[i] == HAL_TIM_CHANNEL_STATE_RESET);
    }
    verdict((sa == TIM_BM_OK) && (s_init == HAL_TIM_STATE_READY) && ch_ready && (sb == TIM_BM_OK) && (sc == TIM_BM_OK) &&
            (c_on == HAL_TIM_CHANNEL_STATE_BUSY) && (sd == TIM_BM_OK) && (c_off == HAL_TIM_CHANNEL_STATE_READY) &&
            (se == TIM_BM_OK) && reset,
            "T1 HAL handle (TIM2): InitTimeBase %u -> State %lu (READY 1), channels READY %u; ConfigOutput %u; StartPwm "
            "CH1 %u -> channel state %lu (BUSY 2); StopPwm %u -> %lu (READY 1); Deinit %u -> State, channels, DMA burst "
            "RESET and unlocked %u", (unsigned)sa, (unsigned long)s_init, (unsigned)ch_ready, (unsigned)sb,
            (unsigned)sc, (unsigned long)c_on, (unsigned)sd, (unsigned long)c_off, (unsigned)se, (unsigned)reset);
}

/* ---- T2 through tim_hal: parity, then the HAL's by-design differences ------------------ */

/* Each case runs from the same image twice, through tim_bm and through tim_hal: Deinit, a
   preparation through the same tier and, where the difference needs it, a register written
   by hand, then the operation. Every bit in which the two images differ must be the one
   listed for the case (CNT aside): a bit the HAL leaves or clears where tim_bm writes the
   reset value, or the counter the HAL start/stop functions also drive. */
typedef struct { const char *what; TIM_TypeDef *tim; uint8_t word; uint32_t bits; } T2dCase_t;

static const T2dCase_t k_t2d[] = {
    /*  0 */ { "InitTimeBase over CR1 UDIS = UIFREMAP = 1 (TIM_Base_SetConfig keeps them)", TIM2, TST_RI_CR1,
               TIM_CR1_UDIS | TIM_CR1_UIFREMAP },
    /*  1 */ { "InitTimeBase over SR.CC1IF = 1 (tim_hal clears UIF only; tim_bm SR = 0)", TIM2, TST_RI_SR, TIM_SR_CC1IF },
    /*  2 */ { "InitTimeBase over TIM12 CR1 CKD = 10 (no IS_TIM_CLOCK_DIVISION_INSTANCE)", TIM12, TST_RI_CR1,
               TIM_CR1_CKD_1 },
    /*  3 */ { "InitTimeBase down over center 1 (R5-3: a first Base_Init in up mode)", TIM2, TST_RI_CR1, 0U },
    /*  4 */ { "ConfigOutput toggle over OC1CE = OC1FE = 1 (TIM_OC1_SetConfig keeps them)", TIM2, TST_RI_CCMR1,
               TIM_CCMR1_OC1CE | TIM_CCMR1_OC1FE },
    /*  5 */ { "ConfigOutput PWM1 over OC1CE = 1 (the PWM call clears OC1FE only)", TIM2, TST_RI_CCMR1, TIM_CCMR1_OC1CE },
    /*  6 */ { "ConfigOutput over CC1NP = 1, TIM2 CH1 (no complementary output)", TIM2, TST_RI_CCER, TIM_CCER_CC1NP },
    /*  7 */ { "ConfigOutput over CC1NP = 1, TIM1 CH1 (complementary output: cleared)", TIM1, TST_RI_CCER, 0U },
    /*  8 */ { "ConfigOutput over CR2 OIS1 = 1, TIM1 (break instance: OISx cleared)", TIM1, TST_RI_CR2, TIM_CR2_OIS1 },
    /*  9 */ { "ConfigMaster over CR2 MMS2 = 0001, TIM1 (MasterOutputTrigger2 = 0)", TIM1, TST_RI_CR2, TIM_CR2_MMS2_0 },
    /* 10 */ { "ConfigSlave over DIER TIE = TDE = 1 (HAL_TIM_SlaveConfigSynchro clears them)", TIM2, TST_RI_DIER,
               TIM_DIER_TIE | TIM_DIER_TDE },
    /* 11 */ { "ConfigSlave reset on TI1FP1 over CH1 input, falling, filter 7 (stage kept)", TIM2, TST_RI_CCER, 0U },
    /* 12 */ { "StartCapture CH1 against CaptureEnable (HAL_TIM_IC_Start sets CEN)", TIM2, TST_RI_CR1, TIM_CR1_CEN },
    /* 13 */ { "StartPwm CH1 against OutputEnable (HAL_TIM_PWM_Start sets CEN)", TIM2, TST_RI_CR1, TIM_CR1_CEN },
    /* 14 */ { "StopPwm CH1 against OutputDisable, TIM1 (no channel left: CEN cleared)", TIM1, TST_RI_CR1, TIM_CR1_CEN },
};
#define T2D_CASES   (sizeof(k_t2d) / sizeof(k_t2d[0]))

static TimBm_Status_t t2d_apply(uint32_t k, const TstTier_t *tier)
{
    TIM_TypeDef *t = k_t2d[k].tim;
    const TimBm_TimeBaseConfig_t tb = tb_cfg(t, 7U, 999U, TIM_BM_COUNT_UP, false);
    const TimBm_TimeBaseConfig_t slow = tb_cfg(t, 65535U, 0xFFFFU, TIM_BM_COUNT_UP, false);   /* no update for 21 s */
    TimBm_OcConfig_t oc = { t, 1U, TIM_BM_OC_PWM1, 0x1234U, false, true };
    TimBm_IcConfig_t ic = { t, 1U, TIM_BM_IC_DIRECT, TIM_BM_EDGE_RISING, 1U, 0U, 0U };
    (void)tier->deinit(t);
    switch (k)
    {
    case 0:  t->CR1 = TIM_CR1_UDIS | TIM_CR1_UIFREMAP; return tier->init_tb(&tb);
    case 1:  t->EGR = TIM_EGR_CC1G; return tier->init_tb(&tb);     /* CC1 an output: CC1G sets CC1IF */
    case 2:  t->CR1 = TIM_CR1_CKD_1; return tier->init_tb(&tb);
    case 3:
    {
        const TimBm_TimeBaseConfig_t c1 = tb_cfg(t, 7U, 999U, TIM_BM_COUNT_CENTER1, false);
        const TimBm_TimeBaseConfig_t dn = tb_cfg(t, 7U, 999U, TIM_BM_COUNT_DOWN, false);
        (void)tier->init_tb(&c1);
        return tier->init_tb(&dn);
    }
    case 4:  t->CCMR1 = TIM_CCMR1_OC1CE | TIM_CCMR1_OC1FE; oc.mode = TIM_BM_OC_TOGGLE; oc.preload = false;
             return tier->config_output(&oc);
    case 5:  t->CCMR1 = TIM_CCMR1_OC1CE; return tier->config_output(&oc);
    case 6:
    case 7:  t->CCER = TIM_CCER_CC1NP; return tier->config_output(&oc);
    case 8:  t->CR2 = TIM_CR2_OIS1; return tier->config_output(&oc);
    case 9:  t->CR2 = TIM_CR2_MMS2_0; return tier->config_master(t, TIM_BM_MMS_UPDATE, false);
    case 10: t->DIER = TIM_DIER_TIE | TIM_DIER_TDE; return tier->config_slave(t, TIM_BM_SMS_TRIGGER, TIM_BM_TRG_ITR(0));
    case 11:
        ic.edge = TIM_BM_EDGE_FALLING; ic.filter = 7U;
        (void)tier->config_input(&ic);
        return tier->config_slave(t, TIM_BM_SMS_RESET, TIM_BM_TRG_TI1FP1);
    case 12:
        (void)tier->init_tb(&slow);
        (void)tier->config_input(&ic);
        return tier->capture_enable(t, TIM_BM_CH(1));
    case 13:
        (void)tier->init_tb(&slow);
        (void)tier->config_output(&oc);
        return tier->output_enable(t, TIM_BM_CH(1));
    default:
        (void)tier->init_tb(&slow);
        (void)tier->config_output(&oc);
        (void)tier->output_enable(t, TIM_BM_CH(1));
        (void)tier->start(t);
        return tier->output_disable(t, TIM_BM_CH(1));
    }
}

static void t2_hal_design(void)
{
    static TstImg_t bm;
    static TstImg_t hal;
    uint32_t bad = 0U;
    for (uint32_t k = 0U; k < T2D_CASES; k++)
    {
        const T2dCase_t *c = &k_t2d[k];
        const TimBm_Status_t sb = t2d_apply(k, &k_tst_tier_bm);
        tst_img_read(c->tim, &bm);
        (void)TIM_BM_Deinit(c->tim);
        const TimBm_Status_t sh = t2d_apply(k, &k_tier_hal);
        tst_img_read(c->tim, &hal);
        (void)TIM_HAL_Deinit(hh(c->tim));
        uint32_t got = 0U;
        uint32_t other_w = TST_RI_N;        /* first word other than the listed one that differs */
        for (uint32_t i = 0U; i < TST_RI_N; i++)
        {
            const uint32_t x = bm.v[i] ^ hal.v[i];
            if (i == c->word)
            {
                got = x;
            }
            else if ((i != TST_RI_CNT) && (x != 0U) && (other_w == TST_RI_N))
            {
                other_w = i;
            }
        }
        const bool ok = (sb == TIM_BM_OK) && (sh == TIM_BM_OK) && (got == c->bits) && (other_w == TST_RI_N);
        if (!ok)
        {
            bad |= 1UL << k;
        }
        verdict(ok, "T2 HAL by design %lu, %s %s: tim_bm %u, tim_hal %u; %s differs in 0x%08lX (listed 0x%08lX), other "
                "words differing: %s", (unsigned long)k, tim_name(c->tim), c->what, (unsigned)sb, (unsigned)sh,
                k_ri_name[c->word], (unsigned long)got, (unsigned long)c->bits,
                (other_w == TST_RI_N) ? "none" : k_ri_name[other_w]);
    }
    logw("T2 HAL by design: %lu cases from a non-reset image, failing 0x%04lX", (unsigned long)T2D_CASES,
        (unsigned long)bad);
}

static void test_t2_hal(void)
{
    uint32_t total = 0U;
    uint32_t par = 0U;
    uint32_t bad = 0U;
    for (uint32_t i = 0U; i < N_TIMS; i++)
    {
        TstT2Result_t r;
        tst_t2_run(k_tims[i].tim, &k_tier_hal, NULL, &r);
        total += r.configs;
        par += r.par_configs;
        const bool ok = (r.mismatches == 0U) && (r.calls_bad == 0U) && (r.ctrl_detected == 1U) && (r.configs > 0U) &&
                        (r.par_configs == r.configs) && (r.par_mismatches == 0U) && (r.par_ctrl == 1U);
        if (!ok)
        {
            bad |= 1UL << i;
        }
        logw("T2 HAL %s: %lu images (time base %lu, OC %lu, IC %lu, master %lu, slave %lu), hand mismatches %lu, calls "
            "not OK %lu, altered image flagged %lu; parity with tim_bm: %lu compared, %lu differ, altered tim_bm image "
            "flagged %lu%s", k_tims[i].name, (unsigned long)r.configs, (unsigned long)r.per_class[0],
            (unsigned long)r.per_class[1], (unsigned long)r.per_class[2], (unsigned long)r.per_class[3],
            (unsigned long)r.per_class[4], (unsigned long)r.mismatches, (unsigned long)r.calls_bad,
            (unsigned long)r.ctrl_detected, (unsigned long)r.par_configs, (unsigned long)r.par_mismatches,
            (unsigned long)r.par_ctrl, ok ? "" : " <-");
        if (r.mismatches != 0U)
        {
            logw("T2 HAL %s first hand mismatch: %s image %lu, %s reads 0x%08lX, hand encoding 0x%08lX", k_tims[i].name,
                k_cls_name[r.first_cls], (unsigned long)r.first_cfg, k_ri_name[r.first_idx], (unsigned long)r.first_got,
                (unsigned long)r.first_exp);
        }
        if (r.par_mismatches != 0U)
        {
            logw("T2 HAL %s first parity difference: image %lu, %s tim_hal 0x%08lX, tim_bm 0x%08lX", k_tims[i].name,
                (unsigned long)r.par_first_cfg, k_ri_name[r.par_first_idx], (unsigned long)r.par_first_tier,
                (unsigned long)r.par_first_bm);
        }
        (void)t14(k_tims[i].tim, "T2");
    }
    verdict(bad == 0U, "T2 HAL CM7 register images: %lu configurations on 12 instances against the hand encoding, %lu "
            "compared bit for bit with tim_bm's (parity), instances failing 0x%04lX", (unsigned long)total,
            (unsigned long)par, (unsigned long)bad);
    t2_hal_design();
}

/* ---- B1 through tim_hal (log only) ---------------------------------------------------- */

static TimBm_Status_t b1h_sp(void) { return TIM_HAL_StartPwm(hh(TIM2), 1U); }
static TimBm_Status_t b1h_oc(void)
{
    const TimBm_OcConfig_t oc = { TIM2, 1U, TIM_BM_OC_PWM1, 100U, false, true };
    return TIM_HAL_ConfigOutput(hh(TIM2), &oc);
}
static TimBm_Status_t b1h_tb(void)
{
    const TimBm_TimeBaseConfig_t c = { TIM2, 0U, 999U, TIM_BM_COUNT_UP, 0U, true, false, false };
    return TIM_HAL_InitTimeBase(hh(TIM2), &c);
}

static void test_b1_hal(void)
{
    (void)TIM_HAL_Deinit(hh(TIM2));
    const uint32_t tb0 = cycles_of(b1h_tb);    /* State RESET: the project's MSP hook runs */
    const uint32_t tb = cycles_of(b1h_tb);     /* State READY                               */
    const uint32_t oc = cycles_of(b1h_oc);
    const uint32_t sp = cycles_of(b1h_sp);
    (void)t14(TIM2, "B1");
    logw("B1 HAL CM7 cycles (400 MHz): InitTimeBase %lu (first, State RESET) and %lu (State READY), ConfigOutput %lu, "
        "StartPwm (OutputEnable and Start) %lu; TakeFlags: none in tim_hal (tim_bm's)", (unsigned long)tb0,
        (unsigned long)tb, (unsigned long)oc, (unsigned long)sp);
}

/* The internal F3 tests, after F2's and before the AD2 steps. No wired pin moves. */
static void f3_internal(void)
{
    tim6_snap(&s_tim6_f3);
    hal_tick_check("before the HAL tests");
    s_tr = &k_tier_hal;
    test_t1_hal();
    test_t2_hal();
    test_t5();
    test_b1_hal();
    s_tr = &k_tst_tier_bm;
    hal_tick_check("during the HAL tests (after T1, T2, T5 and B1 through tim_hal)");
}

/* After the HAL bench steps: the tick still runs, TIM6 as it was. */
static void f3_after(void)
{
    hal_tick_check("after the HAL tests");
    Tim6Snap_t now;
    tim6_snap(&now);
    verdict(memcmp(&now, &s_tim6_f3, sizeof(now)) == 0,
            "TIM6 (CPU1 HAL tick) across the F3 tests: CR1 0x%04lX CR2 0x%04lX DIER 0x%04lX PSC %lu ARR %lu, before "
            "CR1 0x%04lX CR2 0x%04lX DIER 0x%04lX PSC %lu ARR %lu", (unsigned long)now.cr1, (unsigned long)now.cr2,
            (unsigned long)now.dier, (unsigned long)now.psc, (unsigned long)now.arr, (unsigned long)s_tim6_f3.cr1,
            (unsigned long)s_tim6_f3.cr2, (unsigned long)s_tim6_f3.dier, (unsigned long)s_tim6_f3.psc,
            (unsigned long)s_tim6_f3.arr);
}

/* ==========================================================================================
 * The AD2 steps of F1. R3-2: MCO1 = HSE/8 on PA8 (AF0) in the T6 and T8 steps (and in T9),
 * so every frequency the AD2 measures is a ratio with MCO1 in the same acquisition; RCC_CFGR
 * is saved before and restored with read-back after. T11 and T13(a) run inside the T6 step
 * (TIM1 outputs, bus = MCU) and T13(b) inside the T8 step (TIM1 inputs, bus = AD2), so they
 * have MCO1 too and no extra step writes RCC_CFGR (instructions 3.5).
 * ========================================================================================== */

static uint32_t s_cfgr_step;

static bool mco1_on(const char *step)
{
    s_cfgr_step   = RCC->CFGR;
    s_mco_saved   = s_cfgr_step & (RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE);
    s_mco_changed = true;
    MODIFY_REG(RCC->CFGR, RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE, RCC_CFGR_MCO1_1 | RCC_CFGR_MCO1PRE_3);
    const uint32_t on = RCC->CFGR & (RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE);
    if (!pin_cfg(GPIOA, PA_MCO, GPIO_BM_MODE_AF, TST_AF_MCO1, GPIO_BM_PULL_NONE, 0U))
    {
        return false;
    }
    delay_ms(2U);     /* the first ms after an MCO1 change may glitch (RM0399 9.5.4) */
    verdict(on == (RCC_CFGR_MCO1_1 | RCC_CFGR_MCO1PRE_3), "%s MCO1 = HSE/8 on PA8 AF0 (R3-2): MCO1 fields 0x%08lX", step,
            (unsigned long)on);
    return true;
}

static void mco1_off(const char *step)
{
    (void)pin_cfg(GPIOA, PA_MCO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U);
    mco_restore();
    const uint32_t after = RCC->CFGR;
    verdict(after == s_cfgr_step, "%s RCC_CFGR restored: before 0x%08lX after 0x%08lX", step,
            (unsigned long)s_cfgr_step, (unsigned long)after);
}

/* One handshake with an action between HOST_REQ rising and MCU_ACK falling: the host has
   armed or started its instrument when it raises HOST_REQ. */
static bool sync_act(void (*fn)(void *), void *arg)
{
    if (s_bench_abort)
    {
        return false;
    }
    ack(true);
    bool ok = wait_req(true, SYNC_TIMEOUT_CYC);
    if (ok)
    {
        fn(arg);
    }
    ack(false);
    if (ok)
    {
        ok = wait_req(false, SYNC_TIMEOUT_CYC);
    }
    if (!ok)
    {
        bench_abort("host timeout");
    }
    return ok;
}


#define T6_TIM1_PINS   ((1UL << 9) | (1UL << 11) | (1UL << 13) | (1UL << 14))

/* T11: 10 one-pulses by software, 3 us apart. CH1 = the pulse (PWM2, CCR1 = D), CH2 = a
   reference that rises one tick after the start (PWM2, CCR2 = 1); both end at the update. */
#define T11_CCR1    40U
#define T11_ARR     139U
#define T11_PULSES  10U
static uint32_t s_t11_fired;

static void t11_fire(void *arg)
{
    (void)arg;
    s_t11_fired = 0U;
    const uint32_t t0 = DWT->CYCCNT;
    for (uint32_t k = 0U; k < T11_PULSES; k++)
    {
        while ((DWT->CYCCNT - t0) < (k * 1200U))
        {
        }
        (void)TIM_BM_Start(TIM1);
        const uint32_t s = DWT->CYCCNT;
        while (((TIM1->CR1 & TIM_CR1_CEN) != 0U) && ((DWT->CYCCNT - s) < 4000U))
        {
        }
        s_t11_fired++;
    }
}

/* T13(a): DMA1 stream 0, TIM1 UP (request 15) -> CCR1, 16 values, circular. */
#define T13_STREAM_A   DMA1_Stream0
#define T13_STREAM_B   DMA1_Stream1
static uint32_t s_t13a_seq[16] __attribute__((section(".dma_buf"), aligned(32)));
static uint32_t s_t13b_buf[64] __attribute__((section(".dma_buf"), aligned(32)));

static bool t13_dma(DMA_Stream_TypeDef *s, uint32_t req, uint32_t dir, uint32_t periph, uint32_t mem, uint32_t n, bool circ)
{
    const DmaBm_Config_t c = {
        .stream = s, .request_id = req, .direction = dir,
        .periph_data_size = DMA_BM_DATA_SIZE_WORD, .mem_data_size = DMA_BM_DATA_SIZE_WORD,
        .periph_inc = false, .mem_inc = true, .circular = circ, .priority = DMA_BM_PRIORITY_HIGH,
        .enable_it_ht = false, .enable_it_tc = false, .enable_it_te = false, .enable_it_dme = false,
        .enable_it_fe = false, .enable_trbuff = false, .enable_fifo = false, .fifo_threshold = DMA_BM_FIFO_TH_1_2,
        .periph_addr = periph, .mem_addr = mem, .buffer_length = n,
    };
    if (!DMA_BM_InitStream(&c))
    {
        return false;
    }
    DMA_BM_ClearFlags(s, DMA_BM_FLAG_ALL);
    DMA_BM_Start(s);
    return true;
}

/* T11 and T13(a), inside step T6_TIM1 (tim_bm only: plan, the T11 and T13 rows). False when
   a handshake failed (the bench aborted). */
static bool t6_tim1_t11_t13a(void)
{
    /* T11 */
    (void)TIM_BM_Deinit(TIM1);
    {
        const TimBm_TimeBaseConfig_t c = { TIM1, 0U, T11_ARR, TIM_BM_COUNT_UP, 0U, false, true, false };
        const TimBm_OcConfig_t o1 = { TIM1, 1U, TIM_BM_OC_PWM2, T11_CCR1, false, false };
        const TimBm_OcConfig_t o2 = { TIM1, 2U, TIM_BM_OC_PWM2, 1U, false, false };
        const bool ok = (TIM_BM_InitTimeBase(&c) == TIM_BM_OK) && (TIM_BM_ConfigOutput(&o1) == TIM_BM_OK) &&
                        (TIM_BM_ConfigOutput(&o2) == TIM_BM_OK) && (TIM_BM_OutputEnable(TIM1, 0x3U) == TIM_BM_OK);
        logw("T11 TIM1 one-pulse: CH1 PWM2 CCR1=%u, CH2 PWM2 CCR2=1 (reference), ARR=%u, %u pulses by software, config %u",
            (unsigned)T11_CCR1, (unsigned)T11_ARR, (unsigned)T11_PULSES, (unsigned)ok);
        if (!sync_act(t11_fire, NULL))         /* host: LA armed on DIO1 rising */
        {
            return false;
        }
        verdict(ok && (s_t11_fired == T11_PULSES), "T11 %lu software triggers fired, CEN cleared by the hardware each time",
                (unsigned long)s_t11_fired);
        /* control: OPM = 0, periodic */
        const TimBm_TimeBaseConfig_t c2 = { TIM1, 0U, T11_ARR, TIM_BM_COUNT_UP, 0U, false, false, false };
        (void)TIM_BM_InitTimeBase(&c2);
        (void)TIM_BM_Start(TIM1);
        LOG("T11 control: OPM = 0, the same channels run periodically");
        if (!sync0())
        {
            return false;
        }
        (void)TIM_BM_Stop(TIM1);
    }

    /* T13(a) */
    (void)TIM_BM_Deinit(TIM1);
    {
        for (uint32_t k = 0U; k < 16U; k++)
        {
            s_t13a_seq[k] = 10U * (k + 1U);    /* high times 10..160 ticks */
        }
        const TimBm_TimeBaseConfig_t c = { TIM1, 0U, 199U, TIM_BM_COUNT_UP, 0U, true, false, false };
        const TimBm_OcConfig_t oc = { TIM1, 1U, TIM_BM_OC_PWM1, 100U, false, true };
        uint32_t req = 0U;
        const bool ok = (TIM_BM_InitTimeBase(&c) == TIM_BM_OK) && (TIM_BM_ConfigOutput(&oc) == TIM_BM_OK) &&
                        (TIM_BM_GetDmaRequest(TIM1, TIM_BM_EV_UPDATE, &req) == TIM_BM_OK) &&
                        t13_dma(T13_STREAM_A, req, DMA_BM_DIR_MEM_TO_PERIPH, (uint32_t)&TIM1->CCR1,
                                (uint32_t)s_t13a_seq, 16U, true) &&
                        (TIM_BM_EnableDmaRequest(TIM1, TIM_BM_EV_UPDATE) == TIM_BM_OK) &&
                        (TIM_BM_OutputEnable(TIM1, TIM_BM_CH(1)) == TIM_BM_OK) && (TIM_BM_Start(TIM1) == TIM_BM_OK);
        logw("T13A TIM1 UP (request %lu) -> DMA1 stream 0 -> CCR1, 16 values 10..160 ticks, circular, PWM1 ARR=199: config %u",
            (unsigned long)req, (unsigned)ok);
        if (!ok)
        {
            s_fail++;
        }
        if (!sync0())
        {
            return false;
        }
        /* control: UDE = 0, CCR1 stays */
        (void)TIM_BM_DisableDmaRequest(TIM1, TIM_BM_EV_UPDATE);
        (void)DMA_BM_Stop(T13_STREAM_A);
        TIM_BM_WriteCcr(TIM1, 1U, 100U);
        delay_ms(1U);
        LOG("T13A control: UDE = 0, CCR1 = 100");
        if (!sync0())
        {
            return false;
        }
        (void)TIM_BM_Stop(TIM1);
        (void)DMA_BM_DeinitStream(T13_STREAM_A);
    }

    return true;
}

static void step_t6_tim1(void)
{
    const char *const name = tier_step("T6_TIM1");
    if (!step_begin(name) || !buschk() || !mco1_on(name))
    {
        return;
    }
    (void)s_tr->deinit(TIM1);
    for (uint32_t p = 9U; p <= 14U; p++)
    {
        if (((T6_TIM1_PINS >> p) & 1U) != 0U)
        {
            if (!pin_cfg(GPIOE, p, GPIO_BM_MODE_AF, TST_AF_TIM1, GPIO_BM_PULL_DOWN, 0U))
            {
                return;
            }
        }
    }
    /* T6: every point on every channel; the other three channels stay disabled. */
    for (uint32_t ch = 1U; ch <= 4U; ch++)
    {
        for (uint32_t k = 0U; k < TST_T6_POINTS; k++)
        {
            const TstT6Point_t *p = &k_tst_t6[k];
            const TimBm_Status_t st = tst_t6_apply(s_tr, TIM1, ch, p);
            LOG("T6%s TIM1 CH%lu P%lu psc=%u arr=%u mode=%u ccr=%u low=%u center=%u st=%u", s_tr->tag, (unsigned long)ch,
                (unsigned long)k, p->psc, p->arr, p->mode, p->ccr, p->low, p->center, (unsigned)st);
            if (st != TIM_BM_OK)
            {
                s_fail++;
            }
            if (!sync0())                       /* host: LA on DIO1/3/5/6 and DIO14 */
            {
                return;
            }
            (void)s_tr->stop(TIM1);
        }
    }
    /* control: all four channels configured and enabled, MOE cleared (OSSI = 0) */
    (void)s_tr->deinit(TIM1);
    {
        const TimBm_TimeBaseConfig_t c = { TIM1, 0U, 199U, TIM_BM_COUNT_UP, 0U, true, false, false };
        (void)s_tr->init_tb(&c);
        for (uint32_t ch = 1U; ch <= 4U; ch++)
        {
            const TimBm_OcConfig_t oc = { TIM1, ch, TIM_BM_OC_PWM1, 100U, false, true };
            (void)s_tr->config_output(&oc);
        }
        (void)s_tr->output_enable(TIM1, 0xFU);
        (void)s_tr->start(TIM1);
        TIM1->BDTR &= ~TIM_BDTR_MOE;           /* the control, by hand */
        logw("T6%s TIM1 control: CH1-4 PWM1 50 %% enabled, MOE = 0 (BDTR 0x%08lX), OSSI = 0", s_tr->tag,
            (unsigned long)TIM1->BDTR);
        if (!sync0())
        {
            return;
        }
        (void)s_tr->stop(TIM1);
    }

    if ((s_tr == &k_tst_tier_bm) && !t6_tim1_t11_t13a())
    {
        return;
    }

    for (uint32_t p = 9U; p <= 14U; p++)
    {
        if (((T6_TIM1_PINS >> p) & 1U) != 0U)
        {
            (void)pin_cfg(GPIOE, p, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U);
        }
    }
    (void)t14(TIM1, "T6/T11/T13(a)");
    mco1_off(name);
    const bool released = buschk();            /* T14: S5 on the wired pins */
    verdict(released, "T14 %s: after Deinit the bus follows both pulls again", name);
    step_end();
}

static void step_t6_tim15(void)
{
    TstArea_t *a = TST_AREA;
    const char *const name = tier_step("T6_TIM15");
    const uint32_t ll = (s_tr == &k_tst_tier_bm) ? 0U : TST_T6_LL;
    if (!step_begin(name) || !mco1_on(name))
    {
        return;
    }
    for (uint32_t ch = 1U; ch <= 2U; ch++)
    {
        for (uint32_t k = 0U; k < (TST_T6_POINTS - 1U); k++)
        {
            const TstT6Point_t *p = &k_tst_t6[k];
            const bool sent = cm4_send(TST_CMD_T6, (ch << 8) | k | ll);
            LOG("T6%s TIM15 CH%lu P%lu psc=%u arr=%u mode=%u ccr=%u low=%u center=%u st=%lu", s_tr->tag, (unsigned long)ch,
                (unsigned long)k, p->psc, p->arr, p->mode, p->ccr, p->low, p->center,
                sent ? (unsigned long)a->m4_res[0] : 99UL);
            if (!sent || (a->m4_res[0] != 0U))
            {
                s_fail++;
            }
            if (!sync0())
            {
                return;
            }
        }
    }
    (void)cm4_send(TST_CMD_T6, TST_T6_CONTROL | ll);
    logw("T6%s TIM15 control: CH1/CH2 configured, CC1E = CC2E = 0, pins follow the pull-down (status %lu)",
        s_tr->tag, (unsigned long)a->m4_res[0]);
    if (!sync0())
    {
        return;
    }
    (void)cm4_send(TST_CMD_T6, TST_T6_OFF | ll);
    verdict(a->m4_res[1] == 1U, "T14%s CM4 TIM15 after T6: %lu", s_tr->tag, (unsigned long)a->m4_res[1]);
    mco1_off(name);
    step_end();
}

/* ---- T8 (+ T13(b)): captures of the AD2's Digital Out on PE9 (TI1) and PE11 (TI2) -------- */

typedef struct { uint8_t ti; uint32_t f; uint8_t duty; uint8_t psc; } T8Point_t;
static const T8Point_t k_t8[] = {
    { 1U, 1000U, 25U, 3U }, { 1U, 1000U, 50U, 3U }, { 1U, 1000U, 75U, 3U },
    { 1U, 10000U, 25U, 0U }, { 1U, 10000U, 50U, 0U }, { 1U, 10000U, 75U, 0U },
    { 1U, 100000U, 25U, 0U }, { 1U, 100000U, 50U, 0U }, { 1U, 100000U, 75U, 0U },
    { 1U, 1000000U, 25U, 0U }, { 1U, 1000000U, 50U, 0U }, { 1U, 1000000U, 75U, 0U },
    { 2U, 1000U, 25U, 3U }, { 2U, 1000U, 50U, 3U }, { 2U, 1000U, 75U, 3U },
    { 2U, 10000U, 25U, 0U }, { 2U, 10000U, 50U, 0U }, { 2U, 10000U, 75U, 0U },
    { 2U, 100000U, 25U, 0U }, { 2U, 100000U, 50U, 0U }, { 2U, 100000U, 75U, 0U },
    { 2U, 1000000U, 25U, 0U }, { 2U, 1000000U, 50U, 0U }, { 2U, 1000000U, 75U, 0U },
};
#define T8_POINTS   (sizeof(k_t8) / sizeof(k_t8[0]))

/* Reconfigure TIM1 without ever leaving CCxS = 00 on CH1/CH2 (S12 holds while the pins
   are in AF and the AD2 drives): captures off, counter stopped, slave mode off. */
static bool t8_quiesce(void)
{
    /* CaptureDisable refuses outputs (CCxS = 00): the first call after Deinit has no
       capture to disable, and CC1E = CC2E = 0 is what matters. */
    const TimBm_Status_t cd = s_tr->capture_disable(TIM1, 0x3U);
    const bool off = (cd == TIM_BM_OK) || ((TIM1->CCER & (TIM_CCER_CC1E | TIM_CCER_CC2E)) == 0U);
    return off && (s_tr->stop(TIM1) == TIM_BM_OK) &&
           (s_tr->config_slave(TIM1, TIM_BM_SMS_DISABLED, TIM_BM_TRG_TI1FP1) == TIM_BM_OK);
}

static bool t8_pwm_input(uint32_t ti, uint32_t edge, uint32_t psc)
{
    const TimBm_TimeBaseConfig_t c = { TIM1, psc, 0xFFFFU, TIM_BM_COUNT_UP, 0U, false, false, false };
    return t8_quiesce() && (s_tr->init_tb(&c) == TIM_BM_OK) &&
           (s_tr->config_pwm_input(TIM1, ti, edge, 1U, 0U, 0U) == TIM_BM_OK);
}

/* PWM input: 8 captures after two discarded ones (the first holds CNT from before the
   first reset; the first reset after Start came late by up to 9 ticks in the development
   runs, so the second goes too). Period on CCR(ti), high on the other. Every capture is
   printed: the host judges each one. */
#define T8_SKIP   2U
#define T8_CAPS   8U        /* per point                          */
#define T8_CAPS_D 16U       /* for the reset delay (16 + 16)      */
static uint32_t s_t8_n;
static uint32_t s_t8_per[T8_CAPS_D];
static uint32_t s_t8_hi[T8_CAPS_D];

static void t8_measure(uint32_t ti, uint32_t max_wait_cyc, uint32_t ncaps)
{
    const uint32_t ev = (ti == 1U) ? TIM_BM_EV_CC1 : TIM_BM_EV_CC2;
    const uint32_t other = (ti == 1U) ? 2U : 1U;
    s_t8_n = 0U;
    (void)s_tr->start(TIM1);                  /* counter first: a capture with CEN = 0 latches */
    (void)s_tr->capture_enable(TIM1, 0x3U);    /* the stopped CNT                               */
    for (uint32_t k = 0U; k < (ncaps + T8_SKIP); k++)
    {
        uint32_t taken = 0U;
        if (s_tr->wait_flags(TIM1, ev, max_wait_cyc, &taken) != TIM_BM_OK)
        {
            break;
        }
        const uint32_t per = s_tr->read_capture(TIM1, ti);
        const uint32_t hi = s_tr->read_capture(TIM1, other);
        if (k < T8_SKIP)
        {
            continue;
        }
        s_t8_per[s_t8_n] = per;
        s_t8_hi[s_t8_n] = hi;
        s_t8_n++;
    }
    (void)s_tr->capture_disable(TIM1, 0x3U);
    (void)s_tr->stop(TIM1);
}

/* "<tag> n=<n> per <8 values> hi <8 values>" (fits App_Log's 160 characters). */
static void t8_log_pwm(const char *tag)
{
    char per[64];
    char hi[64];
    int lp = 0;
    int lh = 0;
    for (uint32_t k = 0U; k < s_t8_n; k++)
    {
        lp += snprintf(&per[lp], sizeof(per) - (size_t)lp, " %lu", (unsigned long)s_t8_per[k]);
        lh += snprintf(&hi[lh], sizeof(hi) - (size_t)lh, " %lu", (unsigned long)s_t8_hi[k]);
    }
    per[(lp > 0) ? lp : 0] = '\0';
    hi[(lh > 0) ? lh : 0] = '\0';
    LOG("%s n=%lu per%s hi%s", tag, (unsigned long)s_t8_n, per, hi);
}

/* "<tag> n=<n> per <up to 16 values>" */
static void t8_log_per(const char *tag)
{
    char per[100];
    int lp = 0;
    for (uint32_t k = 0U; (k < s_t8_n) && (lp < 90); k++)
    {
        lp += snprintf(&per[lp], sizeof(per) - (size_t)lp, " %lu", (unsigned long)s_t8_per[k]);
    }
    per[(lp > 0) ? lp : 0] = '\0';
    LOG("%s n=%lu per%s", tag, (unsigned long)s_t8_n, per);
}

/* Direct capture on CH1 (CH2 stays an input): `n` successive CCR1 differences. */
static uint32_t s_t8_diff[16];

static uint32_t t8_diffs(uint32_t n, uint32_t max_wait_cyc)
{
    uint32_t got = 0U;
    uint32_t prev = 0U;
    (void)s_tr->start(TIM1);                  /* counter first (see t8_measure)  */
    (void)s_tr->capture_enable(TIM1, TIM_BM_CH(1));
    for (uint32_t k = 0U; k <= n; k++)
    {
        uint32_t taken = 0U;
        if (s_tr->wait_flags(TIM1, TIM_BM_EV_CC1, max_wait_cyc, &taken) != TIM_BM_OK)
        {
            break;
        }
        const uint32_t v = s_tr->read_capture(TIM1, 1U);
        if (k != 0U)
        {
            s_t8_diff[got++] = (v - prev) & 0xFFFFU;
        }
        prev = v;
    }
    (void)s_tr->capture_disable(TIM1, TIM_BM_CH(1));
    (void)s_tr->stop(TIM1);
    return got;
}

static bool t8_direct(uint32_t prescaler, uint32_t filter)
{
    const TimBm_TimeBaseConfig_t c = { TIM1, 0U, 0xFFFFU, TIM_BM_COUNT_UP, 0U, false, false, false };
    const TimBm_IcConfig_t ic = { TIM1, 1U, TIM_BM_IC_DIRECT, TIM_BM_EDGE_RISING, prescaler, filter, 0U };
    return t8_quiesce() && (s_tr->init_tb(&c) == TIM_BM_OK) && (s_tr->config_input(&ic) == TIM_BM_OK);
}

static void t8_log_diffs(const char *what, uint32_t n)
{
    char buf[100];
    int len = 0;
    for (uint32_t k = 0U; (k < n) && (len < 88); k++)
    {
        len += snprintf(&buf[len], sizeof(buf) - (size_t)len, " %lu", (unsigned long)s_t8_diff[k]);
    }
    LOG("%s n=%lu diffs%s", what, (unsigned long)n, buf);
}

static void step_t8(void)
{
    const char *const name = tier_step("T8");
    const char *const tg = s_tr->tag;
    char tag[24];
    if (!step_begin(name) || !buschk() || !mco1_on(name))
    {
        return;
    }
    /* S12: TIM1 CH1/CH2 inputs before the pins go to AF1 */
    if (!t8_pwm_input(1U, TIM_BM_EDGE_RISING, 0U))
    {
        bench_abort("T8 TIM1 input config");
        return;
    }
    if (!pin_cfg(GPIOE, PE_CAP1, GPIO_BM_MODE_AF, TST_AF_TIM1, GPIO_BM_PULL_NONE, 0U) ||
        !pin_cfg(GPIOE, PE_CAP2, GPIO_BM_MODE_AF, TST_AF_TIM1, GPIO_BM_PULL_NONE, 0U))
    {
        return;
    }
    uint32_t up = 0U;
    uint32_t dn = 0U;
    if (!buschk_af((1UL << PE_CAP1) | (1UL << PE_CAP2), &up, &dn))   /* S5 with the pins in AF */
    {
        return;
    }
    for (uint32_t p = PE_CAP1; p <= PE_CAP2; p += 2U)
    {
        (void)pin_cfg(GPIOE, p, GPIO_BM_MODE_AF, TST_AF_TIM1, GPIO_BM_PULL_DOWN, 0U);
    }

    /* The counter-reset delay of the slave reset mode (RM0399 40.3.26: "due to the
       resynchronization circuit on TI1 input", no value given), measured in this session
       on one 1 MHz Digital Out generation: PWM input first, then direct captures of the
       same signal. The host subtracts it from the expected period and high time (owner's
       decision of 2026-10-02, after the first F1 development runs). */
    {
        const bool ok = t8_pwm_input(1U, TIM_BM_EDGE_RISING, 0U);
        LOG("T8%s DELAY ti=1 f=1000000 duty=50 config %u", tg, (unsigned)ok);
        if (!sync0())                          /* host: Digital Out 1 MHz on DIO1, LA */
        {
            return;
        }
        t8_measure(1U, 4000U * 4U, T8_CAPS_D);
        (void)snprintf(tag, sizeof(tag), "T8%s RDELAY_PWM", tg);
        t8_log_per(tag);
        const bool okd = t8_direct(1U, 0U);
        const uint32_t n = t8_diffs(16U, 40000U);
        (void)snprintf(tag, sizeof(tag), okd ? "T8%s RDELAY_DIR" : "T8%s RDELAY_DIR failed", tg);
        t8_log_diffs(tag, n);
        if (!sync0())                          /* host: Digital Out off */
        {
            return;
        }
    }
    for (uint32_t k = 0U; k < T8_POINTS; k++)
    {
        const T8Point_t *p = &k_t8[k];
        const bool ok = t8_pwm_input(p->ti, TIM_BM_EDGE_RISING, p->psc);
        LOG("T8%s P%lu ti=%u f=%lu duty=%u psc=%u config %u", tg, (unsigned long)k, p->ti, (unsigned long)p->f, p->duty,
            p->psc, (unsigned)ok);
        if (!sync0())                          /* host: Digital Out on, LA (DIO14 + the line) */
        {
            return;
        }
        (void)snprintf(tag, sizeof(tag), "T8%s R%lu", tg, (unsigned long)k);
        t8_measure(p->ti, 4U * (400000000UL / p->f), T8_CAPS);
        t8_log_pwm(tag);
        if (!sync0())                          /* host: Digital Out off */
        {
            return;
        }
    }
    /* control: inverted polarity on a 100 kHz 25 % signal -> the "high" is the low time */
    {
        const bool ok = t8_pwm_input(1U, TIM_BM_EDGE_FALLING, 0U);
        LOG("T8%s INV ti=1 f=100000 duty=25 edge=falling config %u", tg, (unsigned)ok);
        if (!sync0())
        {
            return;
        }
        t8_measure(1U, 4000U * 4U, T8_CAPS);
        (void)snprintf(tag, sizeof(tag), "T8%s RINV", tg);
        t8_log_pwm(tag);
        /* control: CC1E = 0 -> CC1IF never rises */
        (void)t8_pwm_input(1U, TIM_BM_EDGE_RISING, 0U);
        (void)s_tr->take_flags(TIM1, TIM_BM_EV_CC1);
        (void)s_tr->start(TIM1);
        delay_ms(1U);
        const uint32_t cc1if = TIM1->SR & TIM_SR_CC1IF;
        (void)s_tr->stop(TIM1);
        verdict(cc1if == 0U, "T8%s control CC1E = 0 with the 100 kHz signal on PE9 for 1 ms: CC1IF %lu (0)", tg,
                (unsigned long)(cc1if != 0U));
        if (!sync0())
        {
            return;
        }
    }
    /* input prescaler 8 at 1 MHz: one capture every 8 periods */
    {
        const bool ok = t8_direct(8U, 0U);
        LOG("T8%s PSC8 ti=1 f=1000000 duty=50 icpsc=8 config %u", tg, (unsigned)ok);
        if (!sync0())
        {
            return;
        }
        const uint32_t n = t8_diffs(8U, 40000U);
        (void)snprintf(tag, sizeof(tag), "T8%s RPSC8", tg);
        t8_log_diffs(tag, n);
        if (!sync0())
        {
            return;
        }
    }
    /* filter: a 10 ns glitch per period (Digital Out custom). Without filter it is captured;
       with ICxF = 0011 (fCK_INT, N = 8: 40 ns) it is not. */
    for (uint32_t f = 0U; f < 2U; f++)
    {
        const uint32_t icf = (f == 0U) ? 0U : 3U;
        const bool ok = t8_direct(1U, icf);
        LOG("T8%s GLITCH ti=1 icf=%lu config %u", tg, (unsigned long)icf, (unsigned)ok);
        if (!sync0())
        {
            return;
        }
        const uint32_t n = t8_diffs(16U, 40000U);
        (void)snprintf(tag, sizeof(tag), "T8%s RGLITCH%lu", tg, (unsigned long)icf);
        t8_log_diffs(tag, n);
        if (!sync0())
        {
            return;
        }
    }
    /* overcapture: CCR1 not read for 3 periods at 100 kHz -> CC1OF; read every time -> none */
    {
        const bool ok = t8_direct(1U, 0U);
        LOG("T8%s OVR ti=1 f=100000 duty=50 config %u", tg, (unsigned)ok);
        if (!sync0())
        {
            return;
        }
        (void)s_tr->capture_enable(TIM1, TIM_BM_CH(1));
        (void)s_tr->start(TIM1);
        delay_cycles(4000U * 4U * 3U);         /* 3 periods of 10 us at 400 MHz */
        const uint32_t of = s_tr->take_ovr(TIM1, TIM_BM_CH(1));
        (void)s_tr->read_capture(TIM1, 1U);
        (void)s_tr->take_flags(TIM1, TIM_BM_EV_CC1);
        (void)s_tr->take_ovr(TIM1, TIM_BM_CH(1));
        uint32_t reads = 0U;
        for (uint32_t k = 0U; k < 16U; k++)
        {
            uint32_t taken = 0U;
            if (s_tr->wait_flags(TIM1, TIM_BM_EV_CC1, 16000U, &taken) == TIM_BM_OK)
            {
                (void)s_tr->read_capture(TIM1, 1U);
                reads++;
            }
        }
        const uint32_t of2 = s_tr->take_ovr(TIM1, TIM_BM_CH(1));
        (void)s_tr->capture_disable(TIM1, TIM_BM_CH(1));
        (void)s_tr->stop(TIM1);
        verdict((of == TIM_BM_CH(1)) && (reads == 16U) && (of2 == 0U),
                "T8%s overcapture: CCR1 unread for 3 periods -> CC1OF %lu (1); read at each of %lu captures -> CC1OF %lu "
                "(0)", tg, (unsigned long)of, (unsigned long)reads, (unsigned long)of2);
        if (!sync0())
        {
            return;
        }
    }
    /* T13(b): TIM1 CC1 (request 11) -> DMA1 stream 1 -> 64 words of CCR1, 1 MHz on PE9 */
    for (uint32_t ctl = 0U; (s_tr == &k_tst_tier_bm) && (ctl < 2U); ctl++)   /* T13(b): tim_bm only */
    {
        for (uint32_t k = 0U; k < 64U; k++)
        {
            s_t13b_buf[k] = 0xDEAD0000UL | k;   /* sentinel */
        }
        uint32_t req = 0U;
        bool ok = t8_direct(1U, 0U) && (TIM_BM_GetDmaRequest(TIM1, TIM_BM_EV_CC1, &req) == TIM_BM_OK) &&
                  t13_dma(T13_STREAM_B, req, DMA_BM_DIR_PERIPH_TO_MEM, (uint32_t)&TIM1->CCR1, (uint32_t)s_t13b_buf, 64U,
                          false);
        if (ctl == 0U)
        {
            ok = ok && (TIM_BM_EnableDmaRequest(TIM1, TIM_BM_EV_CC1) == TIM_BM_OK);
        }
        logw("T13B%s TIM1 CC1 (request %lu) -> DMA1 stream 1, 64 words, CC1DE %lu, config %u", (ctl == 0U) ? "" : " control",
            (unsigned long)req, (unsigned long)(ctl == 0U), (unsigned)ok);
        if (!sync0())                          /* host: Digital Out 1 MHz on DIO1, LA */
        {
            return;
        }
        /* Counter first, capture second: a capture taken while CEN = 0 latches the stopped CNT,
           and the DMA would store it as the first word (one development run: a 152-tick
           first difference). */
        (void)TIM_BM_Start(TIM1);
        (void)TIM_BM_CaptureEnable(TIM1, TIM_BM_CH(1));
        const uint32_t t0 = DWT->CYCCNT;
        while ((DMA_BM_GetRemaining(T13_STREAM_B) != 0U) && ((DWT->CYCCNT - t0) < CYC_PER_MS))
        {
        }
        const uint32_t remaining = DMA_BM_GetRemaining(T13_STREAM_B);
        (void)TIM_BM_CaptureDisable(TIM1, TIM_BM_CH(1));
        (void)TIM_BM_Stop(TIM1);
        (void)TIM_BM_DisableDmaRequest(TIM1, TIM_BM_EV_CC1);
        (void)DMA_BM_DeinitStream(T13_STREAM_B);
        if (ctl == 0U)
        {
            uint32_t dmin = 0xFFFFFFFFUL;
            uint32_t dmax = 0U;
            for (uint32_t k = 1U; k < 64U; k++)
            {
                const uint32_t d = (s_t13b_buf[k] - s_t13b_buf[k - 1U]) & 0xFFFFU;
                dmin = (d < dmin) ? d : dmin;
                dmax = (d > dmax) ? d : dmax;
            }
            LOG("T13B R remaining=%lu diffs=63 min=%lu max=%lu first=0x%08lX", (unsigned long)remaining,
                (unsigned long)dmin, (unsigned long)dmax, (unsigned long)s_t13b_buf[0]);
        }
        else
        {
            uint32_t intact = 0U;
            for (uint32_t k = 0U; k < 64U; k++)
            {
                intact += (s_t13b_buf[k] == (0xDEAD0000UL | k)) ? 1U : 0U;
            }
            verdict((remaining == 64U) && (intact == 64U), "T13(b) control CC1DE = 0: NDTR %lu (64), sentinel intact in %lu "
                    "of 64 words", (unsigned long)remaining, (unsigned long)intact);
        }
        if (!sync0())                          /* host: Digital Out off */
        {
            return;
        }
    }
    /* back to S2 order: the host stopped Digital Out at the last handshake; pins to input,
       then TIM1 may leave its input configuration */
    (void)pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
    (void)t14(TIM1, "T8/T13(b)");
    mco1_off(name);
    const bool released = buschk();
    verdict(released, "T14 %s: after Deinit the bus follows both pulls again", name);
    step_end();
}

/* ---- T9: TIM17 TI1SEL = 0011 (MCO1) ---------------------------------------------------- */

static uint32_t s_t9_n;
static uint32_t s_t9_min;
static uint32_t s_t9_max;

static void t9_capture(void *arg)
{
    (void)arg;
    s_t9_n = 0U;
    s_t9_min = 0xFFFFFFFFUL;
    s_t9_max = 0U;
    uint32_t prev = 0U;
    /* A capture comes every 1 us (400 cycles). In the F2 development runs a WaitFlags call
       per capture, then also with interrupts masked, let CCR1 be overwritten once (a 400-
       and a 401-tick difference): the loop polls TakeFlags instead, bounded by CYCCNT, with
       PRIMASK for the 17 captures (at most about 0.4 ms: the HAL tick is delayed, not lost). */
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    (void)TIM_BM_TakeFlags(TIM17, TIM_BM_EV_CC1);
    for (uint32_t k = 0U; k <= 16U; k++)
    {
        const uint32_t t0 = DWT->CYCCNT;
        uint32_t got = 0U;
        while ((got == 0U) && ((DWT->CYCCNT - t0) < 8000U))
        {
            got = TIM_BM_TakeFlags(TIM17, TIM_BM_EV_CC1);
        }
        if (got == 0U)
        {
            break;
        }
        const uint32_t v = TIM_BM_ReadCapture(TIM17, 1U);
        if (k != 0U)
        {
            const uint32_t d = (v - prev) & 0xFFFFU;
            s_t9_min = (d < s_t9_min) ? d : s_t9_min;
            s_t9_max = (d > s_t9_max) ? d : s_t9_max;
            s_t9_n++;
        }
        prev = v;
    }
    __set_PRIMASK(pm);
}

/* T9 through the tier (F1 tim_bm, F3 tim_hal): the configuration, the start and the stop go
   through s_tr; the 16 captures are read with tim_bm (t9_capture), the oracle. */
static void step_t9(void)
{
    const char *const name = tier_step("T9");
    const char *const tg = s_tr->tag;
    if (!step_begin(name) || !mco1_on(name))
    {
        return;
    }
    const TimBm_TimeBaseConfig_t c = { TIM17, 0U, 0xFFFFU, TIM_BM_COUNT_UP, 0U, false, false, false };
    const TimBm_IcConfig_t ic = { TIM17, 1U, TIM_BM_IC_DIRECT, TIM_BM_EDGE_RISING, 1U, 0U, 3U };   /* MCO1 */
    const bool ok = (s_tr->deinit(TIM17) == TIM_BM_OK) && (s_tr->init_tb(&c) == TIM_BM_OK) &&
                    (s_tr->config_input(&ic) == TIM_BM_OK) && (s_tr->capture_enable(TIM17, TIM_BM_CH(1)) == TIM_BM_OK) &&
                    (s_tr->start(TIM17) == TIM_BM_OK);
    LOG("T9%s TIM17 TI1SEL = 0011 (MCO1), input prescaler 1, config %u", tg, (unsigned)ok);
    if (!sync_act(t9_capture, NULL))           /* host: LA on DIO14 at the same time */
    {
        return;
    }
    verdict(ok && (s_t9_n == 16U) && (s_t9_min >= 199U) && (s_t9_max <= 201U),
            "T9%s TIM17 captures MCO1 (TI1SEL = 0011): %lu periods, %lu..%lu ticks (200 +-1)", tg,
            (unsigned long)s_t9_n, (unsigned long)s_t9_min, (unsigned long)s_t9_max);
    /* control: TI1SEL = 0000, PF7 (unwired) in AF1 with pull-down -> no capture */
    (void)s_tr->capture_disable(TIM17, TIM_BM_CH(1));
    (void)s_tr->stop(TIM17);
    TimBm_IcConfig_t ic0 = ic;
    ic0.tisel = 0U;
    (void)pin_cfg(GPIOF, 7U, GPIO_BM_MODE_AF, 1U, GPIO_BM_PULL_DOWN, 0U);
    const bool ok0 = (s_tr->config_input(&ic0) == TIM_BM_OK) && (s_tr->capture_enable(TIM17, TIM_BM_CH(1)) == TIM_BM_OK);
    (void)TIM_BM_TakeFlags(TIM17, TIM_BM_EV_CC1);
    (void)s_tr->start(TIM17);
    delay_ms(1U);
    const uint32_t caps = TIM_BM_TakeFlags(TIM17, TIM_BM_EV_CC1);
    (void)s_tr->stop(TIM17);
    verdict(ok0 && (caps == 0U), "T9%s control: TI1SEL = 0000 (PF7 unwired, pull-down) for 1 ms: CC1IF %lu (0)", tg,
            (unsigned long)(caps != 0U));
    (void)pin_cfg(GPIOF, 7U, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U);
    (void)t14(TIM17, "T9");
    mco1_off(name);
    step_end();
}

/* ==========================================================================================
 * TREF cross-check: HAL_GetTick against the host clock (plan R2-2, R2-10)
 * ========================================================================================== */

/* A burst of n lines "TICK <label> <i> <HAL_GetTick>", one every
   TICK_BURST_STEP_MS, each printed right after HAL_GetTick() reaches its
   value (so the value is the start of that ms, not up to 1 ms old). Plan
   R3-1: TICK_BURST_LONG lines at the banner and at the verdict, and
   TICK_BURST_STEP lines at the start of every step, before its STEP line.
   The host fits the means of the bursts (board ms, arrival time) by least
   squares: the slope is the cross-check x, and u is the standard error of
   the slope. The first line waits TICK_BURST_START_MS so the console has
   sent the lines before. The constants are next to step_begin(). */
static void tick_burst(const char *label, uint32_t n)
{
    const uint32_t t0 = HAL_GetTick() + TICK_BURST_START_MS;
    for (uint32_t i = 0U; i < n; i++)
    {
        const uint32_t target = t0 + (i * TICK_BURST_STEP_MS);
        while ((int32_t)(HAL_GetTick() - target) < 0)
        {
        }
        LOG("TICK %s %lu %lu", label, (unsigned long)i, (unsigned long)HAL_GetTick());
    }
}

/* ==========================================================================================
 * Entry points
 * ========================================================================================== */

void TIM_Selftest_CM7_PreRelease(void)
{
    TstArea_t *a = TST_AREA;
    memset((void *)a, 0, sizeof(*a));
    a->session = DWT->CYCCNT ^ 0x7A1A0000UL;
    a->version = TIM_BENCH_VERSION;
    a->step    = TST_STEP_NONE;
    __DMB();
    a->magic = TST_AREA_MAGIC;
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

void TIM_Selftest_CM7_Run(void)
{
    TstArea_t *a = TST_AREA;
    char rev4[sizeof(a->m4_rev)];
    char id4[sizeof(a->m4_build_id)];

    vTaskSuspendAll();   /* no task switch for the whole test; the HAL tick (TIM6) keeps running */
    publish_step(TST_STEP_NONE);
    bench_safe();

    LOG("CM7 Rev %s", FW_GIT_REV);
    LOG("CM7 Build id %s", FW_BUILD_ID);
    const uint32_t t0 = HAL_GetTick();
    while ((a->m4_ready != a->session) && ((HAL_GetTick() - t0) < CM4_READY_MS))
    {
    }
    const bool m4_ok = (a->m4_ready == a->session);
    copy_area_str(rev4, a->m4_rev, sizeof(rev4));
    copy_area_str(id4, a->m4_build_id, sizeof(id4));
    LOG("CM4 Rev %s", m4_ok ? rev4 : "(CPU2 not ready)");
    LOG("CM4 Build id %s", m4_ok ? id4 : "(CPU2 not ready)");
    if (!m4_ok || (a->m4_version != TIM_BENCH_VERSION))
    {
        s_fail++;
    }
    LOG("TIM_BENCH v%u idle", (unsigned)TIM_BENCH_VERSION);
    LOG("silicon REV_ID=0x%04lX, test block at 0x%08lX, CM4 table v%lu, steps %lu",
        (unsigned long)((DBGMCU->IDCODE & DBGMCU_IDCODE_REV_ID_Msk) >> DBGMCU_IDCODE_REV_ID_Pos),
        (unsigned long)TST_AREA_ADDR, (unsigned long)a->m4_version, (unsigned long)k_tst_step_count);
    tick_burst("banner", TICK_BURST_LONG);

    step_t0a();
    step_tref("TREF_A", true);
    step_t0b();
    step_t0b_dout();
    step_t0b_s12();
    if (!s_bench_abort)
    {
        f1_internal();
        f2_internal();
        f3_internal();
        step_t6_tim1();
        step_t6_tim15();
        step_t8();
        step_t9();
        s_tr = &k_tst_tier_ll;                 /* F2: T6 and T8 again through tim_ll */
        step_t6_tim1();
        step_t6_tim15();
        step_t8();
        s_tr = &k_tier_hal;                    /* F3: T6 (TIM1), T8 and T9 through tim_hal */
        step_t6_tim1();
        hal_tick_check("during the HAL tests (after T6_TIM1_HAL)");
        step_t8();
        hal_tick_check("during the HAL tests (after T8_HAL)");
        step_t9();
        s_tr = &k_tst_tier_bm;
        f3_after();
        f1_clocks_restore();
        verdict(s_t14_bad == 0U, "T14 %lu Deinit checks against the reset image of RM0399: %lu failed",
                (unsigned long)s_t14_n, (unsigned long)s_t14_bad);
    }
    step_tref("TREF_B", false);
    if (step_begin("DONE"))
    {
        step_end();
    }
    bench_safe();
    verdict(!s_bench_abort, "bench completed without abort");

    (void)cm4_send(TST_CMD_DONE, 0U);
    verdict((a->m4_timeout == 0U) && (a->m4_aborts == 0U), "CM4 finished: timeout %lu, aborts %lu",
            (unsigned long)a->m4_timeout, (unsigned long)a->m4_aborts);
    tick_burst("verdict", TICK_BURST_LONG);
    LOG("Status_Task stack high-water mark %lu words", (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    LOG("FW VERDICT=%s failures=%lu", (s_fail == 0U) ? "PASS" : "FAIL", (unsigned long)s_fail);

    (void)xTaskResumeAll();
}
