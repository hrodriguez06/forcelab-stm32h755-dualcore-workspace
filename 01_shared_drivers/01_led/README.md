# STM32H755 Shared LED Driver (`01_shared_drivers/01_led`)

Bare-metal driver for the three user LEDs of the NUCLEO-H755ZI-Q (LD1 green,
LD2 yellow, LD3 red). It exists to say something useful when nothing else can:
before `MPU_Config()` and the caches, before `HAL_Init()` and the clock tree, and
from a fault handler. It uses no HAL and makes no assumption about clocks.

Status: in use by every dual-core project of the repo since their bring-up.
Phase F1 closed limitations L1 and L2 (mandatory compile-time macros via `#error`),
partially closed L5 (compile-time range checks on `board_def.h` macros), and completed
validation across all projects on host (T1–T3) and hardware Nucleo-144 board (T4).

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

`led_bm.c` unconditionally includes `board_def.h` ([`led_bm.c:13`](led_bm.c#L13)) and requires all six macros to be defined:

| Macro | Meaning | Allowed range | If missing / out of range |
| :--- | :--- | :--- | :--- |
| `BOARD_LED_GREEN_PORT`, `BOARD_LED_GREEN_BIT` | LD1 (green) | Port pointer, bit index `0U..15U` | Compile-time `#error` ([`led_bm.c:15-23`](led_bm.c#L15-L23)) |
| `BOARD_LED_YELLOW_PORT`, `BOARD_LED_YELLOW_BIT` | LD2 (yellow) | Port pointer, bit index `0U..15U` | Compile-time `#error` ([`led_bm.c:25-33`](led_bm.c#L25-L33)) |
| `BOARD_LED_RED_PORT`, `BOARD_LED_RED_BIT` | LD3 (red) | Port pointer, bit index `0U..15U` | Compile-time `#error` ([`led_bm.c:35-43`](led_bm.c#L35-L43)) |

There are **no default hardware pins** in `led_bm.c` (Rule 1 of `01_shared_drivers/README.md`: never hardcode hardware pins in shared drivers).

**`_BIT` is a bit number (0..15), not a `GPIO_PIN_x` mask.** The driver shifts it
directly into `MODER`, `BSRR` and `ODR`. Defining a mask (such as `GPIO_PIN_0 = 0x0001U`)
instead of a bit number or specifying a bit index `> 15U` triggers a compile-time `#error`.

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

All dual-core firmware projects define the required `_BIT` macros (PB0 green, PE1 yellow, PB14 red) in their respective `Common/Inc/board_def.h`:

| Consumer | Uses | Board Definition |
| :--- | :--- | :--- |
| `prj_01_template_dualcore` | `LED_Init`, `LED_BootStage`, `LED_PanicRed`, `LED_Toggle`, all on CPU1 | [`prj_01_template_dualcore/.../board_def.h:54-59`](../../03_firmware_templates/prj_01_template_dualcore/Common/Inc/board_def.h#L54-L59) |
| `prj_02_eth_lwip_dualcore` | The same plus `LED_Set`, all on CPU1 | [`prj_02_eth_lwip_dualcore/.../board_def.h:74-79`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/Common/Inc/board_def.h#L74-L79) |
| `prj_03_adc_dma_eth` | The same plus `LED_Set`, all on CPU1 | [`prj_03_adc_dma_eth/.../board_def.h:74-79`](../../02_firmware_projects/prj_03_adc_dma_eth/Common/Inc/board_def.h#L74-L79) |
| `prj_04_adc_dma_bm_eth` | The same plus `LED_Set`, all on CPU1 | [`prj_04_adc_dma_bm_eth/.../board_def.h:74-79`](../../02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/board_def.h#L74-L79) |
| `prj_eth_lwip_node` | Its own private copy in `Custom_Drivers/01_led/`, not this driver | N/A |

---

## 5. Known limitations

| # | Limitation | Consequence |
| :--- | :--- | :--- |
| L3 | **`MODER` is written outside `gpio_bm`** (section 3.1) | Safe only under the contract of section 3.1 |
| L4 | **`LED_Toggle()` is not atomic** (`ODR` read, then `BSRR`) | Two contexts toggling the same LED can both read the same state |
| L5 | **`pin_num` range check is partial** | Closed for `board_def.h` via compile-time checks (`#if (BOARD_LED_<x>_BIT) > 15U` → `#error`). Remains open at runtime for custom tables passed directly to `LED_Init_Config(cfg)` (values > 15 shift outside `MODER`'s 32 bits) |
| L6 | **Hardware verification scope** | Phase F1 completed formal T1–T3 off-board and T4 on-board verification for basic configuration, boot codes and MODER modes. Dynamic blinking under FreeRTOS load is checked by observation in each project |

*Note: Limitations L1 (masks in `board_def.h`) and L2 (default pins in driver) were completely closed in Phase F1.*

---

## 6. Validation

The driver is validated using the dedicated verification tool [`tools/led_l1l2_checks.py`](../../tools/led_l1l2_checks.py).
Verification covers four tiers (T1–T3 off-board and T4 on hardware):

### 6.1 Test Suites

- **T1: Build Matrix & Selftests (`run_t1`):**
  - Compiles all 8 production configurations (`debug` and `release` for the 4 projects) directly via `cmake --fresh` and `cmake --build --clean-first`.
  - Verifies code 0, 0 compiler warnings, `objs > 0`, and confirms that `led_bm.c.obj` was compiled in the log and is present on disk.
  - Verifies the 9 selftest presets linking `led_bm` (`selftest-gpio`, `selftest-tim`, `selftest-spi`, `selftest-i2c` in `prj_01`; `selftest`, `selftest-adc`, `selftest-uart`, `selftest-hsem`, `selftest-gpio` in `prj_04`) with 0 warnings.
  - Negative control: compiles a synthetic C file with an intentional unused-variable warning outside the repository using project flags via `run_cmd` and verifies that [`evaluate_build_output`](../../tools/led_l1l2_checks.py#L293-L315) detects the warning and fails.
  - Image provenance: verified via `FW_GIT_REV` in `fw_build_id.h` (`prj_01`), in the ELF string table (`prj_04`), and via clean repository working tree and in-execution build freshness (`prj_02`, `prj_03`).

- **T2: Preprocessor `#error` and Range Validation (`run_t2`):**
  - Compiles in a clean temporary directory outside the repository so `-fstack-usage` does not leave `.su` files in the source tree.
  - 4 positive controls: unmodified `board_def.h` compiles with 0 warnings and exit code 0.
  - 16 negative mutations (4 per project): missing `_BIT`, missing `_PORT`, `_PIN` mask instead of `_BIT`, and bit `16U > 15U`; all verified to fail with exit code 1 matching the specific `#error` line.

- **T3: Binary Inspection vs UM2408 Oracle & Control (`run_t3`):**
  - Extracts 24 bytes of `g_led_config` from CM7 `led_bm.c.obj` using `arm-none-eabi-objdump -s`.
  - Compares the 6 32-bit words across all 8 current builds against the UM2408 board oracle:
    `['0x58020400', '0x0', '0x58021000', '0x1', '0x58020400', '0xe']` (PB0, PE1, PB14).
  - Compiles control worktree at baseline commit `9dc6896` in-situ with `build_all.ps1 -Clean` (requiring `pass_count >= 8`), verifying that current binaries match the control bit-for-bit.
  - Negative control: mutates an extracted byte and verifies that [`parse_and_verify_led_config`](../../tools/led_l1l2_checks.py#L671-L703) rejects it while validating exact 24-byte section size without truncation.

- **T4: Hardware Board Verification (`run_t4`, Auditor only):**
  - Flashes `debug` images using `tools/flash.py` with `STM32_Programmer_CLI -v`.
  - Non-intrusive SWD reads (`mode=HOTPLUG`, `-r32`, with explicit `sn=`) of `g_led_config` in RAM and GPIO registers (`MODER_B` and `MODER_E`).
  - Verifies `g_led_config` matches the UM2408 table and verifies PB0, PB14, and PE1 are configured as outputs (`01b`).
  - Negative controls: rejects mutated table words and rejects mutated `MODER` (PB0 input `00b`).

### 6.2 Captures Supporting this Driver

| Capture file | Target / Scope | Executed by | Commit recorded | Result |
| :--- | :--- | :--- | :--- | :--- |
| [`docs/led_l1l2_audit_t1t3_736948f.txt`](docs/led_l1l2_audit_t1t3_736948f.txt) | Off-board T1–T3 (Matrix 8/8, Selftest 9/9, T2 20/20, T3 Oracle & 9dc6896) | Auditor (@Claude) | `3c2176c` | `OVERALL VERDICT: PASS` |
| [`docs/led_l1l2_audit_t4_736948f.txt`](docs/led_l1l2_audit_t4_736948f.txt) | Hardware T4 on Nucleo-144 board (hotplug RAM table, MODER outputs, flash `-v`) | Auditor (@Claude) | `3c2176c` | `OVERALL VERDICT: PASS` |
| [`docs/led_l1l2_audit_t1t3_af836ea.txt`](docs/led_l1l2_audit_t1t3_af836ea.txt) | Off-board T1–T3 (Matrix clean-first, 0 warnings, fresh images) | Auditor (@Claude) | `1f63515` | `OVERALL VERDICT: PASS` |
| [`docs/led_l1l2_checks_824d989.txt`](docs/led_l1l2_checks_824d989.txt) | Off-board T1–T3 final closure run (clean tree, MA1 provenance, exact 24 B) | Implementer | `02c7c6c` | `OVERALL VERDICT: PASS` |

*Superseded intermediate implementer captures were pruned from `docs/` to keep only authoritative captures; their history remains accessible in git:*
- `led_l1l2_checks_2e6a15d.txt`: recorded in commit `78aee90` (`git show 78aee90:01_shared_drivers/01_led/docs/led_l1l2_checks_2e6a15d.txt`)
- `led_l1l2_checks_411673b.txt`: recorded in commit `afe8ee8` (`git show afe8ee8:01_shared_drivers/01_led/docs/led_l1l2_checks_411673b.txt`)
- `led_l1l2_checks_b9fbaad.txt`: recorded in commit `2d221e8` (`git show 2d221e8:01_shared_drivers/01_led/docs/led_l1l2_checks_b9fbaad.txt`)

---

## 7. History

| Milestone | Commits |
| :--- | :--- |
| Initial documentation and limitations catalog | `04fb865` |
| Decision approval and plan for mandatory pin macros (L1/L2) | `c926c77`, `901e6b4` |
| Implementation of mandatory `#error` and `_BIT` switch (H1, H2) | `4c15fba` |
| Verification harness `tools/led_l1l2_checks.py` (H3) | `2e6a15d` |
| Initial implementation report and off-board capture (H4) | `78aee90`, `736948f` |
| First audit: product passes on board; findings B1, M1, M2, m1–m9 | `3c2176c` |
| First audit corrections in tool and `board_def.h` comments | `411673b`, `afe8ee8`, `af836ea` |
| Second audit: MA1, MA2, m9 findings | `1f63515` |
| Second audit corrections (warning C compilation, exact 24 B, report) | `b9fbaad`, `2d221e8`, `9546db3` |
| Second audit approval with MA1 condition for close | `c5ce471` |
| Close: MA1 clean tree provenance, `prj_01` comment aligned, final capture | `824d989`, `02c7c6c` |

### Process Documents History

The process documents (`IMPLEMENTATION_PLAN.md`, `IMPLEMENTER_INSTRUCTIONS.md`,
`IMPLEMENTATION_REPORT.md`, `AUDIT.md`) guide the implementation and audit rounds
and are archived in git history upon closure. They can be inspected using:

```bash
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/01_led/IMPLEMENTATION_PLAN.md)^:01_shared_drivers/01_led/IMPLEMENTATION_PLAN.md
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/01_led/IMPLEMENTER_INSTRUCTIONS.md)^:01_shared_drivers/01_led/IMPLEMENTER_INSTRUCTIONS.md
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/01_led/IMPLEMENTATION_REPORT.md)^:01_shared_drivers/01_led/IMPLEMENTATION_REPORT.md
git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/01_led/AUDIT.md)^:01_shared_drivers/01_led/AUDIT.md
```

### Accepted Tool Limitations

What `tools/led_l1l2_checks.py` still does not check, accepted at the close (`AUDIT.md`):
- **Configure output:** only `CMake Error` fails a configure. `CMake Warning` lines are not counted.
- **Image freshness:** an image counts as built in the run by its modification time (no older than the run start minus 5 s). An ELF written by another process during the run would pass.
- **Revision of `prj_02` and `prj_03`:** these images embed no revision. Their provenance is the clean tree at the start and at the end and the build inside the run. The capture says so in its own words (`rev no embebida`).
- **Ignored files:** the final clean-tree check does not look at ignored files (`git status --ignored`).
