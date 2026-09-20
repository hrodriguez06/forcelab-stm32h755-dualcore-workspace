# Interrupts (NVIC & EXTI) - STM32H755

## 1. Documentation
- `20_nested-vectored-interrupt-controllers-nvic1-and-nvic2.pdf`
- `21_extended-interrupt-and-event-controller-exti.pdf`
- Core-level detail: [`PM0253`](../../00_core_and_system/PM0253_cortex_m7_programming_manual.pdf) (M7),
  [`PM0214`](../../00_core_and_system/PM0214_cortex_m4_programming_manual.pdf) (M4).

---

## 2. Dual-Core NVIC

Each core has its **own independent NVIC**. There is no shared interrupt controller.

| | NVIC1 (CPU1 / M7) | NVIC2 (CPU2 / M4) |
| :--- | :--- | :--- |
| Interrupt lines | up to 240 | up to 240 |
| Priority bits | 4 (16 levels) | 4 (16 levels) |
| Vector table | `.isr_vector` in Flash bank 1 | `.isr_vector` in Flash bank 2 |

**A peripheral interrupt must be enabled on the NVIC of the core that will service it.**
Enabling `USART3_IRQn` on both cores means both ISRs fire and race over the same
registers. Decide ownership per peripheral and document it.

> The interrupt *source* is shared hardware; only the *routing* is per-core. For a
> peripheral genuinely used by both cores, guard it with an HSEM. See
> [`../hsem_dualcore/README.md`](../hsem_dualcore/README.md).

---

## 3. Priority Grouping (MANDATORY)

```c
HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);   /* 4 bits preempt, 0 sub */
```

All 4 bits must be preemption priority. `HAL_Init()` already does this; **do not
override it**. Sub-priorities break the FreeRTOS priority check, which only inspects
the preemption field.

**Lower number = higher priority.** Priority 0 is the most urgent.

---

## 4. FreeRTOS Priority Rules (CRITICAL)

With `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`:

| Priority | Zone | May call `...FromISR()` | Typical use |
| :---: | :--- | :---: | :--- |
| 0 - 4 | above the kernel | **NO** | hard real-time: motor commutation, HRTIM fault, safety shutdown |
| 5 - 15 | kernel-managed | **YES** | everything else: UART, SPI, I2C, DMA, ETH, timers |

An ISR in the 0-4 zone that calls any `...FromISR()` function corrupts the kernel.
The failure is intermittent and hard to trace, so treat this as a hard rule.

```c
/* Kernel-managed: may use FreeRTOS API */
HAL_NVIC_SetPriority(USART3_IRQn, 6, 0);
HAL_NVIC_EnableIRQ(USART3_IRQn);

/* Above the kernel: MUST NOT touch FreeRTOS at all */
HAL_NVIC_SetPriority(HRTIM1_FLT_IRQn, 1, 0);
HAL_NVIC_EnableIRQ(HRTIM1_FLT_IRQn);
```

Also required when FreeRTOS is active: use **TIM6** or **TIM7** as the HAL timebase,
never SysTick. See [`../../01_drivers_and_hal/FreeRTOS/README.md`](../../01_drivers_and_hal/FreeRTOS/README.md).

### Suggested allocation for this project

| Priority | Assigned to |
| :---: | :--- |
| 0 | Reserved (NMI-like emergencies only) |
| 1 - 2 | Safety: HRTIM/TIM break inputs, RAMECC, watchdog early warning |
| 3 - 4 | Hard real-time control loop |
| 5 | HAL timebase (TIM6/TIM7) |
| 6 - 8 | High-rate data: DMA completion, SPI, SDMMC, ETH |
| 9 - 11 | UART, I2C, FDCAN, USB |
| 12 - 15 | Background: EXTI buttons, RTC, low-priority housekeeping |

---

## 5. EXTI on a Dual-Core Device

The EXTI controller is shared, but the **masks are per-core**:

| Register | Purpose |
| :--- | :--- |
| `EXTI_C1IMR1/2/3` | interrupt mask for **CPU1** |
| `EXTI_C2IMR1/2/3` | interrupt mask for **CPU2** |
| `EXTI_C1EMR1/2/3` | event mask for CPU1 (wakeup without ISR) |
| `EXTI_C2EMR1/2/3` | event mask for CPU2 |
| `EXTI_D3PMR1/2/3` | D3 pending-clear mask (autonomous D3 operation) |

The same EXTI line can be routed to CPU1 only, CPU2 only, or **both** independently.
Two cores can therefore react to the same pin edge without any software signalling.

