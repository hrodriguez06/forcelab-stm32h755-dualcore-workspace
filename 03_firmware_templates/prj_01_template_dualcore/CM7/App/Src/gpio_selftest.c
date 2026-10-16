/**
  ******************************************************************************
  * @file    gpio_selftest.c
  * @brief   00_gpio selftest, CPU1 side (preset selftest-gpio).
  *
  *          Runs the tests that need no bench on both cores (T1, T2, TB, B1,
  *          T15), then the Analog Discovery 2 bench steps (T0a, T0b, T3-T10),
  *          then T16 (LCKR, last: a lock lasts until reset), and prints the
  *          verdict. The host side is tools/gpio_hw_check.py; wiring, safety
  *          rules and the meaning of every test are in
  *          01_shared_drivers/00_gpio/README.md.
  *
  *          Electrical safety (no series resistors on the bench):
  *            S2  bench_safe(): every wired pin in input, MCU_ACK low. At start,
  *                on any abort, on a timeout and at the end.
  *            S4  bench_claim(): the only way a wired pin becomes an output, OD
  *                or AF. The permission comes from the CURRENT step of
  *                k_steps[], which only step_begin() sets; callers cannot
  *                pass a step.
  *            S5  buschk(): the bus is shown released (both pulls followed) in
  *                the same step, before either side drives it.
  *            S7  every step is announced and waits for the host before it
  *                moves anything; 10 s without the host -> S2 and abort.
  ******************************************************************************
  */

#include "gpio_selftest.h"
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "gpio_ll.h"
#include "gpio_hal.h"
#include "shared_memory.h"
#include "app_log.h"
#include "stm32h7xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG(fmt, ...)       APP_LOG("[gpio] " fmt, ##__VA_ARGS__)

#define photo_take          gst_photo_take
#define photo_equal         gst_photo_equal

#define BENCH_VERSION       1U
#define CYC_PER_US          400U
#define CYC_PER_MS          400000U
#define SYNC_TIMEOUT_CYC    4000000000UL          /* 10 s at 400 MHz (CYCCNT wraps at 10.7 s) */
#define CM4_TIMEOUT_MS      30000U

/* ---- Bench wiring (README "Bench") ---------------------------------------------- */
#define PE_REQ              2U        /* HOST_REQ, AD2 -> MCU (DIO12)  */
#define PE_EXTI3            3U        /* DIO8,  AD2 only, unused here  */
#define PE_EXTI4            4U        /* DIO10, AD2 only, unused here  */
#define PE_T8               5U        /* DIO9,  MCU only               */
#define PE_SPARE6           6U        /* DIO11, MCU only, unused here  */
#define PE_ACK              7U        /* MCU_ACK, MCU -> AD2 (DIO13)   */
#define BUS_SHIFT           8U        /* PE8..PE15 = DIO0..DIO7        */
#define BUS_MASK            0xFF00UL
#define PA_MCO              8U        /* DIO14, MCU only               */
#define PF_SCOPE1           14U       /* scope 1, 22k to GND, 150 pF   */
#define PF_SCOPE2           15U       /* scope 2, 22k to 3V3, 150 pF   */

typedef enum { BUS_NONE = 0, BUS_MCU, BUS_AD2 } Bus_t;
static const char *const k_bus_name[] = { "NONE", "MCU", "AD2" };

typedef struct
{
    const char *name;
    Bus_t       bus;        /* who may drive PE8..PE15 in this step           */
    uint16_t    pa;         /* other wired pins the MCU may drive: GPIOA bits */
    uint16_t    pe;         /*                                     GPIOE bits */
    uint16_t    pf;         /*                                     GPIOF bits */
} Step_t;

/* The single table of permissions (S4). The host mirrors names and bus owners
   (S7) and checks every announcement against its own copy. */
static const Step_t k_steps[] = {
    { "T0A",         BUS_NONE, 0U,             0U,             0U },
    { "T0B",         BUS_NONE, 0U,             1U << PE_T8,    0U },   /* PE5: the accepted control */
    { "T0B_S5NEG",   BUS_MCU,  0U,             0U,             0U },
    { "T0B_TIMEOUT", BUS_NONE, 0U,             0U,             0U },
    { "T3_BM",       BUS_MCU,  0U,             0U,             0U },
    { "T3_LL",       BUS_MCU,  0U,             0U,             0U },
    { "T3_HAL",      BUS_MCU,  0U,             0U,             0U },
    { "T4_BM",       BUS_AD2,  0U,             0U,             0U },
    { "T4_LL",       BUS_AD2,  0U,             0U,             0U },
    { "T4_HAL",      BUS_AD2,  0U,             0U,             0U },
    { "T5T6",        BUS_NONE, 0U,             0U,             (1U << PF_SCOPE1) | (1U << PF_SCOPE2) },
    { "T8_BM",       BUS_NONE, 0U,             1U << PE_T8,    0U },
    { "T8_LL",       BUS_NONE, 0U,             1U << PE_T8,    0U },
    { "T8_HAL",      BUS_NONE, 0U,             1U << PE_T8,    0U },
    { "T8_CTRL",     BUS_NONE, 0U,             1U << PE_T8,    0U },
    { "T9",          BUS_NONE, 0U,             0U,             (1U << PF_SCOPE1) | (1U << PF_SCOPE2) },
    { "T10_BM",          BUS_NONE, 1U << PA_MCO,   0U,                                 0U },
    { "T10_LL",          BUS_NONE, 1U << PA_MCO,   0U,                                 0U },
    { "T10_HAL",         BUS_NONE, 1U << PA_MCO,   0U,                                 0U },
    { "T11_CM7_RISING",  BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T11_CM7_FALLING", BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T11_CM7_BOTH",    BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T11_CM7_EVENT",   BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T11_CM4_RISING",  BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T11_CM4_FALLING", BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T11_CM4_BOTH",    BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T11_CM4_EVENT",   BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T12",             BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T13_CM7",         BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T13_CM4",         BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T14",             BUS_NONE, 0U,             (1U << PE_T8) | (1U << PE_SPARE6), 0U },
    { "T17",             BUS_NONE, 0U,             0U,                                 0U },
    { "DONE",            BUS_NONE, 0U,             0U,                                 0U },
};

static const Step_t *s_step;          /* NULL between steps: nothing wired may be driven */
static bool          s_bench_abort;
static uint32_t      s_fail;          /* firmware-side verdicts that failed */
static uint32_t      s_mco_saved;     /* RCC_CFGR MCO1 fields before T10     */
static bool          s_mco_changed;

static volatile uint32_t s_cm7_exti0_count = 0U;
static volatile uint32_t s_cm7_exti0_pend_ok = 0U;
static volatile uint32_t s_cm7_exti3_count = 0U;

void EXTI0_IRQHandler(void)
{
    GPIOE->BSRR = (1UL << PE_T8);
    const uint32_t pend = (uint32_t)GPIO_BM_ExtiTakePending(1UL << 0U);
    if (pend == (1UL << 0U))
    {
        s_cm7_exti0_pend_ok++;
    }
    s_cm7_exti0_count++;
    GPIOE->BSRR = (1UL << (PE_T8 + 16U));
}

void EXTI3_IRQHandler(void)
{
    GPIOE->BSRR = (1UL << PE_T8);
    const uint32_t pend = (uint32_t)GPIO_BM_ExtiTakePending(1UL << 3U);
    s_cm7_exti3_count++;
    (void)pend;
    GPIOE->BSRR = (1UL << (PE_T8 + 16U));
}

static const char *const k_tier_name[GST_TIER_COUNT] = { "BM", "LL", "HAL" };

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
    char    line[128];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!ok)
    {
        s_fail++;
    }
    LOG("%s -> %s", line, ok ? "PASS" : "FAIL");
}

static uint32_t crc32_update(uint32_t crc, uint8_t b)
{
    crc ^= b;
    for (uint32_t k = 0U; k < 8U; k++)
    {
        crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
    return crc;
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
   0 while the AD2 does not drive it), MCU_ACK driven low. PE0 and PE1 (LD2) are
   never touched. Raw writes: the safe state must not depend on the code under
   test. The cross-core lock is asked for, and the writes happen even if it is
   refused, with this core's interrupts masked: safety first, and CPU2 never
   configures GPIOA or PF14/PF15, and configures PE6 only when granted permission by CPU1. */
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
    GPIOE->MODER = GPIOE->MODER & ~mask2;
    GPIOE->PUPDR = (GPIOE->PUPDR & ~mask2) | (2UL << (PE_REQ * 2U));

    GPIOE->BSRR   = 1UL << (PE_ACK + 16U);                            /* level first */
    GPIOE->OTYPER = GPIOE->OTYPER & ~(1UL << PE_ACK);
    GPIOE->PUPDR  = GPIOE->PUPDR & ~(3UL << (PE_ACK * 2U));
    GPIOE->MODER  = (GPIOE->MODER & ~(3UL << (PE_ACK * 2U))) | (1UL << (PE_ACK * 2U));

    const uint32_t f2 = (3UL << (PF_SCOPE1 * 2U)) | (3UL << (PF_SCOPE2 * 2U));
    GPIOF->MODER = GPIOF->MODER & ~f2;
    GPIOF->PUPDR = GPIOF->PUPDR & ~f2;

    GPIOA->MODER = GPIOA->MODER & ~(3UL << (PA_MCO * 2U));
    GPIOA->PUPDR = GPIOA->PUPDR & ~(3UL << (PA_MCO * 2U));
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
    LOG("ABORT step=%s reason=%s", (s_step != NULL) ? s_step->name : "-", reason);
}

/* ---- S4: the claim --------------------------------------------------------------------- */

static bool claim_one(const GPIO_TypeDef *port, uint32_t pin)
{
    if (port == GPIOE)
    {
        if ((pin <= 1U) || (pin == PE_REQ) || (pin == PE_EXTI3) || (pin == PE_EXTI4) || (pin == PE_ACK))
        {
            return false;   /* LD2, AD2-only lines, and MCU_ACK (bench_safe owns it) */
        }
        if (pin >= BUS_SHIFT)
        {
            return (s_step != NULL) && (s_step->bus == BUS_MCU);
        }
        return (s_step != NULL) && ((s_step->pe & (1U << pin)) != 0U);
    }
    if ((port == GPIOA) && (pin == PA_MCO))
    {
        return (s_step != NULL) && ((s_step->pa & (1U << pin)) != 0U);
    }
    if ((port == GPIOF) && ((pin == PF_SCOPE1) || (pin == PF_SCOPE2)))
    {
        return (s_step != NULL) && ((s_step->pf & (1U << pin)) != 0U);
    }
    return true;   /* not wired: nothing outside can be driving it */
}

/* All or nothing: every pin of the mask is checked before anything is written. */
static bool bench_claim(const GPIO_TypeDef *port, uint32_t mask)
{
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if (((mask & (1UL << p)) != 0U) && !claim_one(port, p))
        {
            return false;
        }
    }
    return true;
}

/* A wired pin through a tier, output/AF claimed first. */
static bool bench_cfg(GstTier_t t, GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t otype,
                      uint32_t speed, uint32_t pull, uint32_t af, uint32_t level)
{
    const bool drive = (mode == GPIO_BM_MODE_OUTPUT) || (mode == GPIO_BM_MODE_AF);
    if (drive && !bench_claim(port, 1UL << pin))
    {
        bench_abort("claim refused");
        return false;
    }
    const GstPinReq_t r = { port, pin, mode, otype, speed, pull, af, level, false };
    const uint32_t st = gst_apply(t, &r);
    if (st != (uint32_t)GPIO_BM_OK)
    {
        LOG("%s config of pin %lu failed, status %lu", k_tier_name[t], (unsigned long)pin, (unsigned long)st);
        bench_abort("config");
        return false;
    }
    return true;
}

/* Raw input/pull on wired pins of one port (never an output: no claim needed). */
static void pins_input_raw(GPIO_TypeDef *port, uint32_t mask, uint32_t pull)
{
    uint32_t m2 = 0U;
    uint32_t p2 = 0U;
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if ((mask & (1UL << p)) != 0U)
        {
            m2 |= 3UL << (p * 2U);
            p2 |= pull << (p * 2U);
        }
    }
    GpioBm_Section_t s;
    const bool locked = (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK);
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    port->MODER = port->MODER & ~m2;
    port->PUPDR = (port->PUPDR & ~m2) | p2;
    __set_PRIMASK(pm);
    if (locked)
    {
        GPIO_BM_SectionExit(&s);
    }
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

typedef void (*SyncCb_t)(void *ctx);

/* One four-phase handshake. The MCU state is ready when ACK rises; the host
   acts or measures, then raises REQ; `cb` runs right after REQ is seen
   (with interrupts masked if `irq_off`, for the captures that trigger on REQ);
   then ACK drops and the MCU waits for REQ to drop. */
