/**
  ******************************************************************************
  * @file    adc_acquisition.h
  * @brief   ADC1 + DMA1 continuous acquisition management on Cortex-M4.
  *
  *          Coordinates TIM3 10 kHz TRGO hardware trigger, ADC1 16-bit
  *          sampling on PA3, circular DMA into SRAM2 (512 samples),
  *          and statistics calculation & publication to shared memory
  *          (SRAM4) under HSEM 4.
  ******************************************************************************
  */

#ifndef ADC_ACQUISITION_H
#define ADC_ACQUISITION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Initializes GPIO PA3, TIM3 (10 kHz TRGO), and ADC1+DMA1 driver.
  * @return true if initialized successfully, false otherwise.
  */
bool ADC_Acquisition_Init(void);

/**
  * @brief  Periodic process function called from CM4 super-loop.
  *         If a half-buffer is ready, computes statistics and publishes
  *         telemetry to shared memory under HSEM 4.
  */
void ADC_Acquisition_Process(void);

/**
  * @brief  DMA1 Stream 0 IRQ handler. Forwarded from DMA1_Stream0_IRQHandler.
  */
void ADC_Acquisition_DMA_IRQHandler(void);

/**
  * @brief  ADC global IRQ handler. Forwarded from ADC_IRQHandler.
  */
void ADC_Acquisition_ADC_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* ADC_ACQUISITION_H */
