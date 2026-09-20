# This STM32CubeH7 copy has been pruned

This is **not** a pristine clone of the STM32CubeH7 package. It was trimmed on
2026-09-16 so that repository-wide searches stay fast: the full clone was about
970 MB and 11,300 files, which made every `grep` / `find` across this
documentation tree crawl.

Dropping `.git/` also froze every Git **submodule** of the package as an empty
directory. The four submodules that this workspace actually reads were filled in
by hand on 2026-09-19 - see [Submodule state](#submodule-state) below.

## What was kept

| Path | Why |
| :--- | :--- |
| `Drivers/CMSIS/` | CMSIS Core headers and the STM32H7xx device headers |
| `Drivers/STM32H7xx_HAL_Driver/` | HAL and LL source - the reference implementation |
| `Middlewares/` | FreeRTOS and LwIP source (see the submodule table) |
| `Projects/NUCLEO-H745ZI-Q/` | the dual-core reference board for STM32H755ZI |
| `Projects/NUCLEO-H755ZI-Q/` | stub (`readme.txt` only, as shipped by ST) |
| `Documentation/`, `README.md`, `LICENSE.md` | package metadata |

## What was removed

- **`.git/`** (778 MB) - the full history pack.
- `Projects/NUCLEO-H723ZG/`, `NUCLEO-H743ZI/`, `NUCLEO-H753ZI/`,
  `NUCLEO-H7A3ZI-Q/`, `STM32H735G-DK/`, `STM32H743I-EVAL/` - boards with a
  different MCU, single-core or unrelated silicon.

Result: **137 MB, 4,619 files.**

## Submodule state

`.gitmodules` declares 52 submodules. Without `.git/` none of them can be
restored with `git submodule update --init`, so their state here is permanent
until someone copies sources in. Four are populated; the rest are **empty
directories on purpose**.

| Submodule path | State | Notes |
| :--- | :--- | :--- |
| `Drivers/STM32H7xx_HAL_Driver/` | **filled** | 305 files, 127 `.c` - HAL + LL |
| `Drivers/CMSIS/Device/ST/STM32H7xx/` | **filled** | 323 files - `stm32h755xx.h`, startup, system |
| `Middlewares/Third_Party/LwIP/` | **filled** | 768 files - TCP/IP stack source |
| `Middlewares/Third_Party/FreeRTOS/` | **filled** | 313 files - kernel + `GCC/ARM_CM7`, `ARM_CM4F` ports |
| `Drivers/BSP/*` (44 entries) | empty | board support for DISCO / EVAL boards and display, camera, audio and memory parts that the Nucleo-H755ZI-Q does not carry |
| `Middlewares/Third_Party/FatFs`, `LibJPEG`, `mbedTLS` | empty | not used by any project in this workspace |
| `Middlewares/ST/STM32_USB_Device_Library`, `STM32_USB_Host_Library` | empty | USB is documented by `../UM1734_usb_device_library.pdf` only |

**Provenance of the filled four:** copied verbatim from
`02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7/`, which is the
checkout the firmware actually compiles against. They are a **snapshot**, not a
tracked submodule: nothing detects if that project later moves to a different
Cube version. If you bump the Cube version in a project, re-copy these four
trees, or this hub starts documenting an API the firmware no longer uses.

If you need one of the empty ones, fetch it from the upstream repository named
for it in `.gitmodules` and drop it in place.

## Consequences

- **`git pull` no longer works here.** To update, re-clone the package fresh from
  `https://github.com/STMicroelectronics/STM32CubeH7` into a separate location and
  copy across what you need.
- **`git submodule update --init` no longer works here** either, for the same
  reason.
- If you need an example that lived in one of the removed boards, fetch it from that
  fresh clone or from GitHub directly rather than restoring the whole package.

## Which examples to use

`Projects/NUCLEO-H745ZI-Q/Examples/` is the valid reference for STM32H755ZI: the
H745 and H755 are the same die, differing only in the cryptographic accelerator
(present on H755, absent on H745). ST ships the `NUCLEO-H755ZI-Q` folder empty apart
from a `readme.txt`, which is why the H745 folder is the one to read.
