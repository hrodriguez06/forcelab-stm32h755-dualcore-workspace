# Ethernet Bring-Up and Verification Guide

**Target**: NUCLEO-H755ZI-Q (STM32H755ZI), Cortex-M7 (CPU1), RMII + LAN8742A, lwIP 2.1.x on FreeRTOS.

Work through the stages in order. Each stage has a pass criterion; do not move on until it is met, because every later stage assumes the earlier ones hold. The order is deliberate: memory placement, then clocks, then the management interface, then the link, then traffic — that is also the order in which failures are cheapest to diagnose.

---

## 0. Prerequisites

### 0.1 Hardware

| Item | Requirement |
| :--- | :--- |
| Board | NUCLEO-H755ZI-Q, ST-LINK/V3 on-board |
| Cable | Straight Cat5e or better, into a switch or directly into the host PC |
| Host PC | An interface on `192.168.1.0/24`, for example `192.168.1.10/24` |
| Terminal | `py tools/monitor.py` - 115200 8N1 on the ST-LINK virtual COM port (USART3, PD8/PD9) |

Before powering up, confirm against **UM2408** (`00_docs_stm32h7/docs/02_board_hardware/UM2408_nucleo144_board_manual.pdf`) and the **MB1363 schematics** in the same folder that:

- the Ethernet solder bridges are in the default *Ethernet enabled* configuration (the LAN8742A and the RJ45 are populated on this board variant);
- the ST-LINK MCO is routed to `PH0`, because `SystemClock_Config()` runs HSE in **bypass** at 8 MHz.

If the MCO bridge is absent the PLL will not lock and the firmware will hang in `HAL_RCC_OscConfig()` long before Ethernet is reached.

### 0.2 Software

```bash
git submodule update --init --recursive
```

The build needs `external/STM32CubeH7` to provide the HAL, CMSIS, FreeRTOS and lwIP. CMake fails early with an explicit message if the submodule is missing.

```bash
cmake --preset debug
cmake --build --preset debug
```

Flash bank 1 only (CPU2 is not part of this bring-up):

```bash
STM32_Programmer_CLI -c port=SWD mode=UR -w build/debug/CM7/cm7_firmware.elf -rst
```

> **Option bytes.** If `BCM4` is set, CPU2 boots from Flash bank 2. With no CPU2 image programmed it will execute erased flash. Either program a CPU2 image, or clear `BCM4`. `CM4_BOOT_ENABLED` in `CM7/Core/Inc/main.h` is `0`, so CPU1 does not wait for CPU2 in either case.

### 0.3 Read the LEDs before reaching for a debugger

The user LEDs carry a boot code while the firmware starts, and status once it is running. They are driven bare-metal, so they still work when the MPU, the clock tree or the console do not. Full table in `README.md` section 8; the short version:

| What you see | Meaning |
| :--- | :--- |
| Green only, frozen | Died in `MPU_Config()` or on the first instruction after `HAL_MPU_Enable()` |
| Red only, frozen | Died in `HAL_Init()` |
| Green + red, frozen | Died in `SystemClock_Config()` — check the ST-LINK MCO bridge to PH0 |
| All three, frozen | The scheduler never started |
| Green blinking 1 Hz | Healthy; the RTOS is running |
| Yellow on, red off | Link up, no faults |
| **Yellow off, red on** | **No connection** — cable unplugged, or nothing at the far end |
| Yellow on *and* red on | Link up, but a driver counter went non-zero — read the console line |
| Red solid, green stopped | A fault handler or `Error_Handler()` |

Unplugging the cable is the quickest end-to-end check of the whole supervision path: within one `NET_LINK_POLL_PERIOD_MS` (100 ms) the yellow LED must drop and the red LED must light, the console must print `[net] link down`, and plugging back in must reverse both. The frame counters must not move while the link is down, and `rx_nobuf` / `tx_nobuf` / `dma_err` must all stay at 0 across the transition — that is what proves the stop / reconfigure / restart cycle does not leak descriptors.

