# SPI (Serial Peripheral Interface) - STM32H755

## 1. Instance Distribution
- **SPI1, SPI2, SPI3**: High-performance instances supporting up to 150 MHz kernel clock, I2S audio mode, and 16-word FIFO.
- **SPI4, SPI5**: APB2 domain instances.
- **SPI6**: Located in the **D3 domain**, accessible by BDMA.

---

## 2. STM32H7 SPI Specifics vs Legacy STM32
The SPI peripheral in STM32H7 has an updated hardware architecture:
1. **Packet / Data Size**: Supports data frames from 4 to 32 bits dynamically.
2. **Hardware FIFO**: 16 data frames deep.
3. **Master Transfer Control**: Requires setting `TSIZE` (number of data frames) before initiating transmission.
4. **End of Transfer (EOT)**: Generates an EOT flag when the specified number of bytes has been transferred.

---

## 3. DMA Transfer with Cortex-M7
- **Transmit**:
  ```c
  SCB_CleanDCache_by_Addr((uint32_t*)spi_tx_buf, ((size + 31) / 32) * 32);
  HAL_SPI_Transmit_DMA(&hspi1, spi_tx_buf, size);
  ```
- **Receive / TransmitReceive**:
  ```c
  SCB_CleanDCache_by_Addr((uint32_t*)spi_tx_buf, ((size + 31) / 32) * 32);
  HAL_SPI_TransmitReceive_DMA(&hspi1, spi_tx_buf, spi_rx_buf, size);

  void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
  {
      if (hspi->Instance == SPI1) {
          SCB_InvalidateDCache_by_Addr((uint32_t*)spi_rx_buf, ((size + 31) / 32) * 32);
          // Parse spi_rx_buf safely
      }
  }
  ```

---

## 4. GPIO Speed Note
When configuring SPI SCK, MOSI, MISO pins, always set `GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH` to prevent clock distortion at frequencies above 25 MHz.
