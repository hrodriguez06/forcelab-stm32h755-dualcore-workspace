# prj_eth_lwip_node — Ethernet / lwIP node on STM32H755ZI

10/100 Mbps Ethernet bring-up for the **NUCLEO-H755ZI-Q**, running on the **Cortex-M7 (CPU1)** with FreeRTOS and lwIP. The node answers ICMP echo, serves a UDP echo on port 7, and takes a static address of `192.168.1.150/24` (gateway `192.168.1.1`) or a DHCP lease.

Bring-up and verification procedure: **[docs/ETHERNET_BRINGUP.md](docs/ETHERNET_BRINGUP.md)**.

---

## 1. Scope

| In scope | Out of scope |
| :--- | :--- |
| CPU1: MPU, clocks, ETH MAC, RMII, LAN8742A, lwIP, FreeRTOS | CPU2 (Cortex-M4) firmware |
| The three ES0445 Ethernet workarounds | PTP / IEEE 1588, TSO, hardware ARP offload |
| Static addressing and DHCP | TLS, higher-level application protocols |

CPU2 is not built. `CM4_BOOT_ENABLED` in `CM7/Core/Inc/main.h` is `0`, so CPU1 does not wait on the HSEM handshake; set it to `1` and add a `CM4/` directory when a real-time core is needed.

---

## 2. Layout

```text
prj_eth_lwip_node/
├── CMakeLists.txt                   # Top-level orchestrator
├── CMakePresets.json                # Ninja presets: debug / release
├── cmake/
│   ├── arm-none-eabi.cmake          # Cross-compilation toolchain
│   └── common_flags.cmake           # Warning, CPU and definition sets
├── Common/Inc/
│   └── board_def.h                  # Clock tree, MAC address, PHY address
├── CM7/
│   ├── CMakeLists.txt
│   ├── FreeRTOSConfig.h             # Priority grouping 4, syscall ceiling 5
│   ├── linker/stm32h755zi_cm7.ld    # Golden script; .eth_ram -> SRAM3
│   ├── Core/
│   │   ├── Inc/{main.h, stm32h7xx_hal_conf.h, stm32h7xx_it.h}
│   │   └── Src/
│   │       ├── main.c                        # MPU -> caches -> HAL -> clocks
│   │       ├── stm32h7xx_it.c                # ETH_IRQHandler, TIM6
│   │       ├── stm32h7xx_hal_timebase_tim.c  # HAL tick on TIM6
│   │       └── syscalls.c
│   └── App/
│       ├── Inc/{app_main.h, app_log.h, mpu_config.h, net_config.h,
│       │        net_task.h, ethernetif.h, lwipopts.h}
│       └── Src/
│           ├── app_main.c            # Task creation, scheduler start
│           ├── mpu_config.c          # Runs BEFORE SCB_EnableDCache()
│           ├── ethernetif.c          # lwIP glue, Rx thread, link thread
│           ├── net_task.c            # tcpip_init, netif, DHCP, UDP echo
│           └── app_log.c
├── Custom_Drivers/                   # NN_ prefix = slot in the bring-up sequence,
│   │                                 # same numbering as 01_shared_drivers/
│   ├── CMakeLists.txt
│   ├── 01_led/
│   │   └── led_bm.[ch]              # boot codes + runtime status LEDs
│   ├── 04_uart/
│   │   └── uart_hal.[ch]            # ST-LINK VCP console (USART3)
│   └── 08_eth/
│       ├── eth_hal.[ch]             # MAC, RMII pins, descriptors, errata
│       └── lan8742_phy.[ch]         # SMI/MDIO transceiver driver
├── docs/
│   └── ETHERNET_BRINGUP.md          # Verification guide
└── tools/                           # shims; the scripts live in <workspace>/tools/
    ├── env.ps1                      # puts CMake/Ninja/GCC on PATH for the session
    ├── flash.py                     # flashes CPU1/CPU2, reads option bytes
    └── monitor.py                   # follows the USART3 log on the ST-LINK VCP
```

