/**
  ******************************************************************************
  * @file    uart_hal.h
  * @brief   Bring-up console on the ST-LINK virtual COM port, HAL tier.
  *
  *          NUCLEO-H755ZI-Q wiring: USART3, PD8 = TX (AF7), PD9 = RX (AF7),
  *          bridged to the host by the on-board ST-LINK/V3.
  *
  *          This is a blocking, polled console meant for logs and for the
  *          Ethernet verification procedure. Anything that needs throughput or
  *          DMA belongs in a *_ll or *_bm implementation next to this one.
  ******************************************************************************
  */

#ifndef UART_HAL_H
#define UART_HAL_H

#include "stm32h7xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UART_CONSOLE_DEFAULT_BAUDRATE   115200UL

/** @brief  Configures USART3 and its pins. Safe to call more than once. */
HAL_StatusTypeDef UART_Console_Init(uint32_t baudrate);

/** @brief  Blocking write. Returns once the last byte has left the shifter. */
void UART_Console_Write(const uint8_t *data, uint16_t length);

/** @brief  The HAL handle, for debugging. */
UART_HandleTypeDef *UART_Console_GetHandle(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_HAL_H */
