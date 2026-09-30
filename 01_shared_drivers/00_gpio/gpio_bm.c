/**
  ******************************************************************************
  * @file    gpio_bm.c
  * @brief   GPIO port helpers, bare-metal tier.
  ******************************************************************************
  */

#include "gpio_bm.h"
#include <stddef.h>

/* GPIOA..GPIOK are consecutive, 0x400 apart, and RCC_AHB4ENR carries
   GPIOAEN..GPIOKEN in bits 0..10 in the same order. Both facts come from
   RM0399; the static assertions below fail the build if a future CMSIS header
   ever breaks them. */
#define GPIO_BM_PORT_STRIDE     0x400UL
#define GPIO_BM_PORT_COUNT      11UL        /* GPIOA..GPIOK */
#define GPIO_BM_PIN_COUNT       16UL

_Static_assert((GPIOK_BASE - GPIOA_BASE) == ((GPIO_BM_PORT_COUNT - 1UL) * GPIO_BM_PORT_STRIDE),
               "GPIOA..GPIOK no longer span 10 strides of 0x400: "
               "GPIO_BM_EnablePortClock() derives its RCC bit from that stride");
_Static_assert((RCC_AHB4ENR_GPIOAEN_Pos == 0U) &&
               (RCC_AHB4ENR_GPIOKEN_Pos == (GPIO_BM_PORT_COUNT - 1UL)),
               "GPIOAEN..GPIOKEN are no longer bits 0..10 of RCC_AHB4ENR");

/* PA13/PA14 carry SWDIO/SWCLK after reset (RM0399 12.3.2): a fact of the
   silicon, not of any board. */
#define GPIO_BM_SWD_PIN_MASK    ((1UL << 13U) | (1UL << 14U))

/* ---- Per-core state (C7): .data = 0, .bss = these two words ------------------ */
/* Each core links its own copy of this file, so each core has its own hook and
   its own nesting counter. Only the owning core touches them, always with
   PRIMASK set, so they need no lock of their own. */
static const GpioBm_SharedLock_t *s_shared_lock;
static uint32_t                   s_section_depth;

/* ---- Validation (C6): pure functions, no peripheral access ------------------- */

static bool port_index(const GPIO_TypeDef *port, uint32_t *idx)
{
    uint32_t address;

    if (port == NULL)
    {
        return false;
    }

    address = (uint32_t)port;

    if ((address < GPIOA_BASE) || (address > GPIOK_BASE))
    {
        return false;
    }

    /* G4: GPIOA + 4 is not port A. Only exact port base addresses count. */
    if (((address - GPIOA_BASE) % GPIO_BM_PORT_STRIDE) != 0UL)
    {
        return false;
    }

    *idx = (address - GPIOA_BASE) / GPIO_BM_PORT_STRIDE;
    return true;
}

static bool is_swd_pin(const GPIO_TypeDef *port, uint32_t pin_idx)
{
    return ((uint32_t)port == GPIOA_BASE) &&
           (((1UL << pin_idx) & GPIO_BM_SWD_PIN_MASK) != 0UL);
}

