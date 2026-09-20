# USART / UART - STM32H755

## 1. Instance Distribution & Clock Domains
- **USART1, USART6**: High-speed APB2 peripherals (D2 domain).
- **USART2, USART3, UART4, UART5, UART7, UART8**: APB1 peripherals (D2 domain).
- **LPUART1**: Low-power UART in the **D3 domain** (can operate from LSE/LSI in Stop mode with BDMA).

---

## 2. Common Pin Mappings (Nucleo-H755ZI-Q Default)
- **ST-LINK VCP (Virtual COM Port)**:
  - Instance: **USART3**
  - TX: `PD8` (AF7)
  - RX: `PD9` (AF7)
  - Baudrate: 115200 8N1 default.

---

## 3. Recommended Operation Modes

### A. Polling (Blocking) - Prototyping Only
```c
HAL_UART_Transmit(&huart3, (uint8_t*)"Hello\r\n", 7, 100);
```

### B. Interrupt / Idle Line Detection (Variable-length frames)
Utilize `HAL_UARTEx_ReceiveToIdle_IT()` or `HAL_UARTEx_ReceiveToIdle_DMA()`:
```c
/* Start reception until Idle Line is detected */
HAL_UARTEx_ReceiveToIdle_DMA(&huart3, rx_dma_buf, sizeof(rx_dma_buf));

/* Callback fired when transmission pauses (frame received) */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART3) {
        /* Invalidate D-Cache for Cortex-M7 */
        SCB_InvalidateDCache_by_Addr((uint32_t *)rx_dma_buf, ((Size + 31) / 32) * 32);

        /* Process 'Size' bytes */
        handle_rx_packet(rx_dma_buf, Size);

        /* Re-arm */
        HAL_UARTEx_ReceiveToIdle_DMA(&huart3, rx_dma_buf, sizeof(rx_dma_buf));
    }
}
```

---

## 4. Hardware FIFO
The H7 USARTs include an internal **16-byte FIFO** for both TX and RX.
- Enable FIFO mode (`huart.Init.FifoMode = UART_FIFOMODE_ENABLE`) to reduce interrupt load at high baud rates (e.g. > 1 Mbps).