---

## 3. Memory partition

Everything the Ethernet DMA touches is in **D2 SRAM3**, and nothing else is.

| Object | Section | Region | Attributes |
| :--- | :--- | :--- | :--- |
| Rx descriptors | `.RxDescripSection` | SRAM3 `0x30040000` | Normal, non-cacheable, shareable |
| Tx descriptors | `.TxDescripSection` | SRAM3 | idem |
| Rx buffers (8 × 1536 B) | `.RxArraySection` | SRAM3 | idem |
| Tx buffers (4 × 1536 B) | `.eth_ram` | SRAM3 | idem |
| lwIP heap, pbuf pool, FreeRTOS heap | `.bss` | DTCM `0x20000000` | zero wait state, never DMA-visible |

About 19 KB of the 32 KB SRAM3 window is used; a `_Static_assert` in `eth_hal.c` fails the build if that stops being true.

`.itcm_text` is loaded from FLASH and run from ITCM (`>ITCMRAM AT> FLASH`), and **nothing in the CMSIS startup file performs that copy** — it only does `_sidata` → `_sdata` and zeroes `.bss`. `ITCM_Init()` in `main.c` does it, right after the caches come up and before anything could branch there. The section is empty today, so this costs nothing; without it the first function anyone places in ITCM would execute uninitialised memory at `0x00000000`.

The MPU programs SRAM3 as region 6 with `TEX=1, C=0, B=0, S=1` (Normal non-cacheable, shareable, full access, execute-never), and `MPU_Config()` runs **before** `SCB_EnableDCache()`. Because no DMA-visible buffer is ever cacheable, the data path contains no `SCB_CleanDCache_by_Addr()` / `SCB_InvalidateDCache_by_Addr()` calls at all — which is the point of the partition.

Frames are copied between pbufs and the driver's buffers. `docs/ETHERNET_BRINGUP.md` section 9 covers the trade and how to move to zero-copy.

---

## 4. Silicon errata applied (ES0445)

| Erratum | Workaround | Where |
| :--- | :--- | :--- |
| 2.3.11 — MII unavailable on this package | `MediaInterface = HAL_ETH_RMII_MODE`, `SYSCFG_PMCR.EPIS` = RMII before the MAC leaves reset | `eth_hal.c`, `HAL_ETH_MspInit()` |
| 2.25.10 — packets dropped at 10 Mbps on fake dribble + CRC | `ETH_MTLRXQOMR.FEP` set, unconditionally, and re-applied on every start | `ETH_Driver_ApplyErrataRxErrorPackets()` |
| 2.25.8 — spurious Rx watchdog interrupt | `ETH_DMACRXIWTR` written in two steps: `RWT[7:0]` with `RWTU` held at 0, then `RWTU[1:0]`; a non-zero `RWTU` with `RWT` at 0 is refused | `ETH_Driver_SetRxWatchdog()` |
| 2.25.11 — ARP offload not effective | `ETH_MACCR.ARPEN` cleared; lwIP answers ARP in software | `eth_disable_arp_offload()` |
| 2.3.5 — VOS0 / 480 MHz unavailable on revisions Y and W | Clock tree targets 400 MHz at VOS1, valid on every revision | `main.c`, `SystemClock_Config()` |

> `RWT` / `RWTU` live in `ETH_DMACRXIWTR` at offset `0x1138`, not in `ETH_MACWTR` at `0x000C` — the latter holds `PWE` / `WTO[3:0]` and bounds received-packet length. See `docs/ETHERNET_BRINGUP.md` section 6.

---

## 5. FreeRTOS contract

- `HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4)` — all four bits are preemption priority.
- `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`; `ETH_IRQn` runs at **6**, so its callbacks may use the `...FromISR` API.
- `HAL_GetTick()` runs on **TIM6**; SysTick belongs to the scheduler.
- The ETH ISR only signals semaphores. All descriptor work happens in `eth_rx` (priority 24), above the tcpip thread (16), above the link supervisor (16) and the application task (8).

