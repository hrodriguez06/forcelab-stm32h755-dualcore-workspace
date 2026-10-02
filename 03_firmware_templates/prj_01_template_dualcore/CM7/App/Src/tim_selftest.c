/**
  ******************************************************************************
  * @file    tim_selftest.c
  * @brief   03_tim selftest, CPU1 side (preset selftest-tim), phase F0: the
  *          Analog Discovery 2 bench. No driver code yet (F1-F3).
  *
  *          Prints Rev and build id of both cores, then runs the bench steps
  *          T0a, TREF and T0b(b)(c)(d)(f)(g) with the host
  *          (tools/tim_hw_check.py, which runs T0b(a)(e) on a mock DLL), and
  *          prints the global verdict, CPU2's results included. Plan and
  *          meaning of every test: 01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md.
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
static char      s_line[128];             /* verdict() text; one task, no reentrancy */

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
    LOG("%s -> %s", s_line, ok ? "PASS" : "FAIL");
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

/* TREF: MCO1 = HSE/8 on PA8 (AF0) for the AD2 to measure. Control: MCO1 left
   at HSE/8 in the RCC, PA8 moved to AF1 (TIM1_CH1) with TIM1 unclocked in both
   RCC banks: MCO1 no longer reaches the pin, so the AD2 must see no edge (the
   T10 control of 00_gpio). MCO1PRE = 0000 is not a control on this silicon: it
   passes HSE undivided (docs/tim_f0_bench_d01061e.txt). RCC_CFGR is saved and
   restored with read-back; the first ms after each change is not shown to the
   host (RM0399 9.5.4: changing MCO1 may glitch). */
static void step_tref(void)
{
    if (!step_begin("TREF"))
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
    LOG("TREF MCO1 = HSE/8 on PA8 AF0, %lu Hz expected (HSE_VALUE/8), RCC_CFGR=0x%08lX",
        (unsigned long)(HSE_VALUE / 8U), (unsigned long)RCC->CFGR);
    if (!sync0())          /* host: frequency on DIO14 */
    {
        return;
    }
    /* An unclocked TIM1 cannot run: its enable bit in both RCC banks, raw. */
    const uint32_t tim1_c1 = RCC_C1->APB2ENR & RCC_APB2ENR_TIM1EN;
    const uint32_t tim1_c2 = RCC_C2->APB2ENR & RCC_APB2ENR_TIM1EN;
    if (!pin_cfg(GPIOA, PA_MCO, GPIO_BM_MODE_AF, TST_AF_TIM1, GPIO_BM_PULL_NONE, 0U))
    {
        return;
    }
    const uint32_t mco_ctrl  = RCC->CFGR & (RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE);
    const uint32_t af_ctrl   = (GPIOA->AFR[1] >> ((PA_MCO - 8U) * 4U)) & 0xFUL;
    delay_ms(2U);
    LOG("TREF control PA8 AF%lu (TIM1_CH1), TIM1EN C1=%lu C2=%lu, RCC_CFGR=0x%08lX",
        (unsigned long)af_ctrl, (unsigned long)tim1_c1, (unsigned long)tim1_c2, (unsigned long)RCC->CFGR);
    if (!sync0())          /* host: no edge on DIO14 */
    {
        return;
    }
    if (!pin_cfg(GPIOA, PA_MCO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U))
    {
        return;
    }
    mco_restore();
    const uint32_t cfgr_after = RCC->CFGR;
    verdict((mco_on == (RCC_CFGR_MCO1_1 | RCC_CFGR_MCO1PRE_3)) && (mco_ctrl == mco_on),
            "TREF MCO1 fields read back 0x%08lX (HSE/8), still 0x%08lX during the control",
            (unsigned long)mco_on, (unsigned long)mco_ctrl);
    verdict((af_ctrl == TST_AF_TIM1) && (tim1_c1 == 0U) && (tim1_c2 == 0U),
            "TREF control: PA8 AFR=%lu (TIM1_CH1), TIM1 clock off in both banks (C1=%lu C2=%lu)",
            (unsigned long)af_ctrl, (unsigned long)tim1_c1, (unsigned long)tim1_c2);
    verdict(cfgr_after == cfgr_before, "TREF RCC_CFGR restored: before 0x%08lX after 0x%08lX",
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
    LOG("TICK %lu (HAL_GetTick at the banner)", (unsigned long)HAL_GetTick());

    step_t0a();
    step_tref();
    step_t0b();
    step_t0b_dout();
    step_t0b_s12();
    if (step_begin("DONE"))
    {
        step_end();
    }
    bench_safe();
    verdict(!s_bench_abort, "bench completed without abort");

    (void)cm4_send(TST_CMD_DONE, 0U);
    verdict((a->m4_timeout == 0U) && (a->m4_aborts == 0U), "CM4 finished: timeout %lu, aborts %lu",
            (unsigned long)a->m4_timeout, (unsigned long)a->m4_aborts);
    LOG("TICK %lu (HAL_GetTick at the verdict)", (unsigned long)HAL_GetTick());
    LOG("Status_Task stack high-water mark %lu words", (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    LOG("FW VERDICT=%s failures=%lu", (s_fail == 0U) ? "PASS" : "FAIL", (unsigned long)s_fail);

    (void)xTaskResumeAll();
}
