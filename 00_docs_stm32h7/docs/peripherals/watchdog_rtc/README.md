# RTC & Watchdogs (IWDG, WWDG) - STM32H755

## 1. Documentation
- `46_watchdog-overview.pdf`
- `47_system-window-watchdog-wwdg.pdf`
- `48_independent-watchdog-iwdg.pdf`
- `49_real-time-clock-rtc.pdf`

---

## 2. Dual-Core Instances

This device has **two of each watchdog**, one per core. They are independent
peripherals, not a shared one with two views.

| | CPU1 (M7) | CPU2 (M4) | Clock source |
| :--- | :--- | :--- | :--- |
| Independent watchdog | `IWDG1` | `IWDG2` | dedicated **LSI** (~32 kHz) |
| Window watchdog | `WWDG1` | `WWDG2` | APB3 / APB1 |

- **IWDG** runs from LSI, so it survives a stalled PLL or a frozen system clock. Once
  started it **cannot be stopped** except by reset. Use it as the last line of defence.
- **WWDG** runs from the bus clock and enforces both a maximum *and* a minimum refresh
  interval, catching code that runs too fast as well as too slow.

Each core refreshes its own watchdogs. A hung M4 will not be rescued by the M7's IWDG.

---

## 3. WWDG - Mandatory Errata Workaround (CRITICAL)

### ES0445 2.3.1 - AXI domain locked when the watchdog reset is scoped to one CPU (`A`)

If a window watchdog is configured to reset **only its own CPU**, the AXI domain locks
up and the device becomes unrecoverable without a power cycle. This is the default
scope, so the workaround must be applied explicitly.

**Workaround**: make the watchdog reset the **whole system** via `RCC_GCR`.

```c
/* ES0445 2.3.1 - apply BEFORE enabling the window watchdog */
RCC->GCR |= RCC_GCR_WW1RSC;   /* WWDG1 (CPU1 / M7) resets the full system */
RCC->GCR |= RCC_GCR_WW2RSC;   /* WWDG2 (CPU2 / M4) resets the full system */
```

Set only the bit for the watchdog you actually enable. In a dual-core design where both
cores run a WWDG, set both.

### ES0445 2.3.7 - WWDG not functional below VDD 2.7 V with VOS0/VOS1 (`N`)

The WWDG may fail to generate a reset **and** fail to set its reset flag when
`VDD < 2.7 V` while VOS0 or VOS1 voltage scaling is selected. **No workaround.**

- Nucleo-H755ZI-Q runs at 3.3 V, so it is unaffected.
- For a battery or low-voltage design: do not rely on WWDG for safety. Use IWDG, which
  is not subject to this limitation.

---

## 4. IWDG Configuration

```c
IWDG_HandleTypeDef hiwdg1;

hiwdg1.Instance       = IWDG1;
hiwdg1.Init.Prescaler = IWDG_PRESCALER_64;   /* 32 kHz / 64 = 500 Hz -> 2 ms tick */
hiwdg1.Init.Reload    = 2000;                /* 2000 * 2 ms = 4 s timeout          */
hiwdg1.Init.Window    = IWDG_WINDOW_DISABLE;
HAL_IWDG_Init(&hiwdg1);                      /* starts it - irreversible           */

/* In the main loop or a dedicated low-priority task */
HAL_IWDG_Refresh(&hiwdg1);
```

Notes:
- LSI is only accurate to roughly ±50 %, so size the timeout with generous margin.
- **ERRATA ES0445 2.3.4**: LSI can be unstable when it also clocks the RTC or when the
  LSE CSS is enabled. This does not stop the IWDG, but it is another reason to clock the
  RTC from LSE rather than LSI.
- Under FreeRTOS, refresh from a **single** low-priority task, not from several places.
  A watchdog refreshed by an interrupt that keeps firing while the scheduler is dead
  defeats the purpose.

### Debug behaviour

Freeze the watchdogs while halted at a breakpoint, or every debug session ends in a
reset:

```c
__HAL_DBGMCU_FREEZE_IWDG1();
__HAL_DBGMCU_FREEZE_WWDG1();
```

---

## 5. Reset Cause Analysis

After any reset, read the flags before clearing them - this is the fastest way to tell a
watchdog reset from a brownout or a pin reset.

```c
if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDG1RST)) { /* IWDG1 fired  */ }
if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDG1RST)) { /* WWDG1 fired  */ }
if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))   { /* brownout     */ }
if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))   { /* NRST pin     */ }
if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))   { /* software     */ }
__HAL_RCC_CLEAR_RESET_FLAGS();
```

Log the cause into **Backup SRAM** (`0x38800000`, 4 KB, VBAT-backed) so it survives the
reset and is readable on the next boot.

---

## 6. RTC

### Domain and power

RTC, the backup registers and Backup SRAM (`0x38800000`, 4 KB) sit in the **backup
domain** and keep running from `VBAT` when `VDD` is removed. Writes require unlocking:

