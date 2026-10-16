/**
  ******************************************************************************
  * @file    gpio_bm.c
  * @brief   GPIO port helpers, bare-metal tier.
  ******************************************************************************
  */

#include "gpio_bm.h"
#include <stddef.h>
#include <stdint.h>

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

/* ---- Validation for the tiers above --------------------------------------------- */

bool GPIO_BM_IsValidPort(const GPIO_TypeDef *port)
{
    uint32_t idx;
    return port_index(port, &idx);
}

GpioBm_Status_t GPIO_BM_CheckConfig(const GpioBm_PinConfig_t *cfg)
{
    return validate_config(cfg);
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

bool GPIO_BM_IsPortClocked(const GPIO_TypeDef *port)
{
    uint32_t port_idx;

    if (!port_index(port, &port_idx))
    {
        return false;
    }

    /* RCC->AHB4ENR is only this core's bank; the port is clocked when either
       bank has the bit (RM0399 9.7.1), so read both through their fixed
       aliases. RCC_C1/RCC_C2 are CMSIS definitions (stm32h755xx.h). */
    const uint32_t bit = 1UL << port_idx;
#if defined(DUAL_CORE)
    return ((RCC_C1->AHB4ENR | RCC_C2->AHB4ENR) & bit) != 0UL;
#else
    return (RCC->AHB4ENR & bit) != 0UL;
#endif
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
    s->taken       = NULL;
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
            s->taken       = lk;   /* Exit releases this hook, whatever is installed then */
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
        /* The hook that was locked, not the one installed now: a
           GPIO_BM_SetSharedLock() inside the section must not leave the
           semaphore taken or release one this core never took. */
        const GpioBm_SharedLock_t *lk = s->taken;
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
    /* R1: unclocked, the port reads all ones, which would decode as a valid
       OK configuration (analog, OD, VERY_HIGH, reserved pull 3, AF15, 1). */
    if (!GPIO_BM_IsPortClocked(port))
    {
        return GPIO_BM_ERR_NO_CLOCK;
    }

    const uint32_t pos2 = pin_idx * 2U;

    out->port             = (GPIO_TypeDef *)(uintptr_t)port;   /* the struct's field is non-const */
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
    const bool     already = (lckr & GPIO_LCKR_LCKK) != 0UL;
    if (already && ((lckr & mask) == mask))
    {
        GPIO_BM_SectionExit(&section);   /* nothing to add */
        return GPIO_BM_OK;
    }

    /* On a port already locked the sequence is run again with the pins
       already locked kept in it. RM0399 12.4.8 says LCKy can only be written
       while LCKK = 0; on Rev V silicon a second sequence does add pins
       (measured: PF7|PF8, then PF6 -> LCKR 0x000101C0). Either way the
       read-back below decides, so the result is honest on any revision (C12).
       RM0399 12.4.8: the sequence must not be interrupted and every access is
       a 32-bit word (LCKR is declared as a 32-bit volatile). */
    const uint32_t pins = (already ? (lckr & 0xFFFFUL) : 0UL) | mask;
    const uint32_t key  = GPIO_LCKR_LCKK | pins;
    port->LCKR = key;
    port->LCKR = pins;
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

/* ==========================================================================
 * F2 additions: EXTI 0-15 and I/O compensation cell.
 * ========================================================================== */

/* SYSCFG clock enable in the calling core's RCC bank (RM0399 §9.7.1, §13.3).
 * APB4ENR is at offset 0xF4, inside the per-core banked register region,
 * so CPU1 routes to CPU1 bank and CPU2 routes to CPU2 bank. */
static bool enable_syscfg_clock(void)
{
    RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
    volatile uint32_t val = RCC->APB4ENR;
    return (val & RCC_APB4ENR_SYSCFGEN) != 0U;
}

GpioBm_Status_t GPIO_BM_ExtiConfig(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t trigger,
                                   bool interrupt, bool event)
{
    uint32_t port_idx;

    /* Rule 2 (C6): Validate before any peripheral access */
    if (!port_index(port, &port_idx))
    {
        return GPIO_BM_ERR_PARAM;
    }
    if (pin_idx >= GPIO_BM_PIN_COUNT)
    {
        return GPIO_BM_ERR_PARAM;
    }
    if ((trigger < GPIO_BM_EXTI_RISING) || (trigger > GPIO_BM_EXTI_BOTH))
    {
        return GPIO_BM_ERR_PARAM;
    }
    if (!interrupt && !event)
    {
        return GPIO_BM_ERR_PARAM;
    }

    if (!enable_syscfg_clock())
    {
        return GPIO_BM_ERR_VERIFY;
    }

    /* Rule 3 (C10): Shared registers written inside the section */
    GpioBm_Section_t sec;
    GpioBm_Status_t st = GPIO_BM_SectionEnter(&sec);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    /* Rule 4: Conflict check inside section before writing anything.
     * EXTICR routes pin_idx across ports (RM0399 §13.3.2, §13.6.2). If already routed
     * to another port and any core has this line unmasked (IMR1/EMR1 or
     * C2IMR1/C2EMR1), conflict. */
    const uint32_t cr_idx = pin_idx >> 2U;
    const uint32_t shift = (pin_idx & 0x3U) * 4U;
    const uint32_t current_port = (SYSCFG->EXTICR[cr_idx] >> shift) & 0xFUL;
    const uint32_t line_bit = 1UL << pin_idx;

    if (current_port != port_idx)
    {
        const uint32_t any_unmasked = (EXTI->IMR1 | EXTI->EMR1 | EXTI->C2IMR1 | EXTI->C2EMR1) & line_bit;
        if (any_unmasked != 0UL)
        {
            GPIO_BM_SectionExit(&sec);
            return GPIO_BM_ERR_CONFLICT;
        }
    }

    /* Rule 5: Order of ExtiConfig:
     * 1. EXTICR
     * 2. RTSR1/FTSR1 according to trigger
     * 3. Clear this core's PR1
     * 4. Set this core's IMR1/EMR1 (unrequested bits cleared) */

    /* 1. EXTICR (RM0399 §13.3.2, §13.6.2) */
    uint32_t exticr = SYSCFG->EXTICR[cr_idx];
    exticr &= ~(0xFUL << shift);
    exticr |= (port_idx << shift);
    SYSCFG->EXTICR[cr_idx] = exticr;

    /* 2. RTSR1 / FTSR1 (RM0399 §21.6.1, §21.6.2) */
    if ((trigger & GPIO_BM_EXTI_RISING) != 0U)
    {
        EXTI->RTSR1 |= line_bit;
    }
    else
    {
        EXTI->RTSR1 &= ~line_bit;
    }

    if ((trigger & GPIO_BM_EXTI_FALLING) != 0U)
    {
        EXTI->FTSR1 |= line_bit;
    }
    else
    {
        EXTI->FTSR1 &= ~line_bit;
    }

    /* 3. Clear this core's PR1 (w1c, RM0399 §21.6.21) */
#if defined(CORE_CM4)
    EXTI->C2PR1 = line_bit;
#else
    EXTI->PR1 = line_bit;
#endif

    /* 4. Set this core's IMR1 / EMR1 according to interrupt / event (RM0399 §21.6.19, §21.6.20) */
#if defined(CORE_CM4)
    if (interrupt)
    {
        EXTI->C2IMR1 |= line_bit;
    }
    else
    {
        EXTI->C2IMR1 &= ~line_bit;
    }
    if (event)
    {
        EXTI->C2EMR1 |= line_bit;
    }
    else
    {
        EXTI->C2EMR1 &= ~line_bit;
    }
#else
    if (interrupt)
    {
        EXTI->IMR1 |= line_bit;
    }
    else
    {
        EXTI->IMR1 &= ~line_bit;
    }
    if (event)
    {
        EXTI->EMR1 |= line_bit;
    }
    else
    {
        EXTI->EMR1 &= ~line_bit;
    }
#endif

    /* Read-back verification inside section (C12, M1) */
    if (((SYSCFG->EXTICR[cr_idx] >> shift) & 0xFUL) != port_idx)
    {
        GPIO_BM_SectionExit(&sec);
        return GPIO_BM_ERR_VERIFY;
    }
    const uint32_t rtsr_exp = ((trigger & GPIO_BM_EXTI_RISING) != 0U) ? line_bit : 0UL;
    if ((EXTI->RTSR1 & line_bit) != rtsr_exp)
    {
        GPIO_BM_SectionExit(&sec);
        return GPIO_BM_ERR_VERIFY;
    }
    const uint32_t ftsr_exp = ((trigger & GPIO_BM_EXTI_FALLING) != 0U) ? line_bit : 0UL;
    if ((EXTI->FTSR1 & line_bit) != ftsr_exp)
    {
        GPIO_BM_SectionExit(&sec);
        return GPIO_BM_ERR_VERIFY;
    }
#if defined(CORE_CM4)
    const uint32_t imr_exp = interrupt ? line_bit : 0UL;
    if ((EXTI->C2IMR1 & line_bit) != imr_exp)
    {
        GPIO_BM_SectionExit(&sec);
        return GPIO_BM_ERR_VERIFY;
    }
    const uint32_t emr_exp = event ? line_bit : 0UL;
    if ((EXTI->C2EMR1 & line_bit) != emr_exp)
    {
        GPIO_BM_SectionExit(&sec);
        return GPIO_BM_ERR_VERIFY;
    }
#else
    const uint32_t imr_exp = interrupt ? line_bit : 0UL;
    if ((EXTI->IMR1 & line_bit) != imr_exp)
    {
        GPIO_BM_SectionExit(&sec);
        return GPIO_BM_ERR_VERIFY;
    }
    const uint32_t emr_exp = event ? line_bit : 0UL;
    if ((EXTI->EMR1 & line_bit) != emr_exp)
    {
        GPIO_BM_SectionExit(&sec);
        return GPIO_BM_ERR_VERIFY;
    }
#endif

    GPIO_BM_SectionExit(&sec);
    return GPIO_BM_OK;
}

GpioBm_Status_t GPIO_BM_ExtiDisable(GPIO_TypeDef *port, uint32_t pin_idx)
{
    uint32_t port_idx;

    /* Rule 2 (C6): Validate before access */
    if (!port_index(port, &port_idx))
    {
        return GPIO_BM_ERR_PARAM;
    }
    if (pin_idx >= GPIO_BM_PIN_COUNT)
    {
        return GPIO_BM_ERR_PARAM;
    }

    if (!enable_syscfg_clock())
    {
        return GPIO_BM_ERR_VERIFY;
    }

    GpioBm_Section_t sec;
    GpioBm_Status_t st = GPIO_BM_SectionEnter(&sec);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    /* Rule 4: Conflict check. ExtiDisable returns ERR_CONFLICT without writing
     * if the line is routed to another port. */
    const uint32_t cr_idx = pin_idx >> 2U;
    const uint32_t shift = (pin_idx & 0x3U) * 4U;
    const uint32_t current_port = (SYSCFG->EXTICR[cr_idx] >> shift) & 0xFUL;
    if (current_port != port_idx)
    {
        GPIO_BM_SectionExit(&sec);
        return GPIO_BM_ERR_CONFLICT;
    }

    /* Rule 6: ExtiDisable respects the other core.
     * Clear IMR1/EMR1 and PR1 of this core. */
    const uint32_t line_bit = 1UL << pin_idx;
#if defined(CORE_CM4)
    EXTI->C2IMR1 &= ~line_bit;
    EXTI->C2EMR1 &= ~line_bit;
    EXTI->C2PR1 = line_bit;
    const uint32_t other_masks = (EXTI->IMR1 | EXTI->EMR1) & line_bit;
#else
    EXTI->IMR1 &= ~line_bit;
    EXTI->EMR1 &= ~line_bit;
    EXTI->PR1 = line_bit;
    const uint32_t other_masks = (EXTI->C2IMR1 | EXTI->C2EMR1) & line_bit;
#endif

    /* Only if other core's masks are both zero for this line, clear triggers
     * and reset EXTICR field to 0000. */
    if (other_masks == 0UL)
    {
        EXTI->RTSR1 &= ~line_bit;
        EXTI->FTSR1 &= ~line_bit;
        uint32_t exticr = SYSCFG->EXTICR[cr_idx];
        exticr &= ~(0xFUL << shift);
        SYSCFG->EXTICR[cr_idx] = exticr;
    }

    /* Read-back verification inside section (C12, M1) */
#if defined(CORE_CM4)
    if (((EXTI->C2IMR1 | EXTI->C2EMR1) & line_bit) != 0UL)
    {
        GPIO_BM_SectionExit(&sec);
        return GPIO_BM_ERR_VERIFY;
    }
#else
    if (((EXTI->IMR1 | EXTI->EMR1) & line_bit) != 0UL)
    {
        GPIO_BM_SectionExit(&sec);
        return GPIO_BM_ERR_VERIFY;
    }
#endif

    if (other_masks == 0UL)
    {
        if (((EXTI->RTSR1 | EXTI->FTSR1) & line_bit) != 0UL)
        {
            GPIO_BM_SectionExit(&sec);
            return GPIO_BM_ERR_VERIFY;
        }
        if (((SYSCFG->EXTICR[cr_idx] >> shift) & 0xFUL) != 0UL)
        {
            GPIO_BM_SectionExit(&sec);
            return GPIO_BM_ERR_VERIFY;
        }
    }

    GPIO_BM_SectionExit(&sec);
    return GPIO_BM_OK;
}

uint32_t GPIO_BM_ExtiTakePending(uint32_t line_mask)
{
    /* Rule 7: Only lines 0..15. No critical section entered.
     * Read PR1 of this core, mask, write-1-to-clear without RMW, DSB. (RM0399 §21.6.21) */
    uint32_t pending;
    line_mask &= 0xFFFFUL;

#if defined(CORE_CM4)
    pending = EXTI->C2PR1 & line_mask;
    EXTI->C2PR1 = pending;
#else
    pending = EXTI->PR1 & line_mask;
    EXTI->PR1 = pending;
#endif

    __DSB();
    return pending;
}

GpioBm_Status_t GPIO_BM_ExtiSoftwareTrigger(uint32_t pin_idx)
{
    /* Rule 2 (C6): Validate parameter */
    if (pin_idx >= GPIO_BM_PIN_COUNT)
    {
        return GPIO_BM_ERR_PARAM;
    }

    /* Rule 3 (C10): SWIER1 written inside section */
    GpioBm_Section_t sec;
    GpioBm_Status_t st = GPIO_BM_SectionEnter(&sec);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    /* Rule 8: Writes (1 << pin_idx) to SWIER1 (RM0399 §21.6.3). RM0399 §21.5.5: asserts
     * if RTSR1 or FTSR1 and the calling core's mask allow it. The function does not
     * verify those prerequisites. */
    EXTI->SWIER1 = (1UL << pin_idx);

    GPIO_BM_SectionExit(&sec);
    return GPIO_BM_OK;
}

IRQn_Type GPIO_BM_ExtiGetIRQn(uint32_t pin_idx)
{
    /* Rule 2 (C6): Valid pin_idx 0..15 maps to CMSIS EXTI IRQn_Type; > 15 returns INVALID */
    switch (pin_idx)
    {
    case 0U: return EXTI0_IRQn;
    case 1U: return EXTI1_IRQn;
    case 2U: return EXTI2_IRQn;
    case 3U: return EXTI3_IRQn;
    case 4U: return EXTI4_IRQn;
    case 5U:
    case 6U:
    case 7U:
    case 8U:
    case 9U: return EXTI9_5_IRQn;
    case 10U:
    case 11U:
    case 12U:
    case 13U:
    case 14U:
    case 15U: return EXTI15_10_IRQn;
    default: return GPIO_BM_EXTI_IRQN_INVALID;
    }
}

GpioBm_Status_t GPIO_BM_EnableCompensationCell(uint32_t max_cycles)
{
    /* Rule 2 (C6): Validate parameter */
    if (max_cycles == 0U)
    {
        return GPIO_BM_ERR_PARAM;
    }

    /* Rule 9 (Revision 2): If RCC_CR.CSIRDY == 0, return ERR_NOT_READY without writing anything */
    if ((RCC->CR & RCC_CR_CSIRDY) == 0UL)
    {
        return GPIO_BM_ERR_NOT_READY;
    }

    if (!enable_syscfg_clock())
    {
        return GPIO_BM_ERR_VERIFY;
    }

    /* Rule 3 (C10): CCCSR written inside the section */
    GpioBm_Section_t sec;
    GpioBm_Status_t st = GPIO_BM_SectionEnter(&sec);
    if (st != GPIO_BM_OK)
    {
        return st;
    }

    /* RM0399 §12.3.11, §13.3.7, §13.6.5: Set CCCSR.EN.
     * Never writes RCC_CR, HSLV, CS, nor CCCR. */
    SYSCFG->CCCSR |= SYSCFG_CCCSR_EN;

    /* Read-back verification inside section (C12, M1) */
    if ((SYSCFG->CCCSR & SYSCFG_CCCSR_EN) == 0UL)
    {
        GPIO_BM_SectionExit(&sec);
        return GPIO_BM_ERR_VERIFY;
    }

    /* Rule 9 (Revision 2, B2): Exit the critical section immediately.
     * CCCSR.READY is read-only. Waiting outside the section ensures the
     * cross-core lock (HSEM 5) and PRIMASK are held for < 60 us. */
    GPIO_BM_SectionExit(&sec);

    /* Wait for READY flag bounded by max_cycles of DWT->CYCCNT.
     * If CYCCNT does not advance or timeout expires, return ERR_NOT_READY
     * leaving CCCSR.EN = 1. Tracks consecutive unchanged readings (N7). */
    uint32_t prev = DWT->CYCCNT;
    const uint32_t start = prev;
    uint32_t unchanged_reads = 0U;

    while ((SYSCFG->CCCSR & SYSCFG_CCCSR_READY) == 0UL)
    {
        const uint32_t current = DWT->CYCCNT;
        if (current == prev)
        {
            unchanged_reads++;
            if (unchanged_reads >= 100U || unchanged_reads >= max_cycles)
            {
                return GPIO_BM_ERR_NOT_READY;
            }
        }
        else
        {
            unchanged_reads = 0U;
            prev = current;
        }

        if ((uint32_t)(current - start) >= max_cycles)
        {
            return GPIO_BM_ERR_NOT_READY;
        }
    }

    return GPIO_BM_OK;
}
