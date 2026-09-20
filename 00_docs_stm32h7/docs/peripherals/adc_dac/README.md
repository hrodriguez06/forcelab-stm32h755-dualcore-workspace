# ADC & DAC - STM32H755

## 1. ADC Features Overview
- **Instances**: ADC1, ADC2 (D2 domain, fast dual-interleaved), ADC3 (D3 domain).
- **Resolution**: Up to **16-bit** SAR ADC (3.6 MSPS at 16-bit, up to 7.2 MSPS interleaved).
- **Hardware Oversampling**: Built-in 16x/32x/64x/128x/256x/512x/1024x hardware oversampler with up to 4 bits of resolution extension (up to 20 bits effective).

---

## 2. Mandatory Calibration Step
Before starting any conversions, the ADC must be calibrated (both offset and linearity):
```c
/* Calibrate single-ended inputs */
if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED) != HAL_OK) {
    Error_Handler();
}
```

---

## 3. Regular Conversions with DMA & Cortex-M7
```c
/* ADC1 / ADC2 are in D2 -> DMA1 / DMA2 -> buffer in D2 SRAM, never DTCM.
   .dma_buf is non-cacheable SRAM1; align to 32 and size a multiple of 32.
   For ADC3 (D3 domain) use BDMA and the .d3_ram section instead.
   Section names: ../../00_core_and_system/linker_templates/README.md            */
__attribute__((section(".dma_buf"), aligned(32)))
uint16_t adc_buffer[NUM_CHANNELS * NUM_SAMPLES];

/* Start multi-channel DMA scan */
HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buffer, NUM_CHANNELS * NUM_SAMPLES);

/* In callback: Invalidate D-Cache */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc)
{
    if (hadc->Instance == ADC1) {
        /* Only needed if the buffer is in a CACHEABLE region (e.g. .axi_ram).
           With .dma_buf (non-cacheable) both lines can be dropped.
           The Cortex-M4 has no cache, so CPU2 never needs this.                 */
        SCB_InvalidateDCache_by_Addr((uint32_t*)adc_buffer, sizeof(adc_buffer));
        __DMB();                                  /* ERRATA ES0445 2.1.5 */
        // Safe to read adc_buffer
    }
}
```

---

## 4. DAC (Digital to Analog Converter)
- **Instances**: DAC1 (dual 12-bit channels in D3 domain).
- **Output Buffer**: Can be enabled for direct load driving or disabled for reduced power and rail-to-rail range.
- **Triggers**: Can be triggered by TIM6, TIM7, TIM2, TIM4, HRTIM, or external pins.
