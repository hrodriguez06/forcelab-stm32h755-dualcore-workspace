/**
  ******************************************************************************
  * @file    dsp_engine.c
  * @brief   4096-point CMSIS-DSP Real FFT spectral analysis engine on CM7.
  *
  * Pipeline:
  *   1. Scan 4096 raw 16-bit samples to compute DC mean (`mean_code`),
  *      `dc_offset_mv`, and time-domain `v_pp_mv`.
  *   2. Subtract DC mean, scale to millivolts (`3300 mV / 65535`), and multiply
  *      by the precomputed 4-term Blackman-Harris window ($a_0 = 0.35875$,
  *      $a_1 = 0.48829$, $a_2 = 0.14128$, $a_3 = 0.01168$, coherent gain = $a_0$,
  *      sidelobe floor = $-92\text{ dB}$).
  *   3. Execute 4096-point real FFT (`arm_rfft_fast_f32`) + complex magnitude
  *      (`arm_cmplx_mag_f32`) in cached D1 AXI SRAM (`.axi_ram`).
  *   4. Normalize single-sided amplitudes by $2 / (a_0 \cdot N)$.
  *   5. Estimate noise floor across bins $2..2047$, locate peak fundamental $f_0$
  *      with parabolic 3-point sub-bin interpolation, and extract harmonics
  *      $H_1..H_{10}$, SNR (dB), and THD (%).
  ******************************************************************************
  */

#include "dsp_engine.h"
#include "arm_math.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 4-term Blackman-Harris coherent gain (a0) */
#define BH4_COHERENT_GAIN           0.35875f
#define ADC_FULL_SCALE_CODES        65535.0f
#define ADC_VREF_MV_F               3300.0f

/* Working buffers in cached D1 AXI SRAM (0x24000000, 32-byte aligned) */
__attribute__((section(".axi_ram"), aligned(32)))
static float s_window[DSP_BLOCK_SIZE];

__attribute__((section(".axi_ram"), aligned(32)))
static float s_fft_in[DSP_BLOCK_SIZE];

__attribute__((section(".axi_ram"), aligned(32)))
static float s_fft_out[DSP_BLOCK_SIZE];

static arm_rfft_fast_instance_f32 s_rfft;
static uint8_t s_initialized = 0U;

int DSP_Engine_Init(void)
{
    const float a0 = 0.35875f;
    const float a1 = 0.48829f;
    const float a2 = 0.14128f;
    const float a3 = 0.01168f;
    const float inv_n_minus_1 = 1.0f / (float)(DSP_BLOCK_SIZE - 1U);

    for (uint32_t n = 0U; n < DSP_BLOCK_SIZE; n++)
    {
        float phase = 2.0f * (float)M_PI * (float)n * inv_n_minus_1;
        s_window[n] = a0
                    - a1 * cosf(phase)
                    + a2 * cosf(2.0f * phase)
                    - a3 * cosf(3.0f * phase);
    }

    if (arm_rfft_fast_init_f32(&s_rfft, DSP_BLOCK_SIZE) != ARM_MATH_SUCCESS)
    {
        return -1;
    }

    s_initialized = 1U;
    return 0;
}

