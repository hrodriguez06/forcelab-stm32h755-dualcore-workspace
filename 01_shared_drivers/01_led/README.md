# STM32H755 Shared LED Driver (`01_shared_drivers/01_led`)

Bare-metal driver for the three user LEDs of the NUCLEO-H755ZI-Q (LD1 green,
LD2 yellow, LD3 red). It exists to say something useful when nothing else can:
before `MPU_Config()` and the caches, before `HAL_Init()` and the clock tree, and
from a fault handler. It uses no HAL and makes no assumption about clocks.

Status: in use by every dual-core project of the repo since their bring-up.
Phase F1 closed limitations L1 and L2 (mandatory compile-time macros via `#error`).
The L3–L5 close (2026-10-18) configures the pins through `gpio_bm` (L3), makes
`LED_Toggle()` atomic on the calling core (L4) and disables any LED a table gets
wrong (L5). Both passed an independent audit with the auditor's own board runs
(section 6). What remains open is in section 5.

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
| `LED_Init_Config(cfg)` | `LED_Init_ConfigEx(cfg)`, result discarded |
| `LED_Init_ConfigEx(cfg)` | Copies `cfg` into the driver and configures each LED with `GPIO_BM_ConfigOutput(port, bit, push-pull, low speed, no pull, low)`: validation first, port clock, then `BSRR` before `MODER` inside the `gpio_bm` section and its cross-core lock, and a read-back. An LED that call refuses (bad port, pin > 15, SWD pin, pin locked by `LCKR`, lock busy, read-back mismatch) gets `port = NULL` in the copy, so every later call ignores it. Returns the mask of configured LEDs (bit `i` = LED `i`); `NULL` `cfg` returns 0 and touches nothing |
| `LED_Set(led, on)` | One `BSRR` store (atomic). Out-of-range `led` or `NULL` port: nothing |
| `LED_Toggle(led)` | Reads `ODR` and stores `BSRR` with interrupts masked, restoring the caller's `PRIMASK`: atomic against tasks and ISRs of the calling core, not across cores (section 5) |
| `LED_BootStage(stage)` | Shows a 3-bit code: green = bit 0, yellow = bit 1, red = bit 2 |
| `LED_PanicRed()` | Re-enables the red LED's port clock, sets its `MODER` to output and lights it. For fault handlers: no HAL, no interrupts, no wait |

Active high: `on` drives the pin high. `LED_Init_ConfigEx()` writes `OTYPER`,
`OSPEEDR` and `PUPDR` with their reset values (push-pull, low speed, no pull);
`LED_PanicRed()` writes only `MODER` and `BSRR`.

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

- **`LED_Init()`, `LED_Init_Config()` and `LED_Init_ConfigEx()`** configure
  the pins inside the `gpio_bm` section, and through the cross-core lock when the
  project has installed it (`00_gpio/README.md` §3.3). They may be called from
  either core at any time. All projects of the repo call `LED_Init()` before
  CPU1 releases CPU2, when the hook is not installed yet.
- **`LED_PanicRed()`** still writes `MODER` with a plain read-modify-write,
  outside the section and the lock: accepted exception G14 of `00_gpio`, because
  the fault path is terminal and cannot wait for a semaphore.
- **`LED_Set()` and `LED_BootStage()`** write only `BSRR` and may be called from
  either core at any time.
