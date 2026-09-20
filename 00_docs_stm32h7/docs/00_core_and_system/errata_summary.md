# STM32H755 - Silicon Errata Summary (Code-Impacting)

**Source**: `ES0445_stm32h755_errata_sheet.pdf` - Rev 6, September 2025.
**Applicability**: STM32H745xI/G, **STM32H755xI**, STM32H747xI/G, STM32H757xI.
**Silicon revisions covered**: `U`, `V` (`REV_ID` = `0x2003` in `DBGMCU_IDC`).

> This file is a **curated summary**: it lists only the errata that change the code
> you write. For the full list (including analog-only and documentation errata),
> open the PDF. Section numbers below map 1:1 to the PDF.

**Legend**: `A` = workaround available - `P` = partial workaround - `N` = no workaround.

---

## 0. Read This First (highest impact)

| # | Limitation | Impact | Status |
| :--- | :--- | :--- | :---: |
| 2.20.3 | DMA stream **locked** on USART transfers | UART+DMA hangs permanently | A |
| 2.21.1 | DMA stream **locked** on LPUART transfers | LPUART+DMA hangs permanently | A |
| 2.1.5 | Store after cache invalidate without barrier | Silent DMA data corruption on M7 | A |
| 2.3.1 | AXI domain locked on CPU-scoped watchdog reset | Dual-core deadlock on WWDG reset | A |
| 2.1.1 | Write-through D-Cache returns stale data | Silent data corruption on M7 | P |
| 2.3.5 | VOS0 / 480 MHz unavailable on rev Y, W | Clock tree config fails | P |

---

## 1. Cortex-M7 Core, Cache & MPU

### 2.1.5 - Store after cache invalidate without intervening barrier (`A`)
A store issued right after a cache maintenance operation, with no barrier in between,
can produce an inconsistent memory view.

**Workaround**: insert a `DMB` between the cache maintenance operation and the store.

```c
SCB_InvalidateDCache_by_Addr((uint32_t *)rx_buf, sizeof(rx_buf));
__DMB();                 /* MANDATORY - ES0445 2.1.5 */
rx_buf[0] = 0x00;
```

> **Rule for this project**: every `SCB_CleanDCache_by_Addr()` /
> `SCB_InvalidateDCache_by_Addr()` / `SCB_CleanInvalidateDCache_by_Addr()` call must be
> followed by `__DMB()` before the next store to that buffer.
> See `cache_mpu_rules.md`.

### 2.1.1 - Data corruption when D-Cache is configured write-through (`P`)
With write-through memory, a load may return stale data instead of the value written by
the most recent store. No direct workaround exists.

**Workaround**: configure MPU regions as **write-back** instead of write-through.
Do **not** set `FORCEWT` in `CM7_CACR`, and do not set `SIWT` for shared cacheable
regions. If write-through is unavoidable, disable the D-Cache for that code section.

> **Consequence for DMA buffers**: do not try to solve DMA coherency with a
> write-through MPU region. Use either a **non-cacheable** region (`TEX=001, C=0, B=0`)
> or explicit clean/invalidate + `__DMB()`.

### 2.1.4 - ECC error corrupts data when both DEBR registers are locked (`A`)
**Workaround**: never lock both `DEBR0` and `DEBR1` simultaneously.

### 2.1.2 - `PLD` may line-fill an address that would fault (`A`)
**Workaround**: use MPU region 0 to cover all otherwise-unmapped memory, marked
execute-never and inaccessible, so speculative `PLD` cannot touch device memory with
read side effects.

### 2.1.6 - Cortex-M7 FPU interrupt not present on NVIC line 81 (`N`)
Do not rely on an FPU exception IRQ on CPU1.

---

## 2. Cortex-M4 Core

### 2.2.2 / 2.2.5 - `VDIV` / `VSQRT` may not complete with very short ISRs (`A`)
Only applies when the FPU is enabled.

**Workaround** (pick one):
- Disable lazy FP context save: clear `LSPEN` (bit 30 of `FPCCR` at `0xE000EF34`).
- Guarantee every ISR contains more than 2 instructions besides the exception return.

