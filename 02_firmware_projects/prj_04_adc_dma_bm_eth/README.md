# prj_04_adc_dma_bm_eth

Dual-Core STM32H755ZI firmware product with **pure Bare-Metal (`*_bm`)** high-speed 16-bit ADC1 + circular DMA1 on Cortex-M4 (Domain D2, SRAM2), TIM3 TRGO hardware triggering at 500 kSPS, inter-core telemetry sharing via SRAM4 under HSEM 4, and on-demand UDP telemetry server (:5001) over 10/100 Ethernet (RMII + LAN8742 PHY) on Cortex-M7 with FreeRTOS and lwIP.

Includes host utilities for CLI queries (`tools/query_adc.py`) and a real-time web dashboard (`tools/adc_dashboard.py`).

Built according to the workspace **Plan A 3-Tier Architecture** consuming centralized drivers from `01_shared_drivers/` (`02_dma/dma_bm`, `07_adc/adc_bm`, `00_gpio/gpio_bm`, `03_tim/tim_bm`).

---

## 1. Core Architecture & System Roles

| Core | Image / Placement | Operating Model | Peripheral & Memory Ownership |
| :--- | :--- | :--- | :--- |
| **CPU1 (Cortex-M7)** | `cm7_firmware.elf`<br>Flash Bank 1 (`0x08000000`) | FreeRTOS (Priority grouping 4) | - Power (`PWR`), system clocks (`RCC`, PLL1 @ 400 MHz, PLL2P @ 25 MHz for ADC)<br>- Console VCP (`USART3` via `04_uart`)<br>- Ethernet MAC & DMA (`08_eth` via RMII) + LAN8742A PHY<br>- lwIP TCP/IP stack (IPv4, ICMP, UDP :5001)<br>- Descriptors in D2 SRAM3 (`0x30040000`, non-cacheable)<br>- Releases CPU2 via HSEM 0 |
| **CPU2 (Cortex-M4)** | `cm4_firmware.elf`<br>Flash Bank 2 (`0x08100000`) | **Bare-Metal (`*_bm`)** (No RTOS, No HAL ADC/DMA/TIM) | - **ADC1 (`adc_bm`)**: 16-bit, BOOST=10, 25 MHz kernel clock on `PB1`<br>- **TIM3 (`tim_bm`)**: Bare-metal TRGO on Update Event @ 500,000 Hz<br>- **DMA1 (`dma_bm`)**: Stream 0, DMAMUX Request 9, circular mode<br>- DMA circular buffer in D2 SRAM2 (`0x30020000`, 1024 samples)<br>- Computes statistics (min, max, avg, mV) in super-loop without floats<br>- Publishes telemetry + 512-sample waveform to SRAM4 (`0x38008000`) under HSEM 4 |

---

## 2. Memory Footprint Comparison (Cortex-M4)

By migrating from ST's HAL ADC & DMA to our centralized Bare-Metal drivers (`adc_bm` and `dma_bm`), Cortex-M4 achieves a dramatic reduction in flash footprint:

| Metric | `prj_03_adc_dma_eth` (HAL) | `prj_04_adc_dma_bm_eth` (Bare-Metal) | Improvement |
| :--- | :---: | :---: | :---: |
| **CM4 FLASH (Debug)** | 18,344 Bytes (1.75%) | **5,792 Bytes (0.55%)** | **-68.4% (-12,552 B)** |
| **CM4 FLASH (Release)** | 16,252 Bytes (1.55%) | **5,144 Bytes (0.49%)** | **-68.3% (-11,108 B)** |
| **Code (.text Debug)** | 18,320 Bytes | **5,768 Bytes** | **Zero HAL bloat** |
| **Code (.text Release)** | 16,228 Bytes | **5,120 Bytes** | **Zero HAL bloat** |
| **Compiler libcalls** | several `__aeabi_*` | **none** | No 64-bit software helpers |
| **Latency / Overhead** | State-machine callbacks | Direct register writes & atomic bitmask tests | Sub-microsecond determinism |

