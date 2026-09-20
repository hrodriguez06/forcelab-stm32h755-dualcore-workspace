# Timers & PWM - STM32H755

## 1. Timer Categorization

| Category | Instances | Max Counter Clock | Features |
| :--- | :--- | :--- | :--- |
| **Advanced Control** | **TIM1, TIM8** | Up to 240 MHz | 16-bit, complementary PWM, dead-time insertion, emergency break inputs, motor control |
| **General Purpose (32-bit)** | **TIM2, TIM5** | Up to 240 MHz | 32-bit auto-reload, 4 independent channels, quadrature encoder interface |
| **General Purpose (16-bit)** | **TIM3, TIM4, TIM12, TIM13, TIM14, TIM15, TIM16, TIM17** | Up to 240 MHz | 16-bit counters, input capture, output compare, PWM generation |
| **Basic Timers** | **TIM6, TIM7** | Up to 240 MHz | 16-bit, DAC trigger generation, time base |
| **High Resolution Timer** | **HRTIM1** | ~400+ MHz (picosecond resolution) | Multi-phase SMPS, resonant converters, digital power |
| **Low Power Timers** | **LPTIM1, LPTIM2, LPTIM3, LPTIM4, LPTIM5** | Up to 120 MHz | Can run in Stop mode from LSE/LSI |

---

## 2. Timer Clock Frequency Calculation
Timer clock frequencies on STM32H7 depend on the APB prescalers (`TIMPRE` bit in `RCC_CFGR`):
- When the APB prescaler is 1: Timer clock = APB clock.
- When the APB prescaler is > 1: Timer clock = 2 × APB clock (up to domain max).

---

## 3. Standard PWM Output Example
```c
/* Start PWM on Channel 1 */
HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);

/* Dynamically adjust duty cycle */
__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, new_pulse_value);
```

## 4. Complementary Outputs & Dead-time (TIM1 / TIM8)
For half-bridge / full-bridge gate driving:
```c
HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_1); /* Start complementary output (CH1N) */
```