/* Order fixed by the contract: NULL, port, pin, fields, SWD pin. */
static GpioBm_Status_t validate_config(const GpioBm_PinConfig_t *cfg)
{
    uint32_t idx;

    if (cfg == NULL)
    {
        return GPIO_BM_ERR_PARAM;
    }
    if (!port_index(cfg->port, &idx))
    {
        return GPIO_BM_ERR_PARAM;
    }
    if (cfg->pin_idx >= GPIO_BM_PIN_COUNT)
    {
        return GPIO_BM_ERR_PARAM;
    }

    if (cfg->mode > GPIO_BM_MODE_ANALOG)
    {
        return GPIO_BM_ERR_PARAM;
    }
    /* PUPDR = 11 is reserved (RM0399 12.4.4, G3). */
    if (cfg->pull > GPIO_BM_PULL_DOWN)
    {
        return GPIO_BM_ERR_PARAM;
    }
    if ((cfg->mode == GPIO_BM_MODE_ANALOG) && (cfg->pull != GPIO_BM_PULL_NONE))
    {
        return GPIO_BM_ERR_PARAM;
    }
    if ((cfg->mode == GPIO_BM_MODE_OUTPUT) || (cfg->mode == GPIO_BM_MODE_AF))
    {
        if ((cfg->otype > GPIO_BM_OTYPE_OD) || (cfg->speed > GPIO_BM_SPEED_VERY_HIGH))
        {
            return GPIO_BM_ERR_PARAM;
        }
    }
    if ((cfg->mode == GPIO_BM_MODE_AF) && (cfg->af > 0xFUL))
    {
        return GPIO_BM_ERR_PARAM;
    }
    if ((cfg->mode == GPIO_BM_MODE_OUTPUT) && (cfg->initial_level > 1UL))
    {
        return GPIO_BM_ERR_PARAM;
    }

    /* G9: reconfiguring PA13/PA14 leaves the board without SWD. */
    if (is_swd_pin(cfg->port, cfg->pin_idx) && !cfg->allow_debug_pins)
    {
        return GPIO_BM_ERR_DEBUG_PIN;
    }

    return GPIO_BM_OK;
}

/* ---- Existing API ------------------------------------------------------------- */

bool GPIO_BM_EnablePortClock(GPIO_TypeDef *port)
{
    uint32_t port_idx;

    if (!port_index(port, &port_idx))
    {
        return false;
    }

    /* Reads like a cross-core race and is not one. RCC_AHB4ENR is at offset
       0xE0, inside the bank RM0399 9.7.1 calls "registers mapped at the same
       address": the hardware routes the access by core_id, so CPU1 lands in the
       CPU1 bank and CPU2 in the CPU2 bank. Different storage, and a peripheral
       is clocked when either bank has its bit set - which is why ST's own
       __HAL_RCC_*_CLK_ENABLE() macros carry no CORE_CM4 variant.
       Only the registers below 0xD0 (RCC_CR, the PLLs, the RSTRs) are shared by
       both cores. Those are CPU1's by the boot contract, and anything both
       cores must write needs a hardware semaphore - CLAUDE.md rule D.
       That is also why this function takes no section (C10, rule 3). */
    RCC->AHB4ENR |= (1UL << port_idx);

    /* Read back: the RCC enable takes a couple of cycles to land, and a write
       to the port's registers before it does is simply dropped. */
    volatile uint32_t dummy = RCC->AHB4ENR;
    (void)dummy;

    return true;
}

/* G3, C2: both keep their signature and their bool, and every call the
   consumers make today (uart: AF7, PULL_UP, VERY_HIGH; adc: analog) is still
   accepted and leaves the same bits. They are wrappers of GPIO_BM_ConfigPin;
   they set allow_debug_pins because G9 applies to the new API only - the
   legacy calls accepted PA13/PA14 before and still do. */
bool GPIO_BM_ConfigAlternate(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t af, uint32_t pull, uint32_t speed)
{
    const GpioBm_PinConfig_t cfg = {
        .port = port, .pin_idx = pin_idx, .mode = GPIO_BM_MODE_AF,
        .otype = GPIO_BM_OTYPE_PP, .speed = speed, .pull = pull, .af = af,
        .initial_level = 0U, .allow_debug_pins = true,
    };
    return GPIO_BM_ConfigPin(&cfg) == GPIO_BM_OK;
}

bool GPIO_BM_ConfigAnalog(GPIO_TypeDef *port, uint32_t pin_idx)
{
    const GpioBm_PinConfig_t cfg = {
        .port = port, .pin_idx = pin_idx, .mode = GPIO_BM_MODE_ANALOG,
        .otype = GPIO_BM_OTYPE_PP, .speed = GPIO_BM_SPEED_LOW, .pull = GPIO_BM_PULL_NONE,
        .af = 0U, .initial_level = 0U, .allow_debug_pins = true,
    };
    return GPIO_BM_ConfigPin(&cfg) == GPIO_BM_OK;
}

/* ---- Section and hook (C10, rule 2) ------------------------------------------- */

void GPIO_BM_SetSharedLock(const GpioBm_SharedLock_t *lock)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    s_shared_lock = lock;
    __set_PRIMASK(primask);
}

