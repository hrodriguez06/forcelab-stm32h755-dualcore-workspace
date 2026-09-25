# STM32H755 Shared DMA Driver (`01_shared_drivers/02_dma`)

Peripheral-First (Plan A), multi-tier shared DMA driver for the STM32H755ZI dual-core microcontroller (DMA1, DMA2, DMAMUX1, and memory coherency helpers).

---

## 1. Architecture & Layering Matrix

| Driver Tier | Files | Library Target | Target Core | Description & Scope | ST Dependencies |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal Memory** | `dma_mem_bm.h` | Header-only (in `_bm`) | **Cortex-M7** / **M4** | 32-byte cache line alignment, D-Cache clean/invalidate with `__DMB()`, domain reachability validation | CMSIS only (`stm32h7xx.h`) |
| **Bare-Metal DMA** | `dma_bm.[ch]` | `stm32h7_driver_dma_bm` (alias `stm32h7_driver_dma`) | **Cortex-M4** / Early **M7** | Direct register manipulation of DMA1, DMA2 streams and DMAMUX1 channels. Single source of stream geometry | CMSIS only (`stm32h7xx.h`) |
| **Low-Level DMA** | `dma_ll.[ch]` | `stm32h7_driver_dma_ll` | **Cortex-M4** / **M7** | Lightweight handle-based API, CubeMX LL macros, bounded spin timeouts, interrupt callback service | LL inlines (`stm32h7xx_ll_*.h`) |
| **HAL DMA** | `dma_hal.[ch]` | `stm32h7_driver_dma_hal` | **Cortex-M7** | ST HAL wrapper for DMA1, DMA2 and BDMA, errata verification, IRQ dispatch registry | ST HAL (`stm32h7xx_hal.h`) |

### Capability Matrix (Bare-Metal Tier `_bm`)

| Feature | Support in `dma_bm` | Notes |
| :--- | :---: | :--- |
| **Controllers** | DMA1, DMA2 + DMAMUX1 | Stride 0x18 validated; indexed 0..7 |
| **Directions** | P2M, M2P, M2M | M2M forces FIFO mode and rejects circular |
| **Direct Mode** | Yes | Hardware mandates `PSIZE == MSIZE`; validated at Init |
| **FIFO Mode & Thresholds** | Yes (1/4, 1/2, 3/4, Full) | Validated with `DMA_BM_CheckFifoBurst` (RM0399 §16.3.11) |
| **Burst Transfers** | Yes (INCR4, INCR8, INCR16) | Memory burst via `CheckFifoBurst`; peripheral burst via `CheckPeriphBurst` (RM0399 §16.3.14). Static assert for 1 KB AHB limit. |
| **Double-Buffer Mode (DBM)** | Yes (`mem1_addr != 0`) | `DMA_BM_SetIdleMemory`, `DMA_BM_SetIdleMemoryEx` (with explicit length), `DMA_BM_GetCurrentTarget` |
| **Peripheral Flow Control** | Yes (`PFCTRL`) | Forbidden with M2M and circular |
| **Automatic Errata TRBUFF** | Yes (ES0445 2.20.3 / 2.3.12) | Applied automatically for UART/HASH when `REV_ID >= 0x2000` (silicon verified) |
| **Memory Domain Validation** | Yes (`DMA_MEM_BM_IsReachable`) | Rejects ITCM (`0x00000000`) and DTCM (`0x20000000`); NDTR evaluated in PSIZE units |
| **Safe IRQn Resolution** | Yes (`DMA_BM_GetIRQnEx`) | Returns `bool` + `out` pointer; `DMA_BM_IRQN_INVALID` sentinel defined |
| **Stream Collision Protection** | Yes (`fail_if_busy`) | Refuses Init if stream is active instead of aborting |

### Capability Matrix (HAL Tier `_hal` & BDMA)