```c
/* CM4 startup, after SystemInit() - ES0445 2.2.2 */
FPU->FPCCR &= ~FPU_FPCCR_LSPEN_Msk;
```

### 2.2.1 / 2.2.4 - Interrupted loads to SP can cause erroneous behavior (`A`)
Avoid `LDR SP, [Rn], #imm` / `LDR SP, [Rn, #imm]!` forms. Not emitted by GCC at
normal optimization levels; relevant only for hand-written assembly.

### 2.2.3 - Store immediate overlapping exception return may vector wrong IRQ (`A`)
Relevant for hand-written assembly only.

---

## 3. System, Power, Clocks & Dual-Core

### 2.3.1 - AXI domain locked when watchdog reset is scoped to CPU1 or CPU2 (`A`)
**Critical in dual-core designs.** If a window watchdog is configured to reset only one
CPU, the AXI domain can lock up.

**Workaround**: scope the watchdog reset to the **whole system** by setting `WW1RSC`
(CPU1) or `WW2RSC` (CPU2) in `RCC_GCR`.

```c
/* ES0445 2.3.1 - WWDG must reset the full system, not just one CPU */
RCC->GCR |= RCC_GCR_WW1RSC;   /* CPU1 / M7 */
RCC->GCR |= RCC_GCR_WW2RSC;   /* CPU2 / M4 */
```

### 2.3.5 - 480 MHz maximum CPU frequency not available (`P`)
`VOS0` voltage scaling is **not available on silicon revisions Y and W**, capping the
M7 at 400 MHz.

**Workaround**: use silicon revision **V** or **X** to reach 480 MHz.

> **Before generating a 480 MHz clock tree**, confirm the silicon revision via
> `REV_ID[15:0]` in `DBGMCU_IDC` (`0x2003` = rev U/V on this family). On any other
> revision, target 400 MHz with `VOS1`.

### 2.3.7 - WWDG not functional below VDD 2.7 V with VOS0/VOS1 (`N`)
The WWDG may fail to generate a reset and may not set its reset flag. No workaround.
Not an issue on Nucleo-H755ZI-Q at 3.3 V, but blocking for low-voltage designs.

### 2.3.4 - Unstable LSI when it clocks the RTC, or CSS on LSE (`P`)
**Workaround** (pick one):
- Clock the RTC from LSE or HSE/32 and do not enable the LSE CSS.
- If LSI clocks the RTC or `LSECSSON` is set, reset the backup domain on every VDD
  power-up (when `BORRSTF` is set) and restore RTC config, backup registers and
  anti-tamper configuration.

### 2.3.13 - LSE crystal oscillator may be disturbed by transitions on PC13 (`N`)
Do not toggle `PC13` at speed when the LSE is in use. Relevant for the Nucleo, where
`PC13` is the USER button.

### 2.3.11 - Ethernet MII mode unavailable on packages with PC2_C / PC3_C (`A`)
**Workaround**: use **RMII**. This is already the Nucleo-H755ZI-Q configuration
(LAN8742A in RMII). Never generate MII init code for this board.

### 2.3.12 - HASH input data may be corrupted when DMA is used (`A`)
**Workaround** (pick one):
- During a HASH transfer on DMA1, ensure no DMA2 channel is enabled (or vice versa),
  **and** set bit 20 of `DMA_SxCR` for the HASH stream.
- Use interrupt or polling mode for HASH instead of DMA.

### 2.3.2 - Device stalled on two consecutive RDP level regressions (`A`)
**Workaround**: perform a dummy access to backup SRAM before an RDP level 1 -> 0
regression.

### 2.3.3 - GPIO assigned to DAC unusable as output when DAC drives an on-chip peripheral (`N`)

---

## 4. DMA / BDMA / MDMA / DMAMUX

### 2.20.3 & 2.21.1 - DMA stream locked on USART / LPUART transfers (`A`)
**The single most common H7 DMA bug.** A DMA stream servicing a USART or LPUART can
lock permanently.

**Workaround**: enable the alternative peripheral DMA channel protocol by setting
**bit 20 of `DMA_SxCR`** on the stream used for that USART/LPUART. The bit is marked
reserved in RM0399 and must be set **only** on USART/LPUART streams.

