# ADC1 + DMA1 + Ethernet Telemetry Bring-up Guide (`prj_04_adc_dma_bm_eth`)

## 1. Overview & Architecture

This project validates high-speed 16-bit ADC acquisition with circular DMA on the Cortex-M4 (bare-metal, Domain D2), transferring telemetry to the Cortex-M7 (FreeRTOS + lwIP Ethernet) via shared SRAM4 protected by hardware semaphores (HSEM 4). Telemetry can be queried on-demand over UDP port 5001 or visualised continuously on a web dashboard at `http://127.0.0.1:8080`.

```
           +-------------------------------------------------------------+
           |                     NUCLEO-H755ZI-Q                         |
           |                                                             |
           |  Cortex-M4 (Domain D2, Bare-metal)                          |
           |    TIM3 (TRGO @ 500 kHz, APB1 200 MHz)                      |
           |      |                                                      |
           |      v (Hardware Trigger)                                   |
PB1 (A3) ----> ADC1 (16-bit, BOOST=01 (Rev V) / 10 (Rev Y), 25 MHz kernel clock)              |
           |      |                                                      |
           |      v (DMA1 Stream 0, Request 9)                           |
           |    SRAM2 (0x30020000, 1024 samples circular buffer)         |
           |      |                                                      |
           |      v (HT/TC ISR -> Flag only; Stats in Super-Loop)        |
           |    SRAM4 (0x38008000, g_shared.adc under HSEM 4)            |
           |      |                                                      |
           |      v (Non-blocking HSEM 4 read)                           |
           |  Cortex-M7 (Domain D1, FreeRTOS + lwIP)                     |
           |    UDP Server (:5001, net_adc_task)                         |
           |      |                                                      |
           +------|------------------------------------------------------+
                  | RMII (LAN8742A PHY)
                  v
         Host PC (192.168.1.x)
           |-- tools/query_adc.py [--json]
           `-- tools/adc_dashboard.py (http://127.0.0.1:8080)
```

---

## 2. Hardware Pinout & Domain Partitioning

| Peripheral | Domain | Instance | Pin / Bus | Description |
| :--- | :--- | :--- | :--- | :--- |
| **ADC Input** | D2 | ADC1 | `PB1` (CN9 pin 4 / A3) | Channel 5, Single-Ended, **fast channel** |
| **Trigger Timer** | D2 | TIM3 | APB1 (200 MHz) | TRGO on Update Event @ 500,000 Hz (PSC=0, ARR=399) |
| **DMA Controller** | D2 | DMA1_Stream0 | AHB1 (200 MHz) | DMAMUX Request 9 (ADC1) |
| **DMA Buffer** | D2 | SRAM2 | `0x30020000` | 1024 x 16-bit (2048 B, 32-byte aligned); half-buffer = 512 samples = 1.024 ms |
| **Shared Telemetry** | D3 | SRAM4 | `0x38008000` | `SharedMemory_t.adc` guarded by HSEM 4 |
| **Ethernet MAC** | D1/D2 | ETH | RMII (`PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13`) | LAN8742A PHY (Address 0) |
| **VCP Console** | D2 | USART3 | `PD8 (TX), PD9 (RX)` | 115,200 baud, 8N1 |

### Why `PB1`, and what you are allowed to connect to it

`PB1` is one of the nine **fast channels** the datasheet names explicitly (DS12923
Table 87, note 4: `PF3, PF5, PF7, PF9, PA6, PC4, PB1, PF11, PF13`). Everything else
is a slow channel, and at 500 kSPS the difference is the whole point: with
`ADC_SAMPLETIME_8CYCLES_5` at a 25 MHz kernel clock (`adc_ker_ck`), the internal
clock is divided by 2 before the analog core (RM §26.4.3, `f_ADC = 12.5 MHz`), so
the actual sampling window is 8.5 / 12.5 MHz = 680 ns, well above the 114 ns
minimum that Table 87 gives for 16 bits on a fast channel. About 6x of margin.

`PA3` (`ADC12_INP15`, CN9 pin 1 / A0) remains available as a
compile-time alternative - set `BOARD_ADC_USE_FAST_CHANNEL_PB1` to `0` in
`Common/Inc/board_def.h` - but it is a slow channel and was the pin used while the
project sampled at 10 kHz.

**Source impedance is the constraint that actually bites.** Table 87 characterises
16-bit conversions **only up to R<sub>AIN</sub> = 47 ohm** - that is the single row
the table has at this resolution. The highest impedance it tabulates at *any*
resolution is 3.3 kilohm, at 10 bits. So:

- **Good sources**: the 3V3 rail, GND, a divider built from tens of ohms, or
  anything behind an op-amp follower.
- **Bad source**: a bare potentiometer wiper. A 10 kilohm pot presents roughly
  2.5 kilohm at mid-scale, two orders of magnitude past the characterised range.
  The sample-and-hold will not settle, readings come out low and noisy, and the
  acquisition chain gets blamed for an analog problem. If you want to sweep a
  voltage by hand, buffer the pot or drop to 12-bit resolution for that test.

---

## 3. Clock Tree & Silicon Errata Mitigations

1. **ADC Kernel Clock (`adc_ker_ck`)**:
   - Source: `PLL2P`
   - Configuration: `HSE (8 MHz) / 4 * 100 / 8 = 25 MHz`
   - `ADCSEL` set to `RCC_ADCCLKSOURCE_PLL2` by CM7 before releasing HSEM 0.
   - Frequency check: `Fadc_hclk (200 MHz) >= Fadc_ker_ck / 4 (6.25 MHz)` is satisfied.

2. **BOOST Mode Configuration (RM0399 / adc.txt:4597)**:
   - For `Fadc_ker_ck = 25 MHz`, the internal SAR analog core clock is `f_analog = Fadc_ker_ck / 2 = 12.5 MHz` (RM0399 §26.4.3). This corresponds to `BOOST[1:0] = 01b` (`6.25 MHz < f_analog <= 12.5 MHz`).
   - Handled automatically by `ADC_BM_InitEx()` / `ADC_LL_InitEx()` / `ADC_HAL_InitEx()` while `ADSTART = 0` and `JADSTART = 0`.

3. **Injected Queue Disabling (ES0445 2.10.x)**:
   - `ADC_CFGR_JQDIS` set to disable the queue of context for injected conversions and prevent queue corruption.

4. **FPU Lazy Stacking Errata (ES0445 2.2.2)**:
   - Cortex-M4 clears `FPU_FPCCR_LSPEN_Msk` upon wakeup to prevent register corruption during interrupt entry.

5. **HSEM Safety in ISRs**:
   - Semaphore locking (`SHARED_LOCK`) is **NEVER** called in interrupt context.
   - `HAL_ADC_ConvHalfCpltCallback` and `HAL_ADC_ConvCpltCallback` only record the completed half (`ready_half = 0/1`) and set `has_new_data = 1`.
   - Statistics (min, max, average, mV) and publication to `g_shared.adc` take place strictly in the CM4 super-loop.

---

## 4. Host Software & Verification Tools

The project includes two host verification utilities under `tools/`. Unlike
`flash.py`, `monitor.py`, `net_check.py` and `env.ps1` - which are shims onto the
workspace copies - these two are **owned by this project** and have no shim: they
decode this firmware's own UDP protocol, so they change whenever
`CM7/App/Src/adc_server.c` changes. See *Where a script belongs* in the workspace
README.

### A. CLI Query Utility (`tools/query_adc.py`)
Queries the target over UDP and prints human-readable telemetry or JSON:

```powershell
# Plain-text single shot
py tools/query_adc.py --host 192.168.1.150

# JSON output
py tools/query_adc.py --host 192.168.1.150 --json

# Continuous monitoring at 10 Hz
py tools/query_adc.py --host 192.168.1.150 --rate 10 --count 20
```

Sample JSON response:
```json
{
  "ch": 5,
  "raw": 32768,
  "avg": 32765,
  "min": 32710,
  "max": 32820,
  "mv": 1650,
  "rate": 500000,
  "seq": 58594,
  "samples": 30000128,
  "tc": 29297,
  "ht": 29297,
  "err": 0,
  "ovr": 0,
  "m4_hb": 600
}
```

Those figures are what roughly 60 s of uptime looks like: `tc` and `ht` each advance
488.3 times per second (one full 1024-sample buffer every 2.048 ms), `seq` counts the
published half-buffers, and `samples` is 512 per half.

### B. Live Web Dashboard (`tools/adc_dashboard.py`)
Autonomous local server hosting a live HTML5 visualization:

```powershell
py tools/adc_dashboard.py --host 192.168.1.150 --bind 127.0.0.1:8080
```
Open `http://127.0.0.1:8080` in any modern web browser to view:
- Dynamic voltage chart (0 - 3300 mV) with 60-second sliding time window.
- Telemetry summary cards: Voltage, Raw ADC, Min / Max, Sample Counter.
- Diagnostic indicators: CM4 heartbeat, DMA transfer counters (HT/TC/Err), Freshness pill.

---

## 5. Step-by-Step Bring-Up Procedure

### Step 1: Toolchain Setup & Build
```powershell
# In repo root:
.\tools\env.ps1

# Navigate to project:
cd 02_firmware_projects\prj_04_adc_dma_bm_eth

# Configure & Build
cmake --preset debug
cmake --build --preset debug
```

### Step 2: Hardware Connections
1. Connect NUCLEO-H755ZI-Q to host PC via Micro-USB (ST-LINK).
2. Connect RJ45 Ethernet cable to LAN or host PC Ethernet adapter configured to `192.168.1.x` subnet.
3. Connect an analog voltage between 0 and 3.3 V to **CN9 pin 4** (`PB1` / Arduino `A3`).
   Use a **low-impedance** source: the 3V3 rail, GND, a divider made of tens of ohms,
   a signal generator, or a pot behind an op-amp follower. A bare potentiometer wiper
   will read low and noisy at 16 bits - see *Why `PB1`* in section 2 for the numbers.
   Leaving the pin floating is a valid first test too, as long as you expect a
   floating result rather than reading it as a broken DMA chain.

### Step 3: Flashing Firmware
```powershell
py tools/flash.py --tool cubeprog
```
This programs:
- Bank 1 (`0x08000000`): `cm7_firmware.bin`
- Bank 2 (`0x08100000`): `cm4_firmware.bin`

### Step 4: Validate UART Logs
```powershell
py tools/monitor.py
```
Expected output, captured verbatim from a board booting this firmware:
```
=== STM32H755ZI dual-core Ethernet / lwIP - CPU1 (Cortex-M7) ===
[sys] sysclk=400000000 Hz hclk=200000000 Hz
[sys] silicon revision id=0x2003
[ipc] shared block at 0x38008000, magic=0x48375353
[ipc] CPU2 boot handshake: enabled (HSEM 0)
[net] starting TCP/IP stack
[net] interface up, ip=192.168.1.150 netmask=255.255.255.0 gw=192.168.1.1
[net] mac=02:00:00:00:00:01 phy_addr=0
[phy] bcr=1000 bsr=7809 anar=01E1 anlpar=0001 physcsr=0040
[net] udp echo listening on port 7
[adc_srv] listening on UDP port 5001
[ipc] m7=1 m4=1  (CPU2 alive)
[phy] bcr=1100 bsr=782D anar=01E1 anlpar=45E1 physcsr=1058
[phy] partner advertised: 100BASE-TX-FD 100BASE-TX-HD 10BASE-T-FD 10BASE-T-HD
[net] link up: 100 Mbps full duplex
[net] link=100 Mbps full duplex ip=192.168.1.150 rx=0 tx=1 rx_nobuf=0 tx_nobuf=0 dma_err=0 acl_drop=0
```
The PHY is logged twice on purpose: the first line is the link before
auto-negotiation has finished, with an empty `partner advertised`, and the second is
the same registers once it has. A board that never prints the second pair has a cable
or a PHY problem, not a firmware one.

### Step 5: Validate UDP Queries
```powershell
py tools/query_adc.py --host 192.168.1.150
```
Verify:
- `mv` matches the voltage applied to `PB1`.
- `tc` and `ht` each advance ~488.3 times per second (500 kSPS / 1024 samples per full
  buffer), and `samples` therefore climbs by ~500,000 per second.
- `err` remains 0.
- **`ovr` remains 0.** This is `half_overrun_count`: it counts half-buffers the CM4
  super-loop failed to pick up before the DMA filled the next one. `tc`, `ht` and
  `samples` are incremented from the ISR, so they keep reading a perfect 500 kSPS even
  when published statistics are skipping data - `ovr` is the only field that exposes
  it. A non-zero `ovr` means the per-half work no longer fits in the 1.024 ms budget.
- **`adc_ovr` remains 0.** This is `adc_overrun_count`: hardware ADC conversion overruns (OVR flag).
- `m4_hb` continuously increments.

### Step 6: Launch Web Dashboard
```powershell
py tools/adc_dashboard.py
```
Open `http://127.0.0.1:8080` to observe the live real-time waveform and telemetry.

---

## 6. Security Note: UDP Services & Amplification Vector

Both diagnostic services - echo on port 7, telemetry and waveform on port 5001 - answer
without authentication, and the waveform reply is large: a 4-byte `WAVE` command returns
1080 bytes, roughly 270x amplification. Left open, the board is a usable reflector, and
the echo service is one by definition.

The services therefore answer only sources on the interface's own subnet.
`NET_ACL_MODE` in `CM7/App/Inc/net_config.h` selects the policy:

| Value | Behaviour |
| :--- | :--- |
| `NET_ACL_ANY` | Answer any source. The original behaviour, for a debugging session. |
| `NET_ACL_SAME_SUBNET` | Answer only sources on this interface's subnet. **Default.** |

The subnet is read from the live netif rather than restated in the header, so it follows
DHCP and the static fallback with nothing to keep in step. An interface that has no
address yet has a zero netmask, which would otherwise match every source; that case
refuses instead.

**This is not a security boundary.** A source address is trivially spoofed, and anything
on the subnet is still answered. What it removes is the board's value as an off-link
amplifier, without a list of hosts to maintain. On an untrusted network keep using
upstream firewall rules or VLAN isolation, and add a token check in `adc_server.c` if the
telemetry itself is worth protecting.

**If queries stop working after a network change, suspect this first.** The first refusal
logs the address it came from, and every refusal is counted as `acl_drop` in the periodic
`[net]` status line. ICMP is untouched, so `ping` remains the first reachability check.

---

## 7. Hardware Validation Record

Run against a NUCLEO-H755ZI-Q flashed from this tree with `py tools/flash.py --core both`.
The banner reports silicon revision `0x2003`, Rev V - the branch `ADC_BM_BOOST_AUTO`
resolves to the `ker_ck / 2` boost table. The Rev Y path stays unexercised for want of
the silicon.

### A. Acquisition, telemetry and the WAVE header

| Check | Result |
| :--- | :--- |
| Text reply | `ADC1_CH5 [PB1 [A3]]: ... err=0, ovr=0, adc_ovr=0, m4_hb=453` |
| JSON reply | `{"ch":5,...,"err":0,"ovr":0,"adc_ovr":0,"m4_hb":502}` |
| WAVE packet | 1080 B, `version=1`, `header_len=56`, `num_samples=512`, `vref=3300` |
| Dashboard `/api/samples` | `adc_ovr` present with a value, 512 waveform samples served |
| Sustained rate | `samples` 5,208,064 -> 5,463,552 in 0.5 s, about 511 kSPS |
| Error counters | `err`, `ovr` and `adc_ovr` all 0 across ~5 M samples |

A `ch` field reading nonsense - `351272992` was seen once - is the signature of CPU1 and
CPU2 disagreeing about `SharedAdcData_t`, which means one core was reflashed and the
other was not. `shared_memory.h` warns about it in its own header; this is what it looks
like from the outside. Flash both.

An old firmware answering a current tool is rejected rather than misread:

```
ERROR: firmware speaks WAVE v41248, this tool speaks v1 - reflash or update the tool
```

`41248` is `0xA120`, the low half of 500000: the parser read `version` at offset 4 and
found `sampling_rate_hz`, because that board was still emitting the 48-byte header. The
predecessor of this tool drew those bytes as signal instead.

### B. Source filter, both halves

The accept path is easy: query from a host on the board's subnet, get an answer, watch
`acl_drop` stay at 0. The refuse path deserves a procedure, because a timeout on its own
proves nothing - routing, a host firewall and the filter are indistinguishable from
outside. The console counter is what separates them.

With no second subnet to hand, invert the problem and make *every* source off-subnet:

1. Set `NET_STATIC_NETMASK3` to `255` in `CM7/App/Inc/net_config.h` (a /32 netmask).
2. Rebuild and flash **CM7 only** - that header does not reach the CM4 image.
3. Attach `py tools/monitor.py` and query from anywhere.

| Netmask | Query | `rx` / `tx` | `acl_drop` |
| :--- | :--- | :--- | :--- |
| `255.255.255.0` | answered | 1612 / 1603, tracking 1:1 | 0, flat over nine status lines |
| `255.255.255.255` | times out | 37 / 1, tx frozen | 6 -> 16 -> 26 -> 36, tracking `rx` |

And exactly once, on the first refusal:

```
[net] source filter: dropped a UDP request from 192.168.1.114, which is not on 192.168.1.150's subnet - see NET_ACL_MODE in net_config.h
```

Then revert the netmask, rebuild, and flash CM7 again. Do not commit the /32: it is a
test fixture, not a configuration.
