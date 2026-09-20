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

### Boot Synchronization Code Example (CubeMX Pattern)
**CPU1 (Cortex-M7):**
```c
/* 1. Initialize RCC, Clocks, PLL */
SystemClock_Config();

/* 2. Release CPU2 by setting HSEM */
__HAL_RCC_HSEM_CLK_ENABLE();
HAL_HSEM_FastTake(HSEM_ID_0);
HAL_HSEM_Release(HSEM_ID_0, 0);

/* Wait for CPU2 to confirm boot (optional) */
```

**CPU2 (Cortex-M4):**
```c
/* 1. Wait until CPU1 finishes system clock configuration */
__HAL_RCC_HSEM_CLK_ENABLE();
while (!HAL_HSEM_IsSemTaken(HSEM_ID_0)) {
    /* Wait or WFE */
}
/* Wait for release */
while (HAL_HSEM_IsSemTaken(HSEM_ID_0)) {
}

/* 2. Clocks are ready, initialize CPU2 application */
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
1. Place the shared struct in a non-cacheable region of **D2 SRAM3 (`0x30040000`)** or **SRAM4 (`0x38000000`)**.
2. Protect write access with an HSEM semaphore ID:

```c
#define IPC_HSEM_ID 1

bool IPC_WriteData(const void* src, size_t len)
{
    if (HAL_HSEM_FastTake(IPC_HSEM_ID) == HAL_OK) {
        memcpy((void*)SHARED_BUFFER_ADDR, src, len);
        HAL_HSEM_Release(IPC_HSEM_ID, 0);
        return true;
    }
    return false;
}
```
