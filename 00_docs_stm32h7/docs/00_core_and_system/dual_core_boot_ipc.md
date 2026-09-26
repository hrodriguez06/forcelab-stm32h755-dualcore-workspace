# STM32H755 Dual-Core Boot Sequence & Inter-Processor Communication (IPC)

The STM32H755 contains two independent processor cores:
- **CPU1**: Arm Cortex-M7 (up to 480 MHz)
- **CPU2**: Arm Cortex-M4 (up to 240 MHz)

---

## 1. Boot Synchronization Flow

By default with STM32CubeIDE / CubeMX dual-core projects:
1. **Reset**: Both cores can boot simultaneously or CPU1 boots first (controlled by `BCM7` and `BCM4` Option Bytes).
2. **Standard Practice (CPU1 Master)**:
   - CPU1 initializes System Power (SMPS/LDO), Flash latency, RCC, and PLLs.
   - CPU2 waits in a low-power sleep/WFE loop on a Hardware Semaphore (**HSEM**).
   - Once CPU1 finishes clock configuration, it releases the HSEM.
   - CPU2 detects the release, initializes its local MPU and peripherals, and enters its main loop.

### Boot Synchronization Flow & Driver Implementation (`10_hsem`)

> [!CAUTION]
> **Fragile Polling Pitfall:** The naive CubeMX template code polls `while (!HAL_HSEM_IsSemTaken(0))` followed by `while (HAL_HSEM_IsSemTaken(0))`. Because CPU1 acquires and releases HSEM 0 in rapid succession, CPU2 frequently misses the window where the semaphore was taken and hangs permanently. Furthermore, waking CPU2 from deep STOP mode across warm resets requires event-based notification (`SEVONPEND` + `WFE`) rather than interrupt handling.

The centralized driver utility `01_shared_drivers/10_hsem/hsem_bm.h` implements a robust, race-free handshake:

**CPU1 (Cortex-M7):**
```c
#include "hsem_bm.h"

/* 1. Release CPU2 boot core in RCC_GCR and wait until CPU2 parks in STOP (D2CKRDY=0) */
HAL_RCCEx_EnableBootCore(RCC_BOOT_C2);
HSEM_BM_Init();
HSEM_BM_TimebaseStart();
(void)HSEM_BM_BootWaitCpu2Parked(32000000UL); /* 500 ms bounded timeout @ 64 MHz HSI */

/* 2. Configure Power, MPU, Caches, Clocks and PLLs */
MPU_Config();
SystemClock_Config();

/* 3. Initialize shared memory in SRAM4 and publish magic sentinel last */
Shared_Init();

/* 4. Release CPU2 by generating HSEM 0 release event */
(void)HSEM_BM_BootReleaseCpu2(HSEM_ID_BOOT_SYNC, 32000000UL);
```

**CPU2 (Cortex-M4):**
```c
#include "hsem_bm.h"

static bool Shared_MagicReady(void *ctx)
{
    (void)ctx;
    return (g_shared.magic == SHARED_MAGIC);
}

int main(void)
{
    /* 1. Arm HSEM notification, set SEVONPEND, and enter STOP mode via WFE.
          Loop verifies both the HSEM flag and the shared magic predicate
          to safely ignore spurious wakeups. */
    const HsemBm_BootWait_t cfg = {
        .sem_id = HSEM_ID_BOOT_SYNC,
        .ready  = Shared_MagicReady,
        .ctx    = NULL
    };
    (void)HSEM_BM_BootWaitForCpu1(&cfg);

    /* 2. Clocks are stable and shared memory is ready: proceed with CPU2 peripherals */
    HAL_Init();
    HSEM_BM_Init();
    HSEM_BM_TimebaseStart();
    /* ... */
}
```

---

## 2. Resource Allocation Principles

| Category | CPU1 (Cortex-M7) Responsibilities | CPU2 (Cortex-M4) Responsibilities |
| :--- | :--- | :--- |
| **System** | Clock generation (PLL1/2/3), Power supply | Secondary clocks monitoring |
| **High Throughput** | Ethernet, USB HS, Display (LTDC/FMC), DSP/AI | Motor control, hard real-time sensors |
| **Fieldbuses** | Industrial stacks | FDCAN, Modbus, UART protocols |
| **Memory** | AXI SRAM, ITCM, DTCM, Flash Bank 1 | D2 SRAM1/2/3, Flash Bank 2 |

---

## 3. Shared Memory & Mutex with HSEM

To share buffers (e.g. sensor readings or command queues) between M7 and M4:
1. Place the shared struct in a non-cacheable region of **SRAM4 (`0x38000000`)** or **D2 SRAM3 (`0x30040000`)**.
2. Protect multi-word access with an HSEM semaphore ID:

```c
#include "hsem_bm.h"

bool IPC_WriteData(const void* src, size_t len)
{
    if (HSEM_BM_FastTake(HSEM_ID_SHARED_STATE) == HSEM_BM_OK) {
        /* Driver enforces __DMB() memory barrier on take */
        memcpy((void*)SHARED_BUFFER_ADDR, src, len);
        /* Honest release: validates caller ownership before clearing */
        HSEM_BM_Release(HSEM_ID_SHARED_STATE, 0U);
        return true;
    }
    return false;
}
```

