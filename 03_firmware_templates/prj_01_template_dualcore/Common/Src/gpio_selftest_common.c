/**
  ******************************************************************************
  * @file    gpio_selftest_common.c
  * @brief   00_gpio selftest: the tests both cores run, on pins nothing on the
  *          bench is wired to (PF6..PF11, GPIOJ, GPIOK).
  *
  *          T1  refusals of the three tiers: exact status and a snapshot of
  *              every clocked port and both RCC_AHB4ENR banks, unchanged.
  *          T2  the three tiers against a field table written from RM0399
  *              (and, for BM and LL, against raw HAL_GPIO_Init), ReadConfig,
  *              ODR, the legacy calls.
  *          TB  the section: a hook that refuses (ERR_BUSY, nothing written,
  *              PRIMASK restored, nesting counter restored), nesting, and a
  *              hook changed while a section is open (audit A4).
  *          B1  cycles of one pin configuration per tier, with and without the
  *              hook.
  *          T15 the race loop (the orchestration lives in each core's file).
  *
  *          Oracles never call the code under test: raw register reads and
  *          writes, a field table, ST's HAL_GPIO_Init, private counters.
  *          No logging here: the CM7 prints, the CM4 publishes in SRAM4.
  ******************************************************************************
  */

#include "gpio_selftest.h"
#include "gpio_bm.h"
#include "gpio_ll.h"
#include "gpio_hal.h"
#include "shared_memory.h"
#include "stm32h7xx_hal.h"

#include <stddef.h>
#include <string.h>

#define PORT(i)          ((GPIO_TypeDef *)(GPIOA_BASE + ((uint32_t)(i) * 0x400UL)))
#define PORT_COUNT       11U
#define BAD_PORT_MIS     ((GPIO_TypeDef *)(GPIOA_BASE + 4UL))            /* GPIOA + 4        */
#define BAD_PORT_HIGH    ((GPIO_TypeDef *)(GPIOK_BASE + 0x400UL))        /* past GPIOK       */
#define BAD_PORT_LOW     ((GPIO_TypeDef *)(GPIOA_BASE - 0x400UL))        /* below GPIOA      */



uint32_t gst_core_index(void)
{
#if defined(CORE_CM4)
    return 1U;
#else
    return 0U;
#endif
}

/* ---- Snapshot (oracle of "nothing was written") ------------------------------- */

typedef struct
{
    uint32_t rcc[2];                /* RCC_C1->AHB4ENR, RCC_C2->AHB4ENR */
    uint32_t reg[PORT_COUNT][8];    /* MODER OTYPER OSPEEDR PUPDR AFRL AFRH ODR LCKR */
} GstPhoto_t;

static void photo_take(GstPhoto_t *p)
{
    uint32_t en;

    memset(p, 0, sizeof(*p));
    p->rcc[0] = RCC_C1->AHB4ENR;
    p->rcc[1] = RCC_C2->AHB4ENR;
    en = p->rcc[0] | p->rcc[1];

    for (uint32_t i = 0U; i < PORT_COUNT; i++)
    {
        if ((en & (1UL << i)) == 0UL)
        {
            continue;   /* unclocked: nothing to read, and nothing a refusal may have clocked */
        }
        const GPIO_TypeDef *g = PORT(i);
        p->reg[i][0] = g->MODER;
        p->reg[i][1] = g->OTYPER;
        p->reg[i][2] = g->OSPEEDR;
        p->reg[i][3] = g->PUPDR;
        p->reg[i][4] = g->AFR[0];
        p->reg[i][5] = g->AFR[1];
        p->reg[i][6] = g->ODR;
        p->reg[i][7] = g->LCKR;
    }
}