```c
/* ES0445 2.20.3 / 2.21.1 - apply AFTER HAL_DMA_Init(), BEFORE enabling the stream */
#define DMA_SxCR_ALT_PROTOCOL   (1UL << 20)
hdma_usart3_rx.Instance->CR |= DMA_SxCR_ALT_PROTOCOL;
hdma_usart3_tx.Instance->CR |= DMA_SxCR_ALT_PROTOCOL;
```

> Applies to USART1/2/3/6, UART4/5/7/8 and LPUART1 whenever DMA is used.
> See `docs/peripherals/usart_uart/README.md` and `docs/peripherals/dma_mdma/README.md`.

### 2.5.1 - BDMA disable failure and error flag omission (`A`)
**Workaround**: never clear `GIFx` (global interrupt flag) while the channel is active.
Use the specific `HTIFx`, `TCIFx`, `TEIFx` flags and their dedicated clear bits.

### 2.6.4 - Wrong DMA request routed on `DMAMUX_CxCR` write coinciding with sync event (`A`)
**Workaround**: keep `SPOL[1:0]` at `00` whenever `SE` is `0`. When enabling
synchronization (`SE = 1`), set `SPOL[1:0]` to a non-zero value **in the same write**
to `DMAMUX_CxCR`.

### 2.6.1 / 2.6.2 / 2.6.3 - `SOFx` / `OFx` flags not asserted in several cases (`N`)
Do not build logic that depends on DMAMUX synchronization/request-generator overrun
flags being reliable.

### 2.4.1 - Non-flagged MDMA write attempts to reserved area (`A`)
**Workaround**: never let MDMA target reserved address ranges. Validate source and
destination against `STM32H755_Memory_Map.md`.

---

## 5. USART / LPUART / SPI / I2C

### 2.20.1 - Anticipated end-of-transmission signaling in USART slave mode (`A`)
### 2.20.2 - Data corruption due to a noisy receive line (`A`)
### 2.20.4 - Received data may be corrupted upon clearing `ABREN` (`A`)
### 2.20.5 - Noise error flag set while `ONEBIT` is set (`N`)
Do not treat `NE` as meaningful when `ONEBIT = 1`.

### 2.21.2 - LPUART transmitter issue at low `BRR[15:0]` values (`P`)
Keep `BRR` within the range recommended by RM0399; avoid the extreme low end.

### 2.22.1 - SPI master transfer stalls when system clock is much faster than SCK (`A`)
**Workaround** (pick one):
- Disable then re-enable the SPI after each `EOT` event.
- Wait at least one `SCK` cycle after `EOT` before setting `CSTART`.
- Prevent `EOT` entirely: set `TSIZE = 0` and drive transmission only via TXFIFO writes.

> Very likely on this device: M7 at 400-480 MHz driving SPI at a few MHz.

### 2.22.3 - `TXP` interrupt occurring while SPI is disabled (`A`)
**Workaround**: keep `TXP` (and `TXC` when `TSIZE = 0`) interrupts disabled whenever
the SPI peripheral is disabled.

### 2.22.2 / 2.22.4 / 2.22.5 - CRC and EOT-related corruption (`P`/`A`)
Review before enabling SPI hardware CRC or relying on signal state after `EOT`.

### 2.19.2 - Spurious I2C bus error detection in controller mode (`A`)
**Workaround**: on a `BERR` interrupt in controller mode, just clear the flag. No other
action is needed and the ongoing transfer proceeds normally.

```c
/* ES0445 2.19.2 - do NOT abort the transfer on BERR in master mode */
if (hi2c->ErrorCode & HAL_I2C_ERROR_BERR) {
    __HAL_I2C_CLEAR_FLAG(hi2c, I2C_FLAG_BERR);
}
```

### 2.19.6 - I2C transmission stalled after the first byte (`A`)
**Workaround** (pick one):
- Write the first byte into `I2C_TXDR` **before** starting the transfer.
- Choose clocks so that `f(APB) / f(I2C kernel)` is below 1.5 or above 3.

