/**
  ******************************************************************************
  * @file    main.h
  * @brief   Common definitions for CPU1 (Cortex-M7).
  ******************************************************************************
  */

#ifndef MAIN_H
#define MAIN_H

#include "stm32h7xx_hal.h"
#include "board_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set to 1 once CPU2 firmware exists in Flash bank 2. CPU1 then performs the
 * HSEM handshake that releases CPU2 after the clock tree is up.
 * Leave it at 0 while only the Cortex-M7 image is being flashed: waiting for
 * a core that never boots would stall this one. */
#ifndef CM4_BOOT_ENABLED
#define CM4_BOOT_ENABLED        0
#endif

/** HSEM channel used for the CPU1 -> CPU2 boot notification. */
#define HSEM_ID_BOOT            0U

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
