/**
  ******************************************************************************
  * @file    gpio_selftest.c
  * @brief   GPIO driver self-test implementation for Cortex-M4 (prj_04).
  *          CM4 side of the coordinated tests. CM7 drives the sequence through
  *          a job channel in SRAM4 (GPIO_ST_CMD_*); CM4 runs each job and
  *          acknowledges it: T15 phases (b)-(e), T1, T2 against a hand-written
  *          RM0399 field table, and B1. Implements M1 (step-known guard),
  *          B2 (calculated verdict), and B1 (session ID and overlap count).
  ******************************************************************************
  */

#include "gpio_selftest.h"
#include "gpio_bench_protocol.h"
#include "shared_memory.h"
#include "stm32h7xx.h"
#include "gpio_bm.h"
#include "hsem_bm.h"
#include <string.h>
#include <stdbool.h>

#define ST_MAX_LINES 32
static char s_lines[ST_MAX_LINES][SHARED_MBOX_SIZE];
static uint32_t s_line_count = 0U;
static uint32_t s_line_len = 0U;
static uint32_t s_line_next = 0U;

static void Line_Begin(void)
{
    s_line_len = 0U;
    if (s_line_count < ST_MAX_LINES)
    {
        s_lines[s_line_count][0] = '\0';
    }
}

static void Line_End(void)
{
    if (s_line_count < ST_MAX_LINES)
    {
        s_line_count++;
    }
}

static void Line_Str(const char *s)
{
    if (s_line_count >= ST_MAX_LINES) return;
    char *dst = s_lines[s_line_count];
    while ((*s != '\0') && (s_line_len < (SHARED_MBOX_SIZE - 1U)))
    {
        dst[s_line_len++] = *s++;
    }
    dst[s_line_len] = '\0';
}

static void Line_U32(uint32_t v)
{
    char tmp[11];
    uint32_t n = 0U;
    do
    {
        tmp[n++] = (char)('0' + (int)(v % 10U));
        v /= 10U;
    } while ((v != 0U) && (n < sizeof(tmp) - 1U));

    while (n > 0U)
    {
        char c = tmp[--n];
        if (s_line_count < ST_MAX_LINES && s_line_len < (SHARED_MBOX_SIZE - 1U))
        {
            s_lines[s_line_count][s_line_len++] = c;
            s_lines[s_line_count][s_line_len] = '\0';
        }
    }
}

/* "name=value " */
static void Line_KV(const char *key, uint32_t v)
{
    Line_Str(key);
    Line_Str("=");
    Line_U32(v);
    Line_Str(" ");
}

typedef struct {
    uint8_t  step_id;
    uint16_t gpiof_allowed_mask;
} BenchStepPermCM4_t;

static const BenchStepPermCM4_t k_step_perms_cm4[] = {
    { GPIO_BENCH_STEP_T15_RACE,   (1U << 11) },
    { GPIO_BENCH_STEP_T1_REJECT,  (1U << 9) },
    { GPIO_BENCH_STEP_T2_PARITY,  (0xFU << 6) },
    { GPIO_BENCH_STEP_B1_MEASURE, (1U << 11) },
};

static uint8_t s_current_step_id = GPIO_BENCH_STEP_T15_RACE;

static void bench_enter_step(uint8_t step_id)
{
    s_current_step_id = step_id;
}

/**
 * @brief Pure permission check for a given step, port and pin (M1).
 *        CM4 drives only unwired GPIOF pins; the wired lines are CM7's.
 */
static bool bench_step_allows(uint8_t step_id, GPIO_TypeDef *port, uint32_t pin)
{
    if (pin > 15U)
    {
        return false;
    }
    /* PE0 and PE1 (LD2) NEVER allowed */
    if (port == GPIOE && (pin == 0U || pin == 1U))
    {
        return false;
    }
    /* PE2, PE3, PE4 (Solo AD2) NEVER allowed */
    if (port == GPIOE && (pin == 2U || pin == 3U || pin == 4U))
    {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_step_perms_cm4) / sizeof(k_step_perms_cm4[0]); i++)
    {
        if (k_step_perms_cm4[i].step_id == step_id)
        {
            if (port == GPIOF)
            {
                return (k_step_perms_cm4[i].gpiof_allowed_mask & (1U << pin)) != 0U;
            }
        }
    }
    return false;
}

/**
 * @brief Safety guard S4: Bench claim check for Cortex-M4 (M1).
 *        No step parameter accepted; reads s_current_step_id.
 */
static bool bench_claim_output(GPIO_TypeDef *port, uint32_t pin)
{
    return bench_step_allows(s_current_step_id, port, pin);
}

static bool wait_var_cycles(volatile uint32_t *var, uint32_t expected, uint32_t timeout_cycles)
{
    uint32_t start = DWT->CYCCNT;
    while (*var != expected)
    {
        if ((DWT->CYCCNT - start) >= timeout_cycles)
        {
            return false;
        }
    }
    return true;
}