---

## 6. Build and flash

There is no standalone CMake on this workspace: CMake, Ninja and the compiler
ship inside STM32CubeIDE, in plugin directories whose names carry a build stamp
that changes on every update. [`tools/env.ps1`](tools/env.ps1) resolves them by
wildcard and puts them on `$env:PATH` for the current PowerShell session, so no
path has to be written by hand.

```powershell
.\tools\env.ps1                 # PATH for this session (prints the versions it found)
cmake --preset debug
cmake --build --preset debug
```


#### Restoring STM32CubeH7 after a fresh clone

```powershell
..\..\tools\init_cube_submodule.ps1        # from the project directory
```

`git submodule update --init --recursive` does **not** work here, and the
failure modes are not obvious, which is why this is a script rather than a line
in a README:

| What goes wrong | Why |
| :--- | :--- |
| `Filename too long` | ST's `Projects/` tree passes the Windows 260-character limit once nested this deep. The checkout dies and git rolls the registration back, leaving nothing. |
| 48 unwanted repositories | `--recursive` initialises every inner submodule, including 44 BSP repositories for boards this product does not have. |
| ~350 MB of object store | A plain `--depth 1` still downloads every blob in the package, for ~40 MB of files anyone here will open. |

The script does a partial (`--filter=blob:none`), shallow (`--depth 1`), sparse
clone limited to `Drivers` and `Middlewares`, then initialises only the four
inner submodules the firmware compiles against. Measured end to end on this
workspace: **~30 seconds, 7.7 MiB of workspace pack plus 6.2 MiB for the
package**, against 359 MB for the naive approach.

It takes the commit from the gitlink recorded in the index rather than from
whatever `master` is that day, and fetches that exact object if upstream has
moved, so it keeps working over time. `-Check` reports without changing
anything and exits 1 when the package is missing - useful in CI.
`CMakeLists.txt` points at this script when the package is absent.

> **The FreeRTOS version changed when this became a real submodule.** The
> vendored copy that preceded it carried kernel **V11.2.0**, which is *newer*
> than the **V10.6.2** the Cube package pins - someone had upgraded it in place,
> and nothing recorded that. Pinning to ST's commit is the point of a submodule,
> so the package's version is what is used now: it builds clean and runs
> identically on the board. To go back to V11.2.0, check out `d8c632a` inside
> `Middlewares/Third_Party/FreeRTOS` - but note that a fresh
> `git submodule update` restores ST's pin, because the outer repository records
> only the STM32CubeH7 commit, not overrides inside it.

Add `--fresh` to the configure step after renaming or moving source files: the
Ninja cache stores the old paths and will otherwise build a stale graph.

| Command | What it does |
| :--- | :--- |
| `.\tools\env.ps1` | Puts cmake, ninja, gcc, STM32_Programmer_CLI, openocd and the ST-LINK GDB server on PATH. Idempotent. |
| `.\tools\env.ps1 -Check` | Reports what it found and leaves PATH alone. Exit code 1 if a required tool is missing - use this in CI. |
| `.\tools\env.ps1 -Root <dir>` | Skips the search. Needed when several CubeIDE or CubeCLT versions are installed and the newest is not the one you build with. |

If **STM32CubeCLT** is ever installed, the script prefers it over CubeIDE: its
paths are stable and it is the supported way to build from the command line.

Artifacts land in `build/debug/CM7/`: `cm7_firmware.elf`, `.bin`, `.hex` and `.map`.

### Flashing

[`tools/flash.py`](tools/flash.py) finds the programmer, picks up whichever core
images exist and refuses to program an ELF older than its sources. Use the `py`
launcher: the bare `python` on this machine is the Microsoft Store alias stub.

```powershell
py tools/flash.py                    # CPU1 (+ CPU2 when built), debug preset
py tools/flash.py --preset release
py tools/flash.py --tool openocd     # instead of STM32CubeProgrammer
py tools/flash.py --dry-run          # print the command, touch nothing
```

