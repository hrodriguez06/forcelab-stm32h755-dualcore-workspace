/**
  ******************************************************************************
  * @file    uart_hal.h
  * @brief   Hardware-agnostic UART driver, HAL tier.
  ******************************************************************************
  */

#ifndef UART_HAL_H
#define UART_HAL_H

#include "stm32h7xx_hal.h"
#include <stdbool.h>

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

/* ---- Extended HAL API (Additive) --------------------------------------- */

/**
  * @brief  Pin specification for HAL-tier pin injection.
  */
typedef struct
{
    GPIO_TypeDef *port;     /*!< GPIO port (GPIOA..GPIOK) */
    uint32_t      pin_idx;  /*!< GPIO pin index (0..15) */
    uint8_t       af;       /*!< Alternate function (e.g. GPIO_AF7_USART3) */
} uart_hal_pin_t;

/**
  * @brief  Extended configuration structure for advanced UART initialization.
  */
typedef struct
{
    USART_TypeDef         *instance;          /*!< Instance, e.g. USART1, USART3, LPUART1 */
    uint32_t               baudrate;          /*!< Baudrate, e.g. 115200, 921600          */
    uint32_t               word_length;       /*!< e.g. UART_WORDLENGTH_8B, 7B, 9B        */
    uint32_t               stop_bits;         /*!< e.g. UART_STOPBITS_1, 2                */
    uint32_t               parity;            /*!< e.g. UART_PARITY_NONE, EVEN, ODD       */
    uint32_t               hw_flow_ctl;       /*!< e.g. UART_HWCONTROL_NONE, RTS, CTS     */
    uint32_t               oversampling;      /*!< e.g. UART_OVERSAMPLING_16, 8           */
    bool                   enable_clock;      /*!< Automatically enable APB clock in RCC  */
    const uart_hal_pin_t  *tx_pin;            /*!< Optional TX pin configuration          */
    const uart_hal_pin_t  *rx_pin;            /*!< Optional RX pin configuration          */
} uart_hal_config_ex_t;

/**
  * @brief  Initializes UART with injected extended configuration, pins, and clock.
  * @param  huart  Pointer to HAL UART handle.
  * @param  config Injected extended hardware configuration.
  * @return HAL status.
  */
HAL_StatusTypeDef UART_HAL_InitEx(UART_HandleTypeDef *huart, const uart_hal_config_ex_t *config);

/**
  * @brief  Blocking write with configurable timeout and status return.
  * @param  huart      Pointer to HAL UART handle.
  * @param  data       Pointer to bytes to transmit.
  * @param  length     Number of bytes to transmit.
  * @param  timeout_ms Timeout in milliseconds.
  * @return HAL status.
  */
HAL_StatusTypeDef UART_HAL_WriteEx(UART_HandleTypeDef *huart, const uint8_t *data, uint16_t length, uint32_t timeout_ms);

/**
  * @brief  Validates and applies the ES0445 Errata 2.20.3 & 2.21.1 workaround (bit 20).
  *         Checks if the DMA stream CR has bit 20 (DMA_SxCR_TRBUFF) set, setting it if missing.
  * @param  hdma Pointer to HAL DMA handle attached to UART stream.
  * @return HAL status.
  */
HAL_StatusTypeDef UART_HAL_CheckDmaErrata(DMA_HandleTypeDef *hdma);


/* ---- Backward-compatible Console API ----------------------------------- */

/** @brief  Configures default console UART (instance from board_def.h). */
HAL_StatusTypeDef UART_Console_Init(uint32_t baudrate);

/** @brief  Blocking write to the default console UART. */
void UART_Console_Write(const uint8_t *data, uint16_t length);

/** @brief  The default console HAL handle, for debugging. */
UART_HandleTypeDef *UART_Console_GetHandle(void);

/* ---- MSP Override Hooks ------------------------------------------------ */
/**
  * @brief  Weak user hook for UART MSP pinout and clock initialization.
  * @note   Two ways to customize UART pinout/hardware:
  *         1. Implement a strong UART_HAL_MspInit_User() in application code:
  *            ideal for single-UART projects customizing console pins.
  *         2. Define UART_HAL_CUSTOM_MSP at compile time (or before including uart_hal.h):
  *            suppresses HAL_UART_MspInit() in uart_hal.c entirely, allowing full
  *            control from a multi-UART stm32h7xx_hal_msp.c without duplicate symbol errors.
  */
void UART_HAL_MspInit_User(UART_HandleTypeDef *huart);

/**
  * @brief  Weak user hook for UART MSP de-initialization.
  */
void UART_HAL_MspDeInit_User(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* HAL_UART_MODULE_ENABLED */

#endif /* UART_HAL_H */
