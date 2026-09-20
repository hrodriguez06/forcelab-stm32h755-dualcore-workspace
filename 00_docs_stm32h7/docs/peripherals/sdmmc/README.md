# SDMMC (SD / SDIO / MMC Host Interface) - STM32H755

## 1. Documentation
- Chapter PDF: `58_secure-digital-input-output-multimediacard-interface-sdmmc.pdf`
- Delay block (required above 50 MHz): `../fmc_quadspi/25_delay-block-dlyb.pdf`

---

## 2. Instances, Domain & Capabilities

| | SDMMC1 | SDMMC2 |
| :--- | :--- | :--- |
| Domain | **D1** | D2 |
| Bus | AHB3 | AHB2 |
| Max bus width | 8-bit | 8-bit |
| Internal DMA (IDMA) | yes | yes |

- SD Memory Card spec **v4.2**: SDR12, SDR25, SDR50, **SDR104**, DDR50.
- e-MMC spec **v5.1** (HS200 / HS400 depending on voltage).
- Kernel clock up to **200 MHz** (SDR104).
- **IDMA is built in**: SDMMC does not consume a DMA1/DMA2 stream.

---

## 3. Clocking

```
sdmmc_ker_ck  <-  pll1_q_ck  (default)  or  pll2_r_ck
                  selected by SDMMCSEL in RCC_D1CCIPR
```

- `SDMMC_CK = sdmmc_ker_ck / (2 * CLKDIV)`, or `= sdmmc_ker_ck` when `CLKDIV = 0`.
- Keep `sdmmc_ker_ck` at or below **200 MHz**. Above ~50 MHz the **DLYB** (delay block)
  must be tuned to align the sampling phase, otherwise reads fail intermittently at
  temperature extremes.
- Identification phase must run at **400 kHz or less**. The HAL handles this in
  `HAL_SD_Init()`; do not skip straight to the high-speed divider.

```c
RCC_PeriphCLKInitTypeDef p = {0};
p.PeriphClockSelection = RCC_PERIPHCLK_SDMMC;
p.SdmmcClockSelection  = RCC_SDMMCCLKSOURCE_PLL;   /* pll1_q_ck */
HAL_RCCEx_PeriphCLKConfig(&p);
```

---

## 4. Pinout (STM32H755ZI)

**SDMMC1 - all AF12**, the standard 4-bit SD configuration:

| Signal | Pin | AF |
| :--- | :--- | :---: |
| `SDMMC1_CK` | `PC12` | AF12 |
| `SDMMC1_CMD` | `PD2` | AF12 |
| `SDMMC1_D0` | `PC8` | AF12 |
| `SDMMC1_D1` | `PC9` | AF12 |
| `SDMMC1_D2` | `PC10` | AF12 |
| `SDMMC1_D3` | `PC11` | AF12 |
| `SDMMC1_D4..D7` (8-bit) | `PB8`, `PB9`, `PC6`, `PC7` | AF12 |

Voltage-switch transceiver signals (UHS-I only): `SDMMC1_CKIN` `PB8` AF7,
`SDMMC1_CDIR` `PB9` AF7, `SDMMC1_D0DIR` `PC6` AF8, `SDMMC1_D123DIR` `PC7` AF8.

**SDMMC2**: `SDMMC2_D0` `PB14` AF9, `SDMMC2_D1` `PB15` AF9, plus `PD6`/`PD7`/`PG9`-`PG12`
depending on the mapping chosen. Cross-check every pin in
[`../../../STM32H755_Alternate_Functions.md`](../../../STM32H755_Alternate_Functions.md).

### GPIO configuration

```c
GPIO_InitTypeDef g = {0};
g.Mode      = GPIO_MODE_AF_PP;
g.Pull      = GPIO_PULLUP;              /* CMD and D0-D3 need pull-ups */
g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH; /* mandatory above 25 MHz     */
g.Alternate = GPIO_AF12_SDMMC1;
g.Pin       = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
HAL_GPIO_Init(GPIOC, &g);
g.Pin = GPIO_PIN_2;
HAL_GPIO_Init(GPIOD, &g);
```

`GPIO_SPEED_FREQ_VERY_HIGH` is not optional: at lower slew rates the clock edge is too
slow and cards fail CRC at high bus speeds.

> The Nucleo-H755ZI-Q has **no SD card socket**. SDMMC1 signals are available on the
> ST Zio / Morpho connectors (`PC10` = D45, `PC12`, `PD2`). Check
> [`../../02_board_hardware/MB1363_nucleo_h755zi_q_schematics.pdf`](../../02_board_hardware/MB1363_nucleo_h755zi_q_schematics.pdf)
> for the exact connector pins before wiring a breakout.

