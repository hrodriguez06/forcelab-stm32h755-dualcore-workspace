/**
  ******************************************************************************
  * @file    i2c_selftest_common.c
  * @brief   05_i2c selftest, code both cores compile: the bench step table and
  *          the line rules of bench_claim() (Common/Inc/i2c_selftest.h).
  *
  *          Pure tables and logic: nothing here touches a register. Each core's
  *          App/Src/i2c_selftest.c owns its writes, and calls ist_line_rule()
  *          and, for a bus pin in AF, ist_s16_pins_ok() on the registers it
  *          read and its own instance check, from its bench_claim(), before any
  *          pin becomes an output or AF or gets a pull.
  ******************************************************************************
  */

#include "i2c_selftest.h"
#include "gpio_bm.h"

#include <string.h>

/* The single table of permissions (S4, S15, S16). Plan "Banco de pruebas":
   in the F0 steps the H755 never drives the bus; the bus pins go to AF (with the
   owner instance at PE = 0) only in T0B (CPU2, AF4) and T0B_CM7 (CPU1, AF6). In
   the F1 steps one core drives the bus through its instance: CPU2 with I2C1
   (AF4) in F1_CM4, CPU1 with I2C4 (AF6) in F1_CM7 and F1_RECOVER, the only step
   where the pins may also be GPIO open-drain outputs (RecoverBus, T9). */
const IstStep_t k_ist_steps[] = {
    /* name            owner         af  od_out */
    { "T0A",           IST_OWN_NONE, 0U, 0U },
    { "T0C",           IST_OWN_NONE, 0U, 0U },   /* levels and the F334's holds: inputs only */
    { "T0B",           IST_OWN_CM4,  1U, 0U },   /* claims of both cores; CPU2 writes AF4    */
    { "T0B_CM7",       IST_OWN_CM7,  1U, 0U },   /* CPU1 claims and writes AF6               */
    { "T0B_TIMEOUT",   IST_OWN_NONE, 0U, 0U },
    { "F1_NOPIN",      IST_OWN_NONE, 0U, 0U },   /* T1-T3, T13, T15, T2/T14: no bus pin      */
    { "F1_CM4",        IST_OWN_CM4,  1U, 0U },   /* I2C1: T1 (BUSY), T4, T6, T7(a), T8, T10,
                                                    T12, B1                                  */
    { "F1_CM7",        IST_OWN_CM7,  1U, 0U },   /* I2C4: T1 (BUSY), T4, T5, T6, T7(b), T8,
                                                    T10, B1                                  */
    { "F1_RECOVER",    IST_OWN_CM7,  1U, 1U },   /* T9: I2C4, and GPIO open-drain for
                                                    I2C_BM_RecoverBus                        */
    { "F2_NOPIN",      IST_OWN_NONE, 0U, 0U },   /* T1, T2 with i2c_ll: no bus pin         */
    { "F2_CM4",        IST_OWN_CM4,  1U, 0U },   /* I2C1 with i2c_ll: T1 (BUSY), T4, T6,
                                                    T7(a), T14                              */
    { "F2_CM7",        IST_OWN_CM7,  1U, 0U },   /* I2C4 with i2c_ll: T1 (BUSY), T4, T5, T6,
                                                    T7(b), T14                              */
    { "DONE",          IST_OWN_NONE, 0U, 0U },
};

const uint32_t k_ist_step_count = (uint32_t)(sizeof(k_ist_steps) / sizeof(k_ist_steps[0]));

uint32_t ist_step_index(const char *name)
{
    for (uint32_t i = 0U; i < k_ist_step_count; i++)
    {
        if (strcmp(k_ist_steps[i].name, name) == 0)
        {
            return i;
        }
    }
    return IST_STEP_NONE;
}

