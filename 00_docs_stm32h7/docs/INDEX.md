# STM32H755 Master Documentation Hub

Welcome to the central documentation index for **STM32H755ZI**. This index allows developers and AI assistants (Claude Code, Antigravity) to instantly pinpoint the exact hardware manual, peripheral chapter, and design rules.

---

## 1. System Architecture & Core Hardware Manuals

| Document | Location | Description |
| :--- | :--- | :--- |
| **STM32H755 Datasheet** | [DS12923_stm32h755zi_datasheet.pdf](00_core_and_system/DS12923_stm32h755zi_datasheet.pdf) | Electrical specs, operating conditions, pin tables |
| **Alternate Functions (AF)** | [STM32H755_Alternate_Functions.md](../STM32H755_Alternate_Functions.md) | AF0 to AF15 mapping per Port (A to K) |
| **Pin Definitions** | [STM32H755_Pin_Definitions.md](../STM32H755_Pin_Definitions.md) | Pinout table by package and I/O structures |
| **Datasheet Memory Map** | [STM32H755_Memory_Map.md](../STM32H755_Memory_Map.md) | Memory boundaries, default attributes |
| **Errata Sheet (Silicon Bugs)** | [ES0445_stm32h755_errata_sheet.pdf](00_core_and_system/ES0445_stm32h755_errata_sheet.pdf) | Known limitations and silicon workarounds |
| **Full Reference Manual** | [RM0399_stm32h755_reference_manual_full.pdf](00_core_and_system/RM0399_stm32h755_reference_manual_full.pdf) | Complete 3500+ page ST Reference Manual |
| **Cortex-M7 Programming Manual** | [PM0253_cortex_m7_programming_manual.pdf](00_core_and_system/PM0253_cortex_m7_programming_manual.pdf) | M7 core, FPU, MPU, NVIC, pipeline |
| **Cortex-M4 Programming Manual** | [PM0214_cortex_m4_programming_manual.pdf](00_core_and_system/PM0214_cortex_m4_programming_manual.pdf) | M4 core, FPU, MPU, NVIC |
| **Memory Domains & Bus Matrix Guide** | [memory_and_domains.md](00_core_and_system/memory_and_domains.md) | D1, D2, D3 domains, DMA controller routing |
| **Errata Summary (code-impacting)** | [errata_summary.md](00_core_and_system/errata_summary.md) | Curated ES0445 workarounds: DMA lock, cache barriers, 480 MHz, ETH |
| **Linker Templates (M7/M4)** | [linker_templates/](00_core_and_system/linker_templates/README.md) | Dual-core .ld scripts, MPU regions, shared SRAM block |
| **Cache & MPU Rules** | [cache_mpu_rules.md](00_core_and_system/cache_mpu_rules.md) | D-Cache coherency (32-byte), MPU configuration |
| **Dual-Core Boot & IPC** | [dual_core_boot_ipc.md](00_core_and_system/dual_core_boot_ipc.md) | M7/M4 boot handshake, HSEM synchronization |
| **RM0399 System Chapters** | [rm0399_system/](00_core_and_system/rm0399_system/) | Flash, Power (PWR), Clocks/RCC, SMM, Interconnect |

---

## 2. Drivers, HAL, Middleware, FreeRTOS & Tools

