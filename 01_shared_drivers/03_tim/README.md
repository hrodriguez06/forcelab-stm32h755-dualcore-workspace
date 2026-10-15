# STM32H755 Shared Timer Driver (`01_shared_drivers/03_tim`)

Peripheral-First (Plan A), 3-tier shared driver for TIM1–TIM8 and TIM12–TIM17 of the STM32H755ZI: time
base (PSC/ARR, up/down/center, RCR, preload, one-pulse, URS), exact PSC/ARR and duty arithmetic, the
kernel clock of RM0399 table 59, flags, interrupts and DMA requests, output compare and PWM on channels
1–4, input capture (direct, indirect, TRC, prescaler, filter, `TISEL`, PWM input), master/slave and
de-initialisation without an RCC reset. Every parameter is validated before the first access, every
configuration write is read back, and no state is kept in RAM. LPTIM, HRTIM, complementary outputs, dead
time, break, encoder, channels 5–6 and DMA burst are not in the API.

Status: validated on a NUCLEO-H755ZI-Q (Rev V, `REV_ID = 0x2003`) on 2026-10-02/03, in
`prj_01_template_dualcore` only, with an Analog Discovery 2. Each phase passed an independent audit with
the auditor's own bench run: F0 (bench) at `cae5414`, F1 (`tim_bm`) at `3174fef`, F2 (`tim_ll`) at
`071bddb`, F3 (`tim_hal`) at `70b2062`, whose run executes the tests of all four phases: **259/259, `FW
VERDICT=PASS`** (`docs/tim_f3_audit_bench_70b2062.txt`, [section 9](#9-validation)).

---

## 1. Tiers

| Tier | Files | CMake target | Links | ST dependency | Typical use |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal** | `tim_bm.[c\|h]` | `stm32h7_driver_tim_bm` | - | CMSIS only (`stm32h7xx.h`) | Both cores, boot code, ISRs, the other tiers' validation |
| **Low-Level** | `tim_ll.[c\|h]` | `stm32h7_driver_tim_ll` | `_tim_bm` | `stm32h7xx_ll_tim.h` inlines and `LL_TIM_WriteReg`/`ReadReg` | Code written against `LL_TIM_*` constants |
| **HAL** | `tim_hal.[c\|h]` | `stm32h7_driver_tim_hal` | `_tim_bm` | `stm32h7xx_hal.h` (`HAL_TIM_*`) | Code that already keeps a `TIM_HandleTypeDef` |

```c
target_link_libraries(cm7_firmware PRIVATE stm32h7_driver_tim_bm)   # CMake; or _ll, _hal
#include "tim_ll.h"    /* never "03_tim/tim_ll.h": the directory is on the include path */
```

| Tier | CM7 | CM4 |
| :--- | :---: | :---: |
| BM, LL | yes | yes |
| HAL | yes | builds empty in `prj_01`: its CM4 `stm32h7xx_hal_conf.h` has no `HAL_TIM_MODULE_ENABLED` |

- **No unsuffixed target;** the umbrella `stm32h7_shared_drivers` links no `03_tim` tier.
- **`tim_bm` holds the only copy** of the capability table and of every check. `TIM_BM_Check*` runs the
  checks of an operation without writing; LL and HAL call it before they write, so every tier refuses the
  same request with the same status.
- **No state in RAM:** `.data = .bss = 0` in the three objects (`docs/tim_f3_map_sizes_1680350.txt`).
  `.text + .rodata`: `tim_bm` 6532 B (CM7) / 5882 B (CM4), `tim_ll` 4224 / 4122 B, `tim_hal` 3038 B.
- **No `USE_FULL_LL_DRIVER`, no `LL_TIM_Init`; `tim_hal` compiles to nothing** without
  `HAL_TIM_MODULE_ENABLED`. **No** `TIMx_IRQHandler`, `HAL_TIM_*Callback`, MSP or `NVIC_*` call.

### 1.1 Function matrix

| Operation | BM | LL | HAL |
| :--- | :---: | :---: | :---: |
| `IsValidInstance`, `GetCaps`, `KernelClockFromFields`, `GetKernelClockHz`, `CalcTimeBase`, `DutyToCompare`, `Check*` | yes | use `TIM_BM_*` | use `TIM_BM_*` |
| `InitTimeBase`, `ConfigOutput`, `ConfigInput`, `ConfigMaster`, `ConfigSlave` | yes | yes | yes |
| `Deinit` | yes | yes (→ BM) | yes (→ BM, plus the handle) |
| `EnableClock`, `DisableClock`, `TakeFlags`, `TakeOvercapture`, `WaitFlags`, `GetIRQn`, `GetDmaRequest` | yes | yes (→ BM) | use `TIM_BM_*` |
| `Enable/DisableIrq`, `Enable/DisableDmaRequest`, `GenerateEvent`, `SetCompare`, `WriteCcr` | yes | yes | use `TIM_BM_*` |
| `SetPeriod`, `Start`, `Stop`, `GetCounter` | yes | yes | use BM, or `HAL_TIM_Base_Start/Stop` |
| `OutputEnable` / `OutputDisable` | yes | yes | `StartPwm` / `StopPwm` (also CEN, §10.3) |
| `ConfigPwmInput` | yes | yes | two `ConfigInput` + `ConfigSlave` |
| `CaptureEnable` / `CaptureDisable`, `ReadCapture` | yes | yes | `StartCapture` (also CEN) / `HAL_TIM_IC_Stop`; `TIM_BM_ReadCapture` |

---

## 2. API summary

### 2.1 Status codes (`TimBm_Status_t`, all three tiers)

| Status | Meaning |
| :--- | :--- |
| `TIM_BM_OK` | Written and read back |
| `TIM_BM_ERR_PARAM` | Refused before any peripheral access, RCC included |
| `TIM_BM_ERR_UNSUPPORTED` | The instance lacks the feature (channel 3 on TIM12, TRGO on TIM13, an unwired ITR) |
| `TIM_BM_ERR_RANGE` | `CalcTimeBase`/`DutyToCompare`: not reachable with this PSC/ARR width |
| `TIM_BM_ERR_BUSY` | The hardware would ignore the write in this state: `CEN = 1` for the time base, `CCxE = 1` for a channel, `SMS ≠ 0` for a new trigger |
| `TIM_BM_ERR_VERIFY` | A register did not read back what was written (for example an unclocked timer) |
| `TIM_BM_ERR_TIMEOUT` | `WaitFlags`: the limit expired, or `CYCCNT` is not counting |
| `TIM_BM_ERR_HAL` | `tim_hal` only: a HAL call returned an error |

### 2.2 `tim_bm.h`

```c
bool           TIM_BM_IsValidInstance(const TIM_TypeDef *tim);      /* exact base of TIM1-8, 12-17 */
TimBm_Status_t TIM_BM_GetCaps(const TIM_TypeDef *tim, TimBm_Caps_t *out);
TimBm_Status_t TIM_BM_EnableClock(TIM_TypeDef *tim);   /* and DisableClock: this core's RCC bank */
TimBm_Status_t TIM_BM_KernelClockFromFields(uint32_t hclk1_hz, uint32_t d2ppre, uint32_t timpre, uint32_t *out_hz);
TimBm_Status_t TIM_BM_GetKernelClockHz(const TIM_TypeDef *tim, uint32_t hclk1_hz, uint32_t *out_hz);
TimBm_Status_t TIM_BM_CalcTimeBase(uint32_t timer_clock_hz, uint32_t target_hz, uint32_t arr_max, TimBm_TimeBase_t *out);
TimBm_Status_t TIM_BM_DutyToCompare(uint32_t arr, uint32_t counter_bits, uint32_t duty_x100, uint32_t *ccr);
TimBm_Status_t TIM_BM_InitTimeBase(const TimBm_TimeBaseConfig_t *cfg); /* CEN = 0 required */
TimBm_Status_t TIM_BM_SetPeriod(TIM_TypeDef *tim, uint32_t arr);
TimBm_Status_t TIM_BM_Start(TIM_TypeDef *tim);                       /* and Stop */
TimBm_Status_t TIM_BM_Deinit(TIM_TypeDef *tim);                      /* reset image, outputs first, no RCC */
static inline uint32_t TIM_BM_GetCounter(const TIM_TypeDef *tim);
TimBm_Status_t TIM_BM_EnableIrq(TIM_TypeDef *tim, uint32_t ev_mask);  /* and DisableIrq: UPDATE, CC1..4, TRIGGER */
TimBm_Status_t TIM_BM_EnableDmaRequest(TIM_TypeDef *tim, uint32_t ev_mask);  /* and Disable: UPDATE, CC1..4 */
uint32_t       TIM_BM_TakeFlags(TIM_TypeDef *tim, uint32_t ev_mask);  /* SR = ~taken, DSB */
uint32_t       TIM_BM_TakeOvercapture(TIM_TypeDef *tim, uint32_t ch_mask);   /* TIM_BM_CH(n) */
TimBm_Status_t TIM_BM_WaitFlags(TIM_TypeDef *tim, uint32_t ev_mask, uint32_t max_cycles, uint32_t *taken);
TimBm_Status_t TIM_BM_GenerateEvent(TIM_TypeDef *tim, uint32_t ev_mask);
TimBm_Status_t TIM_BM_GetIRQn(const TIM_TypeDef *tim, uint32_t ev, IRQn_Type *out);
TimBm_Status_t TIM_BM_GetDmaRequest(const TIM_TypeDef *tim, uint32_t ev, uint32_t *request_id);
TimBm_Status_t TIM_BM_ConfigOutput(const TimBm_OcConfig_t *cfg);      /* CCxE = 0 required */
TimBm_Status_t TIM_BM_SetCompare(TIM_TypeDef *tim, uint32_t channel, uint32_t ccr);
static inline void TIM_BM_WriteCcr(TIM_TypeDef *tim, uint32_t channel, uint32_t ccr);   /* unchecked */
TimBm_Status_t TIM_BM_OutputEnable(TIM_TypeDef *tim, uint32_t ch_mask);   /* CCxE, then MOE */
TimBm_Status_t TIM_BM_OutputDisable(TIM_TypeDef *tim, uint32_t ch_mask);  /* MOE = 0 when none left */
TimBm_Status_t TIM_BM_ConfigInput(const TimBm_IcConfig_t *cfg);       /* CCxE = 0 required */
TimBm_Status_t TIM_BM_ConfigPwmInput(TIM_TypeDef *tim, uint32_t ti, uint32_t edge, uint32_t prescaler, uint32_t filter, uint32_t tisel);
TimBm_Status_t TIM_BM_CaptureEnable(TIM_TypeDef *tim, uint32_t ch_mask);  /* and CaptureDisable */
static inline uint32_t TIM_BM_ReadCapture(const TIM_TypeDef *tim, uint32_t channel);    /* unchecked */
TimBm_Status_t TIM_BM_ConfigMaster(TIM_TypeDef *tim, uint32_t mms, bool msm);
TimBm_Status_t TIM_BM_ConfigSlave(TIM_TypeDef *tim, uint32_t sms, uint32_t trigger);
TimBm_Status_t TIM_BM_Check<Op>(...);   /* the checks of <Op>, nothing written: TimeBase, Period, Output,
                                           Compare, Channels, Input, PwmInput, Events, Master, Slave */
```

Constants: `TIM_BM_COUNT_UP/DOWN/CENTER1..3`, `TIM_BM_OC_FROZEN..PWM2` (the `OCxM` values),
`TIM_BM_IC_DIRECT/INDIRECT/TRC`, `TIM_BM_EDGE_RISING/FALLING/BOTH`, `TIM_BM_MMS_*` (the `MMS` values),
`TIM_BM_SMS_DISABLED/RESET/GATED/TRIGGER/EXT_CLOCK1/RESET_TRIGGER`, `TIM_BM_TRG_ITR(n)`,
`TIM_BM_TRG_TI1F_ED/TI1FP1/TI2FP2` (driver values, not the `TS[4:0]` encoding; ETRF has none and is
refused), `TIM_BM_EV_*` (the `DIER`/`SR` bit positions) and `TIM_BM_CH(n)` (bit n − 1 = channel n).

### 2.3 `tim_ll.h`

The operations of 2.2 as `TIM_LL_*` with the constants of `stm32h7xx_ll_tim.h`: `LL_TIM_COUNTERMODE_*`,
`LL_TIM_CHANNEL_CH1..CH4`, `LL_TIM_OCMODE_*`, `LL_TIM_ACTIVEINPUT_*`, `LL_TIM_IC_POLARITY_*`,
`LL_TIM_ICPSC_*`, `LL_TIM_IC_FILTER_*`, `LL_TIM_TIMx_TIy_RMP_*`, `LL_TIM_TRGO_*`,
`LL_TIM_SLAVEMODE_*`/`LL_TIM_CLOCKSOURCE_EXT_MODE1`, `LL_TIM_TS_*`; event masks `LL_TIM_DIER_UIE`,
`_CC1IE.._CC4IE`, `_TIE` (the bits of `TIM_BM_EV_*`); channel masks ORs of `LL_TIM_CHANNEL_CHx`. A
constant outside the API (`LL_TIM_TS_ETRF`, `LL_TIM_CHANNEL_CH1N`, …) gives `ERR_PARAM`.

### 2.4 `tim_hal.h`

```c
TimBm_Status_t TIM_HAL_InitTimeBase(TIM_HandleTypeDef *h, const TimBm_TimeBaseConfig_t *cfg);
TimBm_Status_t TIM_HAL_ConfigOutput(TIM_HandleTypeDef *h, const TimBm_OcConfig_t *cfg);
TimBm_Status_t TIM_HAL_StartPwm(TIM_HandleTypeDef *h, uint32_t channel);       /* channel 1..4 */
TimBm_Status_t TIM_HAL_StopPwm(TIM_HandleTypeDef *h, uint32_t channel);
TimBm_Status_t TIM_HAL_ConfigInput(TIM_HandleTypeDef *h, const TimBm_IcConfig_t *cfg);
TimBm_Status_t TIM_HAL_StartCapture(TIM_HandleTypeDef *h, uint32_t channel);
TimBm_Status_t TIM_HAL_ConfigMaster(TIM_HandleTypeDef *h, uint32_t mms, bool msm);
TimBm_Status_t TIM_HAL_ConfigSlave(TIM_HandleTypeDef *h, uint32_t sms, uint32_t ts);   /* TIM_BM_TRG_* */
TimBm_Status_t TIM_HAL_Deinit(TIM_HandleTypeDef *h);
```

Values are `tim_bm`'s. A NULL handle or configuration, or an `h->Instance` that is not a timer or not
`cfg->tim`, gives `ERR_PARAM`; then `TIM_BM_Check*` runs, then the HAL, then a read-back of what it
wrote. `TIM_HAL_Deinit` = `TIM_BM_Deinit` + the handle as `HAL_TIM_Base_DeInit` leaves it, no MSP.

---

## 3. Configuration guide

1. **Clock.** `TIM_BM_EnableClock(tim)` (this core's RCC bank), then `TIM_BM_GetKernelClockHz(tim,
   hclk1_hz, &clk)`: `D2PPRE1/2` and `TIMPRE` through RM0399 table 59, with the caller's `rcc_hclk1`
   (`HAL_RCC_GetHCLKFreq()`). In `prj_01`: 200 000 000 Hz on every instance (T5(e)).
2. **Time base.** `TIM_BM_CalcTimeBase(clk, target_hz, arr_max, &tb)`: the smallest prescaler that fits
   `arr_max` (`0xFFFF`, or `0xFFFFFFFF` on TIM2/TIM5), and the real error in `tb.error_ppm`. Then
   `InitTimeBase` with the counter stopped. In center mode the period is `2·ARR·(PSC+1)` ticks.
3. **Channel.** `TIM_BM_DutyToCompare(arr, bits, duty_x100, &ccr)` (10000 → `ARR + 1` = 100 %), then
   `ConfigOutput` with `CCxE = 0`.
4. **Pin.** The caller's: `GPIO_BM_ConfigAlternateEx` (`00_gpio`), AF of DS12923 table 9.
5. **Output.** `OutputEnable(tim, TIM_BM_CH(n))`: `CCxE`, then `MOE` on TIM1/8/15/16/17.
6. **Start.** `TIM_BM_Start`.

- **Capture:** `ConfigInput` or `ConfigPwmInput`, the pin, `Start`, then `CaptureEnable` (a capture taken
  while `CEN = 0` latches the stopped `CNT`). Discard the first PWM-input capture (example B).
- **Master/slave:** `ConfigMaster` and `ConfigSlave(slave, sms, TIM_BM_TRG_ITR(n))` with an ITR the slave
  wires (§6.1). A new trigger needs `SMS = 0` first (`TIM_BM_SMS_DISABLED`).
- **DMA:** `GetDmaRequest` gives the DMAMUX1 request (RM0399 table 126) for `02_dma`, then
  `EnableDmaRequest`; TIM4 CC4, TIM15 CC2 and TIM12–14 have none (`ERR_UNSUPPORTED`).
- **Interrupts:** `GetIRQn`; priority and enable are the project's. In the ISR, `TakeFlags`.

---

## 4. Project contract

- **One owner core per instance**, no cross-core lock. `prj_01` selftest: CPU1 owns TIM1–5, TIM7, TIM8,
  TIM12–14, TIM16, TIM17; CPU2 owns TIM15.
- **RCC banks (RM0399 9.5.10):** per core; a timer runs while either core enables it, so `DisableClock`
  on one core does not stop it (T16). `RCC_APBxRSTR` is never written; `RCC_CFGR` and `RCC_D2CFGR` are
  only read.
- **TIM6 is CPU1's HAL tick in `prj_01`** (`CM7/Core/Src/stm32h7xx_hal_timebase_tim.c`, with
  `HAL_TIM_PeriodElapsedCallback`): never pass it to a writing function there. CPU2 uses SysTick.
- **No callback, no MSP in the driver.** `HAL_TIM_Base_Init` calls the project's `HAL_TIM_Base_MspInit`
  while `h->State` is RESET (in `prj_01`, the HAL's empty weak one).

---

## 5. Rules the driver enforces

- **C6, validate first.** Every invalid, reserved or unsupported parameter is refused before the first
  access, RCC included, in a fixed order: NULL, instance, channel or event, capability of the instance,
  field range, state.
- **C10, concurrency on one core.** `SR` is cleared with one write (`SR = ~taken`). The RMWs of `DIER`,
  `CCER`, `BDTR`, `CR1`, `CR2`, `SMCR`, `CCMRx`, `TISEL` run with PRIMASK saved and restored (never
  `__enable_irq()`); one exception in `tim_hal` (§10.1).
- **C11, no glitch.** Configuration only with `CEN = 0`; the `UG` that loads PSC runs with `URS = 1`, so
  `SR.UIF = 0` after the init; a channel is complete before `CCxE`; `MOE` last; `Deinit` releases the
  outputs first. Leaving center mode, `CMS` is cleared before `DIR` is written (40.4.1).
- **C12, honesty.** Every configuration register is read back (`ERR_VERIFY`). A write the hardware would
  ignore is refused with `ERR_BUSY`: `CCxS` with `CCxE = 1`, `TS` with `SMS ≠ 0`, PSC or the mode with
  `CEN = 1`. `ARR = 0` is refused (the counter would not count, 40.4.14). `CalcTimeBase` returns the real
  frequency, not the requested one.
- **C13, bounded waits.** The only wait, `TIM_BM_WaitFlags`, is bounded by a number of DWT cycles the
  caller passes, and returns `ERR_TIMEOUT` if `CYCCNT` is not counting.

---

## 6. Silicon facts and errata

### 6.1 Capability table

`k_tim[]` in `tim_bm.c`, each row citing RM0399 rev 4; T2 compares it with `tst_hand_caps()`, typed
separately in the test.

| Instance | Bits | Ch | Up/down/center | RCR | BDTR (`MOE`) | Slave | MMS | ITR (source) | TIxSEL ≠ 0 | DMAMUX1 UP; CC1–CC4 |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :--- | :--- | :--- |
| TIM1 | 16 | 4 | yes | 16 bit | yes | yes | 0–7 | 0–3 (TIM15, TIM2, TIM3, TIM4) | TI1: 1 (COMP1) | 15; 11–14 |
| TIM8 | 16 | 4 | yes | 16 bit | yes | yes | 0–7 | 0–3 (TIM1, TIM2, TIM4, TIM5) | TI1: 1 (COMP2) | 51; 47–50 |
| TIM2 | 32 | 4 | yes | - | - | yes | 0–7 | 0–6 (TIM1, TIM8, TIM3, TIM4, ETH PPS, OTG HS SOF, OTG FS SOF) | TI4: 1–3 (COMP1, COMP2, both) | 22; 18–21 |
| TIM3 | 16 | 4 | yes | - | - | yes | 0–7 | 0–4 (TIM1, TIM2, TIM15, TIM4, ETH PPS) | TI1: 1–3 (COMP1, COMP2, both) | 27; 23–26 |
| TIM4 | 16 | 4 | yes | - | - | yes | 0–7 | 0–3 (TIM1, TIM2, TIM3, TIM8) | none | 32; 29–31, no CC4 |
| TIM5 | 32 | 4 | yes | - | - | yes | 0–7 | 0–3 (TIM1, TIM8, TIM3, TIM4), 6–8 (fdcan1\_soc, OTG HS SOF, OTG FS SOF) | TI1: 1–2 (fdcan1\_tmp, \_rtp) | 59; 55–58 |
| TIM6, TIM7 | 16 | 0 | up | - | - | - | 0–2 | - | - | 69 / 70 |
| TIM12 | 16 | 2 | up | - | - | yes | 0–5 | 0–3 (TIM4, TIM5, TIM13 OC1, TIM14 OC1) | TI1: 1 (spdifrx) | none |
| TIM13, TIM14 | 16 | 1 | up | - | - | - | no TRGO | - | none | none |
| TIM15 | 16 | 2 | up | 8 bit | yes | yes | 0–5 | 0–3 (TIM1, TIM3, TIM16 OC1, TIM17 OC1) | TI1: 1–6; TI2: 1–3 | 106; 105, no CC2 |
| TIM16 | 16 | 1 | up | 8 bit | yes | - | no TRGO | - | TI1: 1–3 (LSI, LSE, WKUP\_IT) | 110; 109 |
| TIM17 | 16 | 1 | up | 8 bit | yes | - | no TRGO | - | TI1: 2 (HSE\_1MHz), 3 (MCO1); 1 reserved | 112; 111 |

Vectors (`stm32h755xx.h`): TIM1 UP/CC/TRG\_COM; TIM8 `TIM8_UP_TIM13`, `TIM8_CC`, `TIM8_TRG_COM_TIM14`;
TIM12 `TIM8_BRK_TIM12`; TIM6 `TIM6_DAC`; the others one each. TRIGGER needs a slave controller.

TIM17 `TI1SEL = 0001` is reserved in RM0399 rev 4 although `stm32h7xx_ll_tim.h` defines
`LL_TIM_TIM17_TI1_RMP_SPDIF_FS`: every tier refuses it (`ERR_UNSUPPORTED`).

### 6.2 Errata (ES0445 rev 6, 2.16)

| ID | Erratum | In this driver |
| :--- | :--- | :--- |
| 2.16.1 | One-pulse mode trigger not detected in master-slave reset + trigger configuration | Refused with `ERR_UNSUPPORTED`, without writing: `ConfigMaster(msm)` with `OPM = 1` and `SMS = reset + trigger`, `ConfigSlave(RESET_TRIGGER)` with `MSM = 1` and `OPM = 1`, and `InitTimeBase(one_pulse)` in that state (T1). Use `MSM = 0` |
| 2.16.2 | Consecutive compare event missed in specific conditions (toggle mode, a CCR change from ARR to 0, or at the crest/valley in center mode) | No workaround exists (ES0445). Not applied |
| 2.16.3 | Output compare clear not working with external counter reset | `ocref_clr` (`OCxCE`, ETR) is outside the API: BM and LL write `OCxCE = 0`; `tim_hal` keeps a previous value (§10.3 row 5). ES0445's workaround (BKIN with `AOE = 1`) is outside the API too |
| 2.16.4 | Unexpected PWM output when using `ocref_clr` | Outside the API (combined and asymmetric PWM are refused); no workaround exists |

**TM1, outside the driver:** `prj_01`'s TIM6 tick doubles PCLK1 when `APB1CLKDivider ≠ RCC_HCLK_DIV1`,
the `TIMPRE = 0` row of table 59; with `TIMPRE = 1` and `D2PPRE1 = /4` it would run at twice the rate.
`TIM_BM_GetKernelClockHz` reads both fields. Correct for `prj_01`; not migrated.

---

## 7. Examples

Each compiles with `-Wall -Wextra -Wpedantic -Wconversion -Werror` (CMSIS/HAL headers as `-isystem`)
(A, B, D on CM4; C on CM7).

### A. 1 kHz, 25 % PWM on TIM15 CH1, from CPU2 (`tim_bm`)

```c
bool pwm_tim15(uint32_t hclk1_hz)
{
    uint32_t clk = 0U;
    TimBm_TimeBase_t tb;
    uint32_t ccr = 0U;
    if ((TIM_BM_EnableClock(TIM15) != TIM_BM_OK) ||
        (TIM_BM_GetKernelClockHz(TIM15, hclk1_hz, &clk) != TIM_BM_OK) ||
        (TIM_BM_CalcTimeBase(clk, 1000U, 0xFFFFU, &tb) != TIM_BM_OK) ||
        (TIM_BM_DutyToCompare(tb.arr, 16U, 2500U, &ccr) != TIM_BM_OK))
    {
        return false;
    }
    const TimBm_TimeBaseConfig_t base = { TIM15, tb.psc, tb.arr, TIM_BM_COUNT_UP, 0U, true, false, false };
    const TimBm_OcConfig_t oc = { TIM15, 1U, TIM_BM_OC_PWM1, ccr, false, true };
    return (TIM_BM_InitTimeBase(&base) == TIM_BM_OK) &&
           (TIM_BM_ConfigOutput(&oc) == TIM_BM_OK) &&
           (GPIO_BM_ConfigAlternateEx(GPIOE, 5U, 4U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE,
                                      GPIO_BM_SPEED_LOW) == GPIO_BM_OK) &&       /* PE5 = TIM15_CH1, AF4 */
           (TIM_BM_OutputEnable(TIM15, TIM_BM_CH(1)) == TIM_BM_OK) &&    /* CC1E, then MOE */
           (TIM_BM_Start(TIM15) == TIM_BM_OK);
}
```

### B. PWM input on TIM1 TI1 (`tim_ll`)

```c
bool pwm_input_tim1(uint32_t *period, uint32_t *high)
{
    const TimLl_TimeBaseConfig_t tb = { TIM1, 0U, LL_TIM_COUNTERMODE_UP, 0xFFFFU, 0U, false, false, false };
    uint32_t taken = 0U;
    if ((TIM_LL_EnableClock(TIM1) != TIM_BM_OK) || (TIM_LL_InitTimeBase(&tb) != TIM_BM_OK) ||
        (TIM_LL_ConfigPwmInput(TIM1, LL_TIM_CHANNEL_CH1, LL_TIM_IC_POLARITY_RISING, LL_TIM_ICPSC_DIV1,
                               LL_TIM_IC_FILTER_FDIV1, 0U) != TIM_BM_OK) ||
        (GPIO_BM_ConfigAlternateEx(GPIOE, 9U, 1U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE,
                                   GPIO_BM_SPEED_LOW) != GPIO_BM_OK) ||          /* PE9 = TIM1_CH1, AF1 */
        (TIM_LL_Start(TIM1) != TIM_BM_OK) ||               /* counter first, then the capture */
        (TIM_LL_CaptureEnable(TIM1, LL_TIM_CHANNEL_CH1 | LL_TIM_CHANNEL_CH2) != TIM_BM_OK) ||
        /* the first capture holds CNT from before the first reset: use the second */
        (TIM_LL_WaitFlags(TIM1, LL_TIM_DIER_CC1IE, 4000000U, &taken) != TIM_BM_OK) ||
        (TIM_LL_WaitFlags(TIM1, LL_TIM_DIER_CC1IE, 4000000U, &taken) != TIM_BM_OK))
    {
        return false;
    }
    *period = TIM_LL_ReadCapture(TIM1, LL_TIM_CHANNEL_CH1);   /* ticks, minus the reset delay (§10.6) */
    *high = TIM_LL_ReadCapture(TIM1, LL_TIM_CHANNEL_CH2);
    return true;
}
```

### C. 10 kHz update interrupt on TIM2, CPU1 code with a handle (`tim_hal`)

```c
static TIM_HandleTypeDef h_tim2;

bool tick_10khz_tim2(void)
{
    const TimBm_TimeBaseConfig_t c = { TIM2, 199U, 99U, TIM_BM_COUNT_UP, 0U, true, false, false };
    IRQn_Type irq;
    h_tim2.Instance = TIM2;
    if ((TIM_BM_EnableClock(TIM2) != TIM_BM_OK) || (TIM_HAL_InitTimeBase(&h_tim2, &c) != TIM_BM_OK) ||
        (TIM_BM_GetIRQn(TIM2, TIM_BM_EV_UPDATE, &irq) != TIM_BM_OK) ||
        (TIM_BM_EnableIrq(TIM2, TIM_BM_EV_UPDATE) != TIM_BM_OK))
    {
        return false;
    }
    HAL_NVIC_SetPriority(irq, 6U, 0U);    /* the project's; its TIM2_IRQHandler calls TIM_BM_TakeFlags */
    HAL_NVIC_EnableIRQ(irq);
    return HAL_TIM_Base_Start(&h_tim2) == HAL_OK;
}
```

### D. TIM2 counts the updates of TIM3 (master/slave, `tim_bm`)

```c
bool count_tim3_updates(void)
{
    const TimBm_TimeBaseConfig_t master = { TIM3, 199U, 999U, TIM_BM_COUNT_UP, 0U, false, false, false };
    const TimBm_TimeBaseConfig_t slave = { TIM2, 0U, 0xFFFFFFFFUL, TIM_BM_COUNT_UP, 0U, false, false, false };
    return (TIM_BM_EnableClock(TIM2) == TIM_BM_OK) && (TIM_BM_EnableClock(TIM3) == TIM_BM_OK) &&
           (TIM_BM_InitTimeBase(&master) == TIM_BM_OK) && (TIM_BM_InitTimeBase(&slave) == TIM_BM_OK) &&
           (TIM_BM_ConfigMaster(TIM3, TIM_BM_MMS_UPDATE, false) == TIM_BM_OK) &&       /* TRGO = update */
           (TIM_BM_ConfigSlave(TIM2, TIM_BM_SMS_EXT_CLOCK1, TIM_BM_TRG_ITR(2)) == TIM_BM_OK) &&  /* ITR2 = TIM3 */
           (TIM_BM_Start(TIM2) == TIM_BM_OK) && (TIM_BM_Start(TIM3) == TIM_BM_OK);
}
```

---

## 8. Test bench (Analog Discovery 2)

The bench of `00_gpio/README.md` §8, not one wire changed (connector pins there): NUCLEO-H755ZI-Q
(MB1363) and AD2 SN `210321B5D38D`, WaveForms GUI closed, `tools/ad2.py` over ctypes. The lines the TIM
tests use:

```text
 AD2                     NUCLEO-H755ZI-Q
 DIO1  ◄──────────────►  PE9   TIM1_CH1 (AF1): PWM out (T6, T11, T13a) or capture in (T8, T13b)
 DIO3  ◄──────────────►  PE11  TIM1_CH2 (AF1): PWM out (T6, T11) or capture in (T8)
 DIO5, DIO6  ◄─────────  PE13, PE14  TIM1_CH3, CH4 (AF1): PWM out (T6)
 DIO0, 2, 4, 7 ────────  PE8, PE10, PE12, PE15: never AF (T0a/T0b, S5 pulls)
 DIO8, DIO10 ──────────  PE3, PE4: never AF, inputs (T0a)
 DIO9, DIO11 ◄─────────  PE5, PE6    TIM15_CH1, CH2 (AF4, CPU2): PWM out (T6)
 DIO12 ───────────────►  PE2   HOST_REQ            DIO13 ◄──────  PE7   MCU_ACK
 DIO14 ◄───────────────  PA8   MCO1 = HSE/8 (AF0): TREF, T9, the reference of T6/T8
 GND   ─────────────────  GND  (first on, last off)    W1, W2, V+, V−: never connected
```

| AD2 | MCU | TIM use | Tests | Who drives |
| :--- | :--- | :--- | :--- | :--- |
| DIO1, DIO3 | PE9, PE11 | TIM1\_CH1, \_CH2 (AF1) | T6, T11, T13(a) in a "bus = MCU" step; T8, T13(b) in a "bus = AD2" step (Digital Out) | MCU **or** AD2, per step (S5, S11, S12) |
| DIO5, DIO6 | PE13, PE14 | TIM1\_CH3, \_CH4 (AF1) | T6 | MCU, in a "bus = MCU" step |
| DIO0, 2, 4, 7 | PE8, PE10, PE12, PE15 | none (never AF) | T0a, T0b(c), S5 | bus |
| DIO8, DIO10 | PE3, PE4 | none (PE4 in AF4 would be TIM15\_CH1N: never) | T0a | AD2 only (not driven) |
| DIO9, DIO11 | PE5, PE6 | TIM15\_CH1, \_CH2 (AF4, CPU2) | T6 (TIM15), T0a, T0b(b) | MCU only |
| DIO12 / DIO13 | PE2 / PE7 | `HOST_REQ` / `MCU_ACK` | every handshake | AD2 / MCU |
| DIO14 | PA8 | MCO1 (AF0); TIM1\_CH1 (AF1) only in the TREF\_A control | TREF\_A/B, T6, T8, T9 | MCU only |
| - | PF6, PF7, PF8, PF9 (unwired, pull-down) | TIM16, TIM17, TIM13, TIM14 CH1 | T7; PF7 is also the T9 control | - |

**Electrical safety.** No series resistors: a contention can damage a pin silently (DS12923 table 11).
S1–S10 of `00_gpio/README.md` §8 apply as they are (one driver per line, S2 safe state, host and firmware
guards, break-before-make S5, announced steps with a 10 s timeout). This driver adds:

| # | Rule | Where |
| :--- | :--- | :--- |
| S11 | Digital Out drives a line even with Digital-IO `OE = 0` (SDK §8). The only `FDwfDigitalOutEnableSet` is in `ad2_set_dout(mask, step_bus_owner)`, which accepts DIO1/DIO3 in a "bus = AD2" step only. S2 also resets Digital Out (`Configure(0)`, `EnableSet(0)`, `Idle = Zet` on 16 channels, read back). An enabled channel gets `Idle = Zet` and push-pull before `Configure(1)`; `Idle = Low` only in the T0b(f) control | `tools/ad2.py`; T0b(e)(f) |
| S12 | PE9/PE11 in AF while the AD2 drives: `bench_claim()` accepts them only in AF1, after reading TIM1 back as inputs (`CC1S`/`CC2S ≠ 00`), `CCxNE = 0`, `CC3E = CC4E = 0`, `MOE = OSSI = OSSR = 0`. The S5 test runs with the pins already in AF | `CM7/App/Src/tim_selftest.c`; T0b(g) |

`bench_claim()` exists on both cores (CPU2 configures PE5/PE6) and always refuses AF on PE0–PE4, PE7,
PE8, PE10, PE12 and PE15. Never touched: PE0, PE1 (LD2), PC13, TIM6, SysTick, LPTIM, HRTIM, `RCC_D2CFGR`,
`TIMPRE`, `RCC_APBxRSTR`, `DBGMCU`. `RCC_CFGR` (`MCO1`/`MCO1PRE`) is written only in the TREF, T6, T8 and
T9 steps, saved before and restored with read-back. **Procedure:** GND first, then the DIO lines; flash
with the script stopped; run the host (§9.3), which resets the board with the AD2 in S2; stop it before
reflashing or unwiring.

---

## 9. Validation

`prj_01_template_dualcore`, preset `selftest-tim` (`TIM_DRIVER_SELFTEST=ON`):
`CM7/App/Src/tim_selftest.c`, `CM4/App/Src/tim_selftest.c`, `Common/Src/tim_selftest_common.c` (block at
`0x3800B000`, SRAM4) and the host `tools/tim_hw_check.py` (protocol v5). CPU1 prints the global verdict,
CPU2's included. Every test prints its negative control; no oracle is the driver (register snapshots,
RM0399 fields typed by hand, the raw HAL, `CYCCNT`, `IDR`, the AD2, the host's arithmetic). With the
option off, production keeps the sections of `2481396` (`docs/tim_f3_map_sizes_1680350.txt`).

### 9.1 Captures

All in `03_firmware_templates/prj_01_template_dualcore/docs/`, one unedited run each, clean `Rev` and
build id equal on both cores and the host.

| Phase | Auditor's run | Delivered run |
| :--- | :--- | :--- |
| F0 bench | `tim_f0_audit_bench_cae5414.txt`: 16/16, PASS | `tim_f0_bench_7ac9e44.txt`: 16/16, PASS |
| F1 `tim_bm` | `tim_f1_audit_bench_3174fef.txt`: 109/109, PASS | `tim_f1_bench_b65fd03.txt`: 109/109, PASS |
| F2 `tim_ll` | `tim_f2_audit_bench_071bddb.txt`: 192/192, PASS | `tim_f2_bench_b63204a.txt`: 192/192, PASS |
| F3 `tim_hal` | `tim_f3_audit_bench_70b2062.txt`: 259/259, PASS | `tim_f3_bench_1680350.txt`: 259/259, PASS |

Failed runs kept: `tim_f1_audit_bench_70e0181.txt` (108/109: T8 at 1 MHz, 202 against 200.6 ± 1.2 ticks,
then R6-1) and `tim_f3_audit_bench_3b5dc5e.txt` (`FW VERDICT=FAIL`: 671 empty ISR calls in T12 LL(a),
test finding m5, §10.4). The F0 runs of the TREF criteria are `tim_f0_bench_*`.

### 9.2 Tests

Values from `tim_f3_audit_bench_70b2062.txt`; "BM/LL/HAL" are the tiers the test runs through.

| Test | Tiers, cores | What | Result | Negative control |
| :--- | :--- | :--- | :--- | :--- |
| T0a | bench | 13 lines loose and walking 1 | 0x4FFF / 0x0000, 13 patterns, 0 mismatches | DIO9 pulled the wrong way: flagged 0x0200 |
| T0b(a)–(g) | bench | host guards on a mock DLL, firmware claims, S5, timeout, Digital Out release, S12 | all PASS | `Idle = Low` holds DIO1 low: the S5 check aborts; S12 wrong TIM1 states refused |
| TREF | bench | MCO1 = HSE/8 against the host clock (R4-1) | ē = +13 273.1 ppm, x = +13 110.4 ± 6.2, \|ē − x\| = 162.7 ≤ 513.7 | PA8 in AF1 with TIM1 unclocked: 0 edges |
| T1 | BM CM7/CM4, LL CM7/CM4, HAL CM7 | refusals, with the registers of 6 timers, both RCC banks (and the HAL handle) read before and after | 39 + 10 (BM), 47 + 12 (LL), 42 (HAL): right status, nothing touched | a valid call changes exactly CCMR1 and CCR1 |
| T2 | BM CM7/CM4, LL CM7/CM4, HAL CM7 | register image of 6008 configurations on 12 instances (1313 on TIM15) against fields typed from RM0399; LL and HAL also bit for bit against BM | 0 mismatches; parity 6008/6008 (LL, HAL) and 1313/1313 (LL); `GetCaps` = hand table on 14 instances; HAL by-design cases 15/15 (§10.3) | an altered expected image, and an altered BM image, are flagged |
| T3 | BM | `CalcTimeBase` (234 vectors), `DutyToCompare` (70), table 59 (34) recomputed by the host with `fractions` | 0 differ | an altered expected vector is flagged |
| T5 | BM, LL, HAL CM7; BM, LL CM4 | time base against `CYCCNT`, ≥ 100 ms, up/RCR/32-bit/down/center | worst \|e\| 3 (BM), 2 (LL), 4 (HAL) ppm, ≤ 100; `GetKernelClockHz` = 200 MHz on 14 instances | PSC + 1 / no RCR / 16 bits / edge-aligned: ≥ 1993 ppm off |
| T6 | BM, LL, HAL TIM1; BM, LL TIM15 | PWM at 1 MHz and 100 kHz, CCR = 1 tick, 0/25/50/100 %, active low, PWM2, center | 36/36 per tier on TIM1, 16/16 on TIM15; worst f/f\_MCO1 error 444 ppm (± 1000), worst high time 10.6 ns (± 15) | `MOE = 0` (TIM1) / `CCxE = 0` (TIM15): 0 edges, level 0 |
| T7 | BM | PWM 1 kHz 25 % of TIM13/14/16/17 on unwired pins, `IDR` against `CYCCNT` | period ≤ 2 ppm off, duty 24.99–25.00 % | `CC1E = 0`: 0 edges |
| T8 | BM, LL, HAL | PWM input on TI1/TI2 at 1 k–1 MHz, 25/50/75 %; ICPSC 8; filter; overcapture | 24/24 per tier; reset delay d = 2.06 / 2.00 / 1.94 ticks; worst residual 0.39 of its bound (1 MHz) | inverted polarity gives the complementary duty; `CC1E = 0`: no CC1IF; no filter: the 10 ns glitch is captured |
| T9 | BM, HAL | TIM17 `TI1SEL = 0011` (MCO1) | 16 periods of 199..201 ticks (200 ± 1) | `TI1SEL = 0000`: no capture |
| T10 | BM; (a)(b) LL | ext. clock 1, gated, trigger, TRC capture on 7 instances, TIM13/14 → TIM12 | CNT 1000 exactly; 20 000 = N·CCR; TIM8 starts on TIM1's update; differences 1000 ± 1 | the slave on an ITR whose master is stopped: 0 |
| T11 | BM | one-pulse on PE9, 10 software triggers | 10 pulses, D and W within ± 14.9 ns | `OPM = 0`: 60 pulses |
| T12 | BM CM7/CM4, LL CM7/CM4 | 10 kHz on every test vector, each source alone and the TIM8/12/13/14 vectors together, 100 ms; no spurious IRQ; `GetIRQn` | 1000 per source, no crossings, empty calls as §10.4; 0 spurious; 84 pairs, 0 differ | `UIE = 0`: 0 IRQ; init by hand with `URS = 0`: 1 IRQ |
| T13 | BM | (a) DMA → CCR1 sequence; (b) 64 captures by DMA; (c) `GetDmaRequest` | (a) the 16 high times in sequence ± 15 ns; (b) 202..203 against 202.52 ± 2.01; (c) 70 pairs, 0 differ | `UDE = 0`: all periods equal; `CC1DE = 0`: buffer intact |
| T14 | all | `Deinit` against the RM0399 reset image after every test; S5 after each pin step | 206 checks on CM7, 0 failed; TIM15 after each CM4 test | a timer left configured is flagged |
| T15 | BM | 10⁶ `Enable/DisableIrq` against a 50 kHz ISR | 0 lost updates against 47 390 ISRs | RMW by hand without PRIMASK: 9483 lost |
| T16 | BM CM4/CM7 | RCC banks: `DisableClock` from one core | TIM2 keeps counting while the other bank enables it | CM7 disables last: TIM2 stops |
| TIM6 | HAL | `HAL_GetTick` against 100 ms of `CYCCNT` before, during and after the HAL tests | 100 ms in 100 ms, 5/5; TIM6 `CR1/CR2/DIER/PSC/ARR` unchanged | - |

### 9.3 Reproducing it

```powershell
.\tools\env.ps1
cd 03_firmware_templates\prj_01_template_dualcore
cmake --preset selftest-tim; cmake --build --preset selftest-tim
py tools/flash.py --preset selftest-tim                      # with the host script stopped
py tools/tim_hw_check.py --checklist-confirmed <name>       # writes docs/tim_f3_bench_<rev>.txt
py tools/tim_hw_check.py --allow-dirty --checklist-confirmed <name>   # development: tim_dev_f3_bench_<rev>.txt
cd ..\..; .\tools\build_all.ps1 -Clean                      # 8/8
```

The host refuses an image whose `Rev` or build id is not this checkout's (`00_gpio/README.md` §9.4), and
a running WaveForms or debugger; `--checklist-confirmed` records the wiring and flashing check.

### 9.4 Measurements (B1)

Cycles of one cold call, DWT with PRIMASK, TIM2 on CM7 (400 MHz), TIM15 on CM4 (200 MHz), in
`tim_f3_audit_bench_70b2062.txt`:

| Function | BM CM7 | LL CM7 | HAL CM7 | BM CM4 | LL CM4 |
| :--- | ---: | ---: | ---: | ---: | ---: |
| `TakeFlags` | 179 | 207 | (BM's) | 280 | 305 |
| `OutputEnable` (HAL: `StartPwm`, with CEN) | 357 | 642 | 907 | 526 | 1120 |
| `ConfigOutput` | 478 | 884 | 1048 | 568 | 1174 |
| `InitTimeBase` (HAL: second call, `State` READY) | 441 | 973 | 645 | 546 | 1142 |

`CalcTimeBase`, worst case over the T3 sweep: 1061 cycles on CM7.

### 9.5 Hardware validation in consumers (F4) and unexercised paths

**Validated on hardware in consumers (phase F4):**
- **TIM3 TRGO → ADC triggering at 500 kSPS:** Validated on hardware in `prj_04_adc_dma_bm_eth` (`tim_bm`) and `prj_03_adc_dma_eth` (`tim_hal` + `tim_bm`). TIM3 update event triggers ADC1 regular conversions; verified bit-by-bit register match against RM0399 §41 (`CR1=0x1`, `CR2=0x20`, `SMCR=0`, `DIER=0`, `PSC=0`, `ARR=0x18F`), sampling rate 500 kHz nominal (~506 kHz measured on this board's fast HSE) within 0.1% equivalence against control `04fb865`, oracle `dSamples / dm4_hb` within ±0.5% (50,000 ± 250), zero errors/overruns, and 20/20 warm resets over SWD.
- **`tim_hal` on CM4:** Validated on hardware in `prj_03_adc_dma_eth` (`CM4/App/Src/adc_acquisition.c`) via `TIM_HAL_InitTimeBase` and `TIM_HAL_ConfigMaster`, with zero `HAL_TIM_*` calls remaining outside `tim_hal.c`.
- **TIM3 and TIM7 in selftests:** Validated on hardware in `prj_04` (`adc_selftest_hw.c` TIM3 and `dma_selftest.c` TIM7 update DMA request) and `prj_03` (`adc_selftest_hw.c` TIM3) using `tim_bm`.

**Evidence captures (auditor independent runs):**
- `02_firmware_projects/prj_03_adc_dma_eth/docs/` and `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/`:
  - `tim_f4_audit3_t3_1caa7c3.txt` (and `_t3_control_`): Live SWD hotplug inspection (T3, 32-bit registers).
  - `tim_f4_audit3_t5_debug_1caa7c3.txt`, `_t5_release_`: Production regression and network counters (T5).
  - `tim_f4_audit3_t8_1caa7c3.txt`: Hardware selftests and control equivalence (T8).
  - `tim_f4_audit4_t4_1893222.txt`: ADC sampling rate, oracle, equivalence vs control, and negative controls (T4).
  - `tim_f4_audit4_t6a_1893222.txt`: 20 repeated warm resets over SWD with edge-aligned telemetry (T6a).

**Remaining not exercised:**
- **TRGO → DAC, TRGO2, channels 5–6:** not in `prj_01`, not in the API.
- **Capture from a pin** on TIM2–5, TIM8, TIM12, TIM15 (only TRC, T10(d)); TIM13, TIM14, TIM16 capture nothing.
- **PWM on a pin** of TIM2–5, TIM8, TIM12 (no pin on the bench). `OCxREF` by TRGO + gated only for TIM2 and TIM13/14; TIM3, TIM4, TIM5, TIM8 have only their register image (T2).
- **Any clock tree, board or silicon other than `prj_01`'s, `prj_03`'s and `prj_04`'s on Rev V (except T3).**
- **LL:** `EnableDmaRequest`, `GenerateEvent`, `SetPeriod`, `SetCompare` only by their refusals; T7, T9, T11, T13, T15, T16 not through LL. **HAL:** `ConfigMaster` and `StopPwm` only in T1/T2 and a by-design case; T3, T7, T10–T13, T15, T16 not through HAL; by-design row 9 from reading only.
- **Never produced or provoked:** `ERR_VERIFY` in any tier; TIM6 with a writing function (`prj_01`'s tick); ES0445 2.16.2–2.16.4.

---

## 10. Known limitations

### 10.1 `TIM_HAL_InitTimeBase` and C10 (R8-2)

`HAL_TIM_Base_Init` and `HAL_TIM_OnePulse_Init` read-modify-write `CR1` without PRIMASK: they may call
the project's MSP, which would never return if it waited on the tick with interrupts masked. So
`TIM_HAL_InitTimeBase` does not protect `CR1` against an ISR writing the same timer. Every other HAL call
that RMWs `CCER`, `DIER`, `BDTR`, `SMCR`, `CR2` or `TISEL` runs under PRIMASK.

### 10.2 `TIM_HAL_ConfigSlave` with a TI trigger (O4)

With `TIM_BM_TRG_TI1FP1`, `TI1F_ED` or `TI2FP2`, `HAL_TIM_SlaveConfigSynchro` rewrites the input stage
(`TIM_TI1_ConfigInputStage`, `stm32h7xx_hal_tim.c` l. 7586, and its TI2 pair) with `CC1E`/`CC1NE` (or
`CC2E`) at 0, then restores `CCER`: a capture in progress can lose an edge. **Call it with the capture
disabled.** The stage keeps its values (row 12); `tim_bm` does not touch `CCER` there.

### 10.3 `tim_hal` against `tim_bm`, by design

From the reset image `tim_hal` leaves the registers as `tim_bm` (T2: 6008/6008); from another image the
HAL leaves or clears some bits. Lines of `Src/stm32h7xx_hal_tim.c` (row 11: `_tim_ex.c`), Cube `8ec3113`,
HAL `e3c518e` (V1.11.6); last column: the T2 case that shows it (bits that differ):

| # | Function | Bits | The HAL | `tim_bm` | T2 case |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 1 | `InitTimeBase` | `CR1` UDIS, UIFREMAP | `TIM_Base_SetConfig` starts from `CR1` (6937), changes DIR/CMS (6943), CKD, ARPE, writes it (6977): keeps them. With UDIS = 1 the UG loads neither PSC nor ARR | writes 0 | 0: `CR1` `0x0802` |
| 2 | `InitTimeBase` | `SR` except UIF | does not touch `SR`; `tim_hal` clears UIF (C11) | `SR = 0` | 1: `SR` `0x0002` |
| 3 | `InitTimeBase` | `CR1` CKD on TIM12–14 | writes CKD only if `IS_TIM_CLOCK_DIVISION_INSTANCE` (6947), which leaves out TIM12–14 although they have CKD (RM0399 42.4.1, 42.5.1) | writes 0 | 2: `CR1` `0x0200` |
| 4 | `InitTimeBase` | DIR and CMS | one write for both (6943, 6977); `tim_hal` runs a first `HAL_TIM_Base_Init` in up mode when `CMS ≠ 00` | `CMS = 00` first | 3: none |
| 5 | `ConfigOutput` | OCxCE; OCxFE in non-PWM modes | `TIM_OC1_SetConfig` clears only OC1M and CC1S (7005–7006); the PWM call clears OCxFE (4297) | whole field | 4: `CCMR1` `0x0084`; 5: `0x0080` |
| 6 | `ConfigOutput` | CCxNP on channels without a complementary output | written only if `IS_TIM_CCXN_INSTANCE` (7015, 7024) | `CCxNP = 0` | 6 (TIM2): `CCER` `0x0008`; 7 (TIM1 CH1): none |
| 7 | `ConfigOutput` | `CR2` OISx/OISxN on TIM1/8/15–17 | set to 0 (7036) | `CR2` untouched | 8: `CR2` `0x0100` |
| 8 | `ConfigOutput`, `ConfigInput` | CCxNE of complementary channels | cleared (7021, 7544) | untouched; never set by this driver | not reachable |
| 9 | `ConfigInput` | OCxM[3] | `TIM_TI1_SetConfig` changes CC1S and IC1F only (7553, 7562) | whole field | not exercised |
| 10 | `ConfigSlave` | `DIER` TIE, TDE | `HAL_TIM_SlaveConfigSynchro` clears them (5655, 5658) | `DIER` untouched | 10: `DIER` `0x4040` |
| 11 | `ConfigMaster` | `CR2` MMS2 on TIM1/8 | `MasterOutputTrigger2 = TIM_TRGO2_RESET` (`_tim_ex.c` 2018) | MMS2 untouched | 9: `CR2` `0x00100000` |
| 12 | `ConfigSlave` | TI1/TI2 input stage | rewritten with the trigger polarity/filter (7386); `tim_hal` passes the current values | untouched | 11: none |
| 13 | `ConfigSlave` | TS and SMS | one write (7398–7408); `tim_hal` makes one call per step SMS = 0, TS, SMS (40.4.3) | three writes | sequence only |
| 14 | `StartCapture` | `CR1` CEN | `HAL_TIM_IC_Start` sets it except in trigger modes (2158, 2163) | `CaptureEnable` does not | 12: `CR1` `0x0001` |
| 15 | `StartPwm` | `CR1` CEN | `HAL_TIM_PWM_Start` sets it except in trigger modes (1494, 1499) | `OutputEnable` does not | 13: `CR1` `0x0001` |
| 16 | `StopPwm` | `CR1` CEN | `__HAL_TIM_DISABLE` once no CCxE/CCxNE is left (1534) | `OutputDisable` does not | 14: `CR1` `0x0001` |

The read-back covers what each HAL call writes: rows 1, 2 (but UIF), 3, 5, 6, 9 never give `ERR_VERIFY`.

### 10.4 Shared vectors

TIM8 shares vectors with TIM12, TIM13 and TIM14. When one ISR call takes two flags, the second has
already made the vector pending again, and the next entry finds no flag: a legitimate empty call (m5).
TIM8 and TIM13 at 10 kHz, empty / two-flag calls in the final run: `0/1000` (BM), `396/1000` (LL); `0/0`
on every single-source vector. An ISR on a shared vector must accept an empty call.

### 10.5 Other limits

- **`InitTimeBase` clears every flag:** `TIM_BM_`/`TIM_LL_InitTimeBase` end with `SR = 0`, so a pending
  CCxIF or TIF from before is lost too. `TIM_HAL_InitTimeBase` clears only UIF (row 2).
- **PWM input reads short:** the slave reset comes late by the TI1 resynchronisation (RM0399 40.3.26, no
  value given): d ≈ 2 ticks here (T8: 1.94–2.06) on the period and the high time, not on a direct
  capture. The first capture after `Start` can be short too: the selftest discards two.
- **The board clock (N1, open, outside the driver):** the HSE comes from the ST-LINK MCO and runs about
  +1.3 % fast (TREF ē = +13 273 ppm in the final run), drifting up to about 1700 ppm between windows
  milliseconds apart; CM7 is then near 405 MHz, above VOS1's 400 MHz. The tests do not depend on it: the
  AD2 measures every frequency as a ratio with MCO1 = HSE/8 in the same acquisition, the timers running
  at exactly 200·f\_MCO1 (R3-2), and the captures judged against MCO1 from another window (T8, T13(b))
  use a 0.5 % relative term (R7-1).

---

## 11. Consumers

- `03_firmware_templates/prj_01_template_dualcore`: the `selftest-tim` preset only (`TIM_DRIVER_SELFTEST=ON`); production links no `03_tim` tier.
- `02_firmware_projects/prj_04_adc_dma_bm_eth`: links `stm32h7_driver_tim_bm` in CM4 (`tim_bm` tier):
  - **TIM3**: in acquisition (`CM4/App/Src/adc_acquisition.c`) for clock enable, time base initialization (PSC=0, ARR=399), master TRGO update event configuration, and late counter start with `TIM_BM_Start(TIM3)`; in selftest ADC (`CM4/App/Src/adc_selftest_hw.c`) via `TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start`, and `TIM_BM_Stop`.
  - **TIM7**: in selftest DMA (`CM4/App/Src/dma_selftest.c`) for clock enable, time base, update DMA request (`EnableDmaRequest`), start, stop, and deinitialization.
- `02_firmware_projects/prj_03_adc_dma_eth`: links `stm32h7_driver_tim_hal` in CM4 (`tim_hal` tier + `tim_bm`):
  - **TIM3**: in acquisition (`CM4/App/Src/adc_acquisition.c`) using `TIM_HAL_InitTimeBase` and `TIM_HAL_ConfigMaster` for time base and TRGO setup, and `TIM_BM_Start(TIM3)` for late start; in selftest ADC (`CM4/App/Src/adc_selftest_hw.c`) using `tim_bm` (`TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start`, `TIM_BM_Stop`). No `HAL_TIM_*` calls remain outside `tim_hal.c`.

---

## 12. Changing this driver

1. **Frozen API, additions only** (catalog rule 2): `tim_bm.h` and `tim_ll.h` as at `071bddb`,
   `tim_hal.h` as at `70b2062`; check with `git diff main -- '01_shared_drivers/03_tim/*.h'`.
2. **One copy of every check** (`TIM_BM_Check*`, called by LL/HAL before writing); a silicon fact goes
   into `k_tim[]` with its RM0399 line and, typed separately, into `tst_hand_caps()`.
3. **Dependencies (§1) and no state in RAM** stay as they are; every test prints its negative control,
   and its oracle is not the driver.
4. **Before merging:** `build_all -Clean` 8/8, `selftest-tim` with 0 warnings and with
   `CMAKE_C_FLAGS=-Werror`, production `.map` unchanged, one clean unedited bench run (the image's
   first).

---

## 13. History

| Milestone | Commits |
| :--- | :--- |
| Plan and implementer instructions (revision 1) | `21c0683` |
| F0: S11 in `ad2.py`, option and preset, bench selftest, host; plan revisions 2–4 (TREF); `prj_01` SMPS fix | `eb5e440` … `7ac9e44`, `2481396`; F0 passes at `708c212` (auditor's run `cae5414`) |
| F1: `tim_bm`, target, tests, host; R5 (reset delay, `TIM_BM_CH`), R6-1 | `1f0fedc` … `b65fd03`; F1 passes at `d1bdcca` (auditor's run `3174fef`) |
| F2: `TIM_BM_Check*`, `tim_ll`, target, tests; R7-1 | `916a3d4` … `b63204a`; F2 passes at `8e968c1` (auditor's run `071bddb`) |
| F3: `tim_hal`, target, tests; R8; m5 | `f29d578` … `1680350`; F3 passes at `27ef8cf` (auditor's run `70b2062`) |
| Development captures (`tim_dev_f1_*`, `tim_dev_f2_*`, `tim_dev_f3_*`) removed from `docs/` | `e778ba7` |
| F4: Consumer migration (`prj_04` TIM3/7 to `tim_bm`, `prj_03` TIM3 to `tim_hal`/`tim_bm`), verification tool `tim_f4_checks.py`, plan revisions 2–5 | `648d262` … `1893222`; F4 passes at `063603c` (auditor's runs `tim_f4_audit3_*_1caa7c3.txt` and `tim_f4_audit4_*_1893222.txt`) |
| This README | the commit that adds it |

Process documents at the parent of the commit that removes each (the catalog owner removes them):

```bash
# F3 process documents (removed at commit 8901b77):
git show 8901b77^:01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md   # revision 8 (F3)
git show 8901b77^:01_shared_drivers/03_tim/IMPLEMENTER_INSTRUCTIONS.md
git show 8901b77^:01_shared_drivers/03_tim/IMPLEMENTATION_REPORT.md
git show 8901b77^:01_shared_drivers/03_tim/AUDIT.md
git show e778ba7^:03_firmware_templates/prj_01_template_dualcore/docs/tim_dev_f2_series_2d1e525_1.txt   # and the other 14

# F4 process documents (removed upon closing F4):
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md)^:01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md   # revision 5 (F4)
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/03_tim/IMPLEMENTER_INSTRUCTIONS.md)^:01_shared_drivers/03_tim/IMPLEMENTER_INSTRUCTIONS.md
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/03_tim/IMPLEMENTATION_REPORT.md)^:01_shared_drivers/03_tim/IMPLEMENTATION_REPORT.md
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/03_tim/AUDIT.md)^:01_shared_drivers/03_tim/AUDIT.md
```