/* CM7 posts the jobs in a fixed order, some of them tens of seconds apart
   (it prints between them). 1e9 cycles is 5 s at 200 MHz; the wait is
   repeated so the counter never has to span more than that. */
#define CM4_WAIT_CHUNK_CYCLES   1000000000UL
#define CM4_WAIT_CHUNKS         12U     /* 60 s */

static bool wait_cmd(uint32_t cmd)
{
    for (uint32_t i = 0U; i < CM4_WAIT_CHUNKS; i++)
    {
        if (wait_var_cycles(&GPIO_TEST_SHARED->cmd, cmd, CM4_WAIT_CHUNK_CYCLES))
        {
            return true;
        }
    }
    return false;
}

static void ack(uint32_t cmd)
{
    __DMB();
    GPIO_TEST_SHARED->ack = cmd;
    __DMB();
}

/* ---- T15 ------------------------------------------------------------------------ */

#define T15_SOLO_ITERS          50000U
#define T15_RMW_ITERS           500000U
#define T15_DRV_ITERS           200000U

/* Same reasoning as the CM7 limit: 100 us at 200 MHz. */
#define T15_HOOK_LIMIT_CYCLES   20000U

#define PF10_SHIFT              (10U * 2U)
#define PF11_SHIFT              (11U * 2U)

static const GpioBm_PinConfig_t k_pf11_in = {
    .port = GPIOF, .pin_idx = 11U, .mode = GPIO_BM_MODE_INPUT,
    .otype = GPIO_BM_OTYPE_PP, .speed = GPIO_BM_SPEED_LOW, .pull = GPIO_BM_PULL_NONE,
};
static const GpioBm_PinConfig_t k_pf11_out = {
    .port = GPIOF, .pin_idx = 11U, .mode = GPIO_BM_MODE_OUTPUT,
    .otype = GPIO_BM_OTYPE_PP, .speed = GPIO_BM_SPEED_LOW, .pull = GPIO_BM_PULL_NONE,
    .initial_level = 0U,
};

static void t15_rmw_loop(uint32_t idx, uint32_t iters, bool concurrent)
{
    uint32_t corr = 0U, ov = 0U;
    volatile GpioSelfTestShared_t *sh = GPIO_TEST_SHARED;

    for (uint32_t i = 0U; i < iters; i++)
    {
        if (concurrent && (sh->in_loop_cm7 != 0U))
        {
            ov++;
        }

        uint32_t val = (i & 1U);
        uint32_t m = GPIOF->MODER;
        m &= ~(3UL << PF11_SHIFT);
        m |= (val << PF11_SHIFT);
        if (concurrent)
        {
            __asm__ volatile("nop; nop; nop; nop;");
        }
        GPIOF->MODER = m;
        if (concurrent)
        {
            __asm__ volatile("nop; nop;");
        }

        if (((GPIOF->MODER >> PF11_SHIFT) & 3U) != val)
        {
            corr++;
        }
        if (((GPIOF->MODER >> PF10_SHIFT) & 3U) > 1U)
        {
            corr++;
        }
    }
    sh->t15_cm4_corr[idx] = corr;
    sh->t15_cm4_overlap[idx] = ov;
    sh->t15_cm4_calls[idx] = iters;
}

static void t15_drv_loop(uint32_t idx, uint32_t iters)
{
    uint32_t corr = 0U, ov = 0U, ok = 0U, busy = 0U, verify = 0U;
    volatile GpioSelfTestShared_t *sh = GPIO_TEST_SHARED;

    for (uint32_t i = 0U; i < iters; i++)
    {
        if (sh->in_loop_cm7 != 0U)
        {
            ov++;
        }

        const GpioBm_PinConfig_t *cfg = ((i & 1U) != 0U) ? &k_pf11_out : &k_pf11_in;
        GpioBm_Status_t r = GPIO_BM_ConfigPin(cfg);
        if (r == GPIO_BM_OK)
        {
            ok++;
        }
        else if (r == GPIO_BM_ERR_BUSY)
        {
            busy++;
        }
        else
        {
            verify++;   /* ERR_VERIFY, or any status other than OK/BUSY */
        }

        if (((GPIOF->MODER >> PF11_SHIFT) & 3U) != cfg->mode)
        {
            corr++;
        }
        if (((GPIOF->MODER >> PF10_SHIFT) & 3U) > 1U)
        {
            corr++;
        }
    }
    sh->t15_cm4_corr[idx] = corr;
    sh->t15_cm4_overlap[idx] = ov;
    sh->t15_cm4_calls[idx] = iters;
    sh->t15_cm4_ok[idx] = ok;
    sh->t15_cm4_busy[idx] = busy;
    sh->t15_cm4_verify[idx] = verify;
}

/* The T15 (e) test hook, instrumented for B1. Only here. */
static volatile uint32_t s_hook_t_enter;
static volatile uint32_t s_hook_t_acq;
static uint32_t s_hook_hold_max;
static uint32_t s_hook_primask_max;
static uint32_t s_hook_fail;
static bool     s_hook_count_calls;