static bool rule_bus(uint32_t core, uint32_t step, uint32_t mode, uint32_t af, uint32_t otype, uint32_t pull)
{
    if (pull != GPIO_BM_PULL_NONE)
    {
        return false;                                   /* S15: never a pull           */
    }
    if (mode == GPIO_BM_MODE_INPUT)
    {
        return true;
    }
    if (otype != GPIO_BM_OTYPE_OD)
    {
        return false;                                   /* S15: never push-pull         */
    }
    if (step >= k_ist_step_count)
    {
        return false;                                   /* between steps: inputs only   */
    }
    const IstStep_t *s = &k_ist_steps[step];
    const uint32_t owner = (core == 0U) ? (uint32_t)IST_OWN_CM7 : (uint32_t)IST_OWN_CM4;
    if (s->owner != owner)
    {
        return false;                                   /* S16: the step's core only    */
    }
    if (mode == GPIO_BM_MODE_AF)
    {
        return (s->af != 0U) && (af == ((core == 0U) ? IST_AF_I2C4 : IST_AF_I2C1));
    }
    return s->od_out != 0U;                             /* GPIO open-drain: recovery    */
}

bool ist_line_rule(uint32_t core, uint32_t step, const GPIO_TypeDef *port, uint32_t pin,
                   uint32_t mode, uint32_t af, uint32_t otype, uint32_t pull)
{
    if ((pin > 15U) || (core > 1U) ||
        ((mode != GPIO_BM_MODE_INPUT) && (mode != GPIO_BM_MODE_OUTPUT) && (mode != GPIO_BM_MODE_AF)))
    {
        return false;
    }
    if ((port == GPIOB) && ((pin == IST_PIN_SCL) || (pin == IST_PIN_SDA)))
    {
        return rule_bus(core, step, mode, af, otype, pull);
    }
    if ((port == GPIOF) && ((pin == IST_PIN_TAP_SCL) || (pin == IST_PIN_TAP_SDA)))
    {
        /* S15: the taps are inputs without pull; never AF4 (I2C4_SCL/SDA). */
        return (mode == GPIO_BM_MODE_INPUT) && (pull == GPIO_BM_PULL_NONE);
    }
    /* Any other pin: an input drives nothing (T0A pulls the AD2 lines). No other
       pin of this bench is ever an output or AF: not the AD2 lines (S4), not
       SPI4 on PE11..PE14 (S13), not the J1 pins PA5/PA6/PB4/PB5 (S14), not
       PE0, PE1, PC13, PA4, PA7, PA13, PA14. MCU_ACK (PE7) is driven by ack()
       directly and never claimed. */
    return mode == GPIO_BM_MODE_INPUT;
}

/* MODER and AFR[1] images with the PB8 and PB9 fields given, the rest 0. */
#define IMG_M(m8, m9)    (((uint32_t)(m8) << (IST_PIN_SCL * 2U)) | ((uint32_t)(m9) << (IST_PIN_SDA * 2U)))
#define IMG_A(a8, a9)    (((uint32_t)(a8) << ((IST_PIN_SCL - 8U) * 4U)) | ((uint32_t)(a9) << ((IST_PIN_SDA - 8U) * 4U)))

uint32_t ist_s16_cases(uint32_t af, uint32_t other_af)
{
    const uint32_t I = GPIO_BM_MODE_INPUT;
    const uint32_t O = GPIO_BM_MODE_OUTPUT;
    const uint32_t A = GPIO_BM_MODE_AF;
    const uint32_t N = GPIO_BM_MODE_ANALOG;
    const uint32_t img[IST_S16_CASES][2] = {
        { IMG_M(I, A), IMG_A(0U, other_af) },     /* 0 PB9 in the other core's AF: refuse  */
        { IMG_M(I, O), IMG_A(0U, 0U) },           /* 1 PB9 a GPIO output: refuse           */
        { IMG_M(A, A), IMG_A(other_af, af) },     /* 2 PB8 in the other AF: refuse         */
        { IMG_M(O, I), IMG_A(0U, 0U) },           /* 3 PB8 a GPIO output: refuse           */
        { IMG_M(I, I), IMG_A(0U, 0U) },           /* 4 both input: accept                  */
        { IMG_M(N, N), IMG_A(0U, 0U) },           /* 5 both analog (reset): accept         */
        { IMG_M(A, A), IMG_A(af, af) },           /* 6 both already in this AF: accept     */
    };
    uint32_t acc = 0U;
    for (uint32_t k = 0U; k < IST_S16_CASES; k++)
    {
        if (ist_s16_pins_ok(img[k][0], img[k][1], af))
        {
            acc |= 1UL << k;
        }
    }
    return acc;
}

/* ---- Data -------------------------------------------------------------------------- */

