# HSEM (Hardware Semaphore) & Dual-Core IPC - STM32H755

## 1. Hardware Semaphore Overview
The STM32H755 provides **32 hardware semaphores (HSEM)** to coordinate mutual exclusion between CPU1 (Cortex-M7) and CPU2 (Cortex-M4).
- HSEM operations are atomic (1-cycle read/write lock).
- Located on the AHB bus matrix.
- Supports 2-step lock (ID check) and 1-step fast lock (`HAL_HSEM_FastTake()`).

---

## 2. Semaphore ID Allocation Convention

Establish a standard map across both core firmware projects:

| Semaphore ID | Assigned Purpose |
| :--- | :--- |
| **HSEM_ID_0** | System Boot & Clock Synchronization (CPU1 releases CPU2) |
| **HSEM_ID_1** | Shared RAM IPC Buffer Mutex (ring buffer or queue) |
| **HSEM_ID_2** | Shared RCC / Power domain registers |
| **HSEM_ID_3** | Shared GPIO / Pin manipulation mutex |
| **HSEM_ID_4** | Shared Hardware Peripherals (e.g. Flash programming mutex) |

---

## 3. Fast Lock & Release Implementation

### Taking a Semaphore (Non-Blocking)
```c
if (HAL_HSEM_FastTake(HSEM_ID_1) == HAL_OK) {
    /* Critical section: write or read shared buffer */
    
    /* Release semaphore */
    HAL_HSEM_Release(HSEM_ID_1, 0);
} else {
    /* Resource busy, retry later */
}
```

### Taking a Semaphore (Blocking with Timeout)
```c
uint32_t tickstart = HAL_GetTick();
while (HAL_HSEM_FastTake(HSEM_ID_1) != HAL_OK) {
    if ((HAL_GetTick() - tickstart) > 50) {
        // Timeout handling
        break;
    }
}
```

---

## 4. Interrupt Notifications
HSEM can generate an interrupt on Core 2 when Core 1 releases a semaphore (and vice-versa).
- `HAL_HSEM_ActivateNotification()` allows event-driven inter-core signaling without polling.