void DSP_Engine_ProcessBlock(const uint16_t *raw_adc,
                             uint32_t fs_hz,
                             uint32_t block_id,
                             DSP_Result_t *out)
{
    if ((raw_adc == NULL) || (out == NULL))
    {
        return;
    }

    if (s_initialized == 0U)
    {
        if (DSP_Engine_Init() != 0)
        {
            return;
        }
    }

    memset(out, 0, sizeof(*out));
    out->block_id = block_id;
    out->fs_hz    = (fs_hz > 0U) ? fs_hz : BOARD_ADC_SAMPLING_RATE_HZ;
    out->df_hz    = (float)out->fs_hz / (float)DSP_BLOCK_SIZE;

    /* 1. Compute DC mean and peak-to-peak range from the 4096 raw samples */
    uint64_t sum_raw = 0ULL;
    uint16_t min_raw = 0xFFFFU;
    uint16_t max_raw = 0x0000U;

    for (uint32_t i = 0U; i < DSP_BLOCK_SIZE; i++)
    {
        uint16_t v = raw_adc[i];
        sum_raw += v;
        if (v < min_raw) { min_raw = v; }
        if (v > max_raw) { max_raw = v; }
    }

    float mean_code   = (float)sum_raw / (float)DSP_BLOCK_SIZE;
    float code_to_mv  = ADC_VREF_MV_F / ADC_FULL_SCALE_CODES;
    out->dc_offset_mv = mean_code * code_to_mv;
    out->v_pp_mv      = (float)(max_raw - min_raw) * code_to_mv;

    /* 2. Remove DC offset, convert to mV, and apply 4-term Blackman-Harris window */
    for (uint32_t i = 0U; i < DSP_BLOCK_SIZE; i++)
    {
        float mv_ac = ((float)raw_adc[i] - mean_code) * code_to_mv;
        s_fft_in[i] = mv_ac * s_window[i];
    }

    /* 3. Execute 4096-point Real FFT */
    arm_rfft_fast_f32(&s_rfft, s_fft_in, s_fft_out, 0);

    /*
     * CMSIS-DSP `arm_rfft_fast_f32` packs bin 0 (DC) in `s_fft_out[0]` and
     * bin N/2 (Nyquist) in `s_fft_out[1]`. Bins 1..(N/2 - 1) are interleaved
     * complex pairs. Compute magnitudes for all N/2 = 2048 complex pairs, then
     * fix bin 0 cleanly.
     */
    float dc_re = fabsf(s_fft_out[0]);
    s_fft_out[1] = 0.0f;

    arm_cmplx_mag_f32(s_fft_out, out->mag_mv, DSP_NUM_BINS);

    /* 4. Normalize single-sided amplitude spectrum for coherent gain */
    float norm = 2.0f / (BH4_COHERENT_GAIN * (float)DSP_BLOCK_SIZE);
    out->mag_mv[0] = dc_re / (BH4_COHERENT_GAIN * (float)DSP_BLOCK_SIZE);
    for (uint32_t k = 1U; k < DSP_NUM_BINS; k++)
    {
        out->mag_mv[k] *= norm;
    }

    /* 5. Locate dominant peak (excluding DC leakage bins 0..4) */
    uint16_t peak_bin = 5U;
    float    peak_mag = out->mag_mv[5];
    for (uint16_t k = 6U; k < (DSP_NUM_BINS - 1U); k++)
    {
        if (out->mag_mv[k] > peak_mag)
        {
            peak_mag = out->mag_mv[k];
            peak_bin = k;
        }
    }

    /* 6. Estimate RMS noise floor excluding a +/-4 bin guard around peak_bin */
    double   noise_energy_sum = 0.0;
    uint32_t noise_bins       = 0U;
    for (uint16_t k = 3U; k < (DSP_NUM_BINS - 1U); k++)
    {
        int32_t dist = (int32_t)k - (int32_t)peak_bin;
        if ((dist >= -4) && (dist <= 4))
        {
            continue;
        }
        noise_energy_sum += (double)out->mag_mv[k] * (double)out->mag_mv[k];
        noise_bins++;
    }

    float noise_floor_mv = (noise_bins > 0U)
        ? sqrtf((float)(noise_energy_sum / (double)noise_bins))
        : 0.1f;
    if (noise_floor_mv < 0.05f)
    {
        noise_floor_mv = 0.05f;
    }
    out->noise_floor_mv = noise_floor_mv;

    /* 7. Validate fundamental tone above noise threshold */
    if ((peak_mag < (3.0f * noise_floor_mv)) || (peak_mag < 3.0f))
    {
        out->f0_bin    = 0U;
        out->f0_hz     = 0.0f;
        out->f0_mag_mv = 0.0f;
        out->snr_db    = 0.0f;
        out->thd_pct   = 0.0f;
        return;
    }

    /* 8. Parabolic 3-point sub-bin interpolation around `peak_bin` */
    float alpha = out->mag_mv[peak_bin - 1U];
    float beta  = out->mag_mv[peak_bin];
    float gamma = out->mag_mv[peak_bin + 1U];
    float denom = alpha - (2.0f * beta) + gamma;
    float delta = 0.0f;
    float interp_mag = beta;

    if (fabsf(denom) > 1e-6f)
    {
        delta = 0.5f * (alpha - gamma) / denom;
        if (delta > 0.5f)  { delta = 0.5f; }
        if (delta < -0.5f) { delta = -0.5f; }
        interp_mag = beta - 0.25f * (alpha - gamma) * delta;
    }

    float exact_bin = (float)peak_bin + delta;
    out->f0_bin     = peak_bin;
    out->f0_hz      = exact_bin * out->df_hz;
    out->f0_mag_mv  = interp_mag;

    /* 9. Extract harmonics H1..H10 (+/-2 bin local search around m * exact_bin) */
    out->harm_bin[0]    = peak_bin;
    out->harm_mag_mv[0] = interp_mag;

    float harm_sq_sum = 0.0f;
    for (uint32_t h = 2U; h <= DSP_MAX_HARMONICS; h++)
    {
        uint32_t center_bin = (uint32_t)(exact_bin * (float)h + 0.5f);
        if (center_bin >= (DSP_NUM_BINS - 2U))
        {
            break;
        }

        uint16_t best_bin = (uint16_t)center_bin;
        float    best_mag = out->mag_mv[center_bin];
        uint16_t lo = (center_bin > 2U) ? (uint16_t)(center_bin - 2U) : 2U;
        uint16_t hi = (uint16_t)(center_bin + 2U);
        if (hi >= DSP_NUM_BINS)
        {
            hi = (uint16_t)(DSP_NUM_BINS - 1U);
        }

        for (uint16_t b = lo; b <= hi; b++)
        {
            if (out->mag_mv[b] > best_mag)
            {
                best_mag = out->mag_mv[b];
                best_bin = b;
            }
        }

        out->harm_bin[h - 1U]    = best_bin;
        out->harm_mag_mv[h - 1U] = best_mag;
        harm_sq_sum += best_mag * best_mag;
    }

    /* 10. Compute THD (%) and SNR (dB) */
    if (interp_mag > 0.1f)
    {
        out->thd_pct = (sqrtf(harm_sq_sum) / interp_mag) * 100.0f;
        if (out->thd_pct > 999.9f)
        {
            out->thd_pct = 999.9f;
        }
    }

    /* Compare fundamental power against total non-harmonic in-band noise power */
    double total_noise_sq = 0.0;
    for (uint16_t k = 3U; k < (DSP_NUM_BINS - 1U); k++)
    {
        uint8_t is_harm = 0U;
        for (uint32_t h = 0U; h < DSP_MAX_HARMONICS; h++)
        {
            uint16_t hb = out->harm_bin[h];
            if ((hb > 0U) && (k >= (hb > 3U ? (hb - 3U) : 0U)) && (k <= (hb + 3U)))
            {
                is_harm = 1U;
                break;
            }
        }
        if (is_harm == 0U)
        {
            total_noise_sq += (double)out->mag_mv[k] * (double)out->mag_mv[k];
        }
    }

    /* Account for Blackman-Harris equivalent noise bandwidth (ENBW = 2.00 bins) */
    float noise_rms = sqrtf((float)(total_noise_sq / 2.0));
    if (noise_rms < 0.05f)
    {
        noise_rms = 0.05f;
    }

    out->snr_db = 20.0f * log10f(interp_mag / noise_rms);
}
