/**
  ******************************************************************************
  * @file    dsp_wavelet.c
  * @brief   4-Level Daubechies-4 (db4) Wavelet Packet Transform (WPT) and
  *          high-frequency transient glitch detector on Cortex-M7.
  ******************************************************************************
  */

#include "dsp_wavelet.h"
#include <math.h>
#include <string.h>

#define DB4_FILTER_LEN              8U
#define ADC_CODE_TO_MV              (3300.0f / 65535.0f)

/* Transient detection thresholds on Level-1 High-Pass detail coefficients */
#define WPT_TRANSIENT_CREST_THRESH  4.5f
#define WPT_TRANSIENT_MIN_MV        35.0f

/* Orthogonal Daubechies-4 (db4) Low-Pass Decomposition Filter h[0..7] */
static const float DB4_LO_D[DB4_FILTER_LEN] = {
    -0.010597401785069032f,
     0.032883011666885200f,
     0.030841381835560764f,
    -0.187034811719093090f,
    -0.027983769416859854f,
     0.630880767929858900f,
     0.714846570552915700f,
     0.230377813308896500f
};

/* Orthogonal Daubechies-4 (db4) High-Pass Decomposition Filter g[k] = (-1)^k h[7-k] */
static const float DB4_HI_D[DB4_FILTER_LEN] = {
    -0.230377813308896500f,
     0.714846570552915700f,
    -0.630880767929858900f,
    -0.027983769416859854f,
     0.187034811719093090f,
     0.030841381835560764f,
    -0.032883011666885200f,
    -0.010597401785069032f
};

/*
 * Gray-code (Paley) natural-to-frequency ordering map for a 4-level WPT tree.
 * High-pass filtering + decimation by 2 mirrors the spectrum of the upper
 * branch at every split, so frequency band b (0..15) lives at binary tree
 * leaf index `WPT_GRAY_MAP[b]`.
 */
static const uint8_t WPT_GRAY_MAP[WPT_NUM_BANDS] = {
     0U,  1U,  3U,  2U,
     6U,  7U,  5U,  4U,
    12U, 13U, 15U, 14U,
    10U, 11U,  9U,  8U
};

/* Ping-pong decomposition buffers in cached D1 AXI SRAM (0x24000000) */
__attribute__((section(".axi_ram"), aligned(32)))
static float s_wpt_a[DSP_BLOCK_SIZE];

__attribute__((section(".axi_ram"), aligned(32)))
static float s_wpt_b[DSP_BLOCK_SIZE];

static uint32_t s_transient_count = 0U;

/**
  * @brief  Decompose a sub-band of length `len` (power of 2) into `len/2`
  *         Approximation coefficients (`dst_lo`) and `len/2` Detail
  *         coefficients (`dst_hi`) using periodic boundary extension.
  */
static void prv_db4_split_node(const float *src,
                               float *dst_lo,
                               float *dst_hi,
                               uint32_t len)
{
    uint32_t half = len >> 1U;
    uint32_t mask = len - 1U;

    for (uint32_t k = 0U; k < half; k++)
    {
        uint32_t base = k << 1U;
        float acc_lo = 0.0f;
        float acc_hi = 0.0f;

        for (uint32_t m = 0U; m < DB4_FILTER_LEN; m++)
        {
            float x = src[(base + m) & mask];
            acc_lo += DB4_LO_D[m] * x;
            acc_hi += DB4_HI_D[m] * x;
        }

        dst_lo[k] = acc_lo;
        dst_hi[k] = acc_hi;
    }
}

void DSP_Wavelet_Init(void)
{
    s_transient_count = 0U;
}

