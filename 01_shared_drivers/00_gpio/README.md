# STM32H755 Shared GPIO Helpers (`01_shared_drivers/00_gpio`)

Bare-metal GPIO helpers for the STM32H755ZI dual-core microcontroller: port clock
enable, alternate-function mode and analog mode. `00_gpio` is the bottom of the
driver stack. `01_led`, `04_uart`, `07_adc` and `08_eth` configure their pins
through it, and it works before the clock tree, before the MPU and before
`HAL_Init()`.

Status: this is **not yet a full GPIO driver**. There is one tier (BM) with three
functions. It has no test of its own; it has been exercised in hardware only
through the drivers that use it (see [section 6](#6-validation)). The plan to turn it
into a 3-tier catalogue driver with output, input, lock and EXTI is in
[`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md), and none of it is implemented
yet.

---

## 1. Tier and target

| Tier | Files | CMake target | Links | ST dependency |
| :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal** | `gpio_bm.[c\|h]` | `stm32h7_driver_gpio` | - | CMSIS only (`stm32h7xx.h`) |

```cmake
target_link_libraries(cm4_firmware PRIVATE stm32h7_driver_gpio)
```

```c
#include "gpio_bm.h"    /* never "00_gpio/gpio_bm.h": the directory is on the include path */
```

- **`stm32h7_driver_gpio` is the BM tier.** There is no LL or HAL tier yet, so the
  unsuffixed name has only ever meant `gpio_bm`, the same situation as
  `stm32h7_driver_dma`.
- **Who links it:** `led`, all three `uart` targets, all three `adc` targets,
  `eth`, and the umbrella `stm32h7_shared_drivers`. A project that links any of
  those gets `gpio_bm.c` compiled once.
- **No HAL or LL headers,** which is what lets `01_led` flash boot codes before the
  clock tree exists.

---

## 2. API

| Function | Does | Returns `false` when |
| :--- | :--- | :--- |
| `bool GPIO_BM_EnablePortClock(GPIO_TypeDef *port)` | Sets the port's bit in `RCC_AHB4ENR` and reads it back, so the next access to the port is not dropped. The bit is derived from the port address (ports are 0x400 apart, `GPIOAEN..GPIOKEN` are bits 0..10), guarded by a `_Static_assert` on the stride. | `port` is `NULL` or outside `GPIOA..GPIOK`. Nothing is written. |
| `bool GPIO_BM_ConfigAlternate(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t af, uint32_t pull, uint32_t speed)` | Clocks the port, then writes `AFR`, `OTYPER` = push-pull, `OSPEEDR`, `PUPDR` and **`MODER` = AF last**. The pin therefore never passes through AF0 with the new mode. | `port` is `NULL`, `pin_idx > 15`, `af > 15`, or the port is invalid. |
| `bool GPIO_BM_ConfigAnalog(GPIO_TypeDef *port, uint32_t pin_idx)` | Clocks the port, sets `MODER` = analog, then `PUPDR` = no pull. | `port` is `NULL`, `pin_idx > 15`, or the port is invalid. |

| Constant | Value | Field |
| :--- | :--- | :--- |
| `GPIO_BM_PULL_NONE` / `_UP` / `_DOWN` | 0 / 1 / 2 | `PUPDR` |
| `GPIO_BM_SPEED_LOW` / `_MEDIUM` / `_HIGH` / `_VERY_HIGH` | 0 / 1 / 2 / 3 | `OSPEEDR` |

- **`pin_idx` is a pin index (0..15), not a HAL mask.** `GPIO_PIN_8` (`0x0100`)
  passed as `pin_idx` is rejected. `board_def.h` files hold HAL masks for the HAL
  drivers, so convert them before calling `gpio_bm` (for example with
  `__builtin_ctz(mask)`).
- **`ConfigAlternate` is push-pull only.** There is no open-drain variant, so it
  cannot set up I2C pins.
- **The constants are the register field values.** They are not validated: see §7.

```c
/* USART3 on PD8/PD9, AF7, as uart_bm does it */
if (!GPIO_BM_ConfigAlternate(GPIOD, 8U, 7U, GPIO_BM_PULL_UP, GPIO_BM_SPEED_VERY_HIGH) ||
    !GPIO_BM_ConfigAlternate(GPIOD, 9U, 7U, GPIO_BM_PULL_UP, GPIO_BM_SPEED_VERY_HIGH))
{
    /* bad port or pin */
}

/* ADC input on PB1, as adc_bm does it when analog_pin_port is set */
(void)GPIO_BM_ConfigAnalog(GPIOB, 1U);
```

---

## 3. What is not here

There is no API for any of the following. Consumers that need them write the
registers themselves, or use `HAL_GPIO_Init`:

- output and input configuration, pin read and write (`led_bm` writes `MODER`
  directly);
- open-drain, lock (`LCKR`), de-initialisation, reading a pin's configuration
  back;
- EXTI, although the workspace guidelines describe `00_gpio` as "GPIO & EXTI";
- the I/O compensation cell and the dual-pad analog switches (`SYSCFG_PMCR`).

---

## 4. Dual-core rules

The two kinds of register this code touches behave differently between the
cores.

| Register | Shared between CPU1 and CPU2? | Consequence |
| :--- | :--- | :--- |
| `RCC_AHB4ENR` (port clock) | **No, banked per core** (RM0399 §9.7.1: offset `0xE0`, in the "registers mapped at the same address" bank routed by core ID) | `GPIO_BM_EnablePortClock` is safe from either core at any time. A port stays clocked while either core has its bit set. |
| `GPIOx_MODER`, `OTYPER`, `OSPEEDR`, `PUPDR`, `AFR` | **Yes, one copy** | Every configuration write is an unprotected read-modify-write. If both cores configure pins of the **same port** at the same time, one core can undo the other's change without any error. |

- **Port ownership is the project's job.** Configure a port from one core, or
  make sure the two cores never configure the same port concurrently (for
  example, CPU1 configures everything before it releases CPU2). The driver takes
  no HSEM: it has to work before the HSEM clock exists.
- **Current exposure: `prj_04`.** CPU1 configures PB13 (ETH TXD1) and PB0/PB14
  (LEDs), and CPU2 configures PB1 (ADC), all on GPIOB. They are safe as long as
  those configurations never overlap in time. The planned fix is a critical
  section with an injectable cross-core lock (`IMPLEMENTATION_PLAN.md`, finding
  G1).
- **Pin levels (`BSRR`) are atomic** (RM0399 §12.3.5), so writing output levels
  from two cores is safe even without a lock. There is just no API for it here yet.

---

## 5. Consumers

| Consumer | Uses | How |
| :--- | :--- | :--- |
| `01_led/led_bm.c` | `EnablePortClock` | Then writes `MODER` directly. |
| `04_uart/uart_bm.c`, `uart_ll.c`, `uart_hal.c` (`InitEx`) | `ConfigAlternate` | TX/RX pins, `PULL_UP`, `SPEED_VERY_HIGH`. `uart_hal.c` also uses `EnablePortClock` in its default MSP. |
| `07_adc/adc_bm.c`, `adc_ll.c`, `adc_hal.c` | `ConfigAnalog` | Only when `analog_pin_port != NULL`. |
| `08_eth/eth_hal.c` | `EnablePortClock` | Then `HAL_GPIO_Init` with masks. |
| `prj_04 CM4/App/Src/adc_acquisition.c` | `EnablePortClock` | Project code. |

Any behaviour change in these three functions reaches every one of them. §8 lists
what that means before changing anything.

---

## 6. Validation

There is no GPIO selftest. The three functions have only been exercised in
hardware indirectly, by their consumers on a NUCLEO-H755ZI-Q (Rev V):

| Function | Exercised by | Evidence |
| :--- | :--- | :--- |
| `ConfigAlternate` (AF7, pull-up, very high speed) | UART selftest on PD5/PD6 (`uart_bm`, `uart_ll`) and the BM early-boot console on PD8/PD9. The HAL console muxes its pins with `HAL_GPIO_Init` in its MSP instead | `prj_04 docs/boot_capture.txt` (`[m4_uart_test] … bm_loop=1,1,1 ll=1,1`) |
| `ConfigAnalog` | ADC1 on PB1 at 500 kSPS | `07_adc/README.md` §9 |
| `EnablePortClock` | Ports A, B, C, D, E and G: LED boot codes (PB0, PE1, PB14), the ETH RMII pins, the console, the ADC pin | Boards boot, and ETH and the console run |

**Never exercised:**

- the rejection paths (`NULL`, pin 16, AF 16);
- `PULL_DOWN` and the `LOW`/`MEDIUM`/`HIGH` speeds;
- `ConfigAlternate` on any port other than GPIOD;
- `EnablePortClock` on ports F, H, I, J and K;
- concurrent configuration from both cores.

---

## 7. Known limitations

These come from the audit in `IMPLEMENTATION_PLAN.md` (IDs in brackets).

- **Cross-core race on shared GPIO registers** (G1): see §4. (G2, a comment in
  `gpio_bm.h` that called the `RCC_AHB4ENR` write a race and said nothing about
  the shared GPIO registers, has been corrected.)
- **`pull` and `speed` are masked, not validated** (G3). `pull = 3` programs
  `PUPDR = 11`, which is reserved, and `speed = 4` silently becomes `LOW`.
  `ConfigAlternate` returns `true` in both cases. No current consumer passes
  those values.
- **Misaligned port pointers are accepted** (G4). An address inside
  `[GPIOA, GPIOK]` that is not on the 0x400 stride, such as `GPIOA + 4`, enables
  a real port and returns `true`.
- **Pin types.** Each consumer has its own pin type (`UartBm_Pin_t`,
  `UartLL_Pin_t`, `uart_hal_pin_t`, `led_pin_config_t`, `port + analog_pin_idx`),
  and `board_def.h` uses HAL masks while `gpio_bm` takes indices (G10).
- **Errata that concern pins, not this code:** ES0445 2.3.3 (a GPIO assigned to
  the DAC cannot be used as an output while the DAC drives an on-chip
  peripheral) and 2.3.13 (transitions on PC13 can disturb the LSE oscillator; on
  the Nucleo, PC13 is the user button). See
  `00_docs_stm32h7/docs/00_core_and_system/errata_summary.md`.

---

## 8. Changing this driver

Seven driver targets and every firmware project depend on these three functions.

1. **Signatures and constants are frozen** (`01_shared_drivers/README.md` rule 2):
   `GPIO_BM_EnablePortClock`, `GPIO_BM_ConfigAlternate`, `GPIO_BM_ConfigAnalog`,
   `GPIO_BM_PULL_*` and `GPIO_BM_SPEED_*`. New capability is added as new
   functions. A behaviour change is acceptable only where the old behaviour was
   already wrong (for example, rejecting `pull = 3`), and it must still accept
   every call the consumers of §5 make today.
2. **Stay free of HAL and LL headers**, and of anything that needs the clock tree,
   the MPU or the HSEM: `01_led` depends on it at boot.
3. **Validate before the first write**, and keep the write order that avoids
   glitches: `MODER` last when entering a mode.
4. **No port or pin literals** in the driver, other than facts about the silicon
   (the port stride).
5. **Before merging:** `.\tools\build_all.ps1 -Clean` (8/8 with `-Werror`), plus
   the UART and ADC selftests, which are the only hardware coverage these
   functions have (`prj_04` presets `selftest-uart` and `selftest-adc`).

---

## 9. Roadmap

[`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (2026-09-23) plans the
catalogue driver in phases F0-F6:

- **F0:** `_gpio_bm` target and compatibility alias.
- **F1:** the BM core. Output, input, open-drain, lock, de-init and read-back, a
  cross-core lock hook, and fixes G1, G3 and G4 (G2 is already done).
- **F2:** EXTI lines 0-15 and the compensation cell.
- **F3 and F4:** the LL and HAL tiers.
- **F5:** dual pads (conditional).
- **F6:** documentation closure.

The test plan uses an Analog Discovery 2 on GPIOF. **None of it is implemented.**
This README describes only what exists. Update it phase by phase as the plan is
carried out.

---

## 10. History

| Milestone | Commits |
| :--- | :--- |
| Port-clock helper promoted from the drivers into `00_gpio` | `784febb` |
| Why the RCC clock enables are not a dual-core race, recorded in `gpio_bm.c` | `91622db` |
| `GPIO_BM_ConfigAlternate`, used by the unified UART driver | `dcd9230` |
| `GPIO_BM_ConfigAnalog`, used by the ADC tiers | `be31f3c` |
| Implementation plan for the catalogue driver | `5f97e2e` |
