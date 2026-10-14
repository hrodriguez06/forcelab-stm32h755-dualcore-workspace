# STM32H755 Shared SPI Driver (`01_shared_drivers/06_spi`)

Peripheral-First (Plan A), 3-tier shared driver for SPI1–SPI6 of the STM32H755ZI in SPI master mode:
Motorola frames, full duplex or transmit only, modes 0–3, 4–32-bit frames (4–16 on SPI4–6), MSB or LSB
first, SS by software or by hardware (`SSIOP`, `MSSI`, `MIDI`, pulse between frames), FIFO threshold,
`AFCNTR`, transfers of 1–65 535 frames by polling, by interrupt and by DMA (DMA1/DMA2 and the BDMA), the
kernel clock read from its multiplexer, and de-initialisation without an RCC reset. Every parameter is
validated before the first access, every configuration write is read back, and no state is kept in RAM.
Slave, CRC, TI, half duplex, receive only, `TSER`, endless transfers, suspension and I2S are not in the API.

Status: validated on a NUCLEO-H755ZI-Q (Rev V, `REV_ID = 0x2003`) on 2026-10-03/04, in
`prj_01_template_dualcore` only, with an Analog Discovery 2 and one jumper (J1). Each phase passed an
independent audit with the auditor's own bench run: F0 at `620a678`, F1 (`spi_bm`) at `b4fa22d`, F2
(`spi_ll`) at `99fe89b`, F3 (`spi_hal`) at `b267f4c`. The final code (`8b5d165`, with finding m4) runs the
tests of all four phases: **45/45, `FW VERDICT=PASS`** (`docs/spi_f3_bench_8b5d165.txt`, [§9](#9-validation)).

---

## 1. Tiers

| Tier | Files | CMake target | Links | ST dependency | Typical use |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal** | `spi_bm.[c\|h]` | `stm32h7_driver_spi_bm` | - | CMSIS only (`stm32h7xx.h`) | Both cores, ISRs, the other tiers' validation |
| **Low-Level** | `spi_ll.[c\|h]` | `stm32h7_driver_spi_ll` | `_spi_bm` | `stm32h7xx_ll_spi.h` inlines and `LL_SPI_WriteReg`/`ReadReg` | Code written against `LL_SPI_*` constants |
| **HAL** | `spi_hal.[c\|h]` | `stm32h7_driver_spi_hal` | `_spi_bm` | `stm32h7xx_hal.h` (`HAL_SPI_*`) | Code that already keeps a `SPI_HandleTypeDef` |

```c
target_link_libraries(cm7_firmware PRIVATE stm32h7_driver_spi_bm)   # CMake; or _ll, _hal
#include "spi_ll.h"    /* never "06_spi/spi_ll.h": the directory is on the include path */
```

| Tier | CM7 | CM4 |
| :--- | :---: | :---: |
| BM, LL | yes | yes |
| HAL | yes (needs `HAL_SPI_MODULE_ENABLED`; `prj_01` enables it only with `SPI_DRIVER_SELFTEST`) | not built on CM4 in `prj_01` (CM4 does not link `_spi_hal`) |

- **No unsuffixed target;** the umbrella `stm32h7_shared_drivers` puts `06_spi` on the include path only.
- **`spi_bm` holds the only copy** of the capability table and of every check (`SPI_BM_Check*`, no write);
  LL and HAL call it before they write, so every tier refuses a request with the same status.
- **No state in RAM:** `.data = .bss = 0` in the three objects. `.text + .rodata`: `spi_bm` 3540 B (CM7)
  / 3502 B (CM4), `spi_ll` 2400 / 2376 B, `spi_hal` 1224 B (`docs/spi_f3_map_sizes_8b5d165.txt`).
- **No `USE_FULL_LL_DRIVER`, no `LL_SPI_Init`; `spi_hal` compiles to nothing** without
  `HAL_SPI_MODULE_ENABLED`, and stops with `#error` if `USE_SPI_CRC != 0`. **No** `SPIx_IRQHandler`,
  `HAL_SPI_*Callback`, MSP or `NVIC_*` call in any tier.

### 1.1 Function matrix

| Operation | BM | LL | HAL |
| :--- | :---: | :---: | :---: |
| `IsValidInstance`, `GetCaps`, `EnableClock`, `DisableClock`, `GetKernelSource`, `CalcPrescaler`, `CalcGuardCycles`, `Check*`, `GetIRQn`, `GetDmaRequest`, `TakeErrors` | yes | use `SPI_BM_*`, plus `SPI_LL_CheckConfig` | use `SPI_BM_*` |
| `Init` | yes | yes | yes (`SPI_BM_Deinit`, then `HAL_SPI_Init`) |
| `Deinit` | yes | use `SPI_BM_Deinit` | yes (→ BM, plus the handle to RESET) |
| `Transfer` (polling) | yes | yes | yes (`HAL_SPI_TransmitReceive` / `Transmit`) |
| `TransferStartIt` + `TransferIrq` | yes | yes | `StartTransferIt` + `HAL_SPI_IRQHandler` + `Wait` |
| `DmaPrepare`, `DmaStart`, `WaitEnd`, `DmaEnd` | yes | yes | `StartTransferDma` + `Wait` |

---

## 2. API summary

### 2.1 Status codes (`SpiBm_Status_t`, all three tiers)

| Status | Meaning |
| :--- | :--- |
| `SPI_BM_OK` | Written and read back; transfer complete |
| `SPI_BM_ERR_PARAM` | Refused before any peripheral access, RCC included |
| `SPI_BM_ERR_UNSUPPORTED` | The instance lacks the feature (frames above 16 bits on SPI4–6) |
| `SPI_BM_ERR_RANGE` | Not reachable: no `MBR` gives the SCK, or the FIFO threshold is above half the FIFO |
| `SPI_BM_ERR_BUSY` | The hardware would ignore the write: `SPE = 1`, `IOLOCK = 1`, `AFCNTR = 1` in force with another `CPOL`/`SSIOP`, not a master, a HAL handle not `READY` |
| `SPI_BM_ERR_VERIFY` | A register did not read back what was written |
| `SPI_BM_ERR_TIMEOUT` | The bound expired (`max_cycles`, `CYCCNT` not counting, `timeout_ms`, or the HAL tick stopped) |
| `SPI_BM_ERR_MODF` | Mode fault: the hardware cleared `SPE` and `MASTER`; `Init` again |
| `SPI_BM_ERR_OVERRUN` | `OVR`: received data lost (the transfer still ran to `EOT`) |
| `SPI_BM_PENDING` | `SpiBm_Xfer_t.status` while an interrupt transfer runs |
| `SPI_BM_ERR_HAL` | `spi_hal` only: a HAL call returned an error with no more specific code |

### 2.2 `spi_bm.h`

```c
bool           SPI_BM_IsValidInstance(const SPI_TypeDef *spi);       /* exact base of SPI1..6 */
SpiBm_Status_t SPI_BM_GetCaps(const SPI_TypeDef *spi, SpiBm_Caps_t *out);
SpiBm_Status_t SPI_BM_EnableClock(SPI_TypeDef *spi);    /* and DisableClock: this core's RCC bank */
SpiBm_Status_t SPI_BM_GetKernelSource(const SPI_TypeDef *spi, uint32_t *field);  /* raw SPIxSEL */
SpiBm_Status_t SPI_BM_CalcPrescaler(uint32_t kernel_hz, uint32_t max_sck_hz, uint32_t *mbr, uint32_t *sck_hz);
SpiBm_Status_t SPI_BM_CalcGuardCycles(uint32_t cpu_hz, uint32_t kernel_hz, uint32_t mbr, uint32_t *cycles);
SpiBm_Status_t SPI_BM_Init(const SpiBm_Config_t *cfg);             /* SPE = 0 required */
SpiBm_Status_t SPI_BM_Deinit(SPI_TypeDef *spi);                    /* reset image, pins released, no RCC */
SpiBm_Status_t SPI_BM_Transfer(SPI_TypeDef *spi, const void *tx, void *rx, uint32_t frames,
                               uint32_t guard_cycles, uint32_t max_cycles);   /* rx may be NULL */
SpiBm_Status_t SPI_BM_TransferStartIt(SPI_TypeDef *spi, SpiBm_Xfer_t *x);
bool           SPI_BM_TransferIrq(SPI_TypeDef *spi, SpiBm_Xfer_t *x);   /* true when x->status is final */
SpiBm_Status_t SPI_BM_DmaPrepare(SPI_TypeDef *spi, uint32_t frames, bool rx);   /* TSIZE, RXDMAEN */
SpiBm_Status_t SPI_BM_DmaStart(SPI_TypeDef *spi);                    /* TXDMAEN, SPE, CSTART */
SpiBm_Status_t SPI_BM_WaitEnd(SPI_TypeDef *spi, uint32_t max_cycles);   /* EOT, MODF or time; no disable */
SpiBm_Status_t SPI_BM_DmaEnd(SPI_TypeDef *spi, uint32_t guard_cycles);  /* after the streams stopped */
SpiBm_Status_t SPI_BM_CheckConfig(cfg), _CheckTransfer(...), _CheckDmaPrepare(...), _CheckDmaStart(spi);  /* no write */
SpiBm_Status_t SPI_BM_GetDmaRequest(const SPI_TypeDef *spi, bool tx, uint32_t *request_id);
SpiBm_Status_t SPI_BM_GetIRQn(const SPI_TypeDef *spi, IRQn_Type *out);
uint32_t       SPI_BM_TakeErrors(SPI_TypeDef *spi);   /* SPI_BM_ERRF_OVR/MODF/TIFRE; one IFCR write, DSB */
```

`SpiBm_Config_t` = `{ spi, mbr (0..7), mode (CPOL << 1 | CPHA), data_bits, lsb_first, direction
(SPI_BM_DIR_FULL_DUPLEX / _TX_ONLY), fifo_threshold (frames per event), nss (SPI_BM_NSS_SOFT / _HW /
_HW_PULSE), nss_active_high, mssi, midi, keep_pins }`. `SpiBm_Xfer_t` = `{ tx, rx, frames, guard_cycles,
tx_done, rx_done, status }`, owned by the caller. **Buffers:** one element per frame, right aligned:
`uint8_t` for 4..8-bit frames, `uint16_t` for 9..16, `uint32_t` for 17..32, aligned to the element; the
data register is accessed with that width, so frames are never packed.

### 2.3 `spi_ll.h`

`SpiLl_Config_t` = `{ spi, baudrate, clock_polarity, clock_phase, data_width, bit_order, transfer_direction,
fifo_threshold, nss, nss_pulse, nss_polarity, ss_idleness, interdata_idleness, keep_pins }` with the
`stm32h7xx_ll_spi.h` constants (`LL_SPI_BAUDRATEPRESCALER_*`, `_POLARITY_*`, `_PHASE_*`, `_DATAWIDTH_*`,
`_MSB_FIRST`/`_LSB_FIRST`, `_FULL_DUPLEX`/`_SIMPLEX_TX`, `_FIFO_TH_*`, `_NSS_SOFT`/`_HARD_OUTPUT`,
`_NSS_POLARITY_*`, `_SS_IDLENESS_*`, `_ID_IDLENESS_*`); any other (`LL_SPI_SIMPLEX_RX`, …) gives `ERR_PARAM`. Functions:
`SPI_LL_CheckConfig`, `SPI_LL_Init`, and `SPI_LL_Transfer`, `_TransferStartIt`, `_TransferIrq`, `_DmaPrepare`,
`_DmaStart`, `_WaitEnd`, `_DmaEnd` with the arguments of their `SPI_BM_` twins.

### 2.4 `spi_hal.h`

```c
SpiBm_Status_t SPI_HAL_Init(SPI_HandleTypeDef *h, const SpiBm_Config_t *cfg);    /* h->Instance == cfg->spi */
SpiBm_Status_t SPI_HAL_Transfer(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames,
                                uint32_t timeout_ms);
SpiBm_Status_t SPI_HAL_StartTransferIt(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames);
SpiBm_Status_t SPI_HAL_StartTransferDma(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames);
SpiBm_Status_t SPI_HAL_Wait(SPI_HandleTypeDef *h, uint32_t timeout_ms);
SpiBm_Status_t SPI_HAL_Deinit(SPI_HandleTypeDef *h);
```

Values are `spi_bm`'s. A NULL handle, or an `h->Instance` that is not an SPI or not `cfg->spi`, gives
`ERR_PARAM`; then `SPI_BM_Check*`, the handle's state (transfers need `READY`; `SPI_HAL_Init` does not check
it: it puts the handle to RESET after `SPI_BM_Deinit`), the HAL call and a read-back of what it wrote.
`StartTransferDma` needs `h->hdmatx` (and `h->hdmarx` with `rx`) linked, with a memory size of at least the
frame's (else `ERR_PARAM`: the HAL's refusal would leave the handle busy). `SPI_HAL_Deinit` =
`SPI_BM_Deinit` + the handle to RESET, no `HAL_SPI_DeInit` (no MSP). No guard in this tier (§10.1).