| Command | What it does |
| :--- | :--- |
| `--list` | Connected ST-LINK probes. Read-only. |
| `--read-ob` | Dump option bytes. Read-only, no confirmation. |
| `--ob NAME=VALUE` | Write an option byte. Repeatable, always confirmed, never inferred. |
| `--erase` | Full chip erase before programming. Confirmed. |
| `--core cm7\|cm4\|both` | Override the autodetection. |

The equivalent by hand, if you would rather not use the script:

```powershell
STM32_Programmer_CLI -c port=SWD mode=UR -w build/debug/CM7/cm7_firmware.elf -v -rst
```

> **Option bytes are not configured for you.** A wrong boot address leaves a
> board that does not start, so there is no "set up dual-core boot" shortcut.
> Note that on this board (device `0x450` Rev V, CubeProgrammer 2.23) `--read-ob`
> reports the *single-core* H7 map - `BOOT_ADD0` / `BOOT_ADD1`, no `BCM7` /
> `BCM4` - even though it identifies the CPU as Cortex-M7/M4. Read the current
> values before writing any name taken from a manual.

> **OpenOCD backend.** ST ships no board file for this Nucleo, so
> [`tools/openocd/stm32h755_dualcore.cfg`](../../tools/openocd/stm32h755_dualcore.cfg)
> is written here, once for the workspace. It declares CPU2 only when a CPU2
> image is being
> programmed: while CPU2 is held in reset its access port is unreachable and
> every reset fails. It also forces SYSRESETREQ over the script default of
> VECTRESET, which hangs on the Cortex-M7.


### Watching the log

[`tools/monitor.py`](tools/monitor.py) follows the USART3 console on the
ST-LINK VCP. It finds the port itself by matching the ST-LINK USB vendor id, so
there is no COM number to look up, and it reopens the port when it disappears -
leave it running in one window and flash from another.

```powershell
py tools/monitor.py                  # autodetect the ST-LINK VCP and follow
py tools/monitor.py --list           # show the serial ports it can see
py tools/monitor.py --timestamps     # prefix each line with elapsed seconds
py tools/monitor.py --log boot.txt   # also append to a file
```

It needs **no packages installed**: the port is driven through the Win32 API
with `ctypes`, not pyserial, which keeps this workspace free of Python
dependencies. The price is that it is Windows-only, like `tools/env.ps1`.

It is a viewer and never writes to the target. Nothing in the firmware reads
from the console, so there is nothing to type at.

### Verified build

Debug and Release both link clean — no errors, no warnings — against:

| Component | Version / commit |
| :--- | :--- |
| Compiler | `arm-none-eabi-gcc` 14.3.1 (GNU Tools for STM32 14.3.rel1) |
| STM32CubeH7 package | `8ec3113d` (master) |
| CMSIS device H7 | `81db1ec` |
| STM32H7xx HAL driver | `e3c518e` |
| FreeRTOS kernel | V10.6.2 (`e41c220`) - the version the Cube package pins |
| lwIP | 2.2.1 (`07680c8`) |

Footprint (Debug): FLASH 84 520 B (8.06 %), DTCM 101 416 B (77.37 %), **SRAM3 `.eth_ram` 18 624 B (56.84 %)**.

### Verified on hardware

Run on a NUCLEO-H755ZI-Q (Device ID `0x450`, Rev V) connected to a router, host on the same `/24`:

| Check | Result |
| :--- | :--- |
| Boot, clocks | `sysclk=400 MHz`, `hclk=200 MHz`, REV_ID `0x2003` |
| Descriptors / buffers | `0x30040000`–`0x300448C0`, all 32-byte aligned |
| RMII pins | all nine read back MODER=AF, AF nibble `0xB` |
| PHY negotiation | `ANLPAR=0xC1E1` → 100 Mbps full duplex |
| ICMP echo | 5/5 and 60/60 × 1400 B, **0 % loss**, avg 2 ms |
| ARP | host resolves `192.168.1.150` → `02:00:00:00:00:01` |
| UDP echo, port 7 | payload returned byte-identical |
| Under load | `rx_nobuf=0 tx_nobuf=0 dma_err=0`, Rx CRC errors `0` |

