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
| **DHCP** | Enabled (`NET_USE_DHCP 1`) | Falls back to static IP if no DHCP server answers |
| **Static Fallback IP** | `192.168.1.50` | Active when DHCP disabled or timeout |
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
