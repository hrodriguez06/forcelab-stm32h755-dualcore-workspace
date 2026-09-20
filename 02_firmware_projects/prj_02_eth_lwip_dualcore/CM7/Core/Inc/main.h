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
#include "shared_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set to 1 once CPU2 firmware exists in Flash bank 2. CPU1 then performs the
 * HSEM handshake that releases CPU2 after the clock tree is up.
 * Set to 0 while only the Cortex-M7 image is being flashed: waiting for
 * a core that never boots would stall this one. */
#ifndef CM4_BOOT_ENABLED
#define CM4_BOOT_ENABLED        1
#endif

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