uint32_t ist_xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void ist_fill(uint8_t *buf, uint32_t n, uint32_t seed)
{
    uint32_t st = (seed != 0U) ? seed : 0x2545F491UL;
    for (uint32_t i = 0U; i < n; i++)
    {
        buf[i] = (uint8_t)(ist_xorshift32(&st) >> 24);
    }
}

/* zlib.crc32 (reflected 0xEDB88320, init and final XOR 0xFFFFFFFF), bitwise: the
   same as the target's crc_byte() (tools/i2c_target_f334/src/target.c). */
uint32_t ist_crc32(const uint8_t *buf, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0U; i < n; i++)
    {
        crc ^= buf[i];
        for (uint32_t k = 0U; k < 8U; k++)
        {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return ~crc;
}

uint32_t ist_check_target(const uint8_t *buf, uint32_t n, uint32_t p0, uint32_t *first)
{
    uint32_t bad = 0U;
    *first = 0xFFFFFFFFUL;
    for (uint32_t i = 0U; i < n; i++)
    {
        if (buf[i] != IST_TARGET_REG(p0 + i))
        {
            if (bad == 0U)
            {
                *first = i;
            }
            bad++;
        }
    }
    return bad;
}

bool ist_s16_pins_ok(uint32_t moder, uint32_t afrh, uint32_t af)
{
    const uint32_t pins[2] = { IST_PIN_SCL, IST_PIN_SDA };
    for (uint32_t k = 0U; k < 2U; k++)
    {
        const uint32_t m = (moder >> (pins[k] * 2U)) & 3UL;
        const uint32_t a = (afrh >> ((pins[k] - 8U) * 4U)) & 0xFUL;
        if ((m == GPIO_BM_MODE_OUTPUT) || ((m == GPIO_BM_MODE_AF) && (a != af)))
        {
            return false;
        }
    }
    return true;
}

/* ==========================================================================================
 * Phase F1: T1, T2 and T14 on any instance, from either core
 * ========================================================================================== */

/* The image of an instance: CR1, CR2, OAR1, OAR2, TIMINGR, TIMEOUTR, ISR. */
void ist_regs(const I2C_TypeDef *i2c, uint32_t r[IST_NREG])
{
    r[0] = i2c->CR1;
    r[1] = i2c->CR2;
    r[2] = i2c->OAR1;
    r[3] = i2c->OAR2;
    r[4] = i2c->TIMINGR;
    r[5] = i2c->TIMEOUTR;
    r[6] = i2c->ISR;
}

/* RM0399 50.7 reset values, typed from the register descriptions (50:2440,
   2572, 2679, 2718, 2764, 2814, 2866: all 0 but ISR = 0x00000001, TXE). */
static const uint32_t k_reset_img[IST_NREG] = { 0U, 0U, 0U, 0U, 0U, 0U, 0x00000001UL };

uint32_t ist_reset_diff(const I2C_TypeDef *i2c)
{
    uint32_t r[IST_NREG];
    ist_regs(i2c, r);
    uint32_t diff = 0U;
    for (uint32_t k = 0U; k < IST_NREG; k++)
    {
        if (r[k] != k_reset_img[k])
        {
            diff |= 1UL << k;
        }
    }
    return diff;
}

/* T1's photo: the instance's image, both RCC banks' enable registers of the I2C
   instances, the kernel clock fields and SYSCFG_PMCR. A refusal writes none. */
#define T1_SNAP   (IST_NREG + 7U)
static void t1_snap(const I2C_TypeDef *i2c, uint32_t *s)
{
    ist_regs(i2c, s);
    s[IST_NREG + 0U] = RCC_C1->APB1LENR;
    s[IST_NREG + 1U] = RCC_C1->APB4ENR;
    s[IST_NREG + 2U] = RCC_C2->APB1LENR;
    s[IST_NREG + 3U] = RCC_C2->APB4ENR;
    s[IST_NREG + 4U] = RCC->D2CCIP2R;
    s[IST_NREG + 5U] = RCC->D3CCIPR;
    s[IST_NREG + 6U] = SYSCFG->PMCR;
}

static bool snap_eq(const uint32_t *a, const uint32_t *b)
{
    for (uint32_t k = 0U; k < T1_SNAP; k++)
    {
        if (a[k] != b[k])
        {
            return false;
        }
    }
    return true;
}

/* ---- Phase F2: Init and CheckConfig through either tier -------------------------- */
static void to_ll(const I2cBm_Config_t *c, I2cLl_Config_t *l)
{
    l->i2c             = c->i2c;
    l->timing          = c->timingr;
    l->analog_filter   = c->analog_filter ? LL_I2C_ANALOGFILTER_ENABLE : LL_I2C_ANALOGFILTER_DISABLE;
    l->digital_filter  = c->dnf;
    l->timeout_scl_low = c->timeout_scl_low;
}

I2cBm_Status_t ist_init(bool ll, const I2cBm_Config_t *c)
{
    if (!ll)
    {
        return I2C_BM_Init(c);
    }
    if (c == NULL)
    {
        return I2C_LL_Init(NULL);
    }
    I2cLl_Config_t l;
    to_ll(c, &l);
    return I2C_LL_Init(&l);
}

I2cBm_Status_t ist_checkconfig(bool ll, const I2cBm_Config_t *c)
{
    if (!ll)
    {
        return I2C_BM_CheckConfig(c);
    }
    if (c == NULL)
    {
        return I2C_LL_CheckConfig(NULL);
    }
    I2cLl_Config_t l;
    to_ll(c, &l);
    return I2C_LL_CheckConfig(&l);
}

static void t1_case(IstT1Result_t *r, I2cBm_Status_t got, I2cBm_Status_t exp)
{
    if (r->cases < IST_T1_MAX)
    {
        r->got[r->cases] = (uint8_t)got;
        r->exp[r->cases] = (uint8_t)exp;
    }
    if (got != exp)
    {
        r->bad++;
    }
    r->cases++;
}

static void rec_w(void *ctx, bool high)
{
    (void)ctx;
    (void)high;
}

static bool rec_r(void *ctx)
{
    (void)ctx;
    return true;
}

/* The refusals of T1 on `i2c` (its clock on, in this core's bank), each status
   written by hand; group 1 with the instance at reset, group 2 initialised with
   `timingr`. A valid Init must change the photo (the control). Leaves the
   instance at reset. */
void ist_t1(I2C_TypeDef *i2c, uint32_t timingr, bool ll, IstT1Result_t *r)
{
    uint32_t a[T1_SNAP];
    uint32_t b[T1_SNAP];
    uint8_t buf[4] = { 0U, 1U, 2U, 3U };
    uint32_t v = 0U;
    IRQn_Type ev = (IRQn_Type)0;
    I2cBm_Caps_t caps;
    I2cBm_TimingOut_t to;
    const I2cBm_Config_t c = { i2c, timingr, true, 0U, 0U };
    I2cBm_Config_t k;
    I2cBm_TimingIn_t ti;
    const I2cBm_TimingIn_t t0 = { 100000000UL, 400000UL, 250U, 20U, true, 0U };
    I2cBm_Recovery_t rec = { rec_w, rec_w, rec_r, rec_r, NULL, 1U };
    I2cBm_Recovery_t rk;
    I2cBm_Xfer_t x = { IST_TARGET_ADDR, buf, 1U, buf, 1U, 0U, 0U, I2C_BM_OK };
    r->cases = 0U;
    r->bad = 0U;
    r->groups_same = 0U;

    /* 1. The instance at reset: parameters, ranges, and what needs an Init. */
    (void)I2C_BM_Deinit(i2c);
    t1_snap(i2c, a);
    t1_case(r, ist_checkconfig(ll, NULL), I2C_BM_ERR_PARAM);
    t1_case(r, ist_init(ll, NULL), I2C_BM_ERR_PARAM);
    k = c; k.i2c = (I2C_TypeDef *)USART3;                       t1_case(r, ist_init(ll, &k), I2C_BM_ERR_PARAM);
    k = c; k.i2c = (I2C_TypeDef *)((uint32_t)I2C1 + 4U);        t1_case(r, ist_init(ll, &k), I2C_BM_ERR_PARAM);
    k = c; k.dnf = 16U;                                          t1_case(r, ist_init(ll, &k), I2C_BM_ERR_PARAM);
    k = c; k.timeout_scl_low = 4097U;                            t1_case(r, ist_init(ll, &k), I2C_BM_ERR_PARAM);
    k = c; k.timingr = timingr | 0x01000000UL;                   t1_case(r, ist_init(ll, &k), I2C_BM_ERR_PARAM);
    if (ll)
    {
        /* LL only: an analog_filter that is not one of the two LL constants. */
        const I2cLl_Config_t lc = { i2c, timingr, 0x00001234UL, 0U, 0U };
        t1_case(r, I2C_LL_Init(&lc), I2C_BM_ERR_PARAM);
    }
    t1_case(r, I2C_BM_CalcTiming(NULL, &to), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_CalcTiming(&t0, NULL), I2C_BM_ERR_PARAM);
    ti = t0; ti.kernel_hz = 0U;                                   t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_PARAM);
    ti = t0; ti.scl_hz = 0U;                                      t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_PARAM);
    ti = t0; ti.scl_hz = 1000001UL;                               t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_PARAM);
    ti = t0; ti.dnf = 16U;                                        t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_PARAM);
    ti = t0; ti.scl_hz = 100000UL; ti.kernel_hz = 3999999UL;      t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_RANGE);
    ti = t0; ti.kernel_hz = 9999999UL;                            t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_RANGE);
    ti = t0; ti.scl_hz = 1000000UL; ti.kernel_hz = 19999999UL; ti.rise_ns = 100U;
                                                                  t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_RANGE);
    ti = t0; ti.scl_hz = 100000UL; ti.rise_ns = 1001U;            t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_RANGE);
    ti = t0; ti.rise_ns = 301U;                                   t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_RANGE);
    ti = t0; ti.scl_hz = 1000000UL; ti.rise_ns = 121U;            t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_RANGE);
    ti = t0; ti.fall_ns = 301U;                                   t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_RANGE);
    ti = t0; ti.scl_hz = 1000000UL; ti.rise_ns = 100U; ti.fall_ns = 121U;
                                                                  t1_case(r, I2C_BM_CalcTiming(&ti, &to), I2C_BM_ERR_RANGE);
    t1_case(r, I2C_BM_GetCaps(NULL, &caps), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_GetCaps(i2c, NULL), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_GetIRQn(i2c, NULL, &ev), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_GetDmaRequest(i2c, true, NULL), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_EnableClock((I2C_TypeDef *)USART3), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_DisableClock(NULL), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_GetKernelSource(i2c, NULL), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_SetFastModePlus((I2C_TypeDef *)USART3, true), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_Deinit((I2C_TypeDef *)USART3), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_RecoverBus(NULL, 1U, &v), I2C_BM_ERR_PARAM);
    rk = rec; rk.sda_read = NULL;                                 t1_case(r, I2C_BM_RecoverBus(&rk, 1U, &v), I2C_BM_ERR_PARAM);
    rk = rec; rk.half_period_cycles = 0U;                         t1_case(r, I2C_BM_RecoverBus(&rk, 1U, &v), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_RecoverBus(&rec, 0U, &v), I2C_BM_ERR_PARAM);
    t1_case(r, ist_write(ll, i2c, IST_TARGET_ADDR, buf, 1U, 1000U), I2C_BM_ERR_BUSY);   /* PE = 0 */
    t1_case(r, ist_read(ll, i2c, IST_TARGET_ADDR, buf, 1U, 1000U), I2C_BM_ERR_BUSY);
    t1_case(r, ist_startit(ll, i2c, NULL), I2C_BM_ERR_PARAM);
    t1_case(r, ist_waitend(ll, i2c, 0U, 1000U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_waitend(ll, i2c, 1U, 0U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_dmaw(ll, i2c, IST_TARGET_ADDR, buf, 1U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_dmaw(ll, i2c, IST_TARGET_ADDR, buf, 0U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_dmaw(ll, i2c, IST_TARGET_ADDR, NULL, 2U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_dmar(ll, i2c, IST_TARGET_ADDR, 0U), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_CheckTransfer((I2C_TypeDef *)USART3, IST_TARGET_ADDR, 1U, 0U), I2C_BM_ERR_PARAM);
    t1_snap(i2c, b);
    r->groups_same |= snap_eq(a, b) ? 1U : 0U;

    /* 2. Initialised (PE = 1): what the hardware would ignore, and the transfers'
       own fields. */
    k = c;
    const I2cBm_Status_t i2 = ist_init(ll, &k);
    t1_snap(i2c, a);
    k = c; k.timingr = timingr ^ 0x00000001UL;                   t1_case(r, ist_init(ll, &k), I2C_BM_ERR_BUSY);
    t1_case(r, ist_checkconfig(ll, &k), I2C_BM_ERR_BUSY);
    k = c; k.dnf = 1U;                                           t1_case(r, ist_init(ll, &k), I2C_BM_ERR_BUSY);
    k = c; k.analog_filter = false;                              t1_case(r, ist_init(ll, &k), I2C_BM_ERR_BUSY);
    k = c; k.timeout_scl_low = 10U;                              t1_case(r, ist_init(ll, &k), I2C_BM_ERR_BUSY);
    t1_case(r, ist_write(ll, i2c, 0x07U, buf, 1U, 1000U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_write(ll, i2c, 0x78U, buf, 1U, 1000U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_write(ll, i2c, IST_TARGET_ADDR, buf, 65536UL, 1000U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_read(ll, i2c, IST_TARGET_ADDR, buf, 0U, 1000U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_write(ll, i2c, IST_TARGET_ADDR, NULL, 1U, 1000U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_writeread(ll, i2c, IST_TARGET_ADDR, buf, 0U, buf, 1U, 1000U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_writeread(ll, i2c, IST_TARGET_ADDR, buf, 1U, buf, 0U, 1000U), I2C_BM_ERR_PARAM);
    t1_case(r, ist_write(ll, i2c, IST_TARGET_ADDR, buf, 1U, 0U), I2C_BM_ERR_PARAM);
    x.tx = NULL;                                                 t1_case(r, ist_startit(ll, i2c, &x), I2C_BM_ERR_PARAM);
    x.tx = buf; x.addr7 = 0x78U;                                 t1_case(r, ist_startit(ll, i2c, &x), I2C_BM_ERR_PARAM);
    t1_case(r, I2C_BM_CheckTransfer(i2c, IST_TARGET_ADDR, 0U, 65536UL), I2C_BM_ERR_PARAM);
    t1_case(r, ist_dmaw(ll, i2c, IST_TARGET_ADDR, buf, 65536UL), I2C_BM_ERR_PARAM);
    t1_case(r, ist_waitend(ll, i2c, 65536UL, 1000U), I2C_BM_ERR_PARAM);
    t1_snap(i2c, b);
    r->groups_same |= snap_eq(a, b) ? 2U : 0U;
    (void)I2C_BM_Deinit(i2c);

    /* The control: a valid Init changes the photo. */
    t1_snap(i2c, a);
    k = c;
    const I2cBm_Status_t ic = ist_init(ll, &k);
    t1_snap(i2c, b);
    r->control_changed = (!snap_eq(a, b) && ((ic == I2C_BM_OK) || (ic == I2C_BM_ERR_BUS)) &&
                          ((i2 == I2C_BM_OK) || (i2 == I2C_BM_ERR_BUS))) ? 1U : 0U;
    r->init_status = (uint32_t)ic;
    (void)I2C_BM_Deinit(i2c);
}

/* T1 with BUSY = 1 (the target holds SDA low after a START on the bus): every
   transfer refused with ERR_BUS, nothing written. `i2c` is initialised and its
   pins in AF. */
void ist_t1_busy(I2C_TypeDef *i2c, bool ll, IstT1Result_t *r)
{
    uint32_t a[T1_SNAP];
    uint32_t b[T1_SNAP];
    uint8_t buf[4] = { 0U, 1U, 2U, 3U };
    I2cBm_Xfer_t x = { IST_TARGET_ADDR, buf, 2U, NULL, 0U, 0U, 0U, I2C_BM_OK };
    r->cases = 0U;
    r->bad = 0U;
    r->groups_same = 0U;
    r->isr = i2c->ISR;
    t1_snap(i2c, a);
    t1_case(r, I2C_BM_CheckTransfer(i2c, IST_TARGET_ADDR, 1U, 0U), I2C_BM_ERR_BUS);
    t1_case(r, ist_write(ll, i2c, IST_TARGET_ADDR, buf, 1U, 100000U), I2C_BM_ERR_BUS);
    t1_case(r, ist_write(ll, i2c, IST_TARGET_ADDR, NULL, 0U, 100000U), I2C_BM_ERR_BUS);
    t1_case(r, ist_read(ll, i2c, IST_TARGET_ADDR, buf, 2U, 100000U), I2C_BM_ERR_BUS);
    t1_case(r, ist_writeread(ll, i2c, IST_TARGET_ADDR, buf, 1U, buf, 2U, 100000U), I2C_BM_ERR_BUS);
    t1_case(r, ist_startit(ll, i2c, &x), I2C_BM_ERR_BUS);
    t1_case(r, ist_dmaw(ll, i2c, IST_TARGET_ADDR, buf, 2U), I2C_BM_ERR_BUS);
    t1_case(r, ist_dmar(ll, i2c, IST_TARGET_ADDR, 2U), I2C_BM_ERR_BUS);
    t1_snap(i2c, b);
    r->groups_same = snap_eq(a, b) ? 1U : 0U;
}

/* ---- T2: register images against the RM0399 field table typed here ------------------- */
/* CR1 (50:2438-2566): PE bit 0, DNF bits 11:8, ANFOFF bit 12; TIMEOUTR (50:2812-2862):
   TIMEOUTA bits 11:0, TIDLE bit 12, TIMOUTEN bit 15; TIMINGR as written (50:2762-2808). */
static const uint32_t k_t2_timingr[] = { 0x00000000UL, 0x30C06C81UL, 0x20B0142FUL, 0x00E01931UL, 0xF0FFFFFFUL };
static const uint32_t k_t2_dnf[] = { 0U, 1U, 15U };
static const uint32_t k_t2_tmo[] = { 0U, 1U, 489U, 4096U };
#define T2_N(t)   ((uint32_t)(sizeof(t) / sizeof((t)[0])))

static void t2_expect(uint32_t timingr, bool analog, uint32_t dnf, uint32_t tmo, uint32_t e[IST_NREG])
{
    e[0] = 0x00000001UL | (analog ? 0U : (1UL << 12)) | (dnf << 8);
    e[1] = 0U;
    e[2] = 0U;
    e[3] = 0U;
    e[4] = timingr;
    e[5] = (tmo == 0U) ? 0U : ((tmo - 1U) | (1UL << 15));
}

/* True when an image differs from the expected one in any of the six registers;
   the first difference is recorded in r (NULL: not recorded). */
static bool t2_differs(const uint32_t g[IST_NREG], const uint32_t e[IST_NREG], uint32_t cs, IstT2Result_t *r)
{
    bool differs = false;
    for (uint32_t k = 0U; k < 6U; k++)
    {
        if (g[k] != e[k])
        {
            if ((r != NULL) && (r->first_bad == 0xFFFFFFFFUL))
            {
                r->first_bad = (cs << 4) | k;
                r->got = g[k];
                r->exp = e[k];
            }
            differs = true;
        }
    }
    return differs;
}

/* T2 with i2c_ll: parity with i2c_bm over the same matrix (phase F2). */
void ist_t2_ll(I2C_TypeDef *i2c, IstT2LlResult_t *r)
{
    r->cases = 0U;
    r->bad_rm = 0U;
    r->bad_parity = 0U;
    r->bad_status = 0U;
    r->bad_deinit = 0U;
    r->first_bad = 0xFFFFFFFFUL;
    r->got = 0U;
    r->exp = 0U;
    r->control = 0U;
    IstT2Result_t first;                               /* where the first parity difference is */
    first.first_bad = 0xFFFFFFFFUL;
    for (uint32_t a = 0U; a < T2_N(k_t2_timingr); a++)
    {
        for (uint32_t f = 0U; f < 2U; f++)
        {
            for (uint32_t d = 0U; d < T2_N(k_t2_dnf); d++)
            {
                for (uint32_t m = 0U; m < T2_N(k_t2_tmo); m++)
                {
                    const bool analog = (f == 0U);
                    const I2cBm_Config_t c = { i2c, k_t2_timingr[a], analog, k_t2_dnf[d], k_t2_tmo[m] };
                    I2cBm_Config_t c2 = c;
                    c2.dnf = (k_t2_dnf[d] + 1U) & 15U;
                    uint32_t e[IST_NREG];
                    uint32_t g_ll[IST_NREG];
                    uint32_t g_bm[IST_NREG];
                    uint32_t g_bm2[IST_NREG];
                    t2_expect(k_t2_timingr[a], analog, k_t2_dnf[d], k_t2_tmo[m], e);
                    const I2cBm_Status_t s_bm = I2C_BM_Init(&c);
                    ist_regs(i2c, g_bm);
                    const bool d1 = (I2C_BM_Deinit(i2c) == I2C_BM_OK) && (ist_reset_diff(i2c) == 0U);
                    const I2cBm_Status_t s_bm2 = I2C_BM_Init(&c2);
                    ist_regs(i2c, g_bm2);
                    const bool d2 = (I2C_BM_Deinit(i2c) == I2C_BM_OK) && (ist_reset_diff(i2c) == 0U);
                    const I2cBm_Status_t s_ll = ist_init(true, &c);
                    ist_regs(i2c, g_ll);
                    const bool d3 = (I2C_BM_Deinit(i2c) == I2C_BM_OK) && (ist_reset_diff(i2c) == 0U);
                    if (((s_ll != I2C_BM_OK) && (s_ll != I2C_BM_ERR_BUS)) || (s_ll != s_bm) ||
                        ((s_bm2 != I2C_BM_OK) && (s_bm2 != I2C_BM_ERR_BUS)))
                    {
                        r->bad_status++;
                    }
                    if (!d1 || !d2 || !d3)
                    {
                        r->bad_deinit++;
                    }
                    if (t2_differs(g_ll, e, r->cases, NULL))
                    {
                        r->bad_rm++;
                    }
                    if (t2_differs(g_ll, g_bm, r->cases, &first))
                    {
                        r->bad_parity++;
                    }
                    if (t2_differs(g_ll, g_bm2, r->cases, NULL))
                    {
                        r->control++;                  /* the parity comparison sees DNF + 1 */
                    }
                    r->cases++;
                }
            }
        }
    }
    r->first_bad = first.first_bad;
    r->got = first.got;
    r->exp = first.exp;
}

void ist_t2(I2C_TypeDef *i2c, IstT2Result_t *r)
{
    r->cases = 0U;
    r->bad = 0U;
    r->bad_deinit = 0U;
    r->bad_status = 0U;
    r->first_bad = 0xFFFFFFFFUL;
    r->got = 0U;
    r->exp = 0U;
    r->control = 0U;
    for (uint32_t a = 0U; a < T2_N(k_t2_timingr); a++)
    {
        for (uint32_t f = 0U; f < 2U; f++)
        {
            for (uint32_t d = 0U; d < T2_N(k_t2_dnf); d++)
            {
                for (uint32_t m = 0U; m < T2_N(k_t2_tmo); m++)
                {
                    const bool analog = (f == 0U);
                    const I2cBm_Config_t c = { i2c, k_t2_timingr[a], analog, k_t2_dnf[d], k_t2_tmo[m] };
                    uint32_t e[IST_NREG];
                    uint32_t g[IST_NREG];
                    t2_expect(k_t2_timingr[a], analog, k_t2_dnf[d], k_t2_tmo[m], e);
                    const I2cBm_Status_t st = I2C_BM_Init(&c);
                    ist_regs(i2c, g);
                    if ((st != I2C_BM_OK) && (st != I2C_BM_ERR_BUS))
                    {
                        r->bad_status++;
                    }
                    if (t2_differs(g, e, r->cases, r))
                    {
                        r->bad++;
                    }
                    /* The control (audit F1, m22): the same image through the same
                       comparison against the expected one with DNF + 1 (mod 16): it
                       must differ in every case. */
                    uint32_t e2[IST_NREG];
                    t2_expect(k_t2_timingr[a], analog, (k_t2_dnf[d] + 1U) & 15U, k_t2_tmo[m], e2);
                    if (t2_differs(g, e2, r->cases, NULL))
                    {
                        r->control++;
                    }
                    if ((I2C_BM_Deinit(i2c) != I2C_BM_OK) || (ist_reset_diff(i2c) != 0U))
                    {
                        r->bad_deinit++;
                    }
                    r->cases++;
                }
            }
        }
    }
}
