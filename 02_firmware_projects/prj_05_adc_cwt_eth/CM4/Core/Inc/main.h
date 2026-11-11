/**
  ******************************************************************************
  * @file    main.h
  * @brief   Common definitions for CPU2 (Cortex-M4).
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

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