void DSP_Wavelet_ProcessBlock(const uint16_t *raw_adc,
                              uint32_t fs_hz,
                              uint32_t block_id,
                              float dc_offset_mv,
                              WPT_Result_t *out)
{
    if ((raw_adc == NULL) || (out == NULL))
    {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->block_id      = block_id;
    out->fs_hz         = (fs_hz > 0U) ? fs_hz : BOARD_ADC_SAMPLING_RATE_HZ;
    out->band_width_hz = (float)out->fs_hz / (float)(2U * WPT_NUM_BANDS);

    /* 1. Convert 4096 raw ADC samples into zero-mean millivolts in `s_wpt_a` */
    for (uint32_t i = 0U; i < DSP_BLOCK_SIZE; i++)
    {
        s_wpt_a[i] = ((float)raw_adc[i] * ADC_CODE_TO_MV) - dc_offset_mv;
    }

    /*
     * 2. Execute 4-level full binary tree WPT decomposition:
     *      Level 1: 1 node  of 4096 ->  2 nodes of 2048 (s_wpt_a -> s_wpt_b)
     *      Level 2: 2 nodes of 2048 ->  4 nodes of 1024 (s_wpt_b -> s_wpt_a)
     *      Level 3: 4 nodes of 1024 ->  8 nodes of  512 (s_wpt_a -> s_wpt_b)
     *      Level 4: 8 nodes of  512 -> 16 nodes of  256 (s_wpt_b -> s_wpt_a)
     */
    const float *src_buf = s_wpt_a;
    float       *dst_buf = s_wpt_b;

    for (uint32_t level = 0U; level < WPT_LEVELS; level++)
    {
        uint32_t num_nodes = 1U << level;
        uint32_t node_len  = DSP_BLOCK_SIZE >> level;
        uint32_t half_len  = node_len >> 1U;

        for (uint32_t n = 0U; n < num_nodes; n++)
        {
            const float *node_src = &src_buf[n * node_len];
            float       *child_lo = &dst_buf[(2U * n) * half_len];
            float       *child_hi = &dst_buf[(2U * n + 1U) * half_len];

            prv_db4_split_node(node_src, child_lo, child_hi, node_len);
        }

        /* After Level 1 completes, inspect the Level-1 High-Pass detail
           branch (`dst_buf[2048..4095]`, covering fs/4 .. fs/2) for impulsive
           microsecond transients before subsequent levels overwrite `dst_buf`. */
        if (level == 0U)
        {
            const float *hf_detail = &dst_buf[DSP_BLOCK_SIZE / 2U];
            uint32_t     hf_len    = DSP_BLOCK_SIZE / 2U;
            double       hf_sq_sum = 0.0;
            float        hf_max    = 0.0f;
            uint16_t     hf_max_idx = 0U;

            for (uint32_t k = 0U; k < hf_len; k++)
            {
                float a = fabsf(hf_detail[k]);
                hf_sq_sum += (double)a * (double)a;
                if (a > hf_max)
                {
                    hf_max     = a;
                    hf_max_idx = (uint16_t)(k * 2U);
                }
            }

            float hf_rms = sqrtf((float)(hf_sq_sum / (double)hf_len));
            float crest  = (hf_rms > 0.05f) ? (hf_max / hf_rms) : 0.0f;

            out->crest_factor_hf   = crest;
            out->transient_mag_mv  = hf_max;
            out->transient_sample  = hf_max_idx;
            out->transient_time_us = ((float)hf_max_idx * 1.0e6f) / (float)out->fs_hz;

            if ((crest > WPT_TRANSIENT_CREST_THRESH) && (hf_max > WPT_TRANSIENT_MIN_MV))
            {
                out->transient_detected = 1U;
                s_transient_count++;
            }
        }

        /* Swap ping-pong buffers for the next decomposition level */
        const float *tmp = src_buf;
        src_buf = dst_buf;
        dst_buf = (float *)(uintptr_t)tmp;
    }

    out->transient_count = s_transient_count;

    /*
     * 3. After 4 levels (`WPT_LEVELS = 4`), `src_buf` (`s_wpt_a`) holds the
     *    16 terminal nodes (256 coefficients each). Reorder them into monotonic
     *    frequency order using `WPT_GRAY_MAP` and compute energy, RMS (mV),
     *    and spectral density per band.
     */
    double band_energy[WPT_NUM_BANDS];
    double total_energy = 0.0;

    for (uint32_t b = 0U; b < WPT_NUM_BANDS; b++)
    {
        uint32_t     leaf_node = WPT_GRAY_MAP[b];
        const float *coeffs    = &src_buf[leaf_node * WPT_COEFFS_PER_BAND];
        double       e_sum     = 0.0;

        for (uint32_t k = 0U; k < WPT_COEFFS_PER_BAND; k++)
        {
            double c = (double)coeffs[k];
            e_sum += c * c;
        }

        band_energy[b] = e_sum;
        total_energy  += e_sum;

        /*
         * Parseval relation for orthogonal wavelet packets at depth J = 4:
         * Time-domain RMS contribution of band b across N = 4096 samples is
         * sqrt( sum(c_{b,k}^2) / N ).
         */
        out->rms_mv[b] = sqrtf((float)(e_sum / (double)DSP_BLOCK_SIZE));
        out->energy_density[b] = (out->band_width_hz > 0.0f)
            ? ((float)(e_sum / (double)DSP_BLOCK_SIZE) / out->band_width_hz)
            : 0.0f;
    }

    out->total_energy = (float)total_energy;

    /* 4. Compute relative energy share (%) and normalized Shannon entropy */
    float shannon = 0.0f;
    if (total_energy > 1e-6)
    {
        for (uint32_t b = 0U; b < WPT_NUM_BANDS; b++)
        {
            float p = (float)(band_energy[b] / total_energy);
            out->energy_pct[b] = p * 100.0f;
            if (p > 1e-7f)
            {
                shannon -= p * logf(p);
            }
        }
    }

    float max_entropy = logf((float)WPT_NUM_BANDS); /* ln(16) = 2.7725887 */
    out->entropy_shannon = shannon;
    out->entropy_norm    = (max_entropy > 0.0f) ? (shannon / max_entropy) : 0.0f;
}
