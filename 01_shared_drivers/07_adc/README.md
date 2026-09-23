# STM32H755 Shared ADC Driver (`01_shared_drivers/07_adc`)

Peripheral-First (Plan A), 3-tier shared ADC driver for the STM32H755ZI dual-core microcontroller (ADC1, ADC2, ADC3).

---

## 1. Architecture & Layering Matrix

| Driver Tier | File Suffix | Library Target | Target Core | Typical Use Case | ST Dependencies |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal** | `adc_bm.[c\|h]` | `stm32h7_driver_adc_bm` | **Cortex-M4** / Early **M7** | High-throughput sensor sampling, DMA double-buffering, zero HAL overhead | Pure CMSIS (`stm32h7xx.h`), zero HAL/LL |
| **Low-Level** | `adc_ll.[c\|h]` | `stm32h7_driver_adc_ll` | **Cortex-M4** / **M7** | Hardware-near configuration with standard LL inlines, 1:1 register parity with BM | ST LL static inlines (`stm32h7xx_ll_adc.h`) |
| **HAL** | `adc_hal.[c\|h]` | `stm32h7_driver_adc` | **Cortex-M7** / **M4** | Standard STM32Cube ecosystem integration, callbacks, FreeRTOS | ST HAL (`stm32h7xx_hal.h`) |

### Capability Matrix across Tiers

| Capability | Bare-Metal (`_bm`) | Low-Level (`_ll`) | HAL (`_hal`) |
| :--- | :---: | :---: | :---: |
| **DMA Circular Double-Buffering (F1)** | Yes (`ADC_BM_IRQHandler_DMA`) | Yes (`ADC_LL_IRQHandler_DMA`) | Yes (`ADC_HAL_Start_DMA`) |
| **Single Polled Reading (F2)** | Yes (`ADC_BM_ReadSingle`) | Yes (`ADC_LL_ReadSingle`) | Yes (`ADC_HAL_ReadSingle`) |
| **Multi-channel Sequencer 1..16 (F3)** | Yes (`ADC_BM_InitEx`) | Yes (`ADC_LL_InitEx`) | Yes (`ADC_HAL_InitEx`) |
| **Interleaved Channel Statistics (F3)** | Yes (`ADC_BM_ComputeStatsChannel`) | Yes (`ADC_LL_ComputeStatsChannel`) | Yes (`ADC_HAL_ComputeStatsChannel`) |
| **Internal Channels: VREF/TEMP/VBAT (F4)** | Yes (ADC3, DS12923 timing check) | Yes (ADC3, DS12923 timing check) | Yes (ADC3, DS12923 timing check) |
| **VDDA / Temperature Calculations (F4)** | Yes (`ADC_BM_CalculateVDDA`/`Temp`) | Yes (`ADC_LL_CalculateVDDA`/`Temp`) | Yes (`ADC_HAL_CalculateVDDA`/`Temp`) |
| **Analog Watchdog 1 / AWD Events (F5)** | Yes (`ADC_BM_TakeAwdEvents`) | Yes (`ADC_LL_TakeAwdEvents`) | Yes (`ADC_HAL_TakeAwdEvents`) |
| **Oversampling with Truncation Guard (F6)**| Yes (2x..1024x, 16b guard) | Yes (2x..1024x, 16b guard) | Yes (2x..1024x, 16b guard) |
| **Automatic BOOST Mode Selection** | Yes (`ADC_BM_BOOST_AUTO`) | Yes (`ADC_LL_BOOST_AUTO`) | Yes (`ADC_HAL_BOOST_AUTO`) |
| **D-Cache Coherency Enforced** | Yes (32-byte alignment check) | Yes (32-byte alignment check) | Yes (32-byte alignment check) |

---

## 2. Silicon Errata, Hardware Constraints & Design Rules

### 2.1 Silicon Errata Workarounds
1. **Injected Context Queue Corruption (ES0445 §2.10.x)**:
   In all tiers, the injected queue is explicitly disabled via `CFGR |= ADC_CFGR_JQDIS`.
2. **Analog Watchdog Output Reset (ES0445 §2.10.4)**:
   The analog watchdog output toward timers (`ADC_AWDy_OUT`) resets upon completion of any regular conversion of an unmonitored channel. When gating or triggering external timers via AWD, configure `awd_all_channels = true`.
