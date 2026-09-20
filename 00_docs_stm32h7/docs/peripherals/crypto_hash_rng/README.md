# Cryptography, HASH & RNG - STM32H755

## 1. Documentation
- `36_true-random-number-generator-rng.pdf` - NIST SP 800-90B compliant TRNG
- `37_cryptographic-processor-cryp.pdf` - AES 128/192/256, DES/TDES
- `38_hash-processor-hash.pdf` - SHA-1, SHA-224, SHA-256, MD5, HMAC

---

## 2. Availability - Read This First

The crypto accelerator is **part-number dependent**:

| Part | CRYP (AES/DES) | HASH | RNG |
| :--- | :---: | :---: | :---: |
| STM32H745 / H747 | **no** | no | yes |
| **STM32H755 / H757** | **yes** | **yes** | yes |

**STM32H755ZI has all three.** Code written for this device must not be assumed portable
to an H745; guard it with `#if defined(CRYP)` if the codebase targets both.

All three sit in the **D2 domain** (AHB2).

---

## 3. RNG - True Random Number Generator

### Clocking (the usual failure point)

`rng_ker_ck` must be **at least 1/16 of `hclk`**, or the RNG raises a clock error
(`CECS`) and refuses to produce output.

```
rng_ker_ck  <-  hsi48_ck (default)  |  pll1_q_ck  |  lse_ck
                selected by RNGSEL in RCC_D2CCIP2R
```

`hsi48_ck` is the normal choice. With `hclk` at 240 MHz, `48 >= 240/16 = 15`: valid.

```c
RCC_PeriphCLKInitTypeDef p = {0};
p.PeriphClockSelection = RCC_PERIPHCLK_RNG;
p.RngClockSelection    = RCC_RNGCLKSOURCE_HSI48;
HAL_RCCEx_PeriphCLKConfig(&p);
__HAL_RCC_RNG_CLK_ENABLE();
```

### Error handling is mandatory

The NIST SP 800-90B entropy source runs continuous health tests. A seed error
(`SECS`/`SEIS`) must be recovered from, not ignored:

```c
uint32_t v;
if (HAL_RNG_GenerateRandomNumber(&hrng, &v) != HAL_OK) {
    /* Seed or clock error: clear, re-seed, retry. Never fall back to rand(). */
    __HAL_RNG_CLEAR_IT(&hrng, RNG_IT_SEI | RNG_IT_CEI);
    HAL_RNG_DeInit(&hrng);
    HAL_RNG_Init(&hrng);
}
```

Discard the **first** word after enabling the peripheral, as the reference manual
requires. Never use RNG output that arrived with an error flag set - that is how a key
ends up predictable.

---

## 4. CRYP - AES / DES / TDES

Supported chaining modes: **ECB, CBC, CTR, GCM, GMAC, CCM, CFB**.

- Prefer **GCM** or **CCM**: they authenticate as well as encrypt. Raw CBC or ECB with no
  MAC is malleable.
- **Never ECB** for anything longer than one block.
- Key and IV registers are write-only. Keep the key in a buffer you can zeroise, and
  zeroise it (`memset` via a `volatile` pointer, or `__DSB()` after) once loaded.
- DES and TDES are present for legacy interop only. Do not design new protocols with
  them.

DMA is supported for bulk encryption directly from memory - see the cache section below.

---

## 5. HASH - SHA-1 / SHA-224 / SHA-256 / MD5 / HMAC

- Hardware HMAC is supported directly; no software wrapper needed.
- MD5 and SHA-1 are provided for legacy compatibility. Use **SHA-256** for anything new.
- Context save/restore lets a long hash be suspended and resumed, which matters when the
  HASH peripheral is shared between tasks or cores.

### ERRATA ES0445 2.3.12 - HASH input data may be corrupted when DMA is used (`A`)

**This one silently produces wrong digests.** A HASH transfer on DMA1 is corrupted if
DMA2 channels are simultaneously active (and vice versa).

**Workaround** - pick one:

1. Ensure no channel on the *other* DMA controller is enabled during the HASH transfer,
   **and** set **bit 20 of `DMA_SxCR`** for the stream carrying HASH data:
   ```c
   #define DMA_SxCR_ALT_PROTOCOL   (1UL << 20)
   hdma_hash.Instance->CR |= DMA_SxCR_ALT_PROTOCOL;
   ```
2. **Use interrupt or polling mode for HASH** when the other controller cannot be
   quiesced. In a system with active UART/SPI/ETH DMA this is usually the only realistic
   option.

In practice, option 2 is the safe default: a corrupted digest fails open, and a system
that also runs Ethernet or UART DMA cannot guarantee condition 1.

Full text: [`../../00_core_and_system/errata_summary.md`](../../00_core_and_system/errata_summary.md).

---

## 6. DMA, Memory & Cache

CRYP and HASH are in **D2**, so their DMA buffers follow the standard D2 rules:

- Buffers must be reachable by DMA1/DMA2: **D2 SRAM** or AXI SRAM, **never DTCM**.
- Place them in the non-cacheable `.dma_buf` section:
  ```c
  uint8_t plaintext[256]  __attribute__((section(".dma_buf"), aligned(32)));
  uint8_t ciphertext[256] __attribute__((section(".dma_buf"), aligned(32)));
  ```
- If a buffer must live in cacheable AXI SRAM, clean before the transfer and invalidate
  after, each followed by `__DMB()` (**ERRATA ES0445 2.1.5**).

Security note: plaintext and keys left in a cacheable region persist in the D-Cache after
you clear RAM. Zeroise the buffer **and** issue `SCB_CleanInvalidateDCache_by_Addr()`
before considering the secret gone.

See [`../../00_core_and_system/linker_templates/README.md`](../../00_core_and_system/linker_templates/README.md).

---

## 7. Dual-Core Notes

CRYP, HASH and RNG are **shared D2 peripherals with no built-in arbitration**. Two cores
using the same engine interleave their block operations and both get garbage.

- Assign each engine to **one owner core**, or
- Guard every operation with an HSEM, taking it for the whole message, not per block.

```c
SHARED_LOCK(HSEM_ID_CRYPTO);
/* full encrypt / hash operation here */
SHARED_UNLOCK(HSEM_ID_CRYPTO);
```

See [`../hsem_dualcore/README.md`](../hsem_dualcore/README.md) and
[`../../00_core_and_system/linker_templates/shared_memory.h`](../../00_core_and_system/linker_templates/shared_memory.h).

---

## 8. Related Security Documentation

- Secure firmware install: [`../../00_core_and_system/app_notes/AN4992_secure_firmware_install.pdf`](../../00_core_and_system/app_notes/AN4992_secure_firmware_install.pdf)
- Secure memory management (SMM): [`../../00_core_and_system/rm0399_system/05_secure-memory-management-smm.pdf`](../../00_core_and_system/rm0399_system/05_secure-memory-management-smm.pdf)
- Readout protection (RDP) levels and the **ES0445 2.3.2** stall on consecutive level
  regressions: [`../../00_core_and_system/errata_summary.md`](../../00_core_and_system/errata_summary.md)

---

## 9. Debug Checklist

| Symptom | Likely cause |
| :--- | :--- |
| RNG returns errors immediately | `rng_ker_ck` below `hclk/16`, or clock not enabled |
| RNG occasionally returns 0 | error flags not checked before using the value |
| HASH digest wrong only under load | **errata 2.3.12** - HASH on DMA with the other controller active |
| CRYP output all zeros | key/IV written after enabling, or buffer in DTCM |
| Works on one core, breaks when both run | no HSEM around the engine |
| Code fails to build on an H745 | CRYP/HASH do not exist on that part |
