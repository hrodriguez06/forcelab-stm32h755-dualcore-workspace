# FMC & QUADSPI (External Memory Interfaces) - STM32H755

## 1. Documentation
- `23_flexible-memory-controller-fmc.pdf` - NOR, PSRAM, SRAM, SDRAM, NAND
- `24_quad-spi-interface-quadspi.pdf` - Quad-SPI NOR Flash, memory-mapped / XIP
- `25_delay-block-dlyb.pdf` - phase alignment above ~50 MHz (also used by SDMMC)

---

## 2. Address Spaces

| Interface | Address range | Size | Default MPU attribute |
| :--- | :--- | :--- | :--- |
| FMC NOR / PSRAM / SRAM bank 1 | `0x60000000` - `0x6FFFFFFF` | 256 MB | Normal, write-back write-allocate |
| FMC NAND Flash | `0x80000000` - `0x8FFFFFFF` | 256 MB | Normal, write-through |
| **QUADSPI memory-mapped** | `0x90000000` - `0x9FFFFFFF` | 256 MB | Normal, write-through |
| FMC SDRAM bank 1 | `0xC0000000` - `0xC3FFFFFF` | 64 MB | Device |
| FMC SDRAM bank 2 | `0xD0000000` - `0xD3FFFFFF` | 64 MB | Device |

Exact boundaries and reset attributes: [`../../../STM32H755_Memory_Map.md`](../../../STM32H755_Memory_Map.md).

Both controllers live in **D1** and are reachable by MDMA, and by DMA1/DMA2 through the
bus matrix.

> The Nucleo-H755ZI-Q has **no external memory fitted**. Both interfaces are brought out
> on the Zio / Morpho headers only. Verify every pin against
> [`MB1363_nucleo_h755zi_q_schematics.pdf`](../../02_board_hardware/MB1363_nucleo_h755zi_q_schematics.pdf)
> before wiring a memory shield.

---

## 3. QUADSPI Pinout (STM32H755ZI)

Bank 1, the common mapping:

| Signal | Candidate pins | AF |
| :--- | :--- | :---: |
| `QUADSPI_CLK` | `PB2`, `PF10` | AF9 |
| `QUADSPI_BK1_NCS` | `PB6` (AF10), `PB10` (AF9), `PG6` (AF10) | - |
| `QUADSPI_BK1_IO0` | `PC9`, `PD11` (AF9), `PF8` (AF10) | - |
| `QUADSPI_BK1_IO1` | `PC10`, `PD12` (AF9), `PF9` (AF10) | - |
| `QUADSPI_BK1_IO2` | `PE2`, `PF7` (AF9) | AF9 |
| `QUADSPI_BK1_IO3` | `PA1`, `PD13`, `PF6` (AF9) | AF9 |

Bank 2: `PE7`-`PE10` (AF10), `PG9`/`PG14` (AF9), `PH2`/`PH3` (AF9),
`QUADSPI_BK2_NCS` on `PC11` (AF9).

**Note the AF is not uniform** - `PF8`/`PF9` are AF10 while `PD11`/`PD12` are AF9 for the
same signal. Always confirm per pin in
[`../../../STM32H755_Alternate_Functions.md`](../../../STM32H755_Alternate_Functions.md).

All QUADSPI pins need `GPIO_SPEED_FREQ_VERY_HIGH`.

---

## 4. QUADSPI Clocking

```
quadspi_ker_ck  <-  rcc_hclk3  |  pll1_q_ck  |  pll2_r_ck  |  per_ck
                    selected by QSPISEL[1:0] in RCC_D1CCIPR
```

> **ERRATA ES0445 2.8.5**: using **HCLK** as the kernel clock causes memory failures
> unless the prescaler preserves a 50 % duty cycle.
> **Use `pll1_q_ck` or `pll2_r_ck` instead.** This is the recommended configuration.

```c
RCC_PeriphCLKInitTypeDef p = {0};
p.PeriphClockSelection   = RCC_PERIPHCLK_QSPI;
p.QspiClockSelection     = RCC_QSPICLKSOURCE_PLL;   /* pll1_q_ck */
HAL_RCCEx_PeriphCLKConfig(&p);
```

Above roughly 50 MHz, tune the **DLYB** to align the sampling phase, or reads drift with
temperature.

---

## 5. QUADSPI Errata - Mandatory Init Sequence

### ES0445 2.8.3 - QUADSPI internal timing criticality (`A`)

Run this **after reset** and **on every exit from memory-mapped mode**. The final
command must complete within 127 kernel clocks of the first `QSPI_CCR` write, so keep
it free of function calls and debugger breakpoints.

```c
/* Save QUADSPI->CR / CCR first if they hold live configuration */
QUADSPI->CR  = 0;                       /* disable, drop max prescaler          */
while (QUADSPI->SR & 0x20) { }          /* wait for BUSY to fall                */
QUADSPI->CR  = 0xFF000001;              /* max prescaler + enable               */
QUADSPI->CCR = 0x20000000;              /* activate free-running clock          */
QUADSPI->CCR = 0x20000000;              /* repeat: avoid back-to-back disable   */
QUADSPI->CR  = 0;                       /* disable                              */
while (QUADSPI->SR & 0x20) { }
/* Restore CR / CCR */
```

### Other QUADSPI errata

