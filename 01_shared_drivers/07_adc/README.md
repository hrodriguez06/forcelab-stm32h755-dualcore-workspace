# STM32H755 Shared ADC Driver (`01_shared_drivers/07_adc`)

Peripheral-First (Plan A), 3-tier shared ADC driver for the STM32H755ZI dual-core
microcontroller (ADC1, ADC2, ADC3). The three tiers expose the same feature set
behind the same function names (`ADC_BM_*`, `ADC_LL_*`, `ADC_HAL_*`), so a
project picks a tier by its constraints and not by the features it needs.

Status: v1.1. All three tiers were validated on a NUCLEO-H755ZI-Q (silicon Rev V,
`REV_ID = 0x2003`) on 2026-09-23/24, and the result closed by six independent
audits. What that covers, and what it does not, is in
[section 9](#9-validation).

---

## 1. Tiers

| Tier | Files | CMake target | Links | ST dependency | Typical use |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal** | `adc_bm.[c\|h]` | `stm32h7_driver_adc_bm` | `stm32h7_driver_gpio`, `stm32h7_driver_dma` | CMSIS only (`stm32h7xx.h`) | Cortex-M4 real-time acquisition, no HAL in the image |
| **Low-Level** | `adc_ll.[c\|h]` | `stm32h7_driver_adc_ll` | `stm32h7_driver_gpio`, `stm32h7_driver_dma` | `stm32h7xx_ll_adc.h` inlines | Same as BM, written with ST LL; leaves the same registers as BM |
| **HAL** | `adc_hal.[c\|h]` | `stm32h7_driver_adc` | `stm32h7_driver_gpio` | ST HAL (`HAL_ADC_*`, `HAL_DMA_*`) | Projects already built on the Cube HAL, callbacks, RTOS |

```cmake
target_link_libraries(cm4_firmware PRIVATE stm32h7_driver_adc_bm)   # or _ll, or stm32h7_driver_adc
```

```c
#include "adc_bm.h"     /* never "07_adc/adc_bm.h": the directory is on the include path */
```

The HAL tier needs `HAL_ADC_MODULE_ENABLED` and `HAL_DMA_MODULE_ENABLED` in the
project's `stm32h7xx_hal_conf.h`; without the first, `adc_hal.h` compiles to nothing.

- `stm32h7_driver_adc` (HAL) is part of the `stm32h7_shared_drivers` umbrella
  target. `_bm` and `_ll` are not, and have to be linked by name.
- BM and LL link `stm32h7_driver_dma`, which is the DMA **bare-metal** tier
  (`dma_bm`), and they read the DMA flags through it. They need nothing from the
  HAL. LL needs no LL `.c` file and no `USE_FULL_LL_DRIVER`.

### 1.1 Feature matrix

The same in all three tiers unless the cell says otherwise.

| ID | Feature | API |
| :--- | :--- | :--- |
| F1 | Circular DMA double buffering, half-buffer hand-off to the consumer | `IRQHandler_DMA`, `GetReadyHalf`, `ComputeStats` |
| F2 | Single software-triggered conversion (polling) | `ReadSingle` |
| F3 | Regular sequence of 1..16 channels, per-rank statistics | `InitEx`, `ComputeStatsChannel` |
| F4 | ADC3 internal channels (VREFINT, temperature, VBAT) with datasheet sampling-time check; VDDA and temperature | `CalculateVDDA`, `CalculateTemperature`, `InjectCalibration` |
| F5 | Analog watchdog 1, interrupt-storm safe | `awd_*` fields, `TakeAwdEvents` |
| F6 | Hardware oversampling 2x..1024x with a 16-bit truncation guard | `oversampling_*` fields |
| - | Automatic `BOOST` selection | `boost_mode = ADC_*_BOOST_AUTO` |
| - | Shared `CCR` protection (ADC1/ADC2) | inside `InitEx` |
| - | D-cache rules on Cortex-M7 (alignment check, invalidate before read) | inside `InitEx` and `ComputeStats*` |
| - | Errata ES0445: injected queue disabled (`JQDIS`) | inside `InitEx` |

---

## 2. Division of Work: Driver vs Application

Nothing about the board is hardcoded in the driver: instance, channels, pin,
trigger, DMA stream and clock all come from the configuration structure.

| Step | BM / LL | HAL |
| :--- | :--- | :--- |
| ADC bus clock (`ADC12EN` / `ADC3EN`) | driver | driver |
| ADC kernel clock source (`RCC_D3CCIPR.ADCSEL`, e.g. PLL2P) | **application (CPU1)** | **application (CPU1)** |
| Analog pin mode | driver if `analog_pin_port != NULL` | driver if `analog_pin_port != NULL` |
| `CCR` clock, `BOOST`, regulator, sequencer, sampling times, oversampling, AWD, `OVRIE` | driver (`InitEx`) | driver (`InitEx`) |
| ADC3 internal paths (`VREFEN`, `TSEN`, `VBATEN`) | driver, from the channel list | driver, from the channel list |
| DMA controller clock (`DMA1EN`/`DMA2EN`) | application (`dma_bm` does it in `DMA_BM_InitStream`) | **application** (`__HAL_RCC_DMA1_CLK_ENABLE()`) |
| DMA stream configuration | **application**, with `DMA_BM_InitStream` | driver (`HAL_DMA_Init` in `InitEx`) |
| NVIC priorities and enables | **application** | **application** |
| IRQ vectors | **application**, forwarding to `ADC_*_IRQHandler_DMA/ADC` | **application**, forwarding to `ADC_HAL_IRQHandler_DMA/ADC` |
| Calibration (`ADCAL`, offset + linearity) | application calls `Calibrate` | application calls `Calibrate` |
| Enable, start DMA, start conversions | application: `Enable`, `DMA_BM_Start`, `StartConversion` | application: `Start_DMA` |
| Trigger timer (e.g. TIM3 TRGO) | **application** | **application** |
| Factory calibration words on Cortex-M4 | **application** injects them ([section 6.6](#66-factory-calibration-and-the-cortex-m4)) | same |

### 2.1 Call sequence

```text
InitEx ──> Calibrate ──> Enable ──> (DMA start) ──> StartConversion ──> trigger timer on
   │                                                                        │
   │   ISR:  DMAx_Streamy_IRQHandler ─> ADC_*_IRQHandler_DMA  (HT/TC, errors)
   │         ADC_IRQHandler / ADC3_IRQHandler ─> ADC_*_IRQHandler_ADC  (OVR, AWD1)
   │                                                                        │
   │   Loop: GetReadyHalf ─> ComputeStats / ComputeStatsChannel ─> use stats
   │
Deinit  (ADC back to deep-power-down, registers cleared)
```

`Calibrate` must run while the ADC is disabled, which is the state `InitEx`
leaves it in. HAL: `ADC_HAL_Start_DMA` enables the ADC and starts DMA and
conversions in one call.

### 2.2 Handles

- Zero-initialise every handle (`= {0}` or `memset`) before `Init`/`InitEx`.
- Give handles static storage: the IRQ handlers write into them.
- HAL tier: one live handle per ADC instance. `InitEx` registers the handle in an
  internal table so the HAL callbacks can find it; `Deinit` removes it.

---

## 3. Configuration Reference (`ADC_*_ConfigEx_t`)

Fields are listed with the BM names; LL is identical with `ADC_LL_` constants, HAL
uses the ST HAL constants where noted. **Every field's zero value is a valid,
documented default**, so a partially filled structure keeps the behaviour of
earlier driver versions.

| Field | Meaning | 0 / NULL means |
| :--- | :--- | :--- |
| `adc_instance` | `ADC1`, `ADC2` or `ADC3` | invalid (rejected) |
| `channels`, `num_channels` | Sequence of `{channel, sampling_time}`, 1..16 ranks. Channel is 0..19 (BM/LL) or `ADC_CHANNEL_x` (HAL) | invalid (rejected) |
| `resolution` | `ADC_*_RESOLUTION_16B/14B/12B/10B/8B`, and `_14B_OPT`/`_12B_OPT` (Rev V only). HAL: `ADC_RESOLUTION_*` | 16 bits |
| `trigger_source` | `ADC_*_TRIG_*` (EXTSEL). HAL: `ADC_EXTERNALTRIG_*` | TIM1_CC1 (only used if `trigger_edge` ≠ 0) |
| `trigger_edge` | `ADC_*_TRIGGER_DISABLED/RISING/FALLING/BOTH`. HAL: `ADC_EXTERNALTRIGCONVEDGE_*` | software trigger |
| `boost_mode` | `ADC_*_BOOST_AUTO` or a fixed `ADC_*_BOOST_LE_*` ([section 6.1](#61-clock-and-boost)) | `BOOST = 00` (≤ 6.25 MHz) |
| `dma_mode` (BM/LL) | `ADC_*_DMA_DISABLED/ONESHOT/CIRCULAR` (`CFGR.DMNGT`) | no DMA |
| `dma_circular` (BM/LL) | Legacy flag; `true` = circular DMA | - |
| `dma_stream`, `dma_request` (HAL) | e.g. `DMA1_Stream0`, `DMA_REQUEST_ADC1` | no DMA |
| `buffer`, `buffer_length` | DMA buffer, length in samples: even and ≥ 2 (HAL: ≥ 2 × `num_channels`). Make each half a multiple of `num_channels` so ranks stay aligned | required only with DMA |
| `kernel_clock_hz` | Frequency of `adc_ker_ck` before `PRESC` | 25 MHz (legacy assumption) |
| `clock_mode` (BM/LL) | `ADC_*_CLOCK_ASYNC` or `SYNC_1/2/4` (`CCR.CKMODE`) | asynchronous |
| `prescaler` (BM/LL) | `ADC_*_PRESC_DIV1..DIV256` (`CCR.PRESC`) | /1 |
| `clock_prescaler` (HAL) | `ADC_CLOCK_ASYNC_DIVx` or `ADC_CLOCK_SYNC_PCLK_DIVx` | `ADC_CLOCK_ASYNC_DIV1` |
| `clock_mode` (HAL) | Reserved, not used: select the mode through `clock_prescaler` | - |
| `continuous_mode` (HAL) | Continuous conversions | disabled |
| `analog_pin_port`, `analog_pin_idx` | Pin put in analog mode by the driver (first channel only) | pin left to the application |
| `oversampling_enable`, `_ratio`, `_shift` | Ratio N = 2..1024, right shift 0..8 bits | oversampling off |
| `awd_enable`, `awd_all_channels`, `awd_channel`, `awd_high_threshold`, `awd_low_threshold` | Analog watchdog 1; thresholds are raw codes on the conversion-result scale | watchdog off |

The legacy single-channel `ADC_*_Init(handle, &AdcBm_Config_t)` is kept unchanged
for existing projects; it is a thin wrapper over `InitEx`. With its new fields
left at zero it programs the ADC bit-for-bit as before (verified by SWD against
the `a7f700a` baseline, see [section 9](#9-validation)).

### 3.1 What `InitEx` rejects

`InitEx` returns `false` without touching `CCR` or the partner ADC when:

- a pointer is `NULL`, `num_channels` is 0 or > 16, or the instance is not an ADC;
- the resolution or prescaler is not valid (the `_OPT` modes on Rev Y);
- DMA is requested and the buffer is `NULL`, odd-sized or too short;
- **on Cortex-M7 with DMA**: the buffer is not 32-byte aligned or half the buffer
  is not a multiple of 32 bytes (cache-line rule, [section 6.5](#65-d-cache-cortex-m7));
- an ADC3 internal channel has a sampling time below the datasheet minimum
  ([section 6.3](#63-sampling-time-and-internal-channels));
- oversampling would produce more than 16 bits after the shift
  ([section 6.4](#64-oversampling));
- the requested `CKMODE`/`PRESC` differs from `CCR` while the target ADC or its
  partner is enabled ([section 6.2](#62-shared-ccr-adc1--adc2)).

---

## 4. API Reference

`X` stands for `BM`, `LL` or `HAL`. BM and LL take an `ADC_TypeDef *` where HAL
takes the handle.

| Function | Tier | Purpose |
| :--- | :--- | :--- |
| `bool ADC_X_InitEx(handle, config)` | all | Full configuration ([section 3](#3-configuration-reference-adc__configex_t)); leaves the ADC disabled |
| `bool ADC_X_Init(handle, config)` | all | Legacy single-channel configuration |
| `bool ADC_X_Deinit(handle)` | all | Stop, clear configuration registers, deep-power-down |
| `bool ADC_X_Calibrate(adc \| handle)` | all | Offset + linearity calibration, single-ended; ADC must be disabled |
| `bool ADC_X_Enable(adc)` / `ADC_X_Disable(adc)` | BM, LL | Set `ADEN` and wait for `ADRDY` / set `ADDIS` and wait |
| `void ADC_X_StartConversion(adc)` / `bool ADC_X_StopConversion(adc)` | BM, LL | `ADSTART` / `ADSTP` |
| `bool ADC_HAL_Start_DMA(handle)` / `ADC_HAL_Stop_DMA(handle)` | HAL | Enable + circular DMA acquisition / stop |
| `bool ADC_X_ReadSingle(adc \| handle, channel, sampling_time, &raw, limit)` | all | One software conversion. `limit` is a spin count (BM/LL) or milliseconds (HAL) |
| `void ADC_X_IRQHandler_DMA(handle[, stream])` | all | Call from the DMA stream vector: HT/TC hand-off, DMA errors |
| `void ADC_X_IRQHandler_ADC(handle)` | all | Call from `ADC_IRQHandler` (ADC1/2) or `ADC3_IRQHandler`: overrun, AWD1 |
| `bool ADC_X_GetReadyHalf(handle, &half)` | all | `true` once per completed half-buffer |
| `void ADC_X_ComputeStats(handle, half, vref_mv)` | all | min / max / average / mV over one half (single channel) |
| `void ADC_X_ComputeStatsChannel(handle, half, rank, vref_mv)` | all | Same for one rank of an interleaved sequence |
| `uint32_t ADC_X_TakeAwdEvents(handle)` | all | Read-and-clear AWD1 events; re-arms the AWD1 interrupt |
| `bool ADC_X_InjectCalibration(&cal)` | all | Provide `VREFINT_CAL`, `TS_CAL1`, `TS_CAL2` |
| `uint32_t ADC_X_CalculateVDDA(raw, bits)` | all | VDDA in mV from a VREFINT reading; 0 if uncalibrated |
| `int32_t ADC_X_CalculateTemperature(raw, vdda_mv, bits)` | all | Junction temperature in whole °C; `ADC_X_TEMPERATURE_ERROR` (-999) if uncalibrated |
| `uint32_t ADC_BM_GetBoostBits(ker_ck_hz)` / `ADC_LL_GetBoostBits` | BM, LL | `BOOST` bits for a given ADC clock and the running silicon revision |
| `ADC_HAL_OnConvHalfCplt/OnConvCplt/OnError/OnLevelOutOfWindow` | HAL | Callback bodies, see [section 5](#5-hal-callbacks) |

`handle->stats` (`ADC_X_Stats_t`) carries the results and the health counters:
`latest_raw`, `average_raw`, `min_raw`, `max_raw`, `millivolts`, `sample_count`,
`dma_ht_count`, `dma_tc_count`, `dma_error_count`, `half_overrun_count` (the
consumer was too slow and a half was overwritten), `adc_overrun_count` (`ISR.OVR`)
and `awd_event_count`. A DMA stall and an ADC overrun are counted separately in
all tiers.

### 4.1 `ReadSingle` rules

- It refuses (returns `false`) while a conversion runs (`ADSTART = 1`) or DMA mode
  is configured (`DMNGT ≠ 0`): it never steals samples from a running acquisition.
- It enables the ADC if needed and **reprograms `SQR1`, the channel's sampling
  time and `EXTEN`** (software trigger). It does not restore them: call `InitEx`
  again before going back to a triggered or sequenced acquisition.
- The datasheet sampling-time check runs in `InitEx`, not in `ReadSingle`; pass
  the same (or a longer) sampling time for internal channels.

---

## 5. HAL Callbacks

The HAL tier routes ST's callbacks to its own handlers in one of three ways:

| Project setting | What happens |
| :--- | :--- |
| `USE_HAL_ADC_REGISTER_CALLBACKS = 1` | `InitEx` registers `ADC_HAL_On*` on the handle with `HAL_ADC_RegisterCallback`. |
| `USE_HAL_ADC_REGISTER_CALLBACKS = 0` (default) | `adc_hal.c` defines `HAL_ADC_ConvHalfCpltCallback`, `HAL_ADC_ConvCpltCallback`, `HAL_ADC_ErrorCallback` and `HAL_ADC_LevelOutOfWindowCallback`. |
| `= 0` and `ADC_HAL_CUSTOM_CALLBACKS` defined | The project defines those four callbacks itself and calls `ADC_HAL_On*` from them. |

`HAL_ADC_ErrorCallback` fires for both an ADC overrun and a DMA error; the driver
tells them apart and increments `adc_overrun_count` or `dma_error_count`.

---

## 6. Hardware Rules the Driver Enforces

### 6.1 Clock and BOOST

On Rev V silicon the ADC core sees the selected clock divided by a fixed 2
(RM0399, ADC clocks):

$$f_{\text{analog}} = \frac{f_{\text{adc\_ker\_ck}}}{\text{PRESC}} \cdot \frac{1}{2}
\quad\text{(asynchronous)}, \qquad
f_{\text{analog}} = \frac{f_{\text{adc\_sclk}}}{\text{CKMODE}} \cdot \frac{1}{2}
\quad\text{(synchronous)}$$

`CR.BOOST[1:0]` must match $f_{\text{analog}}$ and is written while
`ADSTART = JADSTART = 0`:

| $f_{\text{analog}}$ (Rev V) | `BOOST` | Constant |
| :--- | :---: | :--- |
| ≤ 6.25 MHz | `00` | `ADC_*_BOOST_LE_6_25MHZ` |
| 6.25 – 12.5 MHz | `01` | `ADC_*_BOOST_LE_12_5MHZ` |
| 12.5 – 25 MHz | `10` | `ADC_*_BOOST_LE_25MHZ` |
| 25 – 50 MHz | `11` | `ADC_*_BOOST_LE_50MHZ` |

Rev Y has a single `BOOST` bit, set above 20 MHz.

- **BM/LL, `BOOST_AUTO`**: computed from `kernel_clock_hz`, `clock_mode`,
  `prescaler` and the silicon revision read from `DBGMCU->IDCODE`.
  `kernel_clock_hz` must be the real `adc_ker_ck` (e.g. PLL2P = 25 MHz in the
  reference projects); the driver cannot read the clock tree of the other core.
- **HAL, `BOOST_AUTO`**: ST's `HAL_ADC_Init` computes it from the RCC clock tree.
  A fixed `boost_mode` never lowers the value ST computed.

### 6.2 Shared `CCR` (ADC1 / ADC2)

ADC1 and ADC2 share `ADC12_COMMON->CCR`. `CKMODE`, `PRESC`, `VREFEN`, `TSEN` and
`VBATEN` may only be written while **every** ADC of the pair is disabled
(RM0399, ADC common registers). All three tiers apply the same rule in
`InitEx`:

- both ADCs disabled → `CCR` is written;
- the target or its partner enabled → `CCR` must already hold the requested
  `CKMODE`/`PRESC`, otherwise `InitEx` returns `false` and changes nothing.

ST's own `HAL_ADC_Init` silently skips the `CCR` write in that situation and then
computes `BOOST` for a clock that is not the real one; the HAL tier's guard exists
to prevent exactly that (audit finding V1).

### 6.3 Sampling time and internal channels

| Channel | Where | Minimum $t_S$ | Source |
| :--- | :--- | :--- | :--- |
| VREFINT | ADC3 channel 19 (`ADC_*_CHANNEL_VREFINT`) | 4.3 µs | DS12923, table 20 |
| Temperature sensor | ADC3 channel 18 (`ADC_*_CHANNEL_TEMPSENSOR`) | 9 µs | DS12923, table 92 |
| VBAT / 4 | ADC3 channel 17 (`ADC_*_CHANNEL_VBAT`) | 9 µs | DS12923, table 94 |

`InitEx` converts each internal channel's `sampling_time` into microseconds at the
actual $f_{\text{analog}}$ and rejects the configuration if it is shorter. With
25 MHz and /1, `ADC_*_SAMPLETIME_810CYCLES_5` (64.8 µs) is safe for all three;
the self-tests use it.

For external pins, the sampling time depends on the source impedance $R_{AIN}$;
use the $R_{AIN}$ equation of the DS12923 ADC characteristics and its table of
minimum sampling time versus $R_{AIN}$ (table 87). Channels INP0–INP5 of ADC1/ADC2 are the fast
channels; the others go through the slower analog switch matrix.

### 6.4 Oversampling

`oversampling_ratio` is the plain ratio N (2..1024); the driver writes `N - 1` to
`CFGR2.OVSR`. The accumulated result has
$\text{bits} + \lceil\log_2 N\rceil - \text{shift}$ bits, and `InitEx` rejects any
combination above 16, because the DMA moves half-words and would silently drop the
top bits. Example: 16-bit, ×16 needs `shift ≥ 4`.

The HAL tier's `ADC_HAL_OVERSAMPLING_*X` macros stop at 256X; larger ratios are
passed as the integer.

### 6.5 D-cache (Cortex-M7)

With DMA on Cortex-M7, the buffer must be 32-byte aligned and each half must be a
multiple of 32 bytes; `InitEx` enforces it. `ComputeStats`/`ComputeStatsChannel`
invalidate exactly the half they read (`SCB_InvalidateDCache_by_Addr`, followed
by `__DMB()` as ES0445 2.1.5 requires) before reading it. **If the application reads the buffer directly** (e.g. copies a
waveform), it must invalidate that range itself or place the buffer in a
non-cacheable MPU region. The buffer must be in memory the DMA controller
reaches (not DTCM). On Cortex-M4 there is no D-cache and the check is compiled
out.

### 6.6 Factory calibration and the Cortex-M4

`VREFINT_CAL` (`0x1FF1E860`), `TS_CAL1` (`0x1FF1E820`, 30 °C) and `TS_CAL2`
(`0x1FF1E840`, 130 °C) live in System Memory, in the D1 domain.

- **Cortex-M4**: the D2-to-D1 bus bridge does not decode System Memory (RM0399,
  figure 1 note 2), and a read faults. The driver never reads it on CM4. The
  application must read the three words on a core that can (Cortex-M7), pass them
  across (shared SRAM, IPC) and call `ADC_X_InjectCalibration()`. Until then
  `CalculateVDDA` returns 0 and `CalculateTemperature` returns -999. The driver
  never substitutes typical values.
- **Cortex-M7**: the driver reads the words directly when nothing was injected.
  If the MPU uses a deny-all background region, add a region covering
  `0x1FF00000`–`0x1FF1FFFF`, read-only and non-cacheable.

The injected data is stored per tier and per core image and is used by every
handle of that tier.

### 6.7 Resolution encoding

`CFGR.RES` differs between revisions (Rev V: 8 bits = `111b`, `100b` reserved,
`101b`/`110b` = 14/12-bit optimised). The `ADC_*_RESOLUTION_*` constants are
logical values that the driver translates for the running revision; do not write
`RES` directly.

---

## 7. Silicon Errata (ES0445)

| Erratum | Handling |
| :--- | :--- |
| 2.10.x — injected context queue | `CFGR.JQDIS = 1` in all tiers; injected conversions are out of scope. |
| 2.10.4 — `ADC_AWDy_OUT` is reset by conversions of unmonitored channels | When the watchdog output gates or triggers a timer, set `awd_all_channels = true`. |
| 2.1.5 — store after cache maintenance without a barrier | `__DMB()` after the invalidate in `ComputeStats` / `ComputeStatsChannel` (Cortex-M7). |

AWD1 interrupt storm: at hundreds of kSPS a signal outside the window would raise
AWD1 on every conversion. The ADC IRQ handler counts the event and masks
`AWD1IE`; `TakeAwdEvents()` returns the count and re-arms the interrupt.

---

## 8. Examples

### A. BM - one channel at 500 kSPS, TIM3 trigger, circular DMA (Cortex-M4)

This is the configuration of `prj_04_adc_dma_bm_eth`. TIM3 setup is not shown.

```c
#include "adc_bm.h"
#include "dma_bm.h"

#define ADC_BUF_LEN  1024U                      /* 512 samples per half */

static uint16_t       s_buf[ADC_BUF_LEN];       /* in D2 SRAM, reachable by DMA1 */
static AdcBm_Handle_t s_adc = {0};

static const AdcBm_ChannelConfig_t s_ch = {
    .channel = 5U, .sampling_time = ADC_BM_SAMPLETIME_8CYCLES_5   /* PB1 = ADC12_INP5 */
};

bool Acq_Init(void)
{
    NVIC_SetPriority(DMA1_Stream0_IRQn, 5); NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    NVIC_SetPriority(ADC_IRQn, 5);          NVIC_EnableIRQ(ADC_IRQn);

    DmaBm_Config_t dma = {
        .stream = DMA1_Stream0, .request_id = 9U,              /* DMAMUX1: ADC1 */
        .direction = DMA_BM_DIR_PERIPH_TO_MEM,
        .periph_data_size = DMA_BM_DATA_SIZE_HALFWORD,
        .mem_data_size    = DMA_BM_DATA_SIZE_HALFWORD,
        .mem_inc = true, .circular = true, .priority = DMA_BM_PRIORITY_HIGH,
        .enable_it_ht = true, .enable_it_tc = true, .enable_it_te = true, .enable_it_dme = true,
        .periph_addr = (uint32_t)&ADC1->DR, .mem_addr = (uint32_t)s_buf,
        .buffer_length = ADC_BUF_LEN,
    };
    if (!DMA_BM_InitStream(&dma)) { return false; }

    AdcBm_ConfigEx_t cfg = {
        .adc_instance    = ADC1,
        .channels        = &s_ch,
        .num_channels    = 1U,
        .resolution      = ADC_BM_RESOLUTION_16B,
        .trigger_source  = ADC_BM_TRIG_TIM3_TRGO,
        .trigger_edge    = ADC_BM_TRIGGER_RISING,
        .boost_mode      = ADC_BM_BOOST_AUTO,
        .dma_mode        = ADC_BM_DMA_CIRCULAR,
        .buffer          = s_buf,
        .buffer_length   = ADC_BUF_LEN,
        .kernel_clock_hz = 25000000UL,                         /* PLL2P, set by CPU1 */
        .analog_pin_port = GPIOB,
        .analog_pin_idx  = 1U,
    };
    if (!ADC_BM_InitEx(&s_adc, &cfg) || !ADC_BM_Calibrate(ADC1) || !ADC_BM_Enable(ADC1))
    {
        return false;
    }

    DMA_BM_Start(DMA1_Stream0);
    ADC_BM_StartConversion(ADC1);
    TIM3->CR1 |= TIM_CR1_CEN;                                  /* conversions start now */
    return true;
}

void DMA1_Stream0_IRQHandler(void) { ADC_BM_IRQHandler_DMA(&s_adc, DMA1_Stream0); }
void ADC_IRQHandler(void)          { ADC_BM_IRQHandler_ADC(&s_adc); }

void Acq_Process(void)                                         /* super-loop */
{
    uint8_t half;
    if (ADC_BM_GetReadyHalf(&s_adc, &half))
    {
        ADC_BM_ComputeStats(&s_adc, half, 3300U);
        /* s_adc.stats.average_raw, .millivolts, .adc_overrun_count, ... */
    }
}
```

For a sequence, list several channels, size the buffer as a multiple of
`2 × num_channels`, and read each rank with
`ADC_BM_ComputeStatsChannel(&s_adc, half, rank, 3300U)`.

### B. LL - VDDA and temperature by polling (ADC3, Cortex-M4)

```c
#include "adc_ll.h"

static AdcLl_Handle_t s_adc3 = {0};

/* cal was read by CPU1 from System Memory and passed through shared SRAM */
bool Sensors_Read(const AdcLl_CalibrationData_t *cal, uint32_t *vdda_mv, int32_t *temp_c)
{
    static const AdcLl_ChannelConfig_t chs[2] = {
        { .channel = ADC_LL_CHANNEL_VREFINT,    .sampling_time = ADC_LL_SAMPLETIME_810CYCLES_5 },
        { .channel = ADC_LL_CHANNEL_TEMPSENSOR, .sampling_time = ADC_LL_SAMPLETIME_810CYCLES_5 },
    };
    AdcLl_ConfigEx_t cfg = {
        .adc_instance    = ADC3,
        .channels        = chs,                /* enables VREFEN and TSEN, checks t_S */
        .num_channels    = 2U,
        .resolution      = ADC_LL_RESOLUTION_16B,
        .boost_mode      = ADC_LL_BOOST_AUTO,
        .kernel_clock_hz = 25000000UL,
    };

    if (!ADC_LL_InjectCalibration(cal) ||
        !ADC_LL_InitEx(&s_adc3, &cfg) || !ADC_LL_Calibrate(ADC3) || !ADC_LL_Enable(ADC3))
    {
        return false;
    }

    uint16_t vref_raw = 0U, ts_raw = 0U;
    bool ok = ADC_LL_ReadSingle(ADC3, ADC_LL_CHANNEL_VREFINT,
                                ADC_LL_SAMPLETIME_810CYCLES_5, &vref_raw, 100000U) &&
              ADC_LL_ReadSingle(ADC3, ADC_LL_CHANNEL_TEMPSENSOR,
                                ADC_LL_SAMPLETIME_810CYCLES_5, &ts_raw, 100000U);
    if (ok)
    {
        *vdda_mv = ADC_LL_CalculateVDDA(vref_raw, 16U);                 /* 0 if uncalibrated   */
        *temp_c  = ADC_LL_CalculateTemperature(ts_raw, *vdda_mv, 16U);  /* -999 if uncalibrated */
    }
    (void)ADC_LL_Deinit(&s_adc3);
    return ok;
}
```

### C. HAL - DMA acquisition with the analog watchdog

```c
#include "adc_hal.h"

#define ADC_BUF_LEN  1024U

__attribute__((aligned(32))) static uint16_t s_buf[ADC_BUF_LEN];   /* 32 B rule on CM7 */
static AdcHal_Handle_t s_adc = {0};

bool Acq_Init(void)
{
    static const AdcHal_ChannelConfig_t ch = {
        .channel = ADC_CHANNEL_5, .sampling_time = ADC_SAMPLETIME_8CYCLES_5
    };

    __HAL_RCC_DMA1_CLK_ENABLE();                                /* not done by the driver */
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 5, 0); HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    HAL_NVIC_SetPriority(ADC_IRQn, 5, 0);          HAL_NVIC_EnableIRQ(ADC_IRQn);

    AdcHal_ConfigEx_t cfg = {
        .adc_instance       = ADC1,
        .channels           = &ch,
        .num_channels       = 1U,
        .resolution         = ADC_RESOLUTION_16B,
        .trigger_source     = ADC_EXTERNALTRIG_T3_TRGO,
        .trigger_edge       = ADC_EXTERNALTRIGCONVEDGE_RISING,
        .boost_mode         = ADC_HAL_BOOST_AUTO,
        .dma_stream         = DMA1_Stream0,
        .dma_request        = DMA_REQUEST_ADC1,
        .buffer             = s_buf,
        .buffer_length      = ADC_BUF_LEN,
        .kernel_clock_hz    = 25000000UL,
        .analog_pin_port    = GPIOB,
        .analog_pin_idx     = 1U,
        .awd_enable         = true,
        .awd_all_channels   = true,
        .awd_low_threshold  = 10000U,
        .awd_high_threshold = 55000U,
    };

    return ADC_HAL_InitEx(&s_adc, &cfg) &&
           ADC_HAL_Calibrate(&s_adc) &&
           ADC_HAL_Start_DMA(&s_adc);                           /* then start TIM3 */
}

void DMA1_Stream0_IRQHandler(void) { ADC_HAL_IRQHandler_DMA(&s_adc); }
void ADC_IRQHandler(void)          { ADC_HAL_IRQHandler_ADC(&s_adc); }

void Acq_Process(void)
{
    uint8_t half;
    if (ADC_HAL_GetReadyHalf(&s_adc, &half))
    {
        ADC_HAL_ComputeStats(&s_adc, half, 3300U);
    }
    if (ADC_HAL_TakeAwdEvents(&s_adc) != 0U)
    {
        /* signal left [10000, 55000] at least once since the last call */
    }
}
```

---

## 9. Validation

### 9.1 Hardware self-tests

`prj_04_adc_dma_bm_eth` (BM and LL) and `prj_03_adc_dma_eth` (HAL) carry
self-tests built only with `-DADC_DRIVER_SELFTEST=ON`. Both run on CPU2 before the
normal acquisition starts, and CPU1 prints one result line per 2 s under
`[m4_adc_test]`.

From the project directory, in one PowerShell session:

```powershell
. .\tools\env.ps1
cmake --preset selftest-adc                 # prj_03: --preset selftest
cmake --build --preset selftest-adc         # prj_03: --preset selftest
py tools/flash.py --preset selftest-adc --core both
py tools/monitor.py --log docs/hw_selftest_capture.txt --timeout 40   # then press RESET
```

The preset is **`selftest-adc` in `prj_04`**, whose `selftest` preset builds the
DMA driver selftest, and **`selftest` in `prj_03`**. When done, flash
`--preset release` again to put the production image back.

| Test | What it proves | Pass criterion |
| :--- | :--- | :--- |
| T1 | VREFINT read, VDDA computed | line `ADC:`/`HAL:` with `V=1`, `Vd` ≈ 3300 |
| T2 | 16/14/12/10/8-bit and the two optimised modes on ADC3 VREFINT, `CFGR.RES` read back | code scales as 2^(16−N) ±2 %; 8 bits = `111b` on Rev V |
| T3 | ADC1 + DMA at 500 kSPS from TIM3 for 1 s | 488.3 HT and TC per second ±1 %, no error of any kind |
| T3b | ADC3 {VREFINT, TEMPSENSOR} through DMA, `ComputeStatsChannel` | ranks separated; VREFINT within 3 % of T2 |
| T4 | DMA stopped under a running ADC | `adc_overrun_count` rises, `dma_error_count` stays 0 |
| T5 | AWD1 window that trips and one that does not | `A=1` |
| T6 | ADC1 asks for another prescaler while ADC2 is enabled | `InitEx` refuses, `CCR` and ADC2 untouched, /1 accepted |
| T7 | Oversampling ×16, shift 4; truncation rejected | `O=1` |
| T8 (CPU1) | DMA writes a cacheable AXI SRAM buffer holding dirty lines; `ComputeStats` of each tier | stats see the DMA data, not the dirty cache lines |
| G5 (prj_04) | BM and LL leave the same 12 configuration registers on ADC1 and ADC3 | `diff=0` |

Expected summary lines: `HW RESULT PASS mask=2047/2047` (prj_04) and
`HW RESULT PASS mask=31/31` (prj_03), plus
`[cm7_dcache_test] PASS`. The captures from the reference board are in each
project's `docs/hw_selftest_capture.txt`; the code is in
`CM4/App/Src/adc_selftest_hw.c` (criteria in its header) and in
`CM7/App/Src/app_main.c` for T8.

Results recorded on the reference board (`docs/hw_selftest_capture.txt` of each
project):

| Test | `prj_04` BM / LL | `prj_03` HAL | Expected |
| :--- | :--- | :--- | :--- |
| T2 | `v16=24334`, `v8=95`, `r8=7` (`RES = 111b`) | `v16=24327`, `v12=1521`, `v8=95`, `r8=7` | VREFINT 1.180-1.255 V (DS12923 table 20). The code scales as `v16 / 2^(16-N)`. 8 bits = `111b` on Rev V |
| T3 | `ht=488.0 tc=487.0` / `ht=488.0 tc=488.0`, every error counter 0 | `ht=488.0 tc=487.0`, every error counter 0 | 488.28 per second ±1 % (500 kSPS / 1024 samples) |
| T3b | VREFINT 24344 / 24362, TS 12737 / 12749 | VREFINT 24345, TS 12666, `halves=31` | VREFINT within 3 % of T2 |
| T4 | `ovr=2483 / 2482`, `dma=0` | `ovr=2480`, `dma=0` | About 2500 conversions in 5 ms, counted as overruns and not as DMA errors |
| T6 | Refused, `CCR` and ADC2 intact, /1 accepted | Same, and `boost=1 need=1` | `InitEx` refuses a conflicting prescaler |
| T8 | BM `dirty=39321→2048`, LL `dirty=34952→2048` | `dirty_cpu=39321`, `stats_dma=2048` | The DMA data wins over the dirty cache lines |
| G5 | `diff=0` on 12 registers | - | BM and LL leave the same registers |

### 9.2 Register compatibility

- **Against the baseline.** `prj_04` in its default configuration was read by SWD
  while acquiring (`docs/swd_register_dump.txt`, raw output). ADC1 and
  `ADC12_COMMON` are bit-for-bit identical to the pre-refactor baseline `a7f700a`
  in every configuration register. The auditor corroborated both halves on the
  board:
  - it read the flash back and found the CM4 image of `a7f700a` identical bit for
    bit to its own build;
  - it repeated the hot SWD read on `HEAD`. Only the data registers and `CALFACT`
    changed, and `CALFACT` is recalibrated at every boot.
- **BM against LL.** `G5_Run()` in `prj_04/CM4/App/Src/adc_selftest_hw.c`
  initialises ADC1 (application configuration) and ADC3 ({VREFINT, TEMPSENSOR},
  ×16 oversampling, shift 4) once with BM and once with LL. It then compares 12
  registers: `CR` without the start/stop bits, `CFGR`, `CFGR2`, `SMPR1/2`,
  `PCSEL`, `SQR1-4`, `IER` and `CCR`. LL cannot inherit BM's values, so a match
  is real: `ADC_BM_Deinit` clears every compared register except `CR.BOOST` and
  `CCR.CKMODE/PRESC`, and `ADC_LL_InitEx` writes those two explicitly.
- **What G5 does not cover.** It does not compare the analog watchdog registers
  (`LTR`/`HTR`, `AWDxCR`), `DIFSEL` or `OFRx`.

### 9.3 Not exercised in hardware

- **Silicon Rev Y** (`REV_ID < 0x2000`): the single-bit `BOOST` branch and the Rev Y
  `RES` encoding are covered by code review only.
- **HAL callback modes other than the default:** `USE_HAL_ADC_REGISTER_CALLBACKS
  = 1` and `ADC_HAL_CUSTOM_CALLBACKS`. `prj_03` builds with the default, `= 0`.
- **Other modes:** synchronous clock (`CKMODE ≠ 0`), one-shot DMA
  (`ADC_*_DMA_ONESHOT`) and HAL continuous mode.
- **An external signal on a real pin.** The optional T9 (PB1 strapped to 3V3 and
  to GND) was never implemented. T5 (AWD1) and T7 (oversampling) run on internal
  references and report only a pass flag (`A=1`, `O=1`).

---

## 10. Scope and Known Limits

Out of scope for v1.x:

- injected conversions (`JSQR`; the queue is disabled). ES0445 2.10.1-2.10.3,
  2.10.5, 2.10.7 and 2.10.8 all concern injected conversions and the context
  queue;
- dual-ADC simultaneous / interleaved modes (`CCR.DUAL ≠ 0`). ES0445 2.10.6: slave
  data may be shifted in dual regular simultaneous mode;
- differential inputs (`DIFSEL`; all channels are single-ended);
- analog watchdogs 2 and 3, and offset correction (`OFRx`);
- DFSDM and the low-power modes (auto-off, auto-wait).

Behaviour worth knowing:

- `ReadSingle` leaves the sequencer reprogrammed ([section 4.1](#41-readsingle-rules)).
- `CalculateTemperature` returns whole degrees; `vdda_mv` is expected in the
  datasheet VDDA range (it is not range-checked).
- `kernel_clock_hz = 0` means 25 MHz, the value of the reference projects; pass the
  real clock whenever it differs, since `BOOST_AUTO` and the sampling-time check
  depend on it.
- In the reserved offsets of the ADC and `ADC12_COMMON` blocks the silicon mirrors
  ADC1 registers; a raw SWD dump shows the `ISR` value there. It is not a fault.

---

## 11. Changing this driver

This driver is in a shared catalogue. `prj_03` and `prj_04` build it unchanged,
and `prj_02` and the template build the HAL tier through the umbrella target.
Before changing it:

1. **Only add, never change.** Existing prototypes, fields and macro values are
   frozen (`01_shared_drivers/README.md` rule 2). New fields go at the **end** of a
   struct, and the value 0 of every new field must reproduce the previous
   behaviour, because handles and configs are zero-initialised by contract (§2.2).
   New functionality gets an `Ex` suffix and the old function calls it, so there
   is only one code path.
2. **All hardware comes from the configuration:** instance, channels,
   `kernel_clock_hz`, clock mode and prescaler, trigger, and the optional analog
   pin. `adc_*.c` contains no `GPIOx`, no frequency other than the 25 MHz
   fallback, and no `ADC1/2/3` outside the instance tables.
3. **The shared `CCR` is written only while both ADCs of the pair are disabled,
   the target included** (§6.2). Never pulse `ADC12RST`: `RCC_AHB1RSTR` is in the
   register bank both cores share, and pulsing it resets the other ADC too.
4. **The driver resolves the silicon revision itself.** Resolution and `BOOST` are
   translated from `REV_ID`, and the public constants stay logical.
5. **ISRs only set flags and count.** No HSEM and no waiting. All three tiers keep
   the same counters, with the same meaning.
6. **BM and LL stay 1:1**, function by function, and G5 must keep reporting
   `diff=0`.
7. **Never define an ST global symbol without a way to turn it off** (see
   `ADC_HAL_CUSTOM_CALLBACKS`).
8. **Before merging:**
   - `.\tools\build_all.ps1 -Clean`, 8/8 with `-Werror`;
   - both selftest builds (`prj_03` preset `selftest`, `prj_04` preset
     `selftest-adc`);
   - the hardware run;
   - if an init path changed, a new SWD read compared against
     `docs/swd_register_dump.txt`.

---

## 12. History

| Milestone | Commits |
| :--- | :--- |
| First HAL `adc` and BM `adc_bm` (one channel, circular DMA) | `18d7f05`, `4d888b3` |
| `GPIO_BM_ConfigAnalog`, `adc_ll` tier, `InitEx` with F2-F6 | `be31f3c`, `d9962b5`, `baa8e08` |
| Six independent audits and their fixes (oversampling encoding, factory calibration on CM4, `CCR` guard in HAL, T2-T8 on hardware, SWD parity) | `42173dc` … `8568e5a` |
| Automated BM = LL register parity (G5) | `3c1ecb7` |
| README v1.1 | `bf9f4d0` |

The design plan and the full audit record (`PLAN.md`) were removed from this folder
once their content was folded into this README. They remain in git history:

```bash
git show c284211:01_shared_drivers/07_adc/PLAN.md
```