| Feature | Support in `dma_hal` | Notes |
| :--- | :---: | :--- |
| **Controllers** | DMA1, DMA2 (16 streams) + BDMA (8 channels) | Supports both standard DMA streams and D3 BDMA channels |
| **M2M FIFO Enforcement** | Yes | Reconciles HAL defect by forcing `DMA_FIFOMODE_ENABLE` for M2M |
| **Domain Checking** | Yes (`DMA_MEM_BM_IsReachable`) | Enforces DMA1/DMA2 cannot reach TCM; BDMA validated for SRAM4 and Backup SRAM |
| **Automatic Errata TRBUFF** | Yes (`DMA_HAL_CheckErrata`) | Verified on hardware; returns `HAL_BUSY` if stream EN=1 |
| **Safe IRQn Resolution** | Yes (`DMA_HAL_GetIRQnEx`) | Returns `bool` + `out` pointer to avoid SysTick_IRQn aliasing |
| **IRQ Registry & Dispatch** | Yes (`DMA_HAL_IRQDispatch`) | Per-core 24-entry table; no vector symbol definitions in driver |
| **Cache-Coherent MemCopy** | Yes (`DMA_HAL_MemCopy`) | Handles clean, prepare, transfer, wait, and complete invalidate (NDTR in PSIZE, items <= 65535) |

---

## 2. Hardware Agnosticism & Board Definition Contract

Shared drivers must **never** hardcode peripheral stream instances or channels. All hardware allocations are injected through configuration structures (`DmaBm_Config_t`, `DmaBm_ConfigEx_t`) or defined in `board_def.h`.

### Contract in `board_def.h`
Every project utilizing DMA streams allocates them explicitly in `board_def.h`:

```c
/* DMA stream allocation - ONE line per stream in use, per core.
   A stream listed twice indicates a dual-core collision. */
#define BOARD_ADC_DMA_STREAM        DMA1_Stream0   /* CM4 ADC1 */
#define BOARD_ADC_DMA_REQUEST       9U             /* DMAMUX1: adc1_dma */
```

---

## 3. Silicon Errata Compliance (ES0445)

1. **USART/UART DMA Lock (ES0445 2.20.3 / 2.21.1)**:
   The DMA stream can lock permanently unless bit 20 (`TRBUFF`) is set in `DMA_SxCR`.
   `dma_bm` automatically enables `TRBUFF` for USART/UART requests {41–46, 63–66, 71–72, 79–82} when silicon revision `REV_ID >= 0x2000` (`(DBGMCU->IDCODE & 0xFFFF0000U) >= 0x20000000U`). Can also be forced via `enable_trbuff = true`.
2. **HASH_IN DMA Transfer Lock (ES0445 2.3.12)**:
   DMAMUX1 request 78 (HASH_IN) also requires `TRBUFF` bit 20. Handled automatically by `dma_bm`.
3. **Cache Maintenance Barrier (ES0445 2.1.5)**:
   A `__DMB()` memory barrier must follow every D-Cache clean and invalidate operation before data is accessed. Handled in `dma_mem_bm.h`.
4. **No Write-Through Cache (ES0445 2.1.1)**:
   Never configure Cortex-M7 cache policy as write-through; use write-back with explicit maintenance (`DMA_MEM_BM_CleanForTx`, `DMA_MEM_BM_PrepareRx`, `DMA_MEM_BM_CompleteRx`) or non-cacheable MPU regions.
5. **DMAMUX Synchronization (ES0445 2.6.4)**:
   When synchronization is disabled (`SE=0`), `SPOL[1:0]` must remain `00`. `dma_bm` writes `CCR` cleanly without setting spurious sync bits.

---

## 4. Usage Examples

### A. Memory-to-Memory Transfer with FIFO on Cortex-M4 (`dma_bm`)

```c
#include "dma_bm.h"

static uint32_t s_src[64];
static uint32_t s_dst[64];

bool RunMemCopy(void)
{
    DmaBm_ConfigEx_t cfg = {
        .base = {
            .stream           = DMA1_Stream1,
            .request_id       = 0, /* Ignored in M2M */
            .direction        = DMA_BM_DIR_MEM_TO_MEM,
            .periph_data_size = DMA_BM_DATA_SIZE_WORD,
            .mem_data_size    = DMA_BM_DATA_SIZE_WORD,
            .periph_inc       = true, /* Source increments */
            .mem_inc          = true, /* Destination increments */
            .circular         = false,
            .priority         = DMA_BM_PRIORITY_HIGH,
            .enable_fifo      = true,
            .fifo_threshold   = DMA_BM_FIFO_TH_FULL,
            .periph_addr      = (uint32_t)s_src,
            .mem_addr         = (uint32_t)s_dst,
            .buffer_length    = 64
        },
        .mem1_addr       = 0,
        .mem_burst       = DMA_BM_BURST_INCR4,
        .periph_burst    = DMA_BM_BURST_INCR4,
        .fail_if_busy    = true,
        .stop_spin_limit = 10000
    };

    if (!DMA_BM_InitStreamEx(&cfg)) {
        return false;
    }

    if (!DMA_BM_StartEx(DMA1_Stream1)) {
        return false;
    }

    while ((DMA_BM_GetFlags(DMA1_Stream1) & (DMA_BM_FLAG_TC | DMA_BM_FLAG_TE)) == 0) {
        /* Poll or yield */
    }

    uint32_t flags = DMA_BM_TakeFlags(DMA1_Stream1);
    return (flags & DMA_BM_FLAG_TC) != 0U;
}
```

