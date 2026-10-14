# STM32H755 Shared LED Driver (`01_shared_drivers/01_led`)

Bare-metal driver for the three user LEDs of the NUCLEO-H755ZI-Q (LD1 green,
LD2 yellow, LD3 red). It exists to say something useful when nothing else can:
before `MPU_Config()` and the caches, before `HAL_Init()` and the clock tree, and
from a fault handler. It uses no HAL and makes no assumption about clocks.

Status: in use by every dual-core project of the repo since their bring-up. It
has no recorded hardware test of its own and no audit. The LEDs are checked by
eye, never in a capture (`prj_02_eth_lwip_dualcore/README.md` §6, "Not covered
here"). Its known limitations, two of which break catalog rules, are in
[section 5](#5-known-limitations).

---

## 1. Tier and files

| Tier | Files | CMake target | Links |
| :--- | :--- | :--- | :--- |
| BM | `led_bm.[ch]` | `stm32h7_driver_led` | `stm32h7_driver_gpio` (only for `GPIO_BM_EnablePortClock`) |

```cmake
target_link_libraries(cm7_firmware PRIVATE stm32h7_driver_led)
```

```c
#include "led_bm.h"    /* the directory is on the include path */
```

There is no LL or HAL tier: an LED is a `BSRR` store, and the HAL is exactly
what this driver must not depend on.

---

## 2. API

| Function | What it does |
| :--- | :--- |
| `LED_Init()` | `LED_Init_Config()` with the table built from `board_def.h` (section 3) |
| `LED_Init_Config(cfg)` | Copies `cfg` into the driver; for each LED with a non-`NULL` port: enables the port clock (`GPIO_BM_EnablePortClock`), sets `MODER` to output, drives the pin low through `BSRR`. `NULL` `cfg` does nothing |
| `LED_Set(led, on)` | One `BSRR` store (atomic). Out-of-range `led` or `NULL` port: nothing |
| `LED_Toggle(led)` | Reads `ODR`, then one `BSRR` store. Not atomic (section 5) |
| `LED_BootStage(stage)` | Shows a 3-bit code: green = bit 0, yellow = bit 1, red = bit 2 |
| `LED_PanicRed()` | Re-enables the red LED's port clock, sets its `MODER` to output and lights it. For fault handlers: no HAL, no interrupts, no wait |

Active high: `on` drives the pin high. `OTYPER`, `OSPEEDR` and `PUPDR` are not
written: the pins keep their reset values (push-pull, low speed, no pull).

### 2.1 Boot codes (`led_boot_stage_t`)

| Code | Green / yellow / red | Meaning |
| :-: | :--- | :--- |
| 1 | on / off / off | `LED_BOOT_LEDS_ALIVE`: GPIO up, nothing else attempted yet |
| 2 | off / on / off | `LED_BOOT_MPU_DONE`: `MPU_Config()` returned |
| 3 | on / on / off | `LED_BOOT_CACHES_DONE`: I-cache and D-cache enabled |
| 4 | off / off / on | `LED_BOOT_HAL_DONE`: `HAL_Init()` returned |
| 5 | on / off / on | `LED_BOOT_CLOCKS_DONE`: `SystemClock_Config()` returned |
| 6 | off / on / on | `LED_BOOT_APP_ENTERED`: `app_main()` reached, console alive |
| 7 | on / on / on | `LED_BOOT_SCHEDULER`: handing over to FreeRTOS |

A board frozen on one code tells where the start-up stopped. Which codes a project
actually shows, and what its LEDs do after the scheduler starts, is in that
project's README.

---

## 3. Project contract

`led_bm.c` includes `board_def.h` if it exists (`__has_include`) and reads:

| Macro | Meaning | Default if missing |
| :--- | :--- | :--- |
| `BOARD_LED_GREEN_PORT`, `BOARD_LED_GREEN_BIT` | LD1 | `GPIOB`, `0U` |
| `BOARD_LED_YELLOW_PORT`, `BOARD_LED_YELLOW_BIT` | LD2 | `GPIOE`, `1U` |
| `BOARD_LED_RED_PORT`, `BOARD_LED_RED_BIT` | LD3 | `GPIOB`, `14U` |

**`_BIT` is a bit number (0-15), not a `GPIO_PIN_x` mask.** The driver shifts it
directly into `MODER` and `BSRR`. A macro with another name (for example
`BOARD_LED_GREEN_PIN GPIO_PIN_0`) is not read: the default is used instead,
silently (section 5, L1).

A project that wants other pins without `board_def.h` passes its own table to
`LED_Init_Config()`.

### 3.1 Where it may be called on a dual-core board

`LED_Init_Config()` and `LED_PanicRed()` write `MODER` with a plain
read-modify-write, outside the `gpio_bm` section and its cross-core lock
(`00_gpio/README.md` §3.3). That is safe only while the other core cannot be
configuring pins of the same port:

- **`LED_Init()` before CPU1 releases CPU2.** All projects of the repo do this:
  safe by contract.
- **`LED_PanicRed()` from a fault handler.** Accepted exception G14 of `00_gpio`:
  the fault path is terminal and cannot wait for a semaphore.
- `LED_Set()`, `LED_BootStage()` and `LED_Toggle()` write only `BSRR` and may be
  called from either core at any time.

---

## 4. Consumers

| Consumer | Uses |
| :--- | :--- |
| `prj_01_template_dualcore` | `LED_Init`, `LED_BootStage`, `LED_PanicRed`, `LED_Toggle`, all on CPU1; defines the `_BIT` macros |
| `prj_02_eth_lwip_dualcore`, `prj_03_adc_dma_eth`, `prj_04_adc_dma_bm_eth` | The same plus `LED_Set`, all on CPU1; their `board_def.h` define `BOARD_LED_*_PIN` masks, which the driver ignores (L1) |
| `prj_eth_lwip_node` | Its own private copy in `Custom_Drivers/01_led/`, not this driver |

---

## 5. Known limitations

| # | Limitation | Consequence |
| :--- | :--- | :--- |
| L1 | **The pin macros of `prj_02`, `prj_03` and `prj_04` are not read.** They define `BOARD_LED_<x>_PIN` (masks); the driver reads `BOARD_LED_<x>_BIT` and falls back to its defaults. Only `prj_01` defines `_BIT` | Today the defaults are the same pins (PB0, PE1, PB14), so nothing shows. Moving an LED in those projects' `board_def.h` would change nothing, with no warning |
| L2 | **Default pins in the driver** (`#ifndef … #define`), against rule 1 of `01_shared_drivers/README.md` | This is what makes L1 silent. `08_eth` had the same pattern (its E1) and replaced it with an `#error` per missing macro |
| L3 | **`MODER` is written outside `gpio_bm`** (section 3.1) | Safe only under the contract of section 3.1 |
| L4 | **`LED_Toggle()` is not atomic** (`ODR` read, then `BSRR`) | Two contexts toggling the same LED can both read the same state |
| L5 | **`pin_num` is not checked** in `LED_Init_Config()` | A value above 15 shifts outside `MODER`'s 32 bits |
| L6 | **No recorded hardware test and no audit** | The LEDs work by observation in every project; nothing captures it |

Closing L1 and L2 means changing the driver (an `#error` per missing `_PORT`/`_BIT`,
as `08_eth` E1) and the three `board_def.h` at the same time, so that no project
stops building. Not done.

---

## 6. History

| Milestone | Commits |
| :--- | :--- |
| README written from the code (no code change) | the commit that adds this file |