GpioBm_Status_t GPIO_BM_SectionEnter(GpioBm_Section_t *s)
{
    uint32_t primask;

    if (s == NULL)
    {
        return GPIO_BM_ERR_PARAM;
    }

    /* PRIMASK first, the cross-core lock second: while the lock is held no
       ISR of this core can run, so nothing on this core can try to take it
       again and see it as busy. */
    primask = __get_PRIMASK();
    __disable_irq();
    __DSB();
    __ISB();

    s->primask     = primask;
    s->took_shared = 0U;
    s_section_depth++;

    if (s_section_depth == 1UL)
    {
        const GpioBm_SharedLock_t *lk = s_shared_lock;
        if ((lk != NULL) && (lk->lock != NULL))
        {
            if (!lk->lock(lk->ctx))
            {
                s_section_depth--;
                __set_PRIMASK(primask);
                return GPIO_BM_ERR_BUSY;
            }
            s->took_shared = 1U;
        }
    }

    return GPIO_BM_OK;
}

void GPIO_BM_SectionExit(const GpioBm_Section_t *s)
{
    if (s == NULL)
    {
        return;
    }

    if (s_section_depth > 0UL)
    {
        s_section_depth--;
    }

    if (s->took_shared != 0U)
    {
        const GpioBm_SharedLock_t *lk = s_shared_lock;
        /* Every GPIO write of the section must have landed before the other
           core can get in. */
        __DSB();
        if ((lk != NULL) && (lk->unlock != NULL))
        {
            lk->unlock(lk->ctx);
        }
    }

    /* Restore, never force: interrupts masked on entry stay masked. */
    __set_PRIMASK(s->primask);
}

/* ---- LCKR helpers -------------------------------------------------------------- */

static bool pin_locked(const GPIO_TypeDef *port, uint32_t pin_idx)
{
    uint32_t lckr = port->LCKR;
    /* LCKy only freezes the pin once the LCKK sequence has run (RM0399 12.4.8). */
    return ((lckr & GPIO_LCKR_LCKK) != 0UL) && ((lckr & (1UL << pin_idx)) != 0UL);
}

/* ---- Configuration -------------------------------------------------------------- */

GpioBm_Status_t GPIO_BM_ConfigPin(const GpioBm_PinConfig_t *cfg)
{
    GpioBm_Section_t section;
    GpioBm_Status_t  st = validate_config(cfg);

    if (st != GPIO_BM_OK)
    {
        return st;
    }

    GPIO_TypeDef  *port  = cfg->port;
    const uint32_t pin   = cfg->pin_idx;
    const uint32_t pos2  = pin * 2U;
    const uint32_t afr_i = pin >> 3U;
    const uint32_t pos4  = (pin & 7U) * 4U;
    const bool     drive = (cfg->mode == GPIO_BM_MODE_OUTPUT) || (cfg->mode == GPIO_BM_MODE_AF);

    (void)GPIO_BM_EnablePortClock(port);   /* validated above: cannot fail */

    st = GPIO_BM_SectionEnter(&section);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    /* C12: a locked pin ignores every write; say so instead of writing. */
    if (pin_locked(port, pin))
    {
        GPIO_BM_SectionExit(&section);
        return GPIO_BM_ERR_LOCKED;
    }

    uint32_t otyper  = port->OTYPER;
    uint32_t ospeedr = port->OSPEEDR;
    uint32_t pupdr   = port->PUPDR;
    uint32_t afr     = port->AFR[afr_i];
    uint32_t moder   = port->MODER;

    /* C11, rule 4: level, electrical fields, AF, and MODER last, so the pin
       never drives a wrong level or a wrong function in between. */
    if (cfg->mode == GPIO_BM_MODE_OUTPUT)
    {
        port->BSRR = (cfg->initial_level != 0UL) ? (1UL << pin) : (1UL << (pin + 16U));
    }
    if (drive)
    {
        otyper  = (otyper & ~(1UL << pin)) | (cfg->otype << pin);
        ospeedr = (ospeedr & ~(3UL << pos2)) | (cfg->speed << pos2);
        port->OTYPER  = otyper;
        port->OSPEEDR = ospeedr;
    }
    pupdr = (pupdr & ~(3UL << pos2)) | (cfg->pull << pos2);
    port->PUPDR = pupdr;
    if (cfg->mode == GPIO_BM_MODE_AF)
    {
        afr = (afr & ~(0xFUL << pos4)) | (cfg->af << pos4);
        port->AFR[afr_i] = afr;
    }
    moder = (moder & ~(3UL << pos2)) | (cfg->mode << pos2);
    port->MODER = moder;

    /* C12: read-back of the five fields of this pin. */
    const bool ok =
        (((port->MODER   >> pos2) & 3UL)   == ((moder   >> pos2) & 3UL))   &&
        (((port->OTYPER  >> pin)  & 1UL)   == ((otyper  >> pin)  & 1UL))   &&
        (((port->OSPEEDR >> pos2) & 3UL)   == ((ospeedr >> pos2) & 3UL))   &&
        (((port->PUPDR   >> pos2) & 3UL)   == ((pupdr   >> pos2) & 3UL))   &&
        (((port->AFR[afr_i] >> pos4) & 0xFUL) == ((afr >> pos4) & 0xFUL));

    GPIO_BM_SectionExit(&section);
    return ok ? GPIO_BM_OK : GPIO_BM_ERR_VERIFY;
}