---

## 3. Configuration guide

1. **Clock.** `SPI_BM_EnableClock(spi)` (this core's RCC bank). The kernel clock is the project's:
   `SPI_BM_GetKernelSource(spi, &field)` returns the raw `SPI123SEL`/`SPI45SEL`/`SPI6SEL`, and the project
   knows what that source runs at. In `prj_01` every field is 0 (T10): SPI1–3 `pll1_q_ck` = 200 MHz,
   SPI4/5 `pclk2` = 100 MHz, SPI6 `pclk4` = 100 MHz, nominal (§6.3).
2. **Prescaler, guard.** `SPI_BM_CalcPrescaler(kernel_hz, max_sck_hz, &mbr, &sck_hz)`: the smallest `MBR`
   whose SCK is not above the maximum, and the real SCK (`ERR_RANGE` if /256 is not enough).
   `SPI_BM_CalcGuardCycles(cpu_hz, kernel_hz, mbr, &cycles)`: one SCK period in CPU cycles, rounded up, the
   `guard_cycles` of every BM/LL `Transfer`, `DmaEnd` and `SpiBm_Xfer_t` (ES0445 2.22.5, §6.2).
3. **Init.** `SPI_BM_Init(&cfg)` with `SPE = 0` (`SPI_LL_Init`, or `SPI_HAL_Init` with a handle).
4. **Pins.** Then the pins, with `gpio_bm` (`GPIO_BM_ConfigAlternateEx`, AF of DS12923 table 9): the pull
   of SCK at `CPOL`, the pull of SS at its inactive level. **SCK must be in its AF** for a transfer to
   end (§10.2).
5. **Transfer.** `Transfer`, `TransferStartIt` or the DMA sequence. Each call is one transaction: `TSIZE`,
   `SPE = 1`, `CSTART`, `EOT`, the guard, `SPE = 0` (ES0445 2.22.1).

- **SS by software** (`SPI_BM_NSS_SOFT`): `SSM = 1` with `SSI` inactive; the caller drives a GPIO around
  the call (example A). `mssi` must be 0.
- **SS by hardware** (`SPI_BM_NSS_HW`): `SSOE = 1`, SS active from `CSTART` to `EOT`, polarity `SSIOP`,
  `mssi` SCK periods before the first edge. `SPI_BM_NSS_HW_PULSE` (`SSOM = 1`) releases SS between frames
  only with `midi` ≥ 2 (RM0399 53:3470, 53:615-616); the driver does not check it (T5(c) uses 2).
- **`keep_pins`** (`AFCNTR = 1`): the SPI holds SCK at `CPOL` while disabled (T5(e)). A later `Init` with
  another `CPOL` or `SSIOP` is refused with `ERR_BUSY`: `Deinit` first.
- **Interrupt:** fill a `SpiBm_Xfer_t` (`tx`, `rx`, `frames`, `guard_cycles`), enable the vector of `GetIRQn`, call
  `TransferStartIt`; the project's `SPIx_IRQHandler` calls `TransferIrq` until it returns true (ES0445 2.22.3: §6.2).
- **DMA (rule 8, RM0399 53:1262-1274):** `GetDmaRequest(spi, tx, &req)` gives the DMAMUX request
  (DMAMUX1 for SPI1–5, DMAMUX2 for SPI6, T7(d)). Order: `DmaPrepare` (`TSIZE`, `RXDMAEN`) → the caller
  programs and starts its RX and TX streams (`02_dma`) → `DmaStart` (`TXDMAEN`, `SPE`, `CSTART`) →
  `WaitEnd` (ends on `EOT`, with `ERR_OVERRUN` if `OVR` was set; on `MODF`; or on time: `OVR` alone does
  not end it) → the caller stops its streams → `DmaEnd` (guard, `SPE = 0`,
  `TXDMAEN = RXDMAEN = 0`, flags). For DMA1/DMA2 with `dma_hal`, read N3 (§10.3).
- **BDMA (SPI6 only):** it reaches SRAM4 and the backup SRAM, not the D1/D2 RAMs (`02_dma`): buffers in
  `.d3_ram` (SRAM4), from **one core only** (SP3, §10.3); requests 11/12 (T7(b), example C).

---

## 4. Project contract

- **One owner core per instance,** no cross-core lock. `prj_01` selftest: CPU1 owns SPI2–6, CPU2 SPI1.
- **RCC banks (RM0399 9.5.10):** per core; an SPI runs while either core enables it, so `DisableClock`
  on one core does not stop it (T13). `RCC_APBxRSTR` is never written.
- **Kernel multiplexer:** `RCC_D2CCIP1R`/`RCC_D3CCIPR` are the project's; the driver only reads them.
- **No callback, no MSP, no vector in the driver.** `HAL_SPI_Init` calls the project's `HAL_SPI_MspInit`
  (in `prj_01`, the HAL's empty weak one); the project's handlers call `HAL_SPI_IRQHandler`/`HAL_DMA_IRQHandler`.
- **CPU1's HAL tick is TIM6** in `prj_01` (`TICK_INT_PRIORITY 0`): `SPI_HAL_Wait` needs it running (§10.1).

---

## 5. Rules the driver enforces

- **C6, validate first.** Every invalid or unsupported parameter is refused before the first access, RCC
  included, in a fixed order: NULL, instance, capability of the instance, field range, state.
- **C7, C14:** no state in RAM (the interrupt transfer's state is the caller's `SpiBm_Xfer_t`); no vector,
  no `NVIC_*`, no callback (`GetIRQn` only returns the number).
- **C10, concurrency on one core.** Flags are cleared with one `IFCR` write; the read-modify-writes of
  `CR1`, `CFG1` and `IER` run with PRIMASK saved and restored (never `__enable_irq()`).
- **C11, no glitch.** `Init` (rule 3, R2-1): `IER = 0`, flags, `CFG2 = 0` if the master in force has
  `SSM = 1` and another `SSIOP`, `CR1` with `SSI` inactive, `CFG1`, `CFG2` (with `AFCNTR` last). `Deinit`
  (rule 9, R2-2): `IER = 0`, `SPE = 0` keeping `SSI`, `CFG2 = 0` (pins released), `CR1`, `CR2`, `CFG1`
  (`0x00070007`), flags; no RCC write.
- **C12, honesty.** Every configuration register is read back (`ERR_VERIFY`), and `SR.MODF = 0` after
  `Init`. A write the hardware would ignore is refused with `ERR_BUSY`: configuration or `TSIZE` with
  `SPE = 1` (RM0399 53:3273-3275, 53:3302). `CalcPrescaler` returns the real SCK, not the requested one.
- **C13, bounded waits.** BM/LL waits are bounded by `max_cycles` DWT cycles the caller passes; a
  `CYCCNT` that is not counting, or reads the same value more than 1000 times, is `ERR_TIMEOUT`. An abort runs
  `IER = 0`, `SPE = 0` (RM0399 53:1071), flags. `SPI_HAL_Wait` is bounded by `timeout_ms` of
  `HAL_GetTick()` and by a count of polls with the tick stopped (m4, §10.1).

---

## 6. Silicon facts and errata

### 6.1 Capability table

`k_spi[]` in `spi_bm.c`, each row citing RM0399 rev 4; T2 compares `GetCaps` with table 435 typed by hand
in the test, and T7(d) `GetDmaRequest`/`GetIRQn` with `stm32h7xx_hal_dma.h` and `stm32h755xx.h`.

| Instance | Bus, enable bit | Domain | Frame | FIFO | Kernel field | DMAMUX: RX / TX | IRQn |
| :--- | :--- | :---: | :---: | :---: | :--- | :--- | :---: |
| SPI1 | APB2, bit 12 | D2 | 4–32 | 16 B | `SPI123SEL` | 1: 37 / 38 | 35 |
| SPI2 | APB1L, bit 14 | D2 | 4–32 | 16 B | `SPI123SEL` | 1: 39 / 40 | 36 |
| SPI3 | APB1L, bit 15 | D2 | 4–32 | 16 B | `SPI123SEL` | 1: 61 / 62 | 51 |
| SPI4 | APB2, bit 13 | D2 | 4–16 | 8 B | `SPI45SEL` | 1: 83 / 84 | 84 |
| SPI5 | APB2, bit 20 | D2 | 4–16 | 8 B | `SPI45SEL` | 1: 85 / 86 | 85 |
| SPI6 | APB4, bit 5 | D3 | 4–16 | 8 B | `SPI6SEL` | 2: 11 / 12 (BDMA) | 86 |

The FIFO threshold is t frames per event with t × bytes per frame (1, 2, 3 or 4) ≤ FIFO / 2 (RM0399
53:3394), else `ERR_RANGE`. DS12923 says "4 to 16 bits" and "8x 8-bit FIFOs" for all six: the RM rules.

### 6.2 Errata (ES0445, 2.22)

| ID | Erratum | `spi_bm`, `spi_ll` | `spi_hal` |
| :--- | :--- | :--- | :--- |
| 2.22.1 | A `CSTART` less than one SCK after `EOT` can stall the master | One transaction per enable: `SPE = 0` after every `EOT`. T12(a): 10 000 one-frame transfers at /256, 0 `ERR_TIMEOUT`; control by hand (`CSTART` again with `SPE = 1`): 20 stalls in 40 | The HAL also closes every transfer with `SPE = 0` (`SPI_CloseTransfer`) |
| 2.22.2 | CRC error with `UDRDET ≠ 0` (slave) | Out of scope (no slave, no CRC) | Same |
| 2.22.3 | `TXP`/`TXC` raise the IRQ with `SPE = 0` | `IER` only while `SPE = 1`: written after it, cleared before it. T6(c): 0 IRQs in the 10 ms after each end; control `TXPIE = 1` by hand: 2 (CPU1), 1 (CPU2) | **Not met in one HAL path:** when every frame fits the FIFO pre-fill, `DXPIE` is still set when the close clears `SPE` before `IER`: one IRQ follows the close (T6: 1 with 1 frame, 0 with FIFO + 1 and 1024). Kept as the HAL does it (owner's decision, 2026-10-04); the handler must accept it (§10.1) |
| 2.22.4 | `CRCSIZE < 00111` with `CRCEN = 0` and `TSIZE > 0` | `CRCSIZE = 00111`, `CRCEN = 0` in every image (T2, 174 080 + 40 960 configurations) | Needs `USE_SPI_CRC = 0` (`#error`); `SPI_HAL_Init` starts from the reset image, and the HAL keeps its `00111` (T2: equal to the raw `HAL_SPI_Init`) |
| 2.22.5 | Truncated outputs if `SPE = 0` comes too soon after `EOT` | `guard_cycles` (one SCK period, `CalcGuardCycles`) between `EOT` and `SPE = 0`, also in the ISR and `DmaEnd`. T12(b), `CPHA = 0`, /256: last SCK high pulse 126 samples, half the measured period 126.4 (± 2). Control without the guard (decision 6): 156, not reproduced in this run; reproduced (1–2 samples) in 5 of the 8 clean runs of §9.1 | **No guard:** `SPI_CloseTransfer` clears `SPE` right after `EOT` (decision 5, §10.1). Use BM or LL with `CPHA = 0` and a slow SCK |

### 6.3 The board clock (SP4, N1)

The HSE (ST-LINK MCO) runs about +1.3 % fast (`03_tim` N1: TREF ē = +13 273 ppm, `tim_f3_audit_bench_70b2062`):
`pll1_q_ck` near 202.6 MHz, above the 200 MHz of RM0399 table 62 for the SPI1–3 kernel in VOS1 (SP4); SPI6 at /2
near 50.6 MHz. Open, outside the driver. The tests do not depend on it: T5(g) is a ratio to MCO1 = HSE/8 in
the same capture; T5(d) and T12(b) count in SCK periods measured in the same capture (R3-5); T9 in CPU cycles.

---

## 7. Examples

Each compiles with `-Wall -Wextra -Wpedantic -Wconversion -Werror` (CMSIS/HAL headers as `-isystem`;
A for CM4, B and C for CM7 with `HAL_SPI_MODULE_ENABLED`). Pins are those of the bench (§8), except the CS
of example A, PD14: a pin `prj_01` does not use, not wired on the bench.

### A. Polling with CS on a GPIO, SPI1 from CPU2 (`spi_bm`)

```c
bool read_id_spi1(uint8_t id[3])
{
    static const uint8_t cmd[4] = { 0x9FU, 0U, 0U, 0U };
    uint8_t rx[4] = { 0U, 0U, 0U, 0U };
    uint32_t sel = 0U, mbr = 0U;
    uint32_t sck_hz = 0U, guard = 0U;
    const uint32_t kernel_hz = 200000000U;          /* SPI123SEL = 0: pll1_q_ck in prj_01 */
    if ((SPI_BM_EnableClock(SPI1) != SPI_BM_OK) || (SPI_BM_GetKernelSource(SPI1, &sel) != SPI_BM_OK) ||
        (sel != 0U) || (SPI_BM_CalcPrescaler(kernel_hz, 10000000U, &mbr, &sck_hz) != SPI_BM_OK) ||
        (SPI_BM_CalcGuardCycles(SystemCoreClock, kernel_hz, mbr, &guard) != SPI_BM_OK))
    {
        return false;
    }
    const SpiBm_Config_t c = { SPI1, mbr, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U, SPI_BM_NSS_SOFT,
                               false, 0U, 0U, false };
    if ((SPI_BM_Init(&c) != SPI_BM_OK) ||               /* the SPI first, then its pins */
        (GPIO_BM_ConfigOutput(GPIOD, 14U, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE,
                              1U) != GPIO_BM_OK) ||     /* CS inactive (high) */
        (GPIO_BM_ConfigAlternateEx(GPIOA, 5U, 5U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_DOWN,
                                   GPIO_BM_SPEED_HIGH) != GPIO_BM_OK) ||   /* SCK, pull = CPOL */
        (GPIO_BM_ConfigAlternateEx(GPIOA, 6U, 5U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE,
                                   GPIO_BM_SPEED_HIGH) != GPIO_BM_OK) ||   /* MISO */
        (GPIO_BM_ConfigAlternateEx(GPIOB, 5U, 5U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE,
                                   GPIO_BM_SPEED_HIGH) != GPIO_BM_OK))     /* MOSI */
    {
        return false;
    }
    GPIO_BM_Reset(GPIOD, 1UL << 14);
    const SpiBm_Status_t st = SPI_BM_Transfer(SPI1, cmd, rx, 4U, guard, 100000U);
    GPIO_BM_Set(GPIOD, 1UL << 14);
    id[0] = rx[1];
    id[1] = rx[2];
    id[2] = rx[3];
    return st == SPI_BM_OK;
}
```

### B. Interrupt transfer, SPI6 from CPU1 (`spi_ll`)

```c
static SpiBm_Xfer_t s_x;                 /* the transfer's state is the caller's (C7) */

void SPI6_IRQHandler(void) { (void)SPI_LL_TransferIrq(SPI6, &s_x); }   /* the project's vector (C14) */

bool start_spi6_it(const uint16_t *tx, uint16_t *rx, uint32_t frames)
{
    const SpiLl_Config_t c = { SPI6, LL_SPI_BAUDRATEPRESCALER_DIV8, LL_SPI_POLARITY_HIGH, LL_SPI_PHASE_2EDGE,
                               LL_SPI_DATAWIDTH_16BIT, LL_SPI_MSB_FIRST, LL_SPI_FULL_DUPLEX,
                               LL_SPI_FIFO_TH_01DATA, LL_SPI_NSS_SOFT, false, LL_SPI_NSS_POLARITY_LOW,
                               LL_SPI_SS_IDLENESS_00CYCLE, LL_SPI_ID_IDLENESS_00CYCLE, false };
    IRQn_Type irq = SPI6_IRQn;
    uint32_t guard = 0U;
    if ((SPI_BM_EnableClock(SPI6) != SPI_BM_OK) || (SPI_LL_Init(&c) != SPI_BM_OK) ||
        (GPIO_BM_ConfigAlternateEx(GPIOA, 5U, 8U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_UP,
                                   GPIO_BM_SPEED_HIGH) != GPIO_BM_OK) ||   /* SCK, pull = CPOL 1 */
        (GPIO_BM_ConfigAlternateEx(GPIOA, 6U, 8U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE,
                                   GPIO_BM_SPEED_HIGH) != GPIO_BM_OK) ||
        (GPIO_BM_ConfigAlternateEx(GPIOB, 5U, 8U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE,
                                   GPIO_BM_SPEED_HIGH) != GPIO_BM_OK) ||
        (SPI_BM_GetIRQn(SPI6, &irq) != SPI_BM_OK) ||
        (SPI_BM_CalcGuardCycles(SystemCoreClock, 100000000U, 2U, &guard) != SPI_BM_OK))   /* MBR /8 = 2 */
    {
        return false;
    }
    s_x.tx = tx;
    s_x.rx = rx;
    s_x.frames = frames;
    s_x.guard_cycles = guard;            /* ES0445 2.22.5, applied in the ISR */
    NVIC_SetPriority(irq, 6U);
    NVIC_EnableIRQ(irq);
    return SPI_LL_TransferStartIt(SPI6, &s_x) == SPI_BM_OK;   /* then s_x.status leaves SPI_BM_PENDING */
}
```

### C. SPI6 with the BDMA, CPU1 code with a handle (`spi_hal`)

```c
static SPI_HandleTypeDef h_spi6;
static DMA_HandleTypeDef h_rx, h_tx;
static uint8_t s_tx[64] __attribute__((section(".d3_ram"), aligned(32)));
static uint8_t s_rx[64] __attribute__((section(".d3_ram"), aligned(32)));

void SPI6_IRQHandler(void)            { HAL_SPI_IRQHandler(&h_spi6); }   /* the project's (C14) */
void BDMA_Channel0_IRQHandler(void)   { HAL_DMA_IRQHandler(&h_rx); }
void BDMA_Channel1_IRQHandler(void)   { HAL_DMA_IRQHandler(&h_tx); }

static bool bdma(DMA_HandleTypeDef *h, BDMA_Channel_TypeDef *ch, uint32_t req, uint32_t dir)
{
    const DmaHal_Config_t c = { ch, req, dir, DMA_PINC_DISABLE, DMA_MINC_ENABLE, DMA_PDATAALIGN_BYTE,
                                DMA_MDATAALIGN_BYTE, DMA_NORMAL, DMA_PRIORITY_HIGH, DMA_FIFOMODE_DISABLE,
                                DMA_FIFO_THRESHOLD_FULL, DMA_MBURST_SINGLE, DMA_PBURST_SINGLE, true, 6 };
    return DMA_HAL_Init(h, &c) == HAL_OK;
}

bool exchange_spi6_bdma(void)                 /* thread mode: SPI_HAL_Wait needs the tick (m4) */
{
    const SpiBm_Config_t c = { SPI6, 2U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U, SPI_BM_NSS_SOFT,
                               false, 0U, 0U, false };
    IRQn_Type irq = SPI6_IRQn;
    h_spi6.Instance = SPI6;
    if ((SPI_BM_EnableClock(SPI6) != SPI_BM_OK) || (SPI_HAL_Init(&h_spi6, &c) != SPI_BM_OK) ||
        (GPIO_BM_ConfigAlternateEx(GPIOA, 5U, 8U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_DOWN,
                                   GPIO_BM_SPEED_HIGH) != GPIO_BM_OK) ||
        (GPIO_BM_ConfigAlternateEx(GPIOA, 6U, 8U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE,
                                   GPIO_BM_SPEED_HIGH) != GPIO_BM_OK) ||
        (GPIO_BM_ConfigAlternateEx(GPIOB, 5U, 8U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE,
                                   GPIO_BM_SPEED_HIGH) != GPIO_BM_OK) ||
        !bdma(&h_rx, BDMA_Channel0, BDMA_REQUEST_SPI6_RX, DMA_PERIPH_TO_MEMORY) ||
        !bdma(&h_tx, BDMA_Channel1, BDMA_REQUEST_SPI6_TX, DMA_MEMORY_TO_PERIPH) ||
        (SPI_BM_GetIRQn(SPI6, &irq) != SPI_BM_OK))
    {
        return false;
    }
    __HAL_LINKDMA(&h_spi6, hdmarx, h_rx);
    __HAL_LINKDMA(&h_spi6, hdmatx, h_tx);
    HAL_NVIC_SetPriority(irq, 6U, 0U);
    HAL_NVIC_EnableIRQ(irq);
    return (SPI_HAL_StartTransferDma(&h_spi6, s_tx, s_rx, 64U) == SPI_BM_OK) &&
           (SPI_HAL_Wait(&h_spi6, 10U) == SPI_BM_OK);
}
```

---

## 8. Test bench (Analog Discovery 2)

The bench of `00_gpio/README.md` §8, not one wire changed (connector pins there), **plus the jumper J1**:
NUCLEO-H755ZI-Q (MB1363) and AD2 SN `210321B5D38D`, WaveForms GUI closed, `tools/ad2.py` over ctypes. The
AD2 never uses Digital Out in this bench. The lines the SPI tests use:

```text
 AD2                     NUCLEO-H755ZI-Q
 DIO3  ◄───────────────  PE11  SPI4_NSS  (AF5): hardware SS (T5)           CN10-6
 DIO4  ◄───────────────  PE12  SPI4_SCK  (AF5): SCK (T5, T12(b))            CN10-26
 DIO5  ── (input) ─────  PE13  SPI4_MISO (AF5, input): reads its pull (T5(f)) CN10-10
 DIO6  ◄───────────────  PE14  SPI4_MOSI (AF5): MOSI (T5, T7(c))            CN10-8
 DIO0-2, DIO7, DIO8-11  PE8-PE10, PE15 (never AF, S5 pulls); PE3-PE6 (inputs, never AF nor output)
 DIO12 ───────────────►  PE2   HOST_REQ            DIO13 ◄──────  PE7   MCU_ACK
 DIO14 ◄───────────────  PA8   MCO1 = HSE/8 (AF0): reference of T5(g), step SPI4_AD2
 GND   ─────────────────  GND  (first on, last off)    W1, W2, V+, V−: never connected

 J1 (male-male wire on CN7, no resistor):
   CN7-14  PB5  SPI1_MOSI (AF5) / SPI6_MOSI (AF8)  ──┐
   CN7-12  PA6  SPI1_MISO (AF5) / SPI6_MISO (AF8)  ──┘  loop (T4, T6-T9, T11(b), T12(a), T15, B1)
   CN7-10  PA5  SCK of SPI1/SPI6 (not observed)    CN7-19  PB4  alternative MISO with a pull (controls)
```

| Line | MCU | SPI use | Tests | Who drives |
| :--- | :--- | :--- | :--- | :--- |
| DIO3, DIO4, DIO6 | PE11, PE12, PE14 | SPI4 NSS, SCK, MOSI (AF5) | T5, T7(c), T9, T12(b) | MCU, only in a "bus = MCU" step (S13) |
| DIO5 | PE13 | SPI4 MISO (AF5), input with a pull | T5(f) | nobody |
| DIO0–2, DIO7; DIO8–11 | PE8–PE10, PE15; PE3–PE6 | none (never AF) | T0a, S5 | bus; nobody |
| DIO12 / DIO13 | PE2 / PE7 | `HOST_REQ` / `MCU_ACK` | every handshake | AD2 / MCU |
| DIO14 | PA8 | MCO1 (AF0) | T5(g) only (step `SPI4_AD2`) | MCU only |
| J1-A | PB5 | MOSI of SPI1/SPI6 | T4, T6–T9, T11(b), T12(a), T15, B1; GPIO output only in T0c | MCU |
| J1-B | PA6 | MISO of SPI1/SPI6 | same; **never an output** (S14) | PB5 through J1 |
| - | PA5 | SCK of SPI1/SPI6 | same; input in T15 | MCU |
| - | PB4 | alternative MISO | T4/T8 controls, input with a pull; **never an output** | nobody |

**Electrical safety.** No series resistors: a contention can damage a pin silently. S1–S12 of
`00_gpio/README.md` §8 and `03_tim/README.md` §8 apply as they are. This driver adds:

| # | Rule | Where |
| :--- | :--- | :--- |
| S13 | **SPI4 on the bus.** PE11, PE12, PE14 go to AF5 only in a "bus = MCU" step; PE13 to AF5 as an input in the same step. `bench_claim()` reads SPI4 first: clock in CPU1's bank, `SPE = 0`, `AFCNTR = 0`, `MASTER = 1` (or `CFG2 = 0`). With `SPE = 0` and `AFCNTR = 0` the SPI drives nothing (RM0399 53:3456-3464), so the S5 test runs with the pins already in AF5. A step with `keep_pins` ends with `Deinit` before the bus is returned | `CM7/App/Src/spi_selftest.c`; T0b(e) |
| S14 | **J1.** PA6 and PB4 are never GPIO outputs; PB5 is an output only in T0c. One core per step owns PA5, PA6, PB4, PB5 (CPU2 for SPI1, CPU1 for SPI6). Before an AF on J1, `bench_claim()` reads that core's SPI: clock, `SPE = 0`, `AFCNTR = 0`, `IOSWP = 0`, `MASTER = 1` or `CFG2 = 0` (R2-3). `bench_safe()` leaves the four pins as inputs and releases the SPI with rule 9 | `bench_claim()` on both cores; T0b(f) |

Never touched: PE0, PE1, PC13, PA4, PA7, PA13/PA14, TIM6, SysTick, `RCC_D2CFGR`, `RCC_D2CCIP1R`,
`RCC_D3CCIPR`, `RCC_APBxRSTR`, `DBGMCU` (write); `RCC_CFGR` (`MCO1`) only in the step `SPI4_AD2`, saved and
restored with read-back. **Procedure:** GND first, then the DIO lines and J1; flash with the script stopped;
cold start; run the host (§9.3); stop it before reflashing or unwiring.

---

## 9. Validation

`prj_01_template_dualcore`, preset `selftest-spi`: `C*/App/Src/spi_selftest.c`, `Common/Src/spi_selftest_common.c`
(block at `0x3800C000`, SRAM4), host `tools/spi_hw_check.py` (protocol v5). CPU1 prints the global verdict.
Every test that has a negative control prints it; no oracle is the driver (register snapshots, RM0399 fields typed by
hand, the raw `HAL_SPI_Init`, the wire, `CYCCNT`, the AD2 with the host's decoder, the host's arithmetic).
With the option off, production keeps the sections of `2481396` (`docs/spi_f3_map_sizes_8b5d165.txt`).

### 9.1 Captures

All in `03_firmware_templates/prj_01_template_dualcore/docs/`, one unedited run each; the clean ones have
the same `Rev` and build id on both cores and the host.

| Phase | Auditor's run | Delivered run |
| :--- | :--- | :--- |
| F0 bench | `spi_f0_audit_bench_620a678.txt`: 10/10, PASS | `spi_f0_bench_008db8b.txt`, `spi_f0_bench_efc1be8.txt`: 10/10, PASS |
| F1 `spi_bm` | `spi_f1_audit_bench_b4fa22d.txt`: 29/29, PASS | `spi_f1_bench_4142921.txt`: 29/29, PASS (replaced `spi_f1_bench_c41040a.txt`, 28/28) |
| F2 `spi_ll` | `spi_f2_audit_bench_99fe89b.txt`: 38/38, PASS | `spi_f2_bench_73c1b76.txt`: 38/38, PASS |
| F3 `spi_hal` | `spi_f3_audit_bench_b267f4c.txt`: 45/45, PASS | `spi_f3_bench_efb4a3f.txt`: 45/45, PASS |
| Close (m4) | - | **`spi_f3_bench_8b5d165.txt`: 45/45, PASS** (the final code) |

Failed runs kept: `spi_f0_bench_65e19f7.txt` (the `SPE = 1` case of T0b(e) read `SPE = 0`),
`spi_f0_audit_bench_153d5b3.txt` (9/10, T0B(d): F0 audit finding B1), the development runs
`spi_dev_f1_bench_25426f8-dirty*` (transfers without SCK pins never end, E2; T5(d) against the nominal
period) and `spi_dev_f3_bench_dbab128-dirty.txt`, `_2` (T6 HAL read-back of `TSERFIE`; T7(c) HAL and N3).
`_3` passed (45/45) before T6 counted the IRQs after the close.

### 9.2 Tests

Values from `spi_f3_bench_8b5d165.txt`; "BM/LL/HAL" are the tiers the test runs through.

| Test | Tiers, cores | What | Result | Negative control |
| :--- | :--- | :--- | :--- | :--- |
| T0a–T0c | bench | 13 lines loose and walking 1; claims and guards (S13, S14); J1 | 0x4FFF / 0x0000, 13 patterns, 0 mismatches; PB5 wrote 0x5A3C on PA6 over 16 steps | DIO9 pulled the wrong way: flagged 0x0200; `AFCNTR = 1`: S5 fails as it must |
| T1 | BM CM7/CM4, LL CM7/CM4, HAL CM7 | refusals, with the instance's registers and both RCC banks read before and after | 52 + 35 (BM), 48 + 40 (LL), 33 (HAL): right status, nothing touched | a valid `Init` changes the snapshot |
| T2 | BM, LL CM7/CM4; HAL CM7 | register image of 40 960 configurations on SPI1–3 and 30 720 on SPI4–6, against RM0399 fields typed by hand; LL and HAL also against BM, HAL also against the raw `HAL_SPI_Init` | 0 mismatches; HAL = raw HAL in 0; HAL against BM only `CR1.SSI` with hardware SS active low (§10.1); `GetCaps` = table 435 | `MASTER` flipped in the expected image: caught |
| T3 | BM + host | `CalcPrescaler` (202 vectors), `CalcGuardCycles` (102), recomputed by the host | 0 differ | `MBR + 1`: 153/153 flagged; one cycle more: 83/83 flagged |
| T4 | BM, LL, HAL SPI6 (CPU1); BM, LL SPI1 (CPU2) | loop through J1 at 12.5 MHz: modes × sizes × order × 1..4096 frames × threshold; 65 535 frames of 8 bits on SPI6 | 896 transfers (SPI6) and 1408 (SPI1) per tier, 0 failed; host CRC and bit coverage 0 differ; 65 535 frames (SPI6 only): 0 mismatches | MISO moved to PB4 pulled up/down: mismatches = the host's count |
| T5 | BM (a)–(g); LL, HAL (a) | SPI4 decoded by the AD2: (a) modes × {4, 8, 13, 16} × order, hardware SS; (b) `SSIOP`; (c) SS pulse; (d) `MSSI`/`MIDI`; (e) `keep_pins`; (f) RX = pull; (g) SCK/MCO1 | (a) 32 captures per tier, 0 failed; (d) k periods ± 2 samples; (e) SCK held at `CPOL`; (f) exact; (g) /4…/256 within −312…+220 ppm (± 0.5 %) | (a) decoded with the opposite `CPHA` and order: mismatches in 32 of 32; (e) `keep_pins = 0`: SCK follows the pull |
| T6 | BM, LL, HAL SPI6; BM, LL SPI1 | interrupt transfers of 1, FIFO + 1, 1024 frames; (c) IRQs after the end | 0 failed; (c) 0 in 10 ms; after the HAL's close: 1 with 1 frame, as expected (§6.2) | `TXPIE = 1` by hand with `SPE = 0`: 2 (CPU1), 1 (CPU2) spurious IRQs |
| T7 | (a) BM, LL SPI1 DMA2; (b) BM, LL, HAL SPI6 BDMA; (c) BM, HAL SPI4 DMA1; (d) BM | DMA up to 4096 / 8192 frames; (c) decoded by the AD2; (d) `GetDmaRequest`, `GetIRQn` | 0 failed; (d) 0 differ | `RXDMAEN = 0`: `ERR_OVERRUN`, 255 sentinels; HAL: no RX stream, 255 sentinels |
| T8 | BM | speed sweep /256…/2, 1024 frames | pass required up to 25 MHz; recorded: SPI6 passed at 50 MHz, SPI1 at 100 MHz (nominal) | MISO on PB4 at /2: 1021 (SPI6), 1019 (SPI1) mismatches |
| T9 | BM SPI6, SPI4, SPI1 | t(2000) − t(1000) frames against `DSIZE` × 2^(MBR+1) kernel ticks, `CYCCNT` | within −101…+70 ppm (± 1 %) | `MBR + 1`: −50 % |
| T10 | BM | `GetKernelSource` against `RCC_D2CCIP1R`/`RCC_D3CCIPR` read directly | 0 on all six, 0 differ | - |
| T11 | BM | (a) `MODF` on SPI2 by hand; (b) `OVR` on SPI6, `TakeErrors` | `ERR_MODF`, `SPE = MASTER = 0`, `Deinit` + `Init` recover; `TakeErrors` 0x040, all cleared | `SSI` inactive: no `MODF` |
| T12 | BM | (a) 2.22.1; (b) 2.22.5 | (a) 0 timeouts in 10 000; (b) 126 samples against 126.4 | (a) 20 stalls in 40; (b) 156 samples: not reproduced here (1–2 samples in 5 of 8 clean runs, §6.2) |
| T13 | BM CPU1/CPU2 | RCC banks: `DisableClock(SPI2)` from CPU2 while CPU1 sends by DMA | SPI2 keeps running (`CTSIZE` 63 583 → 60 653) | CPU1 disables last: stopped (57 723 → 57 723) |
| T14 | all | `Deinit` against the RM0399 reset image after every test; S5 after each pin step | 0x00 everywhere | before the `Deinit`: 0x0D / 0x0C |
| T15 | BM, LL SPI6 | PA5 (SCK) as an input: `Transfer` | `ERR_TIMEOUT` after 401 018 (BM) / 401 071 (LL) cycles, `max_cycles` 400 000 (+10 %); then `SPE = 0`, `IER = 0`, reset image | PA5 in AF8: `OK` after 3056 / 3191 cycles, 0xA5 received |
| m4 | HAL | `SPI_HAL_Wait(10 ms)` with PRIMASK set, handle busy | `ERR_TIMEOUT` after 14 400 178 cycles, `HAL_GetTick()` unchanged | tick running: `ERR_TIMEOUT` after 10 ms |
| Tick, stack | HAL | `HAL_GetTick` during and after the HAL steps; `Status_Task` | 12 `TICK` lines strictly rising, the last after `DONE`; 640 words free of 1024 | - |

### 9.3 Reproducing it

```powershell
.\tools\env.ps1
cd 03_firmware_templates\prj_01_template_dualcore
cmake --preset selftest-spi; cmake --build --preset selftest-spi
py tools/flash.py --preset selftest-spi                      # with the host script stopped, then a cold start
py tools/spi_hw_check.py --checklist-confirmed <name>       # writes docs/spi_f3_bench_<rev>.txt
py tools/spi_hw_check.py --allow-dirty --checklist-confirmed <name>   # development: spi_dev_f3_bench_<rev>.txt
cd ..\..; .\tools\build_all.ps1 -Clean                      # 8/8
```

The host refuses a `Rev`/build id that is not this checkout's, WaveForms or a debugger running.

### 9.4 Measurements (B1 of the plan)

Cycles of one call, DWT, `Init` and `Transfer` with PRIMASK, SPI6 on CM7 (400 MHz) and SPI1 on CM4
(200 MHz), `Transfer` of 1 frame at /2, transmit only, Debug (`-Og`), in `spi_f3_bench_8b5d165.txt`:

| Function | BM CM7 | LL CM7 | HAL CM7 | BM CM4 | LL CM4 |
| :--- | ---: | ---: | ---: | ---: | ---: |
| `Init` | 489 | 936 | 1558 | 473 | 891 |
| `Transfer` | 1451 | 1954 | 2220 | 1446 | 1605 |
| Per IRQ (`TransferIrq`; HAL: `HAL_SPI_IRQHandler`) | 359–824 | 524–984 | 202–992 | 354–543 | 354–678 |

`CalcPrescaler`, worst case over the T3 sweep: 97 cycles on CM7. `SPI_HAL_Init` includes `SPI_BM_Deinit`.

### 9.5 Not exercised

- **SCK of SPI1/SPI6 and hardware SS of SPI1/2/3/5/6:** no AD2 line (decision 3).
- **SPI2, SPI3, SPI5 have no pins** on the bench and so no transfer that ends (§10.2): only their register
  image (T2), `MODF` without a transfer (T11(a), SPI2) and the RCC banks (T13, SPI2).
- **RX from another device:** only the loop and the pulls. Speeds above 25 MHz only recorded (T8).
- **BM and LL:** `TransferIrq` with `MODF` or `OVR`; the C13 path with `CYCCNT` not counting or stopped in
  `Transfer`, `WaitEnd` and `TransferStartIt` (and inside a guard, m1, accepted by reading). `keep_pins`
  only on SPI4 with `CPOL = 0` (T5(e)); with LL and HAL only in T1/T2. 65 535 frames only on SPI6, 8 bits,
  by polling. T5(b)–(g), T8, T9, T11–T13 not through LL nor HAL.
- **HAL:** `StartTransferIt` without `rx`, and IT on SPI4; `ERR_MODF`/`ERR_OVERRUN` through the HAL; the
  `ERR_TIMEOUT` of `SPI_HAL_Transfer` (the HAL's own); the HAL's error path (§10.1); `spi_hal` on CM4.
- **Never produced:** `ERR_VERIFY` in any tier (the one in the F3 development runs was a read-back defect,
  fixed in `3c23568`); ES0445 2.22.2. Silicon other than Rev V; any clock tree other than `prj_01`'s,
  except the pure functions (T3).

---

## 10. Known limitations

### 10.1 `spi_hal` (from `spi_hal.h`)

- **2.22.5:** no guard (decision 5); **2.22.3:** one IRQ after the close, with `h->State = READY` (§6.2).
- **`SPI_HAL_Wait`:** call it in thread mode with PRIMASK clear, or from an ISR below the tick and the
  SPI/DMA priorities. Otherwise it returns `ERR_TIMEOUT` after 2 × (`SystemCoreClock` / 1000) × the tick
  period polls with the same `HAL_GetTick()` (m4, `1c8ab15`). **`SPI_HAL_Transfer` has no such exit:**
  its bound is the HAL's own timeout, on the same tick.
- **`keep_pins`:** `HAL_SPI_Init` writes `CFG2` (`stm32h7xx_hal_spi.c` l. 398), then `AFCNTR` (l. 474):
  an `Init` releases the pins for a few cycles. **By design:** with hardware SS the HAL leaves
  `CR1.SSI = 0` (l. 375-381), where `spi_bm` writes it inactive (no effect with `SSM = 0`; the only T2 bit).
- **`rx == NULL`** goes through `HAL_SPI_Transmit*`: `CFG2.COMM` = transmit only until a transfer with `rx`.
- **`IER.TSERFIE`** stays at 1 after an IT transfer: harmless without a `TSER` reload; `SPI_HAL_Deinit` clears it.
- **The HAL's error path** clears `SPE` (l. 3121) before the interrupts (l. 3124). Not exercised.

### 10.2 The SCK pin must be in its AF (E2, R3-3)

On this silicon (Rev V), a master whose SCK reaches no pin never sets `EOT` and receives nothing; RM0399
chapter 53 and ES0445 do not say so. `Transfer` then returns `ERR_TIMEOUT` after `max_cycles` with `IER = 0`
and `SPE = 0`; `WaitEnd` returns `ERR_TIMEOUT` with the SPI still enabled: the caller stops its streams,
then calls `DmaEnd`. Evidence (`Transfer` only): T15 in `spi_f3_bench_8b5d165.txt` (BM/LL: 401 018 / 401 071 cycles
against 400 000; control 0xA5 received); first seen in `spi_dev_f1_bench_25426f8-dirty.txt`.

### 10.3 Project notes (and SP4, N1: §6.3)

- **SP3, `.d3_ram`:** both linker scripts of `prj_01` place it at `0x38000000` (`stm32h755zi_cm7.ld`
  l. 182-190, `stm32h755zi_cm4.ld` l. 107-115): only one core may use it (CPU1 in the selftest).
- **N3, for DMA1/DMA2 through `02_dma`, fixed in `02_dma` at `57b1bf7`:** before that commit, `DMA_HAL_Init`
  compared `periph_align` (`DMA_PDATAALIGN_*`, bits 12:11) with `mem_align` (`DMA_MDATAALIGN_*`, bits 14:13) in
  direct mode, so it refused half-words and words although `PSIZE = MSIZE`. It now compares the field values;
  `02_dma` T13 covers it (`02_dma/README.md` §9, without an independent run yet). This driver's T7(c) HAL still
  uses FIFO mode for 9..32-bit frames on DMA1/DMA2 and was not re-run in direct mode after the fix. A build
  older than `57b1bf7` needs FIFO mode. The BDMA path never ran this check.

---

## 11. Consumers

- `prj_01_template_dualcore`: the `selftest-spi` preset only; production links no `06_spi` tier.

---

## 12. Changing this driver

1. **Frozen API, additions only** (catalog rule 2): `spi_bm.h` as at `b4fa22d` plus `SPI_BM_CheckDma*`
   (`14ea027`), `spi_ll.h` as at `99fe89b`, `spi_hal.h` as at `b267f4c` (`1c8ab15` added only the
   precondition text of `SPI_HAL_Wait`); after the merge: `git diff main -- '01_shared_drivers/06_spi/*.h'`.
2. **One copy of every check** (`SPI_BM_Check*`, called by LL/HAL before writing); a silicon fact goes
   into `k_spi[]` with its RM0399 line and, typed separately, into the test's table.
3. **Dependencies (§1), no state in RAM;** every negative control is printed; no oracle is the driver.
4. **Before merging:** `build_all -Clean` 8/8, `selftest-spi` with 0 warnings and with `-Werror`,
   production `.map` unchanged, one clean unedited bench run.

---

## 13. History

| Milestone | Commits |
| :--- | :--- |
| Plan and implementer instructions (revision 1); J1 check, decisions 1–8 | `700b8ed`, `c9cdf78` |
| F0: step table, S13/S14, `bench_claim`/`bench_safe`, host, option and preset; plan revision 2 (P1, M1); B1, M1 | `2f95686` … `620a678`, `d9a9df7`; F0 passes at `6a55bb0` (auditor's runs `153d5b3`, `620a678`) |
| F1: `spi_bm`, target, SPI decoder in `ad2.py`, tests (v3); E2 | `30fdad8` … `b4fa22d`; F1 passes at `a58172f` (auditor's run `b4fa22d`) |
| F2: plan revision 3 (D10); m1; `SPI_BM_CheckDma*`; `spi_ll`, target, tests (v4), T15 | `b9de729` … `99fe89b`; F2 passes at `ea8430b` (auditor's run `99fe89b`); D11 `5d6f21a` |
| F3: `spi_hal`, target, HAL SPI module and `Status_Task` stack under the option, tests (v5) | `4e147b5` … `b267f4c`; F3 passes at `6675064` (auditor's run `b267f4c`) |
| Close: m4, its check, evidence; this README | `1c8ab15`, `8b5d165`, `cc951c4`; the commit that adds this README |

Process documents (plan revision 3; revisions 2 and 1 at `d9a9df7`, `700b8ed`): in this directory until
the catalog owner removes them; then each is at the parent of the commit that removes it:

```bash
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/06_spi/IMPLEMENTATION_PLAN.md)^:01_shared_drivers/06_spi/IMPLEMENTATION_PLAN.md
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/06_spi/AUDIT.md)^:01_shared_drivers/06_spi/AUDIT.md   # and the other two
```