| Document / Repository | Location | Description |
| :--- | :--- | :--- |
| **HAL & LL Drivers Manual** | [UM2217_stm32h7_hal_ll_manual.pdf](01_drivers_and_hal/UM2217_stm32h7_hal_ll_manual.pdf) | Official ST HAL and LL API reference (~2,500 pages) |
| **Dual-Core IPC (OpenAMP)** | [AN5617_dualcore_ipc_openamp.pdf](01_drivers_and_hal/AN5617_dualcore_ipc_openamp.pdf) | RPMsg, OpenAMP, FreeRTOS IPC between M7 and M4 |
| **STM32H7 Firmware Examples** | [AN5033_stm32h7_firmware_examples.pdf](01_drivers_and_hal/AN5033_stm32h7_firmware_examples.pdf) | Directory of all official ST example projects |
| **STM32CubeIDE User Guide** | [UM2609_stm32cubeide_user_guide.pdf](01_drivers_and_hal/UM2609_stm32cubeide_user_guide.pdf) | IDE setup, compiler flags, dual-core simultaneous debugging |
| **STM32CubeProgrammer Manual** | [UM2237_stm32cubeprogrammer_user_manual.pdf](01_drivers_and_hal/UM2237_stm32cubeprogrammer_user_manual.pdf) | Option Bytes programming (BCM7, BCM4), flashing CLI/GUI |
| **LwIP TCP/IP Stack Guide** | [UM1713_lwip_tcpip_stack.pdf](01_drivers_and_hal/UM1713_lwip_tcpip_stack.pdf) | Developing applications on STM32Cube with LwIP |
| **USB Device Library** | [UM1734_usb_device_library.pdf](01_drivers_and_hal/UM1734_usb_device_library.pdf) | USB Device classes (CDC, HID, MSC) |
| **FreeRTOS Kernel & Guide** | [FreeRTOS/](01_drivers_and_hal/FreeRTOS/README.md) | Kernel source code, Cortex-M7/M4 ports, NVIC rules |
| **STM32CubeH7 Package** | [STM32CubeH7 Repository](01_drivers_and_hal/STM32CubeH7/) | CMSIS + device headers, HAL/LL source, LwIP, FreeRTOS, NUCLEO-H745ZI-Q examples - **pruned copy with 48 of 52 submodules left empty**, see [PRUNED.md](01_drivers_and_hal/STM32CubeH7/PRUNED.md) before trusting a path here |

---

## 3. Board Hardware Documentation

| Document | Location | Description |
| :--- | :--- | :--- |
| **Nucleo Circuit Schematics** | [MB1363_nucleo_h755zi_q_schematics.pdf](02_board_hardware/MB1363_nucleo_h755zi_q_schematics.pdf) | Official full schematic diagrams, PHY, jumpers, solder bridges |
| **Nucleo-144 User Manual** | [UM2408_nucleo144_board_manual.pdf](02_board_hardware/UM2408_nucleo144_board_manual.pdf) | MB1363 / NUCLEO-H755ZI-Q user manual, connectors, power |
| **Nucleo Data Brief** | [DB_nucleo-h755zi-q_databrief.pdf](02_board_hardware/DB_nucleo-h755zi-q_databrief.pdf) | Board specifications and connector summary |

---

## 4. Hardware Development, Safety & App Notes

| Document | Location | Description |
| :--- | :--- | :--- |
| **Oscillator Design Guide** | [AN2867_oscillator_design_guide.pdf](00_core_and_system/app_notes/AN2867_oscillator_design_guide.pdf) | Crystal oscillator design ($C_L$, transconductance, margin) |
| **H7 Hardware Development** | [AN5419_h7_hardware_development_guide.pdf](00_core_and_system/app_notes/AN5419_h7_hardware_development_guide.pdf) | Power, decoupling capacitors, reset, PCB layout guidelines |
| **Secure Firmware Install (SFI)** | [AN4992_secure_firmware_install.pdf](00_core_and_system/app_notes/AN4992_secure_firmware_install.pdf) | Secure provisioning and flashing |
| **Dual-Core Safety Manual** | [UM2840_dualcore_safety_manual.pdf](00_core_and_system/safety/UM2840_dualcore_safety_manual.pdf) | Safety requirements for dual-core STM32H7 |
| **Single-Core Safety Manual** | [UM2331_singlecore_safety_manual.pdf](00_core_and_system/safety/UM2331_singlecore_safety_manual.pdf) | Safety requirements for single-core STM32H7 |
| **Self-Test Library (IEC 60730)** | [UM3252_selftest_library.pdf](00_core_and_system/safety/UM3252_selftest_library.pdf) | Class B safety self-test user guide |

---

## 4b. Searchable Text Dumps (grep)

