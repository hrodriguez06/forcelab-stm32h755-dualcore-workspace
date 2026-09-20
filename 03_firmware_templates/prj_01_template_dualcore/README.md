# prj_01_template_dualcore

Golden base for an STM32H755ZI product on the Nucleo-H755ZI-Q. Builds **both**
cores and demonstrates the boot handshake between them, which is the part that
is hard to get right and expensive to debug later.

Copy the directory into `02_firmware_projects/`, rename it, and start deleting.

## 1. What it does

| Core | Image | Role |
| :--- | :--- | :--- |
| CPU1 (Cortex-M7) | `cm7_firmware.elf`, Flash bank 1 `0x08000000` | FreeRTOS. Owns the clock tree, the power supply, the console and the LEDs. Releases CPU2. |
| CPU2 (Cortex-M4) | `cm4_firmware.elf`, Flash bank 2 `0x08100000` | Bare metal. Waits for CPU1, then runs a super-loop. |

CPU1 runs two tasks: a heartbeat that blinks LD1 and increments
`g_shared.m7_heartbeat`, and a status task that prints both counters every two
seconds:

```text
=== STM32H755ZI dual-core template - CPU1 (Cortex-M7) ===
[sys] sysclk=400000000 Hz hclk=200000000 Hz
[ipc] shared block at 0x38008000, magic=0x48375353
[ipc] CPU2 boot handshake: enabled (HSEM 0)
[ipc] m7=1 m4=0  (CPU2 starting)
[ipc] m7=4 m4=15  (CPU2 alive)
```

The first status line says `starting`, not a verdict: the check compares two
samples and there is no earlier one yet. CPU1 routinely gets there before CPU2
has woken from STOP, so a verdict at that point would be a coin toss. From the
second line on the label means what it says.

**`m4=0` and `CPU2 NOT RESPONDING` on the second line and after is the useful
failure**, and it has three causes worth separating before touching any
application code:

1. **CPU2 never left reset** - an option-byte or Flash-bank-2 problem. Check
   with `py tools/flash.py --read-ob`, and confirm the image is there:
   `-r32 0x08100000` should show the CPU2 stack pointer and reset vector.
2. **CPU2 booted but never woke** from the HSEM handshake. `cpu2_woke` in
   CPU2 SRAM2 stays 0. See section 3, "Two things this cost to get right".
3. **CPU2 runs but cannot reach the shared block.** `cpu2_woke` is
   `0x00C0FFEE` and `cpu2_loops` advances while `m4_heartbeat` does not.

## 2. The boot handshake

From `00_docs_stm32h7/docs/00_core_and_system/dual_core_boot_ipc.md`. Both cores
leave reset together, and the ordering is enforced, not assumed:

1. CPU2 subscribes to HSEM 0 and enters STOP on the D2 domain. Entering STOP is
   what drops `D2CKRDY`.
2. CPU1 waits for `D2CKRDY` to drop - that is how it knows CPU2 has parked
   rather than still racing towards RCC.
3. CPU1 configures the MPU, caches, HAL, clock tree and peripheral clocks.
4. CPU1 initialises the shared block and publishes `SHARED_MAGIC` last.
5. CPU1 takes and releases HSEM 0. The notification wakes CPU2.
6. CPU2 initialises only what belongs to it. It never touches RCC, the PLLs or
   the power supply.

Deviating from this produces a board that works "sometimes": the outcome
depends on which core wins a race in the first microseconds.

`CM4_BOOT_ENABLED` in `CM7/Core/Inc/main.h` is `1` here. **Set it to 0 if you
delete the CM4 image** - CPU1 would otherwise wait for a core that never parks.

## 3. Build

```powershell
.\tools\env.ps1                  # CMake, Ninja and the compiler onto PATH
cmake --preset debug
cmake --build --preset debug     # builds BOTH cores
```

Artifacts: `build/debug/CM7/cm7_firmware.elf` and `build/debug/CM4/cm4_firmware.elf`.

The Cube package is **not** checked out per project: `CMakeLists.txt` points at
the workspace submodule in `02_firmware_projects/prj_eth_lwip_node/external/`,
because it is 110 MB and identical for every product. Override with
`-DCUBE_DIR=...`. If it is missing, restore it from the workspace root with
`.\tools\init_cube_submodule.ps1`.

### Verified build

Both cores link clean from a full rebuild - **no errors, no warnings** - against
Cube `8ec3113d` (HAL `e3c518e`, CMSIS device `81db1ec`, FreeRTOS V10.6.2
`e41c220`) with `arm-none-eabi-gcc` 14.3.1.

