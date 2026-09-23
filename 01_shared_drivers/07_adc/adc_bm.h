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

/* Optimized resolutions, available on Rev V silicon only (rejected on Rev Y).
 * These are logical tags, translated internally by ADC_BM_Init. */
#define ADC_BM_RESOLUTION_14B_OPT     0x00000014UL
#define ADC_BM_RESOLUTION_12B_OPT     0x00000012UL

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

/* ---- Clock Modes (ADCx_CCR.CKMODE) --------------------------------------- */
typedef enum
{
    ADC_BM_CLOCK_ASYNC   = 0x00000000UL,                          /*!< adc_ker_ck (default)   */
    ADC_BM_CLOCK_SYNC_1  = ADC_CCR_CKMODE_0,                      /*!< adc_sclk / 1           */
    ADC_BM_CLOCK_SYNC_2  = ADC_CCR_CKMODE_1,                      /*!< adc_sclk / 2           */
    ADC_BM_CLOCK_SYNC_4  = (ADC_CCR_CKMODE_0 | ADC_CCR_CKMODE_1)  /*!< adc_sclk / 4           */
} AdcBm_ClockMode_t;

/* ---- Asynchronous Prescaler (ADCx_CCR.PRESC) ----------------------------- */
typedef enum
{
    ADC_BM_PRESC_DIV1   = 0x00000000UL,    /*!< /1  (default, 0 = legacy) */
    ADC_BM_PRESC_DIV2   = 0x00000001UL,    /*!< /2  */
    ADC_BM_PRESC_DIV4   = 0x00000002UL,    /*!< /4  */
    ADC_BM_PRESC_DIV6   = 0x00000003UL,    /*!< /6  */
    ADC_BM_PRESC_DIV8   = 0x00000004UL,    /*!< /8  */
    ADC_BM_PRESC_DIV10  = 0x00000005UL,    /*!< /10 */
    ADC_BM_PRESC_DIV12  = 0x00000006UL,    /*!< /12 */
    ADC_BM_PRESC_DIV16  = 0x00000007UL,    /*!< /16 */
    ADC_BM_PRESC_DIV32  = 0x00000008UL,    /*!< /32 */
    ADC_BM_PRESC_DIV64  = 0x00000009UL,    /*!< /64 */
    ADC_BM_PRESC_DIV128 = 0x0000000AUL,    /*!< /128 */
    ADC_BM_PRESC_DIV256 = 0x0000000BUL     /*!< /256 */
} AdcBm_Prescaler_t;

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

    /* --- New fields (v1.1) ---
     * Zero-initialized = exact legacy behavior.
     * kernel_clock_hz = 0 -> assume 25 MHz (PLAN.md §5.2).
     * clock_mode = 0 -> ADC_BM_CLOCK_ASYNC.
     * prescaler = 0 -> ADC_BM_PRESC_DIV1.
     * analog_pin_port = NULL -> application configures the pin itself.
     */
    uint32_t            kernel_clock_hz;   /*!< ADC kernel clock in Hz. 0 = legacy 25 MHz.     */
    AdcBm_ClockMode_t   clock_mode;        /*!< ADCx_CCR.CKMODE. 0 = async (legacy).           */
    AdcBm_Prescaler_t   prescaler;         /*!< ADCx_CCR.PRESC. 0 = /1 (legacy).               */
    GPIO_TypeDef        *analog_pin_port;   /*!< GPIO port for analog pin. NULL = skip.         */
    uint32_t             analog_pin_idx;    /*!< Pin index 0..15. Ignored if port is NULL.      */
} AdcBm_Config_t;

/* ---- Internal ADC Channels (ADC3 only) ----------------------------------- */
#define ADC_BM_CHANNEL_VBAT           17U
#define ADC_BM_CHANNEL_TEMPSENSOR     18U
#define ADC_BM_CHANNEL_VREFINT        19U

/* ---- Oversampling Ratios (ratio N: 2..1024, written to CFGR2 as (N-1)) ---- */
#define ADC_BM_OVERSAMPLING_2X        2U
#define ADC_BM_OVERSAMPLING_4X        4U
#define ADC_BM_OVERSAMPLING_8X        8U
#define ADC_BM_OVERSAMPLING_16X       16U
#define ADC_BM_OVERSAMPLING_32X       32U
#define ADC_BM_OVERSAMPLING_64X       64U
#define ADC_BM_OVERSAMPLING_128X      128U
#define ADC_BM_OVERSAMPLING_256X      256U
#define ADC_BM_OVERSAMPLING_512X      512U
#define ADC_BM_OVERSAMPLING_1024X     1024U