Read off `arm-none-eabi-size` (FLASH = `text` + `data`), not estimated. Regenerate with
`arm-none-eabi-size build/<preset>/CM4/cm4_firmware.elf` after any change that touches
the CM4 image, and update the table rather than leaving it to drift.

---

## 3. Hardware Allocation & Pinout

| Signal / Role | Pin | Nucleo Connector | Description |
| :--- | :--- | :--- | :--- |
| **Analog Input** (default) | `PB1` | CN9 pin 4 (A3) | ADC12_INP5 single-ended (0 to 3.3V). Fast channel (DS12923 Table 87 note 4) |
| **Analog Input** (alternative) | `PA3` | CN9 pin 1 (A0) | ADC12_INP15 single-ended (slow channel; via `Common/Inc/board_def.h`) |
| **Ethernet RMII** | `PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13` | On-board | 50 MHz reference clock from LAN8742A PHY |
| **VCP UART Console** | `PD8 (TX), PD9 (RX)` | ST-LINK VCP | 115,200 baud, 8N1 |
| **Status LEDs** | `PB0 (Green), PE1 (Yellow), PB14 (Red)` | On-board LD1-3 | Diagnostics and network state |

---

## 4. Bare-Metal Driver Architecture (`01_shared_drivers/`)

### A. DMA Bare-Metal Driver (`02_dma/dma_bm.[ch]`)
- **Controller & Stream Resolution**: Computes controller (`DMA1`/`DMA2`) and DMAMUX1 channel (0..7 for DMA1, 8..15 for DMA2) automatically from stream pointer address.
- **Stream Configuration**: Direct manipulation of `PAR`, `M0AR`, `NDTR`, `FCR` (direct mode) and `CR`.
- **Interrupts & Overruns**: Atomic checking and clearing of status flags (`TC`, `HT`, `TE`, `DME`, `FE`) in `LISR`/`HISR` and `LIFCR`/`HIFCR`.
- **Bus Alignment**: Explicit 32-byte alignment attributes (`DMA_BM_ALIGN_32`) for deterministic bus bursts.
- **FIFO & Memory-to-Memory**: Optional FIFO with a selectable threshold. RM0399 forbids both direct mode and circular mode in memory-to-memory transfers, so `DMA_BM_InitStream()` forces `DMDIS` for that direction and refuses a circular request outright, instead of leaving the caller to find out from the hardware.
- **Reported Failures**: `DMA_BM_Stop()` returns whether the stream actually went idle, and `InitStream` aborts on a false. Writes to `PAR`, `M0AR` and `NDTR` are ignored while `EN` is set, so a swallowed timeout would have produced a stream silently running its previous configuration.
- **Errata Hook**: `enable_trbuff` exposes `DMA_SxCR` bit 20, mandatory for USART/LPUART with DMA (ES0445 2.20.3).
- **Teardown**: `DMA_BM_DeinitStream()` stops the stream and resets its registers and DMAMUX channel.