- **`LED_Toggle()`** is atomic on the core that calls it. Two cores toggling the
  same LED can still lose a change; in the repo every LED call is on CPU1.

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
| L3 | **`LED_PanicRed()` writes `MODER` outside `gpio_bm`** | Accepted exception G14 (section 3.1). The configuration calls go through `gpio_bm` since the L3–L5 close |
| L4 | **`LED_Toggle()` is not atomic across cores** | Atomic on the calling core since the L3–L5 close; making it cross-core would need an HSEM |
| L6 | **Blinking under FreeRTOS load** | Checked by observation in each project, not by a test |
| L7 | **A busy cross-core lock at init disables the LED** | If `GPIO_BM_ConfigOutput()` returns `GPIO_BM_ERR_BUSY` (the other core held the GPIO lock past the project's bound), that LED gets `port = NULL`. If it is the red one, `LED_PanicRed()` then does nothing. `LED_Init_ConfigEx()` returns the mask, so a caller can retry |

*Closed: L1 (masks in `board_def.h`) and L2 (default pins in the driver) in Phase F1; L5 (a bad pin or port in a table passed to `LED_Init_Config()`) in the L3–L5 close: such an LED is refused before any register is written and disabled.*

---

## 6. Validation

Two tools, both with negative controls that go through the same evaluation function as the real check, on mutated measured data:

- [`tools/led_l1l2_checks.py`](../../tools/led_l1l2_checks.py): T1–T3 off-board and T4 on the board, across the four projects;
- the `selftest-led` preset of `prj_01` with its host [`led_hw_check.py`](../../03_firmware_templates/prj_01_template_dualcore/tools/led_hw_check.py): LT1–LT5 on the board, for L3–L5.

### 6.1 `led_l1l2_checks.py` (T1–T4)

- **T1: Build Matrix & Selftests (`run_t1`):**
  - Compiles all 8 production configurations (`debug` and `release` for the 4 projects) directly via `cmake --fresh` and `cmake --build --clean-first`.
  - Verifies code 0, 0 compiler warnings, `objs > 0`, and confirms that `led_bm.c.obj` was compiled in the log and is present on disk.
  - Verifies the 10 selftest presets linking `led_bm` (`selftest-gpio`, `selftest-tim`, `selftest-spi`, `selftest-i2c`, `selftest-led` in `prj_01`; `selftest`, `selftest-adc`, `selftest-uart`, `selftest-hsem`, `selftest-gpio` in `prj_04`) with 0 warnings.
  - Negative control: compiles a synthetic C file with an intentional unused-variable warning outside the repository using project flags via `run_cmd` and verifies that [`evaluate_build_output`](../../tools/led_l1l2_checks.py#L295) detects the warning and fails.
  - Image provenance: verified via `FW_GIT_REV` in `fw_build_id.h` (`prj_01`), in the ELF string table (`prj_04`), and via clean repository working tree and in-execution build freshness (`prj_02`, `prj_03`).

- **T2: Preprocessor `#error` and Range Validation (`run_t2`):**
  - Compiles in a clean temporary directory outside the repository so `-fstack-usage` does not leave `.su` files in the source tree.
  - 4 positive controls: unmodified `board_def.h` compiles with 0 warnings and exit code 0.
  - 16 negative mutations (4 per project): missing `_BIT`, missing `_PORT`, `_PIN` mask instead of `_BIT`, and bit `16U > 15U`; all verified to fail with exit code 1 matching the specific `#error` line.

- **T3: Binary Inspection vs UM2408 Oracle & Control (`run_t3`):**
  - Extracts 24 bytes of `g_led_config` from CM7 `led_bm.c.obj` using `arm-none-eabi-objdump -s`.
  - Compares the 6 32-bit words across all 8 current builds against the UM2408 board oracle:
    `['0x58020400', '0x0', '0x58021000', '0x1', '0x58020400', '0xe']` (PB0, PE1, PB14).
  - Compiles a control worktree at baseline commit `9dc6896` (`../stm32h755_control_9dc6896`, created by the tool when missing) in-situ with `build_all.ps1 -Clean` (requiring `pass_count >= 8`), verifying that current binaries match the control bit-for-bit.
  - Negative control: mutates an extracted byte and verifies that [`parse_and_verify_led_config`](../../tools/led_l1l2_checks.py#L673) rejects it while validating exact 24-byte section size without truncation.

- **T4: Hardware Board Verification (`run_t4`):**
  - Flashes `debug` images using `tools/flash.py` with `STM32_Programmer_CLI -v`.
  - Non-intrusive SWD reads (`mode=HOTPLUG`, `-r32`, with explicit `sn=`) of `g_led_config` in RAM and of `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR` and `ODR` of GPIOB and GPIOE.
  - Verifies `g_led_config` matches the UM2408 table and that PB0, PB14 and PE1 are push-pull, low-speed, no-pull outputs ([`verify_t4_led_state`](../../tools/led_l1l2_checks.py#L873); missing registers fail).
  - Negative controls, on the measured words and registers of a project: a mutated table word, PB0 `MODER` set to input, and PB0 with a pull-up are all rejected.

### 6.2 `selftest-led` (LT1–LT5)

CM7 only, after the normal `LED_Init()` of the boot and after CPU2 is released; no AD2. It configures only PB0, PE1 and PB14 (plus the console), prints every measured value in hex or as a count, and ends with `FW VERDICT=`. The host opens the serial port before an NRST reset through SWD with `sn=`, checks the clean tree, the revision and the build id (with `--allow-dirty` the provenance is `NOT VERIFIED` and the run cannot pass), and judges every step itself.

| Step | What it does | Host oracle | Control in the same run |
| :--- | :--- | :--- | :--- |
| **LT1** (L3) | The three pins to analog (`GPIO_BM_DeinitPin`), a counting cross-core hook, `LED_Init_ConfigEx` with the `board_def.h` table | Mask `0x7`; `lock` = `unlock` ≥ 3; each pin output, push-pull, low speed, no pull, `ODR` = 0 | Pins to analog again and a hook whose `lock()` returns `false`: mask 0, and the 15-word snapshot of GPIOA/B/E identical before and after |
| **LT2** (L5) | Table with green on pin 16, yellow on port `0x58024000` (not GPIOA–GPIOK), red on PA13 (SWD); `LED_Set`/`LED_Toggle`/`LED_BootStage` after it | Mask 0; the 15-word snapshot identical after init and after the calls | Only the green entry valid: mask `0x1`, and the same snapshot oracle must report a change |
| **LT3** (L4) | TIM17 ISR (rate measured with DWT, ~200 kHz) toggles the green LED while the main loop calls `LED_Toggle` 200 000 times, with a `PRIMASK` snapshot (`ODR` bit, ISR count) around each call | `lost = 0` | The `LED_Toggle` of `0927c75` (no `PRIMASK`), same loop: `lost > 0` (about 2000 per run) |
| **LT4** (G14) | Red to analog, `LED_PanicRed()` | Red output, `ODR` = 1 | Green pin fields identical before and after |
| **LT5** | `LED_Init()`, the project's hook (`Shared_GpioLockInstall()`), TIM17 stopped | Three pins as in LT1; TIM17 `CR1`/`DIER`, NVIC pending and clock off | — |

The run is deterministic from reset: the same image prints the same counts in every run.

**Reproducing it** (PowerShell, from the repository root; the AD2 is not used):

```powershell
. .\tools\env.ps1
cd 03_firmware_templates\prj_01_template_dualcore
cmake --preset selftest-led --fresh
cmake --build build/selftest-led
py tools/flash.py --preset selftest-led --sn 0016000C4142501220353451
py tools/led_hw_check.py --sn 0016000C4142501220353451 --out "$env:TEMP\led_hw_run1.txt"
py tools/led_hw_check.py --mock --out "$env:TEMP\led_mock.txt"
cmake --preset debug; cmake --build build/debug
py tools/flash.py --preset debug --sn 0016000C4142501220353451
cd ..\..
py tools/led_l1l2_checks.py --test t4 --sn 0016000C4142501220353451 -o "$env:TEMP\led_t4.txt"
py tools/led_l1l2_checks.py --test t1,t2,t3 -o "$env:TEMP\led_t1t3.txt"
```

The captures go outside the repository, or each one is committed before the next run: the host and T1–T4 require a clean tree. T4 leaves `prj_04` `debug` on the board.

### 6.3 Captures Supporting this Driver

| Capture file | Scope | Executed by | Image | Result |
| :--- | :--- | :--- | :--- | :--- |
| [`led_audit_hw_91ffca6_run1.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/led_audit_hw_91ffca6_run1.txt) … [`_run3.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/led_audit_hw_91ffca6_run3.txt) | `selftest-led`, LT1–LT5 | Auditor | `91ffca6` | 3/3 `HOST VERDICT=PASS` |
| [`docs/led_audit_t4_91ffca6.txt`](docs/led_audit_t4_91ffca6.txt) | T4, four projects | Auditor | `91ffca6` | `OVERALL VERDICT: PASS` |
| [`docs/led_audit_t1t3_53f03cc.txt`](docs/led_audit_t1t3_53f03cc.txt) | T1–T3 | Auditor | `53f03cc` (same `led_bm.c` as the final tree) | `OVERALL VERDICT: PASS` |
| [`docs/led_audit_warm_prj02_53f03cc.txt`](docs/led_audit_warm_prj02_53f03cc.txt), [`_prj03_`](docs/led_audit_warm_prj03_53f03cc.txt), [`_prj04_`](docs/led_audit_warm_prj04_53f03cc.txt) | 20 warm resets of each `debug` image, boot banner checked | Auditor | `53f03cc` | 20/20 each |
| [`docs/led_dev_hw_65e5244_run1.txt`](docs/led_dev_hw_65e5244_run1.txt), [`_run2.txt`](docs/led_dev_hw_65e5244_run2.txt) | `selftest-led` | Implementer | `65e5244` | `HOST VERDICT=PASS` |
| [`docs/led_dev_t4_65e5244.txt`](docs/led_dev_t4_65e5244.txt), [`docs/led_dev_t1t3_65e5244.txt`](docs/led_dev_t1t3_65e5244.txt), [`docs/led_dev_mock_65e5244.txt`](docs/led_dev_mock_65e5244.txt) | T4, T1–T3, host mock (five injected failures detected) | Implementer | `65e5244` | PASS |

*Pruned captures (rule 7 of `00_gpio/README.md` §14), readable with `git show <commit>:<path>`:*

| Capture | Commit | Superseded by |
| :--- | :--- | :--- |
| `01_shared_drivers/01_led/docs/led_l1l2_audit_t1t3_736948f.txt`, `led_l1l2_audit_t4_736948f.txt`, `led_l1l2_audit_t1t3_af836ea.txt`, `led_l1l2_checks_824d989.txt` | `0927c75` | The T1–T4 captures above (same tool, extended) |
| `01_shared_drivers/01_led/docs/led_l1l2_checks_2e6a15d.txt` | `78aee90` | L1/L2 rounds |
| `01_shared_drivers/01_led/docs/led_l1l2_checks_411673b.txt` | `afe8ee8` | L1/L2 rounds |
| `01_shared_drivers/01_led/docs/led_l1l2_checks_b9fbaad.txt` | `2d221e8` | L1/L2 rounds |
| `01_shared_drivers/01_led/docs/led_dev_hw_c080fcb_run1.txt`, `_run2.txt`, `led_dev_t4_c080fcb.txt`; `03_firmware_templates/prj_01_template_dualcore/docs/led_dev_hw_c080fcb_run1.txt`, `_run2.txt` | `53f03cc` | The first L3–L5 delivery; its selftest printed derived values |
| `03_firmware_templates/prj_01_template_dualcore/docs/led_audit_hw_53f03cc_run1.txt` … `_run3.txt`; `01_shared_drivers/01_led/docs/led_audit_t4_53f03cc.txt` | `2fd007a` | The auditor runs at `91ffca6` |

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
| L3–L5 plan, decisions approved by the owner | `8c30cb8` |
| L3–L5 in the driver: `GPIO_BM_ConfigOutput`, `LED_Init_ConfigEx`, atomic `LED_Toggle` | `654cf75`, `bba74a6`, `3e03485` |
| `selftest-led`, its host and the T4 extensions | `d3a03b0`, `db39a0f`, `c080fcb` |
| First audit: driver correct, evidence not (A1–A12) | `2fd007a` |
| Corrections: raw measured data judged by the host, shared oracles for the controls, hook restored, DWT-measured ISR rate | `65e5244` |
| Audit passed, auditor board runs at `91ffca6` | `147f821` |

### Process Documents History

The process documents (`IMPLEMENTATION_PLAN.md`, `IMPLEMENTER_INSTRUCTIONS.md`,
`IMPLEMENTATION_REPORT.md`, `AUDIT.md`) were removed at each close; the audit
record lists every finding with the captures it was judged on:

```bash
# L1/L2
git show 69179d7^:01_shared_drivers/01_led/IMPLEMENTATION_PLAN.md
git show 69179d7^:01_shared_drivers/01_led/IMPLEMENTER_INSTRUCTIONS.md
git show 69179d7^:01_shared_drivers/01_led/IMPLEMENTATION_REPORT.md
git show 69179d7^:01_shared_drivers/01_led/AUDIT.md
# L3-L5
git show 147f821:01_shared_drivers/01_led/IMPLEMENTATION_PLAN.md
git show 147f821:01_shared_drivers/01_led/IMPLEMENTER_INSTRUCTIONS.md
git show 147f821:01_shared_drivers/01_led/IMPLEMENTATION_REPORT.md
git show 147f821:01_shared_drivers/01_led/AUDIT.md
```

### Accepted Tool Limitations

What `tools/led_l1l2_checks.py` still does not check, accepted at the close (`AUDIT.md`):
- **Configure output:** only `CMake Error` fails a configure. `CMake Warning` lines are not counted.
- **Image freshness:** an image counts as built in the run by its modification time (no older than the run start minus 5 s). An ELF written by another process during the run would pass.
- **Revision of `prj_02` and `prj_03`:** these images embed no revision. Their provenance is the clean tree at the start and at the end and the build inside the run. The capture says so in its own words (`rev no embebida`).
- **Ignored files:** the final clean-tree check does not look at ignored files (`git status --ignored`).
