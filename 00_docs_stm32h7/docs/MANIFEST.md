# PDF Manifest

The PDFs in this tree are **not tracked by git** (see the workspace
`.gitignore`). They are immutable ST publications: versioning them buys nothing
and costs 222 MB in every clone. What git does carry is the `.txt` extraction
of each one, which is what searches actually read.

This file is the inventory, so a fresh clone knows exactly what is missing and
where each file goes.

## How to get them back

Every ST document is reachable by its identifier - `RM0399`, `DS12923`,
`ES0445`, `UM2217`, `AN5419` and so on:

```
https://www.st.com/resource/en/reference_manual/<id>-*.pdf
https://www.st.com/en/search.html#q=<id>            (when the direct link moves)
```

The chapter-level PDFs under `rm0399_system/` and `peripherals/` were split out
of `RM0399_stm32h755_reference_manual_full.pdf` locally; re-split that one
rather than hunting for them individually.

The `.txt` twin of each file is tracked, so searching keeps working with no PDF
present at all. Fetch a PDF when you need a register bit diagram, a figure or a
wide table - the layout those depend on does not survive text extraction.

## PDF-only documents (no .txt twin)

66 of the 94 have a tracked `.txt` extraction, so their content survives in
the repository. These 28 do not - without the PDF, the content is simply not
here. All are public ST or ARM publications retrievable by document id.

- `00_core_and_system/PM0214_cortex_m4_programming_manual.pdf`
- `00_core_and_system/PM0253_cortex_m7_programming_manual.pdf`
- `00_core_and_system/RM0399_stm32h755_reference_manual_full.pdf`
- `00_core_and_system/app_notes/AN2867_oscillator_design_guide.pdf`
- `00_core_and_system/app_notes/AN4992_secure_firmware_install.pdf`
- `00_core_and_system/app_notes/AN5419_h7_hardware_development_guide.pdf`
- `00_core_and_system/datasheets_variants/stm32h745zg_datasheet.pdf`
- `00_core_and_system/datasheets_variants/stm32h755bi_datasheet.pdf`
- `00_core_and_system/rm0399_system/00_front-matter.pdf`
- `00_core_and_system/rm0399_system/01_documentation-conventions.pdf`
- `00_core_and_system/rm0399_system/65_important-security-notice.pdf`
- `00_core_and_system/rm0399_system/66_revision-history.pdf`
- `00_core_and_system/safety/UM2331_singlecore_safety_manual.pdf`
- `00_core_and_system/safety/UM2840_dualcore_safety_manual.pdf`
- `00_core_and_system/safety/UM3252_selftest_library.pdf`
- `01_drivers_and_hal/AN4733_stm32f2_firmware_examples_ref.pdf`
- `01_drivers_and_hal/AN5033_stm32h7_firmware_examples.pdf`
- `01_drivers_and_hal/AN5617_dualcore_ipc_openamp.pdf`
- `01_drivers_and_hal/STM32CubeH7/Documentation/STM32CubeH7GettingStarted.pdf`
- `01_drivers_and_hal/STM32CubeH7/Middlewares/ST/STM32_Network_Library/Software_License_Agreement_SLA0044.pdf`
- `01_drivers_and_hal/STM32CubeH7/Middlewares/Third_Party/OpenAMP/open-amp/docs/openamp_ref.pdf`
- `01_drivers_and_hal/UM1713_lwip_tcpip_stack.pdf`
- `01_drivers_and_hal/UM1734_usb_device_library.pdf`
- `01_drivers_and_hal/UM2237_stm32cubeprogrammer_user_manual.pdf`
- `01_drivers_and_hal/UM2609_stm32cubeide_user_guide.pdf`
- `02_board_hardware/DB_nucleo-h755zi-q_databrief.pdf`
- `02_board_hardware/MB1363_nucleo_h755zi_q_schematics.pdf`
- `02_board_hardware/UM2408_nucleo144_board_manual.pdf`

The four `rm0399_system/` entries here are deliberate: front matter,
documentation conventions, the security notice and the revision history were
never dumped because they only add noise to search results.

## Inventory


### `00_docs_stm32h7/docs/00_core_and_system/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `DS12923_stm32h755zi_datasheet.pdf` | 4883 KB | yes |
| `ES0445_stm32h755_errata_sheet.pdf` | 2324 KB | yes |
| `PM0214_cortex_m4_programming_manual.pdf` | 2669 KB | - |
| `PM0253_cortex_m7_programming_manual.pdf` | 2693 KB | - |
| `RM0399_stm32h755_reference_manual_full.pdf` | 72835 KB | - |

### `00_docs_stm32h7/docs/00_core_and_system/app_notes/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `AN2867_oscillator_design_guide.pdf` | 3133 KB | - |
| `AN4992_secure_firmware_install.pdf` | 2396 KB | - |
| `AN5419_h7_hardware_development_guide.pdf` | 1459 KB | - |

