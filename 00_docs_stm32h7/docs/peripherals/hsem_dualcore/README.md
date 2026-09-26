# HSEM (Hardware Semaphore) & Dual-Core IPC - STM32H755

## 1. Hardware Semaphore Overview
The STM32H755 provides **32 hardware semaphores (HSEM)** to coordinate mutual exclusion between CPU1 (Cortex-M7) and CPU2 (Cortex-M4).
- HSEM operations are atomic hardware bus accesses located on the AHB4 bus matrix (D3 domain).
- Hardware latency: register access takes ~40 ns (~16 cycles at 400 MHz on CM7, ~8 cycles at 200 MHz on CM4).
- Supports 2-step lock (write `COREID` and `PROCID` to `Rx`, read back to verify) and 1-step fast lock (atomic read of `RLRx`).
- **Core IDs (RM0399 Table 95):** CPU1 (Cortex-M7) = `3`, CPU2 (Cortex-M4) = `1`.

> [!WARNING]
> **Single-Core Re-entrancy (S1 Pitfall):** The 1-step fast lock (`RLR`) automatically assigns `PROCID = 0`. The hardware cannot distinguish between different tasks or interrupt handlers running on the same core. Multiple contexts on the same core must use the 2-step lock (`Take`) with distinct non-zero `PROCID`s.

---

## 2. Semaphore ID Allocation Convention

Semaphore IDs are defined at the project level (in `shared_memory.h` or `board_def.h`), not hardcoded in the driver (Rule C1). The canonical allocation in `prj_04_adc_dma_bm_eth` is:

| Semaphore ID | Assigned Purpose | Core Usage |
| :--- | :--- | :--- |
| **0** (`HSEM_ID_BOOT_SYNC`) | System Boot & Clock Synchronization | CPU1 releases CPU2 from STOP |
| **1** (`HSEM_ID_SHARED_STATE`) | Shared RAM IPC State Mutex | Dual-core state coordination |
| **2** (`HSEM_ID_M7_TO_M4_MBOX`) | Mailbox M7 -> M4 | Inter-core command channel |
| **3** (`HSEM_ID_M4_TO_M7_MBOX`) | Mailbox M4 -> M7 | Inter-core telemetry channel |
| **4** (`HSEM_ID_ADC_DATA`) | ADC Telemetry & Waveform buffer | M4 producer, M7 network consumer |
| **5** (`BOARD_GPIO_HSEM_ID`) | Shared GPIO Configuration Hook | Reserved for `00_gpio` |
| **24..31** | Driver Hardware Verification & Selftest | Reserved for hardware diagnostics |

---

## 3. Recommended Driver Patterns (`01_shared_drivers/10_hsem`)

Use the centralized shared drivers (`hsem_bm.h`, `hsem_ll.h`, `hsem_hal.h`) which enforce memory barriers (`__DMB()`), honest release checking, PRIMASK protection, and bounded cycle timeouts.

### 1-Step Fast Lock (Single context per core)
```c
#include "hsem_bm.h"

if (HSEM_BM_FastTake(HSEM_ID_ADC_DATA) == HSEM_BM_OK)
{
    /* Critical section (DMB already performed by driver) */
    UpdateSharedTelemetry();

    /* Release semaphore (validates ownership before clearing) */
    HSEM_BM_Release(HSEM_ID_ADC_DATA, 0U);
}
```

### 2-Step Lock with Process ID (Multi-task / Multi-context)
```c
#define TASK_PROC_ID 0x42U

if (HSEM_BM_Take(HSEM_ID_SHARED_STATE, TASK_PROC_ID) == HSEM_BM_OK)
{
    /* Critical section */
    ProcessState();

    /* Release matching PROCID */
    HSEM_BM_Release(HSEM_ID_SHARED_STATE, TASK_PROC_ID);
}
```

### Bounded Wait with Cycle Timeout and Yield
```c
/* Wait up to 50,000 DWT cycles (~125 µs @ 400 MHz), yielding to OS on contention */
HsemBm_Status_t status = HSEM_BM_TakeTimeoutEx(
    HSEM_ID_ADC_DATA, 0U, 50000U,
    (void (*)(void *))taskYIELD, NULL);

if (status == HSEM_BM_OK)
{
    ReadWaveformBuffer();
    HSEM_BM_Release(HSEM_ID_ADC_DATA, 0U);
}
```

---

## 4. Interrupt & Event Notifications
HSEM can generate an interrupt or an event on the opposite core when a semaphore is released.
- `HSEM_BM_NotifyEnable()` enables interrupt line notification after clearing stale flags.
- `HSEM_BM_TakeOrNotify()` attempts acquisition and arms the notification atomically.
- In boot sequences, `SEVONPEND` converts the pending interrupt flag into an event waking CPU2 from `WFE` (see `00_core_and_system/dual_core_boot_ipc.md`).

For detailed architectural contracts and benchmarks, see [`01_shared_drivers/10_hsem/README.md`](../../../01_shared_drivers/10_hsem/README.md).