/* ---- Oversampling Shifts (number of bits to right-shift) ----------------- */
#define ADC_BM_OVERSAMPLING_SHIFT_NONE 0U
#define ADC_BM_OVERSAMPLING_SHIFT_1B   1U
#define ADC_BM_OVERSAMPLING_SHIFT_2B   2U
#define ADC_BM_OVERSAMPLING_SHIFT_3B   3U
#define ADC_BM_OVERSAMPLING_SHIFT_4B   4U
#define ADC_BM_OVERSAMPLING_SHIFT_5B   5U
#define ADC_BM_OVERSAMPLING_SHIFT_6B   6U
#define ADC_BM_OVERSAMPLING_SHIFT_7B   7U
#define ADC_BM_OVERSAMPLING_SHIFT_8B   8U

/**
  * @brief Single channel descriptor within a regular conversion sequence.
  */
typedef struct
{
    uint32_t channel;           /*!< Channel decimal number (0..19) */
    uint32_t sampling_time;     /*!< ADC_BM_SAMPLETIME_*            */
} AdcBm_ChannelConfig_t;

/**
  * @brief Extended multi-channel ADC configuration (F3, F4, F5, F6).
  */
typedef struct
{
    ADC_TypeDef                 *adc_instance;      /*!< e.g. ADC1, ADC2, ADC3           */
    const AdcBm_ChannelConfig_t *channels;          /*!< Array of channels in sequence   */
    uint32_t                     num_channels;      /*!< Number of channels (1..16)      */
    uint32_t                     resolution;        /*!< ADC_BM_RESOLUTION_*            */
    uint32_t                     trigger_source;    /*!< ADC_BM_TRIG_* (EXTSEL value)    */
    uint32_t                     trigger_edge;      /*!< ADC_BM_TRIGGER_*                */
    AdcBm_Boost_t                boost_mode;        /*!< Boost mode for ker_ck frequency */
    AdcBm_DmaMode_t              dma_mode;          /*!< ADC_BM_DMA_*                    */
    bool                         dma_circular;      /*!< Circular mode                   */
    uint16_t                    *buffer;            /*!< Pointer to DMA buffer           */
    uint32_t                     buffer_length;     /*!< Total samples in buffer         */
    uint32_t                     kernel_clock_hz;   /*!< ADC kernel clock in Hz. 0 = 25M */
    AdcBm_ClockMode_t            clock_mode;        /*!< ADCx_CCR.CKMODE                 */
    AdcBm_Prescaler_t            prescaler;         /*!< ADCx_CCR.PRESC                  */
    GPIO_TypeDef                *analog_pin_port;   /*!< GPIO port for first analog pin  */
    uint32_t                     analog_pin_idx;    /*!< Pin index 0..15                 */

    /* F6 Oversampling */
    bool                         oversampling_enable;
    uint32_t                     oversampling_ratio; /*!< ADC_BM_OVERSAMPLING_*X (2..1024) */
    uint32_t                     oversampling_shift; /*!< ADC_BM_OVERSAMPLING_SHIFT_* (0..8)*/

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
} AdcBm_ConfigEx_t;

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
} AdcBm_Stats_t;

/**
  * @brief ADC Bare-Metal driver handle.
  */
