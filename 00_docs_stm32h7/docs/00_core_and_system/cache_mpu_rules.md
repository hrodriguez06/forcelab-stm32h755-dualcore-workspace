# STM32H755 Cortex-M7 Cache & MPU Rules

The Cortex-M7 core on STM32H755 includes a **32 KB Instruction Cache (I-Cache)** and a **32 KB Data Cache (D-Cache)**. The D-Cache uses a **32-byte cache line** size and a write-back policy by default.

---

## 1. The Cache Coherency Problem

```
[ Cortex-M7 Core ]  <==>  [ D-Cache (32-byte lines) ]  <==>  [ Physical SRAM ]
                                                                     ^
                                                                     |
                                                              [ DMA Controller ]
```

When DMA writes to physical SRAM, the Cortex-M7 might read stale data still held in its D-Cache.
When the CPU writes to a buffer, changes may linger in D-Cache and not yet reach physical SRAM before DMA transmits.

---

## 2. Rule #1: Alignment and Size Granularity

Every DMA buffer used by Cortex-M7 **must**:
1. Be aligned to **32 bytes**: `__attribute__((aligned(32)))`
2. Have a total length rounded up to the nearest multiple of **32 bytes**.

> **WARNING**: If two unrelated variables share the same 32-byte cache line, invalidating the cache line for a DMA receive buffer will **destroy** the changes made by the CPU to the adjacent variable!

```c
#define DMA_BUFFER_SIZE  128 /* Multiple of 32 */

uint8_t dma_tx_buf[DMA_BUFFER_SIZE] __attribute__((aligned(32)));
uint8_t dma_rx_buf[DMA_BUFFER_SIZE] __attribute__((aligned(32)));
```

---

## 3. Strategy A: Explicit Software Cache Maintenance

Use CMSIS core cache management functions before/after DMA operations.

### DMA Transmit (Memory -> Peripheral)
Before triggering the DMA transfer, flush CPU changes from D-Cache out to physical RAM:
```c
/* 1. CPU populates dma_tx_buf */
fill_data(dma_tx_buf, len);

/* 2. Clean D-Cache for the buffer address and size */
SCB_CleanDCache_by_Addr((uint32_t *)dma_tx_buf, ((len + 31) / 32) * 32);

/* 3. Start DMA */
HAL_UART_Transmit_DMA(&huart3, dma_tx_buf, len);
```

### DMA Receive (Peripheral -> Memory)
Before reading data written by the DMA, invalidate the D-Cache line so the core fetches directly from physical RAM:
```c
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        /* Invalidate D-Cache before reading buffer */
        SCB_InvalidateDCache_by_Addr((uint32_t *)dma_rx_buf, ((RX_LEN + 31) / 32) * 32);

        /* Now safe to parse dma_rx_buf */
        process_packet(dma_rx_buf, RX_LEN);
    }
}
```

---

## 4. Strategy B: Hardware MPU Non-Cacheable Region (Recommended for Ethernet & Ring Buffers)

For peripherals with continuous streaming (Ethernet descriptors, high-speed SPI/UART circular buffers), configure an entire memory section (e.g. SRAM1 or SRAM4) as **Non-Cacheable** using the ARM MPU:

```c
void MPU_Config_NonCacheable_D2_SRAM(void)
{
    MPU_Region_InitTypeDef MPU_InitStruct = {0};

    HAL_MPU_Disable();

    /* Configure SRAM1 (0x30000000, 128KB) as Non-Cacheable */
    MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
    MPU_InitStruct.Number           = MPU_REGION_NUMBER0;
    MPU_InitStruct.BaseAddress      = 0x30000000;
    MPU_InitStruct.Size             = MPU_REGION_SIZE_128KB;
    MPU_InitStruct.SubRegionDisable = 0x00;
    MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
    MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
    MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    MPU_InitStruct.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
    MPU_InitStruct.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;

    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}
```
*Note: With this MPU region active, buffers placed in `0x30000000` do NOT require `SCB_CleanDCache` or `SCB_InvalidateDCache`.*