- Lines **0-15**: GPIO, port selected by `SYSCFG_EXTICR1..4`.
- Lines **16+**: internal wakeup sources (PVD, RTC alarm, USB wakeup, ETH wakeup,
  COMP, LPTIM, and so on). Some are *configurable* (edge-selectable), some are
  *direct* (they simply reflect the peripheral flag).
- **Direct lines cannot be software-triggered** and their pending bit is cleared by
  clearing the source peripheral's flag, not by writing `EXTI_PR`.

### D3 autonomous wakeup

`EXTI_D3PMRx` / `EXTI_D3PCRxL` / `EXTI_D3PCRxH` let a D3 peripheral (LPUART1, I2C4,
SPI6, LPTIM2-5, BDMA) run while D1 and D2 are in DStandby. Only relevant in low-power
designs; for an always-on application leave these at reset value.

---

## 6. GPIO Interrupt Example (Nucleo USER button)

The USER button **B1** on the Nucleo-H755ZI-Q is on **`PC13`** (also `WKUP2`),
active high, with an external pull-down.

```c
/* --- CPU1 (M7): EXTI13 on PC13, kernel-managed priority --- */
__HAL_RCC_GPIOC_CLK_ENABLE();

GPIO_InitTypeDef g = {0};
g.Pin  = GPIO_PIN_13;
g.Mode = GPIO_MODE_IT_RISING;     /* button is active high on this board */
g.Pull = GPIO_NOPULL;             /* external pull-down is fitted        */
HAL_GPIO_Init(GPIOC, &g);

HAL_NVIC_SetPriority(EXTI15_10_IRQn, 13, 0);
HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
```

```c
void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_13);   /* clears the pending bit */
}

void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if (pin == GPIO_PIN_13) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(button_sem, &woken);   /* legal: priority 13 >= 5 */
        portYIELD_FROM_ISR(woken);
    }
}
```

> **ERRATA ES0445 2.3.13**: transitions on `PC13` can disturb the LSE crystal
> oscillator. If the RTC runs from LSE, debounce the button in software rather than
> toggling `PC13` fast, and do not use `PC13` as a general-purpose output.
> See [`../../00_core_and_system/errata_summary.md`](../../00_core_and_system/errata_summary.md).

### Nucleo-H755ZI-Q quick reference

| Function | Pin | Notes |
| :--- | :--- | :--- |
| USER button B1 | `PC13` | active high, EXTI13, also `WKUP2` |
| LD1 green | `PB0` | |
| LD2 yellow | `PE1` | |
| LD3 red | `PB14` | also `OTG_HS_DM` (AF12) - unused, the board's USB is FS on PA11/PA12 |

---

## 7. Common Pitfalls

1. **Clear the pending bit before enabling.** A pin configured as EXTI often latches
   an edge during `HAL_GPIO_Init()`.
   ```c
   __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_13);
   HAL_NVIC_ClearPendingIRQ(EXTI15_10_IRQn);
   HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
   ```
2. **Late write-buffer flush on the M7.** Clearing a peripheral flag at the very end of
   an ISR may not reach the peripheral before the handler returns, causing the ISR to
   re-enter. Terminate such handlers with `__DSB()`.
3. **Shared EXTI vectors.** `EXTI9_5_IRQn` and `EXTI15_10_IRQn` cover several lines;
   always test which pin fired.
4. **`__disable_irq()` needs barriers.** Follow it with `__DSB(); __ISB();` before the
   critical section on the M7.
5. **ERRATA ES0445 2.1.6** - the Cortex-M7 FPU interrupt is **not** present on NVIC
   line 81. Do not build FPU exception handling on CPU1 around an IRQ.
6. **ERRATA ES0445 2.2.2** (CPU2) - very short ISRs can corrupt `VDIV`/`VSQRT` results
   when the FPU is enabled. Either clear `LSPEN` in `FPCCR` or keep every M4 ISR
   longer than 2 instructions plus the exception return.
7. **ERRATA ES0445 2.18.3** - the RTC alarm flag may be set repeatedly while the core
   is halted at a breakpoint. Expected behavior, not a bug in your handler.

---

## 8. Cross-References
- Pin multiplexing: [`../../../STM32H755_Alternate_Functions.md`](../../../STM32H755_Alternate_Functions.md), [`../../../STM32H755_Pin_Definitions.md`](../../../STM32H755_Pin_Definitions.md)
- Dual-core coordination: [`../hsem_dualcore/README.md`](../hsem_dualcore/README.md), [`../../00_core_and_system/dual_core_boot_ipc.md`](../../00_core_and_system/dual_core_boot_ipc.md)
- Silicon errata: [`../../00_core_and_system/errata_summary.md`](../../00_core_and_system/errata_summary.md)
