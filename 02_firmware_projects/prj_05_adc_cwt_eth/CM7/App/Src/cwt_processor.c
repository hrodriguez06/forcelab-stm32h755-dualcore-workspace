/**
  ******************************************************************************
  * @file    cwt_processor.c
  * @brief   Continuous Wavelet Transform (CWT) DSP implementation on Cortex-M7.
  *
  *          Mathematical Formulation:
  *            1. 1.5 V DC Offset Removal & Physical Scaling:
  *               The analog signal on PB1 (A3) has a nominal 1.5 V DC offset
  *               (`ADC_NOMINAL_OFFSET_RAW = 29789` at 16-bit / 3.3 V).
  *               - In `CWT_OFFSET_MODE_AUTO_MEAN` (default), we subtract the
  *                 1.5 V nominal bias AND any residual window DC drift
  *                 (`mean_raw - 29789`) so zero-frequency leakage into large
  *                 wavelet scales is completely eliminated.
  *               - In `CWT_OFFSET_MODE_FIXED_1V5`, we subtract strictly
  *                 `ADC_NOMINAL_OFFSET_RAW = 29789`.
  *               - Samples are scaled to signed millivolts:
  *                   x_mv[n] = (raw[n] - ref_raw) * (3300.0f / 65535.0f)
  *
  *            2. Analytic Complex Morlet (Gabor) Wavelet Filter Bank:
  *               Log-spaced center frequencies f_c[s] from 2 kHz to 200 kHz
  *               across S = 24 scales at f_s = 500 kHz, with central
  *               dimensionless frequency omega_0 = 6.0.
  *               For each scale s, the analytic Fourier multiplier (non-negative
  *               frequencies k = 1 .. N/2) is:
  *                   Psi_s[k] = 2.0 * exp( -0.5 * ( (f_k - f_c[s]) / sigma_f[s] )^2 )
  *               where sigma_f[s] = f_c[s] / omega_0.
  *               With the factor of 2.0 on positive frequencies and 0.0 on
  *               negative frequencies (analytic signal projection), a real
  *               sinusoid A * cos(2*pi*f_c*t) produces a constant CWT envelope
  *               magnitude |W(s, t)| = A (directly in mV peak!).
  *
  *            3. Fast FFT Convolution in ITCM + AXI SRAM:
  *               - 512-point radix-2 decimation-in-time FFT (`cwt_fft512_itcm`
  *                 placed in `.itcm_text` zero-wait-state RAM).
  *               - Multiply X[k] by real-valued Psi_s[k] for k = 1 .. N/2.
  *               - 512-point IFFT and envelope magnitude extraction.
  ******************************************************************************
  */