### 2.19.1 - Wrong data sampling when `tSU;DAT` is shorter than one kernel clock (`P`)
### 2.19.3 - Spurious controller transfer on own target address match (`P`)
### 2.19.5 - `OVR` flag not set in underrun condition (`N`)
### 2.19.7 - SDA held low after SMBus timeout in target mode (`A`)

---

## 6. Timers, LPTIM, RTC & Watchdogs

### 2.17.1 - Device may remain stuck in an LPTIM interrupt when entering Stop mode (`A`)
**Workaround**: to disable an LPTIM, do **not** clear its `ENABLE` bit. Reset the whole
peripheral through RCC by setting and clearing the matching `LPTIMxRST` bit.

### 2.17.2 - Device may remain stuck in an LPTIM interrupt when clearing an event flag (`A`)
**Workaround**:
- Clear a flag only when its interrupt is enabled in the interrupt enable register.
- If a flag with a disabled interrupt line must be cleared, clear it **before** the ones
  whose interrupt line is enabled.
- Never clear LPTIM flags outside the interrupt subroutine.

> The standard sequence in `HAL_LPTIM_IRQHandler()` already complies.

### 2.17.3 - LPTIM events and PWM output delayed by one kernel clock cycle (`A`)
Account for the extra cycle in timing-critical LPTIM PWM.

### 2.18.2 - RTC calendar initialization may fail on consecutive INIT entries (`A`)
**Workaround**: after exiting initialization mode, clear `BYPSHAD` (if set), then wait
for `RSF` to rise before re-entering initialization mode. Prefer writing all RTC
registers in a **single** initialization session.

### 2.18.1 - RTC interrupt can be masked by another RTC interrupt (`A`)
### 2.18.3 - Alarm flag repeatedly set when the core is halted in debug (`N`)
Expect spurious alarm behavior while breakpointed; do not chase it as a code bug.

### 2.16.1 - One-pulse mode trigger not detected in master-slave reset+trigger config (`P`)
### 2.16.2 - Consecutive compare event missed in specific conditions (`N`)
### 2.16.3 - Output compare clear not working with external counter reset (`P`)
### 2.16.4 - Unexpected PWM output when using `ocref_clr` (`N`)
### 2.15.1 / 2.15.2 - HRTIM burst idle entry/exit and delayed idle protection (`N`)

---

## 7. Memory Interfaces: QUADSPI, FMC, SDMMC

### 2.8.3 - QUADSPI internal timing criticality (`A`)
**Workaround**: run the following sequence after reset and on every switch out of
memory-mapped mode. The final command must complete within 127 kernel clocks of the
first `QSPI_CCR` write.

```c
/* ES0445 2.8.3 - save QSPI_CR / QSPI_CCR first if they hold live config */
QUADSPI->CR  = 0;                              /* disable, drop max prescaler      */
while (QUADSPI->SR & 0x20) { }                 /* wait for BUSY to fall            */
QUADSPI->CR  = 0xFF000001;                     /* max prescaler + enable           */
QUADSPI->CCR = 0x20000000;                     /* free-running clock               */
QUADSPI->CCR = 0x20000000;                     /* repeat: avoid back-to-back disable */
QUADSPI->CR  = 0;                              /* disable                          */
while (QUADSPI->SR & 0x20) { }
/* restore CR / CCR */
```

### 2.8.5 - QUADSPI memory failure when using HCLK as kernel clock (`A`)
**Workaround** (pick one):
- Use a prescaler that preserves a 50% duty cycle on the QUADSPI clock.
- Select an alternative kernel clock (`pll1_q_ck` or `pll2_r_ck`) via `QSPISEL[1:0]`
  in `RCC_D1CCIPR`. **Preferred.**

### 2.8.1 / 2.8.2 / 2.8.4 - Indirect read with data phase only, `QUADSPI_CCR` clear hang, last-byte memory-mapped read (`P`)
Avoid clearing `QUADSPI_CCR` outright, and never map the very last byte of the memory
window as live data.

### 2.7.1 - Dummy read cycles inserted when reading synchronous memories (`N`)
### 2.7.2 - Wrong data read from a busy NAND memory (`A`)
### 2.7.3 - Unsupported read access with unaligned address (`P`)
Keep FMC accesses naturally aligned.
### 2.7.4 - Duty cycle variation on memory clock with PSRAM continuous clock mode (`A`)