static bool t15_hook_lock(void *ctx)
{
    (void)ctx;
    uint32_t t0 = DWT->CYCCNT;
    if (HSEM_BM_TakeTimeout(BOARD_GPIO_HSEM_ID, 0U, T15_HOOK_LIMIT_CYCLES) != HSEM_BM_OK)
    {
        s_hook_fail++;
        return false;
    }
    s_hook_t_enter = t0;
    s_hook_t_acq   = DWT->CYCCNT;
    if (s_hook_count_calls)
    {
        GPIO_TEST_SHARED->t15_locked_calls++;
    }
    return true;
}

static void t15_hook_unlock(void *ctx)
{
    (void)ctx;
    uint32_t t = DWT->CYCCNT;
    uint32_t hold = t - s_hook_t_acq;
    uint32_t pm   = t - s_hook_t_enter;
    if (hold > s_hook_hold_max)
    {
        s_hook_hold_max = hold;
    }
    if (pm > s_hook_primask_max)
    {
        s_hook_primask_max = pm;
    }
    __DMB();
    (void)HSEM_BM_Release(BOARD_GPIO_HSEM_ID, 0U);
}

static const GpioBm_SharedLock_t k_t15_hook = { t15_hook_lock, t15_hook_unlock, NULL };

/* ---- T1 -------------------------------------------------------------------------- */

#define PHOTO_PORTS     11U
#define PHOTO_REGS      8U

typedef struct
{
    uint32_t enr_c1;
    uint32_t enr_c2;
    uint32_t regs[PHOTO_PORTS][PHOTO_REGS];
} GpioPhoto_t;

static GpioPhoto_t s_ph_before;
static GpioPhoto_t s_ph_after;

static void photo_take(GpioPhoto_t *ph)
{
    ph->enr_c1 = RCC_C1->AHB4ENR;
    ph->enr_c2 = RCC_C2->AHB4ENR;
    uint32_t clocked = ph->enr_c1 | ph->enr_c2;

    for (uint32_t p = 0U; p < PHOTO_PORTS; p++)
    {
        if ((clocked & (1UL << p)) == 0U)
        {
            for (uint32_t r = 0U; r < PHOTO_REGS; r++)
            {
                ph->regs[p][r] = 0U;
            }
            continue;
        }
        const GPIO_TypeDef *g = (const GPIO_TypeDef *)(GPIOA_BASE + (p * 0x400UL));
        ph->regs[p][0] = g->MODER;
        ph->regs[p][1] = g->OTYPER;
        ph->regs[p][2] = g->OSPEEDR;
        ph->regs[p][3] = g->PUPDR;
        ph->regs[p][4] = g->AFR[0];
        ph->regs[p][5] = g->AFR[1];
        ph->regs[p][6] = g->ODR;
        ph->regs[p][7] = g->LCKR;
    }
}

static bool photo_equal(const GpioPhoto_t *x, const GpioPhoto_t *y)
{
    return memcmp(x, y, sizeof(*x)) == 0;
}

static GPIO_TypeDef *t1_unclocked_port(uint32_t *index)
{
    uint32_t clocked = RCC_C1->AHB4ENR | RCC_C2->AHB4ENR;
    for (int32_t p = (int32_t)PHOTO_PORTS - 1; p >= 0; p--)
    {
        if ((clocked & (1UL << (uint32_t)p)) == 0U)
        {
            *index = (uint32_t)p;
            return (GPIO_TypeDef *)(GPIOA_BASE + ((uint32_t)p * 0x400UL));
        }
    }
    *index = 0xFFU;
    return NULL;
}

typedef struct
{
    uint32_t cases;
    uint32_t failed;
    uint32_t first_failed;   /* 1-based index of the first failed case, 0 = none */
} T1Stats_t;

static void t1_check(uint32_t got, uint32_t exp, T1Stats_t *st)
{
    bool ok = (got == exp) && photo_equal(&s_ph_before, &s_ph_after);
    st->cases++;
    if (!ok)
    {
        st->failed++;
        if (st->first_failed == 0U)
        {
            st->first_failed = st->cases;
        }
    }
}

/* CM7 is parked with PRIMASK set while this job runs; PRIMASK here keeps
   CM4's own interrupts out of the photo window too. */
#define T1_CASE(call_expr, expected)                                 \
    do {                                                             \
        uint32_t pm_ = __get_PRIMASK();                              \
        __disable_irq();                                             \
        photo_take(&s_ph_before);                                    \
        uint32_t got_ = (uint32_t)(call_expr);                       \
        photo_take(&s_ph_after);                                     \
        __set_PRIMASK(pm_);                                          \
        t1_check(got_, (uint32_t)(expected), &st);                   \
    } while (0)