#include "cwt_processor.h"
#include "board_def.h"
#include "app_log.h"
#include "stm32h7xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Precomputed Tables & Working Buffers in AXI SRAM (`.axi_ram`) ----- */
typedef struct __attribute__((aligned(32)))
{
    /* Precomputed bit-reversal permutation table for N = 512 */
    uint16_t bitrev[CWT_WINDOW_SAMPLES];

    /* Precomputed twiddle factors for radix-2 FFT: cos and sin for k = 0 .. N/2-1 */
    float    twiddle_cos[CWT_WINDOW_SAMPLES / 2U];
    float    twiddle_sin[CWT_WINDOW_SAMPLES / 2U];

    /* Log-spaced center frequencies f_c[s] in Hz (100 Hz .. 100 kHz) */
    float    f_center_hz[CWT_NUM_SCALES];

    /* Flag indicating whether scale s uses the 32x-decimated low-frequency
     * horizon buffer (true for f_c < 3 kHz) or the full-rate 500 kSPS window */
    uint8_t  scale_is_low_rate[CWT_NUM_SCALES];

    /* Precomputed frequency-domain Analytic Morlet daughter wavelets:
     * Only k = 0 .. N/2 can be non-zero because Analytic Morlet is strictly
     * zero for negative frequencies (k = N/2+1 .. N-1). */
    float    psi_hat[CWT_NUM_SCALES][(CWT_WINDOW_SAMPLES / 2U) + 1U];

    /* Sliding 32x-decimated history buffer (384 chronological samples = 24.576 ms
     * + 128 mirrored right-pad samples = 512 FFT points @ 15.625 kSPS) */
    float    low_hist[CWT_WINDOW_SAMPLES];

    /* Working FFT buffers for vCwtProcessTask (full-rate & low-rate spectra) */
    float    sig_re[CWT_WINDOW_SAMPLES];
    float    sig_im[CWT_WINDOW_SAMPLES];
    float    low_re[CWT_WINDOW_SAMPLES];
    float    low_im[CWT_WINDOW_SAMPLES];
    float    work_re[CWT_WINDOW_SAMPLES];
    float    work_im[CWT_WINDOW_SAMPLES];

    /* Unquantized float scalogram buffer [24][512] in mV before uint16 quantization */
    float    mag_mv[CWT_NUM_SCALES][CWT_WINDOW_SAMPLES];

    /* Local snapshot of SharedAdcData_t copied under HSEM 4 */
    uint16_t raw_snap[CWT_WINDOW_SAMPLES];

    /* Double-buffered completed CWT frames for lock-free/low-latency reader access */
    CwtFrameResult_t frames[2];
} CwtWorkspace_t;

static CwtWorkspace_t s_cwt __attribute__((section(".axi_ram"), aligned(32)));

static volatile uint8_t         s_active_frame_idx = 0U;
static volatile bool            s_has_valid_frame  = false;
static volatile CwtOffsetMode_t s_offset_mode      = CWT_OFFSET_MODE_AUTO_MEAN;
static volatile uint32_t        s_cwt_frames_count = 0U;
static SemaphoreHandle_t        s_frame_mutex      = NULL;

/* ==========================================================================
 * Time-Critical Radix-2 512-Point FFT / IFFT in ITCM RAM (`.itcm_text`)
 * ========================================================================== */

/**
  * @brief  In-place 512-point complex Radix-2 Decimation-In-Time FFT / IFFT.
  *         Executed from zero-wait-state ITCM RAM (`0x00000000`).
  * @param  re      Real part array of length 512.
  * @param  im      Imaginary part array of length 512.
  * @param  inverse false for forward FFT, true for inverse IFFT (with 1/N scaling).
  */
static void __attribute__((section(".itcm_text")))
cwt_fft512_itcm(float * restrict re, float * restrict im, bool inverse)
{
    const uint16_t * restrict bitrev = s_cwt.bitrev;
    const float    * restrict tw_cos = s_cwt.twiddle_cos;
    const float    * restrict tw_sin = s_cwt.twiddle_sin;

    /* 1. Bit-reversal permutation */
    for (uint32_t i = 0U; i < CWT_WINDOW_SAMPLES; i++)
    {
        uint32_t j = bitrev[i];
        if (i < j)
        {
            float tr = re[i];
            float ti = im[i];
            re[i] = re[j];
            im[i] = im[j];
            re[j] = tr;
            im[j] = ti;
        }
    }

    /* 2. Butterfly stages: len = 2, 4, 8, ..., 512 */
    const float sign = inverse ? 1.0f : -1.0f;

    for (uint32_t half = 1U; half < CWT_WINDOW_SAMPLES; half <<= 1U)
    {
        uint32_t len  = half << 1U;
        uint32_t step = (CWT_WINDOW_SAMPLES / 2U) / half;

        for (uint32_t i = 0U; i < CWT_WINDOW_SAMPLES; i += len)
        {
            uint32_t k_tw = 0U;
            for (uint32_t j = 0U; j < half; j++)
            {
                float wr = tw_cos[k_tw];
                float wi = sign * tw_sin[k_tw];
                k_tw += step;

                uint32_t u = i + j;
                uint32_t v = u + half;

                float tr = wr * re[v] - wi * im[v];
                float ti = wr * im[v] + wi * re[v];

                re[v] = re[u] - tr;
                im[v] = im[u] - ti;
                re[u] = re[u] + tr;
                im[u] = im[u] + ti;
            }
        }
    }

    /* 3. Normalize by 1/N on IFFT */
    if (inverse)
    {
        const float inv_n = 1.0f / (float)CWT_WINDOW_SAMPLES;
        for (uint32_t i = 0U; i < CWT_WINDOW_SAMPLES; i++)
        {
            re[i] *= inv_n;
            im[i] *= inv_n;
        }
    }
}

