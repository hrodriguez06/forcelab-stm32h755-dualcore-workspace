/**
  ******************************************************************************
  * @file    gpio_bm.h
  * @brief   GPIO port helpers, bare-metal tier.
  *
  *          Driver tier: *_bm. Direct register access only, no ST HAL and no
  *          LL headers, so this file is usable from the earliest boot code -
  *          before the clock tree, before the MPU and before HAL_Init().
  *          01_led/led_bm.c depends on exactly that property.
  ******************************************************************************
  */

#ifndef GPIO_BM_H
#define GPIO_BM_H

#include <stdbool.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Enables the AHB4 clock of any GPIO port, GPIOA..GPIOK.
  *
  *         Derives the RCC_AHB4ENR bit from the port address rather than
  *         taking one __HAL_RCC_GPIOx_CLK_ENABLE() per port: the ports are
  *         0x400 apart and GPIOAEN..GPIOKEN are bits 0..10 in that order, so a
  *         driver handed a port at run time can bring it up without a switch
  *         over eleven macros it would have to keep in sync.
  *
  * @param  port  Port to clock. NULL and any address outside GPIOA..GPIOK are
  *               rejected rather than turned into an out-of-range bit.
  * @retval true  when the clock is on and readable.
  * @retval false when `port` is not a GPIO port; nothing was written.
  *
  * @note   Includes the read-back that RCC needs before the peripheral answers,
  *         the same one ST's __HAL_RCC_GPIOx_CLK_ENABLE() performs.
  * @note   Dual-core: the write is a read-modify-write on RCC_AHB4ENR, which
  *         both CPUs share, so two cores clocking different ports at the same
  *         instant can lose one of the two updates. ST's own macro has the
  *         identical exposure. This driver does not take an HSEM around it on
  *         purpose - it has to run before the HSEM clock exists. Bring your
  *         ports up from CPU1 during boot, or guard the call yourself with
  *         HSEM if CPU2 has to do it while CPU1 is running.
  */
bool GPIO_BM_EnablePortClock(GPIO_TypeDef *port);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_BM_H */
