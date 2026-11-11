/**
  ******************************************************************************
  * @file    dsp_wavelet.h
  * @brief   4-Level Daubechies-4 (db4) Wavelet Packet Transform (WPT)
  *          16-band time-frequency analyzer and Level-1 HF transient detector.
  *
  * Decomposes each 4096-sample ADC block into a full binary tree of depth
  * J = 4 using orthogonal 8-tap Daubechies-4 (db4) QMF filters, reorders the
  * 16 terminal sub-bands into monotonic frequency order via the Gray-code
  * (Paley) permutation, and computes per-band energy percentage, RMS voltage,
  * spectral energy density, normalized Shannon entropy, and high-frequency
  * crest-factor glitch detection.
  ******************************************************************************
  */

#ifndef DSP_WAVELET_H
#define DSP_WAVELET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "board_def.h"

/**
  * @brief  Complete output of one 4-Level db4 Wavelet Packet Transform cycle.
  */
typedef struct
{
    uint32_t block_id;                      /* Processed block sequence ID    */
    uint32_t fs_hz;                         /* Sampling rate (Hz)             */
    float    band_width_hz;                 /* Sub-band width: fs_hz / 32     */
    float    total_energy;                  /* Parseval total wavelet energy  */
    float    energy_pct[WPT_NUM_BANDS];     /* Energy share per band (0..100%)*/
    float    rms_mv[WPT_NUM_BANDS];         /* RMS voltage per band (mV)      */
    float    energy_density[WPT_NUM_BANDS]; /* Energy / Hz (mV^2 / Hz)        */
    float    entropy_shannon;               /* Shannon wavelet entropy (nats) */
    float    entropy_norm;                  /* Normalized entropy (0.0..1.0)  */
    float    crest_factor_hf;               /* Level-1 HF detail crest factor */
    float    transient_mag_mv;              /* Peak Level-1 HF detail (mV)    */
    uint16_t transient_sample;              /* Sample index (0..4095) of peak */
    float    transient_time_us;             /* Time offset in block (us)      */
    uint8_t  transient_detected;            /* 1 if glitch detected in block  */
    uint32_t transient_count;               /* Cumulative glitch count        */
} WPT_Result_t;

/**
  * @brief  Initialize Wavelet Packet Transform state and reset cumulative
  *         transient counters.
  */
void DSP_Wavelet_Init(void);

/**
  * @brief  Execute 4-Level Daubechies-4 (db4) Wavelet Packet Decomposition
  *         and Level-1 HF transient detection on a 4096-sample ADC block.
  * @param  raw_adc       Pointer to 4096 16-bit unsigned ADC samples.
  * @param  fs_hz         Effective sampling frequency in Hz.
  * @param  block_id      Monotonic block sequence identifier.
  * @param  dc_offset_mv  DC mean voltage (mV) computed by `DSP_Engine`.
  * @param  out           Output structure receiving 16-band WPT metrics.
  */
void DSP_Wavelet_ProcessBlock(const uint16_t *raw_adc,
                              uint32_t fs_hz,
                              uint32_t block_id,
                              float dc_offset_mv,
                              WPT_Result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DSP_WAVELET_H */