static bool sync_cb(SyncCb_t cb, void *ctx, bool irq_off)
{
    if (s_bench_abort)
    {
        return false;
    }
    const uint32_t pm = __get_PRIMASK();
    ack(true);
    if (irq_off)
    {
        __disable_irq();
    }
    bool ok = wait_req(true, SYNC_TIMEOUT_CYC);
    if (ok && (cb != NULL))
    {
        cb(ctx);
    }
    __set_PRIMASK(pm);
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

static bool sync0(void)
{
    return sync_cb(NULL, NULL, false);
}

static bool step_begin(const char *name)
{
    if (s_bench_abort)
    {
        return false;
    }
    s_step = NULL;
    for (uint32_t i = 0U; i < (sizeof(k_steps) / sizeof(k_steps[0])); i++)
    {
        if (strcmp(k_steps[i].name, name) == 0)
        {
            s_step = &k_steps[i];
        }
    }
    if (s_step == NULL)
    {
        bench_abort("unknown step");
        return false;
    }
    LOG("STEP %s bus=%s v=%u", s_step->name, k_bus_name[s_step->bus], (unsigned)BENCH_VERSION);
    return sync0();   /* nothing moves until the host has checked the announcement */
}

static void step_end(void)
{
    s_step = NULL;
}

/* ---- S5: the bus is released, shown electrically --------------------------------------- */

static uint32_t bus_idr(void)
{
    return (GPIOE->IDR >> BUS_SHIFT) & 0xFFUL;
}

static bool buschk(void)
{
    pins_input_raw(GPIOE, BUS_MASK, GPIO_BM_PULL_UP);
    delay_ms(1U);
    const uint32_t up = bus_idr();
    if (!sync0())
    {
        return false;
    }
    pins_input_raw(GPIOE, BUS_MASK, GPIO_BM_PULL_DOWN);
    delay_ms(1U);
    const uint32_t dn = bus_idr();
    if (!sync0())
    {
        return false;
    }
    pins_input_raw(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
    if ((up != 0xFFU) || (dn != 0x00U))
    {
        LOG("BUSCHK idr up=0x%02lX down=0x%02lX", (unsigned long)up, (unsigned long)dn);
        bench_abort("bus not released");
        return false;
    }
    return true;
}

static bool bus_moder_is(uint32_t per_pin)
{
    for (uint32_t p = BUS_SHIFT; p < 16U; p++)
    {
        if (((GPIOE->MODER >> (p * 2U)) & 3UL) != per_pin)
        {
            return false;
        }
    }
    return true;
}

/* ==========================================================================================
 * CPU2 commands
 * ========================================================================================== */

static bool cm4_send(GstCmd_t cmd, uint32_t a0, uint32_t a1, bool wait_ack, uint32_t *seq_out)
{
    GstArea_t *a = GST_AREA;
    a->cmd  = (uint32_t)cmd;
    a->arg0 = a0;
    a->arg1 = a1;
    __DMB();
    const uint32_t seq = a->cmd_seq + 1U;
    a->cmd_seq = seq;
    __DMB();
    if (seq_out != NULL)
    {
        *seq_out = seq;
    }
    if (!wait_ack)
    {
        return true;
    }
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

static bool cm4_wait(volatile uint32_t *field, uint32_t value)
{
    const uint32_t t0 = HAL_GetTick();
    while (*field != value)
    {
        if ((HAL_GetTick() - t0) > CM4_TIMEOUT_MS)
        {
            s_fail++;
            return false;
        }
    }
    __DMB();
    return true;
}

/* ==========================================================================================
 * T1, T2, TB, B1 on both cores
 * ========================================================================================== */

static void report_t1(const char *core, GstTier_t t, const GstT1Result_t *r)
{
    verdict((r->fail == 0U) && (r->cases > 0U) && (r->ctrl_changed == 1U),
            "T1 %s %-3s refusals=%lu fail=%lu control_wrote=%lu", core, k_tier_name[t],
            (unsigned long)r->cases, (unsigned long)r->fail, (unsigned long)r->ctrl_changed);
}

static void report_t2(const char *core, const GstT2Result_t *r)
{
    bool ok = (r->readcfg_bad == 0U) && (r->legacy_bad == 0U) && (r->ctrl_detected == 1U);
    for (uint32_t t = 0U; t < (uint32_t)GST_TIER_COUNT; t++)
    {
        const bool tier_ok = (r->cases[t] == 1440U) && (r->rejected[t] == 288U) && (r->diff_rm[t] == 0U) &&
                             (r->odr_bad[t] == 0U) && ((t == (uint32_t)GST_TIER_HAL) || (r->diff_hal[t] == 0U));
        ok = ok && tier_ok;
        LOG("T2 %s %-3s configs=%lu analog+pull refused=%lu diff_rm=%lu diff_hal=%lu odr_bad=%lu",
            core, k_tier_name[t], (unsigned long)r->cases[t], (unsigned long)r->rejected[t],
            (unsigned long)r->diff_rm[t], (unsigned long)r->diff_hal[t], (unsigned long)r->odr_bad[t]);
    }
    verdict(ok, "T2 %s readconfig_bad=%lu legacy_bad=%lu comparator_control=%lu raw_hal_g7_kept=%lu/6",
            core, (unsigned long)r->readcfg_bad, (unsigned long)r->legacy_bad,
            (unsigned long)r->ctrl_detected, (unsigned long)r->hal_g7_pupdr_kept);
}

static void report_tb(const char *core, const GstTbResult_t *r)
{
    verdict((r->fail == 0U) && (r->checks == 274U) && (r->busy_calls == 32U) && (r->ctrl_wrote == 1U) &&
            (r->swap_ctrl == 1U),
            "TB %s checks=%lu fail=%lu busy_calls=%lu/32 control_wrote=%lu swap_control=%lu", core,
            (unsigned long)r->checks, (unsigned long)r->fail, (unsigned long)r->busy_calls,
            (unsigned long)r->ctrl_wrote, (unsigned long)r->swap_ctrl);
}

static void report_b1(const char *core, const GstB1Result_t *r, uint32_t mhz)
{
    for (uint32_t t = 0U; t < (uint32_t)GST_TIER_COUNT; t++)
    {
        LOG("B1 %s %-3s ConfigPin cycles: no hook %lu/%lu, hook %lu/%lu (min/max) = %lu.%02lu us max with hook",
            core, k_tier_name[t], (unsigned long)r->nohook_min[t], (unsigned long)r->nohook_max[t],
            (unsigned long)r->hook_min[t], (unsigned long)r->hook_max[t],
            (unsigned long)(r->hook_max[t] / mhz), (unsigned long)(((r->hook_max[t] % mhz) * 100U) / mhz));
    }
}

static void run_register_tests(void)
{
    GstArea_t     *a = GST_AREA;
    GstT1Result_t  t1;
    GstT2Result_t  t2;
    GstTbResult_t  tb;
    GstB1Result_t  b1;

    for (uint32_t t = 0U; t < (uint32_t)GST_TIER_COUNT; t++)
    {
        gst_t1_run((GstTier_t)t, &t1);
        report_t1("cm7", (GstTier_t)t, &t1);
    }
    if (cm4_send(GST_CMD_T1, 0U, 0U, true, NULL))
    {
        for (uint32_t t = 0U; t < (uint32_t)GST_TIER_COUNT; t++)
        {
            GstT1Result_t r;
            memcpy(&r, (const void *)&a->m4_t1[t], sizeof(r));
            report_t1("cm4", (GstTier_t)t, &r);
        }
    }

    gst_t2_run(&t2);
    report_t2("cm7", &t2);
    if (cm4_send(GST_CMD_T2, 0U, 0U, true, NULL))
    {
        memcpy(&t2, (const void *)&a->m4_t2, sizeof(t2));
        report_t2("cm4", &t2);
    }

    gst_tb_run(&tb);
    report_tb("cm7", &tb);
    if (cm4_send(GST_CMD_TB, 0U, 0U, true, NULL))
    {
        memcpy(&tb, (const void *)&a->m4_tb, sizeof(tb));
        report_tb("cm4", &tb);
    }

    gst_b1_run(&b1);
    report_b1("cm7", &b1, 400U);
    if (cm4_send(GST_CMD_B1, 0U, 0U, true, NULL))
    {
        memcpy(&b1, (const void *)&a->m4_b1, sizeof(b1));
        report_b1("cm4", &b1, 200U);
    }
}

/* ==========================================================================================
 * T15: the race between the cores, and against an ISR of the same core
 * ========================================================================================== */

#define ISR_PIN   6U     /* PF6: the TIM17 ISR's pin, next to PF10 in the same registers */

static volatile uint32_t s_isr_method;       /* 0: raw RMW, 1: gpio_ll */
static volatile uint32_t s_isr_entries;
static volatile uint32_t s_isr_corrupt;
static volatile uint32_t s_isr_err;
static volatile uint32_t s_isr_which;

void TIM17_IRQHandler(void)
{
    TIM17->SR = ~TIM_SR_UIF;
    const GstPinReq_t prev = gst_race_cfg(ISR_PIN, s_isr_which);
    if (!gst_pin_is(&prev))
    {
        s_isr_corrupt++;             /* the thread wrote back an old PF6 since last time */
    }
    s_isr_which ^= 1U;
    const GstPinReq_t c = gst_race_cfg(ISR_PIN, s_isr_which);
    if (s_isr_method == 0U)
    {
        gst_raw_config(&c);
    }
    else if (gst_apply(GST_TIER_LL, &c) != (uint32_t)GPIO_BM_OK)
    {
        s_isr_err++;
    }
    if (!gst_pin_is(&c))
    {
        s_isr_corrupt++;
    }
    s_isr_entries++;
    __DSB();
}

static void tim17_start(uint32_t method)
{
    const GstPinReq_t a0 = gst_race_cfg(ISR_PIN, 0U);
    gst_raw_config(&a0);
    s_isr_which   = 0U;
    s_isr_method  = method;
    s_isr_entries = 0U;
    s_isr_corrupt = 0U;
    s_isr_err     = 0U;

    RCC->APB2ENR |= RCC_APB2ENR_TIM17EN;
    (void)RCC->APB2ENR;
    TIM17->CR1  = 0U;
    TIM17->PSC  = 0U;
    TIM17->ARR  = 9999U;           /* ~20 kHz from the 200 MHz timer clock */
    TIM17->EGR  = TIM_EGR_UG;
    TIM17->SR   = 0U;
    TIM17->DIER = TIM_DIER_UIE;
    NVIC_SetPriority(TIM17_IRQn, 6U);
    NVIC_ClearPendingIRQ(TIM17_IRQn);
    NVIC_EnableIRQ(TIM17_IRQn);
    TIM17->CR1  = TIM_CR1_CEN;
}

static void tim17_stop(void)
{
    TIM17->CR1  = 0U;
    TIM17->DIER = 0U;
    NVIC_DisableIRQ(TIM17_IRQn);
    TIM17->SR   = 0U;
    NVIC_ClearPendingIRQ(TIM17_IRQn);
    RCC->APB2ENR &= ~RCC_APB2ENR_TIM17EN;
}

static uint32_t s_hold_max_cm7;
static uint32_t s_hold_max_cm4;
static uint32_t s_race_seed;       /* this run's T15 seed; phase k runs with seed ^ k */
static uint32_t s_race_phase;

/* -DGST_RACE_SEED=0x... runs T15 with that seed. It only changes this word,
   read at run time (volatile): the image differs from the default one in the
   4 bytes of the seed (docs/gpio_seed_replay_bin_cmp_fa44386.txt). That
   repeats the sequence of delays, not the interleaving: the replay of a run
   gives other T15 counts (docs/gpio_dev_replay_seed_d9d40bdd_fa44386.txt), and
   the source of the remaining difference was not isolated. 0 = the RNG's
   word. A define that selected code changed the timing even more
   (docs/gpio_dev_replay_seed_6f6841b8_9ccc971.txt). */
#if !defined(GST_RACE_SEED)
#define GST_RACE_SEED 0UL
#endif
static const volatile uint32_t k_race_seed_fixed = (uint32_t)(GST_RACE_SEED);

/* T15 starts this long after the RNG read begins. What runs before it must
   fit, or the start moves with the RNG again: the RNG's bounded waits
   (2 x 1 ms) and three blocking log lines (about 8 ms each at 115200 baud).
   The selftest fails if it does not fit. */
#define RACE_START_MS       40U
#define RACE_START_CYC      (RACE_START_MS * CYC_PER_MS)

/* R2: one 32-bit word from the RNG (HSI48 kernel clock, RNGSEL = 0 at reset).
   CPU1 only: RCC_CR is CPU1's by the boot contract. Leaves HSI48, the RNG
   and its clock as it found them. Returns false with the reason on a timeout,
   a seed or clock error, or RNGSEL not on HSI48. */
static bool race_seed_from_rng(uint32_t *seed, const char **why)
{
    if ((RCC->D2CCIP2R & RCC_D2CCIP2R_RNGSEL) != 0UL)
    {
        *why = "RNGSEL not HSI48";
        return false;
    }

    const bool hsi48_was_on = (RCC->CR & RCC_CR_HSI48ON) != 0UL;
    bool       ok = false;

    *why = "HSI48RDY timeout";
    RCC->CR |= RCC_CR_HSI48ON;
    uint32_t t0 = DWT->CYCCNT;
    while (((RCC->CR & RCC_CR_HSI48RDY) == 0UL) && ((DWT->CYCCNT - t0) <= CYC_PER_MS))
    {
    }
    if ((RCC->CR & RCC_CR_HSI48RDY) != 0UL)
    {
        RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;
        (void)RCC->AHB2ENR;
        RNG->CR = RNG_CR_RNGEN;
        *why = "DRDY timeout";
        t0 = DWT->CYCCNT;
        while ((DWT->CYCCNT - t0) <= CYC_PER_MS)
        {
            const uint32_t sr = RNG->SR;
            if ((sr & (RNG_SR_SECS | RNG_SR_CECS)) != 0UL)
            {
                *why = ((sr & RNG_SR_SECS) != 0UL) ? "RNG SECS" : "RNG CECS";
                break;
            }
            if ((sr & RNG_SR_DRDY) != 0UL)
            {
                *seed = RNG->DR;
                *why  = "ok";
                ok    = true;
                break;
            }
        }
        RNG->CR = 0U;
        RCC->AHB2ENR &= ~RCC_AHB2ENR_RNGEN;
    }
    if (!hsi48_was_on)
    {
        RCC->CR &= ~RCC_CR_HSI48ON;
    }
    return ok;
}

/* Seed of the next phase, published to CPU2 before GST_CMD_RACE_ARM. */
static void race_seed_next(void)
{
    GST_AREA->race_seed = s_race_seed ^ s_race_phase;
    s_race_phase++;
    __DMB();
}

/* Sections a race loop opens: one per call, except the methods that bypass the driver. */
static uint32_t expected_locks(int m, uint32_t iters)
{
    if ((m < 0) || (m == (int)GST_RACE_RAW) || (m == (int)GST_RACE_HAL_RAW))
    {
        return 0U;
    }
    return iters;
}

/* One race phase. cm7 < 0 or cm4 < 0: that core stays out (control of the control). */
static void race_phase(const char *name, int cm7, int cm4, uint32_t iters, bool shim, bool expect_corrupt)
{
    GstArea_t      *a = GST_AREA;
    GstRaceResult_t r7;
    GstRaceResult_t r4;
    uint32_t        seq = 0U;

    memset(&r7, 0, sizeof(r7));
    memset(&r4, 0, sizeof(r4));

    gst_race_prepare();
    a->go = 0U;
    a->in_loop[0] = 0U;
    a->in_loop[1] = 0U;
    a->locked_calls = 0U;
    race_seed_next();

    if (shim)
    {
        gst_shim_install(true);
        (void)cm4_send(GST_CMD_SHIM_ON, 0U, 0U, true, NULL);
    }

    if (cm4 >= 0)
    {
        (void)cm4_send(GST_CMD_RACE_ARM, (uint32_t)cm4, iters, false, &seq);
        (void)cm4_wait(&a->armed_seq, seq);
    }
    a->go = 1U;
    __DMB();
    if (cm7 >= 0)
    {
        gst_race_run((GstRace_t)cm7, iters, &r7);
    }
    if (cm4 >= 0)
    {
        if (cm4_wait(&a->ack_seq, seq))
        {
            memcpy(&r4, (const void *)&a->m4_race, sizeof(r4));
        }
    }
    a->go = 0U;

    if (shim)
    {
        gst_shim_install(false);
        (void)cm4_send(GST_CMD_SHIM_OFF, 0U, 0U, true, NULL);
    }

    const uint32_t corrupt = r7.corrupt + r4.corrupt;
    const bool both = (cm7 >= 0) && (cm4 >= 0);
    bool ok;
    if (expect_corrupt)
    {
        ok = (corrupt > 0U) && (!both || ((r7.overlap > 0U) && (r4.overlap > 0U)));
    }
    else
    {
        ok = (corrupt == 0U) && (r7.status_err == 0U) && (r4.status_err == 0U) &&
             (!both || ((r7.overlap > 0U) && (r4.overlap > 0U)));
    }
    if (shim)
    {
        ok = ok && (a->locked_calls == (r7.locked + r4.locked)) &&
             (r7.locked == expected_locks(cm7, iters)) && (r4.locked == expected_locks(cm4, iters));
        s_hold_max_cm7 = (r7.hold_max > s_hold_max_cm7) ? r7.hold_max : s_hold_max_cm7;
        s_hold_max_cm4 = (r4.hold_max > s_hold_max_cm4) ? r4.hold_max : s_hold_max_cm4;
    }

    LOG("T15 %s cm7: iters=%lu corrupt=%lu err=%lu overlap=%lu locked=%lu hold_max=%lu cyc time=%lu ms",
        name, (unsigned long)r7.iters, (unsigned long)r7.corrupt, (unsigned long)r7.status_err,
        (unsigned long)r7.overlap, (unsigned long)r7.locked, (unsigned long)r7.hold_max,
        (unsigned long)(r7.cycles / CYC_PER_MS));
    LOG("T15 %s cm4: iters=%lu corrupt=%lu err=%lu overlap=%lu locked=%lu hold_max=%lu cyc time=%lu ms",
        name, (unsigned long)r4.iters, (unsigned long)r4.corrupt, (unsigned long)r4.status_err,
        (unsigned long)r4.overlap, (unsigned long)r4.locked, (unsigned long)r4.hold_max,
        (unsigned long)(r4.cycles / 200000U));
    verdict(ok, "T15 %s corrupt=%lu (expected %s) locked_calls=%lu", name, (unsigned long)corrupt,
            expect_corrupt ? ">0 with overlap on both cores" : "0", (unsigned long)a->locked_calls);
}

static void isr_phase(const char *name, GstRace_t main_m, uint32_t isr_method, uint32_t iters, bool expect_corrupt)
{
    GstRaceResult_t r;
    gst_race_prepare();
    GST_AREA->in_loop[1] = 0U;
    race_seed_next();
    tim17_start(isr_method);
    gst_race_run(main_m, iters, &r);
    tim17_stop();

    const uint32_t corrupt = r.corrupt + s_isr_corrupt;
    const bool ok = expect_corrupt ? ((corrupt > 0U) && (s_isr_entries > 0U))
                                   : ((corrupt == 0U) && (s_isr_err == 0U) && (r.status_err == 0U) &&
                                      (s_isr_entries > 0U));
    LOG("T15 %s thread: iters=%lu corrupt=%lu err=%lu; isr: entries=%lu corrupt=%lu err=%lu",
        name, (unsigned long)r.iters, (unsigned long)r.corrupt, (unsigned long)r.status_err,
        (unsigned long)s_isr_entries, (unsigned long)s_isr_corrupt, (unsigned long)s_isr_err);
    verdict(ok, "T15 %s corrupt=%lu (expected %s)", name, (unsigned long)corrupt, expect_corrupt ? ">0" : "0");
}

static void run_race_tests(void)
{
    const uint32_t f7 = g_shared.m7_gpio_lock_fail_count;
    const uint32_t f4 = g_shared.m4_gpio_lock_fail_count;

    /* R2: a new seed per run, so a second run samples other interleavings.
       The RNG is read and checked in every build; a fixed seed
       (GST_RACE_SEED, above) then replaces its word. HSI48 and the RNG start
       after a variable delay, which would shift the cores' relative phase for
       everything after it (docs/gpio_dev_replay_seed_3f9a7bfb_7cedf36*.txt):
       T15 starts at a fixed deadline from t_seed instead, which removes that
       variation (docs/gpio_dev_deadline_*_7cedf36-dirty*.txt). A replay with
       GST_RACE_SEED still does not give the same counts (see above). */
    const uint32_t t_seed = DWT->CYCCNT;
    const char    *why;
    const bool     rng_ok = race_seed_from_rng(&s_race_seed, &why);
    const uint32_t fixed  = k_race_seed_fixed;
    const uint32_t rng    = s_race_seed;
    s_race_seed = (fixed != 0UL) ? fixed : rng;
    LOG("T15 seed=0x%08lX (%s, RNG gave 0x%08lX; phase k uses seed^k)", (unsigned long)s_race_seed,
        (fixed != 0UL) ? "fixed by GST_RACE_SEED" : "RNG", (unsigned long)rng);
    verdict(rng_ok, "T15 seed from the RNG: %s", why);
    const uint32_t used = DWT->CYCCNT - t_seed;
    verdict(used < ((RACE_START_MS - 10U) * CYC_PER_MS), "T15 start deadline: %lu us of %u ms used before this line",
            (unsigned long)(used / CYC_PER_US), (unsigned)RACE_START_MS);
    while ((DWT->CYCCNT - t_seed) < RACE_START_CYC)
    {
    }
    s_race_phase = 0U;

    /* Controls first: without protection the race must be reproduced, and the
       detector must count nothing when only one core runs. */
    race_phase("a_raw_cm7_alone",       GST_RACE_RAW, -1,           500000U, false, false);
    race_phase("b_raw_cm4_alone",       -1,           GST_RACE_RAW, 500000U, false, false);
    race_phase("c_raw_both",            GST_RACE_RAW, GST_RACE_RAW, 500000U, false, true);
    /* Every tier on each core, through the production hook (counting shim). */
    race_phase("d_hal7_ll4",            GST_RACE_HAL, GST_RACE_LL,  200000U, true,  false);
    race_phase("e_ll7_hal4",            GST_RACE_LL,  GST_RACE_HAL, 200000U, true,  false);
    race_phase("f_bm7_bm4",             GST_RACE_BM,  GST_RACE_BM,  200000U, true,  false);
    /* The MSP pattern (HAL_GPIO_Init between SectionEnter/Exit), and its control:
       one writer that skips the section breaks the lock for everybody. */
    race_phase("g_halsection7_bm4",     GST_RACE_HAL_IN_SECTION, GST_RACE_BM, 200000U, true, false);
    race_phase("h_halraw7_bm4",         GST_RACE_HAL_RAW,        GST_RACE_BM, 200000U, true, true);
    /* Same core: an ISR reconfiguring PF6 while the thread reconfigures PF10. */
    isr_phase("i_isr_raw",  GST_RACE_RAW, 0U, 500000U, true);
    isr_phase("j_isr_tier", GST_RACE_HAL, 1U, 200000U, false);

    const uint32_t d7 = g_shared.m7_gpio_lock_fail_count - f7;
    const uint32_t d4 = g_shared.m4_gpio_lock_fail_count - f4;
    verdict((d7 == 0U) && (d4 == 0U), "T15 production hook timeouts during the race: cm7=%lu cm4=%lu",
            (unsigned long)d7, (unsigned long)d4);

    /* B1: the lock limit against the longest hold measured above. */
    const uint32_t hold7_ns = (s_hold_max_cm7 * 1000U) / 400U;
    const uint32_t hold4_ns = (s_hold_max_cm4 * 1000U) / 200U;
    const uint32_t hold_ns  = (hold7_ns > hold4_ns) ? hold7_ns : hold4_ns;
    const uint32_t limit_ns = SHARED_GPIO_LOCK_US * 1000U;
    /* The margin in hundredths, truncated: the gate is the comparison, this
       only prints it (R2-b: an integer print showed 10.08x as "10x"). */
    const uint32_t margin_x100 = (hold_ns > 0U) ? ((limit_ns * 100U) / hold_ns) : 0U;
    verdict((hold_ns > 0U) && ((hold_ns * 10U) <= limit_ns),
            "B1 lock hold max: cm7 %lu cyc (%lu ns), cm4 %lu cyc (%lu ns); limit %u us = %lu.%02lux (>=10x)",
            (unsigned long)s_hold_max_cm7, (unsigned long)hold7_ns, (unsigned long)s_hold_max_cm4,
            (unsigned long)hold4_ns, (unsigned)SHARED_GPIO_LOCK_US,
            (unsigned long)(margin_x100 / 100U), (unsigned long)(margin_x100 % 100U));
}

/* ==========================================================================================
 * Bench steps
 * ========================================================================================== */

/* T0a: every line that is not the handshake follows both MCU pulls. */
static void step_t0a(void)
{
    const uint32_t e_lines = BUS_MASK | (1UL << PE_EXTI3) | (1UL << PE_EXTI4) | (1UL << PE_T8) | (1UL << PE_SPARE6);
    if (!step_begin("T0A"))
    {
        return;
    }
    uint32_t idr_e[2];
    uint32_t idr_a[2];
    for (uint32_t k = 0U; k < 2U; k++)
    {
        const uint32_t pull = (k == 0U) ? GPIO_BM_PULL_UP : GPIO_BM_PULL_DOWN;
        pins_input_raw(GPIOE, e_lines, pull);
        pins_input_raw(GPIOA, 1UL << PA_MCO, pull);
        delay_ms(1U);
        idr_e[k] = GPIOE->IDR & e_lines;
        idr_a[k] = GPIOA->IDR & (1UL << PA_MCO);
        if (!sync0())
        {
            return;
        }
    }
    pins_input_raw(GPIOE, e_lines, GPIO_BM_PULL_NONE);
    pins_input_raw(GPIOA, 1UL << PA_MCO, GPIO_BM_PULL_NONE);
    verdict((idr_e[0] == e_lines) && (idr_a[0] != 0U) && (idr_e[1] == 0U) && (idr_a[1] == 0U),
            "T0A MCU IDR with pull-up E=0x%04lX A8=%lu, with pull-down E=0x%04lX A8=%lu",
            (unsigned long)idr_e[0], (unsigned long)(idr_a[0] >> PA_MCO),
            (unsigned long)idr_e[1], (unsigned long)(idr_a[1] >> PA_MCO));
    step_end();
}

/* T0b: (b) the claim refuses what it must, (c) the host's released-bus check
   catches a driven bus, (d) a missing host sends the MCU to S2. */
static void step_t0b(void)
{
    if (!step_begin("T0B"))
    {
        return;
    }
    const uint32_t moder_e = GPIOE->MODER;
    const uint32_t moder_a = GPIOA->MODER;
    const uint32_t moder_f = GPIOF->MODER;
    const bool refused =
        !bench_claim(GPIOE, 1UL << 0) && !bench_claim(GPIOE, 1UL << 1) && !bench_claim(GPIOE, 1UL << PE_REQ) &&
        !bench_claim(GPIOE, 1UL << PE_EXTI3) && !bench_claim(GPIOE, 1UL << PE_EXTI4) &&
        !bench_claim(GPIOE, 1UL << PE_ACK) && !bench_claim(GPIOE, BUS_MASK) &&
        !bench_claim(GPIOE, 1UL << BUS_SHIFT) && !bench_claim(GPIOA, 1UL << PA_MCO) &&
        !bench_claim(GPIOF, 1UL << PF_SCOPE1) && !bench_claim(GPIOE, (1UL << PE_T8) | (1UL << 0));
    const bool accepted = bench_claim(GPIOE, 1UL << PE_T8);                 /* the control */
    const bool untouched = (GPIOE->MODER == moder_e) && (GPIOA->MODER == moder_a) && (GPIOF->MODER == moder_f);
    verdict(refused && accepted && untouched,
            "T0B(b) claim refuses PE0 PE1 PE2 PE3 PE4 PE7 bus PA8 PF14 and a mixed mask; accepts PE5 (control); MODER untouched");
    if (!sync0())
    {
        return;
    }
    step_end();

    /* (c): the MCU drives 0x5A on purpose while the AD2 only listens. The host
       runs its normal released-bus check, which must fail here. */
    if (!step_begin("T0B_S5NEG") || !buschk())   /* S5: released first, then driven */
    {
        return;
    }
    delay_ms(1U);
    if (!bench_claim(GPIOE, BUS_MASK))
    {
        bench_abort("claim refused");
        return;
    }
    GPIOE->BSRR = ((0x5AUL << BUS_SHIFT) & BUS_MASK) | (((~0x5AUL << BUS_SHIFT) & BUS_MASK) << 16U);
    for (uint32_t p = BUS_SHIFT; p < 16U; p++)
    {
        if (!bench_cfg(GST_TIER_BM, GPIOE, p, GPIO_BM_MODE_OUTPUT, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW,
                       GPIO_BM_PULL_NONE, 0U, (0x5AUL >> (p - BUS_SHIFT)) & 1UL))
        {
            return;
        }
    }
    (void)sync0();       /* host: released-bus check, pull-up phase   */
    (void)sync0();       /* host: released-bus check, pull-down phase */
    pins_input_raw(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
    const bool released = bus_moder_is(0U);
    (void)buschk();      /* and now it really is released */
    verdict(released && !s_bench_abort, "T0B(c) bus driven to 0x5A during the host check, then released (MODER bus = input)");
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
                      (((GPIOE->ODR >> PE_ACK) & 1UL) == 0U);
    verdict(!answered && safe, "T0B(d) no HOST_REQ in 10 s -> bench_safe: GPIOE MODER[31:4]=0x%08lX (0x%08lX expected), PF14/15 and PA8 input, ACK low",
            (unsigned long)e_moder, (unsigned long)expect);
    LOG("T0B timeout handled");
    step_end();
}

/* ---- T3: output operations of each tier, read by the AD2 --------------------------------- */

static void op_writeport(GstTier_t t, uint32_t mask, uint32_t val)
{
    switch (t)
    {
    case GST_TIER_BM: GPIO_BM_WritePort(GPIOE, mask, val); break;
    case GST_TIER_LL: GPIO_LL_WritePort(GPIOE, mask, val); break;
    default:
        if ((mask & val) != 0U)
        {
            HAL_GPIO_WritePin(GPIOE, (uint16_t)(mask & val), GPIO_PIN_SET);
        }
        if ((mask & ~val & 0xFFFFUL) != 0U)
        {
            HAL_GPIO_WritePin(GPIOE, (uint16_t)(mask & ~val & 0xFFFFUL), GPIO_PIN_RESET);
        }
        break;
    }
}

static void op_set(GstTier_t t, uint32_t mask)
{
    switch (t)
    {
    case GST_TIER_BM: GPIO_BM_Set(GPIOE, mask); break;
    case GST_TIER_LL: GPIO_LL_Set(GPIOE, mask); break;
    default:          HAL_GPIO_WritePin(GPIOE, (uint16_t)mask, GPIO_PIN_SET); break;
    }
}

static void op_reset(GstTier_t t, uint32_t mask)
{
    switch (t)
    {
    case GST_TIER_BM: GPIO_BM_Reset(GPIOE, mask); break;
    case GST_TIER_LL: GPIO_LL_Reset(GPIOE, mask); break;
    default:          HAL_GPIO_WritePin(GPIOE, (uint16_t)mask, GPIO_PIN_RESET); break;
    }
}

static void op_write(GstTier_t t, uint32_t pin, uint32_t level)
{
    switch (t)
    {
    case GST_TIER_BM: GPIO_BM_Write(GPIOE, pin, level); break;
    case GST_TIER_LL: GPIO_LL_Write(GPIOE, pin, level); break;
    default:          HAL_GPIO_WritePin(GPIOE, (uint16_t)(1UL << pin), (level != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET); break;
    }
}

static void op_toggle(GstTier_t t, uint32_t mask)
{
    switch (t)
    {
    case GST_TIER_BM: GPIO_BM_Toggle(GPIOE, mask); break;
    case GST_TIER_LL: GPIO_LL_Toggle(GPIOE, mask); break;
    default:          HAL_GPIO_TogglePin(GPIOE, (uint16_t)mask); break;
    }
}

/* Bus as outputs through the tier, all low first. */
static bool bus_to_outputs(GstTier_t t)
{
    if (!bench_claim(GPIOE, BUS_MASK))
    {
        bench_abort("claim refused");
        return false;
    }
    if (t == GST_TIER_BM)
    {
        for (uint32_t p = BUS_SHIFT; p < 16U; p++)
        {
            if (GPIO_BM_ConfigOutput(GPIOE, p, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 0U) != GPIO_BM_OK)
            {
                bench_abort("config");
                return false;
            }
        }
        return true;
    }
    if (t == GST_TIER_LL)
    {
        GpioLL_PinConfig_t cfg[8];
        uint32_t done = 0U;
        for (uint32_t i = 0U; i < 8U; i++)
        {
            const GpioLL_PinConfig_t c = { GPIOE, BUS_SHIFT + i, LL_GPIO_MODE_OUTPUT, LL_GPIO_OUTPUT_PUSHPULL,
                                           LL_GPIO_SPEED_FREQ_LOW, LL_GPIO_PULL_NO, 0U, 0U, false };
            cfg[i] = c;
        }
        if ((GPIO_LL_ConfigPins(cfg, 8U, &done) != GPIO_BM_OK) || (done != 8U))
        {
            bench_abort("config");
            return false;
        }
        return true;
    }
    /* HAL: HAL_GPIO_Init never writes ODR, so the level goes first, as the
       header of gpio_hal.h says. */
    HAL_GPIO_WritePin(GPIOE, (uint16_t)BUS_MASK, GPIO_PIN_RESET);
    const GPIO_InitTypeDef init = { .Pin = BUS_MASK, .Mode = GPIO_MODE_OUTPUT_PP, .Pull = GPIO_NOPULL,
                                    .Speed = GPIO_SPEED_FREQ_LOW, .Alternate = 0U };
    if (GPIO_HAL_Init(GPIOE, &init) != HAL_OK)
    {
        bench_abort("config");
        return false;
    }
    return true;
}

static void step_t3(GstTier_t t)
{
    static const char *const names[GST_TIER_COUNT] = { "T3_BM", "T3_LL", "T3_HAL" };
    if (!step_begin(names[t]) || !buschk())
    {
        return;
    }
    if (!sync0())   /* host: OE of the bus is 0 (read back) */
    {
        return;
    }
    delay_ms(1U);   /* S6: dead time before this side drives */
    if (!bus_to_outputs(t))
    {
        return;
    }

    bool ok = true;
#define SHOW()   do { ok = ok && sync0(); } while (0)
    op_writeport(t, BUS_MASK, 0x5500U);  SHOW();
    op_writeport(t, BUS_MASK, 0xAA00U);  SHOW();
    op_reset(t, BUS_MASK);               SHOW();
    for (uint32_t k = 0U; k < 8U; k++)
    {
        op_write(t, BUS_SHIFT + k, 1U);
        if (k > 0U) { op_write(t, BUS_SHIFT + k - 1U, 0U); }
        SHOW();
    }
    op_set(t, BUS_MASK);                 SHOW();
    for (uint32_t k = 0U; k < 8U; k++)
    {
        op_write(t, BUS_SHIFT + k, 0U);
        if (k > 0U) { op_write(t, BUS_SHIFT + k - 1U, 1U); }
        SHOW();
    }
    op_toggle(t, BUS_MASK);              SHOW();
    op_toggle(t, 0x0F00U);               SHOW();
    op_set(t, 0x3C00U);                  SHOW();
    op_reset(t, 0x8100U);                SHOW();
    if (t == GST_TIER_BM)
    {
        GPIOE->BSRR = (1UL << BUS_SHIFT) | (1UL << (BUS_SHIFT + 16U));   /* set and reset of PE8 at once */
        SHOW();
        op_writeport(t, BUS_MASK, 0x5400U);   /* control: the host expects 0x55 here */
        SHOW();
    }
#undef SHOW

    pins_input_raw(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
    ok = ok && bus_moder_is(0U);
    (void)sync0();   /* bus released again */
    verdict(ok && !s_bench_abort, "%s pin operations shown to the AD2 (the host compares them)", names[t]);
    step_end();
}

/* ---- T4 + T7: input reads of each tier while the AD2 drives -------------------------------- */

typedef struct
{
    GstTier_t t;
    uint32_t  crc_port;
    uint32_t  crc_bits;
    uint32_t  count;
    uint32_t  last;
} T4Ctx_t;

static void t4_sample(void *p)
{
    T4Ctx_t *c = (T4Ctx_t *)p;
    uint32_t port_v = 0U;
    uint32_t bits   = 0U;
    for (uint32_t i = 0U; i < 8U; i++)
    {
        bool b;
        switch (c->t)
        {
        case GST_TIER_BM: b = GPIO_BM_Read(GPIOE, BUS_SHIFT + i); break;
        case GST_TIER_LL: b = GPIO_LL_Read(GPIOE, BUS_SHIFT + i); break;
        default:          b = HAL_GPIO_ReadPin(GPIOE, (uint16_t)(1UL << (BUS_SHIFT + i))) == GPIO_PIN_SET; break;
        }
        bits |= (b ? 1UL : 0UL) << i;
    }
    switch (c->t)
    {
    case GST_TIER_BM: port_v = (GPIO_BM_ReadPort(GPIOE) >> BUS_SHIFT) & 0xFFU; break;
    case GST_TIER_LL: port_v = (GPIO_LL_ReadPort(GPIOE) >> BUS_SHIFT) & 0xFFU; break;
    default:          port_v = bits; break;   /* the HAL has no port read */
    }
    c->crc_port = crc32_update(c->crc_port, (uint8_t)port_v);
    c->crc_bits = crc32_update(c->crc_bits, (uint8_t)bits);
    c->last = port_v;
    c->count++;
}

static void idr_sample(void *p)
{
    *(uint32_t *)p = bus_idr();
}

static bool bus_to_inputs(GstTier_t t)
{
    if (t == GST_TIER_BM)
    {
        for (uint32_t p = BUS_SHIFT; p < 16U; p++)
        {
            if (GPIO_BM_ConfigInput(GPIOE, p, GPIO_BM_PULL_NONE) != GPIO_BM_OK)
            {
                return false;
            }
        }
    }
    else if (t == GST_TIER_LL)
    {
        GpioLL_PinConfig_t cfg[8];
        for (uint32_t i = 0U; i < 8U; i++)
        {
            const GpioLL_PinConfig_t c = { GPIOE, BUS_SHIFT + i, LL_GPIO_MODE_INPUT, 0U, 0U, LL_GPIO_PULL_NO, 0U, 0U, false };
            cfg[i] = c;
        }
        if (GPIO_LL_ConfigPins(cfg, 8U, NULL) != GPIO_BM_OK)
        {
            return false;
        }
    }
    else
    {
        const GPIO_InitTypeDef init = { .Pin = BUS_MASK, .Mode = GPIO_MODE_INPUT, .Pull = GPIO_NOPULL,
                                        .Speed = GPIO_SPEED_FREQ_LOW, .Alternate = 0U };
        if (GPIO_HAL_Init(GPIOE, &init) != HAL_OK)
        {
            return false;
        }
    }
    return bus_moder_is(0U) && ((GPIOE->PUPDR >> 16U) == 0U);
}

static void step_t4(GstTier_t t)
{
    static const char *const names[GST_TIER_COUNT] = { "T4_BM", "T4_LL", "T4_HAL" };
    T4Ctx_t c = { t, 0xFFFFFFFFUL, 0xFFFFFFFFUL, 0U, 0U };
    T4Ctx_t inv = { t, 0xFFFFFFFFUL, 0xFFFFFFFFUL, 0U, 0U };
    uint32_t in_mode = 0U;
    uint32_t an_mode = 0U;

    if (!step_begin(names[t]) || !buschk())
    {
        return;
    }
    if (!bus_to_inputs(t))
    {
        bench_abort("bus not in input");
        return;
    }
    if (!sync0())      /* host: 1 ms, then OE = bus */
    {
        return;
    }
    for (uint32_t k = 0U; k < 256U; k++)
    {
        if (!sync_cb(t4_sample, &c, false))
        {
            return;
        }
    }
    if (t == GST_TIER_BM)
    {
        for (uint32_t k = 0U; k < 256U; k++)   /* control: the host drives the inverted patterns */
        {
            if (!sync_cb(t4_sample, &inv, false))
            {
                return;
            }
        }
    }

    /* T7: the AD2 drives 0xFF; in input IDR follows, in analog it reads 0. */
    if (!sync_cb(idr_sample, &in_mode, false))
    {
        return;
    }
    uint32_t deinit_err = 0U;
    for (uint32_t p = BUS_SHIFT; p < 16U; p++)
    {
        deinit_err += (gst_deinit(t, GPIOE, p) != (uint32_t)GPIO_BM_OK) ? 1U : 0U;
    }
    const bool analog = bus_moder_is(3U);
    if (!sync_cb(idr_sample, &an_mode, false))
    {
        return;
    }
    if (!sync0())      /* host: OE = 0 with read-back */
    {
        return;
    }
    pins_input_raw(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);

    LOG("%s count=%lu crc_port=0x%08lX crc_bits=0x%08lX", names[t], (unsigned long)c.count,
        (unsigned long)~c.crc_port, (unsigned long)~c.crc_bits);
    if (t == GST_TIER_BM)
    {
        LOG("%s inverted count=%lu crc_port=0x%08lX crc_bits=0x%08lX", names[t], (unsigned long)inv.count,
            (unsigned long)~inv.crc_port, (unsigned long)~inv.crc_bits);
    }
    verdict((c.count == 256U) && (c.crc_port == c.crc_bits),
            "%s 256 patterns read, port read and bit reads agree (the host checks the CRC)", names[t]);
    verdict((in_mode == 0xFFU) && (an_mode == 0x00U) && analog && (deinit_err == 0U),
            "T7_%s AD2 drives 0xFF: IDR input=0x%02lX (control), after DeinitPin (analog)=0x%02lX", k_tier_name[t],
            (unsigned long)in_mode, (unsigned long)an_mode);
    step_end();
}

/* ---- T5 + T6: pulls and open-drain, measured by the scope ------------------------------------ */

static bool scope_pins(GstTier_t t, uint32_t m14, uint32_t o14, uint32_t p14, uint32_t l14,
                       uint32_t m15, uint32_t o15, uint32_t p15, uint32_t l15)
{
    return bench_cfg(t, GPIOF, PF_SCOPE1, m14, o14, GPIO_BM_SPEED_LOW, p14, 0U, l14) &&
           bench_cfg(t, GPIOF, PF_SCOPE2, m15, o15, GPIO_BM_SPEED_LOW, p15, 0U, l15) && sync0();
}

static void step_t5t6(void)
{
    const uint32_t M_IN  = GPIO_BM_MODE_INPUT;
    const uint32_t M_OUT = GPIO_BM_MODE_OUTPUT;
    const uint32_t T_PP  = GPIO_BM_OTYPE_PP;
    const uint32_t T_OD  = GPIO_BM_OTYPE_OD;
    if (!step_begin("T5T6"))
    {
        return;
    }
    bool ok = scope_pins(GST_TIER_BM, M_IN, T_PP, GPIO_BM_PULL_NONE, 0U, M_IN, T_PP, GPIO_BM_PULL_NONE, 0U) &&   /* V_NP */
              scope_pins(GST_TIER_BM, M_OUT, T_PP, GPIO_BM_PULL_NONE, 1U, M_OUT, T_PP, GPIO_BM_PULL_NONE, 1U);    /* V_DD */
    for (uint32_t t = 0U; ok && (t < (uint32_t)GST_TIER_COUNT); t++)
    {
        ok = scope_pins((GstTier_t)t, M_IN, T_PP, GPIO_BM_PULL_UP, 0U, M_IN, T_PP, GPIO_BM_PULL_DOWN, 0U) &&  /* R_PU, R_PD */
             scope_pins((GstTier_t)t, M_OUT, T_OD, GPIO_BM_PULL_NONE, 1U, M_OUT, T_OD, GPIO_BM_PULL_NONE, 0U) && /* OD rel / low */
             scope_pins((GstTier_t)t, M_OUT, T_OD, GPIO_BM_PULL_NONE, 1U, M_OUT, T_OD, GPIO_BM_PULL_NONE, 1U);   /* OD rel / rel */
    }
    pins_input_raw(GPIOF, (1UL << PF_SCOPE1) | (1UL << PF_SCOPE2), GPIO_BM_PULL_NONE);
    verdict(ok && !s_bench_abort, "T5T6 pull and open-drain states shown to the scope (the host measures them)");
    step_end();
}

/* ---- T8: no glitch when a pin becomes an output --------------------------------------------- */

typedef struct { GstTier_t t; uint32_t level; bool naive; } T8Ctx_t;

static void t8_act(void *p)
{
    const T8Ctx_t *c = (const T8Ctx_t *)p;
    if (c->naive)
    {
        /* The wrong order: output first (drives the stale ODR 0), then the level. */
        GPIOE->MODER = (GPIOE->MODER & ~(3UL << (PE_T8 * 2U))) | (1UL << (PE_T8 * 2U));
        delay_cycles(CYC_PER_US);
        GPIOE->BSRR = 1UL << PE_T8;
        return;
    }
    const GstPinReq_t r = { GPIOE, PE_T8, GPIO_BM_MODE_OUTPUT, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW,
                            GPIO_BM_PULL_NONE, 0U, c->level, false };
    (void)gst_apply(c->t, &r);
}

/* Starting state: input with `pull`, ODR = `odr`. */
static void t8_case(GstTier_t t, uint32_t pull, uint32_t odr, uint32_t level, bool naive)
{
    pins_input_raw(GPIOE, 1UL << PE_T8, pull);
    GPIOE->BSRR = (odr != 0U) ? (1UL << PE_T8) : (1UL << (PE_T8 + 16U));
    delay_ms(1U);
    if (!bench_claim(GPIOE, 1UL << PE_T8))
    {
        bench_abort("claim refused");
        return;
    }
    T8Ctx_t c = { t, level, naive };
    (void)sync_cb(t8_act, &c, true);
    pins_input_raw(GPIOE, 1UL << PE_T8, GPIO_BM_PULL_NONE);
}

static void step_t8(GstTier_t t)
{
    static const char *const names[GST_TIER_COUNT] = { "T8_BM", "T8_LL", "T8_HAL" };
    if (!step_begin(names[t]))
    {
        return;
    }
    t8_case(t, GPIO_BM_PULL_DOWN, 0U, 1U, false);   /* A: one rise              */
    t8_case(t, GPIO_BM_PULL_UP,   1U, 0U, false);   /* B: one fall              */
    t8_case(t, GPIO_BM_PULL_UP,   0U, 1U, false);   /* C: stale ODR 0, stays high */
    t8_case(t, GPIO_BM_PULL_DOWN, 1U, 0U, false);   /* D: stale ODR 1, stays low  */
    verdict(!s_bench_abort, "%s four transitions shown to the logic analyzer (the host counts them)", names[t]);
    step_end();
}

static void step_t8_ctrl(void)
{
    if (!step_begin("T8_CTRL"))
    {
        return;
    }
    t8_case(GST_TIER_BM, GPIO_BM_PULL_UP, 0U, 1U, true);   /* E: C in the naive order: a pulse */
    step_end();
}

/* ---- T9: rise time per OSPEEDR ---------------------------------------------------------------- */

static void t9_high(void *p) { (void)p; GPIOF->BSRR = (1UL << PF_SCOPE1) | (1UL << PF_SCOPE2); }
static void t9_low(void *p)  { (void)p; GPIOF->BSRR = ((1UL << PF_SCOPE1) | (1UL << PF_SCOPE2)) << 16U; }

static void step_t9(void)
{
    /* Errata E9: 40 edges per speed and pin, fixed beforehand, interleaved in
       four rounds of 10 (LOW, MEDIUM, HIGH, VERY_HIGH each round). Each round
       configures through a different tier, so a tier writing the wrong
       OSPEEDR would show as spread; T2 already shows the three write the same bits. */
    static const GstTier_t round_tier[4] = { GST_TIER_BM, GST_TIER_LL, GST_TIER_HAL, GST_TIER_BM };
    if (!step_begin("T9"))
    {
        return;
    }
    for (uint32_t r = 0U; r < 4U; r++)
    {
        for (uint32_t s = 0U; s < 4U; s++)
        {
            if (!bench_cfg(round_tier[r], GPIOF, PF_SCOPE1, GPIO_BM_MODE_OUTPUT, GPIO_BM_OTYPE_PP, s, GPIO_BM_PULL_NONE, 0U, 0U) ||
                !bench_cfg(round_tier[r], GPIOF, PF_SCOPE2, GPIO_BM_MODE_OUTPUT, GPIO_BM_OTYPE_PP, s, GPIO_BM_PULL_NONE, 0U, 0U))
            {
                return;
            }
            delay_ms(1U);
            for (uint32_t e = 0U; e < 10U; e++)
            {
                if (!sync_cb(t9_high, NULL, true) || !sync_cb(t9_low, NULL, true))
                {
                    return;
                }
            }
        }
    }
    /* Control: pins in input, the same ODR write, no edge. */
    pins_input_raw(GPIOF, (1UL << PF_SCOPE1) | (1UL << PF_SCOPE2), GPIO_BM_PULL_NONE);
    delay_ms(1U);
    (void)sync_cb(t9_high, NULL, true);
    t9_low(NULL);
    verdict(!s_bench_abort, "T9 160 rising edges per pin produced (the host measures them)");
    step_end();
}

/* ---- T10: MCO1 = HSE/2 on PA8 in AF0 ----------------------------------------------------------- */

static void mco_hse_div2(void)
{
    if (!s_mco_changed)
    {
        s_mco_saved   = RCC->CFGR & (RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE);
        s_mco_changed = true;
    }
    /* MCO1 = HSE (010), MCO1PRE = /2 (0010). RCC_CFGR is CPU1's register. */
    MODIFY_REG(RCC->CFGR, RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE, RCC_CFGR_MCO1_1 | RCC_CFGR_MCO1PRE_1);
}

static void step_t10(GstTier_t t)
{
    static const char *const names[GST_TIER_COUNT] = { "T10_BM", "T10_LL", "T10_HAL" };
    if (!step_begin(names[t]))
    {
        return;
    }
    mco_hse_div2();
    if (!bench_cfg(t, GPIOA, PA_MCO, GPIO_BM_MODE_AF, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_HIGH, GPIO_BM_PULL_NONE, 0U, 0U))
    {
        return;
    }
    (void)sync0();   /* host: frequency on DIO14 */
    if (t == GST_TIER_BM)
    {
        /* Control: AF1 (TIM1_CH1, not running) on the same pin: no signal. */
        if (!bench_cfg(t, GPIOA, PA_MCO, GPIO_BM_MODE_AF, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_HIGH, GPIO_BM_PULL_NONE, 1U, 0U))
        {
            return;
        }
        (void)sync0();
    }
    pins_input_raw(GPIOA, 1UL << PA_MCO, GPIO_BM_PULL_NONE);
    mco_restore();
    verdict(!s_bench_abort && ((RCC->CFGR & (RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE)) == s_mco_saved),
            "%s MCO1 shown on PA8, RCC_CFGR MCO fields restored", names[t]);
    step_end();
}

/* ==========================================================================================
 * F2 Steps: EXTI 0-15 and Compensation Cell (T11 - T14, T17)
 * ========================================================================================== */

/* Pull-downs on PE3 and PE4 during F2 steps so lines driven by AD2 do not float (m6, N6) */
static void f2_pins_pd(bool enable)
{
    pins_input_raw(GPIOE, (1UL << PE_EXTI3) | (1UL << PE_EXTI4),
                   enable ? GPIO_BM_PULL_DOWN : GPIO_BM_PULL_NONE);
}

static void step_t11_cm7(const char *name, uint32_t trigger, bool interrupt, bool event, uint32_t expected)
{
    if (!step_begin(name))
    {
        return;
    }
    f2_pins_pd(true);
    if (!bench_cfg(GST_TIER_BM, GPIOE, PE_T8, GPIO_BM_MODE_OUTPUT, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 0U, 0U))
    {
        f2_pins_pd(false);
        return;
    }
    s_cm7_exti3_count = 0U;
    (void)GPIO_BM_ExtiConfig(GPIOE, 3U, trigger, interrupt, event);
    /* M4: Always keep NVIC enabled for EXTI3 so the test isolates that EXTI IMR (not NVIC)
       prevents interrupt generation in EVENT mode */
    NVIC_SetPriority(EXTI3_IRQn, 2U);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    NVIC_EnableIRQ(EXTI3_IRQn);

    (void)sync0();

    NVIC_DisableIRQ(EXTI3_IRQn);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    (void)GPIO_BM_ExtiDisable(GPIOE, 3U);
    pins_input_raw(GPIOE, 1UL << PE_T8, GPIO_BM_PULL_NONE);
    f2_pins_pd(false);

    LOG("%s: CM7 count=%lu (expected %lu)", name, (unsigned long)s_cm7_exti3_count, (unsigned long)expected);
    verdict(!s_bench_abort && (s_cm7_exti3_count == expected), "%s count exact (%lu)", name, (unsigned long)expected);
    step_end();
}

static void step_t11_cm4(const char *name, uint32_t trigger, bool interrupt, bool event, uint32_t expected)
{
    if (!step_begin(name))
    {
        return;
    }
    f2_pins_pd(true);
    /* M3: CM7 verifies claim and !abort before authorizing CM4 to drive PE6 */
    if (s_bench_abort || !bench_claim(GPIOE, 1U << PE_SPARE6))
    {
        bench_abort("cannot claim PE6 for CM4");
        f2_pins_pd(false);
        return;
    }
    (void)cm4_send(GST_CMD_F2_COUNT_RESET, 0U, 0U, true, NULL);
    const uint32_t a0 = 4U | (trigger << 8U);
    const uint32_t a1 = (interrupt ? 2U : 0U) | (event ? 1U : 0U) | GST_F2_AUTH_PE6;
    (void)cm4_send(GST_CMD_F2_CONFIG, a0, a1, true, NULL);

    (void)sync0();

    (void)cm4_send(GST_CMD_F2_DISABLE, 4U, 1U, true, NULL);
    f2_pins_pd(false);

    LOG("%s: CM4 count=%lu (expected %lu)", name, (unsigned long)GST_AREA->m4_exti_count, (unsigned long)expected);
    verdict(!s_bench_abort && (GST_AREA->m4_exti_count == expected), "%s count exact (%lu)", name, (unsigned long)expected);
    step_end();
}

static void step_t12(void)
{
    if (!step_begin("T12"))
    {
        return;
    }
    bool ok = false;
    f2_pins_pd(true);
    /* M3: CM7 verifies claim and !abort before authorizing CM4 to drive PE6 */
    if (s_bench_abort || !bench_claim(GPIOE, 1U << PE_SPARE6))
    {
        bench_abort("cannot claim PE6 for CM4");
        f2_pins_pd(false);
        return;
    }
    if (!bench_cfg(GST_TIER_BM, GPIOE, PE_T8, GPIO_BM_MODE_OUTPUT, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 0U, 0U))
    {
        f2_pins_pd(false);
        return;
    }

    /* Subtest 1: Line 3 in CM7, Line 4 in CM4 */
    s_cm7_exti3_count = 0U;
    (void)cm4_send(GST_CMD_F2_COUNT_RESET, 0U, 0U, true, NULL);

    (void)GPIO_BM_ExtiConfig(GPIOE, 3U, GPIO_BM_EXTI_RISING, true, false);
    NVIC_SetPriority(EXTI3_IRQn, 2U);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    NVIC_EnableIRQ(EXTI3_IRQn);

    if (s_bench_abort)
    {
        goto cleanup;
    }
    (void)cm4_send(GST_CMD_F2_CONFIG, 4U | (GPIO_BM_EXTI_RISING << 8U), 2U | GST_F2_AUTH_PE6, true, NULL);

    (void)sync0();

    const uint32_t cm7_c1 = s_cm7_exti3_count;
    const uint32_t cm4_c1 = GST_AREA->m4_exti_count;
    LOG("T12 independent: CM7=%lu (exp 200), CM4=%lu (exp 200)", (unsigned long)cm7_c1, (unsigned long)cm4_c1);
    ok = (cm7_c1 == 200U) && (cm4_c1 == 200U);

    /* Subtest 2: Line 3 shared in both cores */
    s_cm7_exti3_count = 0U;
    (void)cm4_send(GST_CMD_F2_COUNT_RESET, 0U, 0U, true, NULL);
    (void)cm4_send(GST_CMD_F2_DISABLE, 4U, 0U, true, NULL);
    if (s_bench_abort)
    {
        goto cleanup;
    }
    (void)cm4_send(GST_CMD_F2_CONFIG, 3U | (GPIO_BM_EXTI_RISING << 8U), 2U | GST_F2_AUTH_PE6, true, NULL);

    (void)sync0();

    const uint32_t cm7_c2 = s_cm7_exti3_count;
    const uint32_t cm4_c2 = GST_AREA->m4_exti_count;
    LOG("T12 shared line 3: CM7=%lu (exp 200), CM4=%lu (exp 200)", (unsigned long)cm7_c2, (unsigned long)cm4_c2);
    ok = ok && (cm7_c2 == 200U) && (cm4_c2 == 200U);

    /* Subtest 3 (Control): ExtiDisable of line 3 in CM4 -> CM4 counts 0, CM7 counts 200 */
    s_cm7_exti3_count = 0U;
    (void)cm4_send(GST_CMD_F2_COUNT_RESET, 0U, 0U, true, NULL);
    (void)cm4_send(GST_CMD_F2_DISABLE, 3U, 0U, true, NULL);

    (void)sync0();

    const uint32_t cm7_c3 = s_cm7_exti3_count;
    const uint32_t cm4_c3 = GST_AREA->m4_exti_count;
    LOG("T12 control disabled CM4: CM7=%lu (exp 200), CM4=%lu (exp 0)", (unsigned long)cm7_c3, (unsigned long)cm4_c3);
    ok = ok && (cm7_c3 == 200U) && (cm4_c3 == 0U);

cleanup:
    /* Cleanup */
    NVIC_DisableIRQ(EXTI3_IRQn);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    (void)GPIO_BM_ExtiDisable(GPIOE, 3U);
    (void)cm4_send(GST_CMD_F2_DISABLE, 3U, 1U, true, NULL);
    pins_input_raw(GPIOE, 1UL << PE_T8, GPIO_BM_PULL_NONE);
    f2_pins_pd(false);

    verdict(!s_bench_abort && ok, "T12 dual-core EXTI routing and isolation");
    step_end();
}

static void step_t13_cm7(void)
{
    if (!step_begin("T13_CM7"))
    {
        return;
    }
    f2_pins_pd(true);
    if (!bench_cfg(GST_TIER_BM, GPIOE, PE_T8, GPIO_BM_MODE_OUTPUT, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 0U, 0U))
    {
        f2_pins_pd(false);
        return;
    }
    s_cm7_exti3_count = 0U;
    (void)GPIO_BM_ExtiConfig(GPIOE, 3U, GPIO_BM_EXTI_RISING, true, false);
    NVIC_SetPriority(EXTI3_IRQn, 2U);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    NVIC_EnableIRQ(EXTI3_IRQn);

    (void)sync0();

    /* M4 Negative control: keep NVIC enabled and PE5 in output; disable only the EXTI line */
    (void)GPIO_BM_ExtiDisable(GPIOE, 3U);
    (void)sync0();

    NVIC_DisableIRQ(EXTI3_IRQn);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    pins_input_raw(GPIOE, 1UL << PE_T8, GPIO_BM_PULL_NONE);
    f2_pins_pd(false);
    verdict(!s_bench_abort, "T13_CM7 latency edges measured by host");
    step_end();
}

static void step_t13_cm4(void)
{
    if (!step_begin("T13_CM4"))
    {
        return;
    }
    f2_pins_pd(true);
    /* M3: CM7 verifies claim and !abort before authorizing CM4 to drive PE6 */
    if (s_bench_abort || !bench_claim(GPIOE, 1U << PE_SPARE6))
    {
        bench_abort("cannot claim PE6 for CM4");
        f2_pins_pd(false);
        return;
    }
    (void)cm4_send(GST_CMD_F2_COUNT_RESET, 0U, 0U, true, NULL);
    (void)cm4_send(GST_CMD_F2_CONFIG, 4U | (GPIO_BM_EXTI_RISING << 8U), 2U | GST_F2_AUTH_PE6, true, NULL);

    (void)sync0();

    /* M4 Negative control: keep NVIC enabled and PE6 in output (cleanup=0); disable only EXTI line */
    (void)cm4_send(GST_CMD_F2_DISABLE, 4U, 0U, true, NULL);
    (void)sync0();

    /* Final cleanup for CM4 */
    (void)cm4_send(GST_CMD_F2_DISABLE, 4U, 1U, true, NULL);
    f2_pins_pd(false);

    verdict(!s_bench_abort, "T13_CM4 latency edges measured by host");
    step_end();
}

static void step_t14(void)
{
    if (!step_begin("T14"))
    {
        return;
    }
    f2_pins_pd(true);
    bool all_ok = true;

    /* (a) Pending: line 3 unmasked with NVIC off. Host edge leaves PR1 at 1.
           ExtiConfig again leaves it at 0. Clear NVIC pending. Turn on NVIC -> 0 ISR.
           Negative control: without ExtiConfig, turn on NVIC -> 1 ISR. */
    s_cm7_exti3_count = 0U;
    (void)GPIO_BM_ExtiConfig(GPIOE, 3U, GPIO_BM_EXTI_RISING, true, false);
    NVIC_DisableIRQ(EXTI3_IRQn);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);

    (void)sync0();

    /* Read PR1 directly before ExtiConfig (M5): host edge set it */
    const uint32_t pr1_before = EXTI->PR1 & (1UL << 3U);
    (void)GPIO_BM_ExtiConfig(GPIOE, 3U, GPIO_BM_EXTI_RISING, true, false);
    /* Read PR1 directly after ExtiConfig (M5): ExtiConfig cleared it */
    const uint32_t pr1_after = EXTI->PR1 & (1UL << 3U);

    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    NVIC_EnableIRQ(EXTI3_IRQn);
    delay_ms(2U);
    NVIC_DisableIRQ(EXTI3_IRQn);

    const bool p1_ok = (pr1_before == (1UL << 3U)) && (pr1_after == 0UL) && (s_cm7_exti3_count == 0U);
    LOG("T14(a) Pending cleared by ExtiConfig: pr1_before=0x%08lX pr1_after=0x%08lX count=%lu (exp 0)",
        (unsigned long)pr1_before, (unsigned long)pr1_after, (unsigned long)s_cm7_exti3_count);
    all_ok = all_ok && p1_ok;

    s_cm7_exti3_count = 0U;
    (void)sync0();
    const uint32_t pr1_ctrl = EXTI->PR1 & (1UL << 3U);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    NVIC_EnableIRQ(EXTI3_IRQn);
    delay_ms(2U);
    NVIC_DisableIRQ(EXTI3_IRQn);
    const bool p1_ctrl_ok = (pr1_ctrl == (1UL << 3U)) && (s_cm7_exti3_count == 1U);
    LOG("T14(a) Control uncleared PR1: pr1=0x%08lX count=%lu (exp 1)",
        (unsigned long)pr1_ctrl, (unsigned long)s_cm7_exti3_count);
    all_ok = all_ok && p1_ctrl_ok;
    (void)GPIO_BM_ExtiDisable(GPIOE, 3U);

    /* (b) Conflict: with line 3 in PE and unmasked, ExtiConfig(GPIOD, 3) returns ERR_CONFLICT, photo unchanged */
    (void)GPIO_BM_ExtiConfig(GPIOE, 3U, GPIO_BM_EXTI_RISING, true, false);
    GstPhoto_t b_conf, a_conf;
    photo_take(&b_conf);
    const GpioBm_Status_t st_conf = GPIO_BM_ExtiConfig(GPIOD, 3U, GPIO_BM_EXTI_RISING, true, false);
    photo_take(&a_conf);
    const bool conf_ok = (st_conf == GPIO_BM_ERR_CONFLICT) && photo_equal(&b_conf, &a_conf);
    LOG("T14(b) Conflict GPIOD: status=%lu photo_equal=%u", (unsigned long)st_conf, photo_equal(&b_conf, &a_conf));
    all_ok = all_ok && conf_ok;

    (void)GPIO_BM_ExtiDisable(GPIOE, 3U);
    const GpioBm_Status_t st_free = GPIO_BM_ExtiConfig(GPIOD, 3U, GPIO_BM_EXTI_RISING, true, false);
    const bool free_ok = (st_free == GPIO_BM_OK);
    LOG("T14(b) Control free line routes to GPIOD: status=%lu", (unsigned long)st_free);
    all_ok = all_ok && free_ok;
    (void)GPIO_BM_ExtiDisable(GPIOD, 3U);

    /* M5: Restore routing back to PE after testing GPIOD */
    const GpioBm_Status_t st_pe_restore = GPIO_BM_ExtiConfig(GPIOE, 3U, GPIO_BM_EXTI_RISING, true, false);
    (void)GPIO_BM_ExtiDisable(GPIOE, 3U);
    const bool pe_restored = (st_pe_restore == GPIO_BM_OK);
    LOG("T14(b) Restore routing back to PE: status=%lu", (unsigned long)st_pe_restore);
    all_ok = all_ok && pe_restored;

    /* (c) Deinit rule 6: ExtiDisable on one core leaves RTSR/FTSR/EXTICR while other has mask, and clears when last.
           Uses trigger GPIO_BM_EXTI_BOTH (M5) */
    if (s_bench_abort || !bench_claim(GPIOE, 1U << PE_SPARE6))
    {
        bench_abort("cannot claim PE6 for CM4");
        f2_pins_pd(false);
        return;
    }
    (void)GPIO_BM_ExtiConfig(GPIOE, 3U, GPIO_BM_EXTI_BOTH, true, false);
    (void)cm4_send(GST_CMD_F2_CONFIG, 3U | (GPIO_BM_EXTI_BOTH << 8U), 2U | GST_F2_AUTH_PE6, true, NULL);
    (void)cm4_send(GST_CMD_F2_DISABLE, 3U, 1U, true, NULL);

    const uint32_t cr_kept = (SYSCFG->EXTICR[0] >> 12U) & 0xFU;
    const uint32_t rtsr_kept = (EXTI->RTSR1 >> 3U) & 1UL;
    const uint32_t ftsr_kept = (EXTI->FTSR1 >> 3U) & 1UL;
    const uint32_t c2imr_kept = (EXTI->C2IMR1 >> 3U) & 1UL;
    const uint32_t c2emr_kept = (EXTI->C2EMR1 >> 3U) & 1UL;
    const uint32_t c1imr_kept = (EXTI->IMR1 >> 3U) & 1UL;
    const bool deinit_kept = (cr_kept == 4U) && (rtsr_kept == 1UL) && (ftsr_kept == 1UL) &&
                             (c2imr_kept == 0UL) && (c2emr_kept == 0UL) && (c1imr_kept == 1UL);

    (void)GPIO_BM_ExtiDisable(GPIOE, 3U);
    const uint32_t cr_cleared = (SYSCFG->EXTICR[0] >> 12U) & 0xFU;
    const uint32_t rtsr_cleared = (EXTI->RTSR1 >> 3U) & 1UL;
    const uint32_t ftsr_cleared = (EXTI->FTSR1 >> 3U) & 1UL;
    const uint32_t c1imr_cleared = (EXTI->IMR1 >> 3U) & 1UL;
    const bool deinit_cleared = (cr_cleared == 0U) && (rtsr_cleared == 0UL) && (ftsr_cleared == 0UL) && (c1imr_cleared == 0UL);
    LOG("T14(c) Deinit rule 6 BOTH: shared kept=%u (cr=%lu rtsr=%lu ftsr=%lu), last cleared=%u (cr=%lu rtsr=%lu ftsr=%lu)",
        deinit_kept, (unsigned long)cr_kept, (unsigned long)rtsr_kept, (unsigned long)ftsr_kept,
        deinit_cleared, (unsigned long)cr_cleared, (unsigned long)rtsr_cleared, (unsigned long)ftsr_cleared);
    all_ok = all_ok && deinit_kept && deinit_cleared;

    /* (d) ExtiSoftwareTrigger(0) with line 0 in PE: 100 triggers -> 100 ISRs */
    s_cm7_exti0_count = 0U;
    s_cm7_exti0_pend_ok = 0U;
    (void)GPIO_BM_ExtiConfig(GPIOE, 0U, GPIO_BM_EXTI_RISING, true, false);
    NVIC_SetPriority(EXTI0_IRQn, 2U);
    NVIC_ClearPendingIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI0_IRQn);

    for (uint32_t i = 0U; i < 100U; i++)
    {
        (void)GPIO_BM_ExtiSoftwareTrigger(0U);
        delay_cycles(1000U);
    }
    NVIC_DisableIRQ(EXTI0_IRQn);
    const bool sw_ok = (s_cm7_exti0_count == 100U) && (s_cm7_exti0_pend_ok == 100U);
    LOG("T14(d) ExtiSoftwareTrigger(0): count=%lu (exp 100) pend_ok=%lu (exp 100)",
        (unsigned long)s_cm7_exti0_count, (unsigned long)s_cm7_exti0_pend_ok);
    all_ok = all_ok && sw_ok;

    (void)GPIO_BM_ExtiDisable(GPIOE, 0U);
    s_cm7_exti0_count = 0U;
    NVIC_EnableIRQ(EXTI0_IRQn);
    (void)GPIO_BM_ExtiSoftwareTrigger(0U);
    delay_cycles(1000U);
    NVIC_DisableIRQ(EXTI0_IRQn);
    const bool sw_ctrl_ok = (s_cm7_exti0_count == 0U);
    LOG("T14(d) Control masked line 0: count=%lu (exp 0)", (unsigned long)s_cm7_exti0_count);
    all_ok = all_ok && sw_ctrl_ok;

    f2_pins_pd(false);
    verdict(!s_bench_abort && all_ok, "T14 pending, conflict, deinit rule 6 and software trigger");
    step_end();
}

static void step_t17(void)
{
    if (!step_begin("T17"))
    {
        return;
    }
    bool ok = true;
    const uint32_t cr_saved = RCC->CR & RCC_CR_CSION;
    const uint32_t cccsr_saved = SYSCFG->CCCSR;

    /* (a) Without CSI: CSION = 0, waiting for CSIRDY = 0, and EN = 0 */
    RCC->CR &= ~RCC_CR_CSION;
    uint32_t t0 = DWT->CYCCNT;
    bool csi_off_timeout = false;
    while ((RCC->CR & RCC_CR_CSIRDY) != 0U)
    {
        if ((DWT->CYCCNT - t0) > (100U * CYC_PER_MS)) { csi_off_timeout = true; break; }
    }
    LOG("T17(a) CSIRDY off wait: timeout=%u CSIRDY=%lu",
        csi_off_timeout, (unsigned long)((RCC->CR & RCC_CR_CSIRDY) != 0U));
    const bool csi_is_off = !csi_off_timeout && ((RCC->CR & RCC_CR_CSIRDY) == 0U);

    SYSCFG->CCCSR &= ~SYSCFG_CCCSR_EN;

    GstPhoto_t b_cc, a_cc;
    photo_take(&b_cc);
    const GpioBm_Status_t st_nocsi = GPIO_BM_EnableCompensationCell(50000U);
    photo_take(&a_cc);
    const bool a_ok = csi_is_off && (st_nocsi == GPIO_BM_ERR_NOT_READY) && photo_equal(&b_cc, &a_cc);
    LOG("T17(a) Compensation cell without CSI: st=%lu photo_equal=%u", (unsigned long)st_nocsi, photo_equal(&b_cc, &a_cc));
    ok = ok && a_ok;

    /* Control for (a): manual EN=1 without CSI leaves READY=0 (RM0399 13.3.7) */
    SYSCFG->CCCSR |= SYSCFG_CCCSR_EN;
    delay_cycles(2000U);
    const bool ready_zero = (SYSCFG->CCCSR & SYSCFG_CCCSR_READY) == 0U;
    SYSCFG->CCCSR &= ~SYSCFG_CCCSR_EN;
    LOG("T17(a) Control manual EN=1 without CSI: ready_zero=%u", ready_zero);
    ok = ok && ready_zero;

    /* (b) With CSI: CSION = 1, wait CSIRDY = 1 */
    RCC->CR |= RCC_CR_CSION;
    t0 = DWT->CYCCNT;
    bool csi_on_timeout = false;
    while ((RCC->CR & RCC_CR_CSIRDY) == 0U)
    {
        if ((DWT->CYCCNT - t0) > (100U * CYC_PER_MS)) { csi_on_timeout = true; break; }
    }
    LOG("T17(b) CSIRDY on wait: timeout=%u CSIRDY=%lu",
        csi_on_timeout, (unsigned long)((RCC->CR & RCC_CR_CSIRDY) != 0U));
    const bool csi_is_on = !csi_on_timeout && ((RCC->CR & RCC_CR_CSIRDY) != 0U);

    gst_shim_install(true);
    gst_shim_hold_reset();
    const uint32_t t_start = DWT->CYCCNT;
    const GpioBm_Status_t st_csi = GPIO_BM_EnableCompensationCell(4000000UL);
    const uint32_t cyc_taken = DWT->CYCCNT - t_start;
    const uint32_t hold_cycles = gst_shim_hold_max();
    gst_shim_install(false);

    const bool sec_ok = (hold_cycles >= 1U) && (hold_cycles < (60U * CYC_PER_US));
    const bool b_ok = csi_is_on && (st_csi == GPIO_BM_OK) &&
                      ((SYSCFG->CCCSR & SYSCFG_CCCSR_READY) != 0U) && sec_ok;
    LOG("T17(b) Compensation cell with CSI: st=%lu cycles=%lu sec_hold=%lu CCCSR=0x%08lX (sec_ok=%u)",
        (unsigned long)st_csi, (unsigned long)cyc_taken, (unsigned long)hold_cycles,
        (unsigned long)SYSCFG->CCCSR, sec_ok);
    ok = ok && b_ok;

    /* Restore saved state */
    SYSCFG->CCCSR = cccsr_saved;
    if (cr_saved == 0U)
    {
        RCC->CR &= ~RCC_CR_CSION;
    }
    LOG("T17 restored: RCC_CR.CSION=%lu, CCCSR=0x%08lX",
        (unsigned long)((RCC->CR & RCC_CR_CSION) != 0U), (unsigned long)SYSCFG->CCCSR);

    verdict(!s_bench_abort && ok, "T17 compensation cell without CSI (refused) and with CSI (ready)");
    step_end();
}

/* ==========================================================================================
 * T16: LCKR, last (a lock lasts until reset)
 * ========================================================================================== */

/* Takes GPIOK's clock away from CPU1's bank (RM0399 9.7.1: RCC->AHB4ENR is
   the calling core's copy). True when neither bank clocks the port any more;
   CPU2 never clocks GPIOK in this selftest, and this checks it. */
static bool gpiok_unclock(void)
{
    RCC->AHB4ENR &= ~RCC_AHB4ENR_GPIOKEN;
    volatile uint32_t rb = RCC->AHB4ENR;
    (void)rb;
    return ((RCC_C1->AHB4ENR | RCC_C2->AHB4ENR) & RCC_AHB4ENR_GPIOKEN) == 0UL;
}

/* Test-side oracle for GPIO_BM_IsPortClocked(GPIOK): both RCC banks, raw. */
static bool gpiok_clocked_raw(void)
{
    return ((RCC_C1->AHB4ENR | RCC_C2->AHB4ENR) & RCC_AHB4ENR_GPIOKEN) != 0UL;
}

/* A1 (audit of 2026-09-30): GPIO_HAL_Init on a port unclocked in both banks.
   Unclocked, the port's registers do not read their real value: on Rev V they
   read all ones (LCKR = 0xFFFFFFFF, so every pin looks locked). Before the fix
   GPIO_HAL_Init checked LCKR on the unclocked port, which gives a wrong answer
   either way the silicon reads:
     - all ones (Rev V): a free pin is refused  -> case B below fails;
     - zeros (the audit's assumption): a lower free pin is written before
       apply() finds the locked one               -> case A below fails.
   The step requires both cases, and prints what the unclocked read gives.
   A: PK3 (free) and PK5 (locked) in one mask -> HAL_ERROR, nothing written,
      and the call left the port clocked (MODER at reset is 0xFFFFFFFF, the
      same as an unclocked read, so "MODER unchanged" alone would not tell).
   B: PK3 alone                               -> HAL_OK, PK3 becomes an output.
   C (R1): on the unclocked port, GPIO_BM_IsPortClocked is false and both
      ReadConfig return GPIO_BM_ERR_NO_CLOCK with the output struct untouched
      (sentinel 0xA5 compared byte by byte with a copy). Control, after case A
      clocked the port: IsPortClocked true, both ReadConfig OK, PK3 analog and
      no pull. The oracle of IsPortClocked is a raw read of both RCC banks. */
static void run_t16_a1(void)
{
    const GPIO_InitTypeDef both = { .Pin = GPIO_PIN_3 | GPIO_PIN_5, .Mode = GPIO_MODE_OUTPUT_PP,
                                    .Pull = GPIO_NOPULL, .Speed = GPIO_SPEED_FREQ_LOW, .Alternate = 0U };
    const GPIO_InitTypeDef pk3  = { .Pin = GPIO_PIN_3, .Mode = GPIO_MODE_OUTPUT_PP,
                                    .Pull = GPIO_NOPULL, .Speed = GPIO_SPEED_FREQ_LOW, .Alternate = 0U };

    /* Clocked: PK5 joins the pins T16 locked (PK0|PK1), PK3 goes to analog. */
    (void)GPIO_BM_EnablePortClock(GPIOK);
    const bool     add      = GPIO_HAL_LockPins(GPIOK, GPIO_PIN_5) == HAL_OK;
    const bool     pk3_an   = gst_deinit(GST_TIER_BM, GPIOK, 3U) == (uint32_t)GPIO_BM_OK;
    const uint32_t lckr_on  = GPIOK->LCKR;
    const uint32_t moder_on = GPIOK->MODER;

    /* Unclocked: what the silicon returns (information), then case A. */
    const bool     off      = gpiok_unclock();
    const uint32_t lckr_off = GPIOK->LCKR;
    const bool     pk3_lk   = GPIO_BM_IsLocked(GPIOK, 3U);
    const bool     pk5_lk   = GPIO_BM_IsLocked(GPIOK, 5U);

    /* Case C, still unclocked. */
    GpioBm_PinConfig_t cb, cb_ref;
    GpioLL_PinConfig_t cl, cl_ref;
    (void)memset(&cb, 0xA5, sizeof cb);
    (void)memset(&cl, 0xA5, sizeof cl);
    (void)memcpy(&cb_ref, &cb, sizeof cb);
    (void)memcpy(&cl_ref, &cl, sizeof cl);
    const bool            clk_c    = GPIO_BM_IsPortClocked(GPIOK);
    const bool            raw_c    = gpiok_clocked_raw();
    const GpioBm_Status_t sb       = GPIO_BM_ReadConfig(GPIOK, 3U, &cb);
    const GpioBm_Status_t sl       = GPIO_LL_ReadConfig(GPIOK, 3U, &cl);
    const bool            sent_ok  = (memcmp(&cb, &cb_ref, sizeof cb) == 0) &&
                                     (memcmp(&cl, &cl_ref, sizeof cl) == 0);

    const HAL_StatusTypeDef ha = GPIO_HAL_Init(GPIOK, &both);
    const uint32_t moder_a  = GPIOK->MODER;
    const uint32_t lckr_a   = GPIOK->LCKR;
    const bool     clk_a    = GPIO_BM_IsPortClocked(GPIOK);
    const bool     raw_a    = gpiok_clocked_raw();

    /* Case C control: clocked by case A, PK3 still analog. */
    GpioBm_PinConfig_t kb;
    GpioLL_PinConfig_t kl;
    (void)memset(&kb, 0, sizeof kb);
    (void)memset(&kl, 0, sizeof kl);
    const GpioBm_Status_t kbs = GPIO_BM_ReadConfig(GPIOK, 3U, &kb);
    const GpioBm_Status_t kls = GPIO_LL_ReadConfig(GPIOK, 3U, &kl);
    const bool ctrl_c = clk_a && (kbs == GPIO_BM_OK) && (kls == GPIO_BM_OK) &&
                        (kb.mode == GPIO_BM_MODE_ANALOG) && (kb.pull == GPIO_BM_PULL_NONE) &&
                        (kl.mode == LL_GPIO_MODE_ANALOG) && (kl.pull == LL_GPIO_PULL_NO);

    /* Case B, from the unclocked port again. */
    const bool off2 = gpiok_unclock();
    const HAL_StatusTypeDef hb = GPIO_HAL_Init(GPIOK, &pk3);
    const bool pk3_moved = ((GPIOK->MODER >> (3U * 2U)) & 3UL) == 1UL;

    const bool setup = add && pk3_an && (lckr_on == (GPIO_LCKR_LCKK | 0x23UL)) &&
                       (((moder_on >> (3U * 2U)) & 3UL) == 3UL) && off && off2;
    const bool case_a = (ha == HAL_ERROR) && (moder_a == moder_on) && (lckr_a == lckr_on) && clk_a && raw_a;
    const bool case_b = (hb == HAL_OK) && pk3_moved;
    const bool case_c = !clk_c && !raw_c && (sb == GPIO_BM_ERR_NO_CLOCK) && (sl == GPIO_BM_ERR_NO_CLOCK) &&
                        sent_ok && ctrl_c;
    LOG("T16 A1 GPIOK clocked: add PK5=%u LCKR=0x%08lX MODER=0x%08lX", (unsigned)add, (unsigned long)lckr_on,
        (unsigned long)moder_on);
    LOG("T16 A1 GPIOK unclocked in both banks=%u: LCKR reads 0x%08lX, IsLocked PK3=%u PK5=%u",
        (unsigned)off, (unsigned long)lckr_off, (unsigned)pk3_lk, (unsigned)pk5_lk);
    LOG("T16 A1 case A GPIO_HAL_Init(PK3|PK5)=%u MODER=0x%08lX LCKR=0x%08lX; case B PK3 alone=%u moved=%u",
        (unsigned)ha, (unsigned long)moder_a, (unsigned long)lckr_a, (unsigned)hb, (unsigned)pk3_moved);
    LOG("T16 A1 case A left the port clocked: IsPortClocked=%u raw RCC=%u", (unsigned)clk_a, (unsigned)raw_a);
    LOG("T16 A1 case C unclocked: IsPortClocked=%u raw RCC=%u ReadConfig BM=%u LL=%u (NO_CLOCK=%u) sentinels intact=%u",
        (unsigned)clk_c, (unsigned)raw_c, (unsigned)sb, (unsigned)sl, (unsigned)GPIO_BM_ERR_NO_CLOCK,
        (unsigned)sent_ok);
    LOG("T16 A1 case C control clocked: IsPortClocked=%u BM=%u mode=%lu pull=%lu LL=%u mode=%lu pull=%lu",
        (unsigned)clk_a, (unsigned)kbs, (unsigned long)kb.mode, (unsigned long)kb.pull, (unsigned)kls,
        (unsigned long)kl.mode, (unsigned long)kl.pull);
    verdict(setup && case_a && case_b && case_c,
            "T16 A1: unclocked port: HAL_Init locked pin -> nothing written, free pin -> written; ReadConfig -> NO_CLOCK");
}

static void run_t16(void)
{
    const GstPinReq_t pf7 = { GPIOF, 7U, GPIO_BM_MODE_OUTPUT, 0U, 0U, 0U, 0U, 1U, false };
    const GstPinReq_t pf6 = { GPIOF, 6U, GPIO_BM_MODE_OUTPUT, 0U, 0U, 0U, 0U, 1U, false };
    bool ok = true;

    /* BM on GPIOF (PF7, PF8) */
    (void)GPIO_BM_DeinitPin(GPIOF, 6U);
    (void)GPIO_BM_DeinitPin(GPIOF, 7U);
    const uint32_t lckr0 = GPIOF->LCKR;
    const GpioBm_Status_t st_lock = GPIO_BM_LockPins(GPIOF, (1UL << 7U) | (1UL << 8U));
    const uint32_t lckr1 = GPIOF->LCKR;
    const uint32_t moder = GPIOF->MODER;
    const uint32_t s_bm  = gst_apply(GST_TIER_BM, &pf7);
    const uint32_t s_ll  = gst_apply(GST_TIER_LL, &pf7);
    const uint32_t s_hal = gst_apply(GST_TIER_HAL, &pf7);
    const uint32_t d_bm  = gst_deinit(GST_TIER_BM, GPIOF, 7U);
    const uint32_t d_ll  = gst_deinit(GST_TIER_LL, GPIOF, 7U);
    const uint32_t d_hal = gst_deinit(GST_TIER_HAL, GPIOF, 7U);
    const bool legacy    = GPIO_BM_ConfigAnalog(GPIOF, 7U);
    const bool frozen    = (GPIOF->MODER == moder);
    const bool locked    = GPIO_BM_IsLocked(GPIOF, 7U) && GPIO_LL_IsLocked(GPIOF, 8U) &&
                           !GPIO_BM_IsLocked(GPIOF, 6U) && !GPIO_LL_IsLocked(GPIOF, 6U);
    const uint32_t c6    = gst_apply(GST_TIER_BM, &pf6);                 /* control: PF6 is free */
    const bool c6_moved  = (GPIOF->MODER != moder);
    /* A port already locked. A subset of the locked pins writes nothing; a new
       pin is added by each tier (RM0399 12.4.8 says LCKy is read-only once
       LCKK = 1; this silicon adds them, and the drivers judge by read-back),
       and the pins locked before stay locked. PF3 stays free (control). */
    const GpioBm_Status_t   re_sub = GPIO_BM_LockPins(GPIOF, 1UL << 7U);
    const uint32_t          lckr2  = GPIOF->LCKR;
    const GpioBm_Status_t   add_bm = GPIO_BM_LockPins(GPIOF, 1UL << 6U);
    const GpioBm_Status_t   add_ll = GPIO_LL_LockPins(GPIOF, 1UL << 5U);
    const HAL_StatusTypeDef add_hl = GPIO_HAL_LockPins(GPIOF, 1UL << 4U);
    const uint32_t          lckr3  = GPIOF->LCKR;
    const GstPinReq_t       pf3    = { GPIOF, 3U, GPIO_BM_MODE_OUTPUT, 0U, 0U, 0U, 0U, 1U, false };
    const uint32_t          moder3 = GPIOF->MODER;
    const bool              pf3_ok = !GPIO_BM_IsLocked(GPIOF, 3U) && (gst_apply(GST_TIER_BM, &pf3) == (uint32_t)GPIO_BM_OK) &&
                                     (GPIOF->MODER != moder3);
    const uint32_t          pf6_st = gst_apply(GST_TIER_LL, &pf6);   /* PF6 is locked now */

    const bool bm_ok = (lckr0 == 0U) && (st_lock == GPIO_BM_OK) && (lckr1 == (GPIO_LCKR_LCKK | 0x180UL)) &&
                       (s_bm == (uint32_t)GPIO_BM_ERR_LOCKED) && (s_ll == (uint32_t)GPIO_BM_ERR_LOCKED) &&
                       (s_hal == GST_HAL_ERROR) && (d_bm == (uint32_t)GPIO_BM_ERR_LOCKED) &&
                       (d_ll == (uint32_t)GPIO_BM_ERR_LOCKED) && (d_hal == GST_HAL_ERROR) && !legacy && frozen &&
                       locked && (c6 == (uint32_t)GPIO_BM_OK) && c6_moved && (re_sub == GPIO_BM_OK) &&
                       (lckr2 == lckr1) && (add_bm == GPIO_BM_OK) && (add_ll == GPIO_BM_OK) && (add_hl == HAL_OK) &&
                       (lckr3 == (GPIO_LCKR_LCKK | 0x1F0UL)) && pf3_ok && (pf6_st == (uint32_t)GPIO_BM_ERR_LOCKED);
    ok = ok && bm_ok;
    LOG("T16 BM GPIOF: LCKR before=0x%08lX, LockPins(PF7|PF8)=%u, LCKR after=0x%08lX",
        (unsigned long)lckr0, (unsigned)st_lock, (unsigned long)lckr1);
    LOG("T16 BM GPIOF: PF7 config BM=%lu LL=%lu HAL=%lu, deinit BM=%lu LL=%lu HAL=%lu, ConfigAnalog=%u",
        (unsigned long)s_bm, (unsigned long)s_ll, (unsigned long)s_hal, (unsigned long)d_bm, (unsigned long)d_ll,
        (unsigned long)d_hal, (unsigned)legacy);
    LOG("T16 BM GPIOF: MODER frozen=%u, IsLocked PF7/PF8 yes PF6 no=%u, control PF6 config=%lu moved=%u",
        (unsigned)frozen, (unsigned)locked, (unsigned long)c6, (unsigned)c6_moved);
    LOG("T16 GPIOF relock: PF7 (already)=%u LCKR=0x%08lX; add PF6 BM=%u, PF5 LL=%u, PF4 HAL=%u -> LCKR=0x%08lX",
        (unsigned)re_sub, (unsigned long)lckr2, (unsigned)add_bm, (unsigned)add_ll, (unsigned)add_hl,
        (unsigned long)lckr3);
    LOG("T16 GPIOF: PF6 now refused by LL=%lu; control PF3 free and configurable=%u",
        (unsigned long)pf6_st, (unsigned)pf3_ok);

    /* LL on GPIOJ, HAL on GPIOK: ports with no pad on this package, so a lock
       there can reach nothing. First shown to be real registers. */
    GPIO_TypeDef *const ports[2] = { GPIOJ, GPIOK };
    for (uint32_t k = 0U; k < 2U; k++)
    {
        GPIO_TypeDef *g = ports[k];
        (void)GPIO_BM_EnablePortClock(g);
        const GstPinReq_t p2 = { g, 2U, GPIO_BM_MODE_OUTPUT, 0U, 0U, 0U, 0U, 0U, false };
        const GstPinReq_t p0 = { g, 0U, GPIO_BM_MODE_OUTPUT, 0U, 0U, 0U, 0U, 0U, false };
        const bool real = (gst_apply(GST_TIER_BM, &p2) == (uint32_t)GPIO_BM_OK) && (((g->MODER >> 4U) & 3UL) == 1UL) &&
                          (gst_deinit(GST_TIER_BM, g, 2U) == (uint32_t)GPIO_BM_OK) && (((g->MODER >> 4U) & 3UL) == 3UL);
        uint32_t st;
        if (k == 0U)
        {
            st = (uint32_t)GPIO_LL_LockPins(g, 0x3U);
        }
        else
        {
            st = (GPIO_HAL_LockPins(g, 0x3U) == HAL_OK) ? (uint32_t)GPIO_BM_OK : GST_HAL_ERROR;
        }
        const uint32_t lckr = g->LCKR;
        const uint32_t moder_k = g->MODER;
        const uint32_t own = gst_apply((k == 0U) ? GST_TIER_LL : GST_TIER_HAL, &p0);
        const uint32_t bm  = gst_apply(GST_TIER_BM, &p0);
        const bool k_ok = real && (st == (uint32_t)GPIO_BM_OK) && (lckr == (GPIO_LCKR_LCKK | 0x3UL)) &&
                          (own == ((k == 0U) ? (uint32_t)GPIO_BM_ERR_LOCKED : GST_HAL_ERROR)) &&
                          (bm == (uint32_t)GPIO_BM_ERR_LOCKED) && (g->MODER == moder_k);
        ok = ok && k_ok;
        LOG("T16 %s %s: registers real=%u, LockPins(pin0|pin1)=%lu, LCKR=0x%08lX, own-tier config=%lu, BM config=%lu, "
            "MODER frozen=%u", (k == 0U) ? "LL" : "HAL", (k == 0U) ? "GPIOJ" : "GPIOK", (unsigned)real,
            (unsigned long)st, (unsigned long)lckr, (unsigned long)own, (unsigned long)bm,
            (unsigned)(g->MODER == moder_k));
    }
    verdict(ok, "T16 LCKR: locked pins refuse every tier with nothing written, relock semantics, control pin free");

    run_t16_a1();
}

/* ==========================================================================================
 * Entry points
 * ========================================================================================== */

void GPIO_Selftest_CM7_PreRelease(void)
{
    GstArea_t *a = GST_AREA;
    memset((void *)a, 0, sizeof(*a));
    a->session = DWT->CYCCNT ^ 0x5A5A0000UL;
    __DMB();
    a->magic = GST_AREA_MAGIC;
    __DSB();
}

void GPIO_Selftest_CM7_Run(void)
{
    GstArea_t *a = GST_AREA;

    vTaskSuspendAll();   /* no task switch for the whole test; the HAL tick keeps running */
    bench_safe();

    LOG("CM7 Rev %s", FW_GIT_REV);
    LOG("CM7 Build id %s", FW_BUILD_ID);
    const uint32_t t0 = HAL_GetTick();
    while ((a->m4_ready != a->session) && ((HAL_GetTick() - t0) < 5000U))
    {
    }
    char rev4[sizeof(a->m4_rev)];
    for (uint32_t i = 0U; i < sizeof(rev4); i++)
    {
        rev4[i] = a->m4_rev[i];
    }
    rev4[sizeof(rev4) - 1U] = '\0';
    LOG("CM4 Rev %s", (a->m4_ready == a->session) ? rev4 : "(CPU2 not ready)");
    char id4[sizeof(a->m4_build_id)];
    for (uint32_t i = 0U; i < sizeof(id4); i++)
    {
        id4[i] = a->m4_build_id[i];
    }
    id4[sizeof(id4) - 1U] = '\0';
    LOG("CM4 Build id %s", (a->m4_ready == a->session) ? id4 : "(CPU2 not ready)");
    if (a->m4_ready != a->session)
    {
        s_fail++;
    }
    LOG("GPIO_BENCH v%u idle", (unsigned)BENCH_VERSION);
    LOG("silicon REV_ID=0x%04lX, lock limit %u us, test block at 0x%08lX",
        (unsigned long)((DBGMCU->IDCODE & DBGMCU_IDCODE_REV_ID_Msk) >> DBGMCU_IDCODE_REV_ID_Pos),
        (unsigned)SHARED_GPIO_LOCK_US, (unsigned long)GST_AREA_ADDR);

    /* No bench needed. */
    run_register_tests();
    run_race_tests();

    /* Bench. */
    step_t0a();
    step_t0b();
    for (uint32_t t = 0U; t < (uint32_t)GST_TIER_COUNT; t++)
    {
        step_t3((GstTier_t)t);
    }
    for (uint32_t t = 0U; t < (uint32_t)GST_TIER_COUNT; t++)
    {
        step_t4((GstTier_t)t);
    }
    step_t5t6();
    for (uint32_t t = 0U; t < (uint32_t)GST_TIER_COUNT; t++)
    {
        step_t8((GstTier_t)t);
    }
    step_t8_ctrl();
    step_t9();
    for (uint32_t t = 0U; t < (uint32_t)GST_TIER_COUNT; t++)
    {
        step_t10((GstTier_t)t);
    }

    /* F2 Steps */
    step_t11_cm7("T11_CM7_RISING",  GPIO_BM_EXTI_RISING,  true,  false, 200U);
    step_t11_cm7("T11_CM7_FALLING", GPIO_BM_EXTI_FALLING, true,  false, 200U);
    step_t11_cm7("T11_CM7_BOTH",    GPIO_BM_EXTI_BOTH,    true,  false, 400U);
    step_t11_cm7("T11_CM7_EVENT",   GPIO_BM_EXTI_RISING,  false, true,  0U);

    step_t11_cm4("T11_CM4_RISING",  GPIO_BM_EXTI_RISING,  true,  false, 200U);
    step_t11_cm4("T11_CM4_FALLING", GPIO_BM_EXTI_FALLING, true,  false, 200U);
    step_t11_cm4("T11_CM4_BOTH",    GPIO_BM_EXTI_BOTH,    true,  false, 400U);
    step_t11_cm4("T11_CM4_EVENT",   GPIO_BM_EXTI_RISING,  false, true,  0U);

    step_t12();

    step_t13_cm7();
    step_t13_cm4();

    step_t14();
    step_t17();

    if (step_begin("DONE"))
    {
        step_end();
    }
    bench_safe();
    verdict(!s_bench_abort, "bench completed without abort");

    run_t16();

    (void)cm4_send(GST_CMD_DONE, 0U, 0U, true, NULL);
    verdict((g_shared.m7_gpio_lock_fail_count == 0U) && (g_shared.m4_gpio_lock_fail_count == 0U),
            "production hook: m7_gpio_lock_fail_count=%lu m4_gpio_lock_fail_count=%lu",
            (unsigned long)g_shared.m7_gpio_lock_fail_count, (unsigned long)g_shared.m4_gpio_lock_fail_count);
    LOG("FW VERDICT=%s failures=%lu", (s_fail == 0U) ? "PASS" : "FAIL", (unsigned long)s_fail);

    (void)xTaskResumeAll();
}