### B. Cortex-M7 Cache Maintenance (`dma_mem_bm.h`)

```c
#include "dma_mem_bm.h"

#define BUFFER_SIZE  128
uint8_t tx_buf[BUFFER_SIZE] DMA_MEM_BM_ALIGN;
uint8_t rx_buf[BUFFER_SIZE] DMA_MEM_BM_ALIGN;

void SetupTransfers(void)
{
    /* 1. Before sending data out via DMA: flush CPU dirty cache lines */
    DMA_MEM_BM_CleanForTx(tx_buf, BUFFER_SIZE);

    /* 2. Before receiving data into buffer: clean and invalidate */
    DMA_MEM_BM_PrepareRx(rx_buf, BUFFER_SIZE);
}

void TransferComplete_ISR(void)
{
    /* 3. After DMA completes reception: invalidate to fetch updated RAM contents */
    DMA_MEM_BM_CompleteRx(rx_buf, BUFFER_SIZE);
}
```

### C. Low-Layer Transfer with Interrupts & Callbacks (`dma_ll`)

```c
#include "dma_ll.h"

static DmaLL_Handle_t s_dma_h;

void MyDmaCallback(DmaLL_Handle_t *h, uint32_t flags, void *ctx)
{
    if (flags & DMA_LL_FLAG_TC) {
        /* Transfer complete */
    }
}

void StartDmaLL(void)
{
    DmaLL_Config_t cfg = {
        .stream         = DMA1_Stream1,
        .request        = LL_DMAMUX1_REQ_ADC1,
        .direction      = LL_DMA_DIRECTION_PERIPH_TO_MEMORY,
        .mode           = LL_DMA_MODE_NORMAL,
        .periph_inc     = LL_DMA_PERIPH_NOINCREMENT,
        .mem_inc        = LL_DMA_MEMORY_INCREMENT,
        .periph_size    = LL_DMA_PDATAALIGN_HALFWORD,
        .mem_size       = LL_DMA_MDATAALIGN_HALFWORD,
        .priority       = LL_DMA_PRIORITY_HIGH,
        .fifo_enable    = false,
        .fifo_threshold = LL_DMA_FIFOTHRESHOLD_1_2,
        .mem_burst      = LL_DMA_MBURST_SINGLE,
        .periph_burst   = LL_DMA_PBURST_SINGLE,
        .double_buffer  = false,
        .enable_clock   = true,
        .fail_if_busy   = true
    };

    DMA_LL_Init(&s_dma_h, &cfg);
    DMA_LL_EnableIrq(&s_dma_h, DMA_LL_IT_TC | DMA_LL_IT_TE, 5U);
    DMA_LL_Start(&s_dma_h, (uint32_t)&ADC1->DR, (uint32_t)rx_buf, BUFFER_SIZE / 2);
}

void DMA1_Stream1_IRQHandler(void)
{
    DMA_LL_IrqService(&s_dma_h, MyDmaCallback, NULL);
}
```

### D. HAL DMA & BDMA on Cortex-M7 (`dma_hal`)

```c
#include "dma_hal.h"

static DMA_HandleTypeDef s_hdma_tx;

void InitUartDma(void)
{
    DmaHal_Config_t cfg = {
        .instance         = DMA2_Stream0,
        .request          = DMA_REQUEST_USART3_TX,
        .direction        = DMA_MEMORY_TO_PERIPH,
        .periph_inc       = DMA_PINC_DISABLE,
        .mem_inc          = DMA_MINC_ENABLE,
        .periph_align     = DMA_PDATAALIGN_BYTE,
        .mem_align        = DMA_MDATAALIGN_BYTE,
        .mode             = DMA_NORMAL,
        .priority         = DMA_PRIORITY_LOW,
        .fifo_mode        = DMA_FIFOMODE_ENABLE,
        .fifo_threshold   = DMA_FIFO_THRESHOLD_FULL,
        .mem_burst        = DMA_MBURST_SINGLE,
        .periph_burst     = DMA_PBURST_SINGLE,
        .enable_clock     = true,
        .irq_priority     = 5  /* Configures NVIC priority and enables IRQ */
    };

    /* Inits HAL handle and auto-verifies TRBUFF workaround */
    DMA_HAL_Init(&s_hdma_tx, &cfg);
}

/* User application or project isr file delegates to registry dispatch */
void DMA2_Stream0_IRQHandler(void)
{
    DMA_HAL_IRQDispatch(DMA2_Stream0);
}
```