Three bugs that only hardware could have found, all fixed:

- **The supply configuration has to be written before VOS.** Up to `2165309`, `SystemClock_Config()` never called `HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY)`. `PWR_CR3` is write-once per power-on reset, so after a power-up CPU1 waited forever for `VOSRDY` (no console, no link), and the image only ran when another one had written it since the last power-up. Fixed in `5dd43a2`; checked after a real power cycle with nothing else run since (2026-10-12): `RCC_RSR = 0x00FE0000`, `PWR_CR3 = 0x00010044`, and `net_check.py` with `monitor.py` at the same time pass (ICMP 4/4, UDP 1-1472 B byte-identical, burst 100/100, `rx_nobuf`/`tx_nobuf`/`dma_err` 0). Captures: `docs/x1_cold_boot_swd_5dd43a2.txt`, `docs/x1_cold_boot_t6_netcheck_5dd43a2.txt`.
- **The MPU region map must cover FLASH, DTCM, ITCM and the peripheral block.** The golden `mpu_config.c` in the docs hub programs region 0 as a 4 GB no-access/execute-never guard (ES0445 2.1.2) but carves out only the DMA SRAMs. Enabling that MPU raises `IACCVIOL` on the next instruction fetch plus `MSTKERR` on the exception push, which escalates straight to HardFault lockup before `SystemClock_Config()` ever runs. This project's copy defines the full nine-region map.
- **`ETH_MACFilterConfigTypeDef.BroadcastFilter` is inverted.** It drives `ETH_MACPFR` bit 5, `DBF` = *Disable Broadcast Packets*, so `ENABLE` blocks broadcast and silently makes the board unreachable. It must be `DISABLE`.

Two dependency details that are easy to trip over:

- **`cmsis_os2.h` is not in `stm32-mw-freertos`.** That repo ships only the FreeRTOS *implementation* of CMSIS-RTOS2 (`cmsis_os2.c`, `freertos_os2.h`). The API header itself lives in the CMSIS pack at `Drivers/CMSIS/RTOS2/Include/`, which is part of the STM32CubeH7 superproject rather than a submodule. A recursive clone of STM32CubeH7 provides both; cloning the individual middleware repos does not.
- **The HAL is the v2 ETH driver.** `ETH_TxPacketConfigTypeDef`, `HAL_ETH_ReadData()` and the `HAL_ETH_Rx{Allocate,Link}Callback` / `HAL_ETH_TxFreeCallback` hooks are required. Older HALs that still spell it `ETH_TxPacketConfig`, or that use `HAL_ETH_DescAssignMemory()`, will not build against `eth_hal.c` unchanged.

> **Register naming.** RM0399 and ES0445 spell two registers with an `X` that the CMSIS header omits: `ETH_MTLRXQOMR` is `ETH->MTLRQOMR`, and `ETH_DMACRXIWTR` is `ETH->DMACRIWTR`. `eth_hal.c` uses the CMSIS spelling in code and the manual's in comments.

```bash
STM32_Programmer_CLI -c port=SWD mode=UR -w build/debug/CM7/cm7_firmware.elf -rst
```

Console: 115200 8N1 on the ST-LINK virtual COM port. Follow it with `py tools/monitor.py`.

```text
=== STM32H755ZI CPU1 - Ethernet / lwIP node ===
[sys] sysclk=400000000 Hz hclk=200000000 Hz
[net] starting TCP/IP stack
[net] mac=02:00:00:00:00:01 phy_addr=0
[net] link up: 100 Mbps full duplex
[net] interface up, ip=192.168.1.150 netmask=255.255.255.0 gw=192.168.1.1
[net] udp echo listening on port 7
[net] link=100 Mbps full duplex ip=192.168.1.150 rx=12 tx=9 rx_nobuf=0 tx_nobuf=0 dma_err=0
```