**Nothing lit at all** means the firmware never reached the third instruction of `main()`: suspect the flash image, the option bytes, or power.

---

## 1. Stage 1 — Memory placement and the MPU

This is the stage that decides whether Ethernet can work at all. Descriptors in cacheable memory produce the classic symptom: the first few frames arrive, then reception stops forever.

### 1.1 Confirm the descriptors and buffers landed in D2 SRAM3

```bash
arm-none-eabi-nm --print-size --size-sort build/debug/CM7/cm7_firmware.elf | grep -i -E "g_rx_desc|g_tx_desc|g_rx_pool|g_tx_pool"
```

**Pass criterion**: every symbol has an address in `0x30040000 .. 0x30047FFF` and every address is a multiple of 32.

The same information, with section names, comes from the map file:

```bash
grep -n -A25 "\.eth_ram" build/debug/CM7/cm7_firmware.map
```

You should see `.RxDescripSection`, `.TxDescripSection`, `.RxArraySection` and `.eth_ram` all inside the `.eth_ram` output section, placed at `0x30040000`.

**Fail modes**

| Symptom | Cause |
| :--- | :--- |
| Symbols at `0x24000000` | The section attribute was dropped; check `__attribute__((section(...)))` spelling in `eth_hal.c`. The linker script matches `.RxDescripSection`, not `.RxDecripSection`. |
| Symbols at `0x20000000` | They fell through to `.bss` in DTCM, which the ETH DMA physically cannot reach. |
| Link error about `.eth_ram` overflow | The `_Static_assert` in `eth_hal.c` should have caught this first; reduce `ETH_DRIVER_RX_BUFFER_COUNT`. |

### 1.2 Confirm the MPU region is active and non-cacheable

Halt the target after `main()` has passed `MPU_Config()` and read the MPU registers. In GDB:

```gdb
set $r = 6
set *(uint32_t*)0xE000ED98 = $r        # MPU_RNR  <- region 6 (SRAM3)
x/1xw 0xE000ED9C                       # MPU_RBAR
x/1xw 0xE000EDA0                       # MPU_RASR
```

**Pass criterion** for region 6:

- `MPU_RBAR` base = `0x30040000`
- `MPU_RASR`: `ENABLE` = 1, `SIZE` = 14 (2^15 = 32 KB), `TEX` = 1, `C` = 0, `B` = 0, `S` = 1, `AP` = 0b011 (full access), `XN` = 1

`TEX=1, C=0, B=0` is *Normal, non-cacheable*. If you see `C=1` the D-Cache is caching the descriptors and reception will stall.

### 1.3 Confirm the ordering

Set a breakpoint on `SCB_EnableDCache` and check the call stack: `MPU_Config()` must already have returned. This is the `README.md` section 5.A rule 4, and it is the single most common way to get an intermittent Ethernet fault that "disappears when you add a printf".

---

## 2. Stage 2 — Clocks

### 2.1 System clock

On the console you should see, immediately after reset:

```
=== STM32H755ZI CPU1 - Ethernet / lwIP node ===
[sys] sysclk=400000000 Hz hclk=200000000 Hz
[sys] silicon revision id=0x2003
```

**Pass criterion**: `sysclk` = 400000000, `hclk` = 200000000. HCLK is the Ethernet MAC clock and also what `HAL_ETH_SetMDIOClockRange()` uses to pick the MDC divider.

> Revision id `0x1003` = rev Y, `0x2001` = rev X, `0x2003` = rev V. ES0445 2.3.5 rules out 480 MHz / VOS0 on revisions Y and W; this project runs 400 MHz / VOS1 and is safe on all of them.

### 2.2 RMII reference clock

The 50 MHz `ETH_REF_CLK` is generated by the **LAN8742A**, not by the MCU. Probe `PA1` (pin 30 of CN11, or the PHY side of the series resistor):

**Pass criterion**: a 50 MHz square wave, present as soon as the PHY is powered, regardless of whether a cable is plugged in.

