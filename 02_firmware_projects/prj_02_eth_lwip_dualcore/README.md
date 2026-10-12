# prj_02_eth_lwip_dualcore

Dual-Core STM32H755ZI firmware product with 10/100 Ethernet (RMII + LAN8742 PHY) and lwIP TCP/IP stack running on Cortex-M7 with FreeRTOS, and deterministic Bare-Metal running on Cortex-M4.

Built according to the workspace **Plan A 3-Tier Architecture** and consuming centralized drivers from `01_shared_drivers/`.

---

## 1. Core Architecture & System Roles

| Core | Image / Placement | Operating Model | Peripheral & Memory Ownership |
| :--- | :--- | :--- | :--- |
| **CPU1 (Cortex-M7)** | `cm7_firmware.elf`<br>Flash Bank 1 (`0x08000000`) | FreeRTOS (Priority grouping 4) | - Owns power (`PWR`), system clocks (`RCC`, PLL1 @ 400 MHz)<br>- Owns console VCP (`USART3` via `04_uart`)<br>- Owns Ethernet MAC & DMA (`08_eth` via RMII)<br>- Owns LAN8742A PHY transceiver (`08_eth` via SMI/MDIO)<br>- Runs lwIP TCP/IP stack (IPv4, ICMP, DHCP/Static IP)<br>- Descriptors in D2 SRAM3 (`0x30040000`, non-cacheable)<br>- Releases CPU2 via HSEM 0 |
| **CPU2 (Cortex-M4)** | `cm4_firmware.elf`<br>Flash Bank 2 (`0x08100000`) | Bare-Metal (No RTOS) | - Deterministic super-loop in D2 SRAM2 (`0x30020000`)<br>- Waits for CPU1 boot signal via HSEM 0<br>- Inter-core telemetry counters in SRAM4 (`0x38008000`) |

---

## 2. Shared Drivers Integration (`01_shared_drivers`)

This project does not vendor local peripheral drivers in `Custom_Drivers/`. Instead, both cores link against the central `stm32h7_shared_drivers` INTERFACE library:

- **`01_led` (`led_bm`)**: Board status and 3-bit diagnostic codes (zero overhead, bare-metal).
- **`04_uart` (`uart_hal`)**: Thread-safe VCP logging console on USART3 with line buffering and FreeRTOS mutex (guarded by `HAL_UART_MODULE_ENABLED`).
- **`08_eth` (`eth_hal`, `lan8742_phy`)**: Ethernet MAC + RMII + DMA driver with ES0445 silicon errata mitigations (FEP, Rx watchdog, ARP offload) and Microchip LAN8742A PHY transceiver driver (guarded by `HAL_ETH_MODULE_ENABLED`).

---

## 3. Network Configuration (`CM7/App/Inc/net_config.h`)

| Parameter | Default Value | Notes |
| :--- | :--- | :--- |
| **DHCP** | Disabled (`NET_USE_DHCP 0`) | Set to 1 to ask a server and fall back to the static address after `NET_DHCP_TIMEOUT_S` |
| **Static IP** | `192.168.1.150` | In use, since DHCP is off. Verified on the board in section 6 |
| **Netmask** | `255.255.255.0` | `/24` subnet |
| **Gateway** | `192.168.1.1` | Default route |
| **PHY Address** | `0` | Nucleo-H755ZI-Q on-board LAN8742A address |

---

## 4. Boot Handshake & Dual-Core IPC

From `00_docs_stm32h7/docs/00_core_and_system/dual_core_boot_ipc.md`:

1. Both cores leave reset simultaneously.
2. CPU2 subscribes to HSEM channel 0 and enters `STOP` mode on domain D2 (`SEVONPEND` enabled).
3. CPU1 detects `D2CKRDY` drop, initializes MPU regions (including SRAM3 for ETH and SRAM4 for IPC), clocks, and peripheral buses.
4. CPU1 clears the shared block in SRAM4 (`0x38008000`), initializes `Shared_Init()` and publishes `SHARED_MAGIC`.
5. CPU1 takes and releases HSEM 0 to wake CPU2.
6. CPU1 starts FreeRTOS scheduler; CPU2 enters its bare-metal loop.

Step 4 happens in `main()`, before step 5, so the block is published before
CPU2 can touch it. `app_main()` then refuses to create any task unless
`g_shared.magic == SHARED_MAGIC`, which turns a broken handshake into a stop
at `Error_Handler()` instead of a system running on uninitialised IPC state.

