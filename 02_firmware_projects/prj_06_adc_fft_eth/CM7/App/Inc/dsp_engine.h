/**
  ******************************************************************************
  * @file    dsp_engine.h
  * @brief   4096-point CMSIS-DSP Real FFT spectral analysis engine on CM7
  *          with 4-term Blackman-Harris windowing, parabolic sub-bin peak
  *          interpolation, 10-harmonic extraction, SNR, and THD calculation.
  ******************************************************************************
  */

#ifndef DSP_ENGINE_H
#define DSP_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "board_def.h"

/**
  * @brief  Complete output of one 4096-point FFT spectral analysis cycle.
  */
typedef struct
{
    uint32_t block_id;                      /* Processed block sequence ID    */
    uint32_t fs_hz;                         /* Sampling rate (Hz)             */
    float    df_hz;                         /* Bin spacing: fs_hz / 4096      */
    float    dc_offset_mv;                  /* Mean DC voltage (mV)           */
    float    v_pp_mv;                       /* Peak-to-peak swing (mV)        */
    float    f0_hz;                         /* Parabolic-interpolated f0 (Hz) */
    float    f0_mag_mv;                     /* Fundamental amplitude (mV)     */
    uint16_t f0_bin;                        /* Peak bin index (0..2047)       */
    float    noise_floor_mv;                /* Estimated RMS noise floor (mV) */
    float    snr_db;                        /* Signal-to-Noise Ratio (dB)     */
    float    thd_pct;                       /* Total Harmonic Distortion (%)  */
    float    harm_mag_mv[DSP_MAX_HARMONICS];/* H1..H10 peak magnitudes (mV)   */
    uint16_t harm_bin[DSP_MAX_HARMONICS];   /* H1..H10 bin indices            */
    float    mag_mv[DSP_NUM_BINS];          /* 2048 single-sided bins (mV)    */
} DSP_Result_t;

/**
  * @brief  Precompute the 4096-point 4-term Blackman-Harris window in AXI SRAM
  *         and initialize the CMSIS-DSP `arm_rfft_fast_f32` instance.
  * @retval 0 on success, -1 if CMSIS-DSP initialization failed.
  */
int DSP_Engine_Init(void);

/**
  * @brief  Execute 4096-point Blackman-Harris windowed RFFT and extract
  *         spectral metrics (fundamental f0, H1..H10, noise floor, SNR, THD).
  * @param  raw_adc   Pointer to 4096 16-bit unsigned ADC samples.
  * @param  fs_hz     Effective sampling frequency in Hz.
  * @param  block_id  Monotonic block sequence identifier.
  * @param  out       Output structure receiving spectral metrics and 2048 bins.
  */
void DSP_Engine_ProcessBlock(const uint16_t *raw_adc,
                             uint32_t fs_hz,
                             uint32_t block_id,
                             DSP_Result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DSP_ENGINE_H */