static void job_t1(void)
{
    T1Stats_t st = {0};
    uint32_t port_idx;
    volatile GpioSelfTestShared_t *sh = GPIO_TEST_SHARED;

    bench_enter_step(GPIO_BENCH_STEP_T1_REJECT);
    GPIO_TypeDef *P = t1_unclocked_port(&port_idx);

    if (P != NULL)
    {
        GPIO_TypeDef *const misaligned = (GPIO_TypeDef *)(GPIOA_BASE + 4U);
        GPIO_TypeDef *const past_k     = (GPIO_TypeDef *)(GPIOK_BASE + 0x400U);
        GpioBm_PinConfig_t cfg;

#define CFG(p_, pin_, mode_, ot_, sp_, pu_, af_, lvl_)                                   \
        (cfg = (GpioBm_PinConfig_t){ .port = (p_), .pin_idx = (pin_), .mode = (mode_),   \
                                     .otype = (ot_), .speed = (sp_), .pull = (pu_),       \
                                     .af = (af_), .initial_level = (lvl_),                \
                                     .allow_debug_pins = false }, &cfg)

        T1_CASE(GPIO_BM_ConfigPin(NULL),                                     GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(NULL, 0U, 0U, 0U, 0U, 0U, 0U, 0U)),       GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(misaligned, 0U, 0U, 0U, 0U, 0U, 0U, 0U)), GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(past_k, 0U, 0U, 0U, 0U, 0U, 0U, 0U)),     GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(P, 16U, 0U, 0U, 0U, 0U, 0U, 0U)),         GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(P, 0U, 2U, 0U, 0U, 0U, 16U, 0U)),         GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(P, 0U, 0U, 0U, 0U, 3U, 0U, 0U)),          GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(P, 0U, 1U, 0U, 4U, 0U, 0U, 0U)),          GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(P, 0U, 4U, 0U, 0U, 0U, 0U, 0U)),          GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(P, 0U, 1U, 0U, 0U, 0U, 0U, 2U)),          GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(P, 0U, 3U, 0U, 0U, 1U, 0U, 0U)),          GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(P, 0U, 1U, 2U, 0U, 0U, 0U, 0U)),          GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigPin(CFG(GPIOA, 13U, 0U, 0U, 0U, 0U, 0U, 0U)),     GPIO_BM_ERR_DEBUG_PIN);
        T1_CASE(GPIO_BM_ConfigPin(CFG(GPIOA, 14U, 0U, 0U, 0U, 0U, 0U, 0U)),     GPIO_BM_ERR_DEBUG_PIN);
        T1_CASE(GPIO_BM_ConfigOutput(P, 0U, 0U, 4U, 0U, 0U),                 GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigInput(P, 0U, 3U),                              GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigAlternateEx(P, 0U, 16U, 0U, 0U, 0U),           GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_DeinitPin(misaligned, 0U),                           GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_DeinitPin(P, 16U),                                   GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_DeinitPin(GPIOA, 13U),                               GPIO_BM_ERR_DEBUG_PIN);
        T1_CASE(GPIO_BM_LockPins(P, 0U),                                     GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_LockPins(P, 0x10000U),                               GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_LockPins(past_k, 1U),                                GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ReadConfig(P, 0U, NULL),                             GPIO_BM_ERR_PARAM);
        T1_CASE(GPIO_BM_ConfigAlternate(P, 0U, 7U, 3U, 0U),                  0U);
        T1_CASE(GPIO_BM_ConfigAlternate(P, 0U, 7U, 0U, 4U),                  0U);
        T1_CASE(GPIO_BM_ConfigAlternate(P, 0U, 16U, 0U, 0U),                 0U);
        T1_CASE(GPIO_BM_ConfigAnalog(misaligned, 0U),                        0U);
        T1_CASE(GPIO_BM_EnablePortClock(misaligned),                         0U);
        T1_CASE(GPIO_BM_EnablePortClock(past_k),                             0U);
#undef CFG
    }

    bool control_changed = false;
    if (bench_claim_output(GPIOF, 9U))
    {
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        photo_take(&s_ph_before);
        GpioBm_Status_t r = GPIO_BM_ConfigOutput(GPIOF, 9U, GPIO_BM_OTYPE_OD, GPIO_BM_SPEED_HIGH,
                                                 GPIO_BM_PULL_UP, 1U);
        photo_take(&s_ph_after);
        __set_PRIMASK(pm);
        control_changed = (r == GPIO_BM_OK) && !photo_equal(&s_ph_before, &s_ph_after);
        (void)GPIO_BM_DeinitPin(GPIOF, 9U);
    }

    bool pass = (P != NULL) && (st.failed == 0U) && control_changed;
    sh->t1_cm4_cases = st.cases;
    sh->t1_cm4_failed_cases = st.failed;
    sh->t1_cm4_control_changed = control_changed ? 1U : 0U;
    sh->t1_cm4_pass = pass ? 1U : 0U;

    Line_Begin();
    Line_Str("T1: ");
    Line_KV("port_idx", port_idx);
    Line_KV("cases", st.cases);
    Line_KV("failed", st.failed);
    Line_KV("first", st.first_failed);
    Line_End();
    Line_Begin();
    Line_Str("T1: ");
    Line_Str(control_changed ? "control=changed " : "control=UNCHANGED ");
    Line_Str(pass ? "PASS" : "FAIL");
    Line_End();
}