---

## 5. Build and Flash Instructions

### Build with CMake + Ninja

From this project directory:

```powershell
# 1. Load toolchain into environment (STM32CubeIDE / CubeCLT)
.\tools\env.ps1

# 2. Configure build
cmake --preset debug

# 3. Compile both CM7 and CM4 simultaneously
cmake --build --preset debug
```

### Output Binaries

- `build/debug/CM7/cm7_firmware.elf` / `.bin` (Flash Bank 1: `0x08000000`)
- `build/debug/CM4/cm4_firmware.elf` / `.bin` (Flash Bank 2: `0x08100000`)

### Flash Target (Nucleo-H755ZI-Q)

```powershell
# Flash both cores using STM32CubeProgrammer
py tools/flash.py --tool cubeprog

# Monitor telemetric logs via ST-LINK VCP console
py tools/monitor.py
```

---

## 6. Hardware Verification

Last run: **2026-09-20**, firmware at `68ec30a` - unchanged since, as the
commits after it touch only `tools/` and documentation - on a
NUCLEO-H755ZI-Q (silicon revision `0x2003` = rev V), ST-LINK V3 under reset,
host on the same 192.168.1.0/24 segment.

Re-run it with the commands in section 5; every number below comes from that
run and should be refreshed whenever this section is.

### Data-path re-run, 2026-10-12 (`08_eth` close)

Firmware at `2165309`, the close of `08_eth` (mandatory `board_def.h` macros,
atomic counters, RMII pins through `gpio_hal`), same board and host. Only the
data path was re-run; the build, boot and IPC records below are still those of
`68ec30a`.

| Check | Result |
| :--- | :--- |
| Link | 100 Mbps full duplex |
| ICMP | 4/4 |
| UDP echo, port 7 | 1, 25, 512, 1024 and 1472 B byte-identical, RTT 1.26-2.22 ms |
| UDP burst | 100/100 |
| Firmware counters | `rx`/`tx` 10/1 -> 122/112; `rx_nobuf`, `tx_nobuf`, `dma_err` 0 throughout |

Capture: `01_shared_drivers/08_eth/docs/eth_close_audit_t6_netcheck_2165309_3.txt`
(`net_check.py` and `monitor.py` at the same time).

**Cold boot, fixed in `5dd43a2`.** Up to `2165309` this image never wrote the
supply configuration (`HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY)`), which
the H755 accepts once per power-on reset. After a power-up, CPU1 waited forever
for `VOSRDY` in `SystemClock_Config()` (`CM7/Core/Src/main.c`): no console, no
link, `PWR_CR3` still at its reset value `0x46`. It only ran when another image
that writes it (`prj_01`, `prj_04`) had run since the last power-up; a warm reset
or a reflash keeps it. The `2165309` run above was made that way (`prj_01`
flashed first, no power cycle). `5dd43a2` writes it first thing in
`SystemClock_Config()`, as `prj_01` does. Checked after a real power cycle with
nothing else run since (2026-10-12): `RCC_RSR = 0x00FE0000` (power-on),
`PWR_CR3 = 0x00010044` (direct SMPS, written by this image), and the same
data-path checks pass (ICMP 4/4, UDP 1-1472 B byte-identical, burst 100/100,
`rx_nobuf`/`tx_nobuf`/`dma_err` 0). Captures:
`docs/x1_cold_boot_swd_5dd43a2.txt`, `docs/x1_cold_boot_t6_netcheck_5dd43a2.txt`.

### Build

Clean configure and build of both cores: **0 warnings, 0 errors**.

| Region | CPU1 (M7) | CPU2 (M4) |
| :--- | ---: | ---: |
| FLASH | 85 568 B / 1 MB (8.16 %) | 2 604 B / 1 MB (0.25 %) |
| DTCM | 101 440 B / 128 KB (77.39 %) | - |
| SRAM2 (D2) | - | 12 344 B / 128 KB (9.42 %) |
| SRAM3 `.eth_ram` | 18 624 B / 32 KB (56.84 %) | - |
| SRAM4 `.shared_ram` | 160 B / 32 KB | 160 B / 32 KB |

Both images reserve the same 160 B at `0x38008000`, which is the shared block
agreeing across the two linker scripts.

### Boot