No clock on PA1 means the PHY is held in reset, is not strapped for REFCLKO output, or its own 25 MHz crystal is not running. Nothing downstream can work: without REF_CLK the MAC's RMII state machine never advances, and both transmission and reception are silently dead even though MDIO still answers (MDIO runs on MDC, which the MAC generates itself).

---

## 3. Stage 3 — PHY and the management interface (SMI/MDIO)

MDIO works independently of REF_CLK and of the cable, so it is the first thing to verify after the clocks.

### 3.1 Identifier read-back

`ETH_Driver_Init()` fails with `ETH_DRIVER_PHY_ERROR` if the identifier does not match, and `netif_add()` then fails, which prints:

```
[net] FATAL: netif_add failed, check the MAC and the PHY
```

To see the raw values, break after `LAN8742_Init()` and inspect the instance:

```gdb
p/x *ETH_Driver_GetPhy()
```

**Pass criterion**: `id1 = 0x0007`, `id2 = 0xC130` or `0xC131` (the low nibble is the silicon revision and is masked out by the driver).

| Read-back | Meaning |
| :--- | :--- |
| `0xFFFF` on every register | No answer on MDIO. Check PA2 (MDIO) and PC1 (MDC) are AF11, check the MDIO pull-up, check the PHY address. |
| `0x0000` on every register | MDIO is stuck low, usually a short or a pin still configured as a GPIO output. |
| Plausible but different id | A different transceiver is fitted; the register map in `lan8742_phy.h` may not apply. |

### 3.2 Live register access

Both accessors are exported so a debugger can drive them. To read the Basic Status Register on demand:

```gdb
set $bsr = 0
call LAN8742_ReadRegister(ETH_Driver_GetPhy(), 0x01, &$bsr)
p/x $bsr
```

Useful registers:

| Reg | Name | What to look at |
| :--- | :--- | :--- |
| `0x00` | BCR | bit 12 `AUTONEG_EN` should be 1, bit 11 `POWER_DOWN` and bit 10 `ISOLATE` should be 0 |
| `0x01` | BSR | bit 2 `LINK_STATUS`, bit 5 `AUTONEG_COMPLETE` |
| `0x1F` | PHYSCSR | bits 4:2 negotiated speed and duplex |

> **BSR bit 2 latches low.** A single read reports "down" if the link dropped at any point since the previous read. `LAN8742_GetLinkState()` reads it twice for this reason, and so should you when poking by hand.

---

## 4. Stage 4 — Link, speed and duplex

Plug the cable in. Within a second or two the console prints:

```
[net] link up: 100 Mbps full duplex
[net] interface up, ip=192.168.1.150 netmask=255.255.255.0 gw=192.168.1.1
```

**Pass criterion**: the link line appears, and the speed and duplex match what the switch reports on its own port.

### 4.1 Where the negotiated mode comes from

`ethernetif_link_thread()` polls every `NET_LINK_POLL_PERIOD_MS` (100 ms). On a change it:

1. stops the MAC (speed and duplex can only be programmed while stopped);
2. calls `ETH_Driver_ConfigureLink()`, which writes `ETH_MACCR.FES` and `ETH_MACCR.DM`;
3. re-applies the FEP errata bit and restarts the MAC;
4. raises the lwIP link flag under the TCP/IP core lock.

### 4.2 Confirm the MAC agrees with the PHY

```gdb
x/1xw 0x40028000        # ETH_MACCR
```

| Bit | Name | 100 Mbps full duplex |
| :--- | :--- | :--- |
| 14 | `FES` | 1 (100 Mbps) |
| 13 | `DM` | 1 (full duplex) |
| 1 | `TE` | 1 (transmitter enabled) |
| 0 | `RE` | 1 (receiver enabled) |

A PHY that says 100/full while the MAC says 10/half is the signature of a link thread that is not running, or of `ETH_Driver_ConfigureLink()` returning an error that was ignored.

### 4.3 Force 10 Mbps once

This is worth doing deliberately, because it is the case ES0445 2.25.10 is about. Either configure the switch port to 10 Mbps, or write BCR with auto-negotiation off and `SPEED_SELECT` clear:

