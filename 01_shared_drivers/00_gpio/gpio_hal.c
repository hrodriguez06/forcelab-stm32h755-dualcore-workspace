/**
  ******************************************************************************
  * @file    gpio_hal.c
  * @brief   GPIO pin driver, HAL tier.
  *
  *          HAL_GPIO_Init() and HAL_GPIO_LockPin() inside the gpio_bm section,
  *          with gpio_bm's validation in front. No state in RAM.
  ******************************************************************************
  */

#include "gpio_hal.h"
#include <stddef.h>

#if defined(HAL_GPIO_MODULE_ENABLED)

/* The HAL pull, speed and AF constants are the register field values, as in
   gpio_bm; the modes are not (they carry the output type in bit 4), so they
   are translated below. */
_Static_assert((GPIO_NOPULL == GPIO_BM_PULL_NONE) && (GPIO_PULLUP == GPIO_BM_PULL_UP) &&
               (GPIO_PULLDOWN == GPIO_BM_PULL_DOWN),
               "GPIO_NOPULL/PULLUP/PULLDOWN no longer equal the PUPDR field values");
_Static_assert((GPIO_SPEED_FREQ_LOW == GPIO_BM_SPEED_LOW) && (GPIO_SPEED_FREQ_MEDIUM == GPIO_BM_SPEED_MEDIUM) &&
               (GPIO_SPEED_FREQ_HIGH == GPIO_BM_SPEED_HIGH) && (GPIO_SPEED_FREQ_VERY_HIGH == GPIO_BM_SPEED_VERY_HIGH),
               "GPIO_SPEED_FREQ_* no longer equal the OSPEEDR field values");
_Static_assert((GPIO_PIN_RESET == 0) && (GPIO_PIN_SET == 1), "GPIO_PinState values changed");

#define GPIO_HAL_PIN_COUNT   16UL

/* HAL mode -> MODER value and output type. Only the six plain modes: every
   EXTI mode (GPIO_MODE_IT_*, GPIO_MODE_EVT_*) is out of this driver's scope. */
static bool hal_mode(uint32_t mode, uint32_t *moder, uint32_t *otype)
{
    switch (mode)
    {
    case GPIO_MODE_INPUT:     *moder = GPIO_BM_MODE_INPUT;  *otype = GPIO_BM_OTYPE_PP; return true;
    case GPIO_MODE_OUTPUT_PP: *moder = GPIO_BM_MODE_OUTPUT; *otype = GPIO_BM_OTYPE_PP; return true;
    case GPIO_MODE_OUTPUT_OD: *moder = GPIO_BM_MODE_OUTPUT; *otype = GPIO_BM_OTYPE_OD; return true;
    case GPIO_MODE_AF_PP:     *moder = GPIO_BM_MODE_AF;     *otype = GPIO_BM_OTYPE_PP; return true;
    case GPIO_MODE_AF_OD:     *moder = GPIO_BM_MODE_AF;     *otype = GPIO_BM_OTYPE_OD; return true;
    case GPIO_MODE_ANALOG:    *moder = GPIO_BM_MODE_ANALOG; *otype = GPIO_BM_OTYPE_PP; return true;
    default:                  return false;
    }
}

/* The request as gpio_bm sees it, validated by gpio_bm. `bm` is filled on OK. */
static GpioBm_Status_t check(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t mode, uint32_t pull,
                             uint32_t speed, uint32_t alternate, uint32_t level, bool allow_debug,
                             GpioBm_PinConfig_t *bm)
{
    uint32_t moder;
    uint32_t otype;

    /* Port and pin first, as in gpio_bm: a bad port with a bad mode is ERR_PARAM
       either way, but nothing below may be read before these two are known good. */
    if (!GPIO_BM_IsValidPort(port) || (pin_idx >= GPIO_HAL_PIN_COUNT))
    {
        return GPIO_BM_ERR_PARAM;
    }
    if (!hal_mode(mode, &moder, &otype))
    {
        return GPIO_BM_ERR_PARAM;
    }

    bm->port             = port;
    bm->pin_idx          = pin_idx;
    bm->mode             = moder;
    bm->otype            = otype;
    bm->speed            = speed;
    bm->pull             = pull;
    bm->af               = alternate;
    bm->initial_level    = level;
    bm->allow_debug_pins = allow_debug;
    return GPIO_BM_CheckConfig(bm);
}

/* One pin, already validated. `write_level`: GPIO_HAL_ConfigPin sets the output
   level; GPIO_HAL_Init keeps HAL_GPIO_Init's semantics and leaves ODR alone. */
