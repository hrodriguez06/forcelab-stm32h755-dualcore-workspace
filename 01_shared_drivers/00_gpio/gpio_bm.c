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

bool GPIO_BM_ConfigAlternate(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t af, uint32_t pull, uint32_t speed)
{
    if ((port == NULL) || (pin_idx > 15U) || (af > 15U))
    {
        return false;
    }

    if (!GPIO_BM_EnablePortClock(port))
    {
        return false;
    }

    /* 1. Alternate Function selection: AFR[0] for pins 0..7, AFR[1] for pins 8..15 */
    uint32_t afr_idx   = pin_idx >> 3U;
    uint32_t afr_shift = (pin_idx & 7U) * 4U;
    port->AFR[afr_idx] = (port->AFR[afr_idx] & ~(0xFU << afr_shift)) | ((af & 0xFU) << afr_shift);

    /* 2. Output type: Push-Pull (0) */
    port->OTYPER &= ~(1UL << pin_idx);

    /* 3. Output speed: 2 bits per pin */
    uint32_t pos2 = pin_idx * 2U;
    port->OSPEEDR = (port->OSPEEDR & ~(0x3UL << pos2)) | ((speed & 0x3UL) << pos2);

    /* 4. Pull-up / Pull-down: 2 bits per pin */
    port->PUPDR = (port->PUPDR & ~(0x3UL << pos2)) | ((pull & 0x3UL) << pos2);

    /* 5. Mode: Alternate Function (0b10 = 2) */
    port->MODER = (port->MODER & ~(0x3UL << pos2)) | (0x2UL << pos2);

    return true;
}

bool GPIO_BM_ConfigAnalog(GPIO_TypeDef *port, uint32_t pin_idx)
{
    if ((port == NULL) || (pin_idx > 15U))
    {
        return false;
    }

    if (!GPIO_BM_EnablePortClock(port))
    {
        return false;
    }

    uint32_t pos2 = pin_idx * 2U;

    /* 1. Mode: Analog (0b11) */
    port->MODER |= (0x3UL << pos2);

    /* 2. Pull-up / Pull-down: None (0b00) */
    port->PUPDR &= ~(0x3UL << pos2);

    return true;
}


