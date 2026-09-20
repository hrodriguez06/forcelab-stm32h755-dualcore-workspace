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

    RCC->AHB4ENR |= (1UL << port_idx);

    /* Read back: the RCC enable takes a couple of cycles to land, and a write
       to the port's registers before it does is simply dropped. */
    volatile uint32_t dummy = RCC->AHB4ENR;
    (void)dummy;

    return true;
}