```gdb
call LAN8742_WriteRegister(ETH_Driver_GetPhy(), 0x00, 0x0100)   # 10 Mbps, full duplex, autoneg off
```

**Pass criterion**: `[net] link up: 10 Mbps full duplex`, and ping (stage 5) still answers with no loss. Packet loss that appears only at 10 Mbps means the FEP workaround is not in effect — verify it in stage 6.

Restore auto-negotiation afterwards:

```gdb
call LAN8742_RestartAutoNegotiation(ETH_Driver_GetPhy())
```

---

## 5. Stage 5 — ARP and ICMP echo

### 5.1 Host configuration

Give the host an address on the same subnet.

Linux:

```bash
sudo ip addr add 192.168.1.10/24 dev eth0
sudo ip link set eth0 up
```

Windows (elevated PowerShell):

```powershell
New-NetIPAddress -InterfaceAlias "Ethernet" -IPAddress 192.168.1.10 -PrefixLength 24
```

### 5.2 Ping

```bash
ping 192.168.1.150
```

Windows:

```powershell
ping -t 192.168.1.150
```

**Pass criterion**: replies with no loss. Round-trip time on a direct 100 Mbps link is typically well under 1 ms.

### 5.3 Confirm ARP resolved in software

ES0445 2.25.11 says the MAC's hardware ARP responder is not effective, so `eth_hal.c` clears `ETH_MACCR.ARPEN` and lwIP answers ARP itself. Check the host's cache:

```bash
arp -a | grep 192.168.1.150      # Linux / macOS
arp -a  | findstr 192.168.1.150  # Windows
```

**Pass criterion**: the entry shows `02:00:00:00:00:01`, the address from `board_def.h`.

### 5.4 Read the counters

The firmware prints a status line every five seconds:

```
[net] link=100 Mbps full duplex ip=192.168.1.150 rx=412 tx=409 rx_nobuf=0 tx_nobuf=0 dma_err=0
```

**Pass criterion**: `rx` and `tx` both climb while pinging; `rx_nobuf`, `tx_nobuf` and `dma_err` stay at 0.

| Counter rising | Meaning | Action |
| :--- | :--- | :--- |
| `rx_nobuf` | The Rx pool was empty when the DMA needed a buffer | The input thread is falling behind: raise `ETH_DRIVER_RX_BUFFER_COUNT` or `NET_RX_THREAD_PRIO` |
| `tx_nobuf` | All Tx buffers were still owned by the DMA | Raise `ETH_DRIVER_TX_BUFFER_COUNT`, or check that transmit-complete interrupts are arriving |
| `dma_err` | `HAL_ETH_ErrorCallback()` fired | Read `HAL_ETH_GetDMAError()`; a bus error here usually means a descriptor address outside D2 SRAM |

### 5.5 When ping does not answer

Work down this list; it is ordered by how often each one is the cause.

1. **`rx` stays at 0, everything else looks healthy.** Check the broadcast filter first — this one has actually bitten this project. Read `ETH_MACPFR`:

   ```gdb
   p/x (*(uint32_t*)0x40028008 >> 5) & 1     # DBF, must be 0
   ```

   Bit 5 is `DBF`, *Disable Broadcast Packets*, and the HAL field that drives it is called `BroadcastFilter` — so `filter.BroadcastFilter = ENABLE` **blocks** broadcast rather than admitting it. With broadcast blocked the board never sees an ARP request, never answers one, and is unreachable, while the link, the descriptors, the MMC counters and every other register still look perfectly healthy. `eth_hal.c` sets it to `DISABLE`.

