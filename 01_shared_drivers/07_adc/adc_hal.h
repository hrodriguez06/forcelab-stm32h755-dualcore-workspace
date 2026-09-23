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
    ADC_HAL_BOOST_LE_50MHZ    = (ADC_CR_BOOST_0 | ADC_CR_BOOST_1),    /* 25.0 - 50.0 MHz */
    ADC_HAL_BOOST_AUTO        = 0xFFFFFFFFUL                          /* Automatic from ST HAL */
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

/* ---- Internal ADC Channels (ADC3 only) ----------------------------------- */
#define ADC_HAL_CHANNEL_VBAT           ADC_CHANNEL_VBAT
#define ADC_HAL_CHANNEL_TEMPSENSOR     ADC_CHANNEL_TEMPSENSOR
#define ADC_HAL_CHANNEL_VREFINT        ADC_CHANNEL_VREFINT

/* ---- Oversampling Ratios ------------------------------------------------- */
#define ADC_HAL_OVERSAMPLING_2X        2U
#define ADC_HAL_OVERSAMPLING_4X        4U
#define ADC_HAL_OVERSAMPLING_8X        8U
#define ADC_HAL_OVERSAMPLING_16X       16U
#define ADC_HAL_OVERSAMPLING_32X       32U
#define ADC_HAL_OVERSAMPLING_64X       64U
#define ADC_HAL_OVERSAMPLING_128X      128U
#define ADC_HAL_OVERSAMPLING_256X      256U

/* ---- Oversampling Shifts ------------------------------------------------- */
#define ADC_HAL_OVERSAMPLING_SHIFT_NONE ADC_RIGHTBITSHIFT_NONE
#define ADC_HAL_OVERSAMPLING_SHIFT_1B   ADC_RIGHTBITSHIFT_1
#define ADC_HAL_OVERSAMPLING_SHIFT_2B   ADC_RIGHTBITSHIFT_2
#define ADC_HAL_OVERSAMPLING_SHIFT_3B   ADC_RIGHTBITSHIFT_3
#define ADC_HAL_OVERSAMPLING_SHIFT_4B   ADC_RIGHTBITSHIFT_4
#define ADC_HAL_OVERSAMPLING_SHIFT_5B   ADC_RIGHTBITSHIFT_5
#define ADC_HAL_OVERSAMPLING_SHIFT_6B   ADC_RIGHTBITSHIFT_6
#define ADC_HAL_OVERSAMPLING_SHIFT_7B   ADC_RIGHTBITSHIFT_7
#define ADC_HAL_OVERSAMPLING_SHIFT_8B   ADC_RIGHTBITSHIFT_8

/**
  * @brief Single channel descriptor within a regular conversion sequence.
  */
typedef struct
{
    uint32_t channel;           /*!< e.g. ADC_CHANNEL_15, ADC_CHANNEL_VREFINT */
    uint32_t sampling_time;     /*!< e.g. ADC_SAMPLETIME_32CYCLES_5          */
} AdcHal_ChannelConfig_t;

/**
  * @brief Extended multi-channel ADC configuration (F3, F4, F5, F6).
  */
