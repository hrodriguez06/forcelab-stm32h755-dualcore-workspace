/**
  ******************************************************************************
  * @file    adc_bm.h
  * @brief   Bare-metal ADC driver for STM32H755 (ADC1, ADC2, ADC3).
  *
  *          Driver tier: *_bm (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Zero overhead, direct register manipulation, hardware-agnostic,
  *          suitable for deterministic Cortex-M4 real-time loops.
  ******************************************************************************
  */

#ifndef ADC_BM_H
#define ADC_BM_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"
#include "dma_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief ADC boost mode options (ADC_CR BOOST[1:0])
  * @note  RM0399 / DS12923 vs Silicon Revisions:
  *        - Rev. Y (<= 0x1003): 1-bit boost (BOOST_0). Active if freq > 20 MHz.
  *        - Rev. V (> 0x1003): Frequency is halved before comparison (freq /= 2).
  *          25 MHz becomes 12.5 MHz, which selects BOOST = 01 (BOOST_0).
  *        Using ADC_BM_BOOST_AUTO dynamically determines the correct bits via DBGMCU->IDCODE.
  */
typedef enum
{
    ADC_BM_BOOST_LE_6_25MHZ  = 0x00000000UL,
    ADC_BM_BOOST_LE_12_5MHZ  = ADC_CR_BOOST_0,
    ADC_BM_BOOST_LE_25MHZ    = ADC_CR_BOOST_1,
    ADC_BM_BOOST_LE_50MHZ    = (ADC_CR_BOOST_0 | ADC_CR_BOOST_1),
    ADC_BM_BOOST_AUTO        = 0xFFFFFFFFUL
} AdcBm_Boost_t;

/* ---- Resolutions --------------------------------------------------------- */
#define ADC_BM_RESOLUTION_16B         0x00000000UL
#define ADC_BM_RESOLUTION_14B         ADC_CFGR_RES_0
#define ADC_BM_RESOLUTION_12B         ADC_CFGR_RES_1
#define ADC_BM_RESOLUTION_10B         (ADC_CFGR_RES_0 | ADC_CFGR_RES_1)
#define ADC_BM_RESOLUTION_8B          ADC_CFGR_RES_2

/* ---- Sampling Times ------------------------------------------------------ */
#define ADC_BM_SAMPLETIME_1CYCLE_5    0x00000000UL
#define ADC_BM_SAMPLETIME_2CYCLES_5   0x00000001UL
#define ADC_BM_SAMPLETIME_8CYCLES_5   0x00000002UL
#define ADC_BM_SAMPLETIME_16CYCLES_5  0x00000003UL
#define ADC_BM_SAMPLETIME_32CYCLES_5  0x00000004UL
#define ADC_BM_SAMPLETIME_64CYCLES_5  0x00000005UL
#define ADC_BM_SAMPLETIME_387CYCLES_5 0x00000006UL
#define ADC_BM_SAMPLETIME_810CYCLES_5 0x00000007UL

/* ---- External Trigger Edges (EXTEN) -------------------------------------- */
#define ADC_BM_TRIGGER_DISABLED       0x00000000UL
#define ADC_BM_TRIGGER_RISING         0x00000001UL
#define ADC_BM_TRIGGER_FALLING        0x00000002UL
#define ADC_BM_TRIGGER_BOTH           0x00000003UL

/* ---- Standard External Trigger Sources (EXTSEL) -------------------------- */
#define ADC_BM_TRIG_TIM1_CH1          0x00000000UL
#define ADC_BM_TRIG_TIM1_CH2          0x00000001UL
#define ADC_BM_TRIG_TIM1_CH3          0x00000002UL
#define ADC_BM_TRIG_TIM2_CH2          0x00000003UL
#define ADC_BM_TRIG_TIM3_TRGO         0x00000004UL
#define ADC_BM_TRIG_TIM4_CH4          0x00000005UL
#define ADC_BM_TRIG_EXTI_11           0x00000006UL
#define ADC_BM_TRIG_TIM8_TRGO         0x00000007UL
#define ADC_BM_TRIG_TIM8_TRGO2        0x00000008UL
#define ADC_BM_TRIG_TIM1_TRGO         0x00000009UL
#define ADC_BM_TRIG_TIM1_TRGO2        0x0000000AUL
#define ADC_BM_TRIG_TIM2_TRGO         0x0000000BUL
#define ADC_BM_TRIG_TIM4_TRGO         0x0000000CUL
#define ADC_BM_TRIG_TIM6_TRGO         0x0000000DUL
#define ADC_BM_TRIG_TIM15_TRGO        0x0000000EUL
#define ADC_BM_TRIG_TIM3_CH4          0x0000000FUL

/* ---- DMA Operating Modes ------------------------------------------------ */
typedef enum
{
    ADC_BM_DMA_DISABLED = 0x00000000UL,
    ADC_BM_DMA_ONESHOT  = ADC_CFGR_DMNGT_0,                      /*!< DMNGT = 0b01: One-shot */
    ADC_BM_DMA_CIRCULAR = (ADC_CFGR_DMNGT_0 | ADC_CFGR_DMNGT_1)   /*!< DMNGT = 0b11: Circular */
} AdcBm_DmaMode_t;

/**
  * @brief ADC Hardware configuration structure (Dependency Injection).
  */