| # | Limitation | Consequence |
| :--- | :--- | :--- |
| 2.8.1 | Indirect read mode with only a data phase (`P`) | always include an instruction or address phase |
| 2.8.2 | QUADSPI hangs when `QUADSPI_CCR` is cleared (`P`) | never write 0 to `CCR` to "reset" it |
| 2.8.4 | Memory-mapped read of the last memory byte fails (`P`) | never map the final byte of the window as live data |

---

## 6. Memory-Mapped Mode & the MPU (CRITICAL)

Memory-mapped mode exposes 256 MB of address space at `0x90000000` regardless of the
actual flash size. The Cortex-M7 **speculatively prefetches** cacheable memory, so it
will issue reads beyond the end of the device. Those reads never get a response and the
bus stalls - the classic "H7 hangs at random when XIP is enabled".

**The MPU fix is not optional**: mark the whole 256 MB window inaccessible, then open a
region that matches the real device size.

```c
/* Region 9: block the entire QUADSPI window */
MPU_Region(MPU_REGION_NUMBER9, 0x90000000, MPU_REGION_SIZE_256MB,
           MPU_TEX_LEVEL0, MPU_ACCESS_NOT_CACHEABLE,
           MPU_ACCESS_NOT_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
           MPU_REGION_NO_ACCESS, MPU_INSTRUCTION_ACCESS_DISABLE);

/* Region 10: open only the real device (16 MB example), cacheable + executable */
MPU_Region(MPU_REGION_NUMBER10, 0x90000000, MPU_REGION_SIZE_16MB,
           MPU_TEX_LEVEL0, MPU_ACCESS_CACHEABLE,
           MPU_ACCESS_BUFFERABLE, MPU_ACCESS_NOT_SHAREABLE,
           MPU_REGION_FULL_ACCESS, MPU_INSTRUCTION_ACCESS_ENABLE);
```

Higher region number wins on overlap, so region 10 punches a hole in region 9.
Regions 0..8 are the core memory map in `mpu_config.c`; 9..15 are free for
project additions like this one.
Helper signature and the rest of the region map:
[`../../00_core_and_system/linker_templates/mpu_config.c`](../../00_core_and_system/linker_templates/mpu_config.c).

> **ERRATA ES0445 2.1.1** - never mark external memory write-through as a coherency
> shortcut. Use write-back, or non-cacheable for buffers touched by DMA.

---

## 7. FMC SDRAM Notes

- SDRAM windows default to **Device** memory type, which is strongly ordered and slow.
  For a framebuffer, remap the used portion as Normal write-back through the MPU.
- **ERRATA ES0445 2.7.5 / 2.7.6 (documentation)**: RM0399's `TRCD` and `TXSR` values are
  wrong, and the recovery delay is inconsistent when `TWR[3:0] = 0000`. Take timings
  from the SDRAM datasheet, not from the reference manual table.
- **ERRATA ES0445 2.7.3** - unaligned reads are unsupported (`P`). Keep FMC accesses
  naturally aligned; `__attribute__((packed))` structs over FMC memory will fault or
  return garbage.
- **ERRATA ES0445 2.7.1** - dummy read cycles are inserted on synchronous memories
  (`N`). Budget for the lost bandwidth rather than trying to remove it.
- **ERRATA ES0445 2.7.2** - wrong data read from a busy NAND (`A`): poll the
  ready/busy line before reading.
- SDRAM refresh must be reprogrammed whenever the FMC clock changes. A clock switch
  after `HAL_SDRAM_Init()` without recomputing the refresh counter corrupts memory
  slowly and silently.

---

## 8. DMA & Cache

- **MDMA** is the right controller for external memory: it reaches FMC, QUADSPI, TCM,
  Flash and every SRAM.
- DMA1/DMA2 can reach FMC and QUADSPI through the bus matrix but compete with the
  CPU for D1 bandwidth.
- External memory mapped as cacheable requires the usual maintenance around DMA:
  `SCB_CleanDCache_by_Addr()` / `SCB_InvalidateDCache_by_Addr()` followed by `__DMB()`
  (**ERRATA ES0445 2.1.5**).

---

## 9. Debug Checklist

| Symptom | Likely cause |
| :--- | :--- |
| Random hard faults or hangs once XIP is enabled | MPU does not block the unused part of the 256 MB QUADSPI window |
| QUADSPI works after reset, dies after switching modes | missing 2.8.3 unlock sequence |
| Flaky reads above 50 MHz, temperature dependent | DLYB not tuned |
| QUADSPI unreliable in general | kernel clock is HCLK (errata 2.8.5) - move to `pll1_q_ck` |
| SDRAM corrupts slowly over minutes | refresh counter not recomputed after a clock change |
| Reads from a packed struct in FMC return garbage | unaligned access (errata 2.7.3) |

---

## 10. Cross-References
- Memory map: [`../../../STM32H755_Memory_Map.md`](../../../STM32H755_Memory_Map.md)
- MPU and cache: [`../../00_core_and_system/cache_mpu_rules.md`](../../00_core_and_system/cache_mpu_rules.md)
- Errata: [`../../00_core_and_system/errata_summary.md`](../../00_core_and_system/errata_summary.md)
- MDMA: [`../dma_mdma/README.md`](../dma_mdma/README.md)
- Framebuffers: [`../display_graphics/README.md`](../display_graphics/README.md)
