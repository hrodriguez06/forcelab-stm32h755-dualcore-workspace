# DMA, BDMA, MDMA & DMAMUX - STM32H755

The DMA architecture on STM32H755 is one of the most powerful and complex aspects of the MCU.

---

## 1. DMA Controller Comparison

| Controller | Streams / Channels | Bus Domain | Can Access Memories | Typical Peripheral Routing |
| :--- | :--- | :--- | :--- | :--- |
| **DMA1** | 8 Streams | D2 (AHB3) | SRAM1, SRAM2, SRAM3, AXI SRAM | USART, SPI, I2C, TIM, ADC1/2 |
| **DMA2** | 8 Streams | D2 (AHB3) | SRAM1, SRAM2, SRAM3, AXI SRAM | USART, SPI, SAI, DCMI, Crypt |
| **BDMA** | 8 Channels | D3 (AHB4) | **SRAM4 only** (`0x38000000`) | LPUART1, SPI6, I2C4, ADC3, DAC1 |
| **MDMA** | 16 Channels | D1 (AXI) | **All memories** (AXI, D2, D3, Flash, TCM) | High-speed block transfers, SDMMC, QuadSPI, DMA chaining |

---

## 2. DMAMUX (DMA Request Multiplexer)
All requests from peripherals pass through **DMAMUX1** (for DMA1/DMA2) and **DMAMUX2** (for BDMA).
- DMAMUX allows mapping **any** peripheral request to **any** DMA stream.
- Includes synchronization triggers and request generators based on EXTI or timer events.

---

## 3. Critical DMA Rules for Cortex-M7 (CPU1)

1. **NO DTCM**:
   - DMA1/DMA2 CANNOT access DTCM (`0x20000000`).
   - If a buffer is declared on the stack of CPU1 or default BSS in DTCM, DMA transfers will **fail silently or trigger bus faults**.
   - **Solution**: Place buffers in D2 SRAM (`0x30000000`) using the `.dma_buf` section
     defined in [`../../00_core_and_system/linker_templates/`](../../00_core_and_system/linker_templates/README.md):
     ```c
     __attribute__((section(".dma_buf"), aligned(32))) uint8_t rx_buf[128];
     ```

2. **D-Cache Maintenance**:
   - Transmit: `SCB_CleanDCache_by_Addr((uint32_t*)buf, size);` before `HAL_..._Transmit_DMA()`.
   - Receive: `SCB_InvalidateDCache_by_Addr((uint32_t*)buf, size);` inside `HAL_..._RxCpltCallback()`.
   - Alignment: Always align buffers to 32 bytes (`aligned(32)`).

---

## 4. Key HAL Configurations
```c
/* DMA Stream Configuration Example */
hdma_usart3_rx.Instance                 = DMA1_Stream0;
hdma_usart3_rx.Init.Request             = DMA_REQUEST_USART3_RX; /* Routed via DMAMUX */
hdma_usart3_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
hdma_usart3_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
hdma_usart3_rx.Init.MemInc              = DMA_MINC_ENABLE;
hdma_usart3_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
hdma_usart3_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
hdma_usart3_rx.Init.Mode                = DMA_CIRCULAR; /* Or DMA_NORMAL */
hdma_usart3_rx.Init.Priority            = DMA_PRIORITY_LOW;
hdma_usart3_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
HAL_DMA_Init(&hdma_usart3_rx);

__HAL_LINKDMA(&huart3, hdmarx, hdma_usart3_rx);
```
