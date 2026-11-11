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

#ifndef CM4_BOOT_ENABLED
#define CM4_BOOT_ENABLED        1
#endif

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