3. **Interrupt Storm Mitigation on Analog Watchdog (AWD1)**:
   Upon detecting an out-of-window condition in interrupt context, the driver masks `ADC_IER_AWD1IE` to prevent an IRQ flood (at up to 500 kSPS). The interrupt is safely re-armed when the consumer drains events via `ADC_*_TakeAwdEvents()`.
4. **Dual-ADC Common CCR Mutex / Protection**:
   ADC1 and ADC2 share `ADC12_COMMON->CCR`. RM0399 §26.4.3 stipulates that `CKMODE`, `PRESC`, `VBATEN`, `TSEN`, and `VREFEN` may only be modified when **all** associated ADCs (including the target instance) are disabled (`ADEN = 0`). Both `adc_bm` and `adc_ll` verify mutual state before writing `CCR`.

### 2.2 ADC Clocking & BOOST Configuration
The STM32H7 SAR analog core incorporates an internal divide-by-2 prescaler (RM0399 §26.4.3). Therefore, the analog SAR conversion clock frequency is:
$$f_{\text{analog}} = \frac{f_{\text{ker\_ck}}}{2 \times \text{PRESC}}$$

The hardware `BOOST[1:0]` bitfield in `ADC_CR` must be programmed according to $f_{\text{analog}}$ (while `ADSTART = 0` and `JADSTART = 0`):

| Kernel Clock $f_{\text{ker\_ck}}$ (PRESC=1) | SAR Core Clock $f_{\text{analog}}$ | RM0399 `BOOST[1:0]` | Driver Enum Constant |
| :--- | :--- | :---: | :--- |
| $\le 12.5\,\text{MHz}$ | $\le 6.25\,\text{MHz}$ | `00b` | `ADC_*_BOOST_LE_6_25MHZ` |
| $12.5\,\text{MHz} < f \le 25.0\,\text{MHz}$ | $6.25\,\text{MHz} < f \le 12.5\,\text{MHz}$ | `01b` | `ADC_*_BOOST_LE_12_5MHZ` |
| $25.0\,\text{MHz} < f \le 50.0\,\text{MHz}$ | $12.5\,\text{MHz} < f \le 25.0\,\text{MHz}$ | `10b` | `ADC_*_BOOST_LE_25MHZ` |
| $50.0\,\text{MHz} < f \le 100.0\,\text{MHz}$ | $25.0\,\text{MHz} < f \le 50.0\,\text{MHz}$ | `11b` | `ADC_*_BOOST_LE_50MHZ` |

When `boost_mode = ADC_*_BOOST_AUTO`, the driver automatically inspects $f_{\text{ker\_ck}}$ and selects the optimal `BOOST` mode.

### 2.3 Channel Types, Impedances & Sampling Times (DS12923 & RM0399)

The STM32H755 pins are partitioned into **fast** and **slow** analog channels:

| Channel Group | ADC Channels | Internal Switch Resistance $R_{ADC}$ | Minimum Sampling Considerations |
| :--- | :--- | :--- | :--- |
| **Fast Channels** | ADC1/ADC2: CH0 .. CH5 | $\sim 160\,\Omega - 260\,\Omega$ (DS12923 Table 86/87) | Direct pin routing. Supports $1.5$ to $2.5$ cycle sampling at low source impedance $R_{AIN}$. |
| **Slow Channels** | ADC1/ADC2: CH6 .. CH19 | $\sim 680\,\Omega - 1500\,\Omega$ (DS12923 Table 86/87) | Routed through internal analog mux switches. Requires higher sample cycles ($32.5+$ cycles) depending on $R_{AIN}$. |
| **Internal VREFINT** | ADC3: CH19 | Internal bandgap reference | **$t_S \ge 4.3\,\mu\text{s}$ mandatory** (DS12923 Table 88 / Table 93). Validated at driver init. |
| **Internal Temperature**| ADC3: CH18 | Internal temp sensor diode | **$t_S \ge 9.0\,\mu\text{s}$ mandatory** (DS12923 Table 90 / Table 94). Validated at driver init. |
| **Internal VBAT** | ADC3: CH17 | Built-in $1/4$ resistor bridge | **$t_S \ge 9.0\,\mu\text{s}$ mandatory** (DS12923 Table 91 / Table 95). Validated at driver init. |

The minimum sampling time equation is:
$$t_S \ge (R_{AIN} + R_{ADC}) \cdot C_{ADC} \cdot \ln(2^{N+2})$$