/* ---- T2: against a hand-written RM0399 field table -------------------------------- */

typedef struct
{
    uint32_t moder, otyper, ospeedr, pupdr, afr;
} PinFields_t;

static PinFields_t pin_fields(const GPIO_TypeDef *g, uint32_t pin)
{
    PinFields_t f;
    f.moder   = (g->MODER   >> (pin * 2U)) & 3U;
    f.otyper  = (g->OTYPER  >> pin) & 1U;
    f.ospeedr = (g->OSPEEDR >> (pin * 2U)) & 3U;
    f.pupdr   = (g->PUPDR   >> (pin * 2U)) & 3U;
    f.afr     = (g->AFR[pin >> 3U] >> ((pin & 7U) * 4U)) & 0xFU;
    return f;
}

static bool fields_equal(const PinFields_t *x, const PinFields_t *y)
{
    return (x->moder == y->moder) && (x->otyper == y->otyper) && (x->ospeedr == y->ospeedr) &&
           (x->pupdr == y->pupdr) && (x->afr == y->afr);
}

static void pin_force(GPIO_TypeDef *g, uint32_t pin, const PinFields_t *f)
{
    g->OTYPER  = (g->OTYPER  & ~(1UL << pin))            | (f->otyper << pin);
    g->OSPEEDR = (g->OSPEEDR & ~(3UL << (pin * 2U)))     | (f->ospeedr << (pin * 2U));
    g->PUPDR   = (g->PUPDR   & ~(3UL << (pin * 2U)))     | (f->pupdr << (pin * 2U));
    g->AFR[pin >> 3U] = (g->AFR[pin >> 3U] & ~(0xFUL << ((pin & 7U) * 4U))) | (f->afr << ((pin & 7U) * 4U));
    g->MODER   = (g->MODER   & ~(3UL << (pin * 2U)))     | (f->moder << (pin * 2U));
}

/* Which fields each mode programs, written by hand from RM0399 12.4.1-12.4.10
   (MODER encodes the mode; OTYPER and OSPEEDR only matter for the output
   stage, used by OUTPUT and AF; PUPDR applies in every mode, and the contract
   requires "no pull" in analog; AFRL/AFRH select the function in AF mode). A
   field the mode does not program keeps its previous value. */
typedef struct
{
    uint8_t mode;
    uint8_t writes_otype_speed;
    uint8_t writes_pull;
    uint8_t writes_af;
} RmFieldRow_t;

static const RmFieldRow_t k_rm_fields[4] = {
    { 0U /* input  00 */, 0U, 1U, 0U },
    { 1U /* output 01 */, 1U, 1U, 0U },
    { 2U /* AF     10 */, 1U, 1U, 1U },
    { 3U /* analog 11 */, 0U, 1U, 0U },
};

static const PinFields_t k_t2_start[2] = {
    { .moder = 3U, .otyper = 1U, .ospeedr = 2U, .pupdr = 1U, .afr = 5U },
    { .moder = 1U, .otyper = 0U, .ospeedr = 1U, .pupdr = 2U, .afr = 10U },
};

static const uint32_t k_t2_afs[3] = { 0U, 7U, 15U };

/* ca569b6 legacy bodies, verbatim except for the clock enable (GPIOF is on). */
static void ref_ca569b6_config_alternate(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t af, uint32_t pull, uint32_t speed)
{
    uint32_t afr_idx   = pin_idx >> 3U;
    uint32_t afr_shift = (pin_idx & 7U) * 4U;
    port->AFR[afr_idx] = (port->AFR[afr_idx] & ~(0xFU << afr_shift)) | ((af & 0xFU) << afr_shift);
    port->OTYPER &= ~(1UL << pin_idx);
    uint32_t pos2 = pin_idx * 2U;
    port->OSPEEDR = (port->OSPEEDR & ~(0x3UL << pos2)) | ((speed & 0x3UL) << pos2);
    port->PUPDR = (port->PUPDR & ~(0x3UL << pos2)) | ((pull & 0x3UL) << pos2);
    port->MODER = (port->MODER & ~(0x3UL << pos2)) | (0x2UL << pos2);
}

static void ref_ca569b6_config_analog(GPIO_TypeDef *port, uint32_t pin_idx)
{
    uint32_t pos2 = pin_idx * 2U;
    port->MODER |= (0x3UL << pos2);
    port->PUPDR &= ~(0x3UL << pos2);
}

