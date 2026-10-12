# STM32H755 Shared I2C Driver (`01_shared_drivers/05_i2c`)

Peripheral-First (Plan A), 3-tier shared driver for I2C1–I2C4 of the STM32H755ZI as master with 7-bit
addresses: write, read, write + repeated start + read, the address probe (a write of 0 bytes); 1–65 535
bytes per transfer (`NBYTES` in chunks of 255 with `RELOAD`); Sm, Fm, Fm+ up to 1 MHz; analog and digital
filters; the SCL-low timeout (`TIMEOUTA`, `TIDLE = 0`); `TIMINGR` from the kernel clock (`CalcTiming`); Fm+
per instance; polling, interrupt (state in the caller's struct) and DMA (DMA1/DMA2, BDMA, streams the
caller's); bus recovery through the caller's pin functions. Every parameter is validated before the first
access, every configuration write read back, no state in RAM. Slave, 10-bit addresses, SMBus/PMBus, PEC,
general call, wakeup from Stop and `NOSTRETCH = 1` are not in the API.

Status: validated on a NUCLEO-H755ZI-Q (Rev V, `REV_ID = 0x2003`), 2026-10-06 to 2026-10-10, in
`prj_01_template_dualcore` only, against a NUCLEO-F334R8 bench target (`tools/i2c_target_f334/`), with an
Analog Discovery 2 scope on two removable jumpers (J2/J3). Each phase passed an independent audit with the
auditor's own runs in both bench states: F0 (bench) passed at `ee96c37` (auditor's runs at `85f9d29`), F1
(`i2c_bm`) at `4fee0f9` (runs and frozen API at `a6de15b`), F2 (`i2c_ll`) at `d1c6f30` (at `c483bb9`), F3
(`i2c_hal`) at `4e303f3` (at `01e462f`). The final code (`3bc69fe`) runs the tests of all four phases:
**taps 149/149, notaps 112/112, `FW VERDICT=PASS`** ([§9](#9-validation)).

---

## 1. Tiers

| Tier | Files | CMake target | Links | ST dependency | Typical use |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal** | `i2c_bm.[c\|h]` | `stm32h7_driver_i2c_bm` | - | CMSIS only (`stm32h7xx.h`) | Both cores, ISRs, the other tiers' validation |
| **Low-Level** | `i2c_ll.[c\|h]` | `stm32h7_driver_i2c_ll` | `_i2c_bm` | `stm32h7xx_ll_i2c.h` inlines and `LL_I2C_WriteReg`/`ReadReg` | Code written against `LL_I2C_*` constants |
| **HAL** | `i2c_hal.[c\|h]` | `stm32h7_driver_i2c_hal` | `_i2c_bm` | `stm32h7xx_hal.h` (`HAL_I2C_*`, V1.11.6) | Code that already keeps an `I2C_HandleTypeDef` |

- **Link one tier** (`target_link_libraries(cm7_firmware PRIVATE stm32h7_driver_i2c_ll)`) and include it as
  `"i2c_ll.h"`, never `"05_i2c/…"`; no unsuffixed target, the umbrella only adds the include path.
- **Cores:** BM and LL are built and validated on CM7 and CM4; HAL only on CM7 (CM4 does not link `_i2c_hal`
  in `prj_01`). `i2c_hal` compiles to nothing without `HAL_I2C_MODULE_ENABLED`, which `prj_01` sets only
  with `I2C_DRIVER_SELFTEST`, and stops with `#error` if `USE_HAL_I2C_REGISTER_CALLBACKS != 0`.
- **`i2c_bm` holds the only copy** of the capability table and of every check (`I2C_BM_Check*`, no write);
  LL and HAL call them before they write, so every tier refuses a request with the same status.
- **No state in RAM:** `.data = .bss = 0` in the three objects. `.text + .rodata`: `i2c_bm` 5316 B (CM7)
  / 5312 B (CM4), `i2c_ll` 2674 / 2650 B, `i2c_hal` 1410 B, CM7 only (`docs/i2c_f3_map_sizes_e053f7a.txt`).
- **No** `USE_FULL_LL_DRIVER`, `LL_I2C_Init`, `I2Cx_EV/ER_IRQHandler`, `HAL_I2C_*Callback`, MSP or `NVIC_*` call.

| Operation | BM | LL | HAL |
| :--- | :---: | :---: | :---: |
| `IsValidInstance`, `GetCaps`, `Enable/DisableClock`, `GetKernelSource`, `CalcTiming`, `SetFastModePlus`, `Check*`, `GetIRQn`, `GetDmaRequest`, `TakeErrors`, `RecoverBus` | yes | use `I2C_BM_*`, plus `I2C_LL_CheckConfig` | use `I2C_BM_*` |
| `Init` / `Deinit` | yes / yes | yes / use `I2C_BM_Deinit` | `I2C_BM_Deinit`, then `HAL_I2C_Init` / → BM, the handle to RESET |
| `Write`, `Read`, `WriteRead` (polling) | yes | yes | `HAL_I2C_Master_Transmit`/`_Receive`, `HAL_I2C_Mem_Read` |
| `StartIt` + `XferIrq` | yes | yes | `StartIt` + `HAL_I2C_EV/ER_IRQHandler` + `Wait` |
| `DmaStartWrite`/`Read`, `WaitEnd`, `DmaEnd` | yes | yes | `StartDma` + `Wait` |

---

## 2. API summary

### 2.1 Status codes (`I2cBm_Status_t`, all three tiers)

| Status | Meaning |
| :--- | :--- |
| `I2C_BM_OK` | Written and read back; transfer complete |
| `I2C_BM_ERR_PARAM` | Refused before any peripheral access, RCC and SYSCFG included |
| `I2C_BM_ERR_UNSUPPORTED` | `i2c_hal` only: `timeout_scl_low ≠ 0`, a write-read with more than 2 write bytes (§10.1) |
| `I2C_BM_ERR_RANGE` | `CalcTiming`: no `TIMINGR` meets the mode (t\_r/t\_f above it, kernel below ES0445 2.19.1) |
| `I2C_BM_ERR_BUSY` | The hardware would ignore the write: `TIMINGR`/`ANFOFF`/`DNF` with `PE = 1` and another value, a transfer running, `PE = 0` at a transfer, a HAL handle not `READY` |
| `I2C_BM_ERR_VERIFY` | A register did not read back what was written |
| `I2C_BM_ERR_TIMEOUT` | The bound expired (`max_cycles`, `CYCCNT` stopped or not running, `timeout_ms`, the HAL tick stopped) |
| `I2C_BM_ERR_NACK` | Address or data not acknowledged; the hardware sent the STOP |
| `I2C_BM_ERR_ARLO` | Arbitration lost; with one master, the sign of a bus held since before `Init` (§3.3) |
| `I2C_BM_ERR_BUS` | `BUSY` seen, SCL held low (`TIMEOUT`), a transfer that ended short at `STOPF`, or not recovered |
| `I2C_BM_PENDING` | `I2cBm_Xfer_t.status` while an interrupt transfer runs |
| `I2C_BM_ERR_HAL` | `i2c_hal` only: a HAL error with no more specific code (`BERR`, `OVR`, DMA, …) |

### 2.2 `i2c_bm.h` (frozen at `a6de15b`, plus R4-2)

```c
bool           I2C_BM_IsValidInstance(const I2C_TypeDef *i2c);
I2cBm_Status_t I2C_BM_GetCaps(i2c, &caps), I2C_BM_EnableClock(i2c), I2C_BM_DisableClock(i2c);   /* own RCC bank */
I2cBm_Status_t I2C_BM_GetKernelSource(i2c, &field), I2C_BM_SetFastModePlus(i2c, enable);   /* SYSCFG_PMCR bit */
I2cBm_Status_t I2C_BM_CalcTiming(const I2cBm_TimingIn_t *in, I2cBm_TimingOut_t *out);       /* pure (C15) */
I2cBm_Status_t I2C_BM_CheckConfig(cfg), I2C_BM_Init(cfg), I2C_BM_Deinit(i2c);              /* no RCC write */
I2cBm_Status_t I2C_BM_Write(i2c, addr7, tx, n, max_cycles);           /* n = 0: the address probe */
I2cBm_Status_t I2C_BM_Read(i2c, addr7, rx, n, max_cycles), I2C_BM_WriteRead(i2c, addr7, tx, ntx, rx, nrx, max_cycles);
I2cBm_Status_t I2C_BM_StartIt(i2c, x);  bool I2C_BM_XferIrq(i2c, x);  /* from EV and ER: true when final */
I2cBm_Status_t I2C_BM_DmaStartWrite(i2c, addr7, buf, n), I2C_BM_DmaStartRead(i2c, addr7, n), I2C_BM_DmaEnd(i2c);
I2cBm_Status_t I2C_BM_WaitEnd(i2c, n, max_cycles);   /* n: the whole count (R3-2) */
I2cBm_Status_t I2C_BM_GetDmaRequest(i2c, tx, &id), I2C_BM_GetIRQn(i2c, &ev, &er), I2C_BM_CheckTransfer(i2c, addr7, ntx, nrx);
I2cBm_Status_t I2C_BM_RecoverBus(const I2cBm_Recovery_t *r, uint32_t max_cycles, uint32_t *pulses);
uint32_t       I2C_BM_TakeErrors(i2c);   /* I2C_BM_ERRF_BERR/ARLO/OVR/TIMEOUT (R3-8); one ICR write */
I2cBm_Status_t I2C_BM_CheckWrite, _CheckRead, _CheckWriteRead, _CheckDmaStartWrite, _CheckDmaStartRead, _CheckWaitEnd,
               _CheckStartIt;   /* R4-2 (52fa55c, owner 2026-10-08): the whole check of the call they name, no write */
```

`I2cBm_Config_t` = `{ i2c, timingr, analog_filter, dnf (0..15), timeout_scl_low (0 = off; else TIMEOUTA + 1, 1..4096:
SCL low for timeout_scl_low × 2048 kernel clocks, RM0399 50:1793-1800) }`. `I2cBm_TimingIn_t` = `{ kernel_hz,
scl_hz (1..1 000 000), rise_ns, fall_ns, analog_filter, dnf }` → `{ timingr, mode, scl_hz_model }`. `I2cBm_Xfer_t` =
`{ addr7, tx, ntx, rx, nrx, done_tx, done_rx, status }`, the caller's. `I2cBm_Recovery_t` = `{ scl_write, sda_write,
scl_read, sda_read, ctx, half_period_cycles }` (open drain: `false` drives low).

### 2.3 `i2c_ll.h` (frozen at `c483bb9`)

`I2cLl_Config_t` = `{ i2c, timing, analog_filter (LL_I2C_ANALOGFILTER_ENABLE / _DISABLE; any other value:
ERR_PARAM), digital_filter (0..15), timeout_scl_low }`. `I2C_LL_CheckConfig`, `I2C_LL_Init`, and
`I2C_LL_Write`, `_Read`, `_WriteRead`, `_StartIt`, `_XferIrq`, `_DmaStartWrite`, `_DmaStartRead`, `_WaitEnd`,
`_DmaEnd` take the arguments and give the codes of their `I2C_BM_` twins, validated by `I2C_BM_Check*`; the
image of every configuration is `i2c_bm`'s, bit for bit (T2).

### 2.4 `i2c_hal.h` (frozen at `01e462f`)

```c
I2cBm_Status_t I2C_HAL_Init(I2C_HandleTypeDef *h, const I2cBm_Config_t *cfg);   /* h->Instance == cfg->i2c */
I2cBm_Status_t I2C_HAL_Write(h, addr7, tx, n, timeout_ms), I2C_HAL_Read(h, addr7, rx, n, timeout_ms);
I2cBm_Status_t I2C_HAL_WriteRead(h, addr7, tx, ntx, rx, nrx, timeout_ms);       /* ntx 1 or 2 */
I2cBm_Status_t I2C_HAL_StartIt(h, addr7, tx, ntx, rx, nrx), I2C_HAL_StartDma(h, addr7, tx, rx, n);
I2cBm_Status_t I2C_HAL_Wait(h, timeout_ms), I2C_HAL_Deinit(h);
```

Validation order (C6): the handle and its instance (`ERR_PARAM`); this tier's limits (`ERR_UNSUPPORTED`);
`timeout_ms` 1 .. `HAL_MAX_DELAY − 1` (`ERR_PARAM`, M4) and the call's `I2C_BM_Check*`; the handle's state
(`READY`, else `ERR_BUSY`). HAL errors map to `ERR_TIMEOUT`, `ERR_ARLO`, `ERR_NACK`, `ERR_BUSY`, else
`ERR_HAL`; `HAL_OK` with a read-back that differs is `ERR_VERIFY`. `I2C_HAL_Init` reads back `CR1`, `TIMINGR`,
`TIMEOUTR = 0`, `OAR1 = OA1EN`, `OAR2 = 0`, `CR2 = AUTOEND` (§6.3), `ERR_BUS` with `BUSY`; `StartIt`/`StartDma`
read nothing back (the transfer may be over), `Wait` reads back the close. `I2C_HAL_Deinit` = `I2C_BM_Deinit`
+ the handle to RESET (no `HAL_I2C_DeInit`, no MSP).

---

## 3. Configuration guide

1. **Clock.** `I2C_BM_EnableClock(i2c)` (this core's RCC bank). The kernel clock is the project's;
   `GetKernelSource` returns the raw field. In `prj_01` both are 0: I2C1–3 `pclk1`, I2C4 `pclk4`, 100 MHz.
2. **`TIMINGR`.** `I2C_BM_CalcTiming`: the first `PRESC` 0..15 whose `SCLDEL`, `SDADEL` window, `SCLL`, `SCLH`
   meet the mode (RM0399 tables 405/407, 50:159-171, 50:319-342). The period in prescaled clocks is `N =
   ceil((ceil(kernel_hz / scl_hz) − sync) / (PRESC + 1))`, **rounded up in both places** (R3-1): `scl_hz_model`
   is never above `scl_hz`. `ERR_RANGE` if t\_r/t\_f exceed the mode (Sm 1000/300 ns, Fm 300/300, Fm+ 120/120,
   50:379-383) or the kernel is below 4/10/20 MHz (ES0445 2.19.1, checked only here, R3-3). The bench passes
   t\_r 300 ns with taps and 40 ns without. With taps the scope measures t\_r (30–70 %) at 250–254 ns, the
   50 T5 medians of `i2c_f3_bench_taps_3bc69fe.txt` (248–256 ns over every taps capture of F1–F3).
3. **Init**, then the pins in their AF (§3.1). With `PE = 1` and the same configuration `Init` is a
   software reset; with another `TIMINGR`, filter or timeout, `ERR_BUSY`.
4. **Transfer:** `Write`/`Read`/`WriteRead`, `StartIt`, or the DMA sequence (§3.2).

### 3.1 Open-drain pin guide

- **AF with `GPIO_BM_OTYPE_OD` and no pull**, on every board on the bus (S15): the pull-ups are on the bus,
  and two outputs on one line without series resistors can damage a pin. GPIO open drain only to recover.
  With `PE = 0` the peripheral lets go of SCL and SDA (50:429-431).
- **One instance per pin pair:** change the AF only with the old instance at `PE = 0`, the pins back to
  input for ≥ 1 ms (S16). PB8/PB9 are I2C1 (AF4) and I2C4 (AF6) on this MCU.
- **Fm+:** `SetFastModePlus` sets the instance's `SYSCFG_PMCR.I2CxFMP` (SYSCFG clock needed, else `ERR_BUSY`);
  the `PBxFMP` pin bits are the project's.

### 3.2 Interrupts and DMA

- **Interrupt:** fill an `I2cBm_Xfer_t`, enable **both** vectors of `GetIRQn` at the **same priority** (one
  must not interrupt the other inside `XferIrq`), call `StartIt`; the project's EV and ER handlers both
  call `XferIrq` until it returns true. **Abandon an interrupt transfer only with `I2C_BM_Deinit`:** `Init`
  refuses with `ERR_BUSY` while an enable is set (O9).
- **DMA (BM/LL):** `GetDmaRequest` (DMAMUX1 33/34, 35/36, 73/74; DMAMUX2 13/14 for I2C4, BDMA). Enable the
  stream first, then `DmaStartWrite` (n ≥ 2: `buf[0]` in `TXDR` before START, ES0445 2.19.6; the stream sends
  `buf[1..n-1]`) or `DmaStartRead`, `WaitEnd(i2c, n, …)` (each `TCR` gets the next 255), stop it, `DmaEnd`.
- **DMA (HAL):** link `h->hdmatx`/`hdmarx` (`DMA_HAL_Init` of `02_dma`, with its IRQ), `StartDma`, `Wait`.
  **A DMA transfer given up:** with `ERR_TIMEOUT` the HAL stops nothing; with an error (`ERR_NACK`,
  `ERR_ARLO`, `ERR_HAL`) it clears `TXDMAEN`/`RXDMAEN` and starts an asynchronous abort of the stream
  (`HAL_DMA_Abort_IT`), which may still be running when `Wait` returns (§10.1). So: abort a stream still
  busy (`HAL_DMA_Abort`), wait until no stream of the transfer is in `HAL_DMA_STATE_ABORT` (a DMA1/DMA2
  stream's abort ends in its IRQ; the BDMA's at once), then `I2C_HAL_Deinit`.
- **BDMA (I2C4):** buffers in `.d3_ram` (SRAM4, `0x38000000` in both `prj_01` linker scripts), one core only.
- **Timeouts:** `timeout_scl_low` turns on `TIMEOUTA`: a transfer that meets SCL held low ends with `ERR_BUS`
  and an immediate software reset (`PE = 0`) that lets go of both lines, **no STOP** (§6.3). Every other wait
  is bounded by `max_cycles` DWT cycles, with an exit if `CYCCNT` does not run or stops.

### 3.3 A bus held before `Init` (R3-6)

`ISR.BUSY` is set only by a START seen with `PE = 1` and cleared by a STOP or `PE = 0` (RM0399 50:2897-2900).
A target that holds SDA since before `Init` (a reset in the middle of a read) leaves `BUSY = 0`: `Init` and
`CheckTransfer` return OK and the next transfer ends with `ERR_ARLO` (on this silicon, T9(b)), `ERR_BUS` or
`ERR_TIMEOUT`. So **before the first `Init` after a reset**, with the pins in GPIO open drain, read both
lines with `scl_read` and `sda_read`:

- **SDA low:** `I2C_BM_RecoverBus` gives up to 9 SCL pulses until SDA reads high and **ends with a START +
  STOP** (O6): UM10204 calls it illegal with nothing between; most devices accept it, the F334 did (T9).
  `ERR_BUS` if SCL does not rise when released (also a target that stretches SCL after letting it go).
- **SDA high:** `RecoverBus` returns OK at once **without reading SCL** (O17): a SCL held alone is seen only
  by `scl_read`. (Before it reads SDA, `RecoverBus` returns `ERR_TIMEOUT` if `CYCCNT` does not run.)
- With one master on the bus, `ERR_ARLO` means a hung bus: recover, then `Init` again.

---

## 4. Project contract

- **One owner core per instance,** no cross-core lock. `prj_01` selftest: CPU2 owns I2C1, CPU1 I2C2–4.
- **RCC banks (RM0399 9.5.10):** per core; an I2C runs while either core enables it (T13). `RCC_APBxRSTR`
  is never written; `RCC_D2CCIP2R`/`RCC_D3CCIPR` (kernel multiplexers) are only read.
- **`SYSCFG_PMCR`** is shared with every other bit of the register (RM0399 13:33-137: `PA0SO`, `PA1SO`, `PC2SO`,
  `PC3SO`, `EPIS` of `08_eth`, `BOOSTVDDSEL`, `BOOSTE`, `PB6FMP`..`PB9FMP`): the driver writes only its
  instance's `I2CxFMP`, one bit RMW with PRIMASK, read back. **PRIMASK does not protect against the other
  core** (O7): the project serialises the two cores' writes to the register.
- **No callback, no MSP, no vector in the driver.** `HAL_I2C_Init` calls the project's `HAL_I2C_MspInit`
  while the handle is RESET (in `prj_01`, the HAL's empty weak one). **CPU1's HAL tick is TIM6**
  (`TICK_INT_PRIORITY 0`): the HAL's waits and `I2C_HAL_Wait` need it running.

---

## 5. Rules the driver enforces

- **C6, C7, C14:** validate first, RCC and SYSCFG included (NULL, instance, capability, range, state:
  `ERR_BUSY`, then `ERR_BUS`), a refusal writes nothing; no state in RAM; no vector, `NVIC_*` or callback.
  **C10:** `ICR` written once with the bits to clear; `CR1`/`CR2` RMWs under PRIMASK, saved and restored.
- **C12:** every configuration register read back; refused with `ERR_BUSY`: `TIMINGR`, `ANFOFF`, `DNF`,
  `NOSTRETCH` with `PE = 1` (50:2505-2529, 50:2808), `CR2` with `START = 1` (50:2620-2674). `TIMEOUTA`
  written only with `TIMOUTEN = 0` (50:2861); `PE = 0` read back three times (≥ 3 APB cycles, 50:441-445).
- **C13:** every wait bounded, with an exit when `CYCCNT` reads the same value more than 1000 times or does not run
  (`WaitEnd` then software-resets); `I2C_HAL_Wait` by `timeout_ms` and by polls with the tick stopped.
- **ES0445:** the first byte of every write in `TXDR` before START, DMA too (2.19.6); `BERR` in master mode
  cleared, the transfer goes on (2.19.2); `NACK` leaves the STOP the hardware sends (50:1192-1194).

---

## 6. Silicon facts and errata

### 6.1 Capability table

`k_i2c[]` in `i2c_bm.c`, each row citing RM0399 rev 4; the test's own copy, typed separately, is compared
with `GetCaps`, `GetDmaRequest`, `GetIRQn` and RCC read directly. `09:` RCC, `13:` SYSCFG, `18:` DMAMUX.

| Instance | Domain, bus | Enable bit | Kernel field | Fm+ bit | DMAMUX RX / TX | IRQ EV / ER |
| :--- | :--- | :--- | :--- | :---: | :--- | :---: |
| I2C1 | D2, APB1 | `APB1LENR` 21 (09:7236) | `RCC_D2CCIP2R.I2C123SEL` [13:12] (09:4744-4749) | 0 (13:122-137) | 1: 33 / 34 (table 126) | 31 / 32 |
| I2C2 | D2, APB1 | `APB1LENR` 22 (09:7230) | same | 1 | 1: 35 / 36 (18:113-116) | 33 / 34 |
| I2C3 | D2, APB1 | `APB1LENR` 23 (09:7224) | same | 2 | 1: 73 / 74 (18:110-111) | 72 / 73 |
| I2C4 | D3, APB4 | `APB4ENR` 7 (09:7683) | `RCC_D3CCIPR.I2C4SEL` [9:8] (09:4868-4873) | 3 | 2: 13 / 14 (BDMA, 18:235-236) | 95 / 96 |

### 6.2 Errata (ES0445 rev 6, §2.19)

| ID | Erratum | `i2c_bm`, `i2c_ll` | `i2c_hal` |
| :--- | :--- | :--- | :--- |
| 2.19.1 | tSU;DAT shorter than one kernel period | `CalcTiming` refuses kernels below 4/10/20 MHz (T1, T3) | The same `TIMINGR` (BM's) |
| 2.19.2 | Spurious `BERR` in master mode | Cleared, the transfer goes on; cannot be provoked, no `BERR` in any capture | **Aborts**, `ERR_HAL` (IC1, §10.1) |
| 2.19.3/5/7 | Own address, slave `OVR`, SMBus target timeout | Out of scope (no slave, no SMBus) | Same |
| 2.19.4 | Documentation (`START`, `ADDRCF`) | - | - |
| 2.19.6 | Second byte lost with APB/kernel 1.5–3 | First byte preloaded (polling, IT, DMA). T12: kernel on HSI 64 MHz (ratio 1.56), 2000 writes, 0 stalls. Control without preload: **not reproduced** (decision 6) | Preloaded except in the blocking `Mem_Read` (§10.1); `prj_01` runs ratio 1 |

**IC1** (the plan's name, kept here): the HAL aborts a master transfer on `BERR`, which 2.19.2 calls spurious
and says the transfer goes on (`I2C_IsErrorOccurred` l. 7164-7284, `HAL_I2C_ER_IRQHandler` l. 4664-4706).

### 6.3 Silicon facts (Rev V), each with the delivery capture that shows it

- **`TIMEOUT` fires again every `TIMEOUTA` while SCL is held low** (O22; RM0399 50:2275-2289 does not say so):
  an expiry with no transfer does nothing on the bus; a later transfer ends at the first expiry after its own
  START. `i2c_f3_bench_taps_3bc69fe.txt`, T10(b) (`TIMEOUTA` 10 014 µs), every time from the SCL fall on
  the scope: CPU2's `Write` starts (SDA falls) at 0.010 ms and SDA is let go at 9.885 ms (1 × `TIMEOUTA`
  9.889 ms), n = 1; CPU1's starts on purpose at 14.832 ms and SDA is let go at 19.775 ms (2 × `TIMEOUTA`
  19.778 ms), n = 2, SDA untouched until its START. (CPU1's own `CYCCNT` puts its start at 15 022 µs from
  the fall it saw, in nominal µs of a clock that runs 1.27 % fast, §6.4.) Time a `TIMEOUT` from the START. O23 (a late `AT HOLD` handshake would make CPU1's
  n = 3) no longer applies: since B2 of F2 both cores' starts are timed from the fall CPU1 sees.
- **A master `TIMEOUT` ends with no STOP** (R3-5): RM0399 50:2286-2287 says the master sends one; Rev V went
  on holding both lines, so `i2c_bm` software-resets (`ERR_BUS`). Same capture, CPU2's run: SDA rises with
  SCL still low (`SDAr` 9.885 ms, `SCLr` 50.094 ms), no STOP.
- **`BUSY` is set only by a START seen with `PE = 1`** (R3-6, 50:2897-2900). Same capture, T9(b): SDA held
  before `Init` → `Init` OK, `ISR 0x00000001`, `Write` `ERR_ARLO`, `RecoverBus` 4 pulses (the `Write` gave the
  first edge), then OK. T9 (held after `Init`): `ISR 0x00008001` (`BUSY`), `ERR_BUS`.
- **`CR2.NACK` written with `PE = 0` does not stay** (50:2621-2623: cleared by hardware while `PE = 0`).
  `HAL_I2C_Init` writes `AUTOEND | NACK` there (l. 619, between `PE = 0` at l. 587 and `PE = 1` at l. 634),
  so its image has `CR2 = AUTOEND`. `i2c_f3_bench_*_3bc69fe.txt`, T2-HAL: equal to the raw HAL in all 90
  initialised cases; first seen in the failed `i2c_f3_bench_notaps_a15c442.txt`.

### 6.4 This board

- **The clock runs fast:** CLKREF (`MCU_ACK` held 40 000 000 `CYCCNT` cycles, timed by the AD2) gives +1.27 %
  in `i2c_f3_bench_taps_3bc69fe.txt` (ST-LINK MCO as HSE, `03_tim` N1): the I2C kernels run ~101.3 MHz, above
  VOS1's 100 MHz: **IC3**, the plan's name for this issue (also N1 of `03_tim` and SP4 of `06_spi`), open and
  outside the driver. **So the bus runs above what was asked** (R3-7): raw f\_SCL
  +0.88 to +1.11 % at 100 kHz, +0.81 to +1.13 % at 400 kHz (same capture). T5 judges it normalised with
  CLKREF (the driver's arithmetic) and records the raw one. **A product with this clock must ask with margin.**
- **tHIGH at 400 kHz has about 3 % margin** over Fm's 600 ns: medians 611–622 ns, single periods down to
  596 ns (`T5-LL CM7 k=1`). T5 accepts a minimum up to 2 samples (80 ns at 25 MS/s) below the limit and
  records it (owner's decision, 2026-10-08): 5 records in that capture.

---

## 7. Examples

Each compiles with `-Wall -Wextra -Wpedantic -Wconversion -Werror` (CMSIS/HAL headers as `-isystem`; A for
CM4, B and C for CM7, C with `HAL_I2C_MODULE_ENABLED`; how: the report's "Cierre"). Pins (PB8/PB9), target
(0x42, the F334) and kernel clocks (100 MHz) are those of the bench (§8) and `prj_01`; GPIOB's clock is on.

### A. Polling from CPU2, I2C1, the bus checked before `Init` (`i2c_bm`)

```c
#include "i2c_bm.h"
#include "gpio_bm.h"

static void scl_w(void *ctx, bool high) { (void)ctx; GPIO_BM_Write(GPIOB, 8U, high ? 1U : 0U); }
static void sda_w(void *ctx, bool high) { (void)ctx; GPIO_BM_Write(GPIOB, 9U, high ? 1U : 0U); }
static bool scl_r(void *ctx) { (void)ctx; return GPIO_BM_Read(GPIOB, 8U); }
static bool sda_r(void *ctx) { (void)ctx; return GPIO_BM_Read(GPIOB, 9U); }

static bool pin(uint32_t p, uint32_t af)      /* af 0: GPIO open drain, released; else that AF; never a pull */
{
    return ((af == 0U) ? GPIO_BM_ConfigOutput(GPIOB, p, GPIO_BM_OTYPE_OD, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 1U)
                       : GPIO_BM_ConfigAlternateEx(GPIOB, p, af, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE, GPIO_BM_SPEED_LOW))
           == GPIO_BM_OK;
}

bool read_regs_i2c1(uint8_t reg, uint8_t *out, uint32_t n)
{
    const uint32_t ms = SystemCoreClock / 1000U;
    const I2cBm_Recovery_t rec = { scl_w, sda_w, scl_r, sda_r, NULL, 5U * (ms / 1000U) };   /* 5 us */
    const I2cBm_TimingIn_t ti = { 100000000U, 400000U, 300U, 10U, true, 0U };   /* pclk1 in prj_01 */
    I2cBm_TimingOut_t to;
    uint32_t pulses = 0U;
    /* R3-6: BUSY does not see a bus held since before Init; RecoverBus does not read SCL with SDA high. */
    if (!pin(8U, 0U) || !pin(9U, 0U) || !scl_r(NULL) || (I2C_BM_RecoverBus(&rec, 10U * ms, &pulses) != I2C_BM_OK) ||
        (I2C_BM_EnableClock(I2C1) != I2C_BM_OK) || (I2C_BM_CalcTiming(&ti, &to) != I2C_BM_OK))
    {
        return false;
    }
    const I2cBm_Config_t c = { I2C1, to.timingr, true, 0U, 0U };
    return (I2C_BM_Init(&c) == I2C_BM_OK) && pin(8U, 4U) && pin(9U, 4U) &&               /* AF4 */
           (I2C_BM_WriteRead(I2C1, 0x42U, &reg, 1U, out, n, 10U * ms) == I2C_BM_OK);    /* repeated start */
}
```

### B. Interrupt write from CPU1, I2C4 (`i2c_ll`)

```c
#include "i2c_ll.h"
#include "gpio_bm.h"

static I2cBm_Xfer_t s_x;                   /* the transfer's state is the caller's (C7) */

void I2C4_EV_IRQHandler(void) { (void)I2C_LL_XferIrq(I2C4, &s_x); }   /* the project's vectors (C14), */
void I2C4_ER_IRQHandler(void) { (void)I2C_LL_XferIrq(I2C4, &s_x); }   /* both at the same priority    */

bool start_write_i2c4(const uint8_t *tx, uint32_t n)
{
    const I2cLl_Config_t c = { I2C4, 0x20D0142EU, LL_I2C_ANALOGFILTER_ENABLE, 0U, 0U };  /* CalcTiming: 100 MHz, 400 kHz, 300/10 ns */
    IRQn_Type ev = I2C4_EV_IRQn;
    IRQn_Type er = I2C4_ER_IRQn;
    if ((I2C_BM_EnableClock(I2C4) != I2C_BM_OK) || (I2C_LL_Init(&c) != I2C_BM_OK) ||
        (GPIO_BM_ConfigAlternateEx(GPIOB, 8U, 6U, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE, GPIO_BM_SPEED_LOW) != GPIO_BM_OK) ||
        (GPIO_BM_ConfigAlternateEx(GPIOB, 9U, 6U, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE, GPIO_BM_SPEED_LOW) != GPIO_BM_OK) ||
        (I2C_BM_GetIRQn(I2C4, &ev, &er) != I2C_BM_OK))
    {
        return false;
    }
    s_x = (I2cBm_Xfer_t){ .addr7 = 0x42U, .tx = tx, .ntx = n };         /* write only: rx NULL, nrx 0 */
    NVIC_SetPriority(ev, 6U);  NVIC_SetPriority(er, 6U);                /* EV and ER at one priority */
    NVIC_EnableIRQ(ev);        NVIC_EnableIRQ(er);
    return I2C_LL_StartIt(I2C4, &s_x) == I2C_BM_OK;   /* s_x.status leaves PENDING; to give up: I2C_BM_Deinit */
}
```

### C. A read by BDMA from CPU1, I2C4 with a handle (`i2c_hal`)

```c
#include "i2c_hal.h"
#include "dma_hal.h"
#include "gpio_bm.h"

static I2C_HandleTypeDef h_i2c4;
static DMA_HandleTypeDef h_rx;
static uint8_t s_rx[64] __attribute__((section(".d3_ram"), aligned(32)));   /* the BDMA reaches SRAM4 */

void I2C4_EV_IRQHandler(void)       { HAL_I2C_EV_IRQHandler(&h_i2c4); }     /* the project's (C14) */
void I2C4_ER_IRQHandler(void)       { HAL_I2C_ER_IRQHandler(&h_i2c4); }
void BDMA_Channel0_IRQHandler(void) { HAL_DMA_IRQHandler(&h_rx); }

bool read_i2c4_bdma(void)                   /* thread mode: I2C_HAL_Wait needs the tick */
{
    const I2cBm_Config_t c = { I2C4, 0x20D0142EU, true, 0U, 0U };   /* timeout_scl_low 0: the HAL has none */
    const DmaHal_Config_t d = { BDMA_Channel0, BDMA_REQUEST_I2C4_RX, DMA_PERIPH_TO_MEMORY, DMA_PINC_DISABLE, DMA_MINC_ENABLE,
                                DMA_PDATAALIGN_BYTE, DMA_MDATAALIGN_BYTE, DMA_NORMAL, DMA_PRIORITY_HIGH, DMA_FIFOMODE_DISABLE,
                                DMA_FIFO_THRESHOLD_FULL, DMA_MBURST_SINGLE, DMA_PBURST_SINGLE, true, 6 };   /* its IRQ at 6 */
    h_i2c4.Instance = I2C4;
    if ((I2C_BM_EnableClock(I2C4) != I2C_BM_OK) || (I2C_HAL_Init(&h_i2c4, &c) != I2C_BM_OK) ||
        (GPIO_BM_ConfigAlternateEx(GPIOB, 8U, 6U, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE, GPIO_BM_SPEED_LOW) != GPIO_BM_OK) ||
        (GPIO_BM_ConfigAlternateEx(GPIOB, 9U, 6U, GPIO_BM_OTYPE_OD, GPIO_BM_PULL_NONE, GPIO_BM_SPEED_LOW) != GPIO_BM_OK) ||
        (DMA_HAL_Init(&h_rx, &d) != HAL_OK))
    {
        return false;
    }
    __HAL_LINKDMA(&h_i2c4, hdmarx, h_rx);
    HAL_NVIC_SetPriority(I2C4_EV_IRQn, 6U, 0U);  HAL_NVIC_SetPriority(I2C4_ER_IRQn, 6U, 0U);   /* same */
    HAL_NVIC_EnableIRQ(I2C4_EV_IRQn);            HAL_NVIC_EnableIRQ(I2C4_ER_IRQn);
    I2cBm_Status_t st = I2C_HAL_StartDma(&h_i2c4, 0x42U, NULL, s_rx, 64U);
    st = (st == I2C_BM_OK) ? I2C_HAL_Wait(&h_i2c4, 10U) : st;
    if (st != I2C_BM_OK)                    /* §3.2, §10.1 */
    {
        if (h_rx.State == HAL_DMA_STATE_BUSY)
        {
            (void)HAL_DMA_Abort(&h_rx);     /* ERR_TIMEOUT: the HAL stopped nothing */
        }
        /* after an error the HAL aborted it itself: at once on the BDMA (a DMA1/2 stream: wait for READY) */
        (void)I2C_HAL_Deinit(&h_i2c4);
    }
    return st == I2C_BM_OK;
}
```

---

## 8. Test bench (Analog Discovery 2 and a NUCLEO-F334R8)

The bench of `00_gpio/README.md` §8 and J1 of `06_spi`, not one wire changed, **plus the I2C bus**:

```text
 H755 CN7-2  PB8 ───── SCL node ── W1 ── F334 CN5-10 PB8 (D15)     1.0 kΩ from each node to H755 3V3 (CN8-7)
 H755 CN7-4  PB9 ───── SDA node ── W2 ── F334 CN5-9  PB9 (D14)     no series resistors anywhere
 H755 CN8-11 GND ───────────────── W3 ── F334 CN5-7  GND           first on, last off; no supply rail between
 J2 (removable): SCL node ── H755 CN9-19 PF14 (AD2 scope 1+, 22 kΩ to GND, 150 pF; input only)
 J3 (removable): SDA node ── H755 CN9-21 PF15 (AD2 scope 2+, 22 kΩ to 3V3, 150 pF; input only)
 AD2: DIO12 → PE2 HOST_REQ, DIO13 ← PE7 MCU_ACK (handshake, CLKREF); scope probes on the PF14/PF15 side
```

- **H755:** I2C1 (CPU2) on PB8/PB9 AF4, I2C4 (CPU1) on AF6, never both. PF14/PF15 are taps only.
- **Bench states:** **taps** (J2/J3 in): 100 and 400 kHz, the scope measures (t\_r 250–254 ns, the T5
  medians of the final capture; the ~250 pF on each line is the plan's estimate, not measured); **notaps**
  (J2/J3 out): 400 kHz and 1 MHz, no scope (t\_r about 50 ns is the plan's estimate: nothing measures it
  there). 1 MHz only in notaps (D3).
- **The F334 target** (`tools/i2c_target_f334/`, protocol and build in its README; bare-metal, CMSIS F3
  v2.3.9): 0x42, 256 registers (`reg[i] = i ^ 0xA5`), auto-increment; armed from its COM port (`INIT`, `NACK`,
  `CORRUPT`, `STRETCH`, `HOLD SDA|SCL`, `FMP`, `STATUS`, `LEVELS`, `DUMP`); an `RPT` (START/Sr/STOP, bytes,
  NACKs, CRC-32) after every transfer; a banner with Rev, build id and PB8/PB9 `MODER/OTYPER/PUPDR`. With
  `FMP 0` it stretches each SCL low by 0.8–1.2 µs: never 1 MHz traffic with `FMP 0` (O14).

| # | Rule | Where |
| :--- | :--- | :--- |
| S15 | PB8/PB9 (both boards) never push-pull nor with a pull; PF14/PF15 input without pull | `bench_claim()` of both cores; the F334 banner, checked by the host |
| S16 | One H755 instance in AF on PB8/PB9 at a time, from one core, the owner read first (clock, `PE = 0`) | Firmware |
| S17 | Levels before driving and after every step, as a ratio: taps SDA ≥ 3.0 V and SCL/SDA 0.956 ± 0.015; notaps 1+ < 0.10 V, 2+ ≥ 3.0 V | Host |
| S18 | `sn=` in every programmer call; COM ports by ST-LINK SN; exactly two ST-LINKs. **The other benches (`gpio`, `tim`, `spi`) run with the F334's USB unplugged** | Host, `tools/`, procedure |
| S19 | The bench state declared (`--bench-state`) and checked (scope, PF14) | Host |
| S20 | Both boards on USB of the same PC, GND first; flash or reset only with the host stopped | Procedure |
| S21 | The F334 holds a line only in open drain, in an announced step, ≤ 500 ms, released by its timer | F334, host |

Never touched: PE0, PE1, PC13, PA4, PA7, PA13/PA14, TIM6, SysTick, `DBGMCU` (write), `RCC_APBxRSTR`; `RCC_D2CCIP2R`
only in T12, saved, restored, read back. Each F334 hold follows the handshake that frees CPU1, which waits for
the fall (≤ 2 s) and times from it (B2).

---

## 9. Validation

`prj_01_template_dualcore`, preset `selftest-i2c`: `C*/App/Src/i2c_selftest.c`, `Common/Src/i2c_selftest_common.c`
(block at `0x3800D000`, SRAM4), host `tools/i2c_hw_check.py` (bench v5); CPU1 prints the verdict. No oracle is
the driver: register snapshots, RM0399 fields typed by hand, the raw `HAL_I2C_Init`, the F334's reports and
CRC-32, the AD2 decoded by the host (`ad2.i2c_decode_analog`), `CYCCNT`, the host's own `calc_timing()`.
With the option off, production keeps the sections of `2481396`.

### 9.1 Captures

In `prj_01_template_dualcore/docs/`, one unedited run each; clean ones carry one Rev and build id on all four.
Counts are the host's checks (`n/n host checks passed`), each run with `FW VERDICT=PASS` unless said.

| Phase | Auditor's runs that passed | Delivered runs that passed (the last one first) |
| :--- | :--- | :--- |
| F0 bench | `i2c_f0_audit_bench_{taps,notaps}_85f9d29.txt`: 28/28, 26/26 | `i2c_f0_bench_taps_f7323f2_5.txt` 28/28, `..._notaps_f7323f2_2.txt` 26/26; replaced: `taps_f7323f2` 28/28 |
| F1 `i2c_bm` | `i2c_f1_audit_bench_{taps,notaps}_a6de15b.txt`: 96/96, 74/74; `i2c_f1_audit_bench_notaps_cd8a0f4.txt`: 74/74 | `i2c_f1_bench_{taps,notaps}_26c2b4f.txt`: 96/96, 74/74; replaced: `{taps,notaps}_e27c673` 90/90, 69/69, `_9c07b93` 95/95, 73/73, `_fefacf4` 96/96, 74/74, `notaps_5e44044` 73/73, `notaps_3d92f3e` 74/74 |
| F2 `i2c_ll` | `i2c_f2_audit_bench_{taps,notaps}_c483bb9_2.txt`: 130/130, 98/98 | `i2c_f2_bench_{taps,notaps}_0471570.txt`: 130/130, 98/98; replaced: `{taps,notaps}_48ec427` 130/130, 98/98; `i2c_f1_bench_notaps_acca289.txt` (98/98) is F2's first run under F1's name |
| F3 `i2c_hal` | `i2c_f3_audit_bench_{taps,notaps}_01e462f.txt`: 149/149, 112/112; `i2c_f3_audit_bench_taps_689f36a.txt`: 149/149 | **`i2c_f3_bench_{taps,notaps}_3bc69fe.txt`: 149/149, 112/112** (final code); replaced: `{taps,notaps}_357c60d` 149/149, 112/112 |

Failed or aborted runs kept: F0 `notaps_f7323f2`, `taps_f7323f2_2/_3/_4` (S19/S17 aborts, 1/2: J2/J3, probes);
F1 `taps_5e44044` 90/95, `_3d0ad6d` 91/95, `_3d92f3e` 94/96, `audit_taps_cd8a0f4` 95/96 (T5 stretching
exemptions, T9(b) window, tHIGH tolerance, T10(b) timed from the SCL fall); F2 `audit_taps_9aa7a37` 128/130,
`FW FAIL` (B2: windows opened at a handshake; B3: the T9(b) capture depended on the firmware's latency),
`audit_{taps,notaps}_c483bb9` (S19 aborts, 0/1); F3 `notaps_a15c442` (92/92 host checks, `FW FAIL`, the
firmware aborted: `CR2.NACK`, §6.3). `i2c_dev_*`: development runs.

### 9.2 Tests

Values from `i2c_f3_bench_taps_3bc69fe.txt`; each result is the named function's; "BM/LL/HAL" the tiers.

| Test | Tiers, cores | What (reference) | Result | Negative control |
| :--- | :--- | :--- | :--- | :--- |
| T0a–T0d | bench | AD2 lines; claims (S15, S16); levels and F334 holds seen by CPU1, F334, scope; identity | PASS; holds 50.7 ms (CPU1), 50.0 ms (F334) | each hold is its own control |
| T1 | BM, LL CM7/CM4; HAL CM7 | refusals typed by hand; photo of instance, both RCC banks, kernel fields, `SYSCFG_PMCR` (HAL: + `h->State`) | 62 / 63 / 48, 0 wrong, photo unchanged; with `BUSY = 1`: 8 `ERR_BUS` per tier | a valid `Init` changes the photo |
| T2 | BM, LL CM7 (I2C2–4), CM4 (I2C1); HAL CM7 | 120 images per instance against RM0399 fields by hand; LL vs BM bit for bit; HAL vs the raw `HAL_I2C_Init` and BM | 0 differ; HAL: timeout ≠ 0 refused (90), 30 equal to raw HAL, vs BM only `OAR1.OA1EN`, `CR2.AUTOEND` | DNF + 1 expected: told apart in all |
| T3 | BM + host | `CalcTiming` against `calc_timing()` and RM0399 fields | 403 vectors, 0 differ; 286 OK meet 50:342, 319-331, table 407, 2.19.1 | `SCLL + 1`: 286/286 flagged |
| T4 | BM, LL CM4 I2C1 / CM7 I2C4; HAL CM7 | 1..65 535 bytes, write, read, write-read, probes 0x42/0x43, per speed (F334 report, CRC-32) | 0 failed, all tiers and speeds | `CORRUPT 5`: 1 byte off, at 5 |
| T5 | BM, LL CM4/CM7; HAL CM7 (taps) | 5 transfers per speed decoded by the host: frames, tLOW, tHIGH, tSU;DAT, tHD;STA, tSU;STO, t\_r, f\_SCL | 50 captures, 0 failed; f\_SCL ± 5 % of the model | channels swapped: differs in all |
| T6 | BM, LL, HAL | interrupt, 1/2/256/1000 bytes; IE and IRQs after the end | 0 failed; IE 0; 0 IRQs in 10 ms | IE cleared by hand: `ERR_TIMEOUT` |
| T7 | (a) BM, LL CM4 DMA2; (b) BM, LL, HAL CM7 BDMA | 2/255/256/4096 bytes | 0 failed | `RXDMAEN = 0`: 64 sentinels, `ERR_TIMEOUT` |
| T8 | BM | (a) no target at 0x43; (b) NACK on data byte 3 of 8 | `ERR_NACK`, STOP sent, next OK; 3 counted (F334 `nk=1 rx=4`) | no NACK armed: OK |
| T9, T9(b) | BM CM7 | SDA held after / before `Init`; `RecoverBus` (F334 edges, scope) | §6.3; 5 / 4 pulses, STOP seen, Write OK after | no `RecoverBus`: SDA low 101 ms |
| T10 | BM CM4/CM7 | (a) 200 µs stretching; (b) SCL held 50 ms, `TIMEOUTA` 10 ms (`CYCCNT`, scope) | (a) data OK, ≥ 5 × 200 µs longer; (b) §6.3 | (b) no timeout: `ERR_TIMEOUT` at 30 ms |
| T12 | BM CM4 | 2.19.6, kernel on HSI | 2000 writes, 0 stalls | no preload: not reproduced |
| T13, T14, T15 | BM | RCC banks (I2C2); `Deinit` vs the RM reset image after every test; I2C2 without pins | a write lands while either bank enables; 0x00 everywhere; `ERR_TIMEOUT` within `max_cycles` + 1 ms | both banks off: lost; I2C3 without `Deinit`: 0x11; the AF transfers of T4 |
| m4, tick | HAL | `I2C_HAL_Wait` with PRIMASK set; `HAL_GetTick` at 10 points | `ERR_TIMEOUT` after 13 600 240 cycles, tick unchanged; 10 TICK rising | tick running: 9.4–9.5 ms |

**Controls not reproduced in every run:** T12's (the 2.19.6 stall without preload) in none (decision 6);
the T5 tHIGH records within 2 samples of 600 ns come and go between captures (§6.4).

### 9.3 Reproducing it

```powershell
.\tools\env.ps1; cd 03_firmware_templates\prj_01_template_dualcore
cmake --preset selftest-i2c; cmake --build --preset selftest-i2c
..\..\tools\i2c_target_f334\build.ps1 -Clean                          # the F334 image
py tools/flash.py --preset selftest-i2c --core both --sn <H755 SN>    # host stopped
pushd ..\..\tools\i2c_target_f334; py ..\flash.py --sn <F334 SN> --elf build\f334\i2c_target_f334.elf; popd
py tools/i2c_hw_check.py --bench-state taps --checklist-confirmed <name> --checklist-date YYYY-MM-DD
py tools/i2c_hw_check.py --bench-state notaps ...                     # after J2/J3 are removed
```

The F334 is flashed from its own directory, as its README says: `flash.py` takes the first directory upward
with a `CMakePresets.json` as the project and resolves a relative `--elf` from there (`flash.py:81-84, 459`).
From `prj_01` with `--elf ..\..\tools\i2c_target_f334\build\f334\i2c_target_f334.elf` it programs the same. The host refuses another checkout's Rev/build id, a third ST-LINK, WaveForms or a debugger, and
aborts on S17/S19 before driving the bus. It writes `docs/i2c_f3_bench_<state>_<rev>.txt`, or `..._<rev>_<n>.txt`
(n = 2, 3, …) if that file exists; with `--allow-dirty` the name starts `i2c_dev_`.

### 9.4 Measurements (B1)

DWT cycles of one `i2c_bm` call, CM4 (200 MHz) / CM7 (400 MHz) (R4-1: cycles in BM only; LL and HAL, the
`.map` of §1): `I2C_BM_Init` 330 / 603 (taps), 611 (notaps); `I2C_BM_Write` of 1 byte at 400 kHz 11 395 /
22 072, at 1 MHz 5917 / 11 544; `I2C_BM_XferIrq` per call (IT write of 16 bytes) 147–269 / 133–379.

### 9.5 Not exercised

From the "not exercised" lists of the report, F0 to the close, with what later phases ran taken out.

**Bench (F0)**

- **F334 holds:** only the release by time was used until T9 (`EDGES n` from F1 on). The **500 ms limit
  of S21** was tried only on the host's guard (mock) and by hand, with a development `HOLD SDA 600` that the
  F334 refused (`ERR HOLD ms must be 1..500 (S21)`, no capture). Its IWDG and the fault handlers that release
  the pins were never provoked; nor its S15 abort path (`I2C_TARGET ABORT S15`), whose host report was
  tried on mocks only.
- **Tools:** `--read-ob` and `--ob` never run for real with `--sn` (`--dry-run` only; the `--tool openocd`
  refusal in `--dry-run`); a ST-LINK without VCP, or with another VCP; the host's S19 aborts of m27 by mock
  only. The other benches (`gpio`, `tim`, `spi`) still reset without `sn=` and pick the port by
  `autodetect()`: they run with the F334's USB unplugged (S18). The "GPIO" state (J2/J3 out, F334
  unplugged) and those benches were not run in this work; nor a cold start (sessions begin with a reset by SWD).

**`i2c_bm` (F1)**

- **`ARLO`, multi-master** (T11): one master (S16); `TakeErrors` never saw `ARLO`. **2.19.2 (`BERR`)** cannot
  be provoked: no `BERR` in any capture, in any tier. 2.19.3/5/7: out of scope. **Slave, 10 bits,
  SMBus/PEC, `TIDLE = 1`, `TIMEOUTB`:** outside the contract.
- **I2C2 and I2C3 have no pins:** image (T2), refusals (T1), RCC banks (T13), T15; no transfer. I2C1 by DMA
  only from CPU2 (DMA2, not DMA1); I2C4 by BDMA only from CPU1.
- **1 MHz timings not measured** (no scope without taps, D3): data and duration only; nor the electrical
  effect of the Fm+ bits (I2C2's set without a bus; I2C1's and I2C4's around the 1 MHz traffic).
- **`RecoverBus` with SCL held:** only SDA (T9); its `ERR_BUS` branch by reading only.
- **T10(b) without taps:** no scope, so the `TIMEOUT` time is bounded only from above (≤ 11 ms from when
  CPU1 sees SCL low) and weakly from below.
- **The B1 line** of the firmware is cut at 160 characters by `App_Log`: it loses the fixed text "(IT write
  of 16)", no figure.
- **Partly:** `GetKernelSource` only with `prj_01`'s PCLK (not with T12's HSI); DMA chunking only at 256
  and 4096 bytes (`n < 2` and `n > 65 535` refused, T1); T12's control (2.19.6 without preload): not
  reproduced (decision 6).
- **Paths never taken:** `WaitEnd` with `CYCCNT` stopped on entry (checked in the disassembly); `CYCCNT`
  stopped inside any wait (BM or LL: `clk_expired()` has the exit, no test stops `CYCCNT`); an `Init` over
  `TIMOUTEN = 1` without `Deinit`; `ERR_VERIFY` of `DmaStart*`; the Fm+ clean-up of `bench_safe()`.

**`i2c_ll` (F2)**

- **`ERR_VERIFY` of `i2c_ll`** (the `Init` read-back, a `PE = 0` that does not fall): never fails on the
  bench, no test. **`I2C_LL_Init` over the same running configuration** (a software reset): not tried
  with LL; with another `TIMINGR`, DNF, filter or timeout, T1 refuses it with `ERR_BUSY` through LL too.
- **TIMEOUT, NACK on data, stretching, `RecoverBus`, 2.19.6:** T8, T9, T10, T12 only through BM; the
  `TIMEOUT` path of `I2C_LL_XferIrq` and of the LL waits never ran (the address NACK did: 0x43 in T4).
  `BERR`/`ARLO` in `I2C_LL_XferIrq`: as in F1. I2C2/I2C3 with LL: T1, T2 only. T5 with LL at 1 MHz: no
  scope. B1 with LL: not measured (R4-1).
- **Recorded, not judged:** the host's latency to each T0C hold (`first_us`, 62–68 ms in the F2 correction's runs), only
  with the 2 s limit. **T9 and T9(b) without taps:** no scope; T9(b) in notaps is judged as `5 − pulses`,
  and the firmware's span only recorded (1077 µs). **T9(b)** only gave `ERR_ARLO`; `ERR_BUS`/`ERR_TIMEOUT`
  accepted, never seen.

**`i2c_hal` (F3)**

- **IC1** (`BERR` in `i2c_hal`): cannot be provoked; the HAL's path (`ERR_HAL`, no STOP) by reading only.
- **A blocking HAL call's timeout** (`ERR_TIMEOUT`, the instance with no STOP or reset) never happened: the
  `ERR_TIMEOUT`s seen are `I2C_HAL_Wait`'s (T6C, T7C, m4). No call with a large `timeout_ms` against a held
  SCL (T10 is BM only); `HAL_MAX_DELAY` by refusal only (T1).
- **NACK on data, stretching, held bus, `RecoverBus`, 2.19.6 through `i2c_hal`:** T8, T9, T10, T12 BM only
  (the address NACK did run: 0x43 in T4). The missing preload of the blocking `HAL_I2C_Mem_Read`: `prj_01`
  runs APB/kernel = 1, outside the 1.5–3 window; not provoked.
- **Not run:** `WriteRead` with 2 address bytes (`I2C_MEMADD_SIZE_16BIT`; only `ntx = 1` on the bus, `ntx =
  3` refused); the `StartIt` probe (`ntx = nrx = 0`); `I2C_HAL_Init` with `ERR_BUS`; the blocking calls with
  the tick stopped (no exit, by design). `ERR_VERIFY` of `i2c_hal` only in the failed `a15c442` run.
- **I2C1–I2C3 with the HAL:** T1 (I2C2) and T2 (I2C2, I2C3) only, **no transfer on I2C1 by HAL**; CM4
  does not build the tier. T5 with HAL at 1 MHz: no scope. `ARLO`: one master.
- **Its line text:** the T7(b)-HAL control line says "WaitEnd" (F1's phrase) though the wait is
  `I2C_HAL_Wait`; the line's name carries `HAL`.

**Close.** The examples of §7 compile (the report's "Cierre") but were **not run on the board**. Throughout:
**silicon other than Rev V**; clock trees other than `prj_01`'s (except `CalcTiming`, T3).

---

## 10. Known limitations

### 10.1 `i2c_hal` (from `i2c_hal.h`; lines of `stm32h7xx_hal_i2c.c`, HAL V1.11.6)

- **No `TIMEOUTR`:** `HAL_I2C_Init` (l. 535-642) never writes it; neither `I2C_IsErrorOccurred` (l. 7164-7284)
  nor `HAL_I2C_ER_IRQHandler` (l. 4664-4708) looks at `TIMEOUT`. Worse than it looks: with `ERRIE` set a
  `TIMEOUT` would re-enter `HAL_I2C_ER_IRQHandler` for ever, which never clears `TIMEOUTCF`. So `timeout_scl_low
  ≠ 0` is `ERR_UNSUPPORTED` (owner, 2026-10-09): a held SCL ends a HAL transfer only by `timeout_ms`.
  **`HAL_MAX_DELAY` is refused** (`ERR_PARAM`, M4): the HAL never times out with it (l. 6980, 7020).
- **IC1, 2.19.2:** a `BERR` in master mode ends the transfer with `HAL_I2C_ERROR_BERR` (l. 7235-7243; IT/DMA
  l. 4671-4678, 4704-4706, then `I2C_ITError` l. 6548, without a STOP): `ERR_HAL`; call `I2C_HAL_Deinit`.
- **2.19.6:** preloaded in `Master_Transmit` (l. 1162), `_IT` (l. 1737), `_DMA` (l. 2012) and `Mem_Read_IT`
  (l. 2922), **not in the blocking `HAL_I2C_Mem_Read`** (address on `TXIS` after START, l. 5871-5883).
- **Errors and timeouts (blocking calls):** after a NACK the HAL writes STOP itself only if the bus is
  still busy and no STOP is already asked for (l. 7191-7196), then waits up to `I2C_TIMEOUT_STOPF` (25 ms,
  l. 347, 7205) more: a call can last `timeout_ms + 25 ms`. Every error exit flushes `TXDR` and runs
  `I2C_RESET_CR2` (l. 7267-7273), which clears only the transfer fields `SADD`, `HEAD10R`, `NBYTES`,
  `RELOAD`, `RD_WRN` (`stm32h7xx_hal_i2c.h` l. 781-784; `AUTOEND` stays). A wait that times out leaves the
  instance as it is, no STOP, no reset (l. 6984-6992). After any `ERR_TIMEOUT` or `ERR_HAL`, call
  `I2C_HAL_Deinit`.
- **A DMA transfer given up:** **with `ERR_TIMEOUT` the HAL stops nothing; with an error it starts an
  asynchronous abort that may still be running.** On a timeout of `I2C_HAL_Wait` the streams go on: abort
  them (`HAL_DMA_Abort`). On an error during the transfer (`NACKF` through `I2C_ITMasterCplt` l. 6197,
  `ARLO`/`BERR`/`OVR` through `HAL_I2C_ER_IRQHandler` l. 4706) `I2C_ITError` clears `TXDMAEN` or `RXDMAEN`
  and, if that direction's stream is not `READY`, calls `HAL_DMA_Abort_IT` on it (l. 6612-6627 TX,
  6642-6657 RX). It sets the handle `READY` before (l. 6587), so `Wait` may return with that abort running
  (O33): on DMA1/DMA2 it ends in the stream's IRQ, on the BDMA at once. Wait until no stream is in
  `HAL_DMA_STATE_ABORT`, then call `I2C_HAL_Deinit`. Otherwise a late transfer complete runs
  `I2C_DMAMasterTransmitCplt`/`ReceiveCplt` on the instance (l. 6744, 6750, 6778), and a stream still busy
  makes the next `StartDma` fail in `HAL_DMA_Start_IT` (l. 2077-2089) after it preloaded `TXDR` (l. 2012).
- **C10:** RMWs of `CR1`/`CR2` without PRIMASK: the filter calls (`_ex.c` l. 110-118, 156-170, `PE` dropped
  and raised, not read back), `I2C_TransferConfig` (l. 7321), `I2C_Enable_IRQ`/`_Disable_IRQ` (l. 7415,
  7480), `TXDMAEN`/`RXDMAEN` (l. 2075, 2222), `STOP` (l. 7196), `I2C_RESET_CR2`. Only the HAL's handlers may
  touch the instance during a transfer. The enables are written after START (l. 1745 → 1765, 2059 → 2075, 2207 → 2222).
- **Repeated start** only by `HAL_I2C_Mem_Read`/`_Mem_Read_IT` with 1 or 2 address bytes: `ntx > 2` is
  `ERR_UNSUPPORTED` in `WriteRead` (owner, 2026-10-09) and by extension in `StartIt` (O31); no write-read
  by DMA. By design (T2): `OAR1 = OA1EN | 0` (l. 600), `CR2.AUTOEND` (l. 619).
- **`StartDma`** does not check the DMA handle's alignment, unlike `spi_hal` (bytes only, O32).
- **`I2C_HAL_Wait`** with the tick stopped ends after `2 × (SystemCoreClock / 1000) × tick` polls, not a
  time: 13.6 M cycles (34 ms) here (O34). Call it in thread mode with PRIMASK clear; the blocking calls have
  no such exit. A handle in RESET never becomes `READY`: `ERR_TIMEOUT` after `timeout_ms`.

### 10.2 Maintenance notes

- **The transfer engine of `i2c_ll.c`** (`xfer_poll`, `XferIrq`, `WaitEnd`) is a second copy of
  `i2c_bm.c`'s (O24): a fix in one must be made in the other by hand. Only the checks are shared.
- **`LL_I2C_HandleTransfer` is a `MODIFY_REG`** that keeps `CR2.NACK` and `PECBYTE` (O25): today `CR2` is 0
  on entry to every path and no driver sets them. `i2c_ll` relies on `LL_I2C_SetSMBusTimeoutA` writing the
  whole register (O26; the LL header is pinned by its SHA-256 in the report).
- **`tools/ad2.py`'s decoder:** its glitch counter uses 3 samples, not a time (O11: at 4 MS/s every high pulse
  counts); T5's 0.5 V threshold is not a control (the channel swap is); tSU;DAT is attributed by the bit's
  role, not by who moved the line (O12). **The selftest (O15, deferred):** the 1 MHz guard reads PF14 only (J3
  forgotten in notaps is not seen); CPU2's 120 s rest would run `bench_safe()` on GPIOB in the middle of a
  CPU1 step (margin ~2.4×); a CPU2 result after its 30 s timeout may overlap the next command.

---

## 11. Consumers

- `prj_01_template_dualcore`: the `selftest-i2c` preset only; production links no `05_i2c` tier.

---

## 12. Changing this driver

1. **Frozen API, additions only** (catalog rule 2): `i2c_bm.h` as at `a6de15b` plus the seven
   `I2C_BM_Check*` of R4-2 (`52fa55c`), `i2c_ll.h` as at `c483bb9`, `i2c_hal.h` as at `01e462f`; after the
   merge: `git diff main -- '01_shared_drivers/05_i2c/*.h'`.
2. **One copy of every check** (`I2C_BM_Check*`); a fix of the transfer engine goes into BM and LL (§10.2).
3. **Before merging:** `build_all -Clean` 8/8, `selftest-i2c` with 0 warnings and with `-Werror`, the F334
   with 0 warnings, production `.map` unchanged, two clean unedited bench runs (taps and notaps).

---

## 13. History

| Milestone | Commits |
| :--- | :--- |
| Plan and implementer instructions (revision 1) | `05d4f7c` (2026-10-06) |
| F0: bench, F334 target, `--sn`, guards S15–S21, host; plan revision 2 (D9, M1) | F0 passes at `ee96c37` (auditor's runs at `85f9d29`); `4abd264` |
| F1: `i2c_bm`, decoder, tests T1–T15, B1; plan revision 3 (R3-1..R3-9, D10, D11) | `063c299`; F1 passes at `4fee0f9` (API `a6de15b`) |
| F2: plan revision 4 (R4-1, R4-2); `I2C_BM_Check*`, `i2c_ll`, tests; B2/B3 (windows) | `ceed2fc`; F2 passes at `d1c6f30` (API `c483bb9`) |
| F3: `i2c_hal`, the HAL module under the option, tests; M4 | F3 passes at `4e303f3` (API `01e462f`) |
| Close: this README and the catalog line; its correction (M5–M7, m50–m62) | `5a69267`; corrected after the close audit, part 1 (`9489df1`) |

Process documents (`IMPLEMENTATION_PLAN.md` revision 4, `IMPLEMENTER_INSTRUCTIONS.md`, `IMPLEMENTATION_REPORT.md`,
`AUDIT.md`): here until the catalog owner removes them; then each is at the parent of the commit that removes it:

```bash
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/05_i2c/IMPLEMENTATION_PLAN.md)^:01_shared_drivers/05_i2c/IMPLEMENTATION_PLAN.md
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/05_i2c/AUDIT.md)^:01_shared_drivers/05_i2c/AUDIT.md   # and the other two
```
