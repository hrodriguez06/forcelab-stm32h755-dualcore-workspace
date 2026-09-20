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
[ipc] m7=4 m4=37  (CPU2 alive)
```

**`m4=0` and `CPU2 NOT RESPONDING` is the useful failure.** It means CPU2 never
ran, which is an option-byte or a Flash-bank-2 problem, not an application one.
Check `BCM4` with `py tools/flash.py --read-ob`.

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

Both cores link clean - no errors, no warnings - against Cube `8ec3113d`
(HAL `e3c518e`, CMSIS device `81db1ec`, FreeRTOS V10.6.2 `e41c220`) with
`arm-none-eabi-gcc` 14.3.1.

| | FLASH | RAM |
| :--- | ---: | ---: |
| CPU1 | 27 356 B (2.61 %) | DTCM 55 544 B (42.38 %) |
| CPU2 | 2 540 B (0.24 %) | SRAM2 12 336 B (9.41 %) |

`.shared_ram` starts at `0x38008000` in **both** ELF files and `g_shared`
resolves to that address in both - the check from
`00_docs_stm32h7/docs/00_core_and_system/linker_templates/README.md` section 6.
No CPU1 symbol lands in CPU2's SRAM2.

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
├── Custom_Drivers/              # staging area, NN_ = bring-up order
│   ├── 01_led/                  # bare-metal, boot codes
│   └── 04_uart/                 # HAL, ST-LINK VCP console
└── tools/                       # env.ps1, flash.py, monitor.py, openocd/
```

**`[golden copy]`** means the file must stay byte-identical to
`00_docs_stm32h7/docs/00_core_and_system/linker_templates/`. Edit it there
first, then propagate. Section 7 of that README has the `diff` commands.

## 6. Turning this into a product

1. Copy to `02_firmware_projects/prj_<name>/` and rename the `project()` in
   `CMakeLists.txt`.
2. Delete the core you do not need. Deleting CM4 also means
   `CM4_BOOT_ENABLED 0`.
3. Replace the two CPU1 tasks and the CPU2 super-loop.
4. Add drivers under `Custom_Drivers/` using the `NN_` slot that matches
   `01_shared_drivers/`, so a driver that proves itself can be promoted with a
   plain move.
5. Put every DMA-visible buffer in the right section - `.dma_buf`, `.eth_ram`,
   `.d3_ram` - and align it to 32 bytes. The linker script header lists them
   and `mpu_config.c` marks them non-cacheable.