### 2.4 Handle Lifecycle & Zero-Initialization Rule
> [!IMPORTANT]
> Driver handles (`AdcBm_Handle_t`, `AdcLl_Handle_t`, `AdcHal_Handle_t`) must **ALWAYS** be zero-initialized (`= {0}` or `memset(&handle, 0, sizeof(handle))`) before calling `Init` or `InitEx`. Handles must have static lifetime (declared `static` or in file scope), never allocated uninitialized on a task/thread stack.

### 2.5 Out of Scope for v1.x
The following hardware features are explicitly deferred from the v1.x driver baseline:
- Injected conversion sequences (`JSQR`) - disabled via `JQDIS` due to errata ES0445.
- Dual-ADC simultaneous and interleaved modes (`ADC12_COMMON->CCR.DUAL != 0`).
- Differential input conversions (`DIFSEL` bits; v1 supports single-ended mode).

### 2.6 Dual-Core Domain Isolation & System Memory Calibration (Cortex-M4)
> [!WARNING]
> According to **RM0399 Section 2.1 / Figure 1 Note 2**, the D2-to-D1 AHB bus bridge connects Cortex-M4 (D2 domain) to Flash Banks 1 & 2 (`0x08000000-0x081FFFFF`), AXI SRAM (`0x24000000-0x2407FFFF`), and AHB3/APB3 peripherals (`0x50000000-0x57FFFFFF`).
>
> The bridge does **NOT** decode the System Memory address range (`0x1FF00000 - 0x1FFFFFFF`) residing in the D1 domain. Any direct memory read to the factory calibration registers (`VREFINT_CAL_ADDR = 0x1FF1E860`, `TS_CAL1 = 0x1FF1E820`, `TS_CAL2 = 0x1FF1E840`) from Cortex-M4 causes a bus matrix decode error resulting in an immediate **BusFault / HardFault**.
>
> To resolve this deterministically and safely:
> 1. Cortex-M7 reads the real factory calibration words at boot during `Shared_Init()` and publishes them in SRAM4 (`g_shared.vrefint_cal`, `g_shared.ts_cal1`, `g_shared.ts_cal2`, `g_shared.cal_valid`).
> 2. Cortex-M4 receives them via explicit dependency injection using `ADC_*_InjectCalibration(&cal)`.
> 3. If uncalibrated on Cortex-M4, `ADC_*_CalculateVDDA` returns `0U` and `ADC_*_CalculateTemperature` returns `ADC_*_TEMPERATURE_ERROR` (`-999L`). The driver **never** invents hardcoded constants.

---

## 3. Usage Examples

### A. Bare-Metal Tier (`_bm`) - Multi-channel with DMA Double-Buffering

```c
#include "adc_bm.h"

__attribute__((aligned(32))) static uint16_t s_dma_buf[512];
static AdcBm_Handle_t s_adc_handle = {0};

static const AdcBm_ChannelConfig_t s_channels[2] = {
    { .channel = 15U, .sampling_time = ADC_BM_SAMPLETIME_32CYCLES_5 },
    { .channel = 5U,  .sampling_time = ADC_BM_SAMPLETIME_32CYCLES_5 }
};

void ADC_Init(void)
{
    AdcBm_ConfigEx_t cfg = {
        .adc_instance        = ADC1,
        .channels            = s_channels,
        .num_channels        = 2U,
        .resolution          = ADC_BM_RESOLUTION_16B,
        .trigger_source      = ADC_BM_TRIG_TIM3_TRGO,
        .trigger_edge        = ADC_BM_TRIGGER_RISING,
        .boost_mode          = ADC_BM_BOOST_AUTO,
        .dma_mode            = ADC_BM_DMA_CIRCULAR,
        .buffer              = s_dma_buf,
        .buffer_length       = 512U,
        .kernel_clock_hz     = 25000000UL,
        .clock_mode          = ADC_BM_CLOCK_ASYNC,
        .prescaler           = ADC_BM_PRESC_DIV1,
        .analog_pin_port     = GPIOA,
        .analog_pin_idx      = 3U
    };

    if (ADC_BM_InitEx(&s_adc_handle, &cfg))
    {
        (void)ADC_BM_Calibrate(ADC1);
        (void)ADC_BM_Enable(ADC1);
        ADC_BM_StartConversion(ADC1);
    }
}
```

### B. Low-Level Tier (`_ll`) - Polling VREFINT and Temperature Sensor (ADC3)