---

## 5. Error Handling Policy (RM0399 §16.3.20)

In both `dma_bm` and `dma_ll`:
- **Transfer Error (TEIF / `DMA_LL_FLAG_TE`)**: Occurs on bus error during DMA read/write. Per RM0399 §16.3.20, hardware automatically clears `CR.EN` and disables the stream immediately. Accumulated in `h->error_flags` and passed to callback.
- **Direct Mode Error (DMEIF / `DMA_LL_FLAG_DME`)**: Occurs when a new transfer request arrives in direct mode before the previous one finished. Hardware automatically clears `CR.EN` and disables the stream. Accumulated in `h->error_flags` and passed to callback.
- **FIFO Error (FEIF / `DMA_LL_FLAG_FE`)**: Occurs on FIFO overrun/underrun. Hardware does NOT automatically disable the stream; it sets FEIF and continues once FIFO levels are restored (identical to ST HAL policy which omits FEIE in `HAL_DMA_Start_IT`). Tracked in `h->error_flags` but considered non-fatal.
- **Error Retrieval**: In `dma_ll`, `DMA_LL_TakeErrors()` retrieves and clears accumulated errors inside a `PRIMASK` critical section, preventing race conditions with ISR execution.

---

## 6. Hardware Verification & Silicon Findings (Iteration 2)

All driver tiers underwent exhaustive verification on physical STM32H755ZI-Q silicon (device ID 0x450, silicon revision ID 0x2003, Rev V) in project `prj_04_adc_dma_bm_eth`:

1. **TRBUFF Workaround Verification (ES0445 2.20.3 / 2.3.12)**:
   - Hardware readback of `DMA_SxCR` immediately after `HAL_DMA_Init` on USART3 TX stream confirms that bit 20 (`TRBUFF`) is actively set by the driver on revision >= 0x2000.
   - `DMA_HAL_CheckErrata()` returns `HAL_BUSY` when called on an active stream (`EN=1`) and leaves `CR` unmodified. Note: `HAL_DMA_Init()` clears `EN` before calling errata verification, so propagation of `HAL_BUSY` cannot be tested through `DMA_HAL_Init()`.

2. **BDMA Access to Backup SRAM (RM0399 Rev 4 Fig. 1, Table 2, §2.1.2)**:
   - Silicon test T11b confirmed bidirectional 32-byte M2M transfers between SRAM4 (`0x38000000`) and Backup SRAM (`0x38800000`) via `BDMA_Channel0`.
   - Backup SRAM is confirmed fully accessible to BDMA through the D3 32-bit AHB bus matrix when `PWR->CR1 |= PWR_CR1_DBP` and `RCC->AHB4ENR |= RCC_AHB4ENR_BKPRAMEN` are active. The entry in `dma_mem_bm.h` is retained as valid silicon capability.

3. **Peripheral Burst Rule (RM0399 §16.3.14)**:
   - The peripheral side of the DMA FIFO operates under dedicated hardware rules distinct from memory FIFO bursts: $(PBURST \times PSIZE) \le 16\text{ bytes}$, and if $(PBURST \times PSIZE) = 16\text{ bytes}$, threshold 3/4 is forbidden.
   - When `PINC=0` (fixed peripheral address), alignment is only required to `PSIZE`, not to the burst beat span.

4. **Combinatorial FIFO Burst Matrix**:
   - Tested all 48 combinations of PSIZE = MSIZE across data sizes {BYTE, HALFWORD, WORD}, thresholds {1/4, 1/2, 3/4, FULL}, and bursts {SINGLE, INCR4, INCR8, INCR16}.
   - All 23 valid combinations execute 64-byte M2M transfers verified with `memcmp`.
   - All 25 invalid combinations are rejected by `DMA_BM_InitStreamEx` with `CR == 0`.
   - Tested 3 valid cross-size combinations (BYTE -> WORD, HALFWORD -> WORD, WORD -> BYTE) with byte-for-byte correctness confirmed.


