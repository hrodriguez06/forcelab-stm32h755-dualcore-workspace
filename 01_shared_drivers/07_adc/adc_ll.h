/**
  ******************************************************************************
  * @file    adc_ll.h
  * @brief   Low-Layer ADC driver for STM32H755 (ADC1, ADC2, ADC3).
  *
  *          Driver tier: *_ll (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          High performance, low footprint, based purely on ST LL static inline
  *          functions. Does not require USE_FULL_LL_DRIVER and does not include
  *          stm32h7xx_hal.h.
  ******************************************************************************
  */

#ifndef ADC_LL_H
#define ADC_LL_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"
#include "stm32h7xx_ll_adc.h"
#include "dma_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief ADC boost mode options (ADC_CR BOOST[1:0])
  */
typedef enum
{
    ADC_LL_BOOST_LE_6_25MHZ  = 0x00000000UL,
    ADC_LL_BOOST_LE_12_5MHZ  = ADC_CR_BOOST_0,
    ADC_LL_BOOST_LE_25MHZ    = ADC_CR_BOOST_1,
    ADC_LL_BOOST_LE_50MHZ    = (ADC_CR_BOOST_0 | ADC_CR_BOOST_1),
    ADC_LL_BOOST_AUTO        = 0xFFFFFFFFUL
} AdcLl_Boost_t;

/* ---- Resolutions --------------------------------------------------------- */
#define ADC_LL_RESOLUTION_16B         LL_ADC_RESOLUTION_16B
#define ADC_LL_RESOLUTION_14B         LL_ADC_RESOLUTION_14B
#define ADC_LL_RESOLUTION_12B         LL_ADC_RESOLUTION_12B
#define ADC_LL_RESOLUTION_10B         LL_ADC_RESOLUTION_10B
#define ADC_LL_RESOLUTION_8B          LL_ADC_RESOLUTION_8B
#define ADC_LL_RESOLUTION_14B_OPT     0x00000014UL
#define ADC_LL_RESOLUTION_12B_OPT     0x00000012UL

/* ---- Sampling Times ------------------------------------------------------ */
#define ADC_LL_SAMPLETIME_1CYCLE_5    LL_ADC_SAMPLINGTIME_1CYCLE_5
#define ADC_LL_SAMPLETIME_2CYCLES_5   LL_ADC_SAMPLINGTIME_2CYCLES_5
#define ADC_LL_SAMPLETIME_8CYCLES_5   LL_ADC_SAMPLINGTIME_8CYCLES_5
#define ADC_LL_SAMPLETIME_16CYCLES_5  LL_ADC_SAMPLINGTIME_16CYCLES_5
#define ADC_LL_SAMPLETIME_32CYCLES_5  LL_ADC_SAMPLINGTIME_32CYCLES_5
#define ADC_LL_SAMPLETIME_64CYCLES_5  LL_ADC_SAMPLINGTIME_64CYCLES_5
#define ADC_LL_SAMPLETIME_387CYCLES_5 LL_ADC_SAMPLINGTIME_387CYCLES_5
#define ADC_LL_SAMPLETIME_810CYCLES_5 LL_ADC_SAMPLINGTIME_810CYCLES_5

/* ---- External Trigger Edges (EXTEN) -------------------------------------- */
#define ADC_LL_TRIGGER_DISABLED       0x00000000UL
#define ADC_LL_TRIGGER_RISING         LL_ADC_REG_TRIG_EXT_RISING
#define ADC_LL_TRIGGER_FALLING        LL_ADC_REG_TRIG_EXT_FALLING
#define ADC_LL_TRIGGER_BOTH           LL_ADC_REG_TRIG_EXT_RISINGFALLING

/* ---- Standard External Trigger Sources (EXTSEL) -------------------------- */
#define ADC_LL_TRIG_TIM1_CH1          0x00000000UL
#define ADC_LL_TRIG_TIM1_CH2          0x00000001UL
#define ADC_LL_TRIG_TIM1_CH3          0x00000002UL
#define ADC_LL_TRIG_TIM2_CH2          0x00000003UL
#define ADC_LL_TRIG_TIM3_TRGO         0x00000004UL
#define ADC_LL_TRIG_TIM4_CH4          0x00000005UL
#define ADC_LL_TRIG_EXTI_11           0x00000006UL
#define ADC_LL_TRIG_TIM8_TRGO         0x00000007UL
#define ADC_LL_TRIG_TIM8_TRGO2        0x00000008UL
#define ADC_LL_TRIG_TIM1_TRGO         0x00000009UL
#define ADC_LL_TRIG_TIM1_TRGO2        0x0000000AUL
#define ADC_LL_TRIG_TIM2_TRGO         0x0000000BUL
#define ADC_LL_TRIG_TIM4_TRGO         0x0000000CUL
#define ADC_LL_TRIG_TIM6_TRGO         0x0000000DUL
#define ADC_LL_TRIG_TIM15_TRGO        0x0000000EUL
#define ADC_LL_TRIG_TIM3_CH4          0x0000000FUL