/**
  * @brief  Computes one scale row of the CWT by multiplying the signal's
  *         analytic positive-frequency spectrum with the precomputed Morlet
  *         daughter filter, running IFFT, and extracting envelope magnitudes.
  */
static float __attribute__((section(".itcm_text")))
cwt_compute_scale_itcm(uint32_t scale_idx, uint16_t *out_peak_t)
{
    const uint8_t is_low = s_cwt.scale_is_low_rate[scale_idx];
    const float * restrict src_re = is_low ? s_cwt.low_re : s_cwt.sig_re;
    const float * restrict src_im = is_low ? s_cwt.low_im : s_cwt.sig_im;
    const float * restrict psi    = s_cwt.psi_hat[scale_idx];
    float       * restrict wr     = s_cwt.work_re;
    float       * restrict wi     = s_cwt.work_im;
    float       * restrict mag    = s_cwt.mag_mv[scale_idx];

    /* DC bin (k = 0) is zeroed because the Morlet wavelet is zero-mean */
    wr[0] = 0.0f;
    wi[0] = 0.0f;

    /* Positive frequencies k = 1 .. N/2 */
    const uint32_t half_n = CWT_WINDOW_SAMPLES / 2U;
    for (uint32_t k = 1U; k <= half_n; k++)
    {
        float g = psi[k];
        wr[k] = src_re[k] * g;
        wi[k] = src_im[k] * g;
    }

    /* Negative frequencies k = N/2 + 1 .. N - 1 are zero for analytic Morlet */
    for (uint32_t k = half_n + 1U; k < CWT_WINDOW_SAMPLES; k++)
    {
        wr[k] = 0.0f;
        wi[k] = 0.0f;
    }

    /* Inverse FFT to obtain complex time-domain wavelet coefficients W(s, n) */
    cwt_fft512_itcm(wr, wi, true);

    float    max_m  = 0.0f;
    uint16_t peak_t = 0U;

    if (!is_low)
    {
        /* Full-rate scale (3 kHz .. 100 kHz): direct 1:1 mapping for n = 0..511 */
        for (uint32_t n = 0U; n < CWT_WINDOW_SAMPLES; n++)
        {
            float r = wr[n];
            float m = wi[n];
            float a = sqrtf(r * r + m * m);
            mag[n]  = a;
            if (a > max_m)
            {
                max_m  = a;
                peak_t = (uint16_t)n;
            }
        }
    }
    else
    {
        /* Low-rate scale (100 Hz .. 3 kHz):
         * The current 512-sample window occupies decimated indices 368 .. 383
         * (16 samples @ 15.625 kSPS = 1.024 ms), cushioned on the right by
         * 128 symmetric mirror-padded samples (indices 384 .. 511).
         * Linearly interpolate the 16 envelope points across the 512 output bins. */
        const uint32_t base_idx = 384U - CWT_LOW_SAMPLES_PER_WIN; /* 368 */
        float env[CWT_LOW_SAMPLES_PER_WIN + 1U];

        for (uint32_t d = 0U; d <= CWT_LOW_SAMPLES_PER_WIN; d++)
        {
            uint32_t idx = base_idx + d;
            float r = wr[idx];
            float m = wi[idx];
            env[d] = sqrtf(r * r + m * m);
        }

        const float inv_dec = 1.0f / (float)CWT_LOW_DECIMATION;
        for (uint32_t d = 0U; d < CWT_LOW_SAMPLES_PER_WIN; d++)
        {
            float e0 = env[d];
            float de = (env[d + 1U] - e0) * inv_dec;
            uint32_t n_base = d * CWT_LOW_DECIMATION;
            for (uint32_t sub = 0U; sub < CWT_LOW_DECIMATION; sub++)
            {
                uint32_t n = n_base + sub;
                float a = e0 + de * (float)sub;
                mag[n] = a;
                if (a > max_m)
                {
                    max_m  = a;
                    peak_t = (uint16_t)n;
                }
            }
        }
    }

    *out_peak_t = peak_t;
    return max_m;
}