GpioBm_Status_t GPIO_BM_ConfigOutput(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t otype,
                                     uint32_t speed, uint32_t pull, uint32_t initial_level)
{
    const GpioBm_PinConfig_t cfg = {
        .port = port, .pin_idx = pin_idx, .mode = GPIO_BM_MODE_OUTPUT,
        .otype = otype, .speed = speed, .pull = pull, .af = 0U,
        .initial_level = initial_level, .allow_debug_pins = false,
    };
    return GPIO_BM_ConfigPin(&cfg);
}

GpioBm_Status_t GPIO_BM_ConfigInput(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t pull)
{
    const GpioBm_PinConfig_t cfg = {
        .port = port, .pin_idx = pin_idx, .mode = GPIO_BM_MODE_INPUT,
        .otype = GPIO_BM_OTYPE_PP, .speed = GPIO_BM_SPEED_LOW, .pull = pull, .af = 0U,
        .initial_level = 0U, .allow_debug_pins = false,
    };
    return GPIO_BM_ConfigPin(&cfg);
}

GpioBm_Status_t GPIO_BM_ConfigAlternateEx(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t af,
                                          uint32_t otype, uint32_t pull, uint32_t speed)
{
    const GpioBm_PinConfig_t cfg = {
        .port = port, .pin_idx = pin_idx, .mode = GPIO_BM_MODE_AF,
        .otype = otype, .speed = speed, .pull = pull, .af = af,
        .initial_level = 0U, .allow_debug_pins = false,
    };
    return GPIO_BM_ConfigPin(&cfg);
}

GpioBm_Status_t GPIO_BM_DeinitPin(GPIO_TypeDef *port, uint32_t pin_idx)
{
    GpioBm_Section_t section;
    GpioBm_Status_t  st;
    uint32_t         idx;

    if (!port_index(port, &idx) || (pin_idx >= GPIO_BM_PIN_COUNT))
    {
        return GPIO_BM_ERR_PARAM;
    }
    if (is_swd_pin(port, pin_idx))
    {
        return GPIO_BM_ERR_DEBUG_PIN;
    }

    const uint32_t pos2  = pin_idx * 2U;
    const uint32_t afr_i = pin_idx >> 3U;
    const uint32_t pos4  = (pin_idx & 7U) * 4U;

    (void)GPIO_BM_EnablePortClock(port);

    st = GPIO_BM_SectionEnter(&section);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    if (pin_locked(port, pin_idx))
    {
        GPIO_BM_SectionExit(&section);
        return GPIO_BM_ERR_LOCKED;
    }

    /* C11: MODER = analog first, so the pin stops driving before anything
       else changes; then the reset values of the other fields. */
    port->MODER    = port->MODER | (3UL << pos2);
    port->OTYPER   = port->OTYPER & ~(1UL << pin_idx);
    port->OSPEEDR  = port->OSPEEDR & ~(3UL << pos2);
    port->PUPDR    = port->PUPDR & ~(3UL << pos2);
    port->AFR[afr_i] = port->AFR[afr_i] & ~(0xFUL << pos4);
    port->BSRR     = 1UL << (pin_idx + 16U);

    const bool ok =
        (((port->MODER   >> pos2) & 3UL)   == 3UL) &&
        (((port->OTYPER  >> pin_idx) & 1UL) == 0UL) &&
        (((port->OSPEEDR >> pos2) & 3UL)   == 0UL) &&
        (((port->PUPDR   >> pos2) & 3UL)   == 0UL) &&
        (((port->AFR[afr_i] >> pos4) & 0xFUL) == 0UL);

    GPIO_BM_SectionExit(&section);
    return ok ? GPIO_BM_OK : GPIO_BM_ERR_VERIFY;
}

