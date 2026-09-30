/**
  ******************************************************************************
  * @file    gpio_ll.c
  * @brief   GPIO pin driver, Low-Layer tier.
  *
  *          Register access through the LL inlines of stm32h7xx_ll_gpio.h;
  *          validation, port clock and section from gpio_bm. No state in RAM.
  ******************************************************************************
  */

#include "gpio_ll.h"
#include <stddef.h>

/* The LL constants are the register field values, the same numbers gpio_bm
   uses. That is what lets the LL tier hand its request to GPIO_BM_CheckConfig
   unchanged; if a future LL header ever breaks it, the build stops here. */
_Static_assert((LL_GPIO_MODE_INPUT == GPIO_BM_MODE_INPUT) && (LL_GPIO_MODE_OUTPUT == GPIO_BM_MODE_OUTPUT) &&
               (LL_GPIO_MODE_ALTERNATE == GPIO_BM_MODE_AF) && (LL_GPIO_MODE_ANALOG == GPIO_BM_MODE_ANALOG),
               "LL_GPIO_MODE_* no longer equal the MODER field values");
_Static_assert((LL_GPIO_OUTPUT_PUSHPULL == GPIO_BM_OTYPE_PP) && (LL_GPIO_OUTPUT_OPENDRAIN == GPIO_BM_OTYPE_OD),
               "LL_GPIO_OUTPUT_* no longer equal the OTYPER field values");
_Static_assert((LL_GPIO_SPEED_FREQ_LOW == GPIO_BM_SPEED_LOW) && (LL_GPIO_SPEED_FREQ_MEDIUM == GPIO_BM_SPEED_MEDIUM) &&
               (LL_GPIO_SPEED_FREQ_HIGH == GPIO_BM_SPEED_HIGH) && (LL_GPIO_SPEED_FREQ_VERY_HIGH == GPIO_BM_SPEED_VERY_HIGH),
               "LL_GPIO_SPEED_FREQ_* no longer equal the OSPEEDR field values");
_Static_assert((LL_GPIO_PULL_NO == GPIO_BM_PULL_NONE) && (LL_GPIO_PULL_UP == GPIO_BM_PULL_UP) &&
               (LL_GPIO_PULL_DOWN == GPIO_BM_PULL_DOWN),
               "LL_GPIO_PULL_* no longer equal the PUPDR field values");
_Static_assert((LL_GPIO_AF_0 == 0U) && (LL_GPIO_AF_15 == 15U),
               "LL_GPIO_AF_* no longer equal the AFR field values");

#define GPIO_LL_PIN_COUNT   16UL

static GpioBm_PinConfig_t to_bm(const GpioLL_PinConfig_t *cfg)
{
    const GpioBm_PinConfig_t bm = {
        .port = cfg->port, .pin_idx = cfg->pin_idx, .mode = cfg->mode,
        .otype = cfg->output_type, .speed = cfg->speed, .pull = cfg->pull,
        .af = cfg->alternate, .initial_level = cfg->initial_level,
        .allow_debug_pins = cfg->allow_debug_pins,
    };
    return bm;
}

static GpioBm_Status_t check(const GpioLL_PinConfig_t *cfg)
{
    if (cfg == NULL)
    {
        return GPIO_BM_ERR_PARAM;
    }
    const GpioBm_PinConfig_t bm = to_bm(cfg);
    return GPIO_BM_CheckConfig(&bm);
}

static uint32_t get_af(const GPIO_TypeDef *port, uint32_t mask, uint32_t pin)
{
    return (pin < 8U) ? LL_GPIO_GetAFPin_0_7(port, mask) : LL_GPIO_GetAFPin_8_15(port, mask);
}

static void set_af(GPIO_TypeDef *port, uint32_t mask, uint32_t pin, uint32_t af)
{
    if (pin < 8U)
    {
        LL_GPIO_SetAFPin_0_7(port, mask, af);
    }
    else
    {
        LL_GPIO_SetAFPin_8_15(port, mask, af);
    }
}

/* LCKy only freezes the pin once LCKK is set (RM0399 12.4.8). LL_GPIO_IsPinLocked
   alone looks at LCKy and would call a pin locked before the sequence ran. */
static bool pin_locked(const GPIO_TypeDef *port, uint32_t mask)
{
    return (LL_GPIO_IsAnyPinLocked(port) != 0UL) && (LL_GPIO_IsPinLocked(port, mask) != 0UL);
}

