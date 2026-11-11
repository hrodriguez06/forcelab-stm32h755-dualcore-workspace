/**
  ******************************************************************************
  * @file    udp_dsp.h
  * @brief   FreeRTOS + lwIP UDP server on port 5002 for real-time 4096-point
  *          FFT and 16-band Daubechies-4 (db4) Wavelet Packet Transform (WPT)
  *          telemetry, segmented spectrum dumps, raw waveform snapshots, and
  *          dynamic sampling rate control.
  *
  * Supported UDP ASCII Commands on port `:5002`:
  *   - `STREAM`          : Subscribe caller to automatic 144-byte binary
  *                         telemetry frames (`DSP2` magic) on each new block
  *                         and ensure CM4 ADC capture is running.
  *   - `STOP`            : Unsubscribe caller from streaming and pause CM4
  *                         TIM3 trigger.
  *   - `SPECTRUM`        : Send the full 2048-bin FFT magnitude spectrum in
  *                         4 UDP packets of 1036 bytes (`SPEC` magic, 512 bins
  *                         per chunk, `uint16_t` in $0.1\text{ mV}$ units).
  *   - `WAVE`            : Send a 56-byte `WaveformHeader_t` (`WAVE` magic) +
  *                         512 raw 16-bit ADC samples (1080 bytes total).
  *   - `STATUS` / `STAT` : Human-readable multi-line ASCII summary of FFT,
  *                         16-band WPT, and CM4 ADC/fault status.
  *   - `JSON`            : Single-frame JSON telemetry summary.
  *   - `FS <hz>`         : Reconfigure CM4 TIM3 sampling frequency on the fly
  *                         (`1000..100000` Hz).
  *   - `PING`            : Replies `"PONG\r\n"`.
  *   - `HELP`            : Lists all supported UDP commands.
  ******************************************************************************
  */

#ifndef UDP_DSP_H
#define UDP_DSP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "dsp_engine.h"
#include "dsp_wavelet.h"

#define UDP_DSP_PORT                    5002U
#define UDP_DSP_PACKET_MAGIC            0x44535032UL    /* 'DSP2' */
#define UDP_DSP_SPECTRUM_MAGIC          0x53504543UL    /* 'SPEC' */
#define UDP_DSP_WAVE_MAGIC              0x57415645UL    /* 'WAVE' */
#define UDP_DSP_PROTOCOL_VER            2U
#define UDP_DSP_WAVE_SAMPLES            512U

#pragma pack(push, 1)

/**
  * @brief  144-byte binary telemetry frame streamed on each completed 4096-pt
  *         block (`STREAM` mode). Byte-for-byte compatible with the Node.js
  *         Web Dashboard (`tools/web_dashboard/server.js`) and Python CLI
  *         visualizer (`tools/dsp_visualizer.py`).
  */
typedef struct
{
    /* --- Header & Block Timing (22 bytes, offset 0..21) --- */
    uint32_t magic;                             /* 0x44535032 ('DSP2')        */
    uint8_t  version;                           /* Protocol version (2)       */
    uint8_t  msg_type;                          /* 1 = Periodic Telemetry     */
    uint16_t seq;                               /* UDP packet sequence number */
    uint32_t block_id;                          /* ADC block sequence ID      */
    uint32_t fs_hz;                             /* Effective sampling rate Hz */
    uint16_t block_size;                        /* 4096                       */
    uint16_t dc_offset_mv;                      /* DC mean voltage (mV)       */
    uint16_t v_pp_mv;                           /* Peak-to-peak voltage (mV)  */

    /* --- FFT Spectral Metrics (44 bytes, offset 22..65) --- */
    uint32_t f0_hz_x10;                         /* Fundamental freq (Hz * 10) */
    uint16_t f0_mag_mv;                         /* Fundamental peak (mV)      */
    uint16_t noise_floor_mv;                    /* Noise floor RMS (mV)       */
    int16_t  snr_db_x10;                        /* SNR (dB * 10)              */
    uint16_t thd_pct_x10;                       /* THD (% * 10)               */
    uint16_t harm_mag_mv[DSP_MAX_HARMONICS];    /* H1..H10 amplitudes (mV)    */
    uint16_t raw_preview[4];                    /* First 4 raw ADC samples    */
    uint32_t overrun_count;                     /* CM4 buffer overrun count   */

    /* --- 4-Level db4 WPT Metrics (78 bytes, offset 66..143) --- */
    uint16_t wpt_num_bands;                     /* 16 sub-bands               */
    uint16_t wpt_energy_pct_x10[WPT_NUM_BANDS]; /* Sub-band energy (% * 10)   */
    uint16_t wpt_rms_mv[WPT_NUM_BANDS];         /* Sub-band RMS (mV)          */
    uint16_t wpt_transient_mag_mv;              /* Peak Level-1 HF detail mV  */
    uint16_t wpt_transient_sample;              /* Sample index (0..4095)     */
    uint32_t wpt_transient_count;               /* Cumulative glitch count    */
    uint16_t wpt_crest_x10;                     /* HF crest factor * 10       */
    uint16_t wpt_entropy_norm_x1000;            /* Normalized Shannon * 1000  */
} UDP_DSP_TelemetryPacket_t;