| | FLASH | RAM |
| :--- | ---: | ---: |
| CPU1 | 27 404 B (2.61 %) | DTCM 55 544 B (42.38 %) |
| CPU2 | 2 604 B (0.25 %) | SRAM2 12 344 B (9.42 %) |
| shared | - | SRAM4 160 B, at `0x38008000` in **both** images |

`g_shared` resolves to `0x38008000` in both ELF files - the check from
`00_docs_stm32h7/docs/00_core_and_system/linker_templates/README.md` section 6 -
and no CPU1 symbol lands in CPU2's SRAM2.

### Verified on hardware

Nucleo-H755ZI-Q, ST-LINK/V3 `0016000C4142501220353451`, silicon rev V:

```text
[   0.914] === STM32H755ZI dual-core template - CPU1 (Cortex-M7) ===
[   0.919] [sys] sysclk=400000000 Hz hclk=200000000 Hz
[   0.931] [ipc] shared block at 0x38008000, magic=0x48375353
[   0.938] [ipc] CPU2 boot handshake: enabled (HSEM 0)
[   2.907] [ipc] m7=4 m4=15  (CPU2 alive)
[  10.814] [ipc] m7=20 m4=77  (CPU2 alive)
```

Both counters advance: CPU1 at four per two seconds, which is its 500 ms task
period, and CPU2 freely in its super-loop. The handshake completes and CPU2
reaches SRAM4 across the domain boundary.

### Two things this cost to get right

Both failed *silently* - the build was clean and nothing faulted - and both are
now commented where they live, because a template that hides them is worse than
no template.

**1. `SysTick_Handler` went missing with lwIP.** `FreeRTOSConfig.h` does not map
`xPortSysTickHandler` onto `SysTick_Handler` when `cmsis_os2.c` is in the build,
because that file defines it. Dropping lwIP from the reference project also drops
`cmsis_os2.c`, and then **nothing** defines it: the vector resolves to the weak
`Default_Handler`, an infinite loop. The link succeeds. The board freezes inside
the first tick, `m7_heartbeat` stuck at 1, **CFSR and HFSR both zero** - no fault
at all. The only evidence is `xPSR`: `IPSR = 15 = SysTick`.

**2. CPU2 never woke from `WFE`.** ST's reference CM4 code omits `SEVONPEND`, and
copying it verbatim left CPU2 asleep for ever while every register said the
handshake had worked - `HSEM_C2IER = 1`, `HSEM_C2MISR = 1`, semaphore released.
`HAL_PWREx_EnterSTOPMode` issues a single `__WFE()`, and a *pending* interrupt is
not an *event* unless it is NVIC-enabled or `SEVONPEND` is set. ST's example gets
away with it because CPU1 leaves D2 idle there and the domain exits DSTOP through
the peripheral wake-up path; here CPU1 holds D2 awake, since the console USART3
is on APB1 in D2, so CPU2 only reaches CSleep and needs a real core event.

`cpu2_woke` and `cpu2_loops` in `CM4/Core/Src/main.c` exist because of the
second one. They live in CPU2's own SRAM2, which nothing else writes, so a
debugger can tell "CPU2 is asleep" from "CPU2 runs but cannot reach the shared
block" without halting anything.

## 4. Flash and watch

```powershell
py tools/flash.py                # programs whichever cores are built
py tools/monitor.py --timestamps # follow the console, 115200 8N1
```

`flash.py` writes both images in one connection, CPU1 to bank 1 and CPU2 to
bank 2. Sectors are 128 KB and the banks are separate physical arrays, so
programming one cannot disturb the other.

## 5. Layout

```text
prj_01_template_dualcore/
├── CMakeLists.txt               # builds both cores; locates STM32CubeH7
├── CMakePresets.json
├── cmake/                       # toolchain file and shared flags
├── Common/                      # the dual-core contract
│   ├── Inc/
│   │   ├── board_def.h          # clocks and board constants, both cores
│   │   └── shared_memory.h      # IPC block + HSEM ids  [golden copy]
│   └── Src/shared_memory.c      # the definition, compiled into BOTH images
├── CM7/                         # CPU1: FreeRTOS, D1 domain
│   ├── FreeRTOSConfig.h
│   ├── linker/stm32h755zi_cm7.ld            [golden copy]
│   ├── Core/                    # main, interrupts, HAL config, time base
│   └── App/                     # app_main, mpu_config [golden copy], app_log
├── CM4/                         # CPU2: bare metal, D2 domain
│   ├── linker/stm32h755zi_cm4.ld            [golden copy]
│   └── Core/                    # main (handshake + super-loop), interrupts
└── tools/                       # shims onto <workspace>/tools/
    ├── env.ps1                  # toolchain onto PATH
    ├── flash.py                 # flashes CPU1/CPU2, reads option bytes
    └── monitor.py               # follows the console on the ST-LINK VCP
```