### `00_docs_stm32h7/docs/00_core_and_system/datasheets_variants/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `stm32h745zg_datasheet.pdf` | 4876 KB | - |
| `stm32h755bi_datasheet.pdf` | 4883 KB | - |

### `00_docs_stm32h7/docs/00_core_and_system/rm0399_system/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `00_front-matter.pdf` | 613 KB | - |
| `01_documentation-conventions.pdf` | 97 KB | - |
| `02_memory-and-bus-architecture.pdf` | 391 KB | yes |
| `03_ram-ecc-monitoring-ramecc.pdf` | 172 KB | yes |
| `04_embedded-flash-memory-flash.pdf` | 679 KB | yes |
| `05_secure-memory-management-smm.pdf` | 274 KB | yes |
| `06_art-accelerator.pdf` | 177 KB | yes |
| `07_power-control-pwr.pdf` | 698 KB | yes |
| `08_low-power-d3-domain-application-example.pdf` | 237 KB | yes |
| `14_block-interconnect.pdf` | 232 KB | yes |
| `63_debug-infrastructure.pdf` | 1327 KB | yes |
| `64_device-electronic-signature.pdf` | 96 KB | yes |
| `65_important-security-notice.pdf` | 116 KB | - |
| `66_revision-history.pdf` | 335 KB | - |

### `00_docs_stm32h7/docs/00_core_and_system/safety/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `UM2331_singlecore_safety_manual.pdf` | 7408 KB | - |
| `UM2840_dualcore_safety_manual.pdf` | 4686 KB | - |
| `UM3252_selftest_library.pdf` | 2406 KB | - |

### `00_docs_stm32h7/docs/01_drivers_and_hal/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `AN4733_stm32f2_firmware_examples_ref.pdf` | 487 KB | - |
| `AN5033_stm32h7_firmware_examples.pdf` | 647 KB | - |
| `AN5617_dualcore_ipc_openamp.pdf` | 504 KB | - |

### `00_docs_stm32h7/docs/01_drivers_and_hal/STM32CubeH7/Documentation/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `STM32CubeH7GettingStarted.pdf` | 1085 KB | - |

### `00_docs_stm32h7/docs/01_drivers_and_hal/STM32CubeH7/Middlewares/ST/STM32_Network_Library/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `Software_License_Agreement_SLA0044.pdf` | 84 KB | - |

### `00_docs_stm32h7/docs/01_drivers_and_hal/STM32CubeH7/Middlewares/Third_Party/OpenAMP/open-amp/docs/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `openamp_ref.pdf` | 1103 KB | - |

### `00_docs_stm32h7/docs/01_drivers_and_hal/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `UM1713_lwip_tcpip_stack.pdf` | 941 KB | - |
| `UM1734_usb_device_library.pdf` | 1485 KB | - |
| `UM2217_stm32h7_hal_ll_manual.pdf` | 12340 KB | yes |
| `UM2237_stm32cubeprogrammer_user_manual.pdf` | 10836 KB | - |
| `UM2609_stm32cubeide_user_guide.pdf` | 25229 KB | - |

### `00_docs_stm32h7/docs/02_board_hardware/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `DB_nucleo-h755zi-q_databrief.pdf` | 15238 KB | - |
| `MB1363_nucleo_h755zi_q_schematics.pdf` | 5114 KB | - |
| `UM2408_nucleo144_board_manual.pdf` | 4570 KB | - |

### `00_docs_stm32h7/docs/peripherals/adc_dac/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `26_analog-to-digital-converters-adc.pdf` | 1102 KB | yes |
| `27_digital-to-analog-converter-dac.pdf` | 386 KB | yes |
| `28_voltage-reference-buffer-vrefbuf.pdf` | 126 KB | yes |
| `29_comparator-comp.pdf` | 250 KB | yes |
| `30_operational-amplifiers-opamp.pdf` | 244 KB | yes |

### `00_docs_stm32h7/docs/peripherals/crc_cec_swpmi/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `22_cyclic-redundancy-check-calculation-unit-crc.pdf` | 154 KB | yes |
| `56_single-wire-protocol-master-interface-swpmi.pdf` | 325 KB | yes |
| `62_hdmi-cec-controller-cec.pdf` | 259 KB | yes |

### `00_docs_stm32h7/docs/peripherals/crypto_hash_rng/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `36_true-random-number-generator-rng.pdf` | 268 KB | yes |
| `37_cryptographic-processor-cryp.pdf` | 652 KB | yes |
| `38_hash-processor-hash.pdf` | 282 KB | yes |

### `00_docs_stm32h7/docs/peripherals/dfsdm_camera/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `31_digital-filter-for-sigma-delta-modulators-dfsdm.pdf` | 539 KB | yes |
| `32_digital-camera-interface-dcmi.pdf` | 308 KB | yes |

