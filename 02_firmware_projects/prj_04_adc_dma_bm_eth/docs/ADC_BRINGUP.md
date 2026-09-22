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
PB1 (A3) ----> ADC1 (16-bit, BOOST=10, 25 MHz kernel clock)              |
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
`ADC_SAMPLETIME_8CYCLES_5` at a 25 MHz kernel clock the sampling window is 340 ns,
against the 114 ns minimum that Table 87 gives for 16 bits on a fast channel. About
3x of margin.

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
   - For `12.5 MHz < Fadc_ker_ck <= 25 MHz`, `BOOST[1:0] = 10` (`ADC_CR_BOOST_1`).
   - Configured in `ADC_HAL_Init()` while `ADSTART = 0` and `JADSTART = 0`.

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
Expected output:
```
[BOOT] Cortex-M7 boot sequence started...
[RCC] System clock: 400 MHz, HCLK: 200 MHz, PLL2P (ADC): 25 MHz
[IPC] Shared memory initialized at 0x38008000. Releasing CM4...
[NET] Ethernet link up (100 Mbps Full Duplex)
[NET] IP assigned: 192.168.1.150
[ADC] UDP telemetry server listening on port 5001
```

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

The diagnostic UDP services (Echo on port 7, ADC Telemetry and Waveform on port 5001) respond to incoming datagrams without source IP filtering or authentication:
- **Amplification Risk**: Sending a 4-byte ASCII command (`WAVE`) triggers an immediate 1072-byte binary response containing the complete 512-sample waveform buffer (~268x bandwidth amplification).
- **Deployment Recommendation**: On untrusted or public networks, restrict access using upstream firewall rules, VLAN isolation, or implement source IP allowlisting / HMAC token verification in `adc_server.c`.
