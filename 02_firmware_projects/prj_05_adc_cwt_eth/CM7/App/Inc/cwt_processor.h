/**
  ******************************************************************************
  * @file    cwt_processor.h
  * @brief   Continuous Wavelet Transform (CWT) DSP engine on Cortex-M7.
  *
  *          Computes a 2D Complex Morlet (Gabor) Wavelet Scalogram over
  *          512-sample acquisition windows captured by CPU2 (Cortex-M4) at
  *          500 kSPS on PB1 (A3, 1.5 V DC offset).
  *
  *          Architecture:
  *            - Removes the 1.5 V DC offset (`ADC_NOMINAL_OFFSET_RAW = 29789`)
  *              and optional residual window DC drift before wavelet projection.
  *            - Scales samples to physical AC millivolts [-1500.0, +1800.0] mV.
  *            - Precomputes normalized Analytic Morlet daughter wavelets in the
  *              frequency domain during `CWT_Init()` (zero runtime trig/exp).
  *            - Executes 512-point radix-2 FFT + frequency-domain wavelet
  *              multiplication + 512-point IFFT + envelope magnitude extraction
  *              with inner loops in ITCM (`.itcm_text`) and bulk buffers in
  *              32-byte aligned AXI SRAM (`.axi_ram`).
  *            - Quantizes the 24 x 512 scalogram to `uint16_t` with a per-frame
  *              `magnitude_scale` float multiplier.
  ******************************************************************************
  */

#ifndef CWT_PROCESSOR_H
#define CWT_PROCESSOR_H

#include <stdbool.h>
#include <stdint.h>
#include "shared_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CWT Configuration Constants --------------------------------------- */
#define CWT_WINDOW_SAMPLES          512U
#define CWT_FFT_LOG2_N              9U
#define CWT_NUM_SCALES              24U
#define CWT_SCALES_PER_CHUNK        1U   /* 1 scale * 512 * 2B = 1024B payload + 64B hdr = 1088B <= 1400B */

/* Frequency band covered by the 24 log-spaced Complex Morlet scales (10 Hz .. 4,000 Hz)
 * Tailored for 4-flute cutting tools up to 10,000 RPM:
 *   - Spindle 1X rotation (600 .. 10,000 RPM): 10.0 Hz .. 166.7 Hz
 *   - 4-Flute Tooth Passing Frequency (TPF):   40.0 Hz .. 666.7 Hz
 *   - TPF harmonics (2X..4X) & Chatter modes:  500.0 Hz .. 4,000.0 Hz
 */
#define CWT_FREQ_MIN_HZ             10.0f
#define CWT_FREQ_MAX_HZ             4000.0f
#define CWT_MORLET_OMEGA0           6.0f

/* Dual-rate decimation for very low-frequency scales (< 120 Hz) so 10 Hz .. 120 Hz
 * scales are resolved over a 307.2 ms horizon (fs_low = 10 kHz / 8 = 1.25 kHz,
 * df_low = 2.44 Hz) without FFT bin underflow, while 120 Hz .. 4,000 Hz run at
 * full 10 kSPS resolution across the 51.2 ms window. */
#define CWT_LOW_DECIMATION          8U
#define CWT_LOW_SAMPLES_PER_WIN     (CWT_WINDOW_SAMPLES / CWT_LOW_DECIMATION) /* 64 samples/window */
#define CWT_LOW_BAND_MAX_HZ         120.0f

_Static_assert(CWT_WINDOW_SAMPLES == SHARED_ADC_WAVEFORM_SAMPLES,
               "CWT_WINDOW_SAMPLES must equal SHARED_ADC_WAVEFORM_SAMPLES (512)");

/* ---- 1.5 V DC Offset Removal Mode -------------------------------------- */
typedef enum
{
    CWT_OFFSET_MODE_AUTO_MEAN = 0, /*!< Subtract 1.5V nominal (29789) + residual window DC drift */
    CWT_OFFSET_MODE_FIXED_1V5 = 1  /*!< Subtract strictly fixed 1.5V nominal (29789 raw counts)  */
} CwtOffsetMode_t;