typedef struct
{
    ADC_TypeDef    *adc_instance;      /*!< e.g. ADC1, ADC2, ADC3           */
    uint32_t        channel;           /*!< Channel decimal number (0..19)  */
    uint32_t        resolution;        /*!< ADC_BM_RESOLUTION_*            */
    uint32_t        sampling_time;     /*!< ADC_BM_SAMPLETIME_*             */
    uint32_t        trigger_source;    /*!< ADC_BM_TRIG_* (EXTSEL value)    */
    uint32_t        trigger_edge;      /*!< ADC_BM_TRIGGER_*                */
    AdcBm_Boost_t   boost_mode;        /*!< Boost mode for ker_ck frequency */
    AdcBm_DmaMode_t dma_mode;          /*!< ADC_BM_DMA_*                    */
    bool            dma_circular;      /*!< Legacy compatibility flag       */
    uint16_t       *buffer;            /*!< Pointer to DMA buffer           */
    uint32_t        buffer_length;     /*!< Total samples in buffer         */
} AdcBm_Config_t;

/**
  * @brief Fast integer metrics computed over one half of the DMA buffer.
  */
typedef struct
{
    uint16_t          latest_raw;         /*!< Most recent raw sample                 */
    uint16_t          average_raw;        /*!< Mean code across half-buffer           */
    uint16_t          min_raw;            /*!< Minimum code                           */
    uint16_t          max_raw;            /*!< Maximum code                           */
    uint32_t          millivolts;         /*!< Scaled voltage (mV) at nominal VREF+   */
    volatile uint32_t sample_count;       /*!< Cumulative samples processed           */
    volatile uint32_t dma_tc_count;       /*!< DMA Transfer-Complete counter          */
    volatile uint32_t dma_ht_count;       /*!< DMA Half-Transfer counter              */
    volatile uint32_t dma_error_count;    /*!< DMA error counter                      */
    volatile uint32_t half_overrun_count; /*!< Half-buffers overwritten before consume*/
    volatile uint32_t adc_overrun_count;  /*!< Hardware ADC overrun (OVR flag) counter*/
} AdcBm_Stats_t;

/**
  * @brief ADC Bare-Metal driver handle.
  */
typedef struct
{
    AdcBm_Config_t          config;
    volatile uint8_t        ready_half;     /*!< 0: first half, 1: second half   */
    volatile uint8_t        has_new_data;   /*!< Set by ISR, cleared by consumer */
    volatile AdcBm_Stats_t stats;
} AdcBm_Handle_t;

/**
  * @brief  Computes register bits for ADC_CR_BOOST based on kernel clock and silicon revision.
  * @param  ker_ck_freq_hz ADC kernel clock frequency in Hz (e.g. 25000000 for 25 MHz).
  * @return Bitmask to write into ADC_CR BOOST[1:0] field.
  */
uint32_t ADC_BM_GetBoostBits(uint32_t ker_ck_freq_hz);

/**
  * @brief  Initializes ADC registers, voltage regulator, channel and sequencer.
  * @param  handle Pointer to driver handle.
  * @param  config Pointer to hardware configuration.
  * @return true on success, false on invalid parameters.
  */
bool ADC_BM_Init(AdcBm_Handle_t *handle, const AdcBm_Config_t *config);

/**
  * @brief  Deinitializes ADC peripheral, restores deep-power-down and disables regulator.
  * @param  handle Pointer to driver handle.
  * @return true on success, false on invalid parameters.
  */
bool ADC_BM_Deinit(AdcBm_Handle_t *handle);

/**
  * @brief  Runs full offset and linearity self-calibration on single-ended inputs.
  * @note   Must be called while ADC is disabled (ADEN = 0).
  * @param  adc Pointer to ADC peripheral instance.
  * @return true if calibration succeeded, false on timeout.
  */
bool ADC_BM_Calibrate(ADC_TypeDef *adc);

/**
  * @brief  Enables the ADC (sets ADEN and waits for ADRDY).
  * @param  adc Pointer to ADC peripheral instance.
  * @return true if enabled, false on timeout.
  */
bool ADC_BM_Enable(ADC_TypeDef *adc);

/**
  * @brief  Disables the ADC (sets ADDIS and waits for ADEN=0).
  * @param  adc Pointer to ADC peripheral instance.
  * @return true if disabled, false on timeout.
  */
bool ADC_BM_Disable(ADC_TypeDef *adc);

/**
  * @brief  Starts regular conversions.
  * @param  adc Pointer to ADC peripheral instance.
  */
void ADC_BM_StartConversion(ADC_TypeDef *adc);

/**
  * @brief  Stops regular conversions and waits until idle.
  * @param  adc Pointer to ADC peripheral instance.
  * @return true if stopped successfully, false on timeout.
  */
bool ADC_BM_StopConversion(ADC_TypeDef *adc);

/**
  * @brief  DMA IRQ dispatcher for ADC double-buffering.
  * @param  handle Pointer to driver handle.
  * @param  stream Pointer to associated DMA stream.
  */
void ADC_BM_IRQHandler_DMA(AdcBm_Handle_t *handle, DMA_Stream_TypeDef *stream);

/**
  * @brief  ADC global IRQ handler (checks overrun / error flags).
  * @param  handle Pointer to driver handle.
  */
void ADC_BM_IRQHandler_ADC(AdcBm_Handle_t *handle);

/**
  * @brief  Polls whether a half-buffer is ready for consumption.
  * @param  handle   Pointer to driver handle.
  * @param  out_half Receives index 0 or 1 of the ready half.
  * @return true if new half is ready, false otherwise.
  */
bool ADC_BM_GetReadyHalf(AdcBm_Handle_t *handle, uint8_t *out_half);

/**
  * @brief  Computes min/max/average and millivolts over the specified half.
  * @param  handle  Pointer to driver handle.
  * @param  half    0 for first half, 1 for second half.
  * @param  vref_mv Reference voltage in millivolts (e.g. 3300).
  */
void ADC_BM_ComputeStats(AdcBm_Handle_t *handle, uint8_t half, uint32_t vref_mv);

#ifdef __cplusplus
}
#endif

#endif /* ADC_BM_H */