2. **`rx` still 0 with `DBF` clear.** Now suspect the physical receive path: re-check stage 2.2 (REF_CLK on PA1) and that the RXD0/RXD1/CRS_DV pins (PC4, PC5, PA7) are AF11. A useful discriminator: if `ETH_TX_PACKET_COUNT_GOOD` (`0x40028768`) is incrementing, REF_CLK is present and the fault is receive-specific, because RMII clocks both directions from PA1.
2. **`rx` climbs but `tx` stays at 0.** Reception works, so the MAC and the clocks are fine. lwIP is not replying: check the host and the target are on the same subnet and that the netmask printed by the firmware is right.
3. **`rx` and `tx` both climb, no reply at the host.** Put a sniffer on it (below). Most often the reply leaves with a bad checksum, so verify that `ETH_USE_HARDWARE_CHECKSUM` in `CM7/CMakeLists.txt` matches what the MAC is actually doing.
4. **Reception stops after a handful of frames.** This is the cache-coherency signature. Go back to stage 1.2.
5. **Everything works at 100 Mbps and loses packets at 10 Mbps.** FEP is not set; see stage 6.

### 5.6 Sniffing

Run Wireshark on the host interface with the filter:

```
eth.addr == 02:00:00:00:00:01
```

What you should see for one ping: host ARP request (broadcast), target ARP reply, host ICMP echo request, target ICMP echo reply. Seeing the request but no reply narrows the fault to the transmit path; seeing neither narrows it to the receive path or to the link.

---

## 6. Stage 6 — Verify the errata workarounds are in effect

All three are applied by `ETH_Driver_Init()`, and `FEP` is re-applied by `ETH_Driver_Start()`. Read them back on the live target.

> **Register naming.** RM0399 and ES0445 spell two of these with an `X` that the CMSIS header omits — `ETH_MTLRXQOMR` is `ETH->MTLRQOMR`, `ETH_DMACRXIWTR` is `ETH->DMACRIWTR`. Same registers, same addresses; only the C identifier differs. The absolute addresses in the table below were confirmed against the linked image's disassembly.

| Errata | Register | Expression | Expected |
| :--- | :--- | :--- | :--- |
| 2.25.10 | `ETH_MTLRXQOMR` | `p/x (*(uint32_t*)0x40028D30 >> 4) & 1` | `1` (FEP set) |
| 2.25.11 | `ETH_MACCR` | `p/x (*(uint32_t*)0x40028000 >> 31) & 1` | `0` (ARPEN clear) |
| 2.25.8 | `ETH_DMACRXIWTR` | `x/1xw 0x40029138` | `0x00000000` with the watchdog off |
| 2.3.11 | `SYSCFG_PMCR` | `p/x (*(uint32_t*)0x58000404 >> 21) & 7` | `4` (RMII selected) |

For 2.25.8 the rule is about the *write order*, not the final value. `ETH_Driver_SetRxWatchdog()` performs two separate writes — `RWT[7:0]` first with `RWTU[1:0]` held at zero, then `RWTU[1:0]` alone — and refuses to set a non-zero `RWTU` while `RWT` is zero, which is the forbidden state. To exercise it:

```gdb
call ETH_Driver_SetRxWatchdog(8, 1)
x/1xw 0x40029138                      # expect 0x00010008
call ETH_Driver_SetRxWatchdog(0, 0)   # back off, both fields cleared together
```

> **Which register.** `RWT[7:0]` and `RWTU[1:0]` live in **`ETH_DMACRXIWTR`** (Channel Rx interrupt watchdog timer) at offset `0x1138`, as named by ES0445 rev 6 section 2.25.8 and by RM0399. `ETH_MACWTR` at offset `0x000C` is a different register — `PWE` and `WTO[3:0]`, which bound the accepted length of a received packet — and has nothing to do with this erratum. The two are easy to transpose when reading the errata summary.

---

## 7. Stage 7 — Beyond ping

ICMP proves the link, the MAC, the DMA and the IP layer. It does not prove the UDP or TCP path, because lwIP answers ping from inside the stack with no application involvement.

The firmware registers a UDP echo service on port 7:

```
[net] udp echo listening on port 7
```

Linux / macOS:

```bash
echo "stm32h755" | nc -u -w1 192.168.1.150 7
```

Windows (PowerShell):