typedef struct
{
    AdcBm_Config_t          config;         /*!< Legacy configuration copy */
    volatile uint8_t        ready_half;     /*!< 0: first half, 1: second half   */
    volatile uint8_t        has_new_data;   /*!< Set by ISR, cleared by consumer */
    volatile AdcBm_Stats_t  stats;

    /* --- New fields (v1.1) appended at the end to satisfy Rule 1 --- */
    AdcBm_ConfigEx_t        config_ex;      /*!< Extended multi-channel configuration */
    AdcBm_ChannelConfig_t   single_channel; /*!< Storage for legacy single-channel */
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
  * @brief  Extended initialization supporting multi-channel sequences, oversampling and AWD.
  * @param  handle Pointer to driver handle.
  * @param  config Pointer to extended configuration.
  * @return true on success, false on invalid parameters.
  */
bool ADC_BM_InitEx(AdcBm_Handle_t *handle, const AdcBm_ConfigEx_t *config);

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
  * @brief  Single software-triggered conversion on specified channel (polling mode).
  * @note   PRECONDITION: The ADC must NOT have a conversion in progress (ADSTART=0)
  *         and must NOT be in DMA mode (DMNGT=0). Returns false if active.
  * @param  adc           Pointer to ADC instance.
  * @param  channel       Channel number (0..19).
  * @param  sampling_time Sampling time constant (ADC_BM_SAMPLETIME_*).
  * @param  out_raw       Receives the raw 16-bit conversion result.
  * @param  spin_limit    Maximum loop iterations before timing out.
  * @return true on success, false on timeout, busy or parameter error.
  */
bool ADC_BM_ReadSingle(ADC_TypeDef *adc, uint32_t channel, uint32_t sampling_time, uint16_t *out_raw, uint32_t spin_limit);

/**
  * @brief  DMA IRQ dispatcher for ADC double-buffering.
  * @param  handle Pointer to driver handle.
  * @param  stream Pointer to associated DMA stream.
  */
void ADC_BM_IRQHandler_DMA(AdcBm_Handle_t *handle, DMA_Stream_TypeDef *stream);

/**
  * @brief  ADC global IRQ handler (checks overrun and analog watchdog flags).
  * @param  handle Pointer to driver handle.
  */
void ADC_BM_IRQHandler_ADC(AdcBm_Handle_t *handle);

/**
  * @brief  Reads and atomically clears accumulated analog watchdog event counter.
  *         Re-enables AWD1 interrupt if AWD was enabled.
  * @param  handle Pointer to driver handle.
  * @return Number of AWD events since last call.
  */
uint32_t ADC_BM_TakeAwdEvents(AdcBm_Handle_t *handle);

/**
  * @brief  Polls whether a half-buffer is ready for consumption.
  * @param  handle   Pointer to driver handle.
  * @param  out_half Receives index 0 or 1 of the ready half.
  * @return true if new half is ready, false otherwise.
  */
bool ADC_BM_GetReadyHalf(AdcBm_Handle_t *handle, uint8_t *out_half);

/**
  * @brief  Computes min/max/average and millivolts over the specified half for single channel.
  * @param  handle  Pointer to driver handle.
  * @param  half    0 for first half, 1 for second half.
  * @param  vref_mv Reference voltage in millivolts (e.g. 3300).
  */
void ADC_BM_ComputeStats(AdcBm_Handle_t *handle, uint8_t half, uint32_t vref_mv);

/**
  * @brief  Computes min/max/average and millivolts for a specific channel in an interleaved sequence.
  * @param  handle          Pointer to driver handle.
  * @param  half            0 for first half, 1 for second half.
  * @param  channel_seq_idx Sequence index (0 .. num_channels-1).
  * @param  vref_mv         Reference voltage in millivolts.
  */
void ADC_BM_ComputeStatsChannel(AdcBm_Handle_t *handle, uint8_t half, uint32_t channel_seq_idx, uint32_t vref_mv);

/**
  * @brief  Calculates VDDA in millivolts from a raw VREFINT reading using factory calibration.
  * @param  vrefint_raw     Raw conversion code from channel 19 (VREFINT).
  * @param  resolution_bits ADC resolution bits (16, 14, 12, 10, 8; 0 defaults to 16).
  * @return VDDA in millivolts (nominal ~3300).
  */
uint32_t ADC_BM_CalculateVDDA(uint16_t vrefint_raw, uint8_t resolution_bits);

/**
  * @brief  Calculates junction temperature in degrees Celsius from raw temperature sensor reading.
  * @param  temp_raw        Raw conversion code from channel 18 (TEMPSENSOR).
  * @param  vdda_mv         Actual VDDA supply voltage in mV.
  * @param  resolution_bits ADC resolution bits (16, 14, 12, 10, 8; 0 defaults to 16).
  * @return Temperature in degrees Celsius.
  */
float ADC_BM_CalculateTemperature(uint16_t temp_raw, uint32_t vdda_mv, uint8_t resolution_bits);

/* Backward compatibility aliases */
#define ADC_CalculateVDDA(raw)              ADC_BM_CalculateVDDA((raw), 16U)
#define ADC_CalculateTemperature(raw, vdda) ((int32_t)ADC_BM_CalculateTemperature((raw), (vdda), 16U))

#ifdef __cplusplus
}
#endif

#endif /* ADC_BM_H */

