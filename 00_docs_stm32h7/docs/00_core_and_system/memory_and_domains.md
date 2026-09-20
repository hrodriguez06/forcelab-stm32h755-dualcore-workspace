# STM32H755 Memory Domains & Bus Matrix Architecture

The STM32H755 architecture is divided into three distinct power and clock domains: **D1**, **D2**, and **D3**. Understanding these domains is essential when allocating buffers for DMA and exchanging data between cores.

---

## 1. Domain Memory Summary Table

| Domain | RAM Name | Start Address | Size | Primary Master | DMA Access Capabilities |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **D1 (CD)** | **ITCM** | `0x00000000` | 64 KB | Cortex-M7 (I-side) | MDMA only (no DMA1/DMA2) |
| **D1 (CD)** | **DTCM** | `0x20000000` | 128 KB | Cortex-M7 (D-side) | MDMA only (no DMA1/DMA2) |
| **D1 (CD)** | **AXI SRAM** | `0x24000000` | 512 KB | Cortex-M7 / AXI Matrix | MDMA, DMA1/DMA2 (via AXI-to-AHB bridge) |
| **D2 (CD)** | **SRAM1** | `0x30000000` | 128 KB | Cortex-M4 / AHB3 | DMA1, DMA2, MDMA, Ethernet DMA, SDMMC |
| **D2 (CD)** | **SRAM2** | `0x30020000` | 128 KB | Cortex-M4 / AHB3 | DMA1, DMA2, MDMA |
| **D2 (CD)** | **SRAM3** | `0x30040000` | 32 KB | Cortex-M4 / AHB3 | DMA1, DMA2, MDMA, Shared IPC RAM |
| **D3 (SRD)**| **SRAM4** | `0x38000000` | 64 KB | AHB4 Matrix | BDMA, MDMA, DMA1/DMA2 (via bridge) |
| **D3 (SRD)**| **Backup SRAM** | `0x38800000` | 4 KB | Backup domain | Low power retention |

---

## 2. DMA Controller Assignment Rules

### A. DMA1 and DMA2 (General-Purpose DMA)
- **Bus Master**: Connected to the D2 64-bit AHB bus matrix.
- **Optimal Memory**: **D2 SRAM1, SRAM2, SRAM3** (`0x30000000` - `0x30047FFF`).
- **Can Access**: AXI SRAM (`0x24000000`) via the bus bridge with slight latency penalty.
- **CANNOT Access**: DTCM (`0x20000000`) or ITCM (`0x00000000`). If you pass a DTCM pointer (e.g. standard local stack variable or default bss) to `HAL_UART_Transmit_DMA()`, a bus error or silent transfer stall will occur!

### B. BDMA (Basic DMA)
- **Location**: D3 Domain.
- **Can Access**: Only **D3 SRAM4** (`0x38000000`) and D3 peripherals (SPI6, I2C4, LPUART1, ADC3, DAC1).
- **Use case**: Low power operations while D1 and D2 domains are in DStop/DStandby.

### C. MDMA (Master DMA)
- **Location**: D1 Domain.
- **Can Access**: Can access almost any memory in the chip (ITCM, DTCM, AXI SRAM, D2 SRAM1-3, D3 SRAM4, Flash, external memories, and internal peripheral registers).
- **Special feature**: Can trigger block transfers based on DMA1/DMA2 requests (DMA buffer chaining/circular ping-pong).

---

## 3. Linker Section Allocation

Do **not** hand-roll these sections. The canonical dual-core linker scripts, the matching
MPU configuration and the shared-memory block live in
[`linker_templates/`](linker_templates/README.md). Use the section names they define:

| Attribute | Lands in | Address | Cacheable | Use it for |
| :--- | :--- | :--- | :---: | :--- |
| `.dma_buf` | SRAM1 (D2) | `0x30000000` | no | DMA1 / DMA2 buffers - the default choice |
| `.eth_ram` | SRAM3 (D2) | `0x30040000` | no | Ethernet DMA descriptors and buffers |
| `.d3_ram` | SRAM4 (D3) | `0x38000000` | no | BDMA buffers, D3 peripherals (LPUART1, SPI6, I2C4) |
| `.shared_ram` | SRAM4 (D3) | `0x38008000` | no | M7 <-> M4 shared data, guarded by HSEM |
| `.axi_ram` | AXI SRAM (D1) | `0x24000000` | **yes** | bulk buffers, framebuffers - needs cache maintenance |
| `.itcm_text` | ITCM | `0x00000000` | n/a | time-critical functions on CPU1 |

```c
/* DMA1 / DMA2 buffer: non-cacheable SRAM1, 32-byte aligned, size multiple of 32 */
uint8_t uart_dma_rx_buffer[256] __attribute__((section(".dma_buf"), aligned(32)));

/* BDMA buffer for LPUART1 / SPI6 / I2C4 */
uint8_t lpuart_rx[32] __attribute__((section(".d3_ram"), aligned(32)));
```

A buffer in `.axi_ram` is cacheable, so it still needs
`SCB_CleanDCache_by_Addr()` / `SCB_InvalidateDCache_by_Addr()` followed by `__DMB()`
around every DMA transfer (**ERRATA ES0445 2.1.5**). A buffer in any of the
non-cacheable sections needs none of that. See
[`cache_mpu_rules.md`](cache_mpu_rules.md).

> The default `.bss` on CPU1 lands in **DTCM**, which DMA1 and DMA2 **cannot reach**.
> A plain global handed to `HAL_*_Start_DMA()` fails silently. This is the single most
> common H7 DMA bug.