typedef struct
{
    ADC_TypeDef                  *adc_instance;      /*!< e.g. ADC1, ADC2, ADC3           */
    const AdcHal_ChannelConfig_t *channels;          /*!< Array of channels in sequence   */
    uint32_t                      num_channels;      /*!< Number of channels (1..16)      */
    uint32_t                      resolution;        /*!< e.g. ADC_RESOLUTION_16B         */
    uint32_t                      trigger_source;    /*!< e.g. ADC_EXTERNALTRIG_T3_TRGO   */
    uint32_t                      trigger_edge;      /*!< e.g. ADC_EXTERNALTRIGCONVEDGE_RISING */
    AdcHal_Boost_t                boost_mode;        /*!< e.g. ADC_HAL_BOOST_AUTO         */

    DMA_Stream_TypeDef           *dma_stream;        /*!< e.g. DMA1_Stream0 (NULL if disabled) */
    uint32_t                      dma_request;       /*!< e.g. DMA_REQUEST_ADC1           */
    uint16_t                     *buffer;            /*!< Pointer to DMA buffer (NULL if disabled) */
    uint32_t                      buffer_length;     /*!< Total samples in buffer         */

    uint32_t                      kernel_clock_hz;   /*!< ADC kernel clock in Hz. 0 = 25 MHz */
    GPIO_TypeDef                 *analog_pin_port;   /*!< GPIO port for analog pin (NULL = skip) */
    uint32_t                      analog_pin_idx;    /*!< Pin index 0..15                 */

    /* F6 Oversampling */
    bool                          oversampling_enable;
    uint32_t                      oversampling_ratio; /*!< ADC_HAL_OVERSAMPLING_*X         */
    uint32_t                      oversampling_shift; /*!< ADC_HAL_OVERSAMPLING_SHIFT_*    */

    /* F5 Analog Watchdog 1 (AWD1).
     * NOTE (Errata ES0445 2.10.4): The analog watchdog output toward timers
     * (ADC_AWDy_OUT) resets upon completion of any regular conversion of an
     * unmonitored channel. When gating/triggering timers via AWD, set
     * awd_all_channels = true. */
    bool                          awd_enable;
    bool                          awd_all_channels;   /*!< true: monitor all chs in sequence */
    uint32_t                      awd_channel;        /*!< Monitored ch if !awd_all_channels */
    uint32_t                      awd_high_threshold; /*!< High threshold raw code         */
    uint32_t                      awd_low_threshold;  /*!< Low threshold raw code          */

    /* Clock and conversion mode configuration */
    uint32_t                      clock_prescaler;    /*!< HAL ClockPrescaler (0 = ADC_CLOCK_ASYNC_DIV1) */
    uint32_t                      clock_mode;         /*!< Clock mode (0 = legacy async) */
    bool                          continuous_mode;    /*!< Continuous conversion mode (false = DISABLE, true = ENABLE) */
} AdcHal_ConfigEx_t;

/**
  * @brief Snapshot metrics computed over one half of the DMA buffer
  */
typedef struct
{
    uint16_t          latest_raw;         /*!< Most recent raw ADC sample              */
    uint16_t          average_raw;        /*!< Mean over half-buffer                   */
    uint16_t          min_raw;            /*!< Minimum observed in half-buffer         */
    uint16_t          max_raw;            /*!< Maximum observed in half-buffer         */
    uint32_t          millivolts;         /*!< Scaled voltage (mV) at nominal VREF+    */
    volatile uint32_t sample_count;       /*!< Cumulative samples processed            */
    volatile uint32_t dma_tc_count;       /*!< Transfer-Complete IRQ counter           */
    volatile uint32_t dma_ht_count;       /*!< Half-Transfer IRQ counter               */
    volatile uint32_t dma_error_count;    /*!< DMA error counter                       */
    volatile uint32_t half_overrun_count; /*!< Dropped half-buffers due to consumer lag*/
    volatile uint32_t adc_overrun_count;  /*!< Hardware ADC overrun (OVR flag) counter */
    volatile uint32_t awd_event_count;    /*!< Analog watchdog threshold cross events  */
} AdcHal_Stats_t;

/**
  * @brief Peripheral driver instance handle
  */
typedef struct
{
    ADC_HandleTypeDef       hadc;
    DMA_HandleTypeDef       hdma;
    AdcHal_Config_t         config;

    volatile uint8_t        ready_half;     /*!< 0: first half ready, 1: second half ready */
    volatile uint8_t        has_new_data;   /*!< Flag set by ISR, cleared by consumer      */
    volatile AdcHal_Stats_t stats;

    AdcHal_ConfigEx_t       config_ex;
    AdcHal_ChannelConfig_t  single_channel;
} AdcHal_Handle_t;

/**
  * @brief  Initializes ADC instance, pin-agnostic channel configuration, and DMA.
  * @param  handle  Driver handle pointer.
  * @param  config  Hardware configuration parameters.
  * @return true on success, false on error.
  */
bool ADC_HAL_Init(AdcHal_Handle_t *handle, const AdcHal_Config_t *config);

/**
  * @brief  Extended initialization supporting multi-channel sequences, oversampling and AWD.
  * @param  handle  Driver handle pointer.
  * @param  config  Hardware configuration parameters.
  * @return true on success, false on error.
  */
bool ADC_HAL_InitEx(AdcHal_Handle_t *handle, const AdcHal_ConfigEx_t *config);

/**
  * @brief  Single conversion on specified channel in polling mode (blocking with timeout).
  * @param  handle        Driver handle pointer.
  * @param  channel       Channel identifier (e.g. ADC_CHANNEL_15, ADC_CHANNEL_VREFINT).
  * @param  sampling_time Sampling time constant (ADC_SAMPLETIME_*).
  * @param  out_raw       Receives the raw 16-bit conversion result.
  * @param  timeout_ms    Timeout in milliseconds.
  * @return true on success, false on timeout or parameter error.
  */
