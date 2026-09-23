/**
  ******************************************************************************
  * @file    uart_ll.h
  * @brief   Low-Layer UART/USART/LPUART driver for STM32H755.
  *
  *          Driver tier: *_ll (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          High performance, low footprint, based purely on ST LL static inline
  *          functions. Does not require USE_FULL_LL_DRIVER and does not include
  *          stm32h7xx_hal.h.
  ******************************************************************************
  */

#ifndef UART_LL_H
#define UART_LL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "stm32h7xx_ll_usart.h"
#include "stm32h7xx_ll_lpuart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Status / Error Flags (Bitmask) -------------------------------------- */
#define UART_LL_ERR_ORE               (1UL << 0)  /*!< Overrun error flag   */
#define UART_LL_ERR_NE                (1UL << 1)  /*!< Noise error flag     */
#define UART_LL_ERR_FE                (1UL << 2)  /*!< Framing error flag   */
#define UART_LL_ERR_PE                (1UL << 3)  /*!< Parity error flag    */

/**
  * @brief  Circular ring buffer for interrupt-driven non-blocking reception.
  */
typedef struct
{
    uint8_t        *data;   /*!< Storage buffer array                   */
    size_t          size;   /*!< Total capacity in bytes                */
    volatile size_t head;   /*!< Write index (updated by IRQ)           */
    volatile size_t tail;   /*!< Read index (updated by consumer)       */
} UartLL_RingBuffer_t;

static inline bool UartLL_RingBuffer_Put(UartLL_RingBuffer_t *rb, uint8_t byte)
{
    if ((rb == NULL) || (rb->data == NULL) || (rb->size == 0U))
    {
        return false;
    }

    size_t next_head = (rb->head + 1U) % rb->size;
    if (next_head == rb->tail)
    {
        return false; /* Buffer is full */
    }

    rb->data[rb->head] = byte;
    __DMB();
    rb->head = next_head;
    return true;
}

static inline bool UartLL_RingBuffer_Get(UartLL_RingBuffer_t *rb, uint8_t *byte)
{
    if ((rb == NULL) || (rb->data == NULL) || (byte == NULL))
    {
        return false;
    }

    if (rb->head == rb->tail)
    {
        return false; /* Buffer is empty */
    }

    *byte = rb->data[rb->tail];
    __DMB();
    rb->tail = (rb->tail + 1U) % rb->size;
    return true;
}

static inline size_t UartLL_RingBuffer_Count(const UartLL_RingBuffer_t *rb)
{
    if ((rb == NULL) || (rb->size == 0U))
    {
        return 0;
    }

    if (rb->head >= rb->tail)
    {
        return rb->head - rb->tail;
    }

    return rb->size - (rb->tail - rb->head);
}

/**
  * @brief  Pin specification for optional dependency-injected GPIO configuration.
  */
typedef struct
{
    GPIO_TypeDef *port;     /*!< GPIO port (GPIOA..GPIOK) */
    uint32_t      pin_idx;  /*!< Pin index (0..15)        */
    uint32_t      af;       /*!< Alternate function (0..15)*/
} UartLL_Pin_t;

/**
  * @brief  Hardware configuration structure (Dependency Injection).
  */
typedef struct
{
    USART_TypeDef       *instance;          /*!< Instance: USART1..8 or LPUART1           */
    uint32_t             kernel_clock_hz;   /*!< Peripheral kernel clock frequency in Hz  */
    uint32_t             baudrate;          /*!< Desired baudrate (e.g. 115200, 921600)   */
    uint32_t             data_bits;         /*!< LL_USART_DATAWIDTH_8B, 7B, 9B            */
    uint32_t             stop_bits;         /*!< LL_USART_STOPBITS_1, 2, 0_5, 1_5         */
    uint32_t             parity;            /*!< LL_USART_PARITY_NONE, EVEN, ODD          */
    uint32_t             oversampling;      /*!< LL_USART_OVERSAMPLING_16, 8 (USART only) */
    bool                 fifo_enable;       /*!< true: enable hardware FIFO               */
    uint32_t             hw_flow;           /*!< LL_USART_HWCONTROL_NONE, RTS, CTS, ...   */
    bool                 enable_clock;      /*!< true: driver enables APB clock in RCC    */
    const UartLL_Pin_t  *tx_pin;            /*!< Optional TX pin (NULL if pre-configured) */
    const UartLL_Pin_t  *rx_pin;            /*!< Optional RX pin (NULL if pre-configured) */
} UartLL_Config_t;