static bool photo_equal(const GstPhoto_t *a, const GstPhoto_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* ---- Pin fields, read and forced with raw register accesses -------------------- */

typedef struct { uint32_t mode, otype, speed, pull, af, odr; } GstFields_t;

static GstFields_t fields_read(const GPIO_TypeDef *g, uint32_t pin)
{
    GstFields_t f;
    f.mode  = (g->MODER   >> (pin * 2U)) & 3UL;
    f.otype = (g->OTYPER  >> pin) & 1UL;
    f.speed = (g->OSPEEDR >> (pin * 2U)) & 3UL;
    f.pull  = (g->PUPDR   >> (pin * 2U)) & 3UL;
    f.af    = (g->AFR[pin >> 3U] >> ((pin & 7U) * 4U)) & 0xFUL;
    f.odr   = (g->ODR     >> pin) & 1UL;
    return f;
}

/* Test-side writer, used only on pins nothing is wired to. The caller holds
   PRIMASK and this core is the only one working on the port at that moment. */
static void fields_force(GPIO_TypeDef *g, uint32_t pin, const GstFields_t *f)
{
    const uint32_t p2 = pin * 2U;
    const uint32_t p4 = (pin & 7U) * 4U;
    g->BSRR    = (f->odr != 0UL) ? (1UL << pin) : (1UL << (pin + 16U));
    g->OTYPER  = (g->OTYPER  & ~(1UL << pin)) | (f->otype << pin);
    g->OSPEEDR = (g->OSPEEDR & ~(3UL << p2))  | (f->speed << p2);
    g->PUPDR   = (g->PUPDR   & ~(3UL << p2))  | (f->pull  << p2);
    g->AFR[pin >> 3U] = (g->AFR[pin >> 3U] & ~(0xFUL << p4)) | (f->af << p4);
    g->MODER   = (g->MODER   & ~(3UL << p2))  | (f->mode  << p2);
}

static bool fields_equal5(const GstFields_t *a, const GstFields_t *b)
{
    return (a->mode == b->mode) && (a->otype == b->otype) && (a->speed == b->speed) &&
           (a->pull == b->pull) && (a->af == b->af);
}

/* ---- One request, three tiers --------------------------------------------------- */

bool gst_pin_is(const GstPinReq_t *c)
{
    const GstFields_t f = fields_read(c->port, c->pin);
    return (f.mode == c->mode) && (f.otype == c->otype) && (f.speed == c->speed) &&
           (f.pull == c->pull) && (f.af == c->af);
}

static uint32_t hal_mode_of(uint32_t mode, uint32_t otype)
{
    switch (mode)
    {
    case GPIO_BM_MODE_INPUT:  return GPIO_MODE_INPUT;
    case GPIO_BM_MODE_OUTPUT: return (otype != 0U) ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT_PP;
    case GPIO_BM_MODE_AF:     return (otype != 0U) ? GPIO_MODE_AF_OD : GPIO_MODE_AF_PP;
    case GPIO_BM_MODE_ANALOG: return GPIO_MODE_ANALOG;
    default:                  return 0x7FUL;   /* not a HAL mode */
    }
}

static uint32_t hal_class(HAL_StatusTypeDef h)
{
    return (h == HAL_OK) ? (uint32_t)GPIO_BM_OK : ((h == HAL_BUSY) ? (uint32_t)GPIO_BM_ERR_BUSY : GST_HAL_ERROR);
}

/* What each tier must return for a request gpio_bm answers with `bm`. */
static uint32_t expect_for(GstTier_t t, GpioBm_Status_t bm)
{
    if (t != GST_TIER_HAL)
    {
        return (uint32_t)bm;
    }
    return hal_class(GPIO_HAL_Status(bm));
}

uint32_t gst_apply(GstTier_t t, const GstPinReq_t *c)
{
    switch (t)
    {
    case GST_TIER_BM:
    {
        const GpioBm_PinConfig_t b = {
            .port = c->port, .pin_idx = c->pin, .mode = c->mode, .otype = c->otype,
            .speed = c->speed, .pull = c->pull, .af = c->af, .initial_level = c->level,
            .allow_debug_pins = c->allow_dbg,
        };
        return (uint32_t)GPIO_BM_ConfigPin(&b);
    }
    case GST_TIER_LL:
    {
        const GpioLL_PinConfig_t l = {
            .port = c->port, .pin_idx = c->pin, .mode = c->mode, .output_type = c->otype,
            .speed = c->speed, .pull = c->pull, .alternate = c->af, .initial_level = c->level,
            .allow_debug_pins = c->allow_dbg,
        };
        return (uint32_t)GPIO_LL_ConfigPin(&l);
    }
    default:
    {
        const GpioHal_PinConfig_t h = {
            .port = c->port, .pin_idx = c->pin, .mode = hal_mode_of(c->mode, c->otype),
            .pull = c->pull, .speed = c->speed, .alternate = c->af,
            .initial_level = (GPIO_PinState)c->level, .allow_debug_pins = c->allow_dbg,
        };
        return hal_class(GPIO_HAL_ConfigPin(&h));
    }
    }
}

uint32_t gst_deinit(GstTier_t t, GPIO_TypeDef *port, uint32_t pin)
{
    switch (t)
    {
    case GST_TIER_BM: return (uint32_t)GPIO_BM_DeinitPin(port, pin);
    case GST_TIER_LL: return (uint32_t)GPIO_LL_DeinitPin(port, pin);
    default:          return hal_class(GPIO_HAL_DeinitPin(port, pin));
    }
}

/* ==================================================================================
 * T1: refusals
 * ================================================================================== */

static GstT1Result_t *s_t1;

static void t1_check(bool status_ok, const GstPhoto_t *before)
{
    GstPhoto_t after;
    photo_take(&after);
    s_t1->cases++;
    if (!status_ok || !photo_equal(before, &after))
    {
        s_t1->fail++;
    }
}

/* One refusal: snapshot, call, snapshot, all with PRIMASK set so that nothing
   else on this core (a tick, a task switch) can write a pin in between. */
#define T1_CASE(call_expr, ok_expr)                                   \
    do {                                                              \
        GstPhoto_t b_;                                                \
        uint32_t pm_ = __get_PRIMASK();                               \
        __disable_irq();                                              \
        photo_take(&b_);                                              \
        const uint32_t got_ = (uint32_t)(call_expr);                  \
        t1_check((ok_expr), &b_);                                     \
        __set_PRIMASK(pm_);                                           \
        (void)got_;                                                   \
    } while (0)

#define T1_ST(call, exp)   T1_CASE((call), got_ == (uint32_t)(exp))

static const GstPinReq_t k_t1_base = {
    .port = GPIOF, .pin = 9U, .mode = GPIO_BM_MODE_OUTPUT, .otype = GPIO_BM_OTYPE_PP,
    .speed = GPIO_BM_SPEED_LOW, .pull = GPIO_BM_PULL_NONE, .af = 0U, .level = 1U, .allow_dbg = false,
};

/* The refusals every tier has in common, through gst_apply(). */
static void t1_common(GstTier_t t)
{
    GstPinReq_t c;
#define T1_CFG(field_assign, exp)                                     \
    do { c = k_t1_base; field_assign; T1_ST(gst_apply(t, &c), expect_for(t, (exp))); } while (0)

    T1_CFG(c.port = NULL,                                       GPIO_BM_ERR_PARAM);
    T1_CFG(c.port = BAD_PORT_MIS,                               GPIO_BM_ERR_PARAM);
    T1_CFG(c.port = BAD_PORT_HIGH,                              GPIO_BM_ERR_PARAM);
    T1_CFG(c.port = BAD_PORT_LOW,                               GPIO_BM_ERR_PARAM);
    T1_CFG((c.port = GPIOK, c.pin = 16U),                       GPIO_BM_ERR_PARAM);  /* no clock for K either */
    T1_CFG(c.mode = 4U,                                         GPIO_BM_ERR_PARAM);
    T1_CFG(c.speed = 4U,                                        GPIO_BM_ERR_PARAM);
    T1_CFG(c.pull = 3U,                                         GPIO_BM_ERR_PARAM);
    T1_CFG((c.mode = GPIO_BM_MODE_AF, c.af = 16U),              GPIO_BM_ERR_PARAM);
    T1_CFG(c.level = 2U,                                        GPIO_BM_ERR_PARAM);
    T1_CFG((c.mode = GPIO_BM_MODE_ANALOG, c.pull = GPIO_BM_PULL_UP),   GPIO_BM_ERR_PARAM);
    T1_CFG((c.mode = GPIO_BM_MODE_ANALOG, c.pull = GPIO_BM_PULL_DOWN), GPIO_BM_ERR_PARAM);
    T1_CFG((c.port = GPIOA, c.pin = 13U),                       GPIO_BM_ERR_DEBUG_PIN);
    T1_CFG((c.port = GPIOA, c.pin = 14U),                       GPIO_BM_ERR_DEBUG_PIN);
    T1_CFG((c.port = GPIOA, c.pin = 13U, c.pull = 3U),          GPIO_BM_ERR_PARAM);  /* fields before SWD */
#undef T1_CFG
}

static void t1_bm(void)
{
    GpioBm_PinConfig_t out;
    GpioBm_Section_t   *null_section = NULL;

    t1_common(GST_TIER_BM);

    T1_ST(GPIO_BM_ConfigPin(NULL),                                         GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_ConfigOutput(GPIOF, 9U, 2U, 0U, 0U, 0U),                 GPIO_BM_ERR_PARAM);  /* otype 2 */
    T1_ST(GPIO_BM_ConfigInput(GPIOF, 9U, 3U),                              GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_ConfigAlternateEx(GPIOF, 9U, 16U, 0U, 0U, 0U),           GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_DeinitPin(BAD_PORT_MIS, 0U),                             GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_DeinitPin(GPIOF, 16U),                                   GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_DeinitPin(GPIOA, 13U),                                   GPIO_BM_ERR_DEBUG_PIN);
    T1_ST(GPIO_BM_DeinitPin(GPIOA, 14U),                                   GPIO_BM_ERR_DEBUG_PIN);
    T1_ST(GPIO_BM_LockPins(BAD_PORT_MIS, 1U),                              GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_LockPins(GPIOF, 0U),                                     GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_LockPins(GPIOF, 0x10000UL),                              GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_ReadConfig(GPIOF, 9U, NULL),                             GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_ReadConfig(BAD_PORT_MIS, 0U, &out),                      GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_ReadConfig(GPIOF, 16U, &out),                            GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_SectionEnter(null_section),                              GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_CheckConfig(NULL),                                       GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_BM_IsValidPort(BAD_PORT_MIS),                               false);
    T1_ST(GPIO_BM_IsLocked(BAD_PORT_MIS, 0U),                              false);
    T1_ST(GPIO_BM_IsLocked(GPIOF, 16U),                                    false);
    /* Legacy API: bool, same refusals plus G3/G4 */
    T1_ST(GPIO_BM_ConfigAlternate(NULL, 9U, 7U, 0U, 0U),                   false);
    T1_ST(GPIO_BM_ConfigAlternate(GPIOF, 16U, 7U, 0U, 0U),                 false);
    T1_ST(GPIO_BM_ConfigAlternate(GPIOF, 9U, 16U, 0U, 0U),                 false);
    T1_ST(GPIO_BM_ConfigAlternate(GPIOF, 9U, 7U, 3U, 0U),                  false);  /* G3 pull */
    T1_ST(GPIO_BM_ConfigAlternate(GPIOF, 9U, 7U, 0U, 4U),                  false);  /* G3 speed */
    T1_ST(GPIO_BM_ConfigAlternate(BAD_PORT_MIS, 0U, 7U, 0U, 0U),           false);  /* G4 */
    T1_ST(GPIO_BM_ConfigAnalog(NULL, 0U),                                  false);
    T1_ST(GPIO_BM_ConfigAnalog(GPIOF, 16U),                                false);
    T1_ST(GPIO_BM_EnablePortClock(NULL),                                   false);
    T1_ST(GPIO_BM_EnablePortClock(BAD_PORT_MIS),                           false);
    T1_ST(GPIO_BM_EnablePortClock(BAD_PORT_HIGH),                          false);
}

static void t1_ll(void)
{
    GpioLL_PinConfig_t out;
    uint32_t done = 99U;

    t1_common(GST_TIER_LL);

    const GpioLL_PinConfig_t good6 = { GPIOF, 6U, LL_GPIO_MODE_OUTPUT, LL_GPIO_OUTPUT_PUSHPULL,
                                       LL_GPIO_SPEED_FREQ_LOW, LL_GPIO_PULL_NO, 0U, 1U, false };
    const GpioLL_PinConfig_t good7 = { GPIOF, 7U, LL_GPIO_MODE_OUTPUT, LL_GPIO_OUTPUT_OPENDRAIN,
                                       LL_GPIO_SPEED_FREQ_HIGH, LL_GPIO_PULL_UP, 0U, 1U, false };
    GpioLL_PinConfig_t batch[3] = { good6, good7, good6 };
    batch[2].pin_idx = 8U;
    batch[2].pull    = 3U;   /* the third entry is bad: the first two must not be written */

    T1_ST(GPIO_LL_ConfigPin(NULL),                                         GPIO_BM_ERR_PARAM);
    {
        GpioLL_PinConfig_t c = good6;
        c.output_type = 2U;
        T1_ST(GPIO_LL_ConfigPin(&c),                                       GPIO_BM_ERR_PARAM);
    }
    T1_ST(GPIO_LL_ConfigPins(NULL, 1U, &done),                             GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_LL_ConfigPins(batch, 0U, &done),                            GPIO_BM_ERR_PARAM);
    T1_CASE(GPIO_LL_ConfigPins(batch, 3U, &done), (got_ == (uint32_t)GPIO_BM_ERR_PARAM) && (done == 0U));
    T1_ST(GPIO_LL_DeinitPin(BAD_PORT_MIS, 0U),                             GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_LL_DeinitPin(GPIOF, 16U),                                   GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_LL_DeinitPin(GPIOA, 13U),                                   GPIO_BM_ERR_DEBUG_PIN);
    T1_ST(GPIO_LL_DeinitPin(GPIOA, 14U),                                   GPIO_BM_ERR_DEBUG_PIN);
    T1_ST(GPIO_LL_LockPins(BAD_PORT_MIS, 1U),                              GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_LL_LockPins(GPIOF, 0U),                                     GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_LL_LockPins(GPIOF, 0x10000UL),                              GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_LL_ReadConfig(GPIOF, 9U, NULL),                             GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_LL_ReadConfig(BAD_PORT_MIS, 0U, &out),                      GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_LL_ReadConfig(GPIOF, 16U, &out),                            GPIO_BM_ERR_PARAM);
    T1_ST(GPIO_LL_IsLocked(BAD_PORT_MIS, 0U),                              false);
    T1_ST(GPIO_LL_IsLocked(GPIOF, 16U),                                    false);
}

static void t1_hal(void)
{
    GpioHal_PinConfig_t c;
    GPIO_InitTypeDef    init;
    const GpioHal_PinConfig_t base = { GPIOF, 9U, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL,
                                       GPIO_SPEED_FREQ_LOW, 0U, GPIO_PIN_SET, false };
    const GPIO_InitTypeDef init_base = { .Pin = GPIO_PIN_9, .Mode = GPIO_MODE_OUTPUT_PP, .Pull = GPIO_NOPULL,
                                         .Speed = GPIO_SPEED_FREQ_LOW, .Alternate = 0U };

    t1_common(GST_TIER_HAL);

    T1_ST(GPIO_HAL_ConfigPin(NULL),                                        HAL_ERROR);
    c = base; c.mode = GPIO_MODE_IT_RISING;   T1_ST(GPIO_HAL_ConfigPin(&c), HAL_ERROR);   /* EXTI: out of scope */
    c = base; c.mode = GPIO_MODE_EVT_FALLING; T1_ST(GPIO_HAL_ConfigPin(&c), HAL_ERROR);
    c = base; c.mode = 0x04U;                 T1_ST(GPIO_HAL_ConfigPin(&c), HAL_ERROR);   /* not a HAL mode */

    T1_ST(GPIO_HAL_Init(GPIOF, NULL),                                      HAL_ERROR);
    init = init_base; init.Pin = 0U;                  T1_ST(GPIO_HAL_Init(GPIOF, &init), HAL_ERROR);
    init = init_base; init.Pin = 0x10000UL;           T1_ST(GPIO_HAL_Init(GPIOF, &init), HAL_ERROR);
    init = init_base;                                 T1_ST(GPIO_HAL_Init(BAD_PORT_MIS, &init), HAL_ERROR);
    init = init_base; init.Mode = GPIO_MODE_IT_RISING_FALLING; T1_ST(GPIO_HAL_Init(GPIOF, &init), HAL_ERROR);
    init = init_base; init.Pull = 3U;                 T1_ST(GPIO_HAL_Init(GPIOF, &init), HAL_ERROR);
    init = init_base; init.Mode = GPIO_MODE_ANALOG; init.Pull = GPIO_PULLUP; T1_ST(GPIO_HAL_Init(GPIOF, &init), HAL_ERROR);
    /* A good pin next to a SWD pin in the same mask: PA0 must not be written. */
    init = init_base; init.Pin = GPIO_PIN_0 | GPIO_PIN_13; T1_ST(GPIO_HAL_Init(GPIOA, &init), HAL_ERROR);
    /* A good pin and a bad field for the whole mask: PF6 must not be written. */
    init = init_base; init.Pin = GPIO_PIN_6 | GPIO_PIN_7; init.Speed = 4U; T1_ST(GPIO_HAL_Init(GPIOF, &init), HAL_ERROR);

    T1_ST(GPIO_HAL_DeinitPin(BAD_PORT_MIS, 0U),                            HAL_ERROR);
    T1_ST(GPIO_HAL_DeinitPin(GPIOF, 16U),                                  HAL_ERROR);
    T1_ST(GPIO_HAL_DeinitPin(GPIOA, 13U),                                  HAL_ERROR);
    T1_ST(GPIO_HAL_DeinitPin(GPIOA, 14U),                                  HAL_ERROR);
    T1_ST(GPIO_HAL_LockPins(BAD_PORT_MIS, 1U),                             HAL_ERROR);
    T1_ST(GPIO_HAL_LockPins(GPIOF, 0U),                                    HAL_ERROR);
    T1_ST(GPIO_HAL_LockPins(GPIOF, 0x10000UL),                             HAL_ERROR);
}

void gst_t1_run(GstTier_t tier, GstT1Result_t *r)
{
    GstPhoto_t before;
    GstPhoto_t after;
    GstFields_t analog = { 3U, 0U, 0U, 0U, 0U, 0U };

    memset(r, 0, sizeof(*r));
    s_t1 = r;

    switch (tier)
    {
    case GST_TIER_BM: t1_bm();  break;
    case GST_TIER_LL: t1_ll();  break;
    default:          t1_hal(); break;
    }

    /* Control: a valid call of the same tier does change the snapshot, so the
       comparator can see a write. PF9 is forced to analog first. */
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    fields_force(GPIOF, 9U, &analog);
    photo_take(&before);
    GstPinReq_t ok = k_t1_base;
    const uint32_t st = gst_apply(tier, &ok);
    photo_take(&after);
    __set_PRIMASK(pm);
    r->ctrl_changed = ((st == (uint32_t)GPIO_BM_OK) && !photo_equal(&before, &after)) ? 1U : 0U;
}

/* ==================================================================================
 * T2: register images
 * ================================================================================== */

/* RM0399 12.4: the field encodings; C11: which fields each mode writes. Written
   from the reference manual, not from the driver. */
static GstFields_t rm_expected(const GstPinReq_t *c, const GstFields_t *forced)
{
    GstFields_t e = *forced;
    const bool drive = (c->mode == GPIO_BM_MODE_OUTPUT) || (c->mode == GPIO_BM_MODE_AF);

    e.mode = c->mode;
    e.pull = c->pull;
    if (drive)
    {
        e.otype = c->otype;
        e.speed = c->speed;
    }
    if (c->mode == GPIO_BM_MODE_AF)
    {
        e.af = c->af;
    }
    if (c->mode == GPIO_BM_MODE_OUTPUT)
    {
        e.odr = c->level;
    }
    return e;
}

/* Every field different from what the case writes, so a field the tier forgot
   to write can never read back as right by accident. */
static GstFields_t complement(const GstPinReq_t *c)
{
    GstFields_t f;
    f.mode  = (c->mode + 2U) & 3U;
    f.otype = c->otype ^ 1U;
    f.speed = c->speed ^ 3U;
    f.pull  = (c->pull + 1U) % 3U;
    f.af    = c->af ^ 0xFU;
    f.odr   = c->level ^ 1U;
    return f;
}

static const uint32_t k_t2_af[3] = { 0U, 7U, 15U };

void gst_t2_run(GstT2Result_t *r)
{
    uint32_t    idx = 0U;
    GstFields_t last_expect = { 0 };
    GstFields_t last_image  = { 0 };

    memset(r, 0, sizeof(*r));

    for (uint32_t pin = 6U; pin <= 11U; pin++)
    for (uint32_t mode = 0U; mode < 4U; mode++)
    for (uint32_t otype = 0U; otype < 2U; otype++)
    for (uint32_t speed = 0U; speed < 4U; speed++)
    for (uint32_t pull = 0U; pull < 3U; pull++)
    for (uint32_t a = 0U; a < 3U; a++)
    {
        const GstPinReq_t c = { GPIOF, pin, mode, otype, speed, pull, k_t2_af[a], idx & 1U, false };
        const bool refused = (mode == GPIO_BM_MODE_ANALOG) && (pull != GPIO_BM_PULL_NONE);
        idx++;

        for (uint32_t t = 0U; t < (uint32_t)GST_TIER_COUNT; t++)
        {
            const GstFields_t forced = complement(&c);
            uint32_t pm = __get_PRIMASK();
            __disable_irq();
            fields_force(GPIOF, pin, &forced);
            const uint32_t st = gst_apply((GstTier_t)t, &c);
            const GstFields_t img = fields_read(GPIOF, pin);

            if (refused)
            {
                __set_PRIMASK(pm);
                if (st == expect_for((GstTier_t)t, GPIO_BM_ERR_PARAM))
                {
                    r->rejected[t]++;
                }
                else
                {
                    r->diff_rm[t]++;
                }
                continue;
            }

            const GstFields_t e = rm_expected(&c, &forced);
            r->cases[t]++;
            if ((st != (uint32_t)GPIO_BM_OK) || !fields_equal5(&img, &e))
            {
                r->diff_rm[t]++;
            }
            if (img.odr != e.odr)
            {
                r->odr_bad[t]++;
            }

            /* E11: ReadConfig of both tiers against the registers. */
            GpioBm_PinConfig_t rb;
            GpioLL_PinConfig_t rl;
            if ((GPIO_BM_ReadConfig(GPIOF, pin, &rb) != GPIO_BM_OK) ||
                (rb.mode != img.mode) || (rb.otype != img.otype) || (rb.speed != img.speed) ||
                (rb.pull != img.pull) || (rb.af != img.af) || (rb.initial_level != img.odr) ||
                (GPIO_LL_ReadConfig(GPIOF, pin, &rl) != GPIO_BM_OK) ||
                (rl.mode != img.mode) || (rl.output_type != img.otype) || (rl.speed != img.speed) ||
                (rl.pull != img.pull) || (rl.alternate != img.af) || (rl.initial_level != img.odr))
            {
                r->readcfg_bad++;
            }

            /* Second oracle for BM and LL: ST's own HAL_GPIO_Init from the same
               starting point. (For the HAL tier it would compare HAL with itself.) */
            if (t != (uint32_t)GST_TIER_HAL)
            {
                const GPIO_InitTypeDef init = { .Pin = 1UL << pin, .Mode = hal_mode_of(mode, otype),
                                                .Pull = pull, .Speed = speed, .Alternate = c.af };
                fields_force(GPIOF, pin, &forced);
                HAL_GPIO_Init(GPIOF, &init);
                const GstFields_t ref = fields_read(GPIOF, pin);
                if (!fields_equal5(&ref, &img))
                {
                    r->diff_hal[t]++;
                }
            }
            __set_PRIMASK(pm);

            last_expect = e;
            last_image  = img;
        }
    }

    /* G7, shown on the raw HAL: analog + PULLUP leaves PUPDR as it was. */
    for (uint32_t pin = 6U; pin <= 11U; pin++)
    {
        const GstFields_t forced = { 1U, 0U, 0U, GPIO_BM_PULL_DOWN, 0U, 0U };
        const GPIO_InitTypeDef init = { .Pin = 1UL << pin, .Mode = GPIO_MODE_ANALOG, .Pull = GPIO_PULLUP,
                                        .Speed = 0U, .Alternate = 0U };
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        fields_force(GPIOF, pin, &forced);
        HAL_GPIO_Init(GPIOF, &init);
        const GstFields_t img = fields_read(GPIOF, pin);
        __set_PRIMASK(pm);
        if ((img.mode == 3U) && (img.pull == GPIO_BM_PULL_DOWN))
        {
            r->hal_g7_pupdr_kept++;
        }
    }

    /* Legacy calls, as uart_* and adc_* make them: the bits of ca569b6. */
    for (uint32_t pin = 6U; pin <= 11U; pin++)
    {
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        const GstFields_t f1 = { 0U, 1U, 0U, GPIO_BM_PULL_DOWN, 3U, 0U };
        fields_force(GPIOF, pin, &f1);
        const bool ok1 = GPIO_BM_ConfigAlternate(GPIOF, pin, 7U, GPIO_BM_PULL_UP, GPIO_BM_SPEED_VERY_HIGH);
        const GstFields_t g1 = fields_read(GPIOF, pin);
        const GstFields_t f2 = { 1U, 1U, 2U, GPIO_BM_PULL_UP, 5U, 0U };
        fields_force(GPIOF, pin, &f2);
        const bool ok2 = GPIO_BM_ConfigAnalog(GPIOF, pin);
        const GstFields_t g2 = fields_read(GPIOF, pin);
        __set_PRIMASK(pm);

        const GstFields_t e1 = { 2U, 0U, 3U, GPIO_BM_PULL_UP, 7U, 0U };
        const GstFields_t e2 = { 3U, 1U, 2U, GPIO_BM_PULL_NONE, 5U, 0U };   /* only MODER and PUPDR change */
        if (!ok1 || !ok2 || !fields_equal5(&g1, &e1) || !fields_equal5(&g2, &e2))
        {
            r->legacy_bad++;
        }
    }

    /* Control of the comparator: the last matching image against an expectation
       with one OSPEEDR bit flipped must be seen as a difference. */
    last_expect.speed ^= 1U;
    r->ctrl_detected = fields_equal5(&last_image, &last_expect) ? 0U : 1U;
}

/* ==================================================================================
 * TB: the section and the hook
 * ================================================================================== */

static volatile uint32_t s_tb_lock_calls;
static volatile uint32_t s_tb_unlock_calls;
static volatile bool     s_tb_allow;

static bool tb_lock(void *ctx)   { (void)ctx; s_tb_lock_calls++; return s_tb_allow; }
static void tb_unlock(void *ctx) { (void)ctx; s_tb_unlock_calls++; }
static const GpioBm_SharedLock_t k_tb_hook = { tb_lock, tb_unlock, NULL };

/* A second hook, always allowing, with its own counters: the one A4 installs
   while a section taken on k_tb_hook is still open. */
static volatile uint32_t s_tb2_lock_calls;
static volatile uint32_t s_tb2_unlock_calls;

static bool tb2_lock(void *ctx)   { (void)ctx; s_tb2_lock_calls++; return true; }
static void tb2_unlock(void *ctx) { (void)ctx; s_tb2_unlock_calls++; }
static const GpioBm_SharedLock_t k_tb2_hook = { tb2_lock, tb2_unlock, NULL };

static void tb_counts_clear(void)
{
    s_tb_lock_calls    = 0U;
    s_tb_unlock_calls  = 0U;
    s_tb2_lock_calls   = 0U;
    s_tb2_unlock_calls = 0U;
}

typedef enum
{
    TB_BM_CONFIG = 0, TB_BM_OUTPUT, TB_BM_INPUT, TB_BM_ALTEX, TB_BM_DEINIT, TB_BM_LOCK,
    TB_BM_LEGACY_ALT, TB_BM_LEGACY_ANALOG,
    TB_LL_CONFIG, TB_LL_CONFIGS, TB_LL_DEINIT, TB_LL_LOCK,
    TB_HAL_CONFIG, TB_HAL_INIT, TB_HAL_DEINIT, TB_HAL_LOCK,
    TB_CALL_COUNT
} TbCall_t;

/* Returns true when the call reported "busy" in its own terms. */
static bool tb_call_busy(TbCall_t k)
{
    const GpioBm_PinConfig_t bm = { GPIOF, 9U, GPIO_BM_MODE_OUTPUT, 0U, 0U, 0U, 0U, 1U, false };
    const GpioLL_PinConfig_t ll = { GPIOF, 9U, LL_GPIO_MODE_OUTPUT, 0U, 0U, 0U, 0U, 1U, false };
    const GpioHal_PinConfig_t hl = { GPIOF, 9U, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0U,
                                     GPIO_PIN_SET, false };
    const GPIO_InitTypeDef init = { .Pin = GPIO_PIN_9, .Mode = GPIO_MODE_OUTPUT_PP, .Pull = GPIO_NOPULL,
                                    .Speed = GPIO_SPEED_FREQ_LOW, .Alternate = 0U };

    switch (k)
    {
    case TB_BM_CONFIG:        return GPIO_BM_ConfigPin(&bm) == GPIO_BM_ERR_BUSY;
    case TB_BM_OUTPUT:        return GPIO_BM_ConfigOutput(GPIOF, 9U, 0U, 0U, 0U, 1U) == GPIO_BM_ERR_BUSY;
    case TB_BM_INPUT:         return GPIO_BM_ConfigInput(GPIOF, 9U, 1U) == GPIO_BM_ERR_BUSY;
    case TB_BM_ALTEX:         return GPIO_BM_ConfigAlternateEx(GPIOF, 9U, 5U, 1U, 0U, 0U) == GPIO_BM_ERR_BUSY;
    case TB_BM_DEINIT:        return GPIO_BM_DeinitPin(GPIOF, 9U) == GPIO_BM_ERR_BUSY;
    case TB_BM_LOCK:          return GPIO_BM_LockPins(GPIOF, 1UL << 9U) == GPIO_BM_ERR_BUSY;
    case TB_BM_LEGACY_ALT:    return !GPIO_BM_ConfigAlternate(GPIOF, 9U, 7U, 1U, 3U);
    case TB_BM_LEGACY_ANALOG: return !GPIO_BM_ConfigAnalog(GPIOF, 9U);
    case TB_LL_CONFIG:        return GPIO_LL_ConfigPin(&ll) == GPIO_BM_ERR_BUSY;
    case TB_LL_CONFIGS:       return GPIO_LL_ConfigPins(&ll, 1U, NULL) == GPIO_BM_ERR_BUSY;
    case TB_LL_DEINIT:        return GPIO_LL_DeinitPin(GPIOF, 9U) == GPIO_BM_ERR_BUSY;
    case TB_LL_LOCK:          return GPIO_LL_LockPins(GPIOF, 1UL << 9U) == GPIO_BM_ERR_BUSY;
    case TB_HAL_CONFIG:       return GPIO_HAL_ConfigPin(&hl) == HAL_BUSY;
    case TB_HAL_INIT:         return GPIO_HAL_Init(GPIOF, &init) == HAL_BUSY;
    case TB_HAL_DEINIT:       return GPIO_HAL_DeinitPin(GPIOF, 9U) == HAL_BUSY;
    default:                  return GPIO_HAL_LockPins(GPIOF, 1UL << 9U) == HAL_BUSY;
    }
}

static GstTbResult_t *s_tb;

static void tb_expect(bool cond)
{
    s_tb->checks++;
    if (!cond)
    {
        s_tb->fail++;
    }
}

void gst_tb_run(GstTbResult_t *r)
{
    GstPhoto_t before;
    GstPhoto_t after;
    const GstFields_t analog = { 3U, 0U, 0U, 0U, 0U, 0U };
    const uint32_t entry_pm = __get_PRIMASK();

    memset(r, 0, sizeof(*r));
    s_tb = r;

    __disable_irq();
    fields_force(GPIOF, 9U, &analog);
    __set_PRIMASK(entry_pm);

    GPIO_BM_SetSharedLock(&k_tb_hook);

    for (uint32_t pm_in = 0U; pm_in < 2U; pm_in++)
    {
        for (uint32_t k = 0U; k < (uint32_t)TB_CALL_COUNT; k++)
        {
            s_tb_allow = false;
            s_tb_lock_calls = 0U;
            s_tb_unlock_calls = 0U;

            /* The snapshot is taken with PRIMASK set; the call itself runs with
               the PRIMASK of this round (0: interrupts on, 1: as in a fault handler). */
            __disable_irq();
            photo_take(&before);
            __set_PRIMASK(pm_in);
            const bool busy = tb_call_busy((TbCall_t)k);
            const uint32_t pm_out = __get_PRIMASK();
            __disable_irq();
            photo_take(&after);
            __set_PRIMASK(entry_pm);

            if (busy)
            {
                r->busy_calls++;
            }
            tb_expect(busy);                                 /* the tier's own busy status   */
            tb_expect(photo_equal(&before, &after));         /* nothing written              */
            tb_expect(pm_out == pm_in);                      /* PRIMASK restored, not forced */
            tb_expect(s_tb_lock_calls == 1U);                /* asked once                   */
            tb_expect(s_tb_unlock_calls == 0U);              /* never released what it lacks */

            /* The nesting counter came back to 0: the next section asks the hook
               again. If ERR_BUSY had left it at 1, lock() would not be called. */
            s_tb_allow = true;
            s_tb_lock_calls = 0U;
            s_tb_unlock_calls = 0U;
            tb_expect(GPIO_BM_ConfigOutput(GPIOF, 9U, 0U, 0U, 0U, 0U) == GPIO_BM_OK);
            tb_expect((s_tb_lock_calls == 1U) && (s_tb_unlock_calls == 1U));
            __disable_irq();
            fields_force(GPIOF, 9U, &analog);
            __set_PRIMASK(entry_pm);
        }
    }

    /* Nesting: an outer section takes the lock once; a driver call inside it
       does not ask again; the outer exit releases it and restores PRIMASK. */
    for (uint32_t pm_in = 0U; pm_in < 2U; pm_in++)
    {
        GpioBm_Section_t outer;
        s_tb_allow = true;
        s_tb_lock_calls = 0U;
        s_tb_unlock_calls = 0U;
        __set_PRIMASK(pm_in);
        tb_expect(GPIO_BM_SectionEnter(&outer) == GPIO_BM_OK);
        tb_expect(__get_PRIMASK() == 1U);
        tb_expect(s_tb_lock_calls == 1U);
        tb_expect(GPIO_BM_ConfigOutput(GPIOF, 9U, 0U, 0U, 0U, 1U) == GPIO_BM_OK);
        tb_expect(s_tb_lock_calls == 1U);
        tb_expect(s_tb_unlock_calls == 0U);
        tb_expect(__get_PRIMASK() == 1U);
        GPIO_BM_SectionExit(&outer);
        const uint32_t pm_out = __get_PRIMASK();
        __set_PRIMASK(entry_pm);
        tb_expect(s_tb_unlock_calls == 1U);
        tb_expect(pm_out == pm_in);
    }

    /* A4: GPIO_BM_SetSharedLock() while a section taken on k_tb_hook is open,
       to a second hook and to NULL. The exit must release k_tb_hook (not the
       hook installed now, which never took anything), and the next section
       must use the new hook. Before the fix the exit released whatever was
       installed: tb2_unlock() without a tb2_lock(), or nothing at all, which
       with the production hook leaves the HSEM taken. 8 checks x 4 = 32. */
    {
        const GpioBm_SharedLock_t *const swap_to[2] = { &k_tb2_hook, NULL };

        for (uint32_t pm_in = 0U; pm_in < 2U; pm_in++)
        {
            for (uint32_t w = 0U; w < 2U; w++)
            {
                GpioBm_Section_t s;
                GPIO_BM_SetSharedLock(&k_tb_hook);
                s_tb_allow = true;
                tb_counts_clear();
                __set_PRIMASK(pm_in);
                tb_expect(GPIO_BM_SectionEnter(&s) == GPIO_BM_OK);
                GPIO_BM_SetSharedLock(swap_to[w]);
                tb_expect(GPIO_BM_ConfigOutput(GPIOF, 9U, 0U, 0U, 0U, 1U) == GPIO_BM_OK);   /* nested */
                GPIO_BM_SectionExit(&s);
                const uint32_t pm_out = __get_PRIMASK();
                __set_PRIMASK(entry_pm);
                tb_expect(pm_out == pm_in);                                             /* restored     */
                tb_expect((s_tb_lock_calls == 1U) && (s_tb_unlock_calls == 1U));        /* taken = freed */
                tb_expect((s_tb2_lock_calls == 0U) && (s_tb2_unlock_calls == 0U));      /* new hook idle */

                /* The next section asks the hook installed now. */
                tb_counts_clear();
                tb_expect(GPIO_BM_ConfigOutput(GPIOF, 9U, 0U, 0U, 0U, 0U) == GPIO_BM_OK);
                tb_expect((s_tb_lock_calls == 0U) && (s_tb_unlock_calls == 0U));
                tb_expect((w == 0U) ? ((s_tb2_lock_calls == 1U) && (s_tb2_unlock_calls == 1U))
                                    : ((s_tb2_lock_calls == 0U) && (s_tb2_unlock_calls == 0U)));

                __disable_irq();
                fields_force(GPIOF, 9U, &analog);
                __set_PRIMASK(entry_pm);
            }
        }

        /* Control: the counters tell the hooks apart. Swapped BEFORE the
           section, the same call is locked and released on k_tb2_hook only. */
        GPIO_BM_SetSharedLock(&k_tb_hook);
        GPIO_BM_SetSharedLock(&k_tb2_hook);
        tb_counts_clear();
        const bool c_ok = GPIO_BM_ConfigOutput(GPIOF, 9U, 0U, 0U, 0U, 1U) == GPIO_BM_OK;
        r->swap_ctrl = (c_ok && (s_tb_lock_calls == 0U) && (s_tb_unlock_calls == 0U) &&
                        (s_tb2_lock_calls == 1U) && (s_tb2_unlock_calls == 1U)) ? 1U : 0U;
        GPIO_BM_SetSharedLock(&k_tb_hook);
    }

    /* Control: the same hook, allowing, does write. */
    s_tb_allow = true;
    __disable_irq();
    fields_force(GPIOF, 9U, &analog);
    photo_take(&before);
    const GpioBm_Status_t st = GPIO_BM_ConfigOutput(GPIOF, 9U, 0U, 0U, 0U, 1U);
    photo_take(&after);
    __set_PRIMASK(entry_pm);
    r->ctrl_wrote = ((st == GPIO_BM_OK) && !photo_equal(&before, &after)) ? 1U : 0U;

    Shared_GpioLockInstall();   /* back to the production hook */
}

/* ==================================================================================
 * Race pins, configurations A and B, and B1
 * ================================================================================== */

static uint32_t own_pin(void)   { return (gst_core_index() == 0U) ? GST_RACE_PIN_CM7 : GST_RACE_PIN_CM4; }
static uint32_t other_pin(void) { return (gst_core_index() == 0U) ? GST_RACE_PIN_CM4 : GST_RACE_PIN_CM7; }

/* A and B differ in MODER, OTYPER, OSPEEDR and PUPDR, so a lost update in any
   of the four shared registers shows. AF 5 is written by B only; the prepare
   step sets it on both pins so it never changes during the race. */
GstPinReq_t gst_race_cfg(uint32_t pin, uint32_t which)
{
    GstPinReq_t c;
    c.port = GST_RACE_PORT;
    c.pin = pin;
    c.allow_dbg = false;
    if (which == 0U)
    {
        c.mode = GPIO_BM_MODE_OUTPUT; c.otype = GPIO_BM_OTYPE_OD; c.speed = GPIO_BM_SPEED_VERY_HIGH;
        c.pull = GPIO_BM_PULL_DOWN;   c.af = 5U;                  c.level = 0U;
    }
    else
    {
        c.mode = GPIO_BM_MODE_AF;     c.otype = GPIO_BM_OTYPE_PP; c.speed = GPIO_BM_SPEED_LOW;
        c.pull = GPIO_BM_PULL_UP;     c.af = 5U;                  c.level = 0U;
    }
    return c;
}

static bool field_valid_other(const GstFields_t *f)
{
    /* Each field of the other core's pin must be its A value or its B value. */
    return ((f->mode  == 1U) || (f->mode  == 2U)) &&
           ((f->otype == 1U) || (f->otype == 0U)) &&
           ((f->speed == 3U) || (f->speed == 0U)) &&
           ((f->pull  == 2U) || (f->pull  == 1U)) &&
           (f->af == 5U);
}

static bool own_matches(const GstPinReq_t *c, const GstFields_t *f)
{
    return (f->mode == c->mode) && (f->otype == c->otype) && (f->speed == c->speed) &&
           (f->pull == c->pull) && (f->af == c->af);
}

/* The old code: read-modify-write of each register, no protection, in gpio_bm's order. */
void gst_raw_config(const GstPinReq_t *c)
{
    GstFields_t f = { c->mode, c->otype, c->speed, c->pull, c->af, c->level };
    fields_force(c->port, c->pin, &f);
}

static void hal_init_of(const GstPinReq_t *c)
{
    const GPIO_InitTypeDef init = { .Pin = 1UL << c->pin, .Mode = hal_mode_of(c->mode, c->otype),
                                    .Pull = c->pull, .Speed = c->speed, .Alternate = c->af };
    HAL_GPIO_Init(c->port, &init);
}

void gst_race_prepare(void)
{
    const GstPinReq_t a7 = gst_race_cfg(GST_RACE_PIN_CM7, 0U);
    const GstPinReq_t a4 = gst_race_cfg(GST_RACE_PIN_CM4, 0U);
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    (void)GPIO_BM_EnablePortClock(GST_RACE_PORT);
    gst_raw_config(&a7);
    gst_raw_config(&a4);
    __set_PRIMASK(pm);
}

void gst_b1_run(GstB1Result_t *r)
{
    const uint32_t pin = own_pin();

    for (uint32_t hook = 0U; hook < 2U; hook++)
    {
        GPIO_BM_SetSharedLock((hook != 0U) ? &g_shared_gpio_lock : NULL);
        for (uint32_t t = 0U; t < (uint32_t)GST_TIER_COUNT; t++)
        {
            uint32_t mn = 0xFFFFFFFFUL;
            uint32_t mx = 0U;
            for (uint32_t i = 0U; i < 1000U; i++)
            {
                const GstPinReq_t c = gst_race_cfg(pin, i & 1U);
                uint32_t pm = __get_PRIMASK();
                __disable_irq();
                const uint32_t t0 = DWT->CYCCNT;
                (void)gst_apply((GstTier_t)t, &c);
                const uint32_t dt = DWT->CYCCNT - t0;
                __set_PRIMASK(pm);
                mn = (dt < mn) ? dt : mn;
                mx = (dt > mx) ? dt : mx;
            }
            if (hook != 0U) { r->hook_min[t] = mn;   r->hook_max[t] = mx; }
            else            { r->nohook_min[t] = mn; r->nohook_max[t] = mx; }
        }
    }
    Shared_GpioLockInstall();
}

/* ---- Counting shim around the production hook (T15) ----------------------------- */

static volatile uint32_t s_shim_ok;
static volatile uint32_t s_shim_hold_max;

static bool shim_lock(void *ctx)
{
    (void)ctx;
    if (!g_shared_gpio_lock.lock(NULL))
    {
        return false;
    }
    GST_AREA->locked_calls++;        /* shared, and only ever written inside the lock */
    s_shim_ok++;                     /* this core's private count                    */
    GST_AREA->hold_start[gst_core_index()] = DWT->CYCCNT;
    return true;
}

static void shim_unlock(void *ctx)
{
    (void)ctx;
    const uint32_t held = DWT->CYCCNT - GST_AREA->hold_start[gst_core_index()];
    if (held > s_shim_hold_max)
    {
        s_shim_hold_max = held;
    }
    g_shared_gpio_lock.unlock(NULL);
}

static const GpioBm_SharedLock_t k_shim = { shim_lock, shim_unlock, NULL };

void gst_shim_install(bool on)
{
    if (on)
    {
        GPIO_BM_SetSharedLock(&k_shim);
    }
    else
    {
        Shared_GpioLockInstall();
    }
}

/* R2: jitter, so each run of one image tests another interleaving. A local
   xorshift32 seeded from GST_AREA->race_seed (random per run, printed by CPU1),
   and a busy wait on this core's CYCCNT: delay_cycles() is CPU1's and static. */
static uint32_t race_rand(uint32_t *x)
{
    uint32_t v = *x;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    *x = v;
    return v;
}

static void race_wait(uint32_t cyc)
{
    const uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < cyc)
    {
    }
}