### 2.9.1 - SDMMC command response and receive data end bits not checked (`N`)
**No workaround.** The SDMMC does not validate end bits, so a corrupted response can be
accepted as valid. Add application-level integrity checks (CRC or checksum over stored
payloads) when data integrity matters.

---

## 8. Ethernet (ETH)

The Nucleo-H755ZI-Q uses a LAN8742A PHY in **RMII** mode.

### 2.25.10 - Packets dropped in RMII 10 Mbps mode (fake dribble + CRC error) (`A`)
**Workaround**: set the `FEP` bit of `ETH_MTLRXQOMR` to disable the error-packet drop
mechanism, and accept packets flagged with both dribble and CRC errors.

> Only needed when the link negotiates to 10 Mbps. Safe to apply unconditionally when
> 10 Mbps operation is possible.

### 2.25.8 - Spurious receive watchdog timeout interrupt (`A`)
The register is **`ETH_DMACRXIWTR`** (Channel Rx interrupt watchdog timer, offset
`0x1138`): `RWT[7:0]` in bits 7:0, `RWTU[1:0]` in bits 17:16.

**Workaround**: never leave `RWTU[1:0]` non-zero while `RWT[7:0]` is zero. Program them
in **two successive writes**: first `RWT[7:0]` (keeping `RWTU` at zero), then `RWTU[1:0]`
(keeping `RWT` unchanged).

> Do not confuse it with `ETH_MACWTR` (Watchdog timeout register, offset `0x000C`), which
> holds `PWE` and `WTO[3:0]` and bounds the accepted **length** of a received packet.
> `RWT` / `RWTU` exist only in `ETH_DMACRXIWTR`. Both ES0445 rev 6 section 2.25.8 and
> RM0399 name that register.

### 2.25.2 - Rx DMA may fail to recover after a bus error with Rx timestamping (`A`)
**Workaround**: on a bus error, issue a soft reset, then reconfigure the controller and
rebuild the descriptors. Adds latency.

### 2.25.4 - Incorrectly weighted round robin between Tx and Rx DMA channels (`A`)
### 2.25.6 - IEEE 1588 timestamp interrupt status bits incorrectly cleared (`A`)
Be careful with read-modify-write on CSRs at similar offsets.
### 2.25.9 - Incorrect flexible PPS output interval (`A`)
### 2.25.11 - ARP offload not effective (`A`) - handle ARP in software (LwIP default).
### 2.25.12 - Tx DMA may halt while fetching a TSO header (`A`)
### 2.25.1 / 2.25.3 / 2.25.5 / 2.25.7 - Arbitration, TSO/USO corruption, L4 filtering, SOP bus error (`N`)
Disable TSO/USO if bus errors are plausible in the design.

> Reminder: ETH DMA descriptors **must** live in D2 SRAM (`0x30000000` /
> `0x30040000`) and in a non-cacheable MPU region. See `memory_and_domains.md` and
> `docs/peripherals/ethernet/README.md`.

---

## 9. USB OTG_HS

### 2.24.1 - Host transmission may hang: FS interface -> hub -> low-speed device (`N`)
### 2.24.2 - Isochronous IN endpoint disabled while a packet fetch is in progress (`A`)
### 2.24.5 - Unexpected transfer instead of a zero-length packet (`A`)
Relevant for CDC and control transfers that rely on ZLPs.
### 2.24.3 / 2.24.4 / 2.24.6 / 2.24.7 - Battery charger interrupt mask, false `CIDSCHG`, false chirp-K, ULPI/SOF timing (`A`)

---

## 10. Analog: ADC, DAC, VREFBUF

### 2.10.1 / 2.10.2 / 2.10.7 / 2.10.8 - Injected context queue anomalies (`A`)
When writing a new context into `ADC_JSQR` with `JQDIS = 0` and `JQM = 0`, conversions
can start without a trigger or fail outright.
**Practical rule**: set `JQDIS = 1` (disable the context queue) unless the queue is
genuinely required.