/* ==========================================================================
 * Initialization & Filter Bank Precomputation
 * ========================================================================== */

void CWT_Init(void)
{
    memset(&s_cwt, 0, sizeof(s_cwt));

    if (s_frame_mutex == NULL)
    {
        s_frame_mutex = xSemaphoreCreateMutex();
    }

    /* 1. Precompute 9-bit reversal table for N = 512 */
    for (uint32_t i = 0U; i < CWT_WINDOW_SAMPLES; i++)
    {
        uint32_t rev = 0U;
        uint32_t val = i;
        for (uint32_t b = 0U; b < CWT_FFT_LOG2_N; b++)
        {
            rev = (rev << 1U) | (val & 1U);
            val >>= 1U;
        }
        s_cwt.bitrev[i] = (uint16_t)rev;
    }

    /* 2. Precompute radix-2 FFT twiddle factors */
    for (uint32_t k = 0U; k < (CWT_WINDOW_SAMPLES / 2U); k++)
    {
        float angle = (2.0f * (float)M_PI * (float)k) / (float)CWT_WINDOW_SAMPLES;
        s_cwt.twiddle_cos[k] = cosf(angle);
        s_cwt.twiddle_sin[k] = sinf(angle);
    }

    /* 3. Precompute log-spaced center frequencies and Dual-Rate Analytic Morlet filters
     *    f_c[s] = f_min * (f_max / f_min)^(s / (S - 1))
     *    Ordered from s = 0 (100 Hz, largest scale) to s = 23 (100,000 Hz, smallest scale).
     *    - Scales with f_c < 3,000 Hz use the 32x-decimated horizon (fs_low = 15,625 Hz,
     *      df_low = 30.5176 Hz/bin).
     *    - Scales with f_c >= 3,000 Hz use the full-rate 500 kSPS window
     *      (df_high = 976.5625 Hz/bin).
     */
    const float fs_high_hz = (float)BOARD_ADC_SAMPLING_RATE_HZ;
    const float fs_low_hz  = fs_high_hz / (float)CWT_LOW_DECIMATION;
    const float df_high_hz = fs_high_hz / (float)CWT_WINDOW_SAMPLES; /* 976.5625 Hz */
    const float df_low_hz  = fs_low_hz  / (float)CWT_WINDOW_SAMPLES; /* 30.5176 Hz  */
    const float ratio      = CWT_FREQ_MAX_HZ / CWT_FREQ_MIN_HZ;      /* 1000.0      */

    for (uint32_t s = 0U; s < CWT_NUM_SCALES; s++)
    {
        float frac  = (float)s / (float)(CWT_NUM_SCALES - 1U);
        float fc_hz = CWT_FREQ_MIN_HZ * powf(ratio, frac);
        s_cwt.f_center_hz[s] = fc_hz;

        uint8_t is_low = (fc_hz < CWT_LOW_BAND_MAX_HZ) ? 1U : 0U;
        s_cwt.scale_is_low_rate[s] = is_low;

        float df_hz   = is_low ? df_low_hz : df_high_hz;
        float sigma_f = fc_hz / CWT_MORLET_OMEGA0;
        /* Prevent FFT grid scalloping when sigma_f is narrower than 0.75 * df_hz */
        float min_sig = 0.75f * df_hz;
        if (sigma_f < min_sig)
        {
            sigma_f = min_sig;
        }
        float inv_sig = 1.0f / sigma_f;

        s_cwt.psi_hat[s][0] = 0.0f; /* Zero DC response */
        const uint32_t half_n = CWT_WINDOW_SAMPLES / 2U;

        for (uint32_t k = 1U; k <= half_n; k++)
        {
            float fk    = (float)k * df_hz;
            float delta = (fk - fc_hz) * inv_sig;
            /* Analytic wavelet factor 2.0 on positive frequencies so a real
             * sinusoid A*cos(2*pi*fc*t) yields CWT modulus |W(s,t)| == A. */
            float weight = 2.0f * expf(-0.5f * delta * delta);
            if (k == half_n)
            {
                weight *= 0.5f; /* Nyquist bin weight */
            }
            s_cwt.psi_hat[s][k] = weight;
        }
    }

    /* Copy center frequencies into both output frame buffers */
    for (uint32_t b = 0U; b < 2U; b++)
    {
        memcpy(s_cwt.frames[b].f_center_hz, s_cwt.f_center_hz, sizeof(s_cwt.f_center_hz));
        s_cwt.frames[b].num_scales        = CWT_NUM_SCALES;
        s_cwt.frames[b].num_time_bins     = CWT_WINDOW_SAMPLES;
        s_cwt.frames[b].sampling_rate_hz  = BOARD_ADC_SAMPLING_RATE_HZ;
        s_cwt.frames[b].nominal_offset_mv = ADC_NOMINAL_OFFSET_MV;
    }

    APP_LOG("[cwt] initialized %u-scale Dual-Rate Complex Morlet filter bank (%.1f Hz .. %.1f kHz, N=%u)",
            (unsigned)CWT_NUM_SCALES,
            (double)CWT_FREQ_MIN_HZ,
            (double)(CWT_FREQ_MAX_HZ / 1000.0f),
            (unsigned)CWT_WINDOW_SAMPLES);
}

