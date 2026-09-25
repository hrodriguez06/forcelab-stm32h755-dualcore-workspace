# STM32H755 Shared Ethernet Driver (`01_shared_drivers/08_eth`)

Ethernet MAC + RMII + LAN8742A PHY driver for the STM32H755ZI, **Cortex-M7
(CPU1) only**, HAL tier only. It owns the board wiring, the placement of every
byte the ETH DMA touches and the ES0445 workarounds; the MAC state machine, the
MDIO controller and the descriptor rings are ST's HAL. The TCP/IP stack
(lwIP glue, threads, addressing) belongs to each project, not to this driver.

Status: the data path was validated on a NUCLEO-H755ZI-Q (silicon Rev V,
`REV_ID = 0x2003`) on 2026-09-20 through `prj_02_eth_lwip_dualcore`: ICMP, UDP
echo from 1 B to 1472 B, and a 100-datagram burst with no loss and no pool
exhaustion. The driver sources have not changed since that run. What it covers,
and what it does not, is in [section 9](#9-validation).

---

## 1. Tier and files

| File | Role |
| :--- | :--- |
| `eth_hal.[c\|h]` | MAC, RMII pins, SRAM3 buffer pools, Rx/Tx paths, HAL buffer hooks, ES0445 workarounds, diagnostics |
| `lan8742_phy.[c\|h]` | LAN8742A over SMI/MDIO: identifier check, soft reset, auto-negotiation, link state |

| CMake target | Links | ST dependency | Guard |
| :--- | :--- | :--- | :--- |
| `stm32h7_driver_eth` | `stm32h7_driver_gpio` | ST HAL (`HAL_ETH_*`, `HAL_GPIO_*`, `HAL_SYSCFG_*`) | `HAL_ETH_MODULE_ENABLED` |

```cmake
target_link_libraries(cm7_firmware PRIVATE stm32h7_driver_eth)   # or the stm32h7_shared_drivers umbrella
```

```c
#include "eth_hal.h"    /* never "08_eth/eth_hal.h": the directory is on the include path */
```

- **There is no BM or LL tier, on purpose.** Ethernet under lwIP is a "complex
  stack" in the workspace driver selection matrix (root `README.md` section 10):
  descriptor rings, MDIO, and the MAC/MTL/DMA reset sequence are what the HAL
  is for, and no consumer needs them without an RTOS.
- **Without `HAL_ETH_MODULE_ENABLED`** both `.c` files compile to nothing, so a
  project can link the `stm32h7_shared_drivers` umbrella without paying for
  Ethernet. Every CM4 `stm32h7xx_hal_conf.h` in the workspace leaves it off.
- **It does not link `02_dma`.** The Ethernet peripheral has its own DMA engine,
  which walks descriptors in memory; DMA1/DMA2/BDMA and DMAMUX are not involved.

---

## 2. API summary

### 2.1 `eth_hal.h`

| Function | What it does |
| :--- | :--- |
| `ETH_Driver_Init(mac, phy_addr)` | RMII select, pins, clocks, NVIC (through the MSP), `HAL_ETH_Init()`, MDC divider, errata, checksum policy, MAC filter, then `LAN8742_Init()`. Leaves the MAC **stopped**. |
| `ETH_Driver_Start()` | Re-applies FEP (2.25.10), then `HAL_ETH_Start_IT()` |
| `ETH_Driver_Stop()` | `HAL_ETH_Stop_IT()`, then recovers the Tx buffers the stopped DMA will never finish ([section 5.3](#53-stop-and-the-stranded-tx-descriptors)) |
| `ETH_Driver_ConfigureLink(state)` | Writes speed and duplex from a `lan8742_link_state_t`. MAC must be stopped. |
| `ETH_Driver_SetEventCallbacks(cb)` | Installs `on_rx_complete`, `on_tx_complete`, `on_error` (ISR context). `NULL` removes them. |
| `ETH_Driver_TxBufferAcquire()` / `...Release()` | Take / give back a 1536 B Tx buffer from the SRAM3 pool |
| `ETH_Driver_Transmit(buf, len)` | Hands a filled pool buffer to the DMA, returns immediately |
| `ETH_Driver_ReclaimTxBuffers()` | Returns finished Tx buffers to the pool. Call after `on_tx_complete`. |
| `ETH_Driver_ReadPacket(&seg)` | Pops one complete frame as an `eth_rx_segment_t` chain, or `ETH_DRIVER_NO_PACKET` |
| `ETH_Driver_ReleasePacket(seg)` | Returns a whole chain to the Rx pool |
| `ETH_Driver_GetHandle()` / `GetPhy()` / `GetStats()` | HAL handle (for `ETH_IRQHandler`), PHY instance, counters |
| `ETH_Driver_ApplyErrataRxErrorPackets()` | ES0445 2.25.10, exposed for a manual MAC reconfiguration |
| `ETH_Driver_SetRxWatchdog(rwt, rwtu)` | ES0445 2.25.8, the mandated two-write sequence |
| `ETH_HAL_MspInit_User()` / `MspDeInit_User()` | `__weak` MSP hooks ([section 3.2](#32-overriding-the-msp)) |

Status codes: `ETH_DRIVER_OK`, `_ERROR`, `_PARAM`, `_NO_BUFFER`, `_NO_PACKET`,
`_PHY_ERROR`.

### 2.2 `lan8742_phy.h`

| Function | What it does |
| :--- | :--- |
| `LAN8742_Init(obj, heth, addr)` | Reads PHYI1R/PHYI2R (the check is also the MDIO bus check), soft reset with a 500 ms bound, enables and restarts auto-negotiation. `HAL_ETH_Init()` must have run. |
| `LAN8742_GetLinkState(obj)` | BSR read **twice** (link status latches low), then PHYSCSR for speed and duplex |
| `LAN8742_RestartAutoNegotiation(obj)` | After a cable swap or a forced mode |
| `LAN8742_ReadRegister()` / `WriteRegister()` | Raw clause-22 access, callable from a debugger |
| `LAN8742_LinkStateToString(state)` | For logs |

Accepted identifier: `PHYI1R = 0x0007`, `PHYI2R & 0xFFF0 = 0xC130` (any silicon
revision). Anything else is `LAN8742_ERROR_ID`; `0xFFFF` everywhere means no
answer on MDIO.

---

## 3. Project contract

Everything below has to be provided by the consuming project. The driver does
not create any of it.

| Where | What | Notes |
| :--- | :--- | :--- |
| `board_def.h` | `BOARD_ETH_<SIG>_PORT` / `_PIN` for the nine RMII signals: `REF_CLK`, `MDIO`, `MDC`, `CRS_DV`, `RXD0`, `RXD1`, `TX_EN`, `TXD0`, `TXD1` | All AF11. See [section 10](#10-known-limitations) E1 for what happens if they are missing. |
| `board_def.h` | `BOARD_ETH_PHY_ADDRESS`, `BOARD_ETH_MAC_ADDR0..5` | Read by the project and passed to `ETH_Driver_Init()`; the driver never reads them itself |
| `stm32h7xx_hal_conf.h` | `HAL_ETH_MODULE_ENABLED`, `USE_HAL_ETH_REGISTER_CALLBACKS 0U` | The second one is load-bearing ([section 10](#10-known-limitations) E2) |
| `stm32h7xx_hal_conf.h` | `ETH_RX_DESC_CNT`, `ETH_TX_DESC_CNT` | HAL default 4 each. The Rx pool must stay larger than `ETH_RX_DESC_CNT` (`_Static_assert`). |
| CM7 linker script | `.eth_ram (NOLOAD)` output section collecting `.RxDescripSection`, `.TxDescripSection`, `.RxArraySection`, `.eth_ram*`, placed in D2 SRAM3 (`RAM_D2_ETH`, `0x30040000`, 32 KB) | Every project in the workspace uses the same block |
| `mpu_config.c` | A region over `0x30040000`, 32 KB: Normal, **non-cacheable**, shareable, XN (TEX=1, C=0, B=0, S=1) | Region 6 in every project. Must be in place before `SCB_EnableDCache()`. |
| `stm32h7xx_it.c` | `void ETH_IRQHandler(void) { HAL_ETH_IRQHandler(ETH_Driver_GetHandle()); }` | Not provided by the driver. Losing it (as happened once, `129c96c`) leaves the MAC running with no Rx/Tx completion. |
| CM7 `CMakeLists.txt` | `ETH_USE_HARDWARE_CHECKSUM=0` or `1` | Must match the `CHECKSUM_*` options in `lwipopts.h`, which reads the same macro. The header default is 0. |
| Build (optional) | `ETH_DRIVER_IRQ_PRIORITY` | Default 6: the ISR signals FreeRTOS objects, so it must be numerically ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY` (5), with `NVIC_PRIORITYGROUP_4` |

### 3.1 RMII pin map on the NUCLEO-H755ZI-Q

| Signal | Pin | | Signal | Pin |
| :--- | :--- | :--- | :--- | :--- |
| `REF_CLK` (50 MHz from the LAN8742A) | PA1 | | `RXD1` | PC5 |
| `MDIO` | PA2 | | `TX_EN` | PG11 |
| `MDC` | PC1 | | `TXD0` | PG13 |
| `CRS_DV` | PA7 | | `TXD1` | PB13 |
| `RXD0` | PC4 | | | |

Every pin is configured on its own port from `BOARD_ETH_*` (`993de3b`), so a
board that routes RMII differently only edits `board_def.h`. The port clock
comes from `GPIO_BM_EnablePortClock()`; the pin itself is set up with
`HAL_GPIO_Init()` (AF push-pull, no pull, very high speed).

### 3.2 Overriding the MSP

The default MSP (`ETH_HAL_MspInit_User`, `__weak`) does, in order:
`SYSCFG_PMCR.EPIS = RMII`, the nine pins, the three ETH clocks
(`ETH1MAC`, `ETH1TX`, `ETH1RX`) in CPU1's RCC bank, and `ETH_IRQn` at
`ETH_DRIVER_IRQ_PRIORITY`. Two ways to replace it:

1. Define a strong `ETH_HAL_MspInit_User()` (and `..._MspDeInit_User()`) in the
   application. `HAL_ETH_MspInit()` stays in the driver and calls it.
2. Define `ETH_HAL_CUSTOM_MSP` for the build. `HAL_ETH_MspInit()` and
   `HAL_ETH_MspDeInit()` then disappear from `eth_hal.c` and the project's own
   `stm32h7xx_hal_msp.c` provides them without a duplicate-symbol error.

---

## 4. Memory and cache

All DMA-visible memory is static, in D2 SRAM3, 32-byte aligned:

| Object | Section | Size |
| :--- | :--- | ---: |
| `g_rx_desc[ETH_RX_DESC_CNT]` | `.RxDescripSection` | 4 × 24 B = 96 B |
| `g_tx_desc[ETH_TX_DESC_CNT]` | `.TxDescripSection` | 4 × 24 B = 96 B |
| `g_rx_pool[8][1536]` | `.RxArraySection` | 12 288 B |
| `g_tx_pool[4][1536]` | `.eth_ram` | 6 144 B |
| **Total** | `.eth_ram` output section | **18 624 B / 32 768 B (56.84 %)** |

The total is the figure the `prj_02` build reports for SRAM3.

- **Why SRAM3 and nothing else.** DTCM is not reachable by the ETH DMA at all;
  AXI SRAM is, but it is cacheable and would need a clean before every transmit
  and an invalidate after every receive. SRAM3 under a non-cacheable MPU region
  needs **no cache maintenance anywhere in the data path**, which is why this
  driver has no `SCB_*DCache*` calls and does not use `02_dma/dma_mem_bm.h`.
- **1536 B buffers**: 48 cache lines, and above the 1522 B maximum tagged frame,
  so a normal frame never spans two descriptors. The segment chain in
  `eth_rx_segment_t` exists for the case where it does (smaller `RxBuffLen`).
- **Build-time checks** in `eth_hal.c`: the four objects fit in 32 KB; the buffer
  size is a multiple of 32; the Rx pool is larger than the Rx descriptor count,
  or the ring would starve while the application holds packets.
- **The bookkeeping** (slot flags, segment headers, HAL handle, stats) is ordinary
  `.bss`, deliberately outside `.eth_ram`: the DMA never reads it.
- **Budget for changes**: 14 144 B of SRAM3 are free. A zero-copy Rx pool
  ([section 10](#10-known-limitations) E6) is the thing most likely to need more;
  the fallback is `.dma_buf` in D2 SRAM1, which is also non-cacheable in these
  projects and reachable by the ETH DMA.

---

## 5. Threading and buffer ownership

### 5.1 Interrupts only notify

`HAL_ETH_RxCpltCallback`, `TxCpltCallback` and `ErrorCallback` run in the ETH
interrupt and do nothing but call the hooks installed with
`ETH_Driver_SetEventCallbacks()` (and count `dma_errors`). All descriptor work -
`ReadPacket`, `ReleasePacket`, `Transmit`, `ReclaimTxBuffers` - happens in task
context. In the lwIP projects (`ethernetif.c`), `on_rx_complete` gives a
semaphore to the input thread, and `on_tx_complete` gives the one that
`low_level_output()` waits on when the Tx pool is empty; it calls
`ReclaimTxBuffers()` before every acquire, and again after that wait.

Install the hooks **before** `ETH_Driver_Start()`.

### 5.2 Who owns a buffer when

| Pool | Taken by | Given back by |
| :--- | :--- | :--- |
| Rx (8 × 1536 B) | `HAL_ETH_RxAllocateCallback()`, when the HAL refills a descriptor | `ETH_Driver_ReleasePacket()`, by the application |
| Tx (4 × 1536 B) | `ETH_Driver_TxBufferAcquire()`, by the application | `HAL_ETH_TxFreeCallback()` via `ReclaimTxBuffers()` once sent, or `TxBufferRelease()` on an error path before `Transmit()` |

- A received frame belongs to the caller of `ReadPacket()` until it is released,
  and **its buffer is not recycled into the ring until then**. Holding packets is
  what makes `rx_alloc_failures` climb.
- `Transmit()` rejects any pointer that is not a pool buffer, and any length of 0
  or above 1536 B. `ReleasePacket()` ignores segments whose payload is outside the
  pool, so a bogus pointer from the HAL cannot corrupt a slot.
- Pool bookkeeping is protected by a PRIMASK save/restore lock, usable from a
  task, from an ISR and from inside a FreeRTOS critical section.

### 5.3 Stop and the stranded Tx descriptors

`HAL_ETH_Stop_IT()` halts the DMA with some Tx descriptors possibly still marked
OWN. `HAL_ETH_ReleaseTxPacket()` stops at the first OWN descriptor, so without
help those buffers would never return and a few link drops under traffic would
empty the Tx pool for good. `ETH_Driver_Stop()` clears OWN on every Tx descriptor
(safe only because the DMA is stopped) and then lets the HAL release them, so the
HAL's own bookkeeping and the pool stay in agreement.

Rx buffers attached to descriptors stay attached across a stop and are reused on
the next start.

### 5.4 Link changes

Speed and duplex can only be programmed while the MAC is stopped. The sequence a
link supervisor runs on every change:

```c
lan8742_link_state_t s = LAN8742_GetLinkState(ETH_Driver_GetPhy());
if (s != last && s >= LAN8742_LINK_10M_HALF && s <= LAN8742_LINK_100M_FULL) {
    (void)ETH_Driver_Stop();
    if (ETH_Driver_ConfigureLink(s) == ETH_DRIVER_OK) {
        (void)ETH_Driver_Start();          /* re-applies FEP for a 10 Mbps link */
    }
}
```

In the lwIP projects this lives in `ethernetif_link_thread()`, polled every
`NET_LINK_POLL_PERIOD_MS` (100 ms).

---

## 6. MAC configuration the driver fixes

| Setting | Value | Why |
| :--- | :--- | :--- |
| Media interface | RMII | ES0445 2.3.11: MII is not usable on this package |
| MDC divider | from HCLK, re-derived after `HAL_ETH_Init()` | Stays right if the clock tree changed before a re-init |
| Filter | perfect match on the station address, broadcast **accepted**, no promiscuous, no pass-all-multicast | ARP and DHCP need broadcast |
| Checksum offload | `ETH_USE_HARDWARE_CHECKSUM` (default off: lwIP does it in software) | Keeps the checksum engine out of bring-up |
| Tx | CRC and padding inserted by the MAC; IP header + payload checksum insertion only with hardware checksum | |

> **`BroadcastFilter` is inverted, and getting it wrong is silent.** The HAL
> writes it straight into `ETH_MACPFR.DBF`, *Disable Broadcast Packets*.
> `ENABLE` therefore blocks broadcast: the board never sees an ARP request,
> never answers one, and is unreachable while the link, the descriptors and every
> counter look healthy. The driver sets it to `DISABLE`. This has bitten the
> project once; check `(ETH_MACPFR >> 5) & 1 == 0` first when `rx` stays at 0.

---

## 7. Silicon errata (ES0445)

| Erratum | Workaround | Where |
| :--- | :--- | :--- |
| 2.3.11 - MII not available on this package | RMII selected in `SYSCFG_PMCR.EPIS` in the MSP, before the MAC leaves reset, and again by `HAL_ETH_Init()` | `ETH_HAL_MspInit_User()` |
| 2.25.8 - spurious Rx watchdog interrupt | `ETH_DMACRXIWTR` written in two steps: `RWT` with `RWTU = 0`, then `RWTU` alone. A non-zero `RWTU` with `RWT = 0` is refused. Init leaves the watchdog off. | `ETH_Driver_SetRxWatchdog()` |
| 2.25.10 - good frames dropped at 10 Mbps (fake dribble + CRC) | `ETH_MTLRXQOMR.FEP` set unconditionally (harmless at 100 Mbps), in `Init` and again on every `Start` | `ETH_Driver_ApplyErrataRxErrorPackets()` |
| 2.25.11 - ARP offload not effective | `ETH_MACCR.ARPEN` cleared; lwIP answers ARP in software | `eth_disable_arp_offload()` |

All are applied **after** `HAL_ETH_Init()`, whose software reset (`DMAMR.SWR`)
returns the MTL and DMA registers to reset values.

**Register names.** RM0399 and ES0445 spell two of these with an `X` that CMSIS
omits. Code uses CMSIS, comments quote the manual:

| RM0399 / ES0445 | `stm32h755xx.h` | Address |
| :--- | :--- | :--- |
| `ETH_MTLRXQOMR` | `ETH->MTLRQOMR` | `0x40028D30` |
| `ETH_DMACRXIWTR` | `ETH->DMACRIWTR` | `0x40029138` |

CMSIS has no mask for `RWTU[1:0]`; the driver declares it from RM0399
(bits 17:16). `ETH_MACWTR` (offset `0x000C`) is a different register and has
nothing to do with 2.25.8.

---

## 8. Dual-core rules

- **CPU1 only.** Nothing in the driver may run on the Cortex-M4: the pools are in
  the CM7 linker script, the HAL ETH module is enabled only in CM7 builds, and
  the ETH clocks are enabled in CPU1's RCC bank, which also allocates the D2
  domain to CPU1 so the MAC keeps running whether or not CPU2 is alive.
- **Shared registers it writes.** `SYSCFG_PMCR` (one copy for both cores, written
  by `HAL_SYSCFG_ETHInterfaceSelect()`) and the configuration registers of GPIO
  ports A, B, C and G. The RMII pins share ports A and B with pins other code
  configures - LEDs on PB0/PB14 from CPU1, and in `prj_03`/`prj_04` the ADC input
  on PB1 or PA3 from CPU2. Two cores must not configure pins of the same port at
  the same time: see `00_gpio/README.md` section 4 (G1). The RCC enables are
  per-core banked and are not a race.
- The driver has no HSEM. Keeping the CM4 away from ports A/B/C/G while CPU1 runs
  `ETH_Driver_Init()` is the application's job.

---

## 9. Validation

### 9.1 Hardware run through `prj_02` (2026-09-20)

Firmware at `68ec30a`, NUCLEO-H755ZI-Q, Rev V, host on `192.168.1.0/24`, static
address `192.168.1.150`. The last commits to `08_eth` (`784febb`, `993de3b`) are
ancestors of `68ec30a`, so **the run covers the driver sources as they are
today**. Full record: `02_firmware_projects/prj_02_eth_lwip_dualcore/README.md`
section 6.

| Check | Result |
| :--- | :--- |
| MDIO / PHY | `bcr=1100 bsr=782D anar=01E1 anlpar=45E1 physcsr=1058`; partner advertises 10/100 half/full |
| Link | `100 Mbps full duplex` - auto-negotiation reached over MDIO, which also shows the nine RMII signals are on the right ports |
| ICMP | 4/4, 0 % loss; first reply ~100 ms (host ARP), then 1 ms |
| UDP echo, port 7 | 1, 25, 512, 1024 and **1472 B** (largest single-frame payload, the one that exercises the 1536 B buffers) all byte-identical, RTT 1.14-1.80 ms |
| UDP burst | 100 datagrams back to back: **100 returned, 0 lost** (the 8/4 pools turning over faster than frames arrive) |
| Firmware counters | `rx`/`tx` 1/1 → 133/113; `rx_nobuf`, `tx_nobuf`, `dma_err` **0** throughout |
| SRAM3 | 18 624 B / 32 KB, as computed in [section 4](#4-memory-and-cache) |

### 9.2 Later images

- `prj_04` boot capture (`prj_04 docs/boot_capture.txt`): PHY registers as above,
  `link up: 100 Mbps full duplex`, `rx_nobuf=0 tx_nobuf=0 dma_err=0`.
- `prj_04` T12 (`02_dma/README.md` section 9): the production image streams ADC
  telemetry over UDP port 5001 to `tools/query_adc.py`, at ~500 kSPS on the ADC
  side, with this driver carrying the frames.

Neither is a data-path test on its own; 9.1 is.

### 9.3 Re-running it

With the image running and the host on the same segment:

```bash
py tools/net_check.py --host 192.168.1.150
```

```bash
py tools/monitor.py --timeout 15
```

`net_check.py` runs the ICMP ping, the UDP size sweep up to 1472 B and the burst;
it writes nothing to the target. The result is only conclusive if `monitor.py`
shows `rx_nobuf`, `tx_nobuf` and `dma_err` still at 0 at the same time - both
ends agree. The staged bring-up procedure (placement, MPU, clocks, MDIO, link,
ARP, errata read-back) is `<project>/docs/ETHERNET_BRINGUP.md`, identical in
`prj_02`, `prj_03` and `prj_04`.

### 9.4 Not exercised on hardware

Say so rather than assume it:

- **10 Mbps.** Neither forced 10 Mbps nor the 2.25.10 path (FEP) has a recorded
  run. `ETHERNET_BRINGUP.md` stage 4.3 describes the test.
- **Errata register read-back** (`FEP = 1`, `ARPEN = 0`, `DMACRXIWTR`,
  `SYSCFG_PMCR.EPIS = 4`): described in `ETHERNET_BRINGUP.md` stage 6, not
  recorded for the shared driver.
- **`ETH_Driver_SetRxWatchdog()` with a non-zero value.**
- **Cable unplug/replug under traffic**, which is what exercises the OWN recovery
  in `ETH_Driver_Stop()` ([section 5.3](#53-stop-and-the-stranded-tx-descriptors)).
- **Sustained load** (flood ping, minutes) and **DHCP**.
- **`ETH_USE_HARDWARE_CHECKSUM = 1`**: never built into a tested image.
- **`ETH_HAL_CUSTOM_MSP`** and a strong `ETH_HAL_MspInit_User()`: no consumer
  uses either.

`prj_eth_lwip_node` has the most thorough Ethernet verification in the workspace
(RMII pin read-back, 60 × 1400 B under load, Rx CRC counters), but it ran on that
project's private copy of these files (see [section 10](#10-known-limitations)
E7), not on this driver.

---

## 10. Known limitations

| # | Limitation | Consequence |
| :--- | :--- | :--- |
| E1 | **Fallback pin defaults.** `eth_hal.c` includes `board_def.h` only if it exists (`__has_include`) and defines Nucleo-144 pins for any `BOARD_ETH_*` it does not find. | Contradicts rule 1 of `01_shared_drivers/README.md`, and differs from the UART console (`uart_hal.c`), where every missing `BOARD_CONSOLE_*` macro is a `#error`. A board that forgets one signal silently gets the Nucleo pin for it. |
| E2 | **`USE_HAL_ETH_REGISTER_CALLBACKS == 0` is assumed, not checked.** The driver overrides the HAL's `__weak` `HAL_ETH_RxAllocateCallback`, `RxLinkCallback`, `TxFreeCallback` and the three event callbacks. | With register-callbacks on, `HAL_ETH_Init()` does not default `rxAllocateCallback` and the first Rx refill calls through a NULL pointer. No `#error` guards it. |
| E3 | **HAL callbacks cannot be turned off.** Only the MSP has an opt-out (`ETH_HAL_CUSTOM_MSP`); the buffer and event callbacks above are always strong symbols. | A project cannot use this driver for the MAC and its own HAL ETH callbacks at the same time. |
| E4 | **One instance, global state.** The HAL handle, PHY and pools are file-scope statics. | Matches the silicon (one ETH MAC); listed so nobody tries a second instance. |
| E5 | **`g_stats` is not atomic.** Counters are incremented from task and ISR context without the lock. | Diagnostics only; a count can in theory be lost, never corrupt a pool. |
| E6 | **Copy data path.** The lwIP glue copies each frame between pbufs and the SRAM3 pools (~1.5 kB per frame per direction). | Invisible for control traffic; worth removing for bulk transfer. `ETHERNET_BRINGUP.md` section 9 has the zero-copy plan and its SRAM3 budget. |
| E7 | **`prj_eth_lwip_node` has a private copy** in `Custom_Drivers/08_eth/`, deliberately kept (its verification ran on those sources). | **A fix here does not reach it.** Porting an errata workaround there is an explicit decision followed by a re-run of its bring-up. |
| E8 | **Cross-core GPIO race** on ports A, B, C, G ([section 8](#8-dual-core-rules)). | Not observed; not ruled out by the driver. |

---

## 11. Changing this driver

`prj_02` and `prj_03` link it through the umbrella target, and `prj_04` links
`stm32h7_driver_eth` by name. The dual-core template does not link it (only
`stm32h7_driver_led` and `stm32h7_driver_uart`). Before changing it:

1. **Every existing signature in `eth_hal.h` and `lan8742_phy.h` is frozen**
   (`01_shared_drivers/README.md` rule 2). New behaviour goes into new functions
   or into fields appended to a struct.
2. **Nothing board-specific in the driver.** Pins come from `board_def.h`; MAC
   and PHY address are parameters. E1 is the one existing exception and should
   shrink, not grow.
3. **Every byte the DMA reads or writes stays in `.eth_ram`**, 32-byte aligned,
   and the `_Static_assert`s stay. If a buffer moves to cacheable memory, the cache
   maintenance moves with it, in the same change.
4. **Errata are applied after `HAL_ETH_Init()`** and FEP is re-applied on every
   `Start`. A new MAC reconfiguration path has to do the same.
5. **The ISR only notifies.** No descriptor work, no allocation, no logging in
   the HAL event callbacks.
6. **Keep the HAL and the pool in agreement.** Return buffers through the HAL's
   own bookkeeping (as `ETH_Driver_Stop()` does), never by clearing pool flags
   behind its back.
7. **Before merging:**
   - `.\tools\build_all.ps1 -Clean`, 8/8 with `-Werror`;
   - flash `prj_02` (or any ETH project), run [section 9.3](#93-re-running-it),
     and update the record in `prj_02 README.md` section 6 with the image hash;
   - if the change touches link handling or an erratum, run the matching item of
     [section 9.4](#94-not-exercised-on-hardware) and move it out of that list;
   - decide explicitly whether the change is ported to `prj_eth_lwip_node` (E7).

---

## 12. History

| Milestone | Commits |
| :--- | :--- |
| Driver centralised in `01_shared_drivers` with `prj_02` | `4204cad` |
| `ETH_IRQHandler` restored; MSP callbacks made strong over ST's weak stubs | `129c96c` |
| Own CMake target; `ETH_HAL_MspInit_User` hook and `ETH_HAL_CUSTOM_MSP`; RMII pins moved to `board_def.h` | `ecf01a2` |
| RMII pins decoupled per signal (`BOARD_ETH_<SIG>_PORT`) | `993de3b` |
| Port clock through `00_gpio` (`GPIO_BM_EnablePortClock`) | `784febb` |
| Hardware verification in `prj_02`, and `tools/net_check.py` | `5ccf6f0`, `0e7c334`, `5c910a5` |
| `prj_eth_lwip_node` private copies recorded as a decision | `6c58348` |
| This README | - |