/* ---- Complete CWT Frame Snapshot (Double-Buffered in AXI SRAM) --------- */
typedef struct __attribute__((aligned(32)))
{
    uint32_t frame_seq;          /*!< Matching g_shared.adc.waveform_seq                */
    uint32_t sampling_rate_hz;   /*!< 500,000 Hz                                        */
    uint16_t num_scales;         /*!< CWT_NUM_SCALES (24)                               */
    uint16_t num_time_bins;      /*!< CWT_WINDOW_SAMPLES (512)                          */
    uint16_t nominal_offset_mv;  /*!< 1500 mV                                           */
    uint16_t measured_dc_mv;     /*!< Measured DC bias in mV (~1500 mV)                 */
    int16_t  dc_offset_error_mv; /*!< measured_dc_mv - 1500 mV                          */
    uint16_t vpp_mv;             /*!< Peak-to-peak AC swing in mV                       */
    int16_t  ac_min_mv;          /*!< Min signed AC voltage relative to 1.5V            */
    int16_t  ac_max_mv;          /*!< Max signed AC voltage relative to 1.5V            */
    uint16_t min_raw;            /*!< Minimum raw 16-bit code                           */
    uint16_t max_raw;            /*!< Maximum raw 16-bit code                           */
    uint16_t average_raw;        /*!< Mean raw 16-bit code                              */
    uint16_t latest_raw;         /*!< Latest raw 16-bit code                            */
    uint16_t channel;            /*!< ADC channel (5 for PB1)                           */
    uint16_t offset_mode;        /*!< Active CwtOffsetMode_t                            */
    uint32_t sample_count;       /*!< Total samples acquired by CM4                     */
    uint32_t dma_tc_count;       /*!< DMA Transfer-Complete count                       */
    uint32_t dma_ht_count;       /*!< DMA Half-Transfer count                           */
    uint32_t dma_error_count;    /*!< DMA error count                                   */
    uint32_t half_overrun_count; /*!< Consumer half-buffer overrun count                */
    uint32_t adc_overrun_count;  /*!< Hardware ADC OVR count                            */
    uint32_t cwt_exec_cycles;    /*!< DWT cycles taken by 24-scale CWT on CM7 (400 MHz) */
    uint32_t cwt_exec_us;        /*!< Microseconds taken by 24-scale CWT on CM7         */
    uint32_t cwt_frames_count;   /*!< Total CWT scalograms computed by CM7              */
    uint32_t m4_heartbeat;       /*!< Snapshot of CM4 heartbeat                         */
    uint32_t timestamp_ms;       /*!< CM4 acquisition timestamp (ms)                    */
    float    magnitude_scale;    /*!< Multiplier: mV = scalogram[s][t] * magnitude_scale*/
    float    max_magnitude_mv;   /*!< Peak wavelet coefficient magnitude in frame (mV)  */
    uint16_t peak_scale_idx;     /*!< Scale index (0..23) of peak wavelet energy        */
    uint16_t peak_time_idx;      /*!< Time bin (0..511) of peak wavelet energy          */
    float    peak_freq_hz;       /*!< Center frequency (Hz) of peak wavelet energy      */
    float    f_center_hz[CWT_NUM_SCALES];                       /*!< Scale center frequencies (Hz) */
    uint16_t raw_waveform[CWT_WINDOW_SAMPLES];                  /*!< Raw 16-bit ADC samples        */
    uint16_t scalogram[CWT_NUM_SCALES][CWT_WINDOW_SAMPLES];     /*!< Quantized 2D CWT magnitude    */
} CwtFrameResult_t;

/**
  * @brief  Precomputes radix-2 FFT twiddle tables and normalized Complex Morlet
  *         daughter wavelets across all CWT_NUM_SCALES scales.
  */
void CWT_Init(void);

/**
  * @brief  FreeRTOS task entry point (`vCwtProcessTask`) that continuously
  *         acquires new 512-sample windows from SRAM4, removes the 1.5 V DC
  *         offset, computes the 2D Morlet CWT scalogram, and publishes it
  *         to the double-buffered result slot.
  */
void vCwtProcessTask(void *argument);

/**
  * @brief  Copies the most recently completed CWT frame snapshot into `dest`.
  * @param  dest Pointer to caller-allocated `CwtFrameResult_t` (e.g. in `.axi_ram`).
  * @return true if a valid CWT frame was copied, false if no frame is ready yet.
  */
bool CWT_GetLatestFrame(CwtFrameResult_t *dest);

/**
  * @brief  Sets the 1.5 V DC offset removal mode (`AUTO_MEAN` vs `FIXED_1V5`).
  */
void CWT_SetOffsetMode(CwtOffsetMode_t mode);

/**
  * @brief  Returns the currently active 1.5 V DC offset removal mode.
  */
CwtOffsetMode_t CWT_GetOffsetMode(void);

/**
  * @brief  Returns pointer to the precomputed array of scale center frequencies (Hz).
  */
const float *CWT_GetCenterFrequencies(void);

#ifdef __cplusplus
}
#endif

#endif /* CWT_PROCESSOR_H */