---

## 5. Buffer Placement & Cache (CRITICAL)

The **SDMMC IDMA is an AHB master; it cannot reach DTCM or ITCM.** A buffer left in
the default `.bss` on CPU1 lands in DTCM and the transfer silently reads zeros.

| Buffer location | Works with IDMA | Cache maintenance |
| :--- | :---: | :--- |
| DTCM (`0x20000000`) | **NO** | - |
| AXI SRAM (`0x24000000`) | yes | **required** (cacheable) |
| D2 SRAM1/2/3 (`0x30000000`) | yes | not needed if non-cacheable via MPU |

**Recommended**: place SD buffers in the non-cacheable `.dma_buf` section defined in
[`../../00_core_and_system/linker_templates/`](../../00_core_and_system/linker_templates/README.md).

```c
/* SRAM1, non-cacheable, 32-byte aligned, size multiple of 32 */
uint8_t sd_buf[512] __attribute__((section(".dma_buf"), aligned(32)));
```

If the buffer must live in AXI SRAM (large FatFS cache), maintain the cache manually:

```c
/* Write: flush CPU data out before the card reads it */
SCB_CleanDCache_by_Addr((uint32_t *)sd_buf, sizeof(sd_buf));
__DMB();                                        /* ERRATA ES0445 2.1.5 */
HAL_SD_WriteBlocks_DMA(&hsd1, sd_buf, block, 1);

/* Read: drop stale cache lines after the card has written memory */
HAL_SD_ReadBlocks_DMA(&hsd1, sd_buf, block, 1);
/* ... wait for completion ... */
SCB_InvalidateDCache_by_Addr((uint32_t *)sd_buf, sizeof(sd_buf));
__DMB();                                        /* ERRATA ES0445 2.1.5 */
```

Additional constraints:
- Buffer address **4-byte aligned minimum**, 32-byte aligned in practice.
- Size a multiple of 32 bytes, so no adjacent variable shares a cache line.
- With FatFS, `f_read`/`f_write` on a user buffer that is not aligned makes the
  middleware fall back to its internal scratch buffer. Set
  `_USE_LFN`/`_MAX_SS` deliberately and align application buffers yourself.

---

## 6. Errata

### ES0445 2.9.1 - Command response and receive data end bits not checked (`N`)
**No workaround exists.** The SDMMC does not validate the end bit of command responses
or received data blocks, so a corrupted response can be accepted as valid.

**Mitigation at application level**: for data that matters, store a CRC32 or checksum
alongside each record and verify it after reading. The hardware CRC unit
([`../crc_cec_swpmi/`](../crc_cec_swpmi/README.md)) makes this cheap.

See [`../../00_core_and_system/errata_summary.md`](../../00_core_and_system/errata_summary.md).

---

## 7. Dual-Core Notes

- SDMMC1 sits in **D1**, so it is the natural CPU1 (M7) peripheral. SDMMC2 is in D2.
- If both cores must touch the same card, serialise access with an HSEM and keep the
  file system driver on a single core. FatFS is not reentrant across cores.
- Do not enable `SDMMC1_IRQn` on both NVICs.

---

## 8. Debug Checklist

| Symptom | Likely cause |
| :--- | :--- |
| `HAL_SD_Init()` returns error, card never responds | GPIO speed not `VERY_HIGH`, or missing pull-ups on CMD/D0-D3 |
| Init works, data transfers return zeros | Buffer in DTCM - IDMA cannot reach it |
| Data is correct once, then stale | Missing `SCB_InvalidateDCache_by_Addr()` + `__DMB()` |
| Works at low speed, CRC errors above 50 MHz | DLYB not tuned, or `sdmmc_ker_ck` above 200 MHz |
| Intermittent corruption near other variables | Buffer size not a multiple of 32 (cache line sharing) |

---

## 9. Cross-References
- Memory domains and DMA reachability: [`../../00_core_and_system/memory_and_domains.md`](../../00_core_and_system/memory_and_domains.md)
- Cache rules: [`../../00_core_and_system/cache_mpu_rules.md`](../../00_core_and_system/cache_mpu_rules.md)
- Linker sections: [`../../00_core_and_system/linker_templates/README.md`](../../00_core_and_system/linker_templates/README.md)
- HAL API: [`../../01_drivers_and_hal/UM2217_stm32h7_hal_ll_manual.pdf`](../../01_drivers_and_hal/UM2217_stm32h7_hal_ll_manual.pdf)