/* ==========================================================================
 * FreeRTOS CWT DSP Task (`vCwtProcessTask`)
 * ========================================================================== */

void vCwtProcessTask(void *argument)
{
    uint32_t last_processed_seq = 0U;
    float    dc_ema_raw         = (float)ADC_NOMINAL_OFFSET_RAW;
    bool     hist_primed        = false;

    (void)argument;

    for (;;)
    {
        bool     got_new_window = false;
        uint32_t seq            = 0U;
        uint32_t rate_hz        = BOARD_ADC_SAMPLING_RATE_HZ;
        uint16_t min_raw        = 0U;
        uint16_t max_raw        = 0U;
        uint16_t avg_raw        = 0U;
        uint16_t latest_raw     = 0U;
        uint32_t mv             = 0U;
        uint32_t vpp_mv         = 0U;
        int32_t  ac_min_mv      = 0;
        int32_t  ac_max_mv      = 0;
        int32_t  dc_err_mv      = 0;
        uint32_t sample_count   = 0U;
        uint32_t tc_count       = 0U;
        uint32_t ht_count       = 0U;
        uint32_t err_count      = 0U;
        uint32_t ovr_count      = 0U;
        uint32_t adc_ovr_count  = 0U;
        uint16_t channel        = BOARD_ADC_CHANNEL;
        uint32_t timestamp_ms   = 0U;

        /* 1. Poll shared memory under HSEM 4 for a new 512-sample window */
        if (Shared_TryLockCycles(HSEM_ID_ADC_DATA, SHARED_ADC_TRYLOCK_CYCLES))
        {
            uint32_t dwt_lock_start = DWT->CYCCNT;
            uint32_t cur_seq = g_shared.adc.waveform_seq;

            if ((cur_seq != last_processed_seq) && (cur_seq != 0U))
            {
                seq           = cur_seq;
                rate_hz       = g_shared.adc.sampling_rate_hz;
                min_raw       = g_shared.adc.min_raw;
                max_raw       = g_shared.adc.max_raw;
                avg_raw       = g_shared.adc.average_raw;
                latest_raw    = g_shared.adc.latest_raw;
                mv            = g_shared.adc.millivolts;
                vpp_mv        = g_shared.adc.vpp_mv;
                ac_min_mv     = g_shared.adc.ac_min_mv;
                ac_max_mv     = g_shared.adc.ac_max_mv;
                dc_err_mv     = g_shared.adc.dc_offset_error_mv;
                sample_count  = g_shared.adc.sample_count;
                tc_count      = g_shared.adc.dma_tc_count;
                ht_count      = g_shared.adc.dma_ht_count;
                err_count     = g_shared.adc.dma_error_count;
                ovr_count     = g_shared.adc.half_overrun_count;
                adc_ovr_count = g_shared.adc.adc_overrun_count;
                channel       = (uint16_t)g_shared.adc.channel;
                timestamp_ms  = g_shared.adc.timestamp_ms;

                memcpy(s_cwt.raw_snap, (const void *)g_shared.adc.waveform,
                       sizeof(uint16_t) * CWT_WINDOW_SAMPLES);
                got_new_window = true;
            }
            SHARED_UNLOCK(HSEM_ID_ADC_DATA);

            uint32_t hold = DWT->CYCCNT - dwt_lock_start;
            if (hold > g_shared.m7_adc_lock_hold_cycles_max)
            {
                g_shared.m7_adc_lock_hold_cycles_max = hold;
            }
        }
        else
        {
            g_shared.m7_adc_lock_fail_count++;
        }

        if (!got_new_window)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        last_processed_seq = seq;

        /* 2. Start DWT cycle counter for CWT DSP benchmark */
        uint32_t dwt_cwt_start = DWT->CYCCNT;

        /* 3. Remove 1.5 V DC Offset and convert to physical AC millivolts.
         *    Because the lowest frequency of interest is 100 Hz (T = 10 ms = ~10 windows),
         *    subtracting the instantaneous 1.024 ms window mean directly would high-pass
         *    attenuate signals below ~500 Hz. Instead, AUTO_MEAN uses a slow IIR DC bias
         *    tracker (alpha = 0.02 -> tau ~50 windows, f_-3dB ~3 Hz) so true 1.5 V DC
         *    offset and thermal drift are suppressed while 100 Hz .. 100 kHz pass unattenuated. */
        if (!hist_primed)
        {
            dc_ema_raw = (float)avg_raw;
        }
        else
        {
            dc_ema_raw = 0.98f * dc_ema_raw + 0.02f * (float)avg_raw;
        }

        CwtOffsetMode_t mode = s_offset_mode;
        float ref_raw = (mode == CWT_OFFSET_MODE_AUTO_MEAN)
                      ? dc_ema_raw
                      : (float)ADC_NOMINAL_OFFSET_RAW; /* 29789 counts = 1500 mV */

        const float lsb_mv = (float)BOARD_ADC_VREF_MV / 65535.0f;

        for (uint32_t n = 0U; n < CWT_WINDOW_SAMPLES; n++)
        {
            s_cwt.sig_re[n] = ((float)s_cwt.raw_snap[n] - ref_raw) * lsb_mv;
            s_cwt.sig_im[n] = 0.0f;
        }

        /* 4a. Update 32x-decimated low-frequency horizon buffer (15.625 kSPS, 32.768 ms span) */
        float dec_win[CWT_LOW_SAMPLES_PER_WIN];
        const float inv_dec = 1.0f / (float)CWT_LOW_DECIMATION;
        for (uint32_t d = 0U; d < CWT_LOW_SAMPLES_PER_WIN; d++)
        {
            float sum = 0.0f;
            uint32_t base = d * CWT_LOW_DECIMATION;
            for (uint32_t k = 0U; k < CWT_LOW_DECIMATION; k++)
            {
                sum += s_cwt.sig_re[base + k];
            }
            dec_win[d] = sum * inv_dec;
        }

        const uint32_t hist_len = 384U; /* 24 windows = 24.576 ms chronological history */
        if (!hist_primed)
        {
            for (uint32_t i = 0U; i < hist_len; i++)
            {
                s_cwt.low_hist[i] = dec_win[i % CWT_LOW_SAMPLES_PER_WIN];
            }
            hist_primed = true;
        }
        else
        {
            memmove(&s_cwt.low_hist[0],
                    &s_cwt.low_hist[CWT_LOW_SAMPLES_PER_WIN],
                    (hist_len - CWT_LOW_SAMPLES_PER_WIN) * sizeof(float));
            memcpy(&s_cwt.low_hist[hist_len - CWT_LOW_SAMPLES_PER_WIN],
                   dec_win,
                   CWT_LOW_SAMPLES_PER_WIN * sizeof(float));
        }

        /* Symmetric right-hand mirror padding across indices 384 .. 511 (128 samples)
         * so the current window (indices 368 .. 383) has zero wrap-around discontinuity */
        for (uint32_t i = hist_len; i < CWT_WINDOW_SAMPLES; i++)
        {
            uint32_t mirror_idx = (hist_len - 1U) - (i - hist_len);
            s_cwt.low_hist[i] = s_cwt.low_hist[mirror_idx];
        }

        for (uint32_t i = 0U; i < CWT_WINDOW_SAMPLES; i++)
        {
            s_cwt.low_re[i] = s_cwt.low_hist[i];
            s_cwt.low_im[i] = 0.0f;
        }

        /* 4b. Forward 512-point FFTs for both full-rate (500 kSPS) and low-rate (15.625 kSPS) */
        cwt_fft512_itcm(s_cwt.sig_re, s_cwt.sig_im, false);
        cwt_fft512_itcm(s_cwt.low_re, s_cwt.low_im, false);

        /* 5. Compute all 24 wavelet scales via frequency-domain multiplication + IFFT */
        float    global_max_mv = 0.0f;
        uint16_t best_scale    = 0U;
        uint16_t best_time     = 0U;

        for (uint32_t s = 0U; s < CWT_NUM_SCALES; s++)
        {
            uint16_t scale_peak_t = 0U;
            float scale_max = cwt_compute_scale_itcm(s, &scale_peak_t);
            if (scale_max > global_max_mv)
            {
                global_max_mv = scale_max;
                best_scale    = (uint16_t)s;
                best_time     = scale_peak_t;
            }
        }

        /* 6. Quantize 2D scalogram to uint16_t [0 .. 65535] for UDP transport */
        uint8_t next_idx = (uint8_t)(1U - s_active_frame_idx);
        CwtFrameResult_t *dst = &s_cwt.frames[next_idx];

        float mag_scale;
        float inv_scale;
        if (global_max_mv > 0.001f)
        {
            mag_scale = global_max_mv / 65535.0f;
            inv_scale = 65535.0f / global_max_mv;
        }
        else
        {
            mag_scale = 1.0f / 65535.0f;
            inv_scale = 0.0f;
        }

        for (uint32_t s = 0U; s < CWT_NUM_SCALES; s++)
        {
            const float * restrict src_row = s_cwt.mag_mv[s];
            uint16_t    * restrict dst_row = dst->scalogram[s];
            for (uint32_t n = 0U; n < CWT_WINDOW_SAMPLES; n++)
            {
                float q = src_row[n] * inv_scale + 0.5f;
                if (q > 65535.0f) { q = 65535.0f; }
                dst_row[n] = (uint16_t)q;
            }
        }

        uint32_t cwt_cycles = DWT->CYCCNT - dwt_cwt_start;
        uint32_t cwt_us     = cwt_cycles / (BOARD_SYSCLK_HZ / 1000000UL); /* 400 cycles/us */

        s_cwt_frames_count++;

        /* 7. Populate frame telemetry and swap double-buffer */
        dst->frame_seq          = seq;
        dst->sampling_rate_hz   = (rate_hz != 0U) ? rate_hz : BOARD_ADC_SAMPLING_RATE_HZ;
        dst->num_scales         = CWT_NUM_SCALES;
        dst->num_time_bins      = CWT_WINDOW_SAMPLES;
        dst->nominal_offset_mv  = ADC_NOMINAL_OFFSET_MV;
        dst->measured_dc_mv     = (uint16_t)mv;
        dst->dc_offset_error_mv = (int16_t)dc_err_mv;
        dst->vpp_mv             = (uint16_t)vpp_mv;
        dst->ac_min_mv          = (int16_t)ac_min_mv;
        dst->ac_max_mv          = (int16_t)ac_max_mv;
        dst->min_raw            = min_raw;
        dst->max_raw            = max_raw;
        dst->average_raw        = avg_raw;
        dst->latest_raw         = latest_raw;
        dst->channel            = channel;
        dst->offset_mode        = (uint16_t)mode;
        dst->sample_count       = sample_count;
        dst->dma_tc_count       = tc_count;
        dst->dma_ht_count       = ht_count;
        dst->dma_error_count    = err_count;
        dst->half_overrun_count = ovr_count;
        dst->adc_overrun_count  = adc_ovr_count;
        dst->cwt_exec_cycles    = cwt_cycles;
        dst->cwt_exec_us        = cwt_us;
        dst->cwt_frames_count   = s_cwt_frames_count;
        dst->m4_heartbeat       = g_shared.m4_heartbeat;
        dst->timestamp_ms       = timestamp_ms;
        dst->magnitude_scale    = mag_scale;
        dst->max_magnitude_mv   = global_max_mv;
        dst->peak_scale_idx     = best_scale;
        dst->peak_time_idx      = best_time;
        dst->peak_freq_hz       = s_cwt.f_center_hz[best_scale];

        memcpy(dst->raw_waveform, s_cwt.raw_snap, sizeof(uint16_t) * CWT_WINDOW_SAMPLES);

        if (s_frame_mutex != NULL)
        {
            (void)xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
            s_active_frame_idx = next_idx;
            s_has_valid_frame  = true;
            (void)xSemaphoreGive(s_frame_mutex);
        }
        else
        {
            __DMB();
            s_active_frame_idx = next_idx;
            s_has_valid_frame  = true;
        }

        /* Yield 1 tick (1 ms) to match the 1.024 ms DMA half-buffer cadence */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

bool CWT_GetLatestFrame(CwtFrameResult_t *dest)
{
    if ((dest == NULL) || (!s_has_valid_frame))
    {
        return false;
    }

    if (s_frame_mutex != NULL)
    {
        if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
        {
            return false;
        }
        memcpy(dest, &s_cwt.frames[s_active_frame_idx], sizeof(CwtFrameResult_t));
        (void)xSemaphoreGive(s_frame_mutex);
    }
    else
    {
        memcpy(dest, &s_cwt.frames[s_active_frame_idx], sizeof(CwtFrameResult_t));
    }

    return true;
}

void CWT_SetOffsetMode(CwtOffsetMode_t mode)
{
    s_offset_mode = mode;
}

CwtOffsetMode_t CWT_GetOffsetMode(void)
{
    return s_offset_mode;
}

const float *CWT_GetCenterFrequencies(void)
{
    return s_cwt.f_center_hz;
}