```text
=== STM32H755ZI dual-core Ethernet / lwIP - CPU1 (Cortex-M7) ===
[sys] sysclk=400000000 Hz hclk=200000000 Hz
[sys] silicon revision id=0x2003
[ipc] shared block at 0x38008000, magic=0x48375353
[ipc] CPU2 boot handshake: enabled (HSEM 0)
[ipc] m7=1 m4=1  (CPU2 alive)
```

`magic` reads back as `SHARED_MAGIC`, so the publish in `main()` landed before
CPU2 was released and the check in `app_main()` let the tasks start.

### Inter-core IPC

```text
[ipc] m7=1 m4=1  (CPU2 alive)
```

`m4_heartbeat` from there, sampled every 2 s:

```text
1 -> 16 -> 32 -> 47 -> 62 -> 78 -> 93 -> 109 -> 124 -> 139 -> 155 -> 170
```

Strictly monotonic from 1. The first line reads `alive`, not `starting`: CPU2
has already incremented by the time this firmware reaches `Status_Task`, since
the banner, the magic check and lwIP init give it the time. The template, which
is leaner before the scheduler, loses that race and reports `starting` there -
the label only means "no earlier sample to compare against", never a verdict.

A counter that climbs and then drops back near zero is the signature of
`Shared_Init()` running after the HSEM release and wiping what CPU2 already
wrote; this ordering is what section 4 step 4 exists to prevent.

### Ethernet

```text
[phy] bcr=1100 bsr=782D anar=01E1 anlpar=45E1 physcsr=1058
[phy] partner advertised: 100BASE-TX-FD 100BASE-TX-HD 10BASE-T-FD 10BASE-T-HD
[net] link up: 100 Mbps full duplex
```

Autonegotiation reaches the LAN8742A over MDIO, which is one of the nine RMII
signals `08_eth/eth_hal.c` brings up pin by pin from `BOARD_ETH_*` - so a clean
link is also the check that no signal is being configured on the wrong port.

The counters stay flat while nothing talks to the board, so the receive path
has to be driven from the host.

**ICMP.** `ping 192.168.1.150`, 4 packets: 4/4, 0 % loss. The first reply
takes ~100 ms because the host still has to ARP for the station address; the
rest are 1 ms.

**UDP echo on port 7, by datagram size.** The sizes matter more than the
count: 1472 B is the largest payload that still fits one frame
(1500 MTU - 20 IP - 8 UDP), and it is what actually exercises the 1536 B
buffers in `.eth_ram`. A pool mis-sized or mis-aligned shows up there, not in
small packets.

| Payload | Result | RTT |
| ---: | :--- | ---: |
| 1 B | byte-identical | 1.37 ms |
| 25 B | byte-identical | 1.14 ms |
| 512 B | byte-identical | 1.55 ms |
| 1024 B | byte-identical | 1.57 ms |
| 1472 B | byte-identical | 1.80 ms |

**UDP echo under burst.** 100 datagrams back to back with no pause:
**100 returned, 0 lost**. This is the one that stresses buffer recycling -
the Rx pool is 8 buffers and the Tx pool 4, so they have to turn over faster
than frames arrive. A leak in `ETH_Driver_ReclaimTxBuffers()` or in the Rx
segment release would surface here as losses or as a non-zero `tx_nobuf`.

**What the firmware counted.** `rx` / `tx` went from `1` / `1` before the
tests to **`133` / `113`** after, with `rx_nobuf`, `tx_nobuf` and `dma_err`
all still **0** across 105 consecutive round trips. `rx` runs ahead of what
the host sent because the MAC counts everything it accepts, broadcast and
other stations' ARP included.

That covers the data path in both directions, across the whole size range and
under buffer pressure: descriptors in non-cacheable SRAM3, the nine RMII
signals, and the recycling of both pools.

All of it is one command, and it reads the address out of `net_config.h`:

```powershell
py tools/net_check.py                    # ping, size sweep, burst of 100
py tools/net_check.py --burst 500        # lean harder on buffer recycling
py tools/net_check.py --host 10.0.0.42   # when the board took a DHCP lease
```

It exits non-zero when a check fails, so it works in a script. It only drives
the host side, though: pair it with `py tools/monitor.py --timeout 15` and
confirm `rx_nobuf`, `tx_nobuf` and `dma_err` are still `0` there. Both ends
agreeing is what rules out the host having talked to something else on the
segment.

### Not covered here

The user LEDs (LD1 heartbeat, LD2 link, LD3 fault) are not part of this capture:
they need someone looking at the board. Check them by eye when the console
output above is already good.
