# STM32H755 Shared DMA Driver (`01_shared_drivers/02_dma`)

Peripheral-First (Plan A), 3-tier shared DMA driver for the STM32H755ZI dual-core
microcontroller: DMA1 and DMA2 (16 streams) with DMAMUX1 in all three tiers, BDMA
(8 channels, D3 domain) in the HAL tier, and a header of alignment, cache and
reachability helpers shared by every tier.

Status: every tier was validated on a NUCLEO-H755ZI-Q (silicon Rev V,
`REV_ID = 0x2003`) on 2026-09-25, with a PD5-PD6 loopback fitted. The run is
self-identifying (`rev=df46b94`) and each hardware test has a recorded negative
control. On 2026-10-04 the same board ran the fix to the direct-mode size
check in `DMA_HAL_Init` and its test, T13 (`rev=dd65b06`, with two negative
controls). That change has not had an independent run yet. What all this
covers, and what it does not, is in [section 9](#9-validation).

---

## 1. Tiers

| Tier | Files | CMake target | Links | ST dependency | Typical use |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Memory helpers** | `dma_mem_bm.h` | header-only, comes with every target below | - | CMSIS only | Buffer alignment and size, D-Cache maintenance, "can this master reach that address" |
| **Bare-Metal** | `dma_bm.[c\|h]` | `stm32h7_driver_dma_bm` (alias `stm32h7_driver_dma`) | - | CMSIS only (`stm32h7xx.h`) | Cortex-M4 deterministic loops, pre-RTOS bring-up, fault paths |
| **Low-Level** | `dma_ll.[c\|h]` | `stm32h7_driver_dma_ll` | `stm32h7_driver_dma_bm` | `stm32h7xx_ll_dma.h`, `stm32h7xx_ll_dmamux.h` inlines | Handle-based transfers with LL constants, IRQ service with callback |
| **HAL** | `dma_hal.[c\|h]` | `stm32h7_driver_dma_hal` | `stm32h7_driver_dma_bm` | ST HAL (`HAL_DMA_*`) | Cortex-M7 / FreeRTOS, BDMA, cache-coherent memory copy |

```cmake
target_link_libraries(cm4_firmware PRIVATE stm32h7_driver_dma_bm)   # or _ll, or _hal
```

```c
#include "dma_bm.h"     /* never "02_dma/dma_bm.h": the directory is on the include path */
```

- **`stm32h7_driver_dma` is the BM tier, not HAL.** Unlike `uart` and `adc`,
  where the unsuffixed target is HAL, this name predates the tiers and always
  meant `dma_bm`. It is kept as an alias with no sources of its own, because
  `prj_04` CM4 and `stm32h7_driver_adc_bm` link it. Reaching `dma_bm.c` through
  both names compiles it once.
- **The umbrella `stm32h7_shared_drivers` links only the BM tier.** `_ll` and
  `_hal` have to be linked by name.
- **`dma_bm` is the single copy of the stream geometry**: controller, stream
  index, DMAMUX1 channel, IRQn, and which request IDs need TRBUFF. LL and HAL
  call into it instead of carrying their own tables.
- **LL needs no LL `.c` file** and no `USE_FULL_LL_DRIVER`, and it never
  includes `stm32h7xx_hal.h`.
- **HAL compiles to nothing** without `HAL_DMA_MODULE_ENABLED`. It also needs a
  HAL timebase (`HAL_GetTick`) for `HAL_DMA_Abort` and `HAL_DMA_PollForTransfer`.
  On the CM7 with FreeRTOS that is TIM6 or TIM7, never SysTick.

### 1.1 Feature matrix

| Feature | BM | LL | HAL |
| :--- | :---: | :---: | :---: |
| DMA1 / DMA2 streams + DMAMUX1 | yes | yes | yes |
| BDMA channels + DMAMUX2 (D3) | no | no | yes |
| P2M, M2P, M2M | yes | yes | yes |
| Circular mode | yes | yes | yes |
| Double-buffer mode (DBM) | yes | yes | via `HAL_DMAEx_MultiBufferStart` |
| Peripheral flow control (`PFCTRL`) | yes | yes | yes (`DMA_PFCTRL`) |
| FIFO with threshold, memory and peripheral bursts | yes | yes | yes |
| All configuration validated before the first register write | yes | yes | partly (see §4) |
| TRBUFF (ES0445) applied without the caller asking | yes | yes | UART: `HAL_DMA_Init`; HASH: `DMA_HAL_CheckErrata` |
| Refuses to steal a running stream (`fail_if_busy`) | yes (`…Ex`) | yes | no (HAL stops it) |
| Bounded spin timeouts | yes | yes | no (HAL tick) |
| NVIC helper that resolves the IRQn itself | `DMA_BM_GetIRQnEx` | `DMA_LL_EnableIrq` | `irq_priority` in the config |
| Interrupt service with a callback | no | `DMA_LL_IrqService` | HAL callbacks + `DMA_HAL_IRQDispatch` |
| Cache-coherent memory copy | via `dma_mem_bm.h` | `DMA_LL_MemCopy` | `DMA_HAL_MemCopy` |

All three tiers take the same injection: a stream (or BDMA channel) pointer plus a
numeric DMAMUX request ID. The HAL `DMA_REQUEST_*` and LL `LL_DMAMUX1_REQ_*`
constants have the same numeric values as the raw DMAMUX1 IDs, so one
`board_def.h` works for any tier.

---

## 2. API summary

### 2.1 `dma_bm.h`

| Function | Purpose |
| :--- | :--- |
| `DMA_BM_InitStream(cfg)` | Original API. Wraps `InitStreamEx` with defaults: no DBM, no burst, `fail_if_busy = false` (an active stream **is stopped**). |
| `DMA_BM_InitStreamEx(cfg_ex)` | Full configuration: DBM (`mem1_addr != 0`), memory and peripheral bursts, `PFCTRL`, `fail_if_busy`, `stop_spin_limit`. `DmaBm_ConfigEx_t` embeds `DmaBm_Config_t` as its first member. |
| `DMA_BM_SetTransfer(s, par, m0ar, ndt)` | Re-arms a disabled stream with new addresses and length, using the same validation as Init. |
| `DMA_BM_Start(s)` / `DMA_BM_StartEx(s)` | Both clear the stream flags before setting `EN` (RM0399 requires it). `StartEx` returns `true` once `EN`, TC or TE reads back. |
| `DMA_BM_Stop(s)` / `DMA_BM_StopEx(s, spin)` | Clear `EN` and wait until the stream is really idle. Return `false` on timeout. |
| `DMA_BM_DeinitStream(s)` | Stop, reset the stream registers, clear its flags, reset its DMAMUX1 channel. |
| `DMA_BM_GetFlags` / `DMA_BM_ClearFlags` / `DMA_BM_TakeFlags` | `DMA_BM_FLAG_TC/HT/TE/DME/FE`, hiding the `LISR`/`HISR` bit layout. `TakeFlags` clears exactly the flags it read, so no event is lost. |
| `DMA_BM_SetIdleMemoryEx(s, addr, len)` | DBM: writes the buffer the DMA is **not** using (`M1AR` if `CT=0`, `M0AR` if `CT=1`). Returns `false` if TE is set afterwards (the write lost the race with the switch). |
| `DMA_BM_SetIdleMemory(s, addr)` | Original DBM API. Checks reachability with the *remaining* NDTR; prefer `…Ex`. |
| `DMA_BM_GetCurrentTarget(s)` | DBM: 0 = M0AR, 1 = M1AR. |
| `DMA_BM_GetIRQnEx(s, &irqn)` | Stream → IRQn. `false` for an invalid stream. |
| `DMA_BM_GetIRQn(s)` | Original API. Returns `DMA_BM_IRQN_INVALID` on error, which equals `SysTick_IRQn` (see §10). |
| `DMA_BM_GetController` / `DMA_BM_GetStreamIndex` | Stream pointer → DMA1/DMA2 and 0..7. They reject pointers that are not on the 0x18 stride. |
| `DMA_BM_IsUartRequest(id)` | `true` for the request IDs that need TRBUFF: USART/UART {41-46, 63-66, 71-72, 79-82} **and HASH_IN (78)**. The name is historical. |
| `DMA_BM_CheckFifoBurst` / `DMA_BM_CheckPeriphBurst` | Memory-side (RM0399 Table 116) and peripheral-side (§16.3.14) burst-vs-FIFO rules, exposed so LL shares them. |

### 2.2 `dma_ll.h`

`DMA_LL_Init`, `DMA_LL_Deinit`, `DMA_LL_Start(h, par, mar, ndt)`,
`DMA_LL_StartDoubleBuffer`, `DMA_LL_SetIdleMemory(h, addr)` (uses the length
recorded by `StartDoubleBuffer`), `DMA_LL_Stop`, `DMA_LL_GetRemaining`,
`DMA_LL_TakeFlags`, `DMA_LL_TakeErrors` (PRIMASK-guarded), `DMA_LL_EnableIrq` /
`DMA_LL_DisableIrq` (IRQn resolved from the stream), `DMA_LL_IrqService(h, cb, ctx)`
and `DMA_LL_MemCopy`. The flags are the same bits as BM (`DMA_LL_FLAG_* ==
DMA_BM_FLAG_*`).

### 2.3 `dma_hal.h`

| Function | Purpose |
| :--- | :--- |
| `DMA_HAL_Init(hdma, cfg)` | Enables the clock (optional), validates, calls `HAL_DMA_Init`, then `DMA_HAL_CheckErrata`, configures the NVIC if `irq_priority >= 0`, and registers the handle for dispatch. Forces FIFO mode for M2M. |
| `DMA_HAL_DeInit(hdma)` | Disables the IRQ, unregisters, `HAL_DMA_DeInit`. |
| `DMA_HAL_CheckErrata(hdma)` | On a DMA1/DMA2 stream with a TRBUFF request, sets bit 20 if missing and reads it back. `HAL_BUSY` if the stream is enabled. |
| `DMA_HAL_IRQDispatch(instance)` | Calls `HAL_DMA_IRQHandler` for the registered handle. Per-core table of 24 entries (16 streams + 8 BDMA channels). The driver defines no vector symbols. |
| `DMA_HAL_GetIRQnEx` / `DMA_HAL_GetIRQn` | As in BM, and also for BDMA channels. |
| `DMA_HAL_MemCopy(hdma, dst, src, bytes, ms)` | Cache-coherent blocking M2M on DMA1/DMA2 or BDMA. It validates reachability for the handle's controller, requires `bytes` to be a multiple of both data sizes, and limits items (NDT in PSIZE units) to 1..65535. |

### 2.4 `dma_mem_bm.h`

`DMA_MEM_BM_ALIGN`, `DMA_MEM_BM_SIZE(bytes)` (round up to 32),
`DMA_MEM_BM_IsLineAligned`, `DMA_MEM_BM_CleanForTx`, `DMA_MEM_BM_PrepareRx`,
`DMA_MEM_BM_CompleteRx`, `DMA_MEM_BM_IsReachable(master, addr, bytes)`. See §5.

---

## 3. Board definition contract

The driver never names a stream, a channel or a request. The project allocates
them in `board_def.h`, **one line per stream in use, per core**. A stream that
appears twice is a dual-core collision. With `fail_if_busy = true`, the second
`InitStreamEx` refuses instead of silently stopping the other core's transfer.

```c
/* prj_04_adc_dma_bm_eth/Common/Inc/board_def.h */
#define BOARD_ADC_DMA_STREAM        DMA1_Stream0   /* CPU2 */
#define BOARD_ADC_DMA_REQUEST       9U             /* DMAMUX1: adc1_dma */
```

- **Clocks.** BM always enables the controller clock in the calling core's RCC
  bank. That is not a race: `AHB1ENR` is banked per core (RM0399 §9.7.1). LL and
  HAL do it only when `enable_clock = true`. `adc_hal` leaves it to the project.
- **NVIC.** BM does not touch the NVIC. LL does it through `DMA_LL_EnableIrq`,
  HAL through `irq_priority`. In every tier the project writes the
  `DMAx_Streamy_IRQHandler` vector and calls the tier's service function from it.

---

## 4. Rules the driver enforces

These come from RM0399 chapter 16. `InitStreamEx`, `SetTransfer`, `DMA_LL_Init`,
`DMA_LL_Start` and `DMA_LL_StartDoubleBuffer` **validate everything before the
first register write**, so a rejected call leaves the stream untouched (`CR == 0`
on a clean stream).

| Rule (RM0399) | Consequence in the driver |
| :--- | :--- |
| Event flags must be cleared before setting `EN` (§16.5.5) | `Start`, `StartEx` and `DMA_LL_Start` clear them first. |
| Configuration registers are writable only with `EN = 0` | Init stops the stream first, or refuses if `fail_if_busy`. `SetTransfer` refuses on `EN = 1`. |
| M2M: no circular, no direct mode, no DBM, no `PFCTRL` (§16.3.8, §16.3.11, §16.5.5) | Circular, DBM and `PFCTRL` are rejected. FIFO mode is forced (`DMDIS = 1`) in all three tiers. |
| NDT is 16 bits (`NDT[15:0]`) | 1..65535, otherwise rejected. Previously a larger value was silently truncated. |
| NDT counts **PSIZE** units (§16.3.12) | Every byte span (reachability, cache maintenance, `MemCopy` item count) is `NDT × PSIZE`. |
| If PSIZE < MSIZE, NDT must be a multiple of MSIZE/PSIZE (Table 115) | Rejected otherwise, in Init, `SetTransfer` and both LL starts. HAL `MemCopy` requires `bytes % msize == 0`. |
| Direct mode forces MSIZE = PSIZE when `EN = 1` | PSIZE ≠ MSIZE without FIFO is rejected, because the requested packing would not happen. |
| Addresses aligned to the data size; bursts must not cross 1 KB (§16.3.13) | An incrementing address with a burst must be aligned to beats × size, which also guarantees no 1 KB crossing (`_Static_assert` in `dma_bm.c`). With `PINC = 0`, the peripheral address only needs PSIZE alignment. |
| Memory burst vs FIFO threshold (Table 116) | `DMA_BM_CheckFifoBurst`, the same table as the HAL's `DMA_CheckFifoParam`. |
| Peripheral burst vs FIFO (§16.3.14) | `DMA_BM_CheckPeriphBurst`: PBURST × PSIZE ≤ 16 B, and 3/4 threshold forbidden when it equals 16 B. |
| Reserved values | `DIR = 11`, `PSIZE/MSIZE = 11`, request ID > 127 (`DMAREQ_ID[6:0]`) and request 0 outside M2M are rejected. |
| DBM on-the-fly update (§16.3.11) | `SetIdleMemory*` writes only the register the DMA is not using. |

**The HAL tier validates less.** `DMA_HAL_Init` checks the instance, forces FIFO
for M2M, rejects circular or `PFCTRL` with M2M, and rejects direct mode with
different data sizes. Addresses and lengths are validated only in
`DMA_HAL_MemCopy`. A plain `HAL_DMA_Start*` is ST's code, and it only checks what
the HAL checks.

---

## 5. Memory, cache and reachability (`dma_mem_bm.h`)

| Master | Rule | Ranges |
| :--- | :--- | :--- |
| DMA1 / DMA2 | **Deny list** (anything else passes, so FMC/QSPI external memory stays usable) | ITCM `[0x0000_0000, 0x0001_0000)`, DTCM `[0x2000_0000, 0x2002_0000)`. This is where the default `.bss` and stack of CPU1 live, and the most common H7 DMA bug. |
| BDMA | **Allow list** (RM0399 §2.1.7: *"It can access the internal SRAM4, backup RAM, and AHB4 and APB4 peripherals"*) | SRAM4 `[0x3800_0000, 0x3801_0000)`, backup SRAM `[0x3880_0000, 0x3880_1000)`, D3 peripherals `[0x5800_0000, 0x5803_0000)` |

Both lists reject `addr + bytes` overflow and zero length.

- **Place DMA buffers in a DMA-reachable, preferably non-cacheable section**
  (`.dma_buf` in SRAM1 for DMA1/DMA2, `.d3_ram` in SRAM4 for BDMA). A buffer in
  cacheable `.axi_ram` needs the maintenance below.
- **Alignment and size.** `DMA_MEM_BM_ALIGN` gives 32-byte alignment. The size
  must also be a multiple of 32 (`DMA_MEM_BM_SIZE()`), otherwise invalidating the
  last line destroys whatever variable shares it.
- **`CleanForTx(p, n)`**: before the DMA reads a buffer the CPU wrote. It may
  round out to whole lines, because clean never loses data.
- **`PrepareRx(p, n)`**: before the DMA writes a buffer. It does clean +
  invalidate, so no dirty line can be evicted on top of DMA data mid-transfer.
  **`CompleteRx(p, n)`**: after HT/TC, before the CPU reads the buffer. Both
  return `false`, and do nothing, unless `p` and `n` are 32-byte aligned. They
  enforce that on the CM4 too, so code stays portable between cores.
- **Every maintenance operation is followed by `__DMB()`** (ES0445 2.1.5).
- **Behaviour per core.** On the CM4 (`__DCACHE_PRESENT != 1`), and on a CM7 with
  the D-Cache off, the maintenance functions compile but do nothing.
- **Never solve coherency with a write-through MPU region** (ES0445 2.1.1). Use
  a non-cacheable region, or write-back plus this maintenance.

---

## 6. Silicon errata (ES0445 rev 6)

| Erratum | What the driver does |
| :--- | :--- |
| **2.20.3 / 2.21.1**: a USART/LPUART DMA stream can lock permanently under concurrent transfers | Bit 20 of `DMA_SxCR` (`TRBUFF`) is set for USART/UART requests when `REV_ID >= 0x2000` (Rev V is `0x2003`; Rev Y is `0x1003` and stays below), which is the same condition ST's `HAL_DMA_Init` uses. BM and LL do it by request ID; `enable_trbuff = true` forces it. In HAL, `HAL_DMA_Init` sets it itself and `DMA_HAL_CheckErrata` reads it back. LPUART1 is served only by DMAMUX2/BDMA, which has no bit 20. |
| **2.3.12**: HASH input data may be corrupted when DMA is used | TRBUFF is set for HASH_IN (request 78) in BM and LL. In HAL it is set by `DMA_HAL_CheckErrata`, because ST's HAL applies it to UART requests only. The other half of the workaround **cannot be enforced by a driver**: while HASH transfers through DMA1, no stream may be enabled on DMA2, or the other way round. Otherwise, feed HASH by interrupt or polling. |
| **2.1.5**: store after cache maintenance without a barrier | `__DMB()` after every maintenance operation (§5). |
| **2.1.1**: write-through D-Cache returns stale data | Documentation only: do not use write-through for DMA buffers. |
| **2.6.4**: wrong DMAMUX request routed on a `CxCR` write coinciding with a sync event | The driver writes `DMAMUX_CxCR` whole, with `SE = 0` and `SPOL = 00`. Synchronization is not implemented. If it is ever added, `SE` and a non-zero `SPOL` must go in the same write. |
| **2.6.1-2.6.3**: DMAMUX `SOFx`/`OFx` not asserted in several cases | The driver does not expose them; do not build logic on them. |
| **2.5.1**: BDMA disable fails if `CGIFx` is cleared while the channel is active | BDMA exists only in the HAL tier, and ST's HAL never uses `CGIFx` on an active channel. A future `bdma_bm` must clear only `HTIF`/`TCIF`/`TEIF`. |

---

## 7. Error handling (RM0399 §16.3.20)

| Flag | Raised when | Stream disabled by hardware? |
| :--- | :--- | :--- |
| **TE** | Bus error on a DMA read or write (for example ITCM from DMA1), or a write to the active memory register in DBM | **Yes**, `EN` is cleared. |
| **FE** | FIFO overrun/underrun, or the stream enabled with a threshold incompatible with the memory burst | Only in the burst/threshold case. On overrun/underrun **no**: the stream keeps running. |
| **DME** | Direct mode, P2M with `MINC = 0`, a request arriving before the previous data reached memory | **No.** Software decides whether to stop the stream. |

- **BM** returns the raw flags (`GetFlags`/`TakeFlags`); the caller decides.
- **LL**: `DMA_LL_IrqService` counts TC and HT, accumulates TE/DME/FE in
  `error_flags`, and passes every flag to the callback. It does not stop the
  stream on its own. `DMA_LL_TakeErrors()` reads and clears the accumulated
  errors under PRIMASK.
- **HAL** follows ST's policy. `HAL_DMA_Start_IT` does not enable `FEIE`, and FE
  is reported but not treated as fatal.

---

## 8. Usage examples

### A. M2M with FIFO and bursts (`dma_bm`, either core)

```c
#include "dma_bm.h"
#include "dma_mem_bm.h"

static uint32_t s_src[64] DMA_MEM_BM_ALIGN;
static uint32_t s_dst[64] DMA_MEM_BM_ALIGN;

bool CopyBlock(void)
{
    DmaBm_ConfigEx_t cfg = {
        .base = {
            .stream           = DMA1_Stream1,
            .request_id       = 0U,                     /* M2M: no request */
            .direction        = DMA_BM_DIR_MEM_TO_MEM,  /* PAR = source, M0AR = destination */
            .periph_data_size = DMA_BM_DATA_SIZE_WORD,
            .mem_data_size    = DMA_BM_DATA_SIZE_WORD,
            .periph_inc       = true,
            .mem_inc          = true,
            .priority         = DMA_BM_PRIORITY_HIGH,
            .enable_fifo      = true,                   /* forced for M2M anyway */
            .fifo_threshold   = DMA_BM_FIFO_TH_FULL,
            .periph_addr      = (uint32_t)s_src,
            .mem_addr         = (uint32_t)s_dst,
            .buffer_length    = 64U                     /* in PSIZE units */
        },
        .mem_burst    = DMA_BM_BURST_INCR4,
        .periph_burst = DMA_BM_BURST_INCR4,
        .fail_if_busy = true
    };

    DMA_MEM_BM_CleanForTx(s_src, sizeof(s_src));        /* no-op on CM4 */
    if (!DMA_MEM_BM_PrepareRx(s_dst, sizeof(s_dst)) ||
        !DMA_BM_InitStreamEx(&cfg) || !DMA_BM_StartEx(DMA1_Stream1))
    {
        return false;
    }

    uint32_t spin = 100000U;
    while (((DMA_BM_GetFlags(DMA1_Stream1) & (DMA_BM_FLAG_TC | DMA_BM_FLAG_TE)) == 0U) && (spin-- > 0U)) { }

    uint32_t flags = DMA_BM_TakeFlags(DMA1_Stream1);
    (void)DMA_MEM_BM_CompleteRx(s_dst, sizeof(s_dst));
    return (flags & DMA_BM_FLAG_TC) != 0U;
}
```

### B. Re-arming a normal-mode transfer (`dma_bm`)

```c
/* Stream configured once with InitStreamEx; each packet re-arms it. */
bool SendPacket(const uint8_t *buf, uint32_t len)
{
    if (!DMA_BM_StopEx(BOARD_TX_DMA_STREAM, 0U))                     /* EN must be 0 */
        return false;
    if (!DMA_BM_SetTransfer(BOARD_TX_DMA_STREAM, (uint32_t)&USART2->TDR, (uint32_t)buf, len))
        return false;
    return DMA_BM_StartEx(BOARD_TX_DMA_STREAM);                      /* clears flags, sets EN */
}
```

### C. P2M with interrupt and callback (`dma_ll`)

```c
#include "dma_ll.h"

static DmaLL_Handle_t s_adc_dma;
static uint16_t s_samples[256] DMA_MEM_BM_ALIGN;   /* in .dma_buf when used on the CM7 */

static void OnDma(DmaLL_Handle_t *h, uint32_t flags, void *ctx)
{
    (void)h; (void)ctx;
    if ((flags & DMA_LL_FLAG_TC) != 0U) { /* buffer full */ }
}

bool StartAdcDma(void)
{
    DmaLL_Config_t cfg = {
        .stream         = BOARD_ADC_DMA_STREAM,
        .request        = BOARD_ADC_DMA_REQUEST,
        .direction      = LL_DMA_DIRECTION_PERIPH_TO_MEMORY,
        .mode           = LL_DMA_MODE_CIRCULAR,
        .periph_inc     = LL_DMA_PERIPH_NOINCREMENT,
        .mem_inc        = LL_DMA_MEMORY_INCREMENT,
        .periph_size    = LL_DMA_PDATAALIGN_HALFWORD,
        .mem_size       = LL_DMA_MDATAALIGN_HALFWORD,
        .priority       = LL_DMA_PRIORITY_HIGH,
        .fifo_enable    = false,
        .fifo_threshold = LL_DMA_FIFOTHRESHOLD_1_2,
        .mem_burst      = LL_DMA_MBURST_SINGLE,
        .periph_burst   = LL_DMA_PBURST_SINGLE,
        .enable_clock   = true,
        .fail_if_busy   = true
    };

    return DMA_LL_Init(&s_adc_dma, &cfg) &&
           DMA_LL_EnableIrq(&s_adc_dma, DMA_LL_IT_TC | DMA_LL_IT_HT | DMA_LL_IT_TE, 5U) &&
           DMA_LL_Start(&s_adc_dma, (uint32_t)&ADC1->DR, (uint32_t)s_samples, 256U);
}

void DMA1_Stream0_IRQHandler(void)      /* the vector matching BOARD_ADC_DMA_STREAM */
{
    DMA_LL_IrqService(&s_adc_dma, OnDma, NULL);
}
```

### D. UART TX on the CM7 (`dma_hal`)

```c
#include "dma_hal.h"

static DMA_HandleTypeDef s_hdma_tx;

HAL_StatusTypeDef InitConsoleTxDma(void)
{
    DmaHal_Config_t cfg = {
        .instance       = DMA2_Stream0,
        .request        = DMA_REQUEST_USART3_TX,
        .direction      = DMA_MEMORY_TO_PERIPH,
        .periph_inc     = DMA_PINC_DISABLE,
        .mem_inc        = DMA_MINC_ENABLE,
        .periph_align   = DMA_PDATAALIGN_BYTE,
        .mem_align      = DMA_MDATAALIGN_BYTE,
        .mode           = DMA_NORMAL,
        .priority       = DMA_PRIORITY_LOW,
        .fifo_mode      = DMA_FIFOMODE_DISABLE,
        .fifo_threshold = DMA_FIFO_THRESHOLD_FULL,
        .mem_burst      = DMA_MBURST_SINGLE,
        .periph_burst   = DMA_PBURST_SINGLE,
        .enable_clock   = true,
        .irq_priority   = 5                 /* >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY */
    };
    /* HAL_DMA_Init sets TRBUFF for this request; DMA_HAL_Init reads it back.
       Afterwards: __HAL_LINKDMA(huart, hdmatx, s_hdma_tx), and
       DMA_MEM_BM_CleanForTx() on the buffer before each HAL_UART_Transmit_DMA(). */
    return DMA_HAL_Init(&s_hdma_tx, &cfg);
}

void DMA2_Stream0_IRQHandler(void)
{
    DMA_HAL_IRQDispatch(DMA2_Stream0);
}
```

---

## 9. Validation

Board: NUCLEO-H755ZI-Q, silicon Rev V (`REV_ID = 0x2003`), PD5-PD6 jumper fitted.
Project: `02_firmware_projects/prj_04_adc_dma_bm_eth`, CMake preset `selftest`
(`DMA_DRIVER_SELFTEST=ON`). Each core prints `rev=<git hash>` before its
results.

| Test | Core / tier | What it proves |
| :--- | :--- | :--- |
| T1 | CM4 BM + LL | M2M on DMA1 streams 1-7, including 4-7 (`HISR` path). A matrix of 48 PSIZE = MSIZE combinations checked against a **fixed truth table transcribed from RM0399 Table 116** (independent of the driver): 23 valid ones run in hardware, 25 invalid ones are rejected with `CR == 0`, `mismatch = 0`. Also 3 PSIZE ≠ MSIZE transfers, and LL WORD→BYTE with an overflow canary. |
| T2 | CM4 BM + LL | Every rejection in §4, each asserting that `CR` stays 0. Includes Table 115, the peripheral burst rule, `PINC = 0` alignment, `fail_if_busy` on a live stream, and `SetIdleMemoryEx` with an out-of-range length. |
| T3 | CM4 BM + LL | USART2 TX→RX DMA loopback over PD5-PD6 at 115 200 baud, with TRBUFF read back as 1 on both streams without being requested. |
| T4 / T5 | CM4 BM + LL | Circular P2M paced by TIM7_UP (request 70) at 10 kHz (about 10 TC and 10 HT per second); DBM with `SetIdleMemoryEx` and `DMA_LL_SetIdleMemory`. |
| T6 | CM4 BM + LL | Stop mid-transfer, `TakeFlags`, `SetTransfer`, `StartEx`: completes with no phantom TC. |
| T7 | CM4 BM | ITCM address forced into `M0AR` behind the validation's back: TE set, stream disabled, system alive. |
| T8 / T8b | CM7 BM / LL | M2M between cacheable `.axi_ram` buffers with `CleanForTx`/`PrepareRx`/`CompleteRx`; `DMA_LL_MemCopy` WORD→BYTE; unaligned `PrepareRx` rejected. |
| T9 | CM7 HAL | `DMA_HAL_MemCopy` (including WORD→BYTE, with a cache-coherent overflow canary) and `HAL_DMA_Start_IT` through `DMA_HAL_IRQDispatch`; more than 65 535 items rejected without memory damage if the check regresses. |
| T10 / T10b | CM7 HAL | USART3 TX by DMA2, with `CR` read right after `HAL_DMA_Init` showing TRBUFF = 1. `DMA_HAL_CheckErrata` returns `HAL_BUSY` on an enabled stream and leaves `CR` unchanged. |
| T11 / T11b | CM7 HAL, BDMA | M2M SRAM4→SRAM4 and SRAM4↔backup SRAM on BDMA channel 0. A buffer in SRAM1 is rejected for BDMA. |
| T12 | Production | The normal `release` build of `prj_04`: ADC1 at about 500 kSPS (measured 503-511 kSPS), `err`/`ovr`/`adc_ovr` = 0 over hundreds of millions of samples. |
| T13 | CM7 HAL | Direct mode on DMA2_Stream2 (USART3_TX request). The 3 × 3 PSIZE × MSIZE matrix through `DMA_HAL_Init` is checked against a hand-written RM0399 table (accept the diagonal only): an accepted pair reads back in `CR[12:11]`/`CR[14:13]` with `DMDIS = 0`, and a rejected one leaves `CR == 0`. Then real HALFWORD and WORD direct-mode transfers to `USART3->TDR`, one character per item, each printing a console line. |

- **Evidence:** in `prj_04_adc_dma_bm_eth/docs/`.
  - `dma_selftest_capture.txt`: clean run at `rev=df46b94`.
  - `dma_t13_selftest_dd65b06.txt`: clean run of T1-T11 and T13 at
    `rev=dd65b06` (T13 `ok=3 rej=6 mismatch=0`). Its first two lines come from
    the image that was on the board before the flash.
  - `dma_t12_release_dd65b06_{console,query}.txt`: T12 at `rev=dd65b06`, 120
    queries over about 60 s (about 30 M samples, about 509 kSPS), `err`/`ovr`/`adc_ovr` = 0.
  - `dma_negative_controls.txt`: for each test, the defect it was built to
    catch, reintroduced, with the resulting FAIL on a `-dirty` image. For T13:
    NC1 (`dma_t13_nc1_dd65b06-dirty.txt`) and NC2
    (`dma_t13_nc2_dd65b06-dirty.txt`).
- **Independent run by the auditor:** repeated at `f3ab055` from a clean build,
  including a negative control that reintroduced the NDTR-unit bug and made T9
  fail.

**To reproduce:**

```powershell
.\tools\env.ps1
cd 02_firmware_projects\prj_04_adc_dma_bm_eth
cmake --preset selftest; cmake --build --preset selftest
py tools/monitor.py --port COM4 --timestamps --log dma.log --timeout 150   # start first, in another shell
py tools/flash.py --preset selftest --core both --yes
```

Then restore the production image with `--preset release`, and check T12 with
`py tools/query_adc.py --host <board ip> --rate 2`.

**Not exercised in hardware (covered by review only):**

- `DMA_LL_Deinit` returning `false` when `Stop` times out (cannot be induced).
- `DMA_HAL_Init` propagating a `CheckErrata` failure. `HAL_DMA_Init` disables
  the stream first, so `HAL_BUSY` cannot occur on that path.
- TRBUFF on HASH_IN (request 78). No HASH test exists.
- Silicon below `REV_ID 0x2000` (Rev Y), where TRBUFF is not applied.
- BDMA with LPUART1 or ADC3. Since `06_spi`, the BDMA has run with a real
  D3 peripheral: SPI6 through `dma_hal` (channels 0/1, requests 11/12, buffers
  in `.d3_ram`, up to 8192 frames; `06_spi/README.md` §9, T7(b)). Since `05_i2c`,
  also I2C4 through `dma_hal` (channels 0/1, DMAMUX2 requests 13/14, buffers in
  `.d3_ram`, 2 to 4096 bytes, from CPU1 with the BM, LL and HAL tiers of I2C;
  `05_i2c/README.md` §9, T7(b)).

**Defect N3 (found by `06_spi`), fixed in `57b1bf7`.** Before that commit, in
direct mode `DMA_HAL_Init` compared `periph_align` (`DMA_PDATAALIGN_*`, bits
12:11) with `mem_align` (`DMA_MDATAALIGN_*`, bits 14:13), so it refused direct
mode with half-words or words even when `PSIZE = MSIZE`. It now compares the
field values, as `dma_ll.c` does, and T13 covers it, with negative controls NC1
and NC2. A build older than `57b1bf7` still needs FIFO mode for those sizes on
DMA1/DMA2. The BDMA path never ran this check.

**Flash budget.** `dma_bm` linked into the `prj_04` CM4 release image takes
1 596 B (1 588 B `.text` + 8 B `.rodata`, measured from the `.map`). The first
version took 652 B, and the agreed ceiling is 1 676 B. With `--gc-sections`,
functions a project does not call cost nothing.

---

## 10. Known limitations and caveats

- **`DMA_BM_GetIRQn` / `DMA_HAL_GetIRQn` return `DMA_BM_IRQN_INVALID` on error,
  and that value is `SysTick_IRQn` (-1).** Passing it to `NVIC_SetPriority`
  reprograms SysTick. No negative value would be safe, which is why the original
  functions were left as they are and `…GetIRQnEx` (returns `bool`) was added.
  Use the `…Ex` variants.
- **`DMA_BM_InitStream` (original API) stops a running stream.** On a dual-core
  part that can be the other core's transfer. Use `InitStreamEx` with
  `fail_if_busy = true`, and the allocation contract of §3.
- **DBM idle-buffer writes race with the hardware switch.** Write from the TC
  ISR, as RM0399 recommends. If `SetIdleMemoryEx` returns `false` because TE is
  set, the switch won and the stream is now disabled. The original
  `DMA_BM_SetIdleMemory` checks reachability with the remaining NDTR, not the
  buffer length.
- **`DMA_BM_StartEx` returns `true` also when TE is already set.** It reports
  that the stream started, not that it succeeded. Check the flags.
- **Burst alignment is stricter than RM0399 for incrementing addresses**
  (beats × size instead of size plus no 1 KB crossing). That keeps the
  1 KB check static. It never rejects a correct `.dma_buf` buffer, but it can
  reject an odd start address that the hardware would accept.
- **The T1 matrix cannot catch a wrong `DMA_BM_CheckPeriphBurst`.** With equal
  sizes on both sides, the memory-side table already rejects every
  peripheral-forbidden combination. That function is covered by T2.
- **Timeouts are loop counts, not time.** Re-check `stop_spin_limit` and the LL
  `spin_limit` arguments at a different clock or optimisation level.
- **HAL tier:** addresses and lengths are validated only in `DMA_HAL_MemCopy`
  (§4). The IRQ registry is per core, because every image carries its own copy of
  the driver.

---

## 11. Scope

| Item | Status |
| :--- | :--- |
| BDMA in the BM/LL tiers (`bdma_bm`) | Deferred until a bare-metal D3 consumer exists (LPUART1, SPI6, I2C4, ADC3). It must respect ES0445 2.5.1. |
| MDMA | Out of scope: a different controller (linked lists, triggers) with no consumer. |
| DMAMUX synchronization and request generator | Out of scope. Their overrun flags are unreliable (ES0445 2.6.1-2.6.3). If added, respect 2.6.4. |
| Migrating `uart_hal` / `adc_hal` onto `dma_hal` | Not done. `UART_HAL_CheckDmaErrata` stays as a compatible check. |

---

## 12. Changing this driver

This driver is in a shared catalogue: several projects build it unchanged. Before
changing it:

1. **Public signatures and existing struct fields are immutable**
   (`01_shared_drivers/README.md` rule 2). Add `…Ex` functions or new fields at
   the end of a struct. Check with
   `git diff main -- '01_shared_drivers/02_dma/*.h'`: only additions or comment
   changes.
2. **Validate everything before the first register write.** A rejected
   configuration must leave the stream untouched, and T2 must prove it.
3. **Every new test needs a recorded negative control.** Reintroduce the defect,
   run it, record the FAIL with its `-dirty` hash in
   `dma_negative_controls.txt`, then restore. A test whose expected result is
   computed by the code under test proves nothing: use an independent oracle
   (see T1).
4. **Captures must identify the image** (`rev=<hash>`, clean tree), and every
   run must start from the commit that contains the test.
5. **Measure sizes on the linked `.map`**, excluding "Discarded input sections".
   The object file's `.text` is not the number.
6. **A failing test must not corrupt memory outside its own buffers.**
7. **Before merging:** `.\tools\build_all.ps1 -Clean` (8/8 with `-Werror`), a
   clean `selftest` build, the hardware run, and T12.

---

## 13. History

| Milestone | Commits |
| :--- | :--- |
| First `dma_bm` (DMA1/DMA2 + DMAMUX1) | `40fe018` |
| Hardening, `dma_mem_bm.h`, `dma_ll`, `dma_hal`, selftest | `b508e82` … `40105eb` |
| Audit fixes: NDTR in PSIZE units, NDT ≤ 65535 in HAL, Table 115, TRBUFF check, errata text | `52897a1` … `4c8d6fa`, closed at `f3ab055` |
| Iteration 2: `…Ex` IRQn and DBM APIs, peripheral burst rule, independent T1 oracle, backup SRAM for BDMA | `4922386` … `fd7b7b0` |
| `DMA_HAL_Init` refused HALFWORD and WORD direct mode: it compared the raw `DMA_PDATAALIGN_x` and `DMA_MDATAALIGN_x` constants, which coincide only for BYTE. Found by the 06_spi F3 selftest (N3). Fix and T13 | `57b1bf7`, `dd65b06` |

The implementation plans, the audit record and the iteration reports were
removed from this folder once their content was folded into this README. They
stay in git history:

```bash
git show 87f0fdd:01_shared_drivers/02_dma/AUDIT.md
```

The same commit also holds `IMPLEMENTATION_PLAN.md`, `IMPLEMENTATION_PLAN_2.md`,
`ITERATION_2.md` and `RESUMEN_Z1_ITERACION_2.md`.
