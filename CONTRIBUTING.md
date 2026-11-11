# 🛠️ Collaborator Guide & Development Manual (Onboarding)

> 🌐 **Language / Idioma:** **English** | [Español](CONTRIBUTING.es.md)

Welcome to the **`forcelab-stm32h755-dualcore-workspace`** repository. This workspace is engineered as a modular monorepo for dual-core embedded firmware development targeting the **STM32H755ZI** (Arm Cortex-M7 @ 480 MHz + Arm Cortex-M4 @ 240 MHz).

This guide provides step-by-step instructions to configure your environment, clone dependencies, compile firmware, flash hardware, and follow the project's Git workflow.

---

## 📑 Table of Contents
1. [System Prerequisites](#1-system-prerequisites)
2. [Quickstart Setup](#2-quickstart-setup)
3. [Restoring the STM32CubeH7 SDK (Critical!)](#3-restoring-the-stm32cubeh7-sdk-critical)
4. [Building Dual-Core Firmware Projects](#4-building-dual-core-firmware-projects)
5. [Flashing, Monitoring & Hardware Testing](#5-flashing-monitoring--hardware-testing)
6. [Architecture & Development Rules](#6-architecture--development-rules)
7. [Git Workflow (Branching & Commits)](#7-git-workflow-branching--commits)
8. [Frequently Asked Questions & Troubleshooting (FAQ)](#8-frequently-asked-questions--troubleshooting-faq)

---

## 1. System Prerequisites

Ensure the following tools are installed on your host development machine:

| Tool | Minimum Version | Purpose |
| :--- | :--- | :--- |
| **Git** | 2.30+ | Version control (enable `core.longpaths` on Windows). |
| **GNU Arm Toolchain** | 12.3+ (`arm-none-eabi-gcc`) | Cross-compiler for Cortex-M architectures. |
| **CMake** | 3.22+ | Build configuration and generation system. |
| **Ninja** | 1.10+ | Fast build execution engine. |
| **Python** | 3.10+ | For automated dual-core flasher, serial monitor, and telemetry scripts. |
| **VS Code** (Recommended) | Latest | Multi-root workspace editor environment. |
| **STM32CubeProgrammer / OpenOCD** | Latest | Hardware programming utilities for ST-LINK V3. |

> 💡 **Tip for Windows:** If STM32CubeIDE or STM32CubeCLT is installed, you can automatically add the toolchain to your PowerShell `$env:PATH` by running:
> ```powershell
> .\tools\env.ps1
> ```

---

## 2. Quickstart Setup

```powershell
# 1. Clone the repository
git clone https://github.com/hrodriguez06/forcelab-stm32h755-dualcore-workspace.git
cd forcelab-stm32h755-dualcore-workspace

# 2. Enable long paths in Windows (prevents MAX_PATH errors with nested ST trees)
git config core.longpaths true

# 3. Restore the lightweight STM32CubeH7 SDK
.\tools\init_cube_submodule.ps1

# 4. Open the multi-root workspace in VS Code
code stm32h755.code-workspace
```

---

## 3. Restoring the STM32CubeH7 SDK (Critical!)

> ⚠️ **CRITICAL WARNING:**  
> **DO NOT run `git submodule update --init --recursive`.**  
> ST's upstream package contains 52 nested submodules and deep paths exceeding the Windows 260-character limit. A recursive clone fails midway, leaving broken checkouts while downloading ~350 MB of redundant files.

Use the workspace's dedicated initialization script instead:
```powershell
.\tools\init_cube_submodule.ps1
```
* **How it works:** Executes a partial (`--filter=blob:none`), shallow (`--depth 1`), and sparse checkout restricted to `Drivers` (CMSIS, HAL) and `Middlewares` (FreeRTOS, LwIP).
* **Speed & Size:** Completes in ~20–30 seconds (~26 MB vs. 359 MB).
* **Verification:** To check if the submodule is valid without modifying files:
  ```powershell
  .\tools\init_cube_submodule.ps1 -Check
  ```

---

## 4. Building Dual-Core Firmware Projects

Firmware projects reside in `02_firmware_projects/`. Each project builds **both cores (CM7 and CM4)** in a single configuration step:

### Example: Building `prj_04_adc_dma_bm_eth`

```powershell
cd 02_firmware_projects/prj_04_adc_dma_bm_eth

# Configure build directory with Ninja
cmake -B build -G Ninja

# Compile both targets
cmake --build build
```

### Using CMake Presets (Recommended):
```powershell
# Debug preset
cmake --preset debug
cmake --build --preset debug

# Release preset
cmake --preset release
cmake --build --preset release
```

### Generated Artifacts in `build/`:
* `cm7_firmware.elf` / `.bin`: Cortex-M7 firmware (Flash Bank 1: `0x08000000`, FreeRTOS + lwIP Ethernet).
* `cm4_firmware.elf` / `.bin`: Cortex-M4 firmware (Flash Bank 2: `0x08100000`, Bare-Metal ADC1 + DMA1).

---

## 5. Flashing, Monitoring & Hardware Testing

Connect your **NUCLEO-H755ZI-Q** board via the ST-LINK USB port:

### A. Automatic Dual-Core Flash
Run from either the project directory or the workspace root:
```powershell
# Programs CM7 into Flash Bank 1 and CM4 into Flash Bank 2
python tools/flash.py
```

### B. ST-LINK Virtual COM Port (VCP) Monitor
View live serial telemetry without external terminal software (PuTTY / TeraTerm):
```powershell
python tools/monitor.py
```

### C. Ethernet Network Check (Ping + UDP Echo)
For projects using lwIP (e.g., `prj_04`, `prj_05`, `prj_06`):
```powershell
python tools/net_check.py 192.168.1.50
```

---

## 6. Architecture & Development Rules

When adding or modifying code, adhere strictly to the workspace architecture:

1. **Strict Core Separation:**
   * **Cortex-M7 (CPU1):** System coordinator, FreeRTOS, networking (lwIP Ethernet), user interface, UDP telemetry, and high-load DSP (FFT / CWT).
   * **Cortex-M4 (CPU2):** Real-time determinism, **Bare-Metal (no RTOS)**, dedicated to fast control loops and ADC/DMA acquisition pipelines.
2. **Three-Tier Driver Architecture (`01_shared_drivers/`):**
   * `*_hal.[c|h]`: High-level ST HAL API implementations.
   * `*_ll.[c|h]`: Low-Level ST API register wrappers focused on latency.
   * `*_bm.[c|h]`: Pure Bare-Metal register operations without library overhead.
3. **Shared Memory & D-Cache Coherency:**
   * Inter-core buffers (SRAM4 / AXI SRAM) and DMA descriptors must be **32-byte aligned** (`ALIGN_32BYTES`) and protected by explicit cache maintenance (`SCB_CleanDCache`, `SCB_InvalidateDCache`). See `00_docs_stm32h7/docs/00_core_and_system/cache_mpu_rules.md`.
4. **Hardware Semaphore (HSEM) Synchronization:**
   * Dual-core boots and message queues synchronize via ST Hardware Semaphores. CM7 boots first, initializes clocks, and releases CM4 by freeing `HSEM 0`.

---

## 7. Git Workflow (Branching & Commits)

To ensure code quality and maintain repository stability:

### Branching Model
* **`main`:** Stable, verified firmware. Direct commits are restricted.
* **`feature/<name>`:** New capabilities (e.g., `feature/spi-dma-driver`).
* **`fix/<name>`:** Bug fixes (e.g., `fix/adc-calibration-offset`).
* **`docs/<name>`:** Technical documentation improvements.

### Commit Messages (Conventional Commits)
Write concise commit subjects with typed scopes:
* `feat(adc): add oversampling configuration support`
* `fix(eth): handle link-down reconnection in lwip task`
* `refactor(hsem): unify lock timeout handling`
* `docs(readme): update nucleo jumper wiring table`

### Pull Request (PR) Checklist
1. Rebase your branch against the latest `main`: `git pull origin main --rebase`.
2. Ensure clean compilation: `cmake --build build`.
3. Provide in your PR:
   - Summary of modifications.
   - Affected firmware project(s).
   - Real hardware test results (board, probe, test output).

---

## 8. Frequently Asked Questions & Troubleshooting (FAQ)

### ❓ `Fatal error: STM32CubeH7 is missing or incomplete`
* **Root Cause:** The SDK submodule has not been restored.
* **Fix:** Run `.\tools\init_cube_submodule.ps1` from the workspace root.

### ❓ `Filename too long` error on Windows
* **Root Cause:** Nested ST repository paths exceed the legacy 260-character limit.
* **Fix:** Run `git config --system core.longpaths true` from an elevated shell.

### ❓ `arm-none-eabi-gcc: command not found`
* **Root Cause:** GCC compiler is not registered in the system `$PATH`.
* **Fix:** Run `.\tools\env.ps1` in PowerShell or add your GNU Arm toolchain `bin/` directory to `$PATH`.

### ❓ ST-LINK fails to connect or target is halted
* **Root Cause:** Microcontroller is in low-power sleep mode or debug interface is blocked during dual-core lockup.
* **Fix:** Hold down the black **USER RESET** button on the Nucleo board, execute `python tools/flash.py`, and release the button as soon as OpenOCD / STM32CubeProgrammer initiates connection.
