/**
  ******************************************************************************
  * @file    adc_acquisition.h
  * @brief   ADC1 + DMA1 continuous acquisition management on Cortex-M4.
  *
  *          Coordinates TIM3 500 kHz TRGO hardware trigger, ADC1 16-bit
  *          sampling on PB1 (Arduino A3, Fast Channel 5, 1.5 V DC bias),
  *          circular DMA into SRAM2 (1024 samples, 2 x 512 half-buffers),
  *          and 32-bit integer statistics & waveform publication to shared
  *          memory (SRAM4) under HSEM 4.
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
  * @brief  Initializes analog GPIO (PB1), TIM3 (500 kHz TRGO), and ADC1+DMA1 drivers.
  * @return true if initialized successfully, false otherwise.
  */
bool ADC_Acquisition_Init(void);

/**
  * @brief  Periodic process function called from CM4 super-loop.
  *         If a 512-sample half-buffer is ready, computes 32-bit integer
  *         statistics (including 1.5V DC offset metrics) and publishes
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
