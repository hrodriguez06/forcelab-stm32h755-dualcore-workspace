/**
  ******************************************************************************
  * @file    adc_acquisition.h
  * @brief   Cortex-M4 Bare-Metal ADC1 + DMA1_Stream0 + TIM3_TRGO acquisition
  *          pipeline for prj_06_adc_fft_eth (4096-sample ping-pong blocks).
  *
  * Built exclusively on `01_shared_drivers` bare-metal modules:
  *   - `gpio_bm` : Analog input configuration (PB1 / ADC1_INP5 or PA3 / INP15)
  *   - `tim_bm`  : TIM3 Update TRGO trigger generator (default 25.6 kHz,
  *                 runtime reconfigurable 1..100 kHz via shared memory command)
  *   - `dma_bm`  : DMA1_Stream0 circular ping-pong transfer (2 x 4096 samples)
  *   - `adc_bm`  : ADC1 16-bit SAR with automatic linearity + offset
  *                 calibration, BOOST management, and integer statistics
  *   - `hsem_bm` : Lock-protected publication into D3 SRAM4 (`g_shared.dsp`)
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
  * @brief  Initialize GPIO, TIM3, DMA1_Stream0, and ADC1 in Bare-Metal mode
  *         and start continuous 4096-sample ping-pong conversion at 25.6 kSPS.
  * @retval true on success, false if calibration or hardware init timed out.
  */
bool ADC_Acquisition_Init(void);

/**
  * @brief  Super-loop service routine on CM4:
  *           1. Services pending runtime commands (`START`/`STOP`/`FS <hz>`)
  *              posted by CM7 in `g_shared.dsp`.
  *           2. Checks if a DMA half-buffer (4096 samples) is ready, computes
  *              integer block statistics (`min`, `max`, `mean`, `mV`, `Vpp`),
  *              copies the 4096 samples to `g_shared.dsp.raw_buffer[half]` in
  *              D3 SRAM4, and publishes telemetry under `HSEM_ID_ADC_DATA`.
  */
void ADC_Acquisition_Process(void);

/**
  * @brief  Route `DMA1_Stream0_IRQHandler` to the bare-metal ADC/DMA driver.
  */
void ADC_Acquisition_DMA_IRQHandler(void);

/**
  * @brief  Route `ADC_IRQHandler` to the bare-metal ADC driver.
  */
void ADC_Acquisition_ADC_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* ADC_ACQUISITION_H */
