# USB (OTG_HS & OTG_FS) - STM32H755

## 1. Documentation
- Chapter PDF: `60_usb-on-the-go-high-speed-otg_hs.pdf`
- Device class library: [`../../01_drivers_and_hal/UM1734_usb_device_library.pdf`](../../01_drivers_and_hal/UM1734_usb_device_library.pdf)

---

## 2. The Two Controllers

| | **USB1_OTG_HS** | **USB2_OTG_FS** |
| :--- | :--- | :--- |
| Domain | D2 | D2 |
| With internal FS PHY | 12 Mbps (Full Speed) | 12 Mbps (Full Speed) |
| With external ULPI PHY | **480 Mbps (High Speed)** | not supported |
| FIFO RAM | 4 KB | 1.25 KB |
| Device endpoints | 9 IN / 9 OUT | 6 IN / 6 OUT |
| Modes | Device, Host, OTG | Device, Host, OTG |

Naming is a common source of confusion: **OTG_HS running on its internal PHY is a
Full-Speed port**. High Speed requires an external ULPI PHY (USB3300 or similar) wired
to 12 pins, which the Nucleo does not have.

---

## 3. Nucleo-H755ZI-Q Wiring (CN13 micro-AB)

The board's user USB connector is **OTG_FS**, verified against
[`MB1363_nucleo_h755zi_q_schematics.pdf`](../../02_board_hardware/MB1363_nucleo_h755zi_q_schematics.pdf):

| Signal | Pin | AF | Schematic net |
| :--- | :--- | :---: | :--- |
| `OTG_FS_DM` | `PA11` | **AF10** | `USB_FS_N` |
| `OTG_FS_DP` | `PA12` | **AF10** | `USB_FS_P` |
| `OTG_FS_ID` | `PA10` | AF10 | `USB_FS_ID` |
| `VBUS` sense | `PA9` | analog / no AF | `USB_FS_VBUS` |
| Power switch enable | `PD10` | GPIO out | `USB_FS_PWR_EN` |
| Over-current input | `PG7` | GPIO in | `USB_FS_OVCR` |
| `OTG_FS_SOF` (optional) | `PA8` | AF10 | shared with `MCO1` |

```c
GPIO_InitTypeDef g = {0};
g.Pin       = GPIO_PIN_11 | GPIO_PIN_12;
g.Mode      = GPIO_MODE_AF_PP;
g.Pull      = GPIO_NOPULL;
g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
g.Alternate = GPIO_AF10_OTG1_FS;     /* AF10 - see CMSIS header naming */
HAL_GPIO_Init(GPIOA, &g);

/* PA9 stays analog / input: it is a VBUS sense line, not an AF pin */
```

### OTG_HS pinout (internal FS PHY), for reference

| Signal | Pin | AF |
| :--- | :--- | :---: |
| `OTG_HS_DM` | `PB14` | AF12 |
| `OTG_HS_DP` | `PB15` | AF12 |
| `OTG_HS_ID` | `PB12` | AF12 |

> **Conflict on this board**: `PB14` is wired to **LD3 (red LED)**. Using OTG_HS on the
> Nucleo means giving up LD3 and adding an external connector. Prefer OTG_FS.

ULPI pins for true High Speed: `PA3`, `PA5`, `PB0`, `PB1`, `PB5`, `PB10`-`PB13`, `PC0`,
`PC2`, `PC3` (all AF10). Full list in
[`../../../STM32H755_Alternate_Functions.md`](../../../STM32H755_Alternate_Functions.md).

---

## 4. Clocking - 48 MHz Is Mandatory

The USB kernel clock must be **exactly 48 MHz** with tight jitter, or enumeration fails
intermittently and the host reports "device descriptor request failed".

| Source | Selected by | Notes |
| :--- | :--- | :--- |
| `pll3_q_ck` | `USBSEL` in `RCC_D2CCIP2R` | **preferred**: PLL3 can be dedicated to USB |
| `pll1_q_ck` | `USBSEL` | only if the system PLL yields exactly 48 MHz |
| `hsi48_ck` | `USBSEL` | needs **CRS** trimming against USB SOF to stay in spec |

```c
/* PLL3 -> 48 MHz for USB, independent of the system clock tree */
RCC_PeriphCLKInitTypeDef p = {0};
p.PeriphClockSelection = RCC_PERIPHCLK_USB;
p.PLL3.PLL3M = 5;  p.PLL3.PLL3N = 192;  p.PLL3.PLL3Q = 20;   /* -> 48 MHz */
p.PLL3.PLL3P = 2;  p.PLL3.PLL3R = 2;
p.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
p.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
p.UsbClockSelection = RCC_USBCLKSOURCE_PLL3;
HAL_RCCEx_PeriphCLKConfig(&p);
```

Recompute the PLL3 dividers for your actual HSE. The Nucleo-H755ZI-Q ships with the
ST-LINK MCO as clock source (8 MHz) unless the solder bridges are changed; confirm in
the board manual before copying dividers.

---

## 5. The H7-Specific Trap: USB Voltage Detector

On STM32H7 the USB transceiver is supplied through a dedicated regulator that is **off
after reset**. Without these two calls the peripheral initialises cleanly and then
never enumerates:

```c
HAL_PWREx_EnableUSBVoltageDetector();   /* enable USB33 detector */
/* If the board has no VBUS sense wiring, bypass the detector instead: */
/* HAL_PWREx_EnableUSBReg();  +  USB_OTG_FS->GCCFG |= USB_OTG_GCCFG_VBDEN; */
```

Also enable the transceiver clocks:

```c
__HAL_RCC_USB2_OTG_FS_CLK_ENABLE();
__HAL_RCC_USB2_OTG_FS_ULPI_CLK_DISABLE();   /* internal PHY: ULPI clock OFF */
```

Leaving the ULPI clock enabled while using the internal PHY is the second most common
cause of a dead USB port on this family.

---

## 6. FIFO Configuration

The OTG core has no DMA into system RAM in device mode on the internal PHY; data moves
through the dedicated FIFO RAM. Budget it explicitly:

```c
/* Sizes in 32-bit words. OTG_FS total = 320 words (1.25 KB). */
HAL_PCDEx_SetRxFiFo(&hpcd, 128);      /* shared RX     */
HAL_PCDEx_SetTxFiFo(&hpcd, 0, 64);    /* EP0 IN        */
HAL_PCDEx_SetTxFiFo(&hpcd, 1, 64);    /* EP1 IN (CDC)  */
HAL_PCDEx_SetTxFiFo(&hpcd, 2, 64);    /* EP2 IN        */
```

The sum must not exceed the controller's FIFO RAM (320 words for OTG_FS, 1024 for
OTG_HS). Over-allocating does not fail loudly: endpoints simply drop packets.

---

## 7. Cache & Memory Notes

- Device mode on the **internal PHY** copies through the FIFO, so class buffers do not
  need to be DMA-reachable and D-Cache maintenance is not required for them.
- **OTG_HS in DMA mode** (ULPI, or host mode with `dma_enable = ENABLE`) is a real bus
  master: its buffers must live in **D2 SRAM** and be 32-byte aligned. Place them in
  `.dma_buf`. See
  [`../../00_core_and_system/linker_templates/README.md`](../../00_core_and_system/linker_templates/README.md).
- `USBD_malloc` / the class handle allocated on the heap ends up in DTCM on CPU1. Fine
  for the internal PHY, **not** fine once DMA is enabled.

---

## 8. Errata (ES0445)

| # | Limitation | Practical effect |
| :--- | :--- | :--- |
| 2.24.5 | Unexpected transfer instead of a zero-length packet (`A`) | CDC and control transfers relying on ZLPs can stall - send an explicit ZLP |
| 2.24.2 | Isochronous IN endpoint disabled mid packet fetch (`A`) | audio / video class streams drop frames |
| 2.24.6 | False chirp-K detection when an FS device connects (`A`) | host mode enumeration |
| 2.24.3 | Wrong interrupt mask for battery charger status (`A`) | only if BCD is used |
| 2.24.4 | False `OTG_GINTSTS.CIDSCHG` after reset (`A`) | ignore the first ID-change interrupt |
| 2.24.7 | ULPI access near a micro-frame boundary delays the next SOF (`A`) | HS/ULPI designs only |
| 2.24.1 | Host hang: FS interface -> hub -> low-speed device (`N`) | **no workaround** - avoid low-speed devices behind a hub |

Full text: [`../../00_core_and_system/errata_summary.md`](../../00_core_and_system/errata_summary.md).

---

## 9. Dual-Core Notes

USB sits in **D2**, the M4's domain, but either core can drive it. Pick one:

- **CPU1 (M7)** if the USB payload feeds M7 processing - avoids shipping data across
  the shared block.
- **CPU2 (M4)** for a pure CDC or HID interface, freeing the M7 for control work.

Whichever core owns it, enable `OTG_FS_IRQn` on **that core's NVIC only**, and make sure
that core enables the D2 clocks. Remember the M7 must not put D2 into a low-power state
while the M4 is servicing USB.

---

## 10. Debug Checklist

| Symptom | Likely cause |
| :--- | :--- |
| Host reports "device descriptor request failed" | kernel clock is not exactly 48 MHz, or jitter too high |
| Peripheral inits OK, no enumeration at all | `HAL_PWREx_EnableUSBVoltageDetector()` missing |
| Enumerates only when powered from the ST-LINK port | VBUS sense on `PA9` not wired / detector not bypassed |
| Works, then stalls on large transfers | TX FIFO too small for the endpoint's max packet size |
| Random failures with DMA enabled | buffers in DTCM or unaligned - move to `.dma_buf` |
| Red LED behaves oddly with USB | `PB14` used for OTG_HS_DM while LD3 is fitted |

---

## 11. Cross-References
- Pin multiplexing: [`../../../STM32H755_Alternate_Functions.md`](../../../STM32H755_Alternate_Functions.md)
- Board schematics: [`../../02_board_hardware/MB1363_nucleo_h755zi_q_schematics.pdf`](../../02_board_hardware/MB1363_nucleo_h755zi_q_schematics.pdf)
- Clock tree: [`../rcc/README.md`](../rcc/README.md)
- Class library: [`../../01_drivers_and_hal/UM1734_usb_device_library.pdf`](../../01_drivers_and_hal/UM1734_usb_device_library.pdf)