### B. ADC Bare-Metal Driver (`07_adc/adc_bm.[ch]`)
- **Regulator & Deep Power-Down**: Clears `CR.DEEPPWD`, activates `CR.ADVREGEN`, and enforces `tADCVREG_STUP` stabilization.
- **Clock & Boost Mode**: Applies `BOOST[1:0]` while conversions are stopped. `ADC_BM_BOOST_AUTO` reads `DBGMCU->IDCODE` and picks the range for the silicon: Rev V indexes the table with `ker_ck / 2` (as ST's own `ADC_ConfigureBoostMode` does), Rev Y is a single bit at 20 MHz. A hardcoded constant is wrong on one of the two.
- **Errata ES0445 Compliance**: Enforces `ADC_CFGR_JQDIS` to disable the injected context queue.
- **Self-Calibration**: Automates offset and linearity calibration (`ADCALLIN`, `ADCAL`) while disabled (`ADEN = 0`).
- **Channel Preselection**: Direct setup of `PCSEL` (mandatory for STM32H7).
- **Fast Integer Statistics**: Zero floating-point in the hot path (`min`, `max`, `average`, `millivolts`), and 32-bit arithmetic throughout - `65535 * 3300` fits, so the CM4 image links no `__aeabi_*` helper at all.
- **Hardware Overrun Detection**: Enables `ADC_IER_OVRIE` and counts `OVR` separately from DMA transfer errors. A counter that can never increment does not read as "no overruns", it reads as a clean run.
- **Input Validation & Clean Start**: Rejects an out-of-range channel (the sampling-time write shifts by `(channel - 10) * 3`, which is undefined behaviour past 19), and clears `CFGR`, `CFGR2`, `PCSEL`, `SQR1..4`, `SMPR1..2`, `DIFSEL`, `IER` and `ISR` on init - enabling the peripheral clock does not reset the ADC, so a warm reset otherwise inherits the previous configuration.
- **Teardown**: `ADC_BM_Deinit()` stops conversions, disables the ADC and returns it to Deep-Power-Down.

### C. GPIO Shared Driver & Cross-Core Lock Hook (`00_gpio/gpio_bm.[ch]`, F1b)
- **Shared Mutex via Hardware Semaphore**: Hardware semaphore HSEM 5 protects concurrent register modifications on any GPIO port configured through the driver across both cores.
- **Timeout and Cycle Limits**: 60 µs timeout (`SHARED_GPIO_LOCK_US = 60U`), bounded by fixed compile-time cycle counts (`SHARED_GPIO_LOCK_US * 400U` = 24,000 cycles on CM7; `SHARED_GPIO_LOCK_US * 200U` = 12,000 cycles on CM4; defined in `Common/Inc/shared_memory.h`).
- **Shared Memory Diagnostics**: Diagnostic counters and hold measurements allocated in `SharedMemory_t` located in SRAM4 (`0x38008000`):
  - `m7_gpio_lock_fail_count` (offset `0x4fc`)
  - `m4_gpio_lock_fail_count` (offset `0x500`)
  - `m7_gpio_lock_hold_cycles_max` (offset `0x504`)
  - `m4_gpio_lock_hold_cycles_max` (offset `0x508`)
- **Hook Installation Sequence**:
  - **Cortex-M7**: Installed in `CM7/Core/Src/main.c` via `Shared_GpioLockInstall()` immediately after `Shared_Init()` and before releasing CPU2 via `HSEM_BM_BootReleaseCpu2()`.
  - **Cortex-M4**: Installed in `CM4/Core/Src/main.c` via `Shared_GpioLockInstall()` right after `HSEM_BM_TimebaseStart()` and before `ADC_Acquisition_Init()`.
- **Writers Protected (G1, G13)**: All GPIO configuration writers are routed through `00_gpio`:
  - CM7 Ethernet RMII (`08_eth`): uses `GPIO_HAL_Init` / `GPIO_HAL_DeinitPin`.
  - CM7 Console VCP (`04_uart`): uses `GPIO_HAL_Init` / `GPIO_HAL_DeinitPin`.
  - CM4 ADC pin setup (`adc_acquisition.c`): uses `GPIO_BM_ConfigPin` on PB1.
  - CM4 selftest bridge / loopback (`main.c`, `dma_selftest.c`): use `gpio_bm` on PD5/PD6.

### D. TIM Bare-Metal Driver (`03_tim/tim_bm.[ch]`, Phase F4)
- **Clock Enable & Clean Timebase**: `TIM_BM_EnableClock(TIM3)` enables APB1 clock from CM4 (RM0399 §9.7.1). `TIM_BM_InitTimeBase()` configures PSC=0, ARR=399 (period = 400 ticks at 200 MHz timer clock -> 500 kHz nominal trigger rate, 50 samples per 10 kHz signal period), ARPE=0, and edge-aligned up-counting mode with counter stopped.
- **Master Trigger Output (TRGO)**: `TIM_BM_ConfigMaster()` configures master mode selection to MMS = `010` (Update Event), routing timer updates directly to ADC1 external trigger line (`ADC_BM_TRIG_TIM3_TRGO`, `EXTSEL=4`).
- **Late Counter Start**: `TIM_BM_Start(TIM3)` is invoked after ADC calibration and DMA circular transfers are fully armed and running, eliminating spurious initial conversions and phase jitter.
- **Hardware Selftests (ADC & DMA)**:
  - In `adc_selftest_hw.c`, `Tim3_Prepare`, `Tim3_Run` and `Tim3_Halt` use `tim_bm` functions (`EnableClock`, `InitTimeBase`, `ConfigMaster`, `Start`, `Stop`), enforcing `TIM_STATUS_OK` verification.
  - In `dma_selftest.c`, basic timer TIM7 uses `tim_bm` for time base initialization, update DMA request enablement (`TIM_BM_EnableDmaRequest` for DMAMUX1 request 70), start, stop, and clean deinitialization (`TIM_BM_Deinit`).

---

## 5. Build and Flash Instructions

```powershell
# 1. Load STM32 environment
.\tools\env.ps1

# 2. Configure build presets
cmake --preset debug

# 3. Build both CM7 and CM4 images
cmake --build --preset debug

# 4. Flash both cores to Nucleo-H755ZI-Q
py tools/flash.py --tool cubeprog

# 5. Monitor logs on VCP console
py tools/monitor.py
```

---

## 6. Telemetry & Host Verification

### CLI Query Tool
```powershell
# Query current ADC readings:
py tools/query_adc.py --host 192.168.1.150

# Query as JSON:
py tools/query_adc.py --host 192.168.1.150 --json

# Continuous monitoring at 10 Hz:
py tools/query_adc.py --host 192.168.1.150 --rate 10
```

### Real-Time Web Dashboard
```powershell
py tools/adc_dashboard.py --host 192.168.1.150 --bind 127.0.0.1:8080
```
Open `http://127.0.0.1:8080` in your web browser for live chart visualization and telemetry metrics.

### Two things that will bite you

**Queries must originate on the board's own subnet.** The UDP services answer nobody
else (section 8). A query that times out from a different subnet is the filter doing its
job, not a fault: the board logs the first refusal with the source address and counts
the rest as `acl_drop` in its `[net]` status line.

**The binary WAVE reply is versioned.** Its header carries `version` and `header_len`,
and both host tools locate the sample block at `header_len` rather than a fixed offset.
That is what lets a field be appended without an older tool quietly drawing header bytes
as signal - which is exactly what the predecessor of `query_adc.py` did. A tool meeting
a firmware whose `version` it does not know refuses the packet and says so. Keep
`WaveformHeader_t` in `CM7/App/Src/adc_server.c` and the `WAVE_*` constants in both
tools in step; the firmware asserts its own header size at compile time.

---

## 7. Hardware Validation & Silicon Bring-Up Lessons (NUCLEO-H755ZI-Q)

During the hardware bring-up of `prj_04_adc_dma_bm_eth` on STM32H755ZI Rev V, the following hardware constraints and lessons learned were identified, addressed, and verified:

1. **SMPS Supply Architecture (Run\* Mode Lockup)**:
   - The NUCLEO-H755ZI-Q powers the core logic directly from the internal SMPS step-down regulator.
   - `HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY)` must be invoked in `SystemClock_Config()` before voltage scaling. Without it, the MCU stalls permanently waiting for `PWR_FLAG_VOSRDY`.
2. **Dual-Core Boot Gating & Warm Reset Handshake**:
   - Device 0x450 Rev V leaves CPU2 (Cortex-M4) boot gated by default in Option Bytes (`BCM4=0`).
   - CPU1 explicitly releases CPU2 via `HAL_RCCEx_EnableBootCore(RCC_BOOT_C2)`.
   - On warm resets, SRAM4 (`.shared_ram`) retains values across resets. CPU1 immediately clears `g_shared.magic = 0;` at entry of `main()` and pre-enables CM4 peripheral clocks (DMA1, ADC12, GPIOB, TIM3) before releasing HSEM 0. CPU2 always enters STOP mode waiting for HSEM 0 and `SHARED_MAGIC`, preventing boot races.
3. **STM32H7 ADC1 External Trigger Mapping (`EXTSEL`)**:
   - In STM32H7, `TIM3_TRGO` is mapped to `EXTSEL = 4` (`ADC_CFGR_EXTSEL_2`), defined in `adc_bm.h` as `ADC_BM_TRIG_TIM3_TRGO (0x00000004UL)`. Older families and channels used index 9; using 4 activates continuous conversions at 500 kSPS.
4. **ADC Calibration Duration & Read/Set (`RS`) Bit Safety**:
   - Linear calibration takes 16,384 ADC clock cycles (~0.65 ms at 25 MHz). The wait budgets are named constants in `adc_bm.c` (`ADC_BM_CALIBRATION_TIMEOUT_LOOPS`, `ADC_BM_VREG_STUP_LOOPS`, and the enable/disable/stop pair) rather than bare numbers, but they are still loop counts with no relation to the core clock - re-check them if this driver is ever used at a different frequency or optimisation level.
   - Bit modification on `ADC_CR` masks out all Read-to-Set bits (`ADC_CR_BITS_PROPERTY_RS`: `ADCAL`, `ADSTART`, `ADSTP`, `ADDIS`, `ADEN`) to prevent accidental re-triggers and ensure clean `ADEN=1` assertion.

### Measured Performance

Observed on the board, not estimated. The full record - including how each result was
obtained - is in [`docs/ADC_BRINGUP.md`](docs/ADC_BRINGUP.md) section 7.

- **Silicon**: revision `0x2003` (Rev V), from the boot banner. This is the revision
  `ADC_BM_BOOST_AUTO` resolves to the `ker_ck / 2` boost table; the Rev Y branch is
  still unexercised for want of the part.
- **Acquisition Rate**: `samples` climbed 5,208,064 -> 5,463,552 in 0.5 s, about
  511 kSPS, against a nominal 500 kSPS.
- **Overruns & Errors**: `err`, `ovr` and `adc_ovr` all zero across ~5 M samples. The
  third of those now means something: `ADC_IER_OVRIE` is enabled, so a hardware overrun
  would be counted. Before that it could not increment, and reading zero proved nothing.
- **Binary Protocol**: WAVE replies at 1080 bytes with `version=1` and `header_len=56`,
  and `adc_overrun_count` arriving through the dashboard's own `/api/samples`.
- **Source Filter**: verified in both directions - answered on-subnet with `acl_drop`
  flat at zero, refused off-subnet with `tx` frozen while `acl_drop` tracked `rx`.
- **Inter-Core Heartbeat**: advancing on both cores (`[ipc] m7=... m4=... (CPU2 alive)`).

A `ch` field reading nonsense is worth recognising on sight: it means CPU1 and CPU2
disagree about `SharedAdcData_t`, i.e. one core was reflashed and the other was not.
Flash both - `py tools/flash.py --core both`.

---

## 8. Security Considerations (UDP Amplification Risk)

Both diagnostic services - echo on port 7, telemetry and waveform on port 5001 - answer
without authentication, and a 4-byte `WAVE` command returns 1080 bytes: roughly 270x
amplification. Left open, the board is a usable reflector, and the echo service is one
by definition.

They therefore answer only sources on the interface's own subnet, taken from the live
netif so it follows DHCP and the static fallback. `NET_ACL_MODE` in
`CM7/App/Inc/net_config.h` selects the policy; `NET_ACL_ANY` restores the old behaviour
for a debugging session.

**This is not a security boundary.** A source address is trivially spoofed and anything
on the subnet is still answered. What it removes is the board's value as an off-link
amplifier, without a list of hosts to maintain. On an untrusted network keep upstream
firewall rules or VLAN isolation, and add a token check in `adc_server.c` if the
telemetry itself is worth protecting. ICMP is untouched, so `ping` stays the first
reachability check.

See [`docs/ADC_BRINGUP.md`](docs/ADC_BRINGUP.md) section 6 for the configuration detail
and section 7B for how the refuse path was actually proved, which needs more than a
timeout to be convincing.

---

## 9. GPIO Cross-Core Synchronization & F1b Validation

Phase F1b integrated and verified the cross-core GPIO lock hook and resolved all shared-port concurrency risks (findings G1 and G13 closed in `prj_04`).

### 9.1 Hook Implementation & Error Semantics

- **Hook Architecture**: Implemented in `Common/Src/shared_memory.c` via static callbacks `shared_gpio_lock()` and `shared_gpio_unlock()`, installed through the public function `Shared_GpioLockInstall()`. Uses HSEM channel 5 with a 60 µs bounded spin timeout (`SHARED_GPIO_LOCK_US = 60U`) bounded by fixed compile-time cycle limits (`SHARED_GPIO_LOCK_US * 400U` = 24,000 cycles on CM7, `SHARED_GPIO_LOCK_US * 200U` = 12,000 cycles on CM4, from `Common/Inc/shared_memory.h`). Retention hold time is measured using DWT `CYCCNT`.
- **Diagnostic Variables**: Placed in `SharedMemory_t` at SRAM4 (`0x38008000`):
  - `m7_gpio_lock_fail_count` (offset `0x4fc`)
  - `m4_gpio_lock_fail_count` (offset `0x500`)
  - `m7_gpio_lock_hold_cycles_max` (offset `0x504`)
  - `m4_gpio_lock_hold_cycles_max` (offset `0x508`)
- **Core Startup Sequence**:
  - **Cortex-M7**: `CM7/Core/Src/main.c` invokes `Shared_GpioLockInstall()` right after `Shared_Init()` and before releasing CPU2 via `HSEM_BM_BootReleaseCpu2()`.
  - **Cortex-M4**: `CM4/Core/Src/main.c` invokes `Shared_GpioLockInstall()` right after `HSEM_BM_TimebaseStart()` and before `ADC_Acquisition_Init()`.
- **Console MSP Error Discard (Observation m3)**: In `CM7/App/Src/app_log.c:24`, `App_Log_Init()` calls `(void)UART_Console_Init(UART_CONSOLE_DEFAULT_BAUDRATE);` discarding the return status (`HAL_BUSY` or `HAL_ERROR`). If console GPIO configuration fails in the MSP (e.g. bad parameter, lock busy, or verification failure), boot proceeds without logging the failure. Note that `m7_gpio_lock_fail_count` only increments if the lock acquisition actually times out, not on other GPIO configuration errors.

### 9.2 Validation Evidence (Captures in `docs/`)

All F1b test procedures were executed and verified against strict criteria:

| Test | Objective | Status / Key Metrics | Primary Captures |
| :--- | :--- | :--- | :--- |
| **T1** | Build Matrix & Presets | PASS: 8/8 matrix clean, 7 `prj_04` presets clean, 5 strict `-Werror` builds (0 warnings) | [`docs/gpio_f1b_t1_7cc456d.txt`](docs/gpio_f1b_t1_7cc456d.txt) |
| **T2a** | Static HAL Symbol Check | PASS: 0 unprotected HAL calls across 7 ELFs (`nm -u` per object from `build.ninja`); control `07db657` fails on all 5 CM7s | [`docs/gpio_f1b_t2_7cc456d.txt`](docs/gpio_f1b_t2_7cc456d.txt) |
| **T2b** | Preprocessor Regex Scan | PASS: 0 raw writes across 1951 units; CMSIS macro autoprueba detected 12/12 writes and 0/9 reads; negative control matched 20 raw writes in `07db657` | [`docs/gpio_f1b_t2_7cc456d.txt`](docs/gpio_f1b_t2_7cc456d.txt) |
| **T3** | Live SWD Hook Inspection | PASS: `s_shared_lock == g_shared_gpio_lock` on both cores, `HSEM_R5 = 0`, fail counts = 0, PB1/PB13/PD8/PD9/PB15/PD0 verified; negative control confirmed `s_shared_lock == NULL` | [`docs/gpio_f1b_t3_7cc456d.txt`](docs/gpio_f1b_t3_7cc456d.txt), [`docs/gpio_f1b_t3_control_7cc456d.txt`](docs/gpio_f1b_t3_control_7cc456d.txt) |
| **T4** | Lock Hold Time Measurement | PASS: CM7 = 1228 cycles (3.07 µs, 19.54× margin vs 24000), CM4 = 356 cycles (1.78 µs, 33.71× margin vs 12000), both `>= 10.0x` | [`docs/gpio_f1b_t4_7cc456d.txt`](docs/gpio_f1b_t4_7cc456d.txt) |
| **T5** | Production Regression (Debug/Release) | PASS: ICMP 4/4, UDP sweep 1..1472 B, burst 100/100, ADC samples growing strictly with `OVR == 0`, net error counters = 0 | [`docs/gpio_f1b_t5_debug_7cc456d.txt`](docs/gpio_f1b_t5_debug_7cc456d.txt), [`docs/gpio_f1b_t5_release_7cc456d.txt`](docs/gpio_f1b_t5_release_7cc456d.txt) |
| **T6a** | Warm Reset Stability | PASS: 20 consecutive warm resets via SWD with 10/10 T3 checks passing after each reset | [`docs/gpio_f1b_t6a_8ee9a13.txt`](docs/gpio_f1b_t6a_8ee9a13.txt) |
| **T7** | Template Identity & Non-interference | PASS: `prj_01` debug console active on VCP, identity confirmed by B6 download, PD8/PD9 in AF7 PP pull-up | [`docs/gpio_f1b_t7_7cc456d.txt`](docs/gpio_f1b_t7_7cc456d.txt) |
| **M6** | Final Image Reflash & Verification | PASS: `prj_04` debug reflashed and verified via `net_check.py` 3/3 PASS (ICMP, UDP sweep, burst) | [`docs/gpio_f1b_m6_netcheck_7cc456d.txt`](docs/gpio_f1b_m6_netcheck_7cc456d.txt) |
| **T6b** | Cold Boot Bench Verification | PASS: Physical cold boot: bench owner power-cycled the board (unplugged and reconnected USB); SWD readings (`RCC_RSR = 0x00FE0000`), T3 and T5 verification were executed by the auditor without flashing or resetting. SWD checks PASS, T3 PASS, T5 PASS | [`docs/gpio_f1b_t6b_rsr_492d156.txt`](docs/gpio_f1b_t6b_rsr_492d156.txt), [`docs/gpio_f1b_t6b_swd_492d156.txt`](docs/gpio_f1b_t6b_swd_492d156.txt), [`docs/gpio_f1b_t6b_t3_492d156.txt`](docs/gpio_f1b_t6b_t3_492d156.txt), [`docs/gpio_f1b_t6b_t5_netcheck_492d156.txt`](docs/gpio_f1b_t6b_t5_netcheck_492d156.txt), [`docs/gpio_f1b_t6b_t5_query_492d156.txt`](docs/gpio_f1b_t6b_t5_query_492d156.txt) |
| **Audit** | Independent Auditor Verification | PASS: SWD inspection (`docs/gpio_f1b_audit2_swd_32f343a.txt`), 20 warm resets (`docs/gpio_f1b_audit2_t6a_32f343a.txt`), T2 scan (`docs/gpio_f1b_audit3_t2_492d156.txt`), T3 and T5 (`docs/gpio_f1b_audit3_t3_492d156.txt`, `_t5_debug_`). F1 audit passed at `7687e91`, cold boot T6b passed at `74c9119`. |

### 9.3 Unexercised Tests in Hardware

- **`selftest-gpio` in F1b (H5/M2)**: Preset was compiled cleanly with zero warnings, but not executed on physical hardware under F1b per bench safety rule G-S1 (lines wired to AD2 instruments). Phase T15 hardware contention had already been exercised on `prj_04` during its own F1a (see `docs/gpio_f1a_selftest_ededa84.txt`, prior to F1b without production hook); what was not executed on the bench is `selftest-gpio` with the F1b changes (hook preservation H5 and M2).

---

## 10. Timer Driver Integration & F4 Validation Evidence (`03_tim`)

All F4 verification tests were executed and validated on hardware by the auditor:

| Test | Objective | Status / Key Metrics | Primary Captures |
| :--- | :--- | :--- | :--- |
| **T1** | Build Matrix & Presets | PASS: 8/8 matrix clean, `selftest` & `selftest-adc` clean with strict `-Werror` (0 warnings/errors) | [`docs/tim_f4_t1_0fa3f5a.txt`](docs/tim_f4_t1_0fa3f5a.txt) |
| **T2a** | Linked Symbol Audit | PASS: 4/4 `tim_bm` symbols present in CM4 (`TIM_BM_InitTimeBase`, `TIM_BM_EnableClock`, `TIM_BM_ConfigMaster`, `TIM_BM_Start`); 0/4 on control `04fb865` | [`docs/tim_f4_t2_0fa3f5a.txt`](docs/tim_f4_t2_0fa3f5a.txt) |
| **T2b** | Preprocessor Regex Scan | PASS: 0 illegal writes across 2282 compilation units (CM7 and CM4); CMSIS autoprueba 20/20 writes, 0/12 reads; control `04fb865` matched 78 legacy writes | [`docs/tim_f4_t2_0fa3f5a.txt`](docs/tim_f4_t2_0fa3f5a.txt) |
| **T3** | Live SWD Register Inspection | PASS: 32-bit register read bit-by-bit match against RM0399 §41 (`CR1=0x1`, `CR2=0x20`, `SMCR=0`, `DIER=0`, `PSC=0`, `ARR=0x18F`); altered table rejected | [`docs/tim_f4_audit3_t3_1caa7c3.txt`](docs/tim_f4_audit3_t3_1caa7c3.txt), [`docs/tim_f4_audit3_t3_control_1caa7c3.txt`](docs/tim_f4_audit3_t3_control_1caa7c3.txt) |
| **T4** | ADC Rate & Sequential Equivalence | PASS: 505 998.3 Hz vs 506 009.3 Hz (delta = 0.0022% <= 0.1%); oracle 50 019 (migrated) and 49 938 (control) samples/hb; `ovr`, `err`, `adc_ovr` all 0; restoration verified | [`docs/tim_f4_audit4_t4_1893222.txt`](docs/tim_f4_audit4_t4_1893222.txt) |
| **T5** | Production Regression (Debug/Release) | PASS: ICMP 4/4, UDP sweep, burst 100/100; `rx_nobuf`, `tx_nobuf`, `dma_err`, `acl_drop` all 0 | [`docs/tim_f4_audit3_t5_debug_1caa7c3.txt`](docs/tim_f4_audit3_t5_debug_1caa7c3.txt), [`docs/tim_f4_audit3_t5_release_1caa7c3.txt`](docs/tim_f4_audit3_t5_release_1caa7c3.txt) |
| **T6a** | Warm Reset Stability | PASS: 20/20 resets pass T3 and edge-aligned T4 with negative controls; oracle within 49 970..50 081; 0 overruns or errors | [`docs/tim_f4_audit4_t6a_1893222.txt`](docs/tim_f4_audit4_t6a_1893222.txt) |
| **T7** | Driver Regression & API Freeze | PASS: `prj_01 selftest-tim` compiles cleanly (exit code 0); `git diff main -- 01_shared_drivers/03_tim/*.h` strictly empty | [`docs/tim_f4_t7_0fa3f5a.txt`](docs/tim_f4_t7_0fa3f5a.txt) |
| **T8** | Hardware Selftests & Control Equivalence | PASS: `[m7_dma_test] OVERALL: PASS`, `[m4_dma_test] OVERALL=PASS (T4_TIM7=PASS)` on `selftest`, `HW RESULT PASS` with `HW T4 BM:1 LL:1` on `selftest-adc`; equivalence with control; restoration verified | [`docs/tim_f4_audit3_t8_1caa7c3.txt`](docs/tim_f4_audit3_t8_1caa7c3.txt) |