No `Custom_Drivers/` here: CPU1 links `stm32h7_driver_led` and
`stm32h7_driver_uart` out of `01_shared_drivers/`, so a project cloned from
this template inherits every errata fix made there instead of a copy that
stops receiving them the day it is made. CPU2 links neither - a peripheral has
exactly one owner, and its liveness shows through `g_shared.m4_heartbeat`.

**`[golden copy]`** means the file must stay byte-identical to
`00_docs_stm32h7/docs/00_core_and_system/linker_templates/`. Edit it there
first, then propagate. Section 7 of that README has the `diff` commands.

## 6. Turning this into a product

1. Copy to `02_firmware_projects/prj_<name>/` and rename the `project()` in
   `CMakeLists.txt`.
2. Delete the core you do not need. Deleting CM4 also means
   `CM4_BOOT_ENABLED 0`.
3. Replace the two CPU1 tasks and the CPU2 super-loop.
4. Add drivers by linking more targets from `01_shared_drivers/`
   (`stm32h7_driver_gpio`, `..._led`, `..._uart`, `..._eth`, or the
   `stm32h7_shared_drivers` umbrella) in the `CMakeLists.txt` of the core that
   owns the peripheral. Only when a driver is specific to this one product does
   it belong in a project-local `Custom_Drivers/`, using the same `NN_` slot so
   promoting it later is a plain move.
5. Put every DMA-visible buffer in the right section - `.dma_buf`, `.eth_ram`,
   `.d3_ram` - and align it to 32 bytes. The linker script header lists them
   and `mpu_config.c` marks them non-cacheable.

## 7. Hardware verification

Last run: **2026-09-20**, on a NUCLEO-H755ZI-Q (silicon revision `0x2003` =
rev V), both images programmed under reset. Re-run it with the commands in
section 4; the numbers below belong to that run and should be refreshed with
it.

This was the first run after the template stopped vendoring its drivers and
started linking `01_shared_drivers/`, so it is mainly a check that the console
and the LEDs still work when they come from the library.

### Console - the shared `uart_hal`

```text
=== STM32H755ZI dual-core template - CPU1 (Cortex-M7) ===
[sys] sysclk=400000000 Hz hclk=200000000 Hz
[sys] silicon revision id=0x2003
[ipc] shared block at 0x38008000, magic=0x48375353
[ipc] CPU2 boot handshake: enabled (HSEM 0)
[ipc] m7=1 m4=0  (CPU2 starting)
[ipc] m7=4 m4=15  (CPU2 alive)
```

A banner at all is the test: `UART_Console_Init()` and `UART_Console_Write()`
now come from `01_shared_drivers/04_uart`. `magic` reads back as
`SHARED_MAGIC`, and `m4_heartbeat` climbs monotonically from there -
`15 -> 30 -> 46 -> 61 -> ... -> 277` over 35 s.

### LEDs - the shared `led_bm`, read over SWD

The board is not always in front of whoever runs this, and the LEDs are
readable without it:

```powershell
# MODER: are the three pins outputs, on the ports board_def.h names?
STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 0x58020400 4 -r32 0x58021000 4

# ODR: LD1 toggles every 500 ms, so two reads a few hundred ms apart differ
STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 0x58020414 1
```

| Register | Value | Meaning |
| :--- | :--- | :--- |
| `GPIOB_MODER` | `0xDFFFFEBD` | PB0 `bits[1:0]=01`, PB14 `bits[29:28]=01` - both outputs |
| `GPIOE_MODER` | `0xFFFFFFF7` | PE1 `bits[3:2]=01` - output |
| `GPIOB_ODR` | alternates `0x4000` / `0x4001` | LD1 toggling, LD3 steady on |
| `GPIOE_ODR` | `0x00000002` | LD2 steady on |

The pins land where `board_def.h` puts them, which is what makes
`BOARD_LED_<x>_BIT` worth writing: the shared driver reads those, and before
the move this file spelled them as `GPIO_PIN_x` masks that nothing read.

> [!NOTE]
> **LD2 and LD3 steady on is correct here, not a fault.** `LED_BOOT_SCHEDULER`
> is 7, and `LED_BootStage()` writes the stage as a 3-bit code across the three
> LEDs, so handing over to FreeRTOS lights all three. The heartbeat task then
> toggles the green one only. Red means a fault only when it is the *only* one
> lit, which is what `LED_PanicRed()` leaves behind.

### About `tools/`

The scripts under `tools/` are three-line shims; the implementations live once
in the workspace, at `<workspace>/tools/`. They used to be copied per project
and had already drifted - `--timeout` existed in one tree and not the others,
which is what made this capture awkward to automate. `py tools/flash.py` and
`tools/env.ps1` work from here exactly as before.