### `00_docs_stm32h7/docs/peripherals/display_graphics/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `19_chrom-art-accelerator-controller-dma2d.pdf` | 309 KB | yes |
| `33_lcd-tft-display-controller-ltdc.pdf` | 301 KB | yes |
| `34_dsi-host-dsi.pdf` | 1046 KB | yes |
| `35_jpeg-codec-jpeg.pdf` | 222 KB | yes |

### `00_docs_stm32h7/docs/peripherals/dma_mdma/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `15_mdma-controller-mdma.pdf` | 272 KB | yes |
| `16_direct-memory-access-controller-dma.pdf` | 323 KB | yes |
| `17_basic-direct-memory-access-controller-bdma.pdf` | 235 KB | yes |
| `18_dma-request-multiplexer-dmamux.pdf` | 250 KB | yes |

### `00_docs_stm32h7/docs/peripherals/ethernet/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `57_management-data-input-output-mdios.pdf` | 195 KB | yes |
| `61_ethernet-eth-media-access-control-mac-with-dma-controller.pdf` | 1679 KB | yes |

### `00_docs_stm32h7/docs/peripherals/fdcan/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `59_controller-area-network-with-flexible-data-rate-fdcan.pdf` | 840 KB | yes |

### `00_docs_stm32h7/docs/peripherals/fmc_quadspi/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `23_flexible-memory-controller-fmc.pdf` | 662 KB | yes |
| `24_quad-spi-interface-quadspi.pdf` | 296 KB | yes |
| `25_delay-block-dlyb.pdf` | 151 KB | yes |

### `00_docs_stm32h7/docs/peripherals/gpio/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `12_general-purpose-i-os-gpio.pdf` | 269 KB | yes |
| `13_system-configuration-controller-syscfg.pdf` | 213 KB | yes |

### `00_docs_stm32h7/docs/peripherals/hsem_dualcore/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `11_hardware-semaphore-hsem.pdf` | 200 KB | yes |

### `00_docs_stm32h7/docs/peripherals/i2c/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `50_inter-integrated-circuit-i2c-interface.pdf` | 643 KB | yes |

### `00_docs_stm32h7/docs/peripherals/interrupts_exti/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `20_nested-vectored-interrupt-controllers-nvic1-and-nvic2.pdf` | 168 KB | yes |
| `21_extended-interrupt-and-event-controller-exti.pdf` | 312 KB | yes |

### `00_docs_stm32h7/docs/peripherals/rcc/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `09_reset-and-clock-control-rcc.pdf` | 1152 KB | yes |
| `10_clock-recovery-system-crs.pdf` | 193 KB | yes |

### `00_docs_stm32h7/docs/peripherals/sdmmc/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `58_secure-digital-input-output-multimediacard-interface-sdmmc.pdf` | 690 KB | yes |

### `00_docs_stm32h7/docs/peripherals/spi/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `53_serial-peripheral-interface-spi.pdf` | 778 KB | yes |
| `54_serial-audio-interface-sai.pdf` | 592 KB | yes |
| `55_spdif-receiver-interface-spdifrx.pdf` | 368 KB | yes |

### `00_docs_stm32h7/docs/peripherals/timers_pwm/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `39_high-resolution-timer-hrtim.pdf` | 1167 KB | yes |
| `40_advanced-control-timers-tim1-tim8.pdf` | 882 KB | yes |
| `41_general-purpose-timers-tim2-tim3-tim4-tim5.pdf` | 672 KB | yes |
| `42_general-purpose-timers-tim12-tim13-tim14.pdf` | 463 KB | yes |
| `43_general-purpose-timers-tim15-tim16-tim17.pdf` | 648 KB | yes |
| `44_basic-timers-tim6-tim7.pdf` | 212 KB | yes |
| `45_low-power-timer-lptim.pdf` | 289 KB | yes |

### `00_docs_stm32h7/docs/peripherals/usart_uart/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `51_universal-synchronous-asynchronous-receiver-transmitter-usar.pdf` | 685 KB | yes |
| `52_low-power-universal-asynchronous-receiver-transmitter-lpuart.pdf` | 537 KB | yes |

### `00_docs_stm32h7/docs/peripherals/usb/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `60_usb-on-the-go-high-speed-otg_hs.pdf` | 2417 KB | yes |

### `00_docs_stm32h7/docs/peripherals/watchdog_rtc/`

| File | Size | .txt twin tracked |
| :--- | ---: | :---: |
| `46_watchdog-overview.pdf` | 149 KB | yes |
| `47_system-window-watchdog-wwdg.pdf` | 185 KB | yes |
| `48_independent-watchdog-iwdg.pdf` | 169 KB | yes |
| `49_real-time-clock-rtc.pdf` | 375 KB | yes |