```c
#include "adc_ll.h"
#include <string.h>

static AdcLl_Handle_t s_ll_handle = {0};

void Read_Sensors(void)
{
    AdcLl_ChannelConfig_t chs[2] = {
        { .channel = ADC_LL_CHANNEL_VREFINT,    .sampling_time = ADC_LL_SAMPLETIME_810CYCLES_5 },
        { .channel = ADC_LL_CHANNEL_TEMPSENSOR, .sampling_time = ADC_LL_SAMPLETIME_810CYCLES_5 }
    };

    AdcLl_ConfigEx_t cfg = {
        .adc_instance    = ADC3,
        .channels        = chs,
        .num_channels    = 2U,
        .resolution      = ADC_LL_RESOLUTION_16B,
        .trigger_edge    = ADC_LL_TRIGGER_DISABLED,
        .boost_mode      = ADC_LL_BOOST_AUTO,
        .kernel_clock_hz = 25000000UL
    };

    memset(&s_ll_handle, 0, sizeof(s_ll_handle));
    if (ADC_LL_InitEx(&s_ll_handle, &cfg))
    {
        (void)ADC_LL_Calibrate(ADC3);
        (void)ADC_LL_Enable(ADC3);

        uint16_t vref_raw = 0;
        if (ADC_LL_ReadSingle(ADC3, ADC_LL_CHANNEL_VREFINT, ADC_LL_SAMPLETIME_810CYCLES_5, &vref_raw, 100000U))
        {
            uint32_t vdda_mv = ADC_LL_CalculateVDDA(vref_raw, 16U);

            uint16_t temp_raw = 0;
            if (ADC_LL_ReadSingle(ADC3, ADC_LL_CHANNEL_TEMPSENSOR, ADC_LL_SAMPLETIME_810CYCLES_5, &temp_raw, 100000U))
            {
                float temp_c = ADC_LL_CalculateTemperature(temp_raw, vdda_mv, 16U);
                (void)temp_c;
            }
        }
        ADC_LL_Deinit(&s_ll_handle);
    }
}
```

### C. HAL Tier (`_hal`) - Standard HAL Integration with Analog Watchdog

```c
#include "adc_hal.h"
#include <string.h>

static AdcHal_Handle_t s_hal_handle = {0};

void HAL_ADC_Setup(void)
{
    AdcHal_ChannelConfig_t ch = {
        .channel       = ADC_CHANNEL_15,
        .sampling_time = ADC_SAMPLETIME_32CYCLES_5
    };

    AdcHal_ConfigEx_t cfg = {
        .adc_instance       = ADC1,
        .channels           = &ch,
        .num_channels       = 1U,
        .resolution         = ADC_RESOLUTION_16B,
        .trigger_source     = ADC_EXTERNALTRIG_T3_TRGO,
        .trigger_edge       = ADC_EXTERNALTRIGCONVEDGE_RISING,
        .boost_mode         = ADC_HAL_BOOST_AUTO,
        .awd_enable         = true,
        .awd_all_channels   = true,
        .awd_high_threshold = 55000U,
        .awd_low_threshold  = 10000U
    };

    memset(&s_hal_handle, 0, sizeof(s_hal_handle));
    if (ADC_HAL_InitEx(&s_hal_handle, &cfg))
    {
        (void)ADC_HAL_Calibrate(&s_hal_handle);
    }
}
```

---

## 4. Self-Test Framework (`ADC_DRIVER_SELFTEST`)

Both `prj_04_adc_dma_bm_eth` and `prj_03_adc_dma_eth` include a built-in automated hardware self-test:
- **CMake Option**: `-DADC_DRIVER_SELFTEST=ON`
- **Scope**:
  - `prj_04` (CM4): Runs BM vs LL register comparison after InitEx, polling `ReadSingle` on VREFINT and TEMPSENSOR, verifies `CalculateVDDA` (2800..3600 mV) and `CalculateTemperature` (0..100 °C), verifies oversampling truncation rejection, trips Analog Watchdog 1 and tests `TakeAwdEvents()`.
  - `prj_03` (CM4): Runs HAL tier self-test on ADC3 internal channels, oversampling truncation validation, and Analog Watchdog 1.
  - CM7: Validates D-cache coherency rules and 32-byte alignment verification.
- **Reporting**: Non-intrusive publication to `g_shared.m4_to_m7` mailbox without dynamic memory allocation.