/* ---- Clock Modes (ADCx_CCR.CKMODE) --------------------------------------- */
typedef enum
{
    ADC_LL_CLOCK_ASYNC   = 0x00000000UL,                          /*!< adc_ker_ck (default)   */
    ADC_LL_CLOCK_SYNC_1  = ADC_CCR_CKMODE_0,                      /*!< adc_sclk / 1           */
    ADC_LL_CLOCK_SYNC_2  = ADC_CCR_CKMODE_1,                      /*!< adc_sclk / 2           */
    ADC_LL_CLOCK_SYNC_4  = (ADC_CCR_CKMODE_0 | ADC_CCR_CKMODE_1)  /*!< adc_sclk / 4           */
} AdcLl_ClockMode_t;

/* ---- Asynchronous Prescaler (ADCx_CCR.PRESC) ----------------------------- */
typedef enum
{
    ADC_LL_PRESC_DIV1   = 0x00000000UL,    /*!< /1  (default, 0 = legacy) */
    ADC_LL_PRESC_DIV2   = 0x00000001UL,    /*!< /2  */
    ADC_LL_PRESC_DIV4   = 0x00000002UL,    /*!< /4  */
    ADC_LL_PRESC_DIV6   = 0x00000003UL,    /*!< /6  */
    ADC_LL_PRESC_DIV8   = 0x00000004UL,    /*!< /8  */
    ADC_LL_PRESC_DIV10  = 0x00000005UL,    /*!< /10 */
    ADC_LL_PRESC_DIV12  = 0x00000006UL,    /*!< /12 */
    ADC_LL_PRESC_DIV16  = 0x00000007UL,    /*!< /16 */
    ADC_LL_PRESC_DIV32  = 0x00000008UL,    /*!< /32 */
    ADC_LL_PRESC_DIV64  = 0x00000009UL,    /*!< /64 */
    ADC_LL_PRESC_DIV128 = 0x0000000AUL,    /*!< /128 */
    ADC_LL_PRESC_DIV256 = 0x0000000BUL     /*!< /256 */
} AdcLl_Prescaler_t;

/* ---- DMA Operating Modes ------------------------------------------------ */
typedef enum
{
    ADC_LL_DMA_DISABLED = 0x00000000UL,
    ADC_LL_DMA_ONESHOT  = LL_ADC_REG_DMA_TRANSFER_LIMITED,
    ADC_LL_DMA_CIRCULAR = LL_ADC_REG_DMA_TRANSFER_UNLIMITED
} AdcLl_DmaMode_t;

/**
  * @brief ADC Hardware configuration structure (Dependency Injection).
  */
typedef struct
{
    ADC_TypeDef       *adc_instance;      /*!< e.g. ADC1, ADC2, ADC3           */
    uint32_t           channel;           /*!< Channel decimal number (0..19)  */
    uint32_t           resolution;        /*!< ADC_LL_RESOLUTION_*            */
    uint32_t           sampling_time;     /*!< ADC_LL_SAMPLETIME_*             */
    uint32_t           trigger_source;    /*!< ADC_LL_TRIG_* (EXTSEL value)    */
    uint32_t           trigger_edge;      /*!< ADC_LL_TRIGGER_*                */
    AdcLl_Boost_t      boost_mode;        /*!< Boost mode for ker_ck frequency */
    AdcLl_DmaMode_t    dma_mode;          /*!< ADC_LL_DMA_*                    */
    bool               dma_circular;      /*!< Legacy compatibility flag       */
    uint16_t          *buffer;            /*!< Pointer to DMA buffer           */
    uint32_t           buffer_length;     /*!< Total samples in buffer         */

    uint32_t           kernel_clock_hz;   /*!< ADC kernel clock in Hz. 0 = 25 MHz */
    AdcLl_ClockMode_t  clock_mode;        /*!< ADCx_CCR.CKMODE. 0 = async      */
    AdcLl_Prescaler_t  prescaler;         /*!< ADCx_CCR.PRESC. 0 = /1          */
    GPIO_TypeDef       *analog_pin_port;   /*!< GPIO port for analog pin. NULL = skip */
    uint32_t            analog_pin_idx;    /*!< Pin index 0..15                 */
} AdcLl_Config_t;