static void job_t2(void)
{
    volatile GpioSelfTestShared_t *sh = GPIO_TEST_SHARED;
    uint32_t combos = 0U, mismatches = 0U;
    PinFields_t last_got = {0}, last_exp = {0};
    bool have_last = false;
    bool claims = true;

    bench_enter_step(GPIO_BENCH_STEP_T2_PARITY);
    for (uint32_t pin = 6U; pin <= 9U; pin++)
    {
        claims = claims && bench_claim_output(GPIOF, pin);
    }

    static const uint32_t pins[2] = { 6U, 8U };   /* AFR[0] and AFR[1] */

    for (uint32_t mode = 0U; claims && (mode < 4U); mode++)
    for (uint32_t ot = 0U; ot < 2U; ot++)
    for (uint32_t sp = 0U; sp < 4U; sp++)
    for (uint32_t pu = 0U; pu < 3U; pu++)
    for (uint32_t ai = 0U; ai < 3U; ai++)
    {
        uint32_t level = combos & 1U;
        combos++;
        for (uint32_t pi = 0U; pi < 2U; pi++)
        for (uint32_t sv = 0U; sv < 2U; sv++)
        {
            uint32_t pin = pins[pi];
            const PinFields_t *start = &k_t2_start[sv];
            GpioBm_PinConfig_t cfg = {
                .port = GPIOF, .pin_idx = pin, .mode = mode, .otype = ot, .speed = sp,
                .pull = pu, .af = k_t2_afs[ai], .initial_level = level, .allow_debug_pins = false,
            };

            pin_force(GPIOF, pin, start);
            GpioBm_Status_t r = GPIO_BM_ConfigPin(&cfg);

            if ((mode == 3U) && (pu != 0U))
            {
                if (r != GPIO_BM_ERR_PARAM)
                {
                    mismatches++;
                }
                continue;
            }

            const RmFieldRow_t *row = &k_rm_fields[mode];
            PinFields_t exp = {
                .moder   = row->mode,
                .otyper  = row->writes_otype_speed ? ot : start->otyper,
                .ospeedr = row->writes_otype_speed ? sp : start->ospeedr,
                .pupdr   = row->writes_pull ? pu : start->pupdr,
                .afr     = row->writes_af ? k_t2_afs[ai] : start->afr,
            };
            PinFields_t got = pin_fields(GPIOF, pin);
            if ((r != GPIO_BM_OK) || !fields_equal(&got, &exp))
            {
                mismatches++;
            }
            else
            {
                last_got = got;
                last_exp = exp;
                have_last = true;
            }
            if ((mode == 1U) && (((GPIOF->ODR >> pin) & 1U) != level))
            {
                mismatches++;
            }
        }
    }

    /* Legacy with the consumers' arguments against the ca569b6 bodies (PF6
       through the driver, PF7 through the reference). */
    for (uint32_t sv = 0U; claims && (sv < 2U); sv++)
    {
        pin_force(GPIOF, 6U, &k_t2_start[sv]);
        pin_force(GPIOF, 7U, &k_t2_start[sv]);
        bool ok_alt = GPIO_BM_ConfigAlternate(GPIOF, 6U, 7U, GPIO_BM_PULL_UP, GPIO_BM_SPEED_VERY_HIGH);
        ref_ca569b6_config_alternate(GPIOF, 7U, 7U, GPIO_BM_PULL_UP, GPIO_BM_SPEED_VERY_HIGH);
        PinFields_t a = pin_fields(GPIOF, 6U), b = pin_fields(GPIOF, 7U);
        if (!ok_alt || !fields_equal(&a, &b))
        {
            mismatches++;
        }

        pin_force(GPIOF, 6U, &k_t2_start[sv]);
        pin_force(GPIOF, 7U, &k_t2_start[sv]);
        bool ok_an = GPIO_BM_ConfigAnalog(GPIOF, 6U);
        ref_ca569b6_config_analog(GPIOF, 7U);
        a = pin_fields(GPIOF, 6U);
        b = pin_fields(GPIOF, 7U);
        if (!ok_an || !fields_equal(&a, &b))
        {
            mismatches++;
        }
    }

    bool control_detected = false;
    if (have_last)
    {
        PinFields_t altered = last_exp;
        altered.pupdr ^= 1U;
        control_detected = !fields_equal(&last_got, &altered);
    }

    for (uint32_t pin = 6U; claims && (pin <= 9U); pin++)
    {
        (void)GPIO_BM_DeinitPin(GPIOF, pin);
    }

    bool pass = claims && (mismatches == 0U) && control_detected;
    sh->t2_cm4_combos = combos;
    sh->t2_cm4_mismatches = mismatches;
    sh->t2_cm4_control_detected = control_detected ? 1U : 0U;
    sh->t2_cm4_pass = pass ? 1U : 0U;

    Line_Begin();
    Line_Str("T2 vs RM table: ");
    Line_KV("combos", combos);
    Line_KV("mismatches", mismatches);
    Line_End();
    Line_Begin();
    Line_Str("T2: ");
    Line_Str(control_detected ? "control=detected " : "control=MISSED ");
    Line_Str(pass ? "PASS" : "FAIL");
    Line_End();
}

