# Drivers, HAL, Middleware, FreeRTOS & Tools Documentation

This directory contains the official software drivers, APIs, toolchain manuals, FreeRTOS kernel, and the STM32CubeH7 repository:

## 1. Official ST Manuals & Application Notes
- **[`UM2217_stm32h7_hal_ll_manual.pdf`](UM2217_stm32h7_hal_ll_manual.pdf)**: Description of STM32H7 HAL and low-layer drivers (~2,500 pages of complete API specifications).
- **[`AN5617_dualcore_ipc_openamp.pdf`](AN5617_dualcore_ipc_openamp.pdf)**: Inter-processor communication (IPC) for STM32H745/755 dual-core devices (OpenAMP, RPMsg, FreeRTOS).
- **[`AN5033_stm32h7_firmware_examples.pdf`](AN5033_stm32h7_firmware_examples.pdf)**: Comprehensive directory of firmware examples available in the STM32CubeH7 package.
- **[`UM1713_lwip_tcpip_stack.pdf`](UM1713_lwip_tcpip_stack.pdf)**: Developing applications on STM32Cube with LwIP TCP/IP stack (Ethernet).
- **[`UM1734_usb_device_library.pdf`](UM1734_usb_device_library.pdf)**: STM32Cube USB Device library guide (CDC, HID, MSC, etc.).

## 2. IDE, Toolchain & Programming Software
- **[`UM2609_stm32cubeide_user_guide.pdf`](UM2609_stm32cubeide_user_guide.pdf)**: Official STM32CubeIDE user manual. Explains project management, build settings, compiler options, and dual-core simultaneous debug configurations (M7 + M4 on separate GDB ports).
- **[`UM2237_stm32cubeprogrammer_user_manual.pdf`](UM2237_stm32cubeprogrammer_user_manual.pdf)**: STM32CubeProgrammer manual (GUI and CLI). Used for programming Option Bytes (`BCM7`, `BCM4`), independent Flash bank flashing, and security readout protection.

## 3. FreeRTOS Real-Time Kernel
- **Location**: [`FreeRTOS/`](FreeRTOS/)
- **Guide**: [`FreeRTOS/README.md`](FreeRTOS/README.md) (NVIC priority grouping, SysTick conflict fix, D-Cache with RTOS).
- **Source Code**: [`FreeRTOS/FreeRTOS-Kernel/`](FreeRTOS/FreeRTOS-Kernel/) (Official kernel source files, Cortex-M7 and M4 ports, memory managers).

## 4. STM32CubeH7 Repository
- **Location**: [`STM32CubeH7/`](STM32CubeH7/)
- **Official Example Projects**: `STM32CubeH7/Projects/NUCLEO-H745ZI-Q/Examples/` (Compatible with NUCLEO-H755ZI-Q).
- **CMSIS Headers & Definitions**: `STM32CubeH7/Drivers/CMSIS/Device/ST/STM32H7xx/Include/stm32h755xx.h`
- **Middlewares**: `STM32CubeH7/Middlewares/` (LwIP, FatFS, OpenAMP, FreeRTOS).
