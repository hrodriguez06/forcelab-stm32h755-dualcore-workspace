# STM32H755 Shared Hardware Semaphore (HSEM) Driver (`01_shared_drivers/10_hsem`)

Peripheral-First (Plan A), 3-tier shared Hardware Semaphore driver for the
STM32H755ZI dual-core microcontroller: Cortex-M7 (CPU1) and Cortex-M4 (CPU2).
Provides atomic mutual exclusion, single-core collision protection, bounded
cycle timeouts, interrupt notifications, and a race-free dual-core boot handshake.

Status: Implemented and audited. Corrective actions for independent audit findings
(B1–B6, M1–M7, m1–m8) are implemented and verified via multi-project CI builds and
hardware-in-the-loop self-tests. The test suite is self-identifying and every
hardware test has a recorded negative control. What is covered, and what is not,
is documented in [section 9](#9-validation).

---

## 1. Tiers

| Tier | Files | CMake Target | Links | ST Dependency | Typical Use |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal** | `hsem_bm.[c\|h]` | `stm32h7_driver_hsem_bm` | — | CMSIS only (`stm32h7xx.h`) | Cortex-M4 deterministic loops, pre-RTOS bring-up, boot handshake, fault diagnostics |
| **Low-Level** | `hsem_ll.[c\|h]` | `stm32h7_driver_hsem_ll` | `stm32h7_driver_hsem_bm` | `stm32h7xx_ll_hsem.h` inlines | LL inline wrappers, identical semantics to BM |
| **HAL** | `hsem_hal.[c\|h]` | `stm32h7_driver_hsem_hal` | `stm32h7_driver_hsem_bm` | ST HAL (`HAL_HSEM_*`) | Cortex-M7 FreeRTOS / lwIP, HAL handle callbacks |

```cmake
target_link_libraries(cm4_firmware PRIVATE stm32h7_driver_hsem_bm)   # or _ll
target_link_libraries(cm7_firmware PRIVATE stm32h7_driver_hsem_hal)  # or _bm, _ll
```

```c
#include "hsem_bm.h"    /* never "10_hsem/hsem_bm.h": the directory is on the include path */
```

- **No unsuffixed target (Rule C4):** There is deliberately no `stm32h7_driver_hsem`
  target. Unlike DMA/GPIO (where unsuffixed means BM) or UART/ADC (where unsuffixed
  means HAL), HSEM was built with all three tiers simultaneously; callers must
  explicitly select `_bm`, `_ll`, or `_hal`.
- **`hsem_bm` is the single source of truth:** Validation logic, PRIMASK protection,
  memory barriers (`__DMB()`), DWT cycle timebase, and the dual-core boot handshake
  live exclusively in `hsem_bm`. Both LL and HAL link against `hsem_bm` and reuse
  these primitives.
- **Zero RAM state in BM and LL (Rule C7):** `.bss` and `.data` of `hsem_bm.o` and
  `hsem_ll.o` are exactly **0 bytes**. Both cores execute the same code without
  sharing any memory state beyond the hardware registers.
- **Strict downward dependencies (Rule C8):** BM depends only on CMSIS headers.
  LL depends only on ST LL inlines and BM. HAL is enclosed within
  `#if defined(HAL_HSEM_MODULE_ENABLED)` and BM. The driver never includes FreeRTOS
  or project headers (`board_def.h`, `shared_memory.h`).
- **No reserved project symbols (Rule A11):** The driver never defines
  `HAL_HSEM_FreeCallback` or `HSEMx_IRQHandler`. Those belong strictly to the
  application project or CubeMX HAL overrides.

### 1.1 Feature Matrix

| Feature | BM | LL | HAL |
| :--- | :---: | :---: | :---: |
| 1-Step Fast Lock (atomic `RLR` read) | yes | yes | yes |
| 2-Step Lock (`Rx` write + readback with `PROCID`) | yes | yes | yes |
| Single-core re-entrancy / self-collision detection (S1) | yes (`BUSY_SELF`) | yes (`BUSY_SELF`) | yes (`HAL_BUSY`) |
| Honest release verification (checks owner before clearing) | yes | yes | yes |
| Memory barriers (`__DMB()`) on lock/unlock | yes | yes | yes |
| Strictly 32-bit word register accesses | yes | yes | yes |
| Bounded wait in CPU cycles (DWT `CYCCNT`) | yes | yes | yes |
| Cooperative `relax` callback hook during wait | yes (`…Ex`) | yes (`…Ex`) | yes (`…Ex`) |
| Query lock status, owner core and process ID | yes | via BM | via BM |
| Active semaphore bitmask (`GetLockedMask`) | yes | via BM | via BM |
| Bulk release by core with security key (`ReleaseAllOfCore`) | yes | via BM | via BM |
| Interrupt / event notification enable/disable | yes | yes | yes |
| Atomic take-or-notify (`TakeOrNotify`) | yes | no | no |
| IRQ service with trailing `__DSB()` | — | — | `HSEM_HAL_IRQService` |
| Callback dispatch mechanism | `NotifyTake` | `NotifyTake` | `RegisterCallback` / `Dispatch` |
| Race-free dual-core boot handshake | yes | via BM | via BM |

---

## 2. API Summary

### 2.1 Return Status (`HsemBm_Status_t`)

All BM and LL functions map to these canonical status codes:

| Code | Value | Description |
| :--- | :---: | :--- |
| `HSEM_BM_OK` | 0 | Semaphore acquired by caller, or operation successful |
| `HSEM_BM_BUSY` | 1 | Semaphore is currently held by the other core |
| `HSEM_BM_BUSY_SELF` | 2 | Held by THIS core: other PROCID, other context, or re-entry (S1) |
| `HSEM_BM_TIMEOUT` | 3 | Semaphore was not acquired before `max_cycles` elapsed |
| `HSEM_BM_ERR_PARAM` | 4 | Invalid parameter (`id > 31`, `proc_id > 255`, `cycles == 0`, key `0`) |
| `HSEM_BM_ERR_NOT_OWNER` | 5 | Release refused: caller is not the registered owner; nothing written |
| `HSEM_BM_ERR_TIMEBASE` | 6 | DWT `CYCCNT` not running: a bounded cycle wait is impossible |
| `HSEM_BM_ERR_CORE` | 7 | Build flag `CORE_CM4` disagrees with `SCB->CPUID`, or RCC clock off |

HAL functions return standard `HAL_StatusTypeDef` (`HAL_OK`, `HAL_BUSY`, `HAL_TIMEOUT`, `HAL_ERROR`).

### 2.2 `hsem_bm.h` API

```c
/* Initialization & Timebase Primitives */
HsemBm_Status_t HSEM_BM_Init(void);
HsemBm_Status_t HSEM_BM_CheckInit(void);
bool            HSEM_BM_TimebaseStart(void);
uint32_t        HSEM_BM_GetCoreId(void);

/* Locking & Releasing */
HsemBm_Status_t HSEM_BM_FastTake(uint32_t id);
HsemBm_Status_t HSEM_BM_Take(uint32_t id, uint32_t proc_id);
HsemBm_Status_t HSEM_BM_TakeTimeout(uint32_t id, uint32_t proc_id, uint32_t max_cycles);
HsemBm_Status_t HSEM_BM_TakeTimeoutEx(uint32_t id, uint32_t proc_id, uint32_t max_cycles,
                                      void (*relax)(void *ctx), void *ctx);
HsemBm_Status_t HSEM_BM_Release(uint32_t id, uint32_t proc_id);

/* Status & Inspection (strictly reads Rx, never RLRx) */
bool            HSEM_BM_IsLocked(uint32_t id);
bool            HSEM_BM_GetOwner(uint32_t id, HsemBm_Owner_t *out);
uint32_t        HSEM_BM_GetLockedMask(uint32_t core_id);

/* Recovery (RM0399 §11.3.6) */
bool            HSEM_BM_SetClearKey(uint16_t key);
HsemBm_Status_t HSEM_BM_ReleaseAllOfCore(uint32_t core_id, uint16_t key);

/* Notifications */
bool            HSEM_BM_NotifyEnable(uint32_t sem_mask);
bool            HSEM_BM_NotifyDisable(uint32_t sem_mask);
uint32_t        HSEM_BM_NotifyTake(void);
uint32_t        HSEM_BM_GetRawStatus(void);
IRQn_Type       HSEM_BM_GetIRQn(void);
HsemBm_Status_t HSEM_BM_TakeOrNotify(uint32_t id, uint32_t proc_id);

/* Dual-Core Boot Handshake */
HsemBm_Status_t HSEM_BM_BootWaitForCpu1(const HsemBm_BootWait_t *cfg);
HsemBm_Status_t HSEM_BM_BootWaitCpu2Parked(uint32_t max_cycles);
HsemBm_Status_t HSEM_BM_BootReleaseCpu2(uint32_t sem_id, uint32_t max_cycles);
```

### 2.3 `hsem_ll.h` API

Drop-in replacement using ST LL static inlines (`stm32h7xx_ll_hsem.h`), maintaining identical
status codes and safety invariants:

```c
HsemBm_Status_t HSEM_LL_FastTake(uint32_t id);
HsemBm_Status_t HSEM_LL_Take(uint32_t id, uint32_t proc_id);
HsemBm_Status_t HSEM_LL_TakeTimeout(uint32_t id, uint32_t proc_id, uint32_t max_cycles);
HsemBm_Status_t HSEM_LL_TakeTimeoutEx(uint32_t id, uint32_t proc_id, uint32_t max_cycles,
                                      void (*relax)(void *ctx), void *ctx);
HsemBm_Status_t HSEM_LL_Release(uint32_t id, uint32_t proc_id);
bool            HSEM_LL_NotifyEnable(uint32_t sem_mask);
bool            HSEM_LL_NotifyDisable(uint32_t sem_mask);
uint32_t        HSEM_LL_NotifyTake(void);
```

*(Diagnostic queries, recovery, and boot handshake are reused directly from `hsem_bm.h`.)*

### 2.4 `hsem_hal.h` API

Wrapper around ST HAL (`stm32h7xx_hal_hsem.c`) that injects parameter validation,
`__DMB()` memory barriers, honest release verification, DWT cycle timeouts, and
callback registration:

```c
HAL_StatusTypeDef HSEM_HAL_FastTake(uint32_t id);
HAL_StatusTypeDef HSEM_HAL_Take(uint32_t id, uint32_t proc_id);
HAL_StatusTypeDef HSEM_HAL_TakeTimeout(uint32_t id, uint32_t proc_id, uint32_t max_cycles);
HAL_StatusTypeDef HSEM_HAL_TakeTimeoutEx(uint32_t id, uint32_t proc_id, uint32_t max_cycles,
                                        void (*relax)(void *ctx), void *ctx);
HAL_StatusTypeDef HSEM_HAL_Release(uint32_t id, uint32_t proc_id);
HAL_StatusTypeDef HSEM_HAL_NotifyEnable(uint32_t sem_mask);
HAL_StatusTypeDef HSEM_HAL_NotifyDisable(uint32_t sem_mask);
HAL_StatusTypeDef HSEM_HAL_RegisterCallback(HsemHal_FreeCallback_t cb, void *ctx);
void              HSEM_HAL_IRQService(void);
void              HSEM_HAL_Dispatch(uint32_t sem_mask);
```

---

## 3. Board Definition & Architecture Contract

### 3.1 Rule C1: Dynamic Allocation Only
The driver does not contain any `#define` for semaphore IDs, security keys, or PROCIDs.
All identifiers are supplied dynamically by the caller.

In dual-core projects (`prj_04_adc_dma_bm_eth`), IDs are declared in `shared_memory.h`:

```c
#define HSEM_ID_BOOT_SYNC         0U   /* reserved: M7 -> M4 boot release      */
#define HSEM_ID_SHARED_STATE      1U   /* guards SharedMemory_t.state          */
#define HSEM_ID_M7_TO_M4_MBOX     2U   /* guards m7_to_m4 mailbox              */
#define HSEM_ID_M4_TO_M7_MBOX     3U   /* guards m4_to_m7 mailbox              */
#define HSEM_ID_ADC_DATA          4U   /* guards adc telemetry and waveform    */
#define BOARD_GPIO_HSEM_ID        5U   /* reserved for 00_gpio shared pin lock */
#define BOARD_HSEM_TEST_ID_FIRST 24U   /* IDs 24..31 reserved for selftests    */
```

And the security clear key is defined in `board_def.h`:
```c
#define BOARD_HSEM_CLEAR_KEY      0xA5C3U
```

### 3.2 SRAM4 Placement & Cache Coherency
Hardware semaphores coordinate CPU timing and bus arbitration, but **they do not
perform data cache maintenance**.
- Shared memory structures (`g_shared`) must be placed in **SRAM4 (`0x38000000`, D3 domain)**.
- SRAM4 is configured as **Normal, Non-Cacheable** in `mpu_config.c` on Cortex-M7.
- Cortex-M4 has no data cache.
- Consequently, neither core requires cache clean or invalidate operations to observe
  updates made by the other core under semaphore protection.

---

## 4. Rules the Driver Enforces

1. **Validation Before Hardware Access (Rule C6):**
   `id > 31`, `proc_id > 255`, `proc_id == 0` in 2-step `Take`, `max_cycles == 0`,
   `key == 0`, and empty masks are rejected immediately before touching `HSEM->`.
2. **Honest Release (Rule C11 / Finding S1):**
   RM0399 §11.3.5 states that releasing a semaphore with an incorrect `COREID` or
   `PROCID` is **ignored silently by hardware**. The driver explicitly reads `Rx`
   prior to writing, verifying that the caller is indeed the owner. If not, it
   writes nothing and returns `HSEM_BM_ERR_NOT_OWNER`.
3. **Memory Barriers (Rule C10 / Finding S2):**
   Under ARMv7-M, memory accesses between Normal (SRAM4) and Device (HSEM peripheral)
   memory are not ordered without an explicit barrier. A `__DMB()` is executed
   immediately after acquiring a lock, and immediately prior to releasing it.
4. **Strictly 32-bit Word Accesses (Rule C10):**
   RM0399 §11.4 mandates that all HSEM registers must be accessed as 32-bit words.
   Byte or half-word accesses trigger a bus error. Verified by assembly inspection:
   only `ldr.w` and `str.w` are emitted.
5. **Single-Core Re-entrancy Protection (Finding S1):**
   The 1-step lock (`RLR`) automatically assigns `PROCID = 0` and grants ownership to
   any context on that core. To prevent a second task or ISR on the same core from
   inadvertently entering the critical section, `FastTake` reads `Rx` under PRIMASK.
   If already held by the calling core, it returns `HSEM_BM_BUSY_SELF` (or `HAL_BUSY`).
6. **Local Critical Sections (Rule C12 / Finding S7):**
   Bitwise RMW operations on `CnIER` and `FastTake` pre-checks save and restore
   `PRIMASK` via `__get_PRIMASK()` and `__set_PRIMASK()`. Interrupts are never
   blindly re-enabled with `__enable_irq()`, ensuring safety when called from fault
   handlers or nested ISRs.
7. **Bounded Cycle Timeouts (Rule C13 / Finding S3 & S4):**
   All timeout loops are bounded by DWT `CYCCNT` cycles, guaranteeing calibrated time
   duration on each core. Callers can provide a `relax` callback (e.g. `taskYIELD` or `__WFE`).

---

## 5. Silicon Facts & RM0399 / ARMv7-M Matrix

| Silicon Fact / RM0399 Rule | Implementation Mechanism | Validation |
| :--- | :--- | :--- |
| **RM §11.3.8 Table 95:** COREID 3 = CM7, COREID 1 = CM4 | `Init` checks `SCB->CPUID` against expected Cortex-M core. | H0 |
| **RM §11.3.3:** 1-step lock forces PROCID = 0 | `FastTake` performs pre-check under PRIMASK for `BUSY_SELF`. | H7 |
| **RM §11.3.3:** 2-step lock checks COREID + PROCID | `Take` writes `Rx` and verifies readback. | H1, H3 |
| **RM §11.3.5:** Unauthorized release ignored silently | `Release` checks owner before writing (`ERR_NOT_OWNER`). | H3 |
| **RM §11.3.6:** Reset all semaphores of a core | `ReleaseAllOfCore` writes `KEYR` and `HSEM_CR`. | H8 |
| **RM §11.3.7:** Notification procedure | `TakeOrNotify`, `NotifyEnable` clears flag before arming. | H9 |
| **RM §11.4:** 32-bit accesses only | `uint32_t volatile` pointers only; no byte/half-word instructions. | objdump |
| **RM §11.4.2:** Reading `RLR` locks semaphore | Only `FastTake` and `TakeOrNotify` read `RLR`; diagnostics read `R`. | H0, grep |
| **ARMv7-M:** Normal vs Device memory ordering | `__DMB()` following lock acquisition and before release. | H1b, objdump |
| **ARMv7-M:** WFE wake-up requires event | `BootWaitForCpu1` arms `SEVONPEND` to convert pending IRQ to event. | H5 |

---

## 6. Silicon Errata (ES0445 Rev 6)

STM32H755ZI Silicon Errata sheet **ES0445** does **not** list any errata for the
Hardware Semaphore (HSEM) peripheral across Silicon Revisions Y, X, and V.

All synchronization constraints are dictated directly by RM0399 hardware architecture
and the ARMv7-M memory consistency model.

---

## 7. Dual-Core Boot Handshake

```mermaid
sequenceDiagram
    participant C7 as CPU1 (Cortex-M7)
    participant H as HSEM Peripheral
    participant C4 as CPU2 (Cortex-M4)

    Note over C4: Boot from reset (BCM4=1)
    C4->>H: Enable HSEMEN, C2ICR, C2IER[0]
    C4->>C4: Set SEVONPEND, PDDS_D2=0, SLEEPDEEP, WFE
    Note over C4: CPU2 parks in STOP (D2CKRDY drops to 0)

    Note over C7: Boot from reset (BCM7=1)
    C7->>C7: Invalidate SHARED_MAGIC (magic=0)
    C7->>H: Enable HSEMEN, TimebaseStart(), SetClearKey(BOARD_HSEM_CLEAR_KEY)
    C7->>C7: BootWaitCpu2Parked (monitors D2CKRDY == 0)
    C7->>C7: Configure MPU, Caches, Clocks (SystemClock_Config)
    C7->>C7: Shared_Init (clears SRAM4, writes SHARED_MAGIC last + DMB)
    C7->>H: BootReleaseCpu2: FastTake(0) + DMB + Release(0)
    H-->>C4: C2ISR[0]=1 -> Event triggered via SEVONPEND

    C4->>C4: Wakes from WFE -> verifies flag & magic==SHARED_MAGIC
    Note over C4: If spurious wakeup -> increments wake_count and returns to WFE
    C4->>H: Clear C2ICR[0], Disable C2IER[0]
    C4->>C4: Restore SCB->SCR, ClearPendingIRQ(HSEM2_IRQn), Init HAL & Peripherals
```

### 7.1 Double Predicate Verification
On soft resets (`-rst`), SRAM4 retains stale memory contents. If CPU2 wakes
prematurely from an electrical glitch or warm reset, evaluating `magic` alone could
cause CPU2 to start executing before CPU1 has configured the clock tree.
`HSEM_BM_BootWaitForCpu1` enforces a double predicate:
1. `C2ISR[id]` flag must be set by CPU1 releasing HSEM 0.
2. The user-supplied `ready()` callback must return `true` (verifying `magic == SHARED_MAGIC`).

---

## 8. Usage Examples

### 8.1 1-Step Fast Lock (Single Task / Interrupt-free)
```c
#include "hsem_bm.h"

void LogTelemetry(void)
{
    if (HSEM_BM_FastTake(HSEM_ID_ADC_DATA) == HSEM_BM_OK)
    {
        /* Memory barrier __DMB() already executed */
        g_shared.adc.millivolts = ReadMv();

        /* Honest release verifies ownership before clearing */
        HSEM_BM_Release(HSEM_ID_ADC_DATA, 0U);
    }
}
```

### 8.2 2-Step Lock with Process ID (FreeRTOS Tasks)
```c
#include "hsem_bm.h"

#define TASK_A_PROC_ID   0x01U

void TaskA_Function(void *arg)
{
    for (;;)
    {
        if (HSEM_BM_Take(HSEM_ID_SHARED_STATE, TASK_A_PROC_ID) == HSEM_BM_OK)
        {
            UpdateSharedState();
            HSEM_BM_Release(HSEM_ID_SHARED_STATE, TASK_A_PROC_ID);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

### 8.3 Bounded Wait with Cooperative OS Yield
```c
#include "hsem_bm.h"
#include "FreeRTOS.h"
#include "task.h"

bool ReadBufferWithTimeout(void)
{
    /* Wait up to 10,000 cycles (~25 µs @ 400 MHz), yielding to FreeRTOS scheduler */
    HsemBm_Status_t st = HSEM_BM_TakeTimeoutEx(
        HSEM_ID_ADC_DATA, 0U, 10000UL,
        (void (*)(void *))taskYIELD, NULL);

    if (st == HSEM_BM_OK)
    {
        memcpy(local_buf, (void *)g_shared.adc.waveform, 1024);
        HSEM_BM_Release(HSEM_ID_ADC_DATA, 0U);
        return true;
    }
    return false;
}
```

---

## 9. Validation

The driver is validated through the hardware self-test suite running on physical hardware:

| Test ID | Objective | Negative Control | Verification Result |
| :---: | :--- | :--- | :--- |
| **H0** | Silicon characterization, COREID validation, invalid parameter rejection | Raw direct read of `RLR` locks semaphore; parameter errors reject upfront | COREID: CM7=3, CM4=1. Direct RLR read confirms lock. |
| **H1** | Exact mutual exclusion under lock: 1-step `FastTake` (500k iterations per core) | Unprotected increments lose counts (H2) | Total = 1,000,000, Lost = 0 |
| **H1 (2-step)** | Exact mutual exclusion under lock: 2-step `Take` (500k iterations per core) | Unprotected increments lose counts (H2) | Total = 1,000,000, Lost = 0 |
| **H1b** | Torn-read protection across memory domains (8-word records under lock) | Unprotected reads yield torn records (H2) | 500,000 reads, 0 torn reads |
| **H2** | Negative control for H1 & H1b (deliberate contention without lock) | Required to fail if hazard is not reproduced | Data loss reproduced (> 0 lost), torn reads reproduced (> 0 torn) |
| **H3** | Honest release protection: wrong PROCID, cross-core unauthorized release | Authorized release succeeds and clears lock | Unauthorized release returns `ERR_NOT_OWNER`, lock remains held |
| **H4** | Bounded wait timeout: 10 ms wait against held lock (CM7 waiting on CM4) | Immediate return when free | Returned `TIMEOUT` at 10.0 ms; `relax` calls invoked |
| **H4 (sym)** | Symmetric bounded wait timeout: 10 ms wait against held lock (CM4 waiting on CM7) | Immediate return when free | Returned `TIMEOUT` at 10.0 ms; `relax` calls invoked |
| **H5** | Boot handshake: 500 ms wait, release before magic (H5b), spurious EXTI line 15 (H5c) | Evaluates `cpu2_wake_count` in SRAM4 | `D2CKRDY` stays 0 for 500 ms; H5b and H5c wake and re-park safely |
| **H6** | 20 consecutive warm resets across dual cores | Divergence S5 observed before migration | 20/20 resets successful; stable heartbeat ratio |
| **H7** | Same-core collision: 1-step re-entrancy and cross-PROCID releases | Raw `HAL_HSEM_FastTake` returns `HAL_OK` (hazard confirmed) | Driver returns `BUSY_SELF` (or `HAL_BUSY`); cross-release returns `ERR_NOT_OWNER` |
| **H8** | Bulk recovery: `ReleaseAllOfCore` with security key | Invalid key leaves locks intact | Target core semaphores cleared; other core semaphores untouched |
| **H9** | Asynchronous notifications: NVIC interrupt-driven (`HSEM2_IRQn` on CM4, `HSEM1_IRQn` on CM7) | Neg 1: IER disabled yields 0 IRQs; Neg 2: stale flag cleared on enable yields 0 spurious IRQs | 500/500 notifications received on CM4, 100/100 received on CM7 |
| **H10** | Tier parity: bit-for-bit register snapshots across BM, LL, and HAL (13 steps) | Mutated snapshots detect mismatches | Exact match across registers and return statuses |
| **H11** | Full regression under load (Ethernet UDP burst + 500 kSPS ADC streaming) | Inspected per-core fail counters | ADC rate = 500 kSPS, `err/ovr/adc_ovr = 0`, lock fail counts = 0 |

### 9.1 Hardware Benchmark B1
Direct measurements using DWT `CYCCNT`:

- **Raw Peripheral Access Latency (AHB4 via D3 domain):**
  - Cortex-M7 (CPU1 @ 400 MHz): `FastTake` = 16 cycles (40.0 ns), `Take` = 30 cycles (75.0 ns), `Release` = 3 cycles (7.5 ns).
  - Cortex-M4 (CPU2 @ 200 MHz): `FastTake` = 8 cycles (40.0 ns), `Take` = 24 cycles (120.0 ns), `Release` = 6 cycles (30.0 ns).
- **Application Retention Times:**
  - Measured empirically in `adc_acquisition.c` and `adc_server.c` using DWT cycle counters (`g_adc_lock_hold_cycles_*` and `g_m7_adc_lock_hold_cycles_*`).

---

## 10. Known Limitations, Pitfalls & Caveats

1. **SWD Debugger Access (RLR Bank Hazard):**
   Reading memory addresses `0x58026480..0x580264FC` (`HSEM_RLRx`) through an SWD
   debugger or GDB takes the semaphores as bus master 0 (CPU1). Memory dump tools
   must strictly restrict inspection to `0x58026400..0x5802647C` (`HSEM_Rx`), which
   is non-destructive.
2. **SRAM4 Warm Reset Persistence:**
   On soft reset, SRAM4 (`0x38008000`) is not cleared by hardware. CPU1 must
   explicitly zero `g_shared.magic` at the very beginning of `main()` and clear stale
   locks via `HSEM_BM_ReleaseAllOfCore()`.
3. **No HSEM Locks in ISR Context:**
   Never attempt to lock an HSEM inside an Interrupt Service Routine. If the ISR
   preempts the thread holding that same HSEM, the core deadlocks permanently.
   `adc_acquisition.c` and `07_adc` follow this rule strictly.
4. **Timebase Prerequisite:**
   Calling `TakeTimeout` requires a running DWT `CYCCNT`. Both cores must call
   `HSEM_BM_TimebaseStart()` and `HSEM_BM_Init()` before invoking timeout-bounded
   operations.

---

## 11. Scope

### In Scope
- Core locking primitives (1-step and 2-step) across BM, LL, and HAL tiers.
- Honest release verification and single-core self-collision detection.
- Bounded cycle timeouts with `relax` hooks.
- Banked interrupt and event notifications via NVIC.
- Dual-core boot synchronization utilities.
- Target hardware validation on STM32H755ZI.

### Out of Scope
- Peripheral clock configuration (managed by RCC / `SystemClock_Config`).
- Core boot release register `RCC_GCR.BOOT_C2` (managed by `HAL_RCCEx_EnableBootCore`).
- RTOS mutex integration or queuing (the application chooses whether to yield).
- Project semaphore ID definitions (managed by `shared_memory.h` / `board_def.h`).

---

## 12. Audit Status

Formal audit findings and verification records are maintained independently in `AUDIT.md`.
All corrective actions for findings B1–B6, M1–M7, and m1–m8 have been implemented and
verified on the physical target.