/* ---- B1 -------------------------------------------------------------------------- */

#define B1_SAMPLES      1000U

static void b1_measure(bool with_hook, volatile uint32_t *mn, volatile uint32_t *mx)
{
    uint32_t lo = 0xFFFFFFFFU, hi = 0U;
    GPIO_BM_SetSharedLock(with_hook ? &k_t15_hook : NULL);
    for (uint32_t i = 0U; i < B1_SAMPLES; i++)
    {
        const GpioBm_PinConfig_t *cfg = ((i & 1U) != 0U) ? &k_pf11_out : &k_pf11_in;
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        uint32_t t0 = DWT->CYCCNT;
        (void)GPIO_BM_ConfigPin(cfg);
        uint32_t dt = DWT->CYCCNT - t0;
        __set_PRIMASK(pm);
        if (dt < lo) { lo = dt; }
        if (dt > hi) { hi = dt; }
    }
    Shared_GpioLockInstall();
    *mn = lo;
    *mx = hi;
}

static void job_b1(void)
{
    volatile GpioSelfTestShared_t *sh = GPIO_TEST_SHARED;

    bench_enter_step(GPIO_BENCH_STEP_B1_MEASURE);
    if (bench_claim_output(GPIOF, 11U))
    {
        b1_measure(false, &sh->b1_cm4_cfg_nohook_min, &sh->b1_cm4_cfg_nohook_max);
        b1_measure(true, &sh->b1_cm4_cfg_hook_min, &sh->b1_cm4_cfg_hook_max);
        GPIOF->MODER &= ~(3UL << PF11_SHIFT);
        sh->b1_cm4_done = 1U;
    }

    Line_Begin();
    Line_Str("B1 cycles: ");
    Line_KV("nohook_min", sh->b1_cm4_cfg_nohook_min);
    Line_KV("nohook_max", sh->b1_cm4_cfg_nohook_max);
    Line_End();
    Line_Begin();
    Line_Str("B1 cycles: ");
    Line_KV("hook_min", sh->b1_cm4_cfg_hook_min);
    Line_KV("hook_max", sh->b1_cm4_cfg_hook_max);
    Line_End();
    Line_Begin();
    Line_Str("B1 cycles: ");
    Line_KV("hold_max_e", sh->b1_cm4_hold_max);
    Line_KV("primask_max_e", sh->b1_cm4_primask_max);
    Line_End();
}

/* ---- Entry ------------------------------------------------------------------------ */

static void cm4_fail(const char *why)
{
    Line_Begin();
    Line_Str(why);
    Line_End();
    Line_Begin();
    Line_Str("CM4 VERDICT=FAIL");
    Line_End();
    GPIO_TEST_SHARED->cm4_all_passed = 0U;
    GPIO_TEST_SHARED->cm4_done = 1U;
}