```c
HAL_PWR_EnableBkUpAccess();
__HAL_RCC_RTC_ENABLE();
```

### Clock source

| Source | Accuracy | Notes |
| :--- | :--- | :--- |
| **LSE** 32.768 kHz | best | **recommended** for real timekeeping |
| LSI ~32 kHz | ±50 % | see errata 2.3.4 below |
| HSE / 32 | good | stops with VDD - no VBAT operation |

> **ERRATA ES0445 2.3.4** - LSI is unstable when it clocks the RTC, or when the LSE CSS
> is enabled (`P`).
> **Workaround**: clock the RTC from LSE or HSE/32 **and leave the LSE CSS disabled**.
> If LSI must clock the RTC, or `LSECSSON` is set, reset the backup domain on every VDD
> power-up (when `BORRSTF` is set) and restore the RTC configuration, backup registers
> and anti-tamper settings.

> **ERRATA ES0445 2.3.13** - transitions on **`PC13`** can disturb the LSE crystal
> (`N`). On the Nucleo-H755ZI-Q `PC13` is the **USER button**, so an application that
> polls or interrupts on it heavily while the RTC runs from LSE can lose time. Debounce
> in software and never drive `PC13` as a fast output.

### Initialisation

> **ERRATA ES0445 2.18.2** - calendar initialisation may fail on consecutive INIT mode
> entries (`A`).
> **Workaround**: after exiting initialisation mode, clear `BYPSHAD` (if set), then wait
> for `RSF` to rise before entering initialisation mode again.
> **Best practice: write every RTC register in a single initialisation session.** Do not
> set the time in one call and the date in another.

```c
/* One session: time and date together */
HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
```

Guard re-initialisation with a magic value in a backup register so a warm reset does not
wipe the calendar:

```c
#define RTC_MAGIC   0x32F2U

if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != RTC_MAGIC) {
    /* First power-up: configure the calendar */
    rtc_set_defaults(&hrtc);
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, RTC_MAGIC);
}
/* else: calendar already running on VBAT, leave it alone */
```

### Other RTC errata

| # | Limitation | Practical effect |
| :--- | :--- | :---: |
| 2.18.1 | An RTC interrupt can be masked by another RTC interrupt (`A`) | check every enabled flag in the handler, not just the expected one |
| 2.18.3 | Alarm flag repeatedly set while the core is halted in debug (`N`) | expected at a breakpoint - not a bug in your code |
| 2.18.4 | Tamper fails to trigger timestamp for a few cycles after clearing `TSF` (`N`) | do not clear `TSF` in a tight loop |
| 2.3.8 | A tamper event does not erase Backup RAM when its clock is disabled (`A`) | keep the Backup RAM clock enabled if tamper erase is a security requirement |

---

## 7. Dual-Core Notes

- The RTC is a **shared** peripheral in the backup domain. Both cores can read it, but
  configuration must be done by **one owner only** - normally CPU1 (M7), during the boot
  phase before CPU2 is released.
- Guard any read-modify-write of backup registers with an HSEM. See
  [`../hsem_dualcore/README.md`](../hsem_dualcore/README.md).
- RTC alarm interrupts route through EXTI, which has **per-core masks**
  (`EXTI_C1IMRx` / `EXTI_C2IMRx`). Enable the line on the core that will service it. See
  [`../interrupts_exti/README.md`](../interrupts_exti/README.md).
- `IWDG1`/`WWDG1` belong to CPU1, `IWDG2`/`WWDG2` to CPU2. Do not refresh one core's
  watchdog from the other.

---

## 8. Debug Checklist

| Symptom | Likely cause |
| :--- | :--- |
| Device locks up hard after a watchdog reset, needs power cycle | `RCC_GCR.WW1RSC` / `WW2RSC` not set (errata 2.3.1) |
| Resets during every debug session | `__HAL_DBGMCU_FREEZE_IWDG1()` missing |
| WWDG never resets on a low-voltage board | errata 2.3.7 - use IWDG instead |
| Calendar resets on every boot | missing backup-register magic guard |
| Time drifts noticeably | RTC clocked from LSI (errata 2.3.4) - move to LSE |
| Time loss correlated with button presses | errata 2.3.13 - `PC13` activity disturbing LSE |
| An RTC interrupt is occasionally missed | errata 2.18.1 - handler only checks one flag |

---

## 9. Cross-References
- Errata: [`../../00_core_and_system/errata_summary.md`](../../00_core_and_system/errata_summary.md)
- Clock tree and LSE/LSI: [`../rcc/README.md`](../rcc/README.md)
- Interrupts and EXTI: [`../interrupts_exti/README.md`](../interrupts_exti/README.md)
- Safety manuals: [`../../00_core_and_system/safety/`](../../00_core_and_system/safety/)
- Memory map (Backup SRAM): [`../../../STM32H755_Memory_Map.md`](../../../STM32H755_Memory_Map.md)
