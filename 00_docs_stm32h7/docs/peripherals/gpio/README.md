# GPIO (General Purpose Input/Output) - STM32H755

## 1. Overview
The STM32H755 provides up to 11 GPIO ports (**GPIOA** through **GPIOK**).
- All GPIO ports are clocked on the **AHB4 bus** (D3 domain).
- Clock enable macros: `__HAL_RCC_GPIOA_CLK_ENABLE()`, `__HAL_RCC_GPIOB_CLK_ENABLE()`, etc.

---

## 2. Pin Mode & Speed Options
- **Modes**:
  - `GPIO_MODE_INPUT`
  - `GPIO_MODE_OUTPUT_PP` (Push-Pull)
  - `GPIO_MODE_OUTPUT_OD` (Open-Drain, required for I2C)
  - `GPIO_MODE_AF_PP` / `GPIO_MODE_AF_OD` (Alternate Function)
  - `GPIO_MODE_ANALOG` (Default low-power state and ADC/DAC inputs)
  - `GPIO_MODE_IT_RISING` / `GPIO_MODE_IT_FALLING` / `GPIO_MODE_EVT` (External interrupts via EXTI)
- **Speeds**:
  - `GPIO_SPEED_FREQ_LOW` (~8 MHz)
  - `GPIO_SPEED_FREQ_MEDIUM` (~25 MHz)
  - `GPIO_SPEED_FREQ_HIGH` (~50 MHz)
  - `GPIO_SPEED_FREQ_VERY_HIGH` (~100 MHz+, essential for high-speed SPI, SDMMC, FMC, QuadSPI)

---

## 3. Alternate Function Reference
Always cross-reference the pin and AF number in:
- [STM32H755_Alternate_Functions.md](../../../STM32H755_Alternate_Functions.md)
- [STM32H755_Pin_Definitions.md](../../../STM32H755_Pin_Definitions.md)

### Standard AF Initialization Pattern (HAL)
```c
GPIO_InitTypeDef GPIO_InitStruct = {0};

__HAL_RCC_GPIOD_CLK_ENABLE();

/* Example: USART3 TX (PD8) & RX (PD9) -> AF7 */
GPIO_InitStruct.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
GPIO_InitStruct.Pull      = GPIO_PULLUP;
GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);
```

---

## 4. High-Speed I/O Compensation Cell
When operating I/Os at speeds exceeding 50 MHz with VDD > 2.7 V:
- Enable the I/O compensation cell in `SYSCFG` (`HAL_EnableCompensationCell()`) to maintain clean signal slew rates.