Every RM0399 chapter, plus the HAL/LL manual, the errata sheet and the datasheet, has a
`.txt` twin next to the PDF. Same basename, `.txt` extension. PDFs are not searchable
with `grep`; these are.

| What | Where |
| :--- | :--- |
| RM0399 chapters | `<peripheral folder>/<NN>_<name>.txt` and `00_core_and_system/rm0399_system/*.txt` |
| HAL & LL API | `01_drivers_and_hal/UM2217_stm32h7_hal_ll_manual.txt` |
| Errata (full) | `00_core_and_system/ES0445_stm32h755_errata_sheet.txt` |
| Datasheet | `00_core_and_system/DS12923_stm32h755zi_datasheet.txt` |

Use the `.txt` to **find** something and the `.pdf` to **read** register bit diagrams,
figures and wide tables, whose layout does not survive extraction.

```bash
# Which chapter documents a register or bit?
grep -rn --include='*.txt' 'WW1RSC' docs/

# Exact HAL function signature
grep -n 'HAL_UARTEx_ReceiveToIdle_DMA' docs/01_drivers_and_hal/UM2217_stm32h7_hal_ll_manual.txt

# Everything the errata says about a peripheral
grep -n -A15 'DMA stream locked' docs/00_core_and_system/ES0445_stm32h755_errata_sheet.txt
```

Front matter, documentation conventions, the security notice and the revision history
are deliberately **not** dumped: they only add noise to search results.

---

## 4c. Source Trees You Can Grep

The manuals tell you what a register does; these trees tell you what ST's code
actually writes to it. All four live inside the pruned Cube package and are the
same snapshot the firmware compiles against.

| What | Where | Size |
| :--- | :--- | :--- |
| HAL + LL implementation | `01_drivers_and_hal/STM32CubeH7/Drivers/STM32H7xx_HAL_Driver/` | 127 `.c`, 305 files |
| Device headers (register maps) | `01_drivers_and_hal/STM32CubeH7/Drivers/CMSIS/Device/ST/STM32H7xx/Include/stm32h755xx.h` | 323 files in tree |
| LwIP TCP/IP stack | `01_drivers_and_hal/STM32CubeH7/Middlewares/Third_Party/LwIP/` | 768 files |
| FreeRTOS (ST distribution) | `01_drivers_and_hal/STM32CubeH7/Middlewares/Third_Party/FreeRTOS/` | 313 files |

```bash
# How does ST actually program a register? (implementation, not prose)
grep -rn 'ETH_MACCR_' docs/01_drivers_and_hal/STM32CubeH7/Drivers/STM32H7xx_HAL_Driver/Src/

# Bit masks and struct offsets for a peripheral
grep -n 'ETH_DMAC' docs/01_drivers_and_hal/STM32CubeH7/Drivers/CMSIS/Device/ST/STM32H7xx/Include/stm32h755xx.h

# A working dual-core example for the same die
ls docs/01_drivers_and_hal/STM32CubeH7/Projects/NUCLEO-H745ZI-Q/Examples/
```

> **Two FreeRTOS copies, on purpose.** `01_drivers_and_hal/FreeRTOS/FreeRTOS-Kernel/`
> is the upstream kernel (**V11.1.0+**), kept for the port documentation and the
> Cortex-M NVIC rules. The copy under `STM32CubeH7/Middlewares/` is **ST's**
> (**V11.2.0**, plus the `CMSIS_RTOS_V2` wrapper) and is the one the firmware
> links against. When an API question decides code, read ST's copy.

> **48 of the 52 Cube submodules are empty directories**, including every
> `Drivers/BSP/*`, FatFs, mbedTLS and the USB libraries. An empty result there
> means the source was never fetched, not that the symbol does not exist - see
> [PRUNED.md](01_drivers_and_hal/STM32CubeH7/PRUNED.md).

---

## 5. Modular Peripheral Directory (With Extracted RM0399 Chapters)

When working on a specific peripheral, navigate directly to its folder to find the quick cheatsheet and official ST chapter PDF:

| Peripheral Group | Directory / Guide | Official RM0399 Chapter PDFs Included |
| :--- | :--- | :--- |
| **Clocks & Reset** | [peripherals/rcc/](peripherals/rcc/README.md) | `09_reset-and-clock-control-rcc.pdf`, `10_clock-recovery-system-crs.pdf` |
| **GPIO & SYSCFG** | [peripherals/gpio/](peripherals/gpio/README.md) | `12_general-purpose-i-os-gpio.pdf`, `13_system-configuration-controller-syscfg.pdf` |
| **DMA / BDMA / MDMA** | [peripherals/dma_mdma/](peripherals/dma_mdma/README.md) | `15_mdma...`, `16_direct-memory-access...`, `17_basic-direct-memory...`, `18_dmamux...` |
| **Dual-Core HSEM** | [peripherals/hsem_dualcore/](peripherals/hsem_dualcore/README.md) | `11_hardware-semaphore-hsem.pdf` |
| **USART / UART** | [peripherals/usart_uart/](peripherals/usart_uart/README.md) | `51_universal-synchronous...usar.pdf`, `52_low-power...lpuart.pdf` |
| **SPI / SAI / SPDIF** | [peripherals/spi/](peripherals/spi/README.md) | `53_serial-peripheral-interface-spi.pdf`, `54_sai.pdf`, `55_spdifrx.pdf` |
| **I2C** | [peripherals/i2c/](peripherals/i2c/README.md) | `50_inter-integrated-circuit-i2c-interface.pdf` |
| **Timers & PWM** | [peripherals/timers_pwm/](peripherals/timers_pwm/README.md) | `39_hrtim.pdf`, `40_tim1-tim8.pdf`, `41_tim2-tim5.pdf`, `42_tim12-tim14.pdf`, `43_tim15-tim17.pdf`, `44_tim6-tim7.pdf`, `45_lptim.pdf` |
| **ADC / DAC / Analog** | [peripherals/adc_dac/](peripherals/adc_dac/README.md) | `26_adc.pdf`, `27_dac.pdf`, `28_vrefbuf.pdf`, `29_comp.pdf`, `30_opamp.pdf` |
| **Ethernet & MDIOS** | [peripherals/ethernet/](peripherals/ethernet/README.md) | `61_ethernet-eth...pdf`, `57_management-data-input-output-mdios.pdf` |
| **CAN FD (FDCAN)** | [peripherals/fdcan/](peripherals/fdcan/README.md) | `59_controller-area-network-with-flexible-data-rate-fdcan.pdf` |
| **USB (OTG_HS)** | [peripherals/usb/](peripherals/usb/README.md) | `60_usb-on-the-go-high-speed-otg_hs.pdf` |
| **SDMMC** | [peripherals/sdmmc/](peripherals/sdmmc/README.md) | `58_secure-digital-input-output-multimediacard-interface-sdmmc.pdf` |
| **FMC & QUADSPI** | [peripherals/fmc_quadspi/](peripherals/fmc_quadspi/README.md) | `23_fmc.pdf`, `24_quadspi.pdf`, `25_dlyb.pdf` |
| **Display & 2D Graphics** | [peripherals/display_graphics/](peripherals/display_graphics/README.md) | `19_dma2d.pdf`, `33_ltdc.pdf`, `34_dsi.pdf`, `35_jpeg.pdf` |
| **Crypto / HASH / RNG** | [peripherals/crypto_hash_rng/](peripherals/crypto_hash_rng/README.md) | `36_rng.pdf`, `37_cryp.pdf`, `38_hash.pdf` |
| **RTC & Watchdogs** | [peripherals/watchdog_rtc/](peripherals/watchdog_rtc/README.md) | `46_watchdog-overview.pdf`, `47_wwdg.pdf`, `48_iwdg.pdf`, `49_rtc.pdf` |
| **Interrupts (NVIC/EXTI)** | [peripherals/interrupts_exti/](peripherals/interrupts_exti/README.md) | `20_nvic1-and-nvic2.pdf`, `21_exti.pdf` |
| **DFSDM & Camera** | [peripherals/dfsdm_camera/](peripherals/dfsdm_camera/README.md) | `31_dfsdm.pdf`, `32_dcmi.pdf` |
| **CRC / CEC / SWPMI** | [peripherals/crc_cec_swpmi/](peripherals/crc_cec_swpmi/README.md) | `22_crc.pdf`, `56_swpmi.pdf`, `62_cec.pdf` |

