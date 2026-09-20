/**
  ******************************************************************************
  * @file    uart_hal.h
  * @brief   Hardware-agnostic UART driver, HAL tier.
  ******************************************************************************
  */

#ifndef UART_HAL_H
#define UART_HAL_H

#include "stm32h7xx_hal.h"

#if defined(HAL_UART_MODULE_ENABLED)

#ifdef __cplusplus
extern "C" {
#endif

#define UART_CONSOLE_DEFAULT_BAUDRATE   115200UL

/**
  * @brief  Hardware configuration structure for UART peripheral instances.
  */
typedef struct
{
    USART_TypeDef *instance;    /*!< Hardware instance, e.g. USART1, USART3, LPUART1 */
    uint32_t       baudrate;    /*!< Target baudrate, e.g. 115200, 921600            */
} uart_hal_config_t;

/**
  * @brief  Initializes any UART instance using injected configuration.
  * @param  huart  Pointer to HAL UART handle.
  * @param  config Injected hardware configuration (instance + baudrate).
  * @return HAL status.
  */
HAL_StatusTypeDef UART_HAL_Init(UART_HandleTypeDef *huart, const uart_hal_config_t *config);

/**
  * @brief  Blocking write to an initialized UART handle.
  * @param  huart  Pointer to HAL UART handle.
  * @param  data   Pointer to bytes to transmit.
  * @param  length Number of bytes to transmit.
  */
void UART_HAL_Write(UART_HandleTypeDef *huart, const uint8_t *data, uint16_t length);

/* ---- Backward-compatible Console API ----------------------------------- */

/** @brief  Configures default console UART (instance from board_def.h / Nucleo fallback). */
HAL_StatusTypeDef UART_Console_Init(uint32_t baudrate);

/** @brief  Blocking write to the default console UART. */
void UART_Console_Write(const uint8_t *data, uint16_t length);

/** @brief  The default console HAL handle, for debugging. */
UART_HandleTypeDef *UART_Console_GetHandle(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_UART_MODULE_ENABLED */

#endif /* UART_HAL_H */