/* ---- Internal ADC Channels (ADC3 only) ----------------------------------- */
#define ADC_LL_CHANNEL_VBAT           17U
#define ADC_LL_CHANNEL_TEMPSENSOR     18U
#define ADC_LL_CHANNEL_VREFINT        19U

/* ---- Oversampling Ratios (ratio N: 2..1024, written to CFGR2 as (N-1)) ---- */
#define ADC_LL_OVERSAMPLING_2X        2U
#define ADC_LL_OVERSAMPLING_4X        4U
#define ADC_LL_OVERSAMPLING_8X        8U
#define ADC_LL_OVERSAMPLING_16X       16U
#define ADC_LL_OVERSAMPLING_32X       32U
#define ADC_LL_OVERSAMPLING_64X       64U
#define ADC_LL_OVERSAMPLING_128X      128U
#define ADC_LL_OVERSAMPLING_256X      256U
#define ADC_LL_OVERSAMPLING_512X      512U
#define ADC_LL_OVERSAMPLING_1024X     1024U

/* ---- Oversampling Shifts (number of bits to right-shift) ----------------- */
#define ADC_LL_OVERSAMPLING_SHIFT_NONE 0U
#define ADC_LL_OVERSAMPLING_SHIFT_1B   1U
#define ADC_LL_OVERSAMPLING_SHIFT_2B   2U
#define ADC_LL_OVERSAMPLING_SHIFT_3B   3U
#define ADC_LL_OVERSAMPLING_SHIFT_4B   4U
#define ADC_LL_OVERSAMPLING_SHIFT_5B   5U
#define ADC_LL_OVERSAMPLING_SHIFT_6B   6U
#define ADC_LL_OVERSAMPLING_SHIFT_7B   7U
#define ADC_LL_OVERSAMPLING_SHIFT_8B   8U

/**
  * @brief Single channel descriptor within a regular conversion sequence.
  */
typedef struct
{
    uint32_t channel;           /*!< Channel decimal number (0..19) */
    uint32_t sampling_time;     /*!< ADC_LL_SAMPLETIME_*            */
} AdcLl_ChannelConfig_t;

/**
  * @brief Extended multi-channel ADC configuration (F3, F4, F5, F6).
  */
typedef struct
{
    ADC_TypeDef                 *adc_instance;      /*!< e.g. ADC1, ADC2, ADC3           */
    const AdcLl_ChannelConfig_t *channels;          /*!< Array of channels in sequence   */
    uint32_t                     num_channels;      /*!< Number of channels (1..16)      */
    uint32_t                     resolution;        /*!< ADC_LL_RESOLUTION_*            */
    uint32_t                     trigger_source;    /*!< ADC_LL_TRIG_* (EXTSEL value)    */
    uint32_t                     trigger_edge;      /*!< ADC_LL_TRIGGER_*                */
    AdcLl_Boost_t                boost_mode;        /*!< Boost mode for ker_ck frequency */
    AdcLl_DmaMode_t              dma_mode;          /*!< ADC_LL_DMA_*                    */
    bool                         dma_circular;      /*!< Circular mode                   */
    uint16_t                    *buffer;            /*!< Pointer to DMA buffer           */
    uint32_t                     buffer_length;     /*!< Total samples in buffer         */
    uint32_t                     kernel_clock_hz;   /*!< ADC kernel clock in Hz. 0 = 25M */
    AdcLl_ClockMode_t            clock_mode;        /*!< ADCx_CCR.CKMODE                 */
    AdcLl_Prescaler_t            prescaler;         /*!< ADCx_CCR.PRESC                  */
    GPIO_TypeDef                *analog_pin_port;   /*!< GPIO port for first analog pin  */
    uint32_t                     analog_pin_idx;    /*!< Pin index 0..15                 */

    /* F6 Oversampling */
    bool                         oversampling_enable;
    uint32_t                     oversampling_ratio; /*!< ADC_LL_OVERSAMPLING_*X (2..1024) */
    uint32_t                     oversampling_shift; /*!< ADC_LL_OVERSAMPLING_SHIFT_* (0..8)*/

    /* F5 Analog Watchdog 1 (AWD1).
     * NOTE (Errata ES0445 2.10.4): The analog watchdog output toward timers
     * (ADC_AWDy_OUT) resets upon completion of any regular conversion of an
     * unmonitored channel. When gating/triggering timers via AWD, set
     * awd_all_channels = true. */
    bool                         awd_enable;
    bool                         awd_all_channels;   /*!< true: monitor all chs in sequence */
    uint32_t                     awd_channel;        /*!< Monitored ch if !awd_all_channels */
    uint32_t                     awd_high_threshold; /*!< High threshold raw code         */
    uint32_t                     awd_low_threshold;  /*!< Low threshold raw code          */
} AdcLl_ConfigEx_t;

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
    volatile uint32_t awd_event_count;    /*!< Analog watchdog threshold cross events */
} AdcLl_Stats_t;

