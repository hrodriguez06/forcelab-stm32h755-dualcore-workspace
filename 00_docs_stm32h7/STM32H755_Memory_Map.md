# STM32H755 - Memory Map

## Table 6. Memory map and default device memory area attributes

| Region | Boundary address | Arm® Cortex®-M7 | Arm® Cortex®-M4 | Type | Attributes | Execute never |
| :--- | :--- | :--- | :--- | :--- | :--- | :---: |
| **External devices** | `0xD0000000 - 0xD3FFFFFF` | FMC SDRAM bank 2 (or Reserved in case of FMC remap) | FMC SDRAM bank 2 (or Reserved in case of FMC remap) | Device | — | Y |
| | `0xCC000000 - 0xCFFFFFFF` | FMC SDRAM bank 1 (or remap of FMC NOR/PSRAM/SRAM 4 bank 1) | FMC SDRAM bank 1 (or remap of FMC NOR/PSRAM/SRAM 4 bank 1) | Device | — | Y |
| | `0xC8000000 - 0xCBFFFFFF` | FMC SDRAM bank 1 (or remap of FMC NOR/PSRAM/SRAM 3 bank 1) | FMC SDRAM bank 1 (or remap of FMC NOR/PSRAM/SRAM 3 bank 1) | Device | — | Y |
| | `0xC4000000 - 0xC7FFFFFF` | FMC SDRAM bank 1 (or remap of FMC NOR/PSRAM/SRAM 2 bank 1) | FMC SDRAM bank 1 (or remap of FMC NOR/PSRAM/SRAM 2 bank 1) | Device | — | Y |
| | `0xC0000000 - 0xC3FFFFFF` | FMC SDRAM bank 1 (or remap of FMC NOR/PSRAM/SRAM 1 bank 1) | FMC SDRAM bank 1 (or remap of FMC NOR/PSRAM/SRAM 1 bank 1) | Device | — | Y |
| | `0xA0000000 - 0xBFFFFFFF` | Reserved | Reserved | Device | — | Y |
| **External Memories** | `0x90000000 - 0x9FFFFFFF` | QUADSPI | QUADSPI | Normal | Write-through | N |
| | `0x80000000 - 0x8FFFFFFF` | FMC NAND Flash memory | FMC NAND Flash memory | Normal | Write-through | N |
| | `0x70000000 - 0x7FFFFFFF` | Reserved (or remap of FMC SDRAM bank 2) | Reserved (or remap of FMC SDRAM bank 2) | Normal | Write-Back Write Allocate | N |
| | `0x6C000000 - 0x6FFFFFFF` | FMC NOR/PSRAM/SRAM 4 bank 1 (or remap of FMC SDRAM bank 1) | FMC NOR/PSRAM/SRAM 4 bank 1 (or remap of FMC SDRAM bank 1) | Normal | Write-Back Write Allocate | N |
| | `0x68000000 - 0x6BFFFFFF` | FMC NOR/PSRAM/SRAM 3 bank 1 (or remap of FMC SDRAM bank 1) | FMC NOR/PSRAM/SRAM 3 bank 1 (or remap of FMC SDRAM bank 1) | Normal | Write-Back Write Allocate | N |
| | `0x64000000 - 0x67FFFFFF` | FMC NOR/PSRAM/SRAM 2 bank 1 (or remap of FMC SDRAM bank 1) | FMC NOR/PSRAM/SRAM 2 bank 1 (or remap of FMC SDRAM bank 1) | Normal | Write-Back Write Allocate | N |
| | `0x60000000 - 0x63FFFFFF` | FMC NOR/PSRAM/SRAM 1 bank 1 (or remap of FMC SDRAM bank 1) | FMC NOR/PSRAM/SRAM 1 bank 1 (or remap of FMC SDRAM bank 1) | Normal | Write-Back Write Allocate | N |
| **Peripherals** | `0x40000000 - 0x5FFFFFFF` | Peripherals (refer to *Table 7: Register boundary addresses*) | Peripherals (refer to *Table 7: Register boundary addresses*) | Device | — | Y |
| **RAM** | `0x38801000 - 0x3FFFFFFF` | Reserved | Reserved | Normal | Write-Back Write Allocate | N |
| | `0x38800000 - 0x38800FFF` | Backup SRAM | Backup SRAM | Normal | Write-Back Write Allocate | N |
| | `0x38010000 - 0x387FFFFF` | Reserved | Reserved | Normal | Write-Back Write Allocate | N |
| | `0x38000000 - 0x3800FFFF` | SRAM4 | SRAM4 | Normal | Write-Back Write Allocate | N |
| | `0x30048000 - 0x37FFFFFF` | Reserved | Reserved | Normal | Write-Back Write Allocate | N |
| | `0x30040000 - 0x30047FFF` | SRAM3 | SRAM3 | Normal | Write-Back Write Allocate | N |
| | `0x30020000 - 0x3003FFFF` | SRAM2 | SRAM2 | Normal | Write-Back Write Allocate | N |
| | `0x30000000 - 0x3001FFFF` | SRAM1 | SRAM1 | Normal | Write-Back Write Allocate | N |
| | `0x24080000 - 0x2FFFFFFF` | Reserved | Reserved | Normal | Write-Back Write Allocate | N |
| | `0x24000000 - 0x2407FFFF` | AXI SRAM | AXI SRAM | Normal | Write-Back Write Allocate | N |
| | `0x20020000 - 0x23FFFFFF` | Reserved | Reserved | Normal | Write-Back Write Allocate | N |
| | `0x20000000 - 0x2001FFFF` | DTCM | Reserved | Normal | Write-Back Write Allocate | N |
| **Code** | `0x1FF20000 - 0x1FFFFFFF` | Reserved | Reserved | Normal | Write-through | N |
| | `0x1FF00000 - 0x1FF1FFFF` | System Memory | Reserved | Normal | Write-through | N |
| | `0x10048000 - 0x1FEFFFFF` | Reserved | Reserved | Normal | Write-through | N |
| | `0x10040000 - 0x10047FFF` | SRAM3 (Alias)<sup>(1)</sup> | SRAM3 (Alias)<sup>(1)</sup> | Normal | Write-through | N |
| | `0x10020000 - 0x1003FFFF` | SRAM2 (Alias)<sup>(1)</sup> | SRAM2 (Alias)<sup>(1)</sup> | Normal | Write-through | N |
| | `0x10000000 - 0x1001FFFF` | SRAM1 (Alias)<sup>(1)</sup> | SRAM1 (Alias)<sup>(1)</sup> | Normal | Write-through | N |
| | `0x08200000 - 0x0FFFFFFF` | Reserved | Reserved | Normal | Write-through | N |
| | `0x08100000 - 0x081FFFFF` | Flash memory bank 2<sup>(2)</sup> | Flash memory bank 2<sup>(2)</sup> | Normal | Write-through | N |
| | `0x08000000 - 0x080FFFFF` | Flash memory bank 1<sup>(3)</sup> | Flash memory bank 1<sup>(3)</sup> | Normal | Write-through | N |
| | `0x00010000 - 0x07FFFFFF` | Reserved | Reserved | Normal | Write-through | N |
| | `0x00000000 - 0x0000FFFF` | ITCM | VTOR REMAP<sup>(4)</sup> | Normal | Write-through | N |

---

### Footnotes

1. Alias to maintain Arm® Cortex®-M4 Harvard architecture.
2. Flash memory bank 2 boundary is limited to `0x08100000 - 0x0817FFFF` on STM32H745xG/STM32H747xG.
3. Flash memory bank 1 boundary is limited to `0x08000000 - 0x0807FFFF` on STM32H745xG/STM32H747xG.
4. Selectable boot memory alias.

> **Note:** All the memory map areas that are not allocated to on-chip memories and peripherals are considered “Reserved”. For the detailed mapping of available memory and register areas, refer to the following table.