void GPIO_SelfTest_CM4_Run(void)
{
    volatile GpioSelfTestShared_t *sh = GPIO_TEST_SHARED;

    /* Banner Rev <hash> (S8) */
    Line_Begin();
    Line_Str("Rev ");
    Line_Str(FW_GIT_REV);
    Line_End();

    /* Ensure GPIOF clock enabled for PF6..PF11 (CM4 bank) */
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOFEN;
    __DSB();

    bench_enter_step(GPIO_BENCH_STEP_T15_RACE);
    if (!bench_claim_output(GPIOF, 11U))
    {
        cm4_fail("ERROR: bench_claim_output rejected PF11!");
        return;
    }

    /* Wait for CM7 to initialize SRAM4 block and write non-zero session_id (B1) */
    uint32_t start_cyc = DWT->CYCCNT;
    while (sh->session_id == 0U)
    {
        if ((DWT->CYCCNT - start_cyc) >= CM4_WAIT_CHUNK_CYCLES)
        {
            cm4_fail("TIMEOUT waiting for session_id from CM7");
            return;
        }
    }
    uint32_t session = sh->session_id;
    sh->cm4_ready = session;
    __DMB();

    /* (b) solo, hand RMW */
    if (!wait_cmd(GPIO_ST_CMD_T15_B)) { cm4_fail("TIMEOUT waiting for T15 (b)"); return; }
    t15_rmw_loop(0U, T15_SOLO_ITERS, false);
    ack(GPIO_ST_CMD_T15_B);

    /* (c) both, hand RMW */
    if (!wait_cmd(GPIO_ST_CMD_T15_C)) { cm4_fail("TIMEOUT waiting for T15 (c)"); return; }
    sh->in_loop_cm4 = 1U;
    t15_rmw_loop(1U, T15_RMW_ITERS, true);
    sh->in_loop_cm4 = 0U;
    ack(GPIO_ST_CMD_T15_C);

    /* (d) both, gpio_bm without hook */
    if (!wait_cmd(GPIO_ST_CMD_T15_D)) { cm4_fail("TIMEOUT waiting for T15 (d)"); return; }
    sh->in_loop_cm4 = 1U;
    t15_drv_loop(2U, T15_DRV_ITERS);
    sh->in_loop_cm4 = 0U;
    ack(GPIO_ST_CMD_T15_D);

    /* (e) both, gpio_bm with the test hook */
    if (!wait_cmd(GPIO_ST_CMD_T15_E)) { cm4_fail("TIMEOUT waiting for T15 (e)"); return; }
    s_hook_hold_max = 0U;
    s_hook_primask_max = 0U;
    s_hook_fail = 0U;
    s_hook_count_calls = true;
    GPIO_BM_SetSharedLock(&k_t15_hook);
    sh->in_loop_cm4 = 1U;
    t15_drv_loop(3U, T15_DRV_ITERS);
    sh->in_loop_cm4 = 0U;
    Shared_GpioLockInstall();
    s_hook_count_calls = false;
    sh->t15_cm4_hook_fail = s_hook_fail;
    sh->b1_cm4_hold_max = s_hook_hold_max;
    sh->b1_cm4_primask_max = s_hook_primask_max;
    GPIOF->MODER &= ~(3UL << PF11_SHIFT);
    ack(GPIO_ST_CMD_T15_E);

    /* (f) and (g) run on CM7 alone: CM4 waits for the T1 job, touching no GPIO. */

    Line_Begin();
    Line_Str("T15 (b): ");
    Line_KV("corr", sh->t15_cm4_corr[0]);
    Line_End();
    Line_Begin();
    Line_Str("T15 (c): ");
    Line_KV("corr", sh->t15_cm4_corr[1]);
    Line_KV("ovl", sh->t15_cm4_overlap[1]);
    Line_End();
    Line_Begin();
    Line_Str("T15 (d): ");
    Line_KV("corr", sh->t15_cm4_corr[2]);
    Line_KV("verify", sh->t15_cm4_verify[2]);
    Line_KV("ovl", sh->t15_cm4_overlap[2]);
    Line_End();
    Line_Begin();
    Line_Str("T15 (e): ");
    Line_KV("corr", sh->t15_cm4_corr[3]);
    Line_KV("verify", sh->t15_cm4_verify[3]);
    Line_KV("busy", sh->t15_cm4_busy[3]);
    Line_KV("hook_fail", sh->t15_cm4_hook_fail);
    Line_End();
    Line_Begin();
    Line_Str("T15 (e): ");
    Line_KV("ok", sh->t15_cm4_ok[3]);
    Line_KV("calls", sh->t15_cm4_calls[3]);
    Line_KV("ovl", sh->t15_cm4_overlap[3]);
    Line_End();

    bool t15_ok = (sh->t15_cm4_corr[0] == 0U) &&
                  (sh->t15_cm4_overlap[1] > 0U) && (sh->t15_cm4_overlap[2] > 0U) &&
                  (sh->t15_cm4_corr[3] == 0U) && (sh->t15_cm4_verify[3] == 0U) &&
                  (sh->t15_cm4_busy[3] == 0U) && (sh->t15_cm4_hook_fail == 0U) &&
                  (sh->t15_cm4_ok[3] == sh->t15_cm4_calls[3]) && (sh->t15_cm4_overlap[3] > 0U);

    if (!wait_cmd(GPIO_ST_CMD_T1)) { cm4_fail("TIMEOUT waiting for T1"); return; }
    job_t1();
    ack(GPIO_ST_CMD_T1);

    if (!wait_cmd(GPIO_ST_CMD_T2)) { cm4_fail("TIMEOUT waiting for T2"); return; }
    job_t2();
    ack(GPIO_ST_CMD_T2);

    if (!wait_cmd(GPIO_ST_CMD_B1)) { cm4_fail("TIMEOUT waiting for B1"); return; }
    job_b1();
    ack(GPIO_ST_CMD_B1);

    /* B2: Calculated verdict for CM4 */
    bool cm4_ok = t15_ok && (sh->t1_cm4_pass != 0U) && (sh->t2_cm4_pass != 0U) && (sh->b1_cm4_done != 0U);
    sh->cm4_all_passed = cm4_ok ? 1U : 0U;
    sh->cm4_done = 1U;
    __DMB();

    Line_Begin();
    Line_Str("CM4 VERDICT=");
    Line_Str(cm4_ok ? "PASS" : "FAIL");
    Line_End();
}

void GPIO_SelfTest_CM4_Poll(void)
{
    if (s_line_next >= s_line_count) return;

    SHARED_LOCK(HSEM_ID_M4_TO_M7_MBOX);
    if (g_shared.m4_to_m7_len == 0U)
    {
        uint32_t len = (uint32_t)strlen(s_lines[s_line_next]);
        memcpy((void *)g_shared.m4_to_m7, s_lines[s_line_next], len + 1U);
        g_shared.m4_to_m7_len = len;
        s_line_next++;
    }
    SHARED_UNLOCK(HSEM_ID_M4_TO_M7_MBOX);
}