---

## 6. Sequential RM0399 Chapter Map (00 to 66)

| Ch # | Chapter Title | Assigned Directory |
| :---: | :--- | :--- |
| **00** | Front matter | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **01** | Documentation conventions | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **02** | Memory and bus architecture | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **03** | RAM ECC monitoring (RAMECC) | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **04** | Embedded Flash memory (FLASH) | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **05** | Secure memory management (SMM) | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **06** | ART Accelerator | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **07** | Power control (PWR) | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **08** | Low-power D3 domain example | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **09** | Reset and clock control (RCC) | [docs/peripherals/rcc/](peripherals/rcc/) |
| **10** | Clock recovery system (CRS) | [docs/peripherals/rcc/](peripherals/rcc/) |
| **11** | Hardware semaphore (HSEM) | [docs/peripherals/hsem_dualcore/](peripherals/hsem_dualcore/) |
| **12** | General-purpose I/Os (GPIO) | [docs/peripherals/gpio/](peripherals/gpio/) |
| **13** | System configuration controller (SYSCFG) | [docs/peripherals/gpio/](peripherals/gpio/) |
| **14** | Block interconnect | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **15** | MDMA controller | [docs/peripherals/dma_mdma/](peripherals/dma_mdma/) |
| **16** | Direct memory access controller (DMA) | [docs/peripherals/dma_mdma/](peripherals/dma_mdma/) |
| **17** | Basic DMA controller (BDMA) | [docs/peripherals/dma_mdma/](peripherals/dma_mdma/) |
| **18** | DMA request multiplexer (DMAMUX) | [docs/peripherals/dma_mdma/](peripherals/dma_mdma/) |
| **19** | Chrom-ART Accelerator (DMA2D) | [docs/peripherals/display_graphics/](peripherals/display_graphics/) |
| **20** | Nested vectored interrupt controllers (NVIC1/2) | [docs/peripherals/interrupts_exti/](peripherals/interrupts_exti/) |
| **21** | Extended interrupt and event controller (EXTI) | [docs/peripherals/interrupts_exti/](peripherals/interrupts_exti/) |
| **22** | Cyclic redundancy check (CRC) | [docs/peripherals/crc_cec_swpmi/](peripherals/crc_cec_swpmi/) |
| **23** | Flexible memory controller (FMC) | [docs/peripherals/fmc_quadspi/](peripherals/fmc_quadspi/) |
| **24** | Quad-SPI interface (QUADSPI) | [docs/peripherals/fmc_quadspi/](peripherals/fmc_quadspi/) |
| **25** | Delay block (DLYB) | [docs/peripherals/fmc_quadspi/](peripherals/fmc_quadspi/) |
| **26** | Analog-to-digital converters (ADC) | [docs/peripherals/adc_dac/](peripherals/adc_dac/) |
| **27** | Digital-to-analog converter (DAC) | [docs/peripherals/adc_dac/](peripherals/adc_dac/) |
| **28** | Voltage reference buffer (VREFBUF) | [docs/peripherals/adc_dac/](peripherals/adc_dac/) |
| **29** | Comparator (COMP) | [docs/peripherals/adc_dac/](peripherals/adc_dac/) |
| **30** | Operational amplifiers (OPAMP) | [docs/peripherals/adc_dac/](peripherals/adc_dac/) |
| **31** | Digital filter for sigma-delta modulators (DFSDM) | [docs/peripherals/dfsdm_camera/](peripherals/dfsdm_camera/) |
| **32** | Digital camera interface (DCMI) | [docs/peripherals/dfsdm_camera/](peripherals/dfsdm_camera/) |
| **33** | LCD-TFT display controller (LTDC) | [docs/peripherals/display_graphics/](peripherals/display_graphics/) |
| **34** | DSI Host (DSI) | [docs/peripherals/display_graphics/](peripherals/display_graphics/) |
| **35** | JPEG codec (JPEG) | [docs/peripherals/display_graphics/](peripherals/display_graphics/) |
| **36** | True random number generator (RNG) | [docs/peripherals/crypto_hash_rng/](peripherals/crypto_hash_rng/) |
| **37** | Cryptographic processor (CRYP) | [docs/peripherals/crypto_hash_rng/](peripherals/crypto_hash_rng/) |
| **38** | HASH processor (HASH) | [docs/peripherals/crypto_hash_rng/](peripherals/crypto_hash_rng/) |
| **39** | High-resolution timer (HRTIM) | [docs/peripherals/timers_pwm/](peripherals/timers_pwm/) |
| **40** | Advanced-control timers (TIM1/8) | [docs/peripherals/timers_pwm/](peripherals/timers_pwm/) |
| **41** | General-purpose timers (TIM2/3/4/5) | [docs/peripherals/timers_pwm/](peripherals/timers_pwm/) |
| **42** | General-purpose timers (TIM12/13/14) | [docs/peripherals/timers_pwm/](peripherals/timers_pwm/) |
| **43** | General-purpose timers (TIM15/16/17) | [docs/peripherals/timers_pwm/](peripherals/timers_pwm/) |
| **44** | Basic timers (TIM6/7) | [docs/peripherals/timers_pwm/](peripherals/timers_pwm/) |
| **45** | Low-power timer (LPTIM) | [docs/peripherals/timers_pwm/](peripherals/timers_pwm/) |
| **46** | Watchdog overview | [docs/peripherals/watchdog_rtc/](peripherals/watchdog_rtc/) |
| **47** | System window watchdog (WWDG) | [docs/peripherals/watchdog_rtc/](peripherals/watchdog_rtc/) |
| **48** | Independent watchdog (IWDG) | [docs/peripherals/watchdog_rtc/](peripherals/watchdog_rtc/) |
| **49** | Real-time clock (RTC) | [docs/peripherals/watchdog_rtc/](peripherals/watchdog_rtc/) |
| **50** | Inter-integrated circuit (I2C) | [docs/peripherals/i2c/](peripherals/i2c/) |
| **51** | USART / UART | [docs/peripherals/usart_uart/](peripherals/usart_uart/) |
| **52** | Low-power UART (LPUART) | [docs/peripherals/usart_uart/](peripherals/usart_uart/) |
| **53** | Serial peripheral interface (SPI) | [docs/peripherals/spi/](peripherals/spi/) |
| **54** | Serial audio interface (SAI) | [docs/peripherals/spi/](peripherals/spi/) |
| **55** | SPDIF receiver (SPDIFRX) | [docs/peripherals/spi/](peripherals/spi/) |
| **56** | Single wire protocol (SWPMI) | [docs/peripherals/crc_cec_swpmi/](peripherals/crc_cec_swpmi/) |
| **57** | Management data input/output (MDIOS) | [docs/peripherals/ethernet/](peripherals/ethernet/) |
| **58** | SD/SDIO/MMC interface (SDMMC) | [docs/peripherals/sdmmc/](peripherals/sdmmc/) |
| **59** | Controller area network (FDCAN) | [docs/peripherals/fdcan/](peripherals/fdcan/) |
| **60** | USB on-the-go high-speed (OTG_HS) | [docs/peripherals/usb/](peripherals/usb/) |
| **61** | Ethernet MAC with DMA | [docs/peripherals/ethernet/](peripherals/ethernet/) |
| **62** | HDMI consumer electronics control (CEC) | [docs/peripherals/crc_cec_swpmi/](peripherals/crc_cec_swpmi/) |
| **63** | Debug infrastructure | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **64** | Device electronic signature | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **65** | Important security notice | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
| **66** | Revision history | [docs/00_core_and_system/rm0399_system/](00_core_and_system/rm0399_system/) |