/**
  * @brief  12-byte header prepended to each of the 4 segmented spectrum UDP
  *         packets (`SPECTRUM` command: 4 chunks x 512 bins = 2048 bins).
  */
typedef struct
{
    uint32_t magic;                             /* 0x53504543 ('SPEC')        */
    uint32_t block_id;                          /* Matching ADC block ID      */
    uint8_t  chunk_idx;                         /* 0 .. 3                     */
    uint8_t  total_chunks;                      /* 4                          */
    uint16_t bins_in_chunk;                     /* 512                        */
} UDP_DSP_SpectrumChunkHeader_t;

/**
  * @brief  56-byte backwards-compatible header for the `WAVE` command
  *         (followed by 512 `uint16_t` raw ADC samples = 1080 bytes total).
  */
typedef struct
{
    uint32_t magic;                             /* 0x57415645 ('WAVE')        */
    uint16_t version;                           /* 1                          */
    uint16_t header_len;                        /* 56                         */
    uint32_t sampling_rate_hz;
    uint16_t num_samples;                       /* 512                        */
    uint16_t vref_mv;                           /* 3300                       */
    uint32_t seq;                               /* block_id                   */
    uint16_t min_raw;
    uint16_t max_raw;
    uint16_t avg_raw;
    uint16_t channel;
    uint32_t millivolts;
    uint32_t sample_count;
    uint32_t dma_tc_count;
    uint32_t half_overrun_count;
    uint32_t m4_heartbeat;
    uint32_t timestamp_ms;
    uint32_t adc_overrun_count;
} UDP_DSP_WaveformHeader_t;

#pragma pack(pop)

_Static_assert(sizeof(UDP_DSP_TelemetryPacket_t) == 144U,
               "UDP_DSP_TelemetryPacket_t must be exactly 144 bytes");
_Static_assert(sizeof(UDP_DSP_SpectrumChunkHeader_t) == 12U,
               "UDP_DSP_SpectrumChunkHeader_t must be 12 bytes");
_Static_assert(sizeof(UDP_DSP_WaveformHeader_t) == 56U,
               "UDP_DSP_WaveformHeader_t must be 56 bytes");

/**
  * @brief  Bind the UDP DSP server PCB on port 5002 (called on `tcpip_thread`).
  * @retval 0 on success, -1 on failure.
  */
int UDP_DSP_Init(void);

/**
  * @brief  Update the thread-safe DSP snapshot in AXI SRAM and, if a client is
  *         currently subscribed via `STREAM`, transmit the 144-byte binary
  *         telemetry packet (`UDP_DSP_TelemetryPacket_t`).
  *         Safe to call from `vDspProcessTask` (acquires `LOCK_TCPIP_CORE()`).
  */
void UDP_DSP_PublishAndStream(const DSP_Result_t *fft_res,
                              const WPT_Result_t *wpt_res,
                              const uint16_t *raw_block,
                              uint32_t overrun_count);

/**
  * @brief  Copy the latest completed FFT and WPT results for status logging.
  * @retval 1 if a valid frame was copied, 0 if no frame has been processed yet.
  */
int UDP_DSP_GetLatestResults(DSP_Result_t *fft_out,
                             WPT_Result_t *wpt_out,
                             uint32_t *exec_us_out);

/**
  * @brief  Record the execution time (in microseconds) of the most recent
  *         FFT + WPT cycle.
  */
void UDP_DSP_SetLastExecTimeUs(uint32_t exec_us);

#ifdef __cplusplus
}
#endif

#endif /* UDP_DSP_H */