/**
  * @brief ADC Low-Layer driver handle.
  */
typedef struct
{
    AdcLl_Config_t          config;         /*!< Legacy configuration copy */
    volatile uint8_t        ready_half;     /*!< 0: first half, 1: second half   */
    volatile uint8_t        has_new_data;   /*!< Set by ISR, cleared by consumer */
    volatile AdcLl_Stats_t  stats;

    /* --- New fields (v1.1) appended at the end to satisfy Rule 1 --- */
    AdcLl_ConfigEx_t        config_ex;      /*!< Extended multi-channel configuration */
    AdcLl_ChannelConfig_t   single_channel; /*!< Storage for legacy single-channel */
} AdcLl_Handle_t;

/**
  * @brief  Computes register bits for ADC_CR_BOOST based on kernel clock and silicon revision.
  * @param  ker_ck_freq_hz ADC kernel clock frequency in Hz.
  * @return Bitmask for ADC_CR BOOST[1:0] field.
  */
uint32_t ADC_LL_GetBoostBits(uint32_t ker_ck_freq_hz);

/**
  * @brief  Initializes ADC registers, voltage regulator, channel and sequencer via LL.
  * @param  handle Pointer to driver handle.
  * @param  config Pointer to hardware configuration.
  * @return true on success, false on invalid parameters.
  */
bool ADC_LL_Init(AdcLl_Handle_t *handle, const AdcLl_Config_t *config);

/**
  * @brief  Extended initialization supporting multi-channel sequences, oversampling and AWD.
  * @param  handle Pointer to driver handle.
  * @param  config Pointer to extended configuration.
  * @return true on success, false on invalid parameters.
  */
bool ADC_LL_InitEx(AdcLl_Handle_t *handle, const AdcLl_ConfigEx_t *config);

/**
  * @brief  Deinitializes ADC peripheral, restores deep-power-down and disables regulator.
  * @param  handle Pointer to driver handle.
  * @return true on success, false on invalid parameters.
  */
bool ADC_LL_Deinit(AdcLl_Handle_t *handle);

/**
  * @brief  Runs full offset and linearity self-calibration on single-ended inputs.
  * @param  adc Pointer to ADC peripheral instance.
  * @return true if calibration succeeded, false on timeout.
  */
bool ADC_LL_Calibrate(ADC_TypeDef *adc);

/**
  * @brief  Enables the ADC (sets ADEN and waits for ADRDY).
  * @param  adc Pointer to ADC peripheral instance.
  * @return true if enabled, false on timeout.
  */
bool ADC_LL_Enable(ADC_TypeDef *adc);

/**
  * @brief  Disables the ADC (sets ADDIS and waits for ADEN=0).
  * @param  adc Pointer to ADC peripheral instance.
  * @return true if disabled, false on timeout.
  */
bool ADC_LL_Disable(ADC_TypeDef *adc);

/**
  * @brief  Starts regular conversions.
  * @param  adc Pointer to ADC peripheral instance.
  */
void ADC_LL_StartConversion(ADC_TypeDef *adc);

/**
  * @brief  Stops regular conversions and waits until idle.
  * @param  adc Pointer to ADC peripheral instance.
  * @return true if stopped successfully, false on timeout.
  */
bool ADC_LL_StopConversion(ADC_TypeDef *adc);

/**
  * @brief  Single software-triggered conversion on specified channel (polling mode).
  * @note   PRECONDITION: The ADC must NOT have a conversion in progress (ADSTART=0)
  *         and must NOT be in DMA mode (DMNGT=0). Returns false if active.
  * @param  adc           Pointer to ADC instance.
  * @param  channel       Channel number (0..19).
  * @param  sampling_time Sampling time constant (ADC_LL_SAMPLETIME_*).
  * @param  out_raw       Receives the raw 16-bit conversion result.
  * @param  spin_limit    Maximum loop iterations before timing out.
  * @return true on success, false on timeout, busy or parameter error.
  */
bool ADC_LL_ReadSingle(ADC_TypeDef *adc, uint32_t channel, uint32_t sampling_time, uint16_t *out_raw, uint32_t spin_limit);

