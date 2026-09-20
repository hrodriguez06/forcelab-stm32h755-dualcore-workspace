# I2C (Inter-Integrated Circuit) - STM32H755

## 1. Instances & Domains
- **I2C1, I2C2, I2C3**: Located in the **D2 domain** (APB1).
- **I2C4**: Located in the **D3 domain** (APB4). Can operate in low-power modes (Stop) and wake up the MCU on address match.

---

## 2. Bus Modes & Timings
- **Standard Mode (Sm)**: Up to 100 kHz.
- **Fast Mode (Fm)**: Up to 400 kHz.
- **Fast Mode Plus (Fm+)**: Up to 1 MHz (requires enabling Fm+ drive on selected pins via `SYSCFG`).

### Timing Register Calculation (`I2C_TIMINGR`)
In STM32H7, `I2C_TIMINGR` configuration depends strictly on the I2C kernel clock source (`I2C123CLK` / `I2C4CLK`).
Use STM32CubeMX or the ST timing spreadsheet to calculate prescalers (`PRESC`, `SCLL`, `SCLH`, `SDADEL`, `SCLDEL`).

---

## 3. GPIO Pin Configuration
- Pins must be configured in **Open-Drain mode with Pull-up**:
  ```c
  GPIO_InitStruct.Pin       = GPIO_PIN_8 | GPIO_PIN_9; /* Example: GPIOB I2C1 */
  GPIO_InitStruct.Mode      = GPIO_MODE_AF_OD;
  GPIO_InitStruct.Pull      = GPIO_PULLUP;
  GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
  ```
- *External pull-up resistors (2.2kΩ to 4.7kΩ) are recommended for reliable operation at 400 kHz / 1 MHz.*

---

## 4. Key HAL Functions
- Memory Read/Write (EEPROMs / Sensors):
  ```c
  HAL_I2C_Mem_Read(&hi2c1, DevAddress, MemAddress, I2C_MEMADD_SIZE_8BIT, pData, Size, Timeout);
  HAL_I2C_Mem_Write(&hi2c1, DevAddress, MemAddress, I2C_MEMADD_SIZE_8BIT, pData, Size, Timeout);
  ```
- DMA Transfers:
  ```c
  HAL_I2C_Master_Receive_DMA(&hi2c1, DevAddress, pRxData, Size);
  ```
