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
   RM0399; the static assertion below fails the build if a future CMSIS header
   ever breaks the first one. */
#define GPIO_BM_PORT_STRIDE     0x400UL

_Static_assert(((uint32_t)GPIOB - (uint32_t)GPIOA) == GPIO_BM_PORT_STRIDE,
               "GPIO ports are no longer 0x400 apart: "
               "GPIO_BM_EnablePortClock() derives its RCC bit from that stride");

bool GPIO_BM_EnablePortClock(GPIO_TypeDef *port)
{
    uint32_t address;
    uint32_t port_idx;

    if (port == NULL)
    {
        return false;
    }

    address = (uint32_t)port;

    if ((address < (uint32_t)GPIOA) || (address > (uint32_t)GPIOK))
    {
        return false;
    }

    port_idx = (address - (uint32_t)GPIOA) / GPIO_BM_PORT_STRIDE;

    /* Reads like a cross-core race and is not one. RCC_AHB4ENR is at offset
       0xE0, inside the bank RM0399 9.7.1 calls "registers mapped at the same
       address": the hardware routes the access by core_id, so CPU1 lands in the
       CPU1 bank and CPU2 in the CPU2 bank. Different storage, and a peripheral
       is clocked when either bank has its bit set - which is why ST's own
       __HAL_RCC_*_CLK_ENABLE() macros carry no CORE_CM4 variant.
       Only the registers below 0xD0 (RCC_CR, the PLLs, the RSTRs) are shared by
       both cores. Those are CPU1's by the boot contract, and anything both
       cores must write needs an HSEM - CLAUDE.md rule D. */
    RCC->AHB4ENR |= (1UL << port_idx);

    /* Read back: the RCC enable takes a couple of cycles to land, and a write
       to the port's registers before it does is simply dropped. */
    volatile uint32_t dummy = RCC->AHB4ENR;
    (void)dummy;

    return true;
}