/**
  * @brief  DMA IRQ dispatcher for ADC double-buffering.
  * @param  handle Pointer to driver handle.
  * @param  stream Pointer to associated DMA stream.
  */
void ADC_LL_IRQHandler_DMA(AdcLl_Handle_t *handle, DMA_Stream_TypeDef *stream);

/**
  * @brief  ADC global IRQ handler (checks overrun and analog watchdog flags).
  * @param  handle Pointer to driver handle.
  */
void ADC_LL_IRQHandler_ADC(AdcLl_Handle_t *handle);

/**
  * @brief  Reads and atomically clears accumulated analog watchdog event counter.
  *         Re-enables AWD1 interrupt if AWD was enabled.
  * @param  handle Pointer to driver handle.
  * @return Number of AWD events since last call.
  */
uint32_t ADC_LL_TakeAwdEvents(AdcLl_Handle_t *handle);

/**
  * @brief  Polls whether a half-buffer is ready for consumption.
  * @param  handle   Pointer to driver handle.
  * @param  out_half Receives index 0 or 1 of the ready half.
  * @return true if new half is ready, false otherwise.
  */
bool ADC_LL_GetReadyHalf(AdcLl_Handle_t *handle, uint8_t *out_half);

/**
  * @brief  Computes min/max/average and millivolts over the specified half for single channel.
  * @param  handle  Pointer to driver handle.
  * @param  half    0 for first half, 1 for second half.
  * @param  vref_mv Reference voltage in millivolts (e.g. 3300).
  */
void ADC_LL_ComputeStats(AdcLl_Handle_t *handle, uint8_t half, uint32_t vref_mv);

/**
  * @brief  Computes min/max/average and millivolts for a specific channel in an interleaved sequence.
  * @param  handle          Pointer to driver handle.
  * @param  half            0 for first half, 1 for second half.
  * @param  channel_seq_idx Sequence index (0 .. num_channels-1).
  * @param  vref_mv         Reference voltage in millivolts.
  */
void ADC_LL_ComputeStatsChannel(AdcLl_Handle_t *handle, uint8_t half, uint32_t channel_seq_idx, uint32_t vref_mv);

/**
  * @brief ADC factory calibration data container.
  */
typedef struct
{
    uint16_t vrefint_cal;    /*!< Raw VREFINT calibration word (3.3V, 30°C) */
    uint16_t ts_cal1;        /*!< Raw TS_CAL1 calibration word (3.3V, 30°C) */
    uint16_t ts_cal2;        /*!< Raw TS_CAL2 calibration word (3.3V, 130°C) */
} AdcLl_CalibrationData_t;

#define ADC_LL_TEMPERATURE_ERROR   (-999L)

/**
  * @brief  Injects factory calibration data (especially needed on Cortex-M4 where ROM is unreachable).
  * @param  cal Pointer to calibration data structure.
  * @return true if calibration data is plausible, false otherwise.
  */
bool ADC_LL_InjectCalibration(const AdcLl_CalibrationData_t *cal);

/**
  * @brief  Calculates VDDA in millivolts from a raw VREFINT reading using factory calibration.
  * @param  vrefint_raw     Raw conversion code from channel 19 (VREFINT).
  * @param  resolution_bits ADC resolution bits (16, 14, 12, 10, 8; 0 defaults to 16).
  * @return VDDA in millivolts (nominal ~3300), or 0U if uncalibrated/error.
  */
uint32_t ADC_LL_CalculateVDDA(uint16_t vrefint_raw, uint8_t resolution_bits);

/**
  * @brief  Calculates junction temperature in degrees Celsius from a raw temperature sensor reading.
  * @param  temp_raw        Raw conversion code from channel 18 (TEMPSENSOR).
  * @param  vdda_mv         Actual VDDA supply voltage in mV.
  * @param  resolution_bits ADC resolution bits (16, 14, 12, 10, 8; 0 defaults to 16).
  * @return Temperature in degrees Celsius, or ADC_LL_TEMPERATURE_ERROR if uncalibrated/error.
  */
int32_t ADC_LL_CalculateTemperature(uint16_t temp_raw, uint32_t vdda_mv, uint8_t resolution_bits);

#define ADC_LL_CalculateVDDA_16B(raw)         ADC_LL_CalculateVDDA((raw), 16U)
#define ADC_LL_CalculateTemperature_16B(r, v) ADC_LL_CalculateTemperature((r), (v), 16U)

#ifdef __cplusplus
}
#endif

#endif /* ADC_LL_H */

