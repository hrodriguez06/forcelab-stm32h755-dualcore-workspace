# Linker Templates - STM32H755ZI (Dual-Core)

Ready-to-use GCC linker scripts and the companion MPU / shared-memory files for a
dual-core STM32H755ZI project (Nucleo-H755ZI-Q).

| File | Purpose |
| :--- | :--- |
| [`stm32h755zi_cm7.ld`](stm32h755zi_cm7.ld) | CPU1 (Cortex-M7), Flash bank 1 |
| [`stm32h755zi_cm4.ld`](stm32h755zi_cm4.ld) | CPU2 (Cortex-M4), Flash bank 2 |
| [`mpu_config.c`](mpu_config.c) | MPU regions matching the CM7 script |
| [`mpu_config.h`](mpu_config.h) | `MPU_Config()` declaration, companion to the `.c` |
| [`shared_memory.h`](shared_memory.h) | M7 <-> M4 shared block + HSEM ids |

---

## 1. Memory Partition

This partition is the contract between the two linker scripts, the MPU
configuration and the application. **Do not change one without the others.**

| Region | Address | Size | Owner | Cacheable | Reachable by |
| :--- | :--- | :---: | :--- | :---: | :--- |
| FLASH bank 1 | `0x08000000` | 1 MB | M7 code | yes (ART) | - |
| FLASH bank 2 | `0x08100000` | 1 MB | M4 code | n/a | - |
| ITCM | `0x00000000` | 64 KB | M7 `.itcm_text` | n/a | core only |
| DTCM | `0x20000000` | 128 KB | M7 `.data/.bss/stack` | n/a | **not DMA1/2** |
| AXI SRAM (D1) | `0x24000000` | 512 KB | M7 `.axi_ram` | **yes** | MDMA, DMA1/2 via bus matrix |
| SRAM1 (D2) | `0x30000000` | 128 KB | M7 `.dma_buf` | no | DMA1, DMA2, MDMA |
| SRAM2 (D2) | `0x30020000` | 128 KB | M4 `.data/.bss/stack` | n/a | DMA1, DMA2, MDMA |
| SRAM3 (D2) | `0x30040000` | 32 KB | `.eth_ram` | no | ETH DMA, DMA1/2, MDMA |
| SRAM4 (D3) | `0x38000000` | 32 KB | `.d3_ram` | no | **BDMA**, MDMA |
| SRAM4 (D3) | `0x38008000` | 32 KB | `.shared_ram` | no | both cores, MDMA |

> Source of truth for addresses and sizes:
> [`../../../STM32H755_Memory_Map.md`](../../../STM32H755_Memory_Map.md) and
> [`../memory_and_domains.md`](../memory_and_domains.md).

### Why this split

- **DTCM for M7 data**: zero wait state, but **DMA1 and DMA2 cannot reach it**.
  Anything handed to those controllers must be moved out via a section attribute.
- **SRAM1 for DMA, non-cacheable**: removes the whole class of D-Cache coherency
  bugs for the buffers you touch most often.
- **SRAM3 for Ethernet**: ETH DMA descriptors are required to sit in D2 SRAM.
- **SRAM4 for shared / BDMA**: BDMA reaches *only* D3, and the M4 (which has no
  cache) can read the shared block without any maintenance operation.
- **SRAM2 reserved for M4**: no overlap at all with M7 data, so a stack overflow
  on one core cannot silently corrupt the other.

---

## 2. Section Attributes

```c
#include "shared_memory.h"

/* DMA1 / DMA2 buffer - SRAM1, non-cacheable.
   Align to 32 AND size to a multiple of 32 (cache line).                   */
uint8_t uart_rx[64] __attribute__((section(".dma_buf"), aligned(32)));

/* Ethernet descriptors - SRAM3. The HAL section names are already routed
   there by the linker script (.RxDescripSection / .TxDescripSection).      */
uint8_t eth_pool[4096] __attribute__((section(".eth_ram"), aligned(32)));

/* BDMA buffer for LPUART1 / SPI6 / I2C4 - SRAM4.                          */
uint8_t lpuart_rx[32] __attribute__((section(".d3_ram"), aligned(32)));

/* Large cacheable working buffer - AXI SRAM.
   Needs clean/invalidate + __DMB() around every DMA transfer.             */
uint8_t frame[65536] __attribute__((section(".axi_ram"), aligned(32)));

/* Time-critical routine executed from ITCM.                               */
void __attribute__((section(".itcm_text"))) control_loop(void) { /* ... */ }

/* Shared block - defined ONCE, on the M7 side.                            */
SharedMemory_t g_shared __attribute__((section(".shared_ram")));
```

---

## 3. Integration Steps

### 3.1 STM32CubeIDE

1. Right-click the **CM7** project -> *Properties* -> *C/C++ Build* -> *Settings*
   -> *MCU GCC Linker* -> *General* -> *Linker Script*: point it at
   `stm32h755zi_cm7.ld`.
2. Repeat on the **CM4** project with `stm32h755zi_cm4.ld`.
3. Add `MPU_Config()` to the CM7 `main()` **before** `SCB_EnableDCache()`.
4. Add `shared_memory.h` to a folder visible to both projects.

### 3.2 Copying `.itcm_text` at startup

The `.itcm` section is loaded in Flash and must be copied to ITCM before use.
Add this to the CM7 `Reset_Handler`, next to the existing `.data` copy loop:

```c
extern uint32_t _si_itcm, _s_itcm, _e_itcm;

static void itcm_copy(void)
{
    uint32_t *src = &_si_itcm;
    uint32_t *dst = &_s_itcm;
    while (dst < &_e_itcm) { *dst++ = *src++; }
    __DSB();
    __ISB();
}
```

Skip this and any call into `.itcm_text` executes garbage.

### 3.3 Option bytes (mandatory for dual-core)

With STM32CubeProgrammer (see
[`../../01_drivers_and_hal/UM2237_stm32cubeprogrammer_user_manual.pdf`](../../01_drivers_and_hal/UM2237_stm32cubeprogrammer_user_manual.pdf)):

- `BCM7` = enabled, `BOOT_CM7_ADD0` = `0x0800` (-> `0x08000000`)
- `BCM4` = enabled, `BOOT_CM4_ADD0` = `0x0810` (-> `0x08100000`)

```
STM32_Programmer_CLI -c port=SWD mode=UR -ob BCM7=1 BCM7_ADD0=0x0800 BCM4=1 BCM4_ADD0=0x0810
```

---

## 4. Boot Sequence (matches `../dual_core_boot_ipc.md`)

1. Both cores release from reset. **CPU2 (M4) must wait**: it enters
   `HAL_PWREx_ClearPendingEvent()` + `HAL_PWREx_EnterSTOPMode(..., PWR_D2_DOMAIN)`
   or waits on **HSEM 0**.
2. CPU1 (M7) runs `MPU_Config()`, enables I-Cache and D-Cache, then configures
   power (`HAL_PWREx_ConfigSupply`), voltage scaling and the full clock tree.
3. CPU1 initialises `g_shared` and writes `SHARED_MAGIC`.
4. CPU1 takes and releases **HSEM 0**, which wakes CPU2.
5. CPU2 initialises its own peripherals only. It must **never** reprogram RCC
   PLLs or the power supply.

---

## 5. Errata That Affect This Configuration

Read [`../errata_summary.md`](../errata_summary.md) before trusting the build.
The ones that bite here:

- **2.1.5** - `__DMB()` after every cache maintenance call. Applies to every
  buffer in `.axi_ram`.
- **2.1.1** - never configure an MPU region as write-through. `mpu_config.c`
  uses write-back for AXI SRAM and non-cacheable everywhere else, on purpose.
- **2.1.2** - MPU region 0 exists as a background guard against speculative
  `PLD` line-fills into unmapped memory. It denies **everything**, so it only
  works because regions 1..8 map FLASH, ITCM, DTCM, AXI SRAM, the D2/D3 SRAMs
  and the peripheral block back in. Remove one of those and the CPU HardFault
  locks up inside `HAL_MPU_Enable()`, before any console output exists.
  Regions 9..15 are free for project additions.
- **2.3.1** - if WWDG is enabled on either core, set `RCC_GCR.WW1RSC` /
  `WW2RSC`, or the AXI domain locks up.
- **2.3.5** - `VOS0` / 480 MHz is unavailable on silicon revisions Y and W.
  Check `REV_ID` in `DBGMCU_IDC` before generating a 480 MHz clock tree.
- **2.20.3 / 2.21.1** - set bit 20 of `DMA_SxCR` on every USART / LPUART DMA
  stream, whatever memory the buffer lives in.

---

## 6. Verification

After linking, confirm the placement:

```bash
arm-none-eabi-size   -A -x build/cm7.elf
arm-none-eabi-nm     -n -S build/cm7.elf | grep -E '0x?(30|38|24|00)0'
arm-none-eabi-objdump -h build/cm7.elf | grep -E 'dma_buf|eth_ram|d3_ram|shared_ram|axi_ram|itcm'
```

Sanity checks:

- `.shared_ram` must start at `0x38008000` in **both** ELF files.
- `.dma_buf` must fall inside `0x30000000`-`0x3001FFFF`.
- No CM7 symbol may land inside `0x30020000`-`0x3003FFFF` (that is M4 territory).

---

## 7. Golden Copies and Drift Check

The files in this directory are the **golden copies**. A project consumes them by
copying them in, so the copies can silently drift apart - and a drifted
`mpu_config.c` is the one failure that is invisible: the board simply goes dead
inside `HAL_MPU_Enable()` (see the `@warning` at the top of that file).

Keep the copies byte-identical and verify with `diff` before every release:

```bash
# from the workspace root
T=00_docs_stm32h7/docs/00_core_and_system/linker_templates

for P in 02_firmware_projects/prj_eth_lwip_node \
         03_firmware_templates/prj_01_template_dualcore ; do
    diff -u $T/mpu_config.c       $P/CM7/App/Src/mpu_config.c
    diff -u $T/mpu_config.h       $P/CM7/App/Inc/mpu_config.h
    diff -u $T/stm32h755zi_cm7.ld $P/CM7/linker/stm32h755zi_cm7.ld
done

# Dual-core consumers carry two more:
D=03_firmware_templates/prj_01_template_dualcore
diff -u $T/stm32h755zi_cm4.ld $D/CM4/linker/stm32h755zi_cm4.ld
diff -u $T/shared_memory.h    $D/Common/Inc/shared_memory.h
```

Silence from all of them means every consumer is in sync. Add a line per
project as products are created - a copy nobody diffs is a copy that drifts.

**Direction of the fix:** an errata workaround or a region change is edited *here
first*, then propagated to every project. A change made in a project and not
brought back becomes an orphan patch that the next project will not inherit.
Nothing enforces this automatically today - until the drivers are consumed as a
Git submodule, the `diff` above is the only gate.