void gst_race_run(GstRace_t m, uint32_t iters, GstRaceResult_t *r)
{
    const uint32_t me    = gst_core_index();
    const uint32_t other = me ^ 1U;
    const uint32_t pin   = own_pin();
    const uint32_t opin  = other_pin();
    uint32_t       x     = GST_AREA->race_seed ^ (me + 1U);

    if (x == 0U)
    {
        x = 0x9E3779B9UL;            /* xorshift32 never leaves 0 */
    }

    memset(r, 0, sizeof(*r));
    s_shim_ok = 0U;
    s_shim_hold_max = 0U;

    race_wait(race_rand(&x) & 0x3FFU);   /* after go: 0..1023 cycles */

    GST_AREA->in_loop[me] = 1U;
    __DSB();
    const uint32_t t0 = DWT->CYCCNT;

    for (uint32_t i = 0U; i < iters; i++)
    {
        const GstPinReq_t c = gst_race_cfg(pin, i & 1U);
        uint32_t st = (uint32_t)GPIO_BM_OK;

        race_wait(race_rand(&x) & 0x3FU);    /* 0..63 cycles, outside any section */

        switch (m)
        {
        case GST_RACE_RAW:            gst_raw_config(&c); break;
        case GST_RACE_BM:             st = gst_apply(GST_TIER_BM, &c);  break;
        case GST_RACE_LL:             st = gst_apply(GST_TIER_LL, &c);  break;
        case GST_RACE_HAL:            st = gst_apply(GST_TIER_HAL, &c); break;
        case GST_RACE_HAL_IN_SECTION:
        {
            GpioBm_Section_t s;
            if (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK)
            {
                hal_init_of(&c);
                GPIO_BM_SectionExit(&s);
            }
            else
            {
                st = (uint32_t)GPIO_BM_ERR_BUSY;
            }
            break;
        }
        default:                      hal_init_of(&c); break;   /* GST_RACE_HAL_RAW */
        }

        if (st != (uint32_t)GPIO_BM_OK)
        {
            r->status_err++;
        }
        const GstFields_t mine = fields_read(GST_RACE_PORT, pin);
        const GstFields_t his  = fields_read(GST_RACE_PORT, opin);
        if (!own_matches(&c, &mine) || !field_valid_other(&his))
        {
            r->corrupt++;
        }
        if (GST_AREA->in_loop[other] != 0U)
        {
            r->overlap++;
        }
    }

    r->cycles = DWT->CYCCNT - t0;
    GST_AREA->in_loop[me] = 0U;
    __DSB();
    r->iters    = iters;
    r->locked   = s_shim_ok;
    r->hold_max = s_shim_hold_max;
}
