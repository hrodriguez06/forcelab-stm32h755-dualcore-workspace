/**
  ******************************************************************************
  * @file    uart_bm.h
  * @brief   Bare-metal UART/USART/LPUART driver for STM32H755.
  *
  *          Driver tier: *_bm (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Zero overhead, direct register manipulation, hardware-agnostic.
  *          Usable before HAL_Init(), before SystemClock_Config(), and inside
  *          fault handlers.
  ******************************************************************************
  */

#ifndef UART_BM_H
#define UART_BM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Data Bits ----------------------------------------------------------- */
#define UART_BM_DATA_BITS_8           0x00000000UL
#define UART_BM_DATA_BITS_7           0x00000001UL
#define UART_BM_DATA_BITS_9           0x00000002UL

/* ---- Stop Bits ----------------------------------------------------------- */
#define UART_BM_STOP_BITS_1           0x00000000UL
#define UART_BM_STOP_BITS_0_5         0x00000001UL
#define UART_BM_STOP_BITS_2           0x00000002UL
#define UART_BM_STOP_BITS_1_5         0x00000003UL

/* ---- Parity -------------------------------------------------------------- */
#define UART_BM_PARITY_NONE           0x00000000UL
#define UART_BM_PARITY_EVEN           0x00000001UL
#define UART_BM_PARITY_ODD            0x00000002UL

/* ---- Hardware Flow Control ----------------------------------------------- */
#define UART_BM_HW_FLOW_NONE          0x00000000UL
#define UART_BM_HW_FLOW_RTS           0x00000001UL
#define UART_BM_HW_FLOW_CTS           0x00000002UL
#define UART_BM_HW_FLOW_RTS_CTS       0x00000003UL

/* ---- Status / Error Flags (Bitmask) -------------------------------------- */
#define UART_BM_ERR_ORE               (1UL << 0)  /*!< Overrun error flag   */
#define UART_BM_ERR_NE                (1UL << 1)  /*!< Noise error flag     */
#define UART_BM_ERR_FE                (1UL << 2)  /*!< Framing error flag   */
#define UART_BM_ERR_PE                (1UL << 3)  /*!< Parity error flag    */

/**
  * @brief  Pin specification for optional dependency-injected GPIO configuration.
  */
typedef struct
{
    GPIO_TypeDef *port;     /*!< GPIO port (GPIOA..GPIOK) */
    uint32_t      pin_idx;  /*!< Pin index (0..15)        */
    uint32_t      af;       /*!< Alternate function (0..15)*/
} UartBm_Pin_t;

/**
  * @brief  Hardware configuration structure (Dependency Injection).
  */
typedef struct
{
    USART_TypeDef       *instance;          /*!< Instance: USART1..8 or LPUART1           */
    uint32_t             kernel_clock_hz;   /*!< Peripheral kernel clock frequency in Hz  */
    uint32_t             baudrate;          /*!< Desired baudrate (e.g. 115200, 921600)   */
    uint32_t             data_bits;         /*!< UART_BM_DATA_BITS_*                      */
    uint32_t             stop_bits;         /*!< UART_BM_STOP_BITS_*                      */
    uint32_t             parity;            /*!< UART_BM_PARITY_*                         */
    bool                 oversampling_8;    /*!< true: OVER8=1, false: OVER16=0 (USART)   */
    bool                 fifo_enable;       /*!< true: enable hardware FIFO (CR1_FIFOEN)  */
    uint32_t             hw_flow;           /*!< UART_BM_HW_FLOW_*                        */
    bool                 enable_clock;      /*!< true: driver enables APB clock in RCC    */
    const UartBm_Pin_t  *tx_pin;            /*!< Optional TX pin (NULL if pre-configured) */
    const UartBm_Pin_t  *rx_pin;            /*!< Optional RX pin (NULL if pre-configured) */
} UartBm_Config_t;

/**
  * @brief  Bare-metal UART driver handle.
  */
typedef struct
{
    USART_TypeDef          *instance;      /*!< Active peripheral instance */
    bool                    is_lpuart;     /*!< true if LPUART1            */
    bool                    fifo_enabled;  /*!< true if FIFO is enabled    */
    uint32_t                data_bits;     /*!< Configured data bits       */
    volatile uint32_t       error_flags;   /*!< Accumulated error bitmask  */
} UartBm_Handle_t;

/**
  * @brief  Initializes UART/USART/LPUART peripheral with injected configuration.
  * @param  h   Pointer to handle structure to initialize.
  * @param  cfg Pointer to configuration structure.
  * @return true on success, false on invalid parameter, unsupported clock, or BRR error > 2%.
  */
bool UART_BM_Init(UartBm_Handle_t *h, const UartBm_Config_t *cfg);

/**
  * @brief  De-initializes peripheral (disables UE and clears control registers).
  * @param  h Pointer to handle structure.
  * @return true on success, false if invalid handle.
  */
bool UART_BM_Deinit(UartBm_Handle_t *h);

/**
  * @brief  Non-blocking single byte transmission.
  * @param  h Pointer to handle structure.
  * @param  b Byte to transmit.
  * @return true if byte was placed in TDR/FIFO, false if TXE/TXFNF was not ready.
  */
bool UART_BM_WriteByte(UartBm_Handle_t *h, uint8_t b);

/**
  * @brief  Writes a buffer of bytes with explicit spin count per byte (deterministic).
  * @param  h          Pointer to handle structure.
  * @param  d          Data buffer to transmit.
  * @param  n          Number of bytes to transmit.
  * @param  spin_limit Maximum loop iterations per byte before timing out.
  * @return Number of bytes actually written.
  */
size_t UART_BM_Write(UartBm_Handle_t *h, const uint8_t *d, size_t n, uint32_t spin_limit);

/**
  * @brief  Non-blocking single byte reception. Checks and clears ORE/FE/NE/PE.
  * @param  h Pointer to handle structure.
  * @param  b Pointer to byte storage.
  * @return true if a byte was read into *b, false if RXNE/RXFNE was empty.
  */
bool UART_BM_ReadByte(UartBm_Handle_t *h, uint8_t *b);

/**
  * @brief  Waits until transmission complete (TC flag) up to spin_limit iterations.
  * @param  h          Pointer to handle structure.
  * @param  spin_limit Maximum spin loop iterations.
  * @return true if flushed (TC set), false on timeout.
  */
bool UART_BM_Flush(UartBm_Handle_t *h, uint32_t spin_limit);

/**
  * @brief  Reads and atomically clears accumulated error flags.
  * @param  h Pointer to handle structure.
  * @return Bitmask of UART_BM_ERR_* accumulated since last call.
  */
uint32_t UART_BM_TakeErrors(UartBm_Handle_t *h);

#ifdef __cplusplus
}
#endif

#endif /* UART_BM_H */