/* Steps 2 and 3 of GPIO_BM_ConfigPin, for an entry already validated. */
static GpioBm_Status_t apply(const GpioLL_PinConfig_t *cfg)
{
    GpioBm_Section_t section;
    GPIO_TypeDef    *port  = cfg->port;
    const uint32_t   pin   = cfg->pin_idx;
    const uint32_t   mask  = 1UL << pin;
    const bool       drive = (cfg->mode == LL_GPIO_MODE_OUTPUT) || (cfg->mode == LL_GPIO_MODE_ALTERNATE);
    GpioBm_Status_t  st;

    (void)GPIO_BM_EnablePortClock(port);   /* validated: cannot fail */

    st = GPIO_BM_SectionEnter(&section);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    /* C12: a locked pin ignores every write; say so instead of writing. */
    if (pin_locked(port, mask))
    {
        GPIO_BM_SectionExit(&section);
        return GPIO_BM_ERR_LOCKED;
    }

    /* What the pin must read back: the fields written, and the ones this call
       leaves alone as they were inside the section. */
    const uint32_t otype = drive ? cfg->output_type : LL_GPIO_GetPinOutputType(port, mask);
    const uint32_t speed = drive ? cfg->speed : LL_GPIO_GetPinSpeed(port, mask);
    const uint32_t af    = (cfg->mode == LL_GPIO_MODE_ALTERNATE) ? cfg->alternate : get_af(port, mask, pin);

    /* C11: level, electrical fields, AF, and MODER last. */
    if (cfg->mode == LL_GPIO_MODE_OUTPUT)
    {
        if (cfg->initial_level != 0UL)
        {
            LL_GPIO_SetOutputPin(port, mask);
        }
        else
        {
            LL_GPIO_ResetOutputPin(port, mask);
        }
    }
    if (drive)
    {
        LL_GPIO_SetPinOutputType(port, mask, cfg->output_type);
        LL_GPIO_SetPinSpeed(port, mask, cfg->speed);
    }
    LL_GPIO_SetPinPull(port, mask, cfg->pull);
    if (cfg->mode == LL_GPIO_MODE_ALTERNATE)
    {
        set_af(port, mask, pin, cfg->alternate);
    }
    LL_GPIO_SetPinMode(port, mask, cfg->mode);

    /* C12: read-back of the five fields. */
    const bool ok =
        (LL_GPIO_GetPinMode(port, mask)       == cfg->mode) &&
        (LL_GPIO_GetPinOutputType(port, mask) == otype)     &&
        (LL_GPIO_GetPinSpeed(port, mask)      == speed)     &&
        (LL_GPIO_GetPinPull(port, mask)       == cfg->pull) &&
        (get_af(port, mask, pin)              == af);

    GPIO_BM_SectionExit(&section);
    return ok ? GPIO_BM_OK : GPIO_BM_ERR_VERIFY;
}

GpioBm_Status_t GPIO_LL_ConfigPin(const GpioLL_PinConfig_t *cfg)
{
    GpioBm_Status_t st = check(cfg);
    if (st != GPIO_BM_OK)
    {
        return st;
    }
    return apply(cfg);
}

GpioBm_Status_t GPIO_LL_ConfigPins(const GpioLL_PinConfig_t *cfgs, uint32_t count, uint32_t *done)
{
    uint32_t i;

    if (done != NULL)
    {
        *done = 0U;
    }
    if ((cfgs == NULL) || (count == 0U))
    {
        return GPIO_BM_ERR_PARAM;
    }

    /* All or nothing for the parameters: nothing is touched until every entry passes. */
    for (i = 0U; i < count; i++)
    {
        GpioBm_Status_t st = check(&cfgs[i]);
        if (st != GPIO_BM_OK)
        {
            return st;
        }
    }

    for (i = 0U; i < count; i++)
    {
        GpioBm_Status_t st = apply(&cfgs[i]);
        if (st != GPIO_BM_OK)
        {
            return st;
        }
        if (done != NULL)
        {
            *done = i + 1U;
        }
    }
    return GPIO_BM_OK;
}