```powershell
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Connect("192.168.1.150", 7)
$bytes = [Text.Encoding]::ASCII.GetBytes("stm32h755")
$udp.Send($bytes, $bytes.Length) | Out-Null
$remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
[Text.Encoding]::ASCII.GetString($udp.Receive([ref]$remote))
```

**Pass criterion**: the same payload comes back.

### 7.1 Sustained traffic

Leave a flood ping running for a few minutes and watch the counters:

```bash
sudo ping -f -s 1400 192.168.1.150
```

**Pass criterion**: no loss, and `rx_nobuf` / `tx_nobuf` / `dma_err` all still 0. A slow leak in the buffer pools shows up here and nowhere else.

---

## 8. Switching to DHCP

Set `NET_USE_DHCP` to `1` in `CM7/App/Inc/net_config.h` and rebuild. The task requests a lease, waits `NET_DHCP_TIMEOUT_S` seconds and falls back to the static address if none arrives. The address in use is always printed by the interface status callback, so the console tells you which path was taken.

---

## 9. Moving to a zero-copy data path

The driver copies each frame between lwIP pbufs and its own buffers in `.eth_ram`. That is one `memcpy` of up to ~1.5 kB per frame per direction, which at 100 Mbps costs a low single-digit percentage of a 400 MHz M7 — invisible for control traffic, and worth removing for sustained bulk transfer.

To go zero-copy, lwIP's pbuf payloads must themselves live in `.eth_ram`:

1. Enable `LWIP_SUPPORT_CUSTOM_PBUF` and declare a custom pool whose backing memory is placed in `.RxArraySection`.
2. Change `HAL_ETH_RxAllocateCallback()` to hand out that pool's payloads and `HAL_ETH_RxLinkCallback()` to chain `pbuf_custom` structures instead of `eth_rx_segment_t`.
3. In `low_level_output()`, build the `ETH_BufferTypeDef` chain directly from the pbuf chain instead of calling `pbuf_copy_partial()`, and free the pbuf in `HAL_ETH_TxFreeCallback()` rather than returning a slot to the pool.

Budget the pools first: SRAM3 is 32 KB total and the current layout already uses about 19 KB. A zero-copy Rx pool large enough to absorb burst traffic may not fit alongside the Tx buffers, in which case move the Tx side to `.dma_buf` in SRAM1, which is equally non-cacheable and equally reachable by the ETH DMA.

---

## 10. Quick reference

### Pin map (all AF11)

| Signal | Pin |
| :--- | :--- |
| `ETH_REF_CLK` | `PA1` |
| `ETH_MDIO` | `PA2` |
| `ETH_MDC` | `PC1` |
| `ETH_CRS_DV` | `PA7` |
| `ETH_RXD0` | `PC4` |
| `ETH_RXD1` | `PC5` |
| `ETH_TX_EN` | `PG11` |
| `ETH_TXD0` | `PG13` |
| `ETH_TXD1` | `PB13` |

### Register addresses used above

| Register | Address |
| :--- | :--- |
| `ETH_MACCR` | `0x40028000` |
| `ETH_MTLRXQOMR` | `0x40028D30` |
| `ETH_DMACRXIWTR` | `0x40029138` |
| `SYSCFG_PMCR` | `0x58000404` |
| `MPU_RNR` / `RBAR` / `RASR` | `0xE000ED98` / `0xE000ED9C` / `0xE000EDA0` |

### Files that matter

| Concern | File |
| :--- | :--- |
| MPU regions, cache attributes | `CM7/App/Src/mpu_config.c` |
| Memory sections | `CM7/linker/stm32h755zi_cm7.ld` |
| MAC, RMII pins, errata, buffer pools | `Custom_Drivers/08_eth/eth_hal.c` |
| PHY, link detection | `Custom_Drivers/08_eth/lan8742_phy.c` |
| lwIP glue, Rx thread, link thread | `CM7/App/Src/ethernetif.c` |
| Stack start-up, addressing, services | `CM7/App/Src/net_task.c` |
| Addresses, threading, timeouts | `CM7/App/Inc/net_config.h` |