### 2.10.3 - Unexpected regular conversion with consecutive injected conversions in dual interleaved mode (`A`)
### 2.10.5 - Injected data stored in the wrong `ADC_JDRx` registers (`A`)
### 2.10.6 - ADC slave data may be shifted in dual regular simultaneous mode (`A`)
### 2.10.4 - `ADC_AWDy_OUT` reset by non-guarded channels (`A`)

### 2.11.1 - Invalid DAC analog output when `MODE` is programmed before initialization (`A`)
**Workaround**: always configure the DAC `MODE` bitfield **after** DAC initialization.

### 2.11.2 - DMA underrun flag not set on a specific clock cycle (`N`)
### 2.12.1 - Overshoot on VREFBUF output (`A`)
### 2.12.2 - VREFBUF Hold mode cannot be used (`N`)

---

## 11. Graphics & Miscellaneous

### 2.13.1 - Device stalled when accessing LTDC registers while the pixel clock is disabled (`A`)
**Workaround**: never touch LTDC registers unless the pixel clock is running.
### 2.14.1 - DSI tearing effect parasitic detection (`P`)
### 2.14.2 / 2.14.3 / 2.14.4 / 2.14.5 - DSI HS clock timing, immediate update, DPI timing, generic packet (`P`/`A`)
### 2.26.1 - Missed CEC messages in normal receiving mode (`A`)
### 2.26.2 - Unexpected `TXERR` flag during CEC transmission (`A`)

---

## 12. Documentation Errata (RM0399 is wrong, silicon is right)

| Section | Erratum |
| :--- | :--- |
| 2.6.5 | `DMAMUX_RGCFR` is **write-only**, not read-write |
| 2.7.5 | Incorrect SDRAM `TRCD` and `TXSR` delays documented |
| 2.7.6 | SDRAM recovery delay inconsistent when `TWR[3:0] = 0000` |
| 2.19.4 | I2C `START` bit is cleared on setting `ADDRCF`, **not** on address match |

---

## 13. Checklist for Generated Code

- [ ] `__DMB()` after every D-Cache maintenance call (2.1.5).
- [ ] No write-through MPU regions used for coherency (2.1.1).
- [ ] `DMA_SxCR` bit 20 set on every USART/LPUART DMA stream (2.20.3, 2.21.1).
- [ ] `RCC_GCR.WW1RSC` / `WW2RSC` set when WWDG is enabled (2.3.1).
- [ ] Silicon revision checked before targeting 480 MHz / `VOS0` (2.3.5).
- [ ] Ethernet configured as **RMII**, never MII (2.3.11).
- [ ] `ETH_MTLRXQOMR.FEP` set if 10 Mbps link is possible (2.25.10).
- [ ] `ETH_DMACRXIWTR`: `RWT` then `RWTU`, in two separate writes (2.25.8).
- [ ] LPTIM disabled via `RCC` reset, never via the `ENABLE` bit (2.17.1).
- [ ] I2C `BERR` in master mode: clear the flag and continue (2.19.2).
- [ ] SPI master: `EOT` handling per 2.22.1 when `f(SYS) >> f(SCK)`.
- [ ] CM4 FPU: `LSPEN` cleared, or all ISRs longer than 2 instructions (2.2.2).
- [ ] ADC: `JQDIS = 1` unless the injected context queue is required (2.10.x).
- [ ] DAC: `MODE` programmed **after** initialization (2.11.1).
- [ ] QUADSPI: kernel clock from `pll1_q_ck` / `pll2_r_ck`, plus the 2.8.3 unlock sequence.

---

## 14. References

- Full errata: [`ES0445_stm32h755_errata_sheet.pdf`](ES0445_stm32h755_errata_sheet.pdf)
- Cache & MPU rules: [`cache_mpu_rules.md`](cache_mpu_rules.md)
- Memory domains: [`memory_and_domains.md`](memory_and_domains.md)
- Dual-core boot & IPC: [`dual_core_boot_ipc.md`](dual_core_boot_ipc.md)
- Memory map: [`../../STM32H755_Memory_Map.md`](../../STM32H755_Memory_Map.md)