/**
  * @brief  Low-layer UART driver handle.
  */
typedef struct
{
    USART_TypeDef          *instance;      /*!< Active peripheral instance */
    bool                    is_lpuart;     /*!< true if LPUART1            */
    bool                    fifo_enabled;  /*!< true if FIFO is enabled    */
    volatile uint32_t       error_flags;   /*!< Accumulated error bitmask  */
} UartLL_Handle_t;

/**
  * @brief  Initializes UART/USART/LPUART peripheral using ST LL static inline calls.
  * @param  h   Pointer to handle structure to initialize.
  * @param  cfg Pointer to configuration structure.
  * @return true on success, false on invalid parameter or baud rate error > 2%.
  */
bool UART_LL_Init(UartLL_Handle_t *h, const UartLL_Config_t *cfg);

/**
  * @brief  De-initializes peripheral using ST LL calls.
  * @param  h Pointer to handle structure.
  * @return true on success, false if invalid handle.
  */
bool UART_LL_Deinit(UartLL_Handle_t *h);

/**
  * @brief  Non-blocking single byte transmission.
  * @param  h Pointer to handle structure.
  * @param  b Byte to transmit.
  * @return true if byte was placed in TDR/FIFO, false if TXE/TXFNF was not ready.
  */
bool UART_LL_WriteByte(UartLL_Handle_t *h, uint8_t b);

/**
  * @brief  Writes a buffer of bytes with explicit spin count per byte (deterministic).
  * @param  h          Pointer to handle structure.
  * @param  d          Data buffer to transmit.
  * @param  n          Number of bytes to transmit.
  * @param  spin_limit Maximum loop iterations per byte before timing out.
  * @return Number of bytes actually written.
  */
size_t UART_LL_Write(UartLL_Handle_t *h, const uint8_t *d, size_t n, uint32_t spin_limit);

/**
  * @brief  Non-blocking single byte reception. Checks and clears ORE/FE/NE/PE.
  * @param  h Pointer to handle structure.
  * @param  b Pointer to byte storage.
  * @return true if a byte was read into *b, false if RXNE/RXFNE was empty.
  */
bool UART_LL_ReadByte(UartLL_Handle_t *h, uint8_t *b);

/**
  * @brief  Waits until transmission complete (TC flag) up to spin_limit iterations.
  * @param  h          Pointer to handle structure.
  * @param  spin_limit Maximum spin loop iterations.
  * @return true if flushed (TC set), false on timeout.
  */
bool UART_LL_Flush(UartLL_Handle_t *h, uint32_t spin_limit);

/**
  * @brief  Reads and atomically clears accumulated error flags.
  * @param  h Pointer to handle structure.
  * @return Bitmask of UART_LL_ERR_* accumulated since last call.
  */
uint32_t UART_LL_TakeErrors(UartLL_Handle_t *h);

/**
  * @brief  Enables RXNE interrupt and sets NVIC priority.
  * @param  h        Pointer to handle structure.
  * @param  irqn     IRQ number for this UART.
  * @param  priority Preemption priority in NVIC.
  * @return true on success.
  */
bool UART_LL_EnableRxIrq(UartLL_Handle_t *h, IRQn_Type irqn, uint32_t priority);

/**
  * @brief  Disables RXNE interrupt and disables NVIC IRQ.
  * @param  h    Pointer to handle structure.
  * @param  irqn IRQ number for this UART.
  */
void UART_LL_DisableRxIrq(UartLL_Handle_t *h, IRQn_Type irqn);

/**
  * @brief  Interrupt Service Routine handler for UART RX.
  *         Pushes incoming bytes into the provided ring buffer and clears errors.
  * @param  h    Pointer to handle structure.
  * @param  ring Pointer to target ring buffer.
  */
void UART_LL_IrqService(UartLL_Handle_t *h, UartLL_RingBuffer_t *ring);

#ifdef __cplusplus
}
#endif

#endif /* UART_LL_H */