bool ADC_HAL_ReadSingle(AdcHal_Handle_t *handle, uint32_t channel, uint32_t sampling_time, uint16_t *out_raw, uint32_t timeout_ms);

/**
  * @brief  Reads and atomically clears accumulated analog watchdog event counter.
  * @param  handle  Driver handle pointer.
  * @return Number of AWD events since last call.
  */
uint32_t ADC_HAL_TakeAwdEvents(AdcHal_Handle_t *handle);

/**
  * @brief  Deinitializes ADC and DMA peripherals, unregisters handle from internal table.
  * @param  handle  Driver handle pointer.
  * @return true on success, false on error.
  */
bool ADC_HAL_Deinit(AdcHal_Handle_t *handle);

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

/**
  * @brief  Computes min/max/average and millivolts for a specific channel in an interleaved sequence.
  * @param  handle          Driver handle pointer.
  * @param  half            0 for first half, 1 for second half.
  * @param  channel_seq_idx Sequence index (0 .. num_channels-1).
  * @param  vref_mv         Reference voltage in millivolts (e.g. 3300).
  */
void ADC_HAL_ComputeStatsChannel(AdcHal_Handle_t *handle, uint8_t half, uint32_t channel_seq_idx, uint32_t vref_mv);

/**
  * @brief ADC factory calibration data container.
  */
typedef struct
{
    uint16_t vrefint_cal;    /*!< Raw VREFINT calibration word (3.3V, 30°C) */
    uint16_t ts_cal1;        /*!< Raw TS_CAL1 calibration word (3.3V, 30°C) */
    uint16_t ts_cal2;        /*!< Raw TS_CAL2 calibration word (3.3V, 130°C) */
} AdcHal_CalibrationData_t;

#define ADC_HAL_TEMPERATURE_ERROR   (-999L)

/**
  * @brief  Injects factory calibration data (especially needed on Cortex-M4 where ROM is unreachable).
  * @param  cal Pointer to calibration data structure.
  * @return true if calibration data is plausible, false otherwise.
  */
bool ADC_HAL_InjectCalibration(const AdcHal_CalibrationData_t *cal);

/**
  * @brief  Calculates VDDA in millivolts from a raw VREFINT reading using factory calibration.
  * @param  vrefint_raw     Raw conversion code from channel VREFINT.
  * @param  resolution_bits ADC resolution bits (16, 14, 12, 10, 8; 0 defaults to 16).
  * @return VDDA in millivolts (nominal ~3300), or 0U if uncalibrated/error.
  */
uint32_t ADC_HAL_CalculateVDDA(uint16_t vrefint_raw, uint8_t resolution_bits);

/**
  * @brief  Calculates junction temperature in degrees Celsius from a raw temperature sensor reading.
  * @param  temp_raw        Raw conversion code from channel TEMPSENSOR.
  * @param  vdda_mv         Actual VDDA supply voltage in mV (calculated via ADC_HAL_CalculateVDDA).
  * @param  resolution_bits ADC resolution bits (16, 14, 12, 10, 8; 0 defaults to 16).
  * @return Temperature in degrees Celsius, or ADC_HAL_TEMPERATURE_ERROR if uncalibrated/error.
  */
int32_t ADC_HAL_CalculateTemperature(uint16_t temp_raw, uint32_t vdda_mv, uint8_t resolution_bits);

#define ADC_HAL_CalculateVDDA_16B(raw)         ADC_HAL_CalculateVDDA((raw), 16U)
#define ADC_HAL_CalculateTemperature_16B(r, v) ADC_HAL_CalculateTemperature((r), (v), 16U)

/**
  * @brief  Public callback hooks. Projects with custom callbacks can call these directly.
  * @param  hadc ADC handle pointer.
  */
void ADC_HAL_OnConvHalfCplt(ADC_HandleTypeDef *hadc);
void ADC_HAL_OnConvCplt(ADC_HandleTypeDef *hadc);
void ADC_HAL_OnError(ADC_HandleTypeDef *hadc);
void ADC_HAL_OnLevelOutOfWindow(ADC_HandleTypeDef *hadc);

#ifdef __cplusplus
}
#endif

#endif /* HAL_ADC_MODULE_ENABLED */

#endif /* ADC_HAL_H */
