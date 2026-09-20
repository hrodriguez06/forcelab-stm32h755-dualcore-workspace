# CRC, HDMI-CEC & SWPMI - STM32H755

## 1. Documentation
- `22_cyclic-redundancy-check-calculation-unit-crc.pdf`
- `56_single-wire-protocol-master-interface-swpmi.pdf`
- `62_hdmi-cec-controller-cec.pdf`

These three unrelated peripherals share a folder because each is small. **CRC is the one
you will actually use**; CEC and SWPMI need external hardware not present on the Nucleo.

---

## 2. CRC Calculation Unit

Hardware CRC in **D1** (AHB1). Fully programmable, which makes it useful well beyond the
default Ethernet polynomial.

| Parameter | Options |
| :--- | :--- |
| Polynomial length | 7, 8, 16, **32** bit |
| Polynomial | programmable (`CRC_POL`) |
| Initial value | programmable (`CRC_INIT`) |
| Input reversal | none / byte / half-word / word |
| Output reversal | none / bit-reversed |
| Input data width | 8, 16, 32 bit |

### Default configuration = Ethernet CRC-32

```c
CRC_HandleTypeDef hcrc;

hcrc.Instance                     = CRC;
hcrc.Init.DefaultPolynomialUse    = DEFAULT_POLYNOMIAL_ENABLE;  /* 0x04C11DB7 */
hcrc.Init.DefaultInitValueUse     = DEFAULT_INIT_VALUE_ENABLE;  /* 0xFFFFFFFF */
hcrc.Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_NONE;
hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
hcrc.InputDataFormat              = CRC_INPUTDATA_FORMAT_BYTES;
HAL_CRC_Init(&hcrc);

uint32_t crc = HAL_CRC_Calculate(&hcrc, (uint32_t *)data, len);
```

### Matching a software CRC (the usual frustration)

Standard **CRC-32 (zlib / PKZIP)** needs input *and* output reversal plus a final XOR
that the hardware does not apply:

```c
hcrc.Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_BYTE;
hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_ENABLE;
/* ... */
uint32_t crc32 = HAL_CRC_Calculate(&hcrc, buf, len) ^ 0xFFFFFFFFU;
```

If a hardware CRC does not match a reference implementation, the cause is almost always
reversal or the final XOR, not the polynomial.

### Accumulation

`HAL_CRC_Accumulate()` continues from the current value instead of reloading `CRC_INIT`,
which lets a CRC be built across several buffers or streamed as data arrives.

### Where CRC earns its place in this project

- **Firmware image integrity** at boot, before jumping to an application.
- **Shared-memory validation** between M7 and M4: append a CRC to the message in
  `.shared_ram` so the reader can detect a torn write. See
  [`../../00_core_and_system/linker_templates/shared_memory.h`](../../00_core_and_system/linker_templates/shared_memory.h).
- **SD card data integrity**: **ERRATA ES0445 2.9.1** means the SDMMC does not check
  response and data end bits, so a corrupted block can be accepted as valid. Storing a
  CRC with each record is the recommended mitigation. See
  [`../sdmmc/README.md`](../sdmmc/README.md).
- **IEC 60730 Class B** self-test of Flash contents. See
  [`../../00_core_and_system/safety/UM3252_selftest_library.pdf`](../../00_core_and_system/safety/UM3252_selftest_library.pdf).

### Dual-core warning

There is **one** CRC unit and it is **stateful**: `CRC_DR` accumulates. If both cores use
it without arbitration, each corrupts the other's result with no error indication.

- Assign it to one core, **or**
- Take an HSEM around the *entire* calculation, not per word:

```c
SHARED_LOCK(HSEM_ID_CRC);
__HAL_CRC_DR_RESET(&hcrc);
uint32_t crc = HAL_CRC_Calculate(&hcrc, buf, len);
SHARED_UNLOCK(HSEM_ID_CRC);
```

The same applies to preemption within one core: an ISR that uses the CRC unit will
corrupt a calculation running in a task. Keep CRC use in one context, or disable
interrupts around it.

---

## 3. HDMI-CEC

Consumer Electronics Control: the low-speed single-wire bus that lets HDMI devices
control each other (a TV powering on a set-top box, for example).

- Clocked from **LSE (32.768 kHz)** or **HSI/488**, selected by `CECSEL` in
  `RCC_D2CCIP2R`.
- Runs at roughly 400 bit/s - it is a control channel, not a data path.
- Requires an external CEC line with pull-up and level shifting to the HDMI connector.
  **Not present on the Nucleo-H755ZI-Q.**

### Errata

| # | Limitation | Effect |
| :--- | :--- | :--- |
| 2.26.1 | Missed CEC messages in normal receiving mode (`A`) | implement application-level retry / acknowledgement |
| 2.26.2 | Unexpected `TXERR` flag during transmission (`A`) | do not abort on the first `TXERR`; validate against the actual bus state |

---

## 4. SWPMI

Single Wire Protocol Master Interface: the contactless front-end bus used to talk to an
eSE / SIM secure element (NFC card emulation).

- Half-duplex, single-wire, with its own transceiver.
- Supports DMA for frame transfer.
- Needs an external SWP-capable secure element. **Not present on the Nucleo-H755ZI-Q.**

If SWPMI is used with DMA, its buffers follow the ordinary D2 rules: place them in
`.dma_buf`, 32-byte aligned, never in DTCM.

---

## 5. Cross-References
- Shared memory and HSEM: [`../hsem_dualcore/README.md`](../hsem_dualcore/README.md)
- SDMMC integrity mitigation: [`../sdmmc/README.md`](../sdmmc/README.md)
- Errata: [`../../00_core_and_system/errata_summary.md`](../../00_core_and_system/errata_summary.md)
- Safety / Class B: [`../../00_core_and_system/safety/`](../../00_core_and_system/safety/)
