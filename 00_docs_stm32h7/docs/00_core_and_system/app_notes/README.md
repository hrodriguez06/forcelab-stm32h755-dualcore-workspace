# Application Notes (Hardware & System)

This directory contains critical application notes for hardware design, oscillator layout, and secure provisioning on STM32H7:

- **[`AN2867_oscillator_design_guide.pdf`](AN2867_oscillator_design_guide.pdf)**: Guidelines for oscillator design on STM32 MCUs. Crucial for calculating crystal load capacitors ($C_{L1}, C_{L2}$), oscillator transconductance gain margin ($g_m$), and avoiding startup stalls on HSE (High-Speed External) and LSE (32.768 kHz).
- **[`AN5419_h7_hardware_development_guide.pdf`](AN5419_h7_hardware_development_guide.pdf)**: Getting started with STM32H7 hardware development. Details power supplies, decoupling capacitors, ground planes, reset circuits, boot pin configurations, and PCB layout recommendations.
- **[`AN4992_secure_firmware_install.pdf`](AN4992_secure_firmware_install.pdf)**: Introduction to Secure Firmware Install (SFI) for protecting firmware during manufacturing and flashing.
