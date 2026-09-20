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

#ifdef __cplusplus
extern "C" {
#endif

/** HSEM channel used for the CPU1 -> CPU2 boot notification.
 *  Must match CM7/Core/Inc/main.h and HSEM_ID_BOOT_SYNC in shared_memory.h. */
#define HSEM_ID_BOOT            0U

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