---

## 7. Configuration

Everything tunable is in two files.

| Setting | File |
| :--- | :--- |
| IP address, netmask, gateway, DHCP on/off | `CM7/App/Inc/net_config.h` |
| Thread priorities and stacks, poll periods, UDP echo port | `CM7/App/Inc/net_config.h` |
| MAC address, PHY address, clock constants | `Common/Inc/board_def.h` |
| Checksum policy (`ETH_USE_HARDWARE_CHECKSUM`) | `CM7/CMakeLists.txt` — one definition, read by both `eth_hal.c` and `lwipopts.h` |

> **Do not "fix" `CHECKSUM_GEN_ICMP`.** Under hardware offload it must stay `0`, and that looks like a bug but is not. RM0399 requires the ICMP checksum field to be `0x0000` on the way in — "if it does not equal 0x0000, an incorrect checksum may be inserted" — and lwIP only writes `0x0000` there while `CHECKSUM_GEN_ICMP` is `0` (`icmp.c`, the `#else` branch of the echo-reply path). Setting it to `1` makes lwIP write a non-zero value that the MAC then overwrites incorrectly, and every echo reply is dropped by the host. Hardware offload on ⇒ software ICMP generation off; they are a matched pair. `lwipopts.h` carries the full note.
| Buffer and descriptor counts | `Custom_Drivers/08_eth/eth_hal.h`, `CM7/Core/Inc/stm32h7xx_hal_conf.h` |

---

## 8. LED diagnostics

`Custom_Drivers/01_led/led_bm.c` drives LD1/LD2/LD3 with direct register writes and no HAL, so it works before the MPU, before the clock tree and inside a fault handler — the three places where the console can tell you nothing.

### During start-up: a 3-bit boot code

Green = bit 0, yellow = bit 1, red = bit 2. **If the board freezes, the LEDs stop on the last stage that completed.**

| Code | LEDs | Last completed stage | A freeze here means |
| :---: | :--- | :--- | :--- |
| 1 | G | `LED_Init()` | `MPU_Config()` denied a region the CPU needs — instruction fetch or stack push faults, and the fault handler cannot run either |
| 2 | ·Y | `MPU_Config()` | I-cache or D-cache enable |
| 3 | GY | caches enabled | `HAL_Init()` — usually the TIM6 timebase |
| 4 | ··R | `HAL_Init()` | `SystemClock_Config()` — almost always HSE not starting, so check the ST-LINK MCO bridge to PH0 |
| 5 | G·R | clocks at 400 MHz | peripheral clock config, or `app_main()` before the console opens |
| 6 | ·YR | `app_main()` reached | task creation |
| 7 | GYR | handing over to FreeRTOS | the scheduler never started, or the net task died before its first loop |

Boot code 1 is what the MPU bug in section 6 looked like: green on, nothing else, forever.

### After the scheduler starts

**Green LD1** is the heartbeat: 1 Hz while the scheduler runs, stopped if it doesn't.

**Yellow LD2 and red LD3 are read together**, which makes all four states distinct:

| Y | R | Meaning |
| :---: | :---: | :--- |
| 1 | 0 | Link up, no faults — the healthy state |
| 0 | 1 | **No connection** — cable unplugged, or nothing at the far end |
| 1 | 1 | Link up, but a driver error is latched (`dma_errors`, `rx_alloc_failures` or `tx_alloc_failures`; the console line says which) |
| 0 | 0 | Transient only, between the two polls of a link change |

Link state follows the cable and never latches. Errors do latch, so a burst of buffer starvation overnight is still visible in the morning; only a reset clears it.

Solid red with **no heartbeat** is different again — that is a fault handler or `Error_Handler()`; all five fault handlers call `LED_PanicRed()`.