GpioBm_Status_t GPIO_BM_ReadConfig(const GPIO_TypeDef *port, uint32_t pin_idx, GpioBm_PinConfig_t *out)
{
    uint32_t idx;

    if (out == NULL)
    {
        return GPIO_BM_ERR_PARAM;
    }
    if (!port_index(port, &idx) || (pin_idx >= GPIO_BM_PIN_COUNT))
    {
        return GPIO_BM_ERR_PARAM;
    }

    const uint32_t pos2 = pin_idx * 2U;

    out->port             = (GPIO_TypeDef *)port;
    out->pin_idx          = pin_idx;
    out->mode             = (port->MODER   >> pos2) & 3UL;
    out->otype            = (port->OTYPER  >> pin_idx) & 1UL;
    out->speed            = (port->OSPEEDR >> pos2) & 3UL;
    out->pull             = (port->PUPDR   >> pos2) & 3UL;
    out->af               = (port->AFR[pin_idx >> 3U] >> ((pin_idx & 7U) * 4U)) & 0xFUL;
    out->initial_level    = (port->ODR     >> pin_idx) & 1UL;
    out->allow_debug_pins = false;
    return GPIO_BM_OK;
}

/* ---- LCKR (rule 5) ------------------------------------------------------------ */

GpioBm_Status_t GPIO_BM_LockPins(GPIO_TypeDef *port, uint32_t mask)
{
    GpioBm_Section_t section;
    GpioBm_Status_t  st;
    uint32_t         idx;

    if (!port_index(port, &idx) || (mask == 0UL) || ((mask & ~0xFFFFUL) != 0UL))
    {
        return GPIO_BM_ERR_PARAM;
    }

    (void)GPIO_BM_EnablePortClock(port);

    st = GPIO_BM_SectionEnter(&section);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    uint32_t lckr = port->LCKR;
    if ((lckr & GPIO_LCKR_LCKK) != 0UL)
    {
        /* Already locked: LCKR is frozen until reset and the sequence would
           be ignored. Either the pins are among the locked ones or not. */
        GPIO_BM_SectionExit(&section);
        return ((lckr & mask) == mask) ? GPIO_BM_OK : GPIO_BM_ERR_LOCKED;
    }

    /* RM0399 12.4.8: the write sequence must not be interrupted and every
       access is a 32-bit word (LCKR is declared as a 32-bit volatile). */
    const uint32_t key = GPIO_LCKR_LCKK | mask;
    port->LCKR = key;
    port->LCKR = mask;
    port->LCKR = key;
    lckr = port->LCKR;
    (void)lckr;
    lckr = port->LCKR;

    GPIO_BM_SectionExit(&section);

    if (((lckr & GPIO_LCKR_LCKK) == 0UL) || ((lckr & mask) != mask))
    {
        return GPIO_BM_ERR_VERIFY;
    }
    return GPIO_BM_OK;
}

bool GPIO_BM_IsLocked(const GPIO_TypeDef *port, uint32_t pin_idx)
{
    uint32_t idx;

    if (!port_index(port, &idx) || (pin_idx >= GPIO_BM_PIN_COUNT))
    {
        return false;
    }
    return pin_locked(port, pin_idx);
}
