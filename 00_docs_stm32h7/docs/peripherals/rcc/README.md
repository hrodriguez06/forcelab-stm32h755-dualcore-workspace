# RCC (Reset and Clock Control) - STM32H755

## 1. Clock Domains & Bus Architecture
The STM32H755 clock tree distributes clocks across three main domains:
- **D1 Domain**: `sys_d1cpre_ck` (CPU1 / Cortex-M7 up to 480 MHz), `hclk` (AXI, AHB3 up to 240 MHz).
- **D2 Domain**: `sys_d2cpre_ck` (CPU2 / Cortex-M4 up to 240 MHz), `hclk` (AHB1, AHB2 up to 240 MHz), `pclk1`/`pclk2` (APB1, APB2 up to 120 MHz).
- **D3 Domain**: `hclk` (AHB4 up to 240 MHz), `pclk4` (APB4 up to 120 MHz).

---

## 2. Main PLL Configurations
- **PLL1**: Dedicated to CPU1 (DIVP), system interconnects, and optional peripheral kernel clocks.
- **PLL2**: Typically generates clocks for ADCs, SPIs, FDCAN, or SAI.
- **PLL3**: Typically generates clocks for USB, Ethernet PTP, or LTDC/Audio.

---

## 3. Peripheral Kernel Clocks
In the STM32H7, peripherals have independent kernel clocks selected via `RCC->D1CCIPR`, `RCC->D2CCIP1R`, `RCC->D2CCIP2R`, and `RCC->D3CCIPR`:
- **USART1/6**: Can select `pclk2`, `pll2_q_ck`, `pll3_q_ck`, `hsi_ker_ck`, `csi_ker_ck`, `lse_ck`.
- **USART2/3/4/5/7/8**: Can select `pclk1`, `pll2_q_ck`, `pll3_q_ck`, `hsi_ker_ck`, `csi_ker_ck`, `lse_ck`.
- **FDCAN**: Can select `hse_ck`, `pll1_q_ck`, `pll2_q_ck`.
- **SPI1/2/3**: Can select `pll1_q_ck`, `pll2_p_ck`, `pll3_p_ck`, `i2s_ckin`, `per_ck`.

---

## 4. Key HAL Functions
- `HAL_RCC_OscConfig(&RCC_OscInitStruct)`: Configures HSE/HSI/CSI/LSE/LSI and PLLs.
- `HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4)`: Configures domain prescalers and Flash latency.
- `HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct)`: Selects kernel clock source for specific peripherals.

---

## 5. Dual-Core Caveat
Only **CPU1 (Cortex-M7)** should initialize and reconfigure the main PLLs and system prescalers. CPU2 should wait until CPU1 confirms clock stability via HSEM before enabling peripheral clocks.