GpioBm_Status_t GPIO_LL_DeinitPin(GPIO_TypeDef *port, uint32_t pin_idx)
{
    GpioBm_Section_t section;
    GpioBm_Status_t  st;

    /* The checks of GPIO_BM_DeinitPin, in its order (port and pin, then SWD),
       taken from gpio_bm so the SWD rule has one copy: an analog request
       without pull and without allow_debug_pins fails exactly on those. */
    const GpioBm_PinConfig_t req = {
        .port = port, .pin_idx = pin_idx, .mode = GPIO_BM_MODE_ANALOG,
        .otype = GPIO_BM_OTYPE_PP, .speed = GPIO_BM_SPEED_LOW, .pull = GPIO_BM_PULL_NONE,
        .af = 0U, .initial_level = 0U, .allow_debug_pins = false,
    };
    st = GPIO_BM_CheckConfig(&req);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    const uint32_t mask = 1UL << pin_idx;

    (void)GPIO_BM_EnablePortClock(port);

    st = GPIO_BM_SectionEnter(&section);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    if (pin_locked(port, mask))
    {
        GPIO_BM_SectionExit(&section);
        return GPIO_BM_ERR_LOCKED;
    }

    /* C11: analog first, so the pin stops driving before anything else changes. */
    LL_GPIO_SetPinMode(port, mask, LL_GPIO_MODE_ANALOG);
    LL_GPIO_SetPinOutputType(port, mask, LL_GPIO_OUTPUT_PUSHPULL);
    LL_GPIO_SetPinSpeed(port, mask, LL_GPIO_SPEED_FREQ_LOW);
    LL_GPIO_SetPinPull(port, mask, LL_GPIO_PULL_NO);
    set_af(port, mask, pin_idx, LL_GPIO_AF_0);
    LL_GPIO_ResetOutputPin(port, mask);

    const bool ok =
        (LL_GPIO_GetPinMode(port, mask)       == LL_GPIO_MODE_ANALOG)     &&
        (LL_GPIO_GetPinOutputType(port, mask) == LL_GPIO_OUTPUT_PUSHPULL) &&
        (LL_GPIO_GetPinSpeed(port, mask)      == LL_GPIO_SPEED_FREQ_LOW)  &&
        (LL_GPIO_GetPinPull(port, mask)       == LL_GPIO_PULL_NO)         &&
        (get_af(port, mask, pin_idx)          == LL_GPIO_AF_0);

    GPIO_BM_SectionExit(&section);
    return ok ? GPIO_BM_OK : GPIO_BM_ERR_VERIFY;
}

GpioBm_Status_t GPIO_LL_ReadConfig(const GPIO_TypeDef *port, uint32_t pin_idx, GpioLL_PinConfig_t *out)
{
    if (out == NULL)
    {
        return GPIO_BM_ERR_PARAM;
    }
    if (!GPIO_BM_IsValidPort(port) || (pin_idx >= GPIO_LL_PIN_COUNT))
    {
        return GPIO_BM_ERR_PARAM;
    }
    if (!GPIO_BM_IsPortClocked(port))   /* R1: same rule as GPIO_BM_ReadConfig */
    {
        return GPIO_BM_ERR_NO_CLOCK;
    }

    const uint32_t mask = 1UL << pin_idx;

    out->port             = (GPIO_TypeDef *)(uintptr_t)port;   /* the struct's field is non-const */
    out->pin_idx          = pin_idx;
    out->mode             = LL_GPIO_GetPinMode(port, mask);
    out->output_type      = LL_GPIO_GetPinOutputType(port, mask);
    out->speed            = LL_GPIO_GetPinSpeed(port, mask);
    out->pull             = LL_GPIO_GetPinPull(port, mask);
    out->alternate        = get_af(port, mask, pin_idx);
    out->initial_level    = LL_GPIO_IsOutputPinSet(port, mask);
    out->allow_debug_pins = false;
    return GPIO_BM_OK;
}

GpioBm_Status_t GPIO_LL_LockPins(GPIO_TypeDef *port, uint32_t mask)
{
    GpioBm_Section_t section;
    GpioBm_Status_t  st;
    uint32_t         lckr;

    if (!GPIO_BM_IsValidPort(port) || (mask == 0UL) || ((mask & ~0xFFFFUL) != 0UL))
    {
        return GPIO_BM_ERR_PARAM;
    }

    (void)GPIO_BM_EnablePortClock(port);

    st = GPIO_BM_SectionEnter(&section);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    const bool already = LL_GPIO_IsAnyPinLocked(port) != 0UL;
    if (already && (LL_GPIO_IsPinLocked(port, mask) != 0UL))
    {
        GPIO_BM_SectionExit(&section);   /* nothing to add */
        return GPIO_BM_OK;
    }

    /* As GPIO_BM_LockPins: on a locked port the locked pins stay in the
       sequence and the read-back decides. Three word writes and a read,
       uninterrupted (the section), RM0399 12.4.8. */
    LL_GPIO_LockPin(port, (already ? (READ_REG(port->LCKR) & 0xFFFFUL) : 0UL) | mask);
    lckr = READ_REG(port->LCKR);

    GPIO_BM_SectionExit(&section);

    if (((lckr & GPIO_LCKR_LCKK) == 0UL) || ((lckr & mask) != mask))
    {
        return GPIO_BM_ERR_VERIFY;
    }
    return GPIO_BM_OK;
}

bool GPIO_LL_IsLocked(const GPIO_TypeDef *port, uint32_t pin_idx)
{
    if (!GPIO_BM_IsValidPort(port) || (pin_idx >= GPIO_LL_PIN_COUNT))
    {
        return false;
    }
    return pin_locked(port, 1UL << pin_idx);
}
