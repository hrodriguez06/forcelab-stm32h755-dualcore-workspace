# FreeRTOS on STM32H755 (Dual-Core Cortex-M7 / Cortex-M4)

## 1. Directory Structure
- **Source Code**: [`FreeRTOS-Kernel/`](FreeRTOS-Kernel/) (Official FreeRTOS Kernel repository).
  - Core files: `tasks.c`, `queue.c`, `timers.c`, `event_groups.c`, `stream_buffer.c`.
  - Port files: `portable/GCC/ARM_CM7/r0p1/` (for Cortex-M7) and `portable/GCC/ARM_CM4F/` (for Cortex-M4).
  - Memory managers: `portable/MemMang/` (`heap_4.c` recommended).

---

## 2. Mandatory Rules for FreeRTOS on STM32 (NVIC & SysTick)

### A. NVIC Priority Grouping (CRITICAL)
In Cortex-M microcontrollers running FreeRTOS, all interrupt priority bits must be allocated to preemption priority (no subpriority bits):
```c
HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
```

### B. SysCall Interrupt Priority Limits
Any interrupt Service Routine (ISR) that calls FreeRTOS APIs (functions ending in `...FromISR`) **must** have a priority numerically equal to or greater than `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (meaning lower hardware urgency):
- STM32 uses 4 priority bits (`0` to `15`, where `0` is highest priority).
- If `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` is `5`:
  - ISRs with priority `0` - `4`: **CANNOT call FreeRTOS APIs** (they have zero jitter and never get masked by the kernel).
  - ISRs with priority `5` - `15`: **CAN call FreeRTOS APIs** safely.

### C. HAL Timebase Timer (SysTick Conflict Resolution)
By default, the ST HAL driver uses **SysTick** for `HAL_Delay()` and `HAL_GetTick()`. FreeRTOS also requires SysTick for its context switcher.
- **ST Recommendation**: Change the HAL timebase source in STM32CubeMX (SYS -> Timebase Source) from SysTick to a basic hardware timer, e.g. **TIM6** or **TIM7**.
- This leaves SysTick exclusively to FreeRTOS and prevents priority inversion deadlocks inside HAL drivers.

---

## 3. D-Cache Coherency with FreeRTOS on Cortex-M7
- When passing buffers between FreeRTOS tasks and DMA controllers (Ethernet, SPI, UART), ensure the buffers are aligned to 32 bytes (`__attribute__((aligned(32)))`).
- Either call `SCB_CleanDCache_by_Addr` / `SCB_InvalidateDCache_by_Addr` around DMA triggers, or place shared task DMA buffers in an MPU Non-Cacheable region.

---

## 4. Dual-Core Independent Kernels
On the STM32H755:
- **CPU1 (M7)** and **CPU2 (M4)** can each run their own independent instance of FreeRTOS.
- Inter-processor task communication is handled via **OpenAMP** (RPMsg) or Hardware Semaphores (**HSEM**) with shared memory in D2 SRAM3 (`0x30040000`) or SRAM4 (`0x38000000`).
