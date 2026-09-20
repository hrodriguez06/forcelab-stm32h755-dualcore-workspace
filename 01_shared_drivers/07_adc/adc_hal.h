/**
  ******************************************************************************
  * @file    adc_hal.h
  * @brief   ADC + DMA driver for STM32H755 (Plan A 3-tier architecture: *_hal).
  *
  *          Provides decoupled initialization, calibration and circular DMA
  *          management for ADC1 / ADC2.
  *
  *          Silicon errata & hardware guarantees:
  *          - ES0445 2.10.x: JQDIS set to disable injected context queue.
  *          - RM0399 / adc.txt:4597: BOOST[1:0] configured according to
  *            adc_ker_ck frequency (BOOST = 10 for 12.5 MHz < f <= 25 MHz).
  *          - Mandatory calibration (offset and linearity) prior to conversion.
  *          - Fast integer statistics computation (zero floating-point in
  *            hot path).
  ******************************************************************************
  */

#ifndef ADC_HAL_H
#define ADC_HAL_H

#include "stm32h7xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

#if defined(HAL_ADC_MODULE_ENABLED)

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief ADC boost mode options (ADC_CR BOOST[1:0])
  */
typedef enum
{
    ADC_HAL_BOOST_LE_6_25MHZ  = 0x00000000UL,                         /* <= 6.25 MHz */
    ADC_HAL_BOOST_LE_12_5MHZ  = ADC_CR_BOOST_0,                       /* 6.25 - 12.5 MHz */
    ADC_HAL_BOOST_LE_25MHZ    = ADC_CR_BOOST_1,                       /* 12.5 - 25.0 MHz */
    ADC_HAL_BOOST_LE_50MHZ    = (ADC_CR_BOOST_0 | ADC_CR_BOOST_1)     /* 25.0 - 50.0 MHz */
} AdcHal_Boost_t;

/**
  * @brief Dependency injection configuration for ADC + DMA
  */
typedef struct
{
    ADC_TypeDef          *adc_instance;      /*!< e.g. ADC1                       */
    uint32_t              channel;           /*!< e.g. ADC_CHANNEL_15             */
    uint32_t              resolution;        /*!< e.g. ADC_RESOLUTION_16B         */
    uint32_t              sampling_time;     /*!< e.g. ADC_SAMPLETIME_32CYCLES_5  */
    uint32_t              trigger_source;    /*!< e.g. ADC_EXTERNALTRIG_T3_TRGO   */
    uint32_t              trigger_edge;      /*!< e.g. ADC_EXTERNALTRIGCONVEDGE_RISING */
    AdcHal_Boost_t        boost_mode;        /*!< e.g. ADC_HAL_BOOST_LE_25MHZ     */

    DMA_Stream_TypeDef   *dma_stream;        /*!< e.g. DMA1_Stream0               */
    uint32_t              dma_request;       /*!< e.g. DMA_REQUEST_ADC1           */
    uint16_t             *buffer;            /*!< Pointer to DMA buffer in SRAM2  */
    uint32_t              buffer_length;     /*!< Total samples in buffer (e.g. 512) */
} AdcHal_Config_t;

/**
  * @brief Snapshot metrics computed over one half of the DMA buffer
  */
typedef struct
{
    uint16_t latest_raw;       /*!< Most recent raw ADC sample              */
    uint16_t average_raw;      /*!< Mean over half-buffer                   */
    uint16_t min_raw;          /*!< Minimum observed in half-buffer         */
    uint16_t max_raw;          /*!< Maximum observed in half-buffer         */
    uint32_t millivolts;       /*!< Scaled voltage (mV) at nominal VREF+    */
    uint32_t sample_count;     /*!< Cumulative samples processed            */
    uint32_t dma_tc_count;        /*!< Transfer-Complete IRQ counter           */
    uint32_t dma_ht_count;        /*!< Half-Transfer IRQ counter               */
    uint32_t dma_error_count;     /*!< DMA error counter                       */
    uint32_t half_overrun_count;  /*!< Dropped half-buffers due to consumer lag*/
} AdcHal_Stats_t;

/**
  * @brief Peripheral driver instance handle
  */
typedef struct
{
    ADC_HandleTypeDef   hadc;
    DMA_HandleTypeDef   hdma;
    AdcHal_Config_t     config;

    volatile uint8_t    ready_half;     /*!< 0: first half ready, 1: second half ready */
    volatile uint8_t    has_new_data;   /*!< Flag set by ISR, cleared by consumer      */
    AdcHal_Stats_t      stats;
} AdcHal_Handle_t;

/**
  * @brief  Initializes ADC instance, pin-agnostic channel configuration, and DMA.
  * @param  handle  Driver handle pointer.
  * @param  config  Hardware configuration parameters.
  * @return true on success, false on error.
  */
bool ADC_HAL_Init(AdcHal_Handle_t *handle, const AdcHal_Config_t *config);

/**
  * @brief  Performs full offset and linearity calibration on single-ended inputs.
  * @param  handle  Driver handle pointer.
  * @return true on success, false on error.
  */
bool ADC_HAL_Calibrate(AdcHal_Handle_t *handle);

/**
  * @brief  Starts circular DMA acquisition.
  * @param  handle  Driver handle pointer.
  * @return true on success, false on error.
  */
bool ADC_HAL_Start_DMA(AdcHal_Handle_t *handle);

/**
  * @brief  Stops circular DMA acquisition.
  * @param  handle  Driver handle pointer.
  * @return true on success, false on error.
  */
bool ADC_HAL_Stop_DMA(AdcHal_Handle_t *handle);

/**
  * @brief  DMA stream IRQ handler entry. Call from DMAx_Streamy_IRQHandler.
  * @param  handle  Driver handle pointer.
  */
void ADC_HAL_IRQHandler_DMA(AdcHal_Handle_t *handle);

/**
  * @brief  ADC global IRQ handler entry. Call from ADC_IRQHandler.
  * @param  handle  Driver handle pointer.
  */
void ADC_HAL_IRQHandler_ADC(AdcHal_Handle_t *handle);

/**
  * @brief  Polls whether a half-buffer is ready for processing.
  * @param  handle    Driver handle pointer.
  * @param  out_half  Output pointer receiving 0 or 1.
  * @return true if new half is available, false otherwise.
  */
bool ADC_HAL_GetReadyHalf(AdcHal_Handle_t *handle, uint8_t *out_half);

/**
  * @brief  Computes min/max/average and millivolts over the specified half.
  * @param  handle   Driver handle pointer.
  * @param  half     0 for first half (0..len/2-1), 1 for second half (len/2..len-1).
  * @param  vref_mv  Nominal reference voltage in mV (e.g. 3300).
  */
void ADC_HAL_ComputeStats(AdcHal_Handle_t *handle, uint8_t half, uint32_t vref_mv);

#ifdef __cplusplus
}
#endif

#endif /* HAL_ADC_MODULE_ENABLED */

#endif /* ADC_HAL_H */