static GpioBm_Status_t apply(const GpioBm_PinConfig_t *bm, uint32_t hal_mode_value, bool write_level)
{
    GpioBm_Section_t   section;
    GpioBm_PinConfig_t before;
    GpioBm_PinConfig_t after;
    GpioBm_Status_t    st;
    GPIO_TypeDef      *port  = bm->port;
    const uint32_t     pin   = bm->pin_idx;
    const bool         drive = (bm->mode == GPIO_BM_MODE_OUTPUT) || (bm->mode == GPIO_BM_MODE_AF);

    (void)GPIO_BM_EnablePortClock(port);   /* validated: cannot fail */

    st = GPIO_BM_SectionEnter(&section);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    /* C12: HAL_GPIO_Init would write a locked pin and change nothing. */
    if (GPIO_BM_IsLocked(port, pin))
    {
        GPIO_BM_SectionExit(&section);
        return GPIO_BM_ERR_LOCKED;
    }

    (void)GPIO_BM_ReadConfig(port, pin, &before);

    /* C11: the level first. HAL_GPIO_Init then writes OSPEEDR, OTYPER, PUPDR,
       AFR and MODER last. */
    if (write_level && (bm->mode == GPIO_BM_MODE_OUTPUT))
    {
        port->BSRR = (bm->initial_level != 0UL) ? (1UL << pin) : (1UL << (pin + 16U));
    }

    GPIO_InitTypeDef init = {
        .Pin       = 1UL << pin,
        .Mode      = hal_mode_value,
        .Pull      = bm->pull,
        .Speed     = bm->speed,
        .Alternate = bm->af,
    };
    HAL_GPIO_Init(port, &init);

    /* C12: what HAL_GPIO_Init writes for this mode, and what it leaves. */
    (void)GPIO_BM_ReadConfig(port, pin, &after);
    const bool ok =
        (after.mode  == bm->mode) &&
        (after.pull  == bm->pull) &&
        (after.otype == (drive ? bm->otype : before.otype)) &&
        (after.speed == (drive ? bm->speed : before.speed)) &&
        (after.af    == ((bm->mode == GPIO_BM_MODE_AF) ? bm->af : before.af));

    GPIO_BM_SectionExit(&section);
    return ok ? GPIO_BM_OK : GPIO_BM_ERR_VERIFY;
}

HAL_StatusTypeDef GPIO_HAL_ConfigPin(const GpioHal_PinConfig_t *cfg)
{
    GpioBm_PinConfig_t bm;
    GpioBm_Status_t    st;

    if (cfg == NULL)
    {
        return HAL_ERROR;
    }

    st = check(cfg->port, cfg->pin_idx, cfg->mode, cfg->pull, cfg->speed, cfg->alternate,
               (uint32_t)cfg->initial_level, cfg->allow_debug_pins, &bm);
    if (st != GPIO_BM_OK)
    {
        return GPIO_HAL_Status(st);
    }
    return GPIO_HAL_Status(apply(&bm, cfg->mode, true));
}

HAL_StatusTypeDef GPIO_HAL_Init(GPIO_TypeDef *port, const GPIO_InitTypeDef *init)
{
    GpioBm_PinConfig_t bm;
    GpioBm_Status_t    st;
    uint32_t           pin;

    if ((init == NULL) || (init->Pin == 0UL) || ((init->Pin & ~0xFFFFUL) != 0UL))
    {
        return HAL_ERROR;
    }

    /* The whole mask is validated, and checked for LCKR, before the first write. */
    for (pin = 0U; pin < GPIO_HAL_PIN_COUNT; pin++)
    {
        if ((init->Pin & (1UL << pin)) == 0UL)
        {
            continue;
        }
        st = check(port, pin, init->Mode, init->Pull, init->Speed, init->Alternate, 0U, false, &bm);
        if (st != GPIO_BM_OK)
        {
            return GPIO_HAL_Status(st);
        }
    }
    for (pin = 0U; pin < GPIO_HAL_PIN_COUNT; pin++)
    {
        if (((init->Pin & (1UL << pin)) != 0UL) && GPIO_BM_IsLocked(port, pin))
        {
            return HAL_ERROR;
        }
    }

    for (pin = 0U; pin < GPIO_HAL_PIN_COUNT; pin++)
    {
        if ((init->Pin & (1UL << pin)) == 0UL)
        {
            continue;
        }
        (void)check(port, pin, init->Mode, init->Pull, init->Speed, init->Alternate, 0U, false, &bm);
        st = apply(&bm, init->Mode, false);
        if (st != GPIO_BM_OK)
        {
            return GPIO_HAL_Status(st);
        }
    }
    return HAL_OK;
}

HAL_StatusTypeDef GPIO_HAL_DeinitPin(GPIO_TypeDef *port, uint32_t pin_idx)
{
    return GPIO_HAL_Status(GPIO_BM_DeinitPin(port, pin_idx));
}

HAL_StatusTypeDef GPIO_HAL_LockPins(GPIO_TypeDef *port, uint32_t mask)
{
    GpioBm_Section_t section;
    GpioBm_Status_t  st;
    uint32_t         lckr;

    if (!GPIO_BM_IsValidPort(port) || (mask == 0UL) || ((mask & ~0xFFFFUL) != 0UL))
    {
        return HAL_ERROR;
    }

    (void)GPIO_BM_EnablePortClock(port);

    st = GPIO_BM_SectionEnter(&section);
    if (st != GPIO_BM_OK)
    {
        return GPIO_HAL_Status(st);
    }

    lckr = port->LCKR;
    if ((lckr & GPIO_LCKR_LCKK) != 0UL)
    {
        /* Frozen until reset. HAL_GPIO_LockPin would "succeed" here for any mask. */
        GPIO_BM_SectionExit(&section);
        return ((lckr & mask) == mask) ? HAL_OK : HAL_ERROR;
    }

    const HAL_StatusTypeDef hs = HAL_GPIO_LockPin(port, (uint16_t)mask);
    lckr = port->LCKR;

    GPIO_BM_SectionExit(&section);

    if ((hs != HAL_OK) || ((lckr & GPIO_LCKR_LCKK) == 0UL) || ((lckr & mask) != mask))
    {
        return HAL_ERROR;
    }
    return HAL_OK;
}

#endif /* HAL_GPIO_MODULE_ENABLED */
