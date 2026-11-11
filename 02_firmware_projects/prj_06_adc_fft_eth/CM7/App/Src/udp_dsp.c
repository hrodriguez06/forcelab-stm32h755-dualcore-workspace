/**
  ******************************************************************************
  * @file    udp_dsp.c
  * @brief   FreeRTOS + lwIP UDP server on port 5002 for real-time 4096-point
  *          FFT and 16-band Daubechies-4 (db4) Wavelet Packet Transform (WPT)
  *          telemetry, segmented spectrum dumps, raw waveform snapshots, and
  *          dynamic sampling rate control.
  ******************************************************************************
  */

#include "udp_dsp.h"
#include "net_config.h"
#include "net_task.h"
#include "shared_memory.h"
#include "board_def.h"
#include "app_log.h"

#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"

#include "FreeRTOS.h"
#include "task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPECTRUM_CHUNKS                 4U
#define SPECTRUM_BINS_PER_CHUNK         512U
#define SPECTRUM_CHUNK_PAYLOAD_BYTES    (sizeof(UDP_DSP_SpectrumChunkHeader_t) + \
                                         (SPECTRUM_BINS_PER_CHUNK * sizeof(uint16_t)))

static struct udp_pcb *s_dsp_pcb       = NULL;
static ip_addr_t       s_stream_ip;
static uint16_t        s_stream_port   = 0U;
static uint8_t         s_stream_active = 0U;
static uint16_t        s_tx_seq        = 0U;

/* Thread-safe snapshot of the most recent completed block in D1 AXI SRAM */
__attribute__((section(".axi_ram"), aligned(32)))
static DSP_Result_t s_snap_fft;

__attribute__((section(".axi_ram"), aligned(32)))
static WPT_Result_t s_snap_wpt;

__attribute__((section(".axi_ram"), aligned(32)))
static uint16_t s_snap_raw[DSP_BLOCK_SIZE];

/* Static TX scratch buffers in D1 AXI SRAM (avoids large stack frames on tcpip_thread) */
__attribute__((section(".axi_ram"), aligned(32)))
static uint8_t s_spec_chunk_buf[SPECTRUM_CHUNK_PAYLOAD_BYTES];

__attribute__((section(".axi_ram"), aligned(32)))
static uint8_t s_wave_pkt_buf[sizeof(UDP_DSP_WaveformHeader_t) +
                              (UDP_DSP_WAVE_SAMPLES * sizeof(uint16_t))];

__attribute__((section(".axi_ram"), aligned(32)))
static char s_ascii_tx_buf[1024];

static volatile uint8_t  s_snap_valid   = 0U;
static volatile uint32_t s_last_exec_us = 0U;

/* ---- Private Helper: Send a UDP datagram from inside tcpip_thread -------- */
static void prv_udp_send_bytes(struct udp_pcb *pcb,
                               const ip_addr_t *addr,
                               u16_t port,
                               const void *data,
                               u16_t len)
{
    if ((pcb == NULL) || (addr == NULL) || (data == NULL) || (len == 0U))
    {
        return;
    }

    struct pbuf *q = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (q == NULL)
    {
        return;
    }

    memcpy(q->payload, data, len);
    (void)udp_sendto(pcb, q, addr, port);
    pbuf_free(q);
}

/* ---- Private Helper: Post a runtime command to Cortex-M4 via SRAM4 ------- */
static void prv_send_cm4_command(uint32_t run, uint32_t fs_hz)
{
    if (Shared_TryLockCycles(HSEM_ID_ADC_DATA, SHARED_ADC_TRYLOCK_CYCLES))
    {
        g_shared.dsp.cmd_run = run;
        if (fs_hz > 0U)
        {
            g_shared.dsp.cmd_fs_hz = fs_hz;
        }
        __DMB();
        g_shared.dsp.cmd_seq = g_shared.dsp.cmd_seq + 1U;
        __DMB();
        SHARED_UNLOCK(HSEM_ID_ADC_DATA);
    }
    else
    {
        g_shared.m7_adc_lock_fail_count++;
    }
}

/* ---- Private Helper: Build 144-byte Telemetry Packet --------------------- */
static void prv_build_telemetry_packet(UDP_DSP_TelemetryPacket_t *pkt,
                                       const DSP_Result_t *fft_res,
                                       const WPT_Result_t *wpt_res,
                                       const uint16_t *raw_block,
                                       uint32_t overrun_count)
{
    memset(pkt, 0, sizeof(*pkt));

    pkt->magic          = UDP_DSP_PACKET_MAGIC;
    pkt->version        = UDP_DSP_PROTOCOL_VER;
    pkt->msg_type       = 1U;
    pkt->seq            = s_tx_seq++;
    pkt->block_id       = fft_res->block_id;
    pkt->fs_hz          = fft_res->fs_hz;
    pkt->block_size     = (uint16_t)DSP_BLOCK_SIZE;
    pkt->dc_offset_mv   = (uint16_t)(fft_res->dc_offset_mv + 0.5f);
    pkt->v_pp_mv        = (uint16_t)(fft_res->v_pp_mv + 0.5f);

    pkt->f0_hz_x10      = (uint32_t)(fft_res->f0_hz * 10.0f + 0.5f);
    pkt->f0_mag_mv      = (uint16_t)(fft_res->f0_mag_mv + 0.5f);
    pkt->noise_floor_mv = (uint16_t)(fft_res->noise_floor_mv + 0.5f);
    pkt->snr_db_x10     = (int16_t)(fft_res->snr_db * 10.0f);
    pkt->thd_pct_x10    = (uint16_t)(fft_res->thd_pct * 10.0f + 0.5f);

    for (uint32_t h = 0U; h < DSP_MAX_HARMONICS; h++)
    {
        pkt->harm_mag_mv[h] = (uint16_t)(fft_res->harm_mag_mv[h] + 0.5f);
    }

    if (raw_block != NULL)
    {
        for (uint32_t i = 0U; i < 4U; i++)
        {
            pkt->raw_preview[i] = raw_block[i];
        }
    }

    pkt->overrun_count = overrun_count;

    if (wpt_res != NULL)
    {
        pkt->wpt_num_bands = (uint16_t)WPT_NUM_BANDS;
        for (uint32_t b = 0U; b < WPT_NUM_BANDS; b++)
        {
            float e_x10 = wpt_res->energy_pct[b] * 10.0f + 0.5f;
            if (e_x10 > 1000.0f) { e_x10 = 1000.0f; }
            pkt->wpt_energy_pct_x10[b] = (uint16_t)e_x10;
            pkt->wpt_rms_mv[b]         = (uint16_t)(wpt_res->rms_mv[b] + 0.5f);
        }

        pkt->wpt_transient_mag_mv    = (uint16_t)(wpt_res->transient_mag_mv + 0.5f);
        pkt->wpt_transient_sample    = wpt_res->transient_sample;
        pkt->wpt_transient_count     = wpt_res->transient_count;
        pkt->wpt_crest_x10           = (uint16_t)(wpt_res->crest_factor_hf * 10.0f + 0.5f);
        pkt->wpt_entropy_norm_x1000  = (uint16_t)(wpt_res->entropy_norm * 1000.0f + 0.5f);
    }
}

/* ---- Private Helper: Send 4-chunk 2048-bin Spectrum Burst ---------------- */
static void prv_send_spectrum(struct udp_pcb *pcb,
                              const ip_addr_t *addr,
                              u16_t port)
{
    if (s_snap_valid == 0U)
    {
        const char *msg = "ERR: No FFT block available yet\r\n";
        prv_udp_send_bytes(pcb, addr, port, msg, (u16_t)strlen(msg));
        return;
    }

    UDP_DSP_SpectrumChunkHeader_t *hdr = (UDP_DSP_SpectrumChunkHeader_t *)s_spec_chunk_buf;
    uint16_t *bins_out = (uint16_t *)(s_spec_chunk_buf + sizeof(UDP_DSP_SpectrumChunkHeader_t));

    for (uint8_t c = 0U; c < SPECTRUM_CHUNKS; c++)
    {
        taskENTER_CRITICAL();
        hdr->magic         = UDP_DSP_SPECTRUM_MAGIC;
        hdr->block_id      = s_snap_fft.block_id;
        hdr->chunk_idx     = c;
        hdr->total_chunks  = SPECTRUM_CHUNKS;
        hdr->bins_in_chunk = SPECTRUM_BINS_PER_CHUNK;

        uint32_t base_bin = (uint32_t)c * SPECTRUM_BINS_PER_CHUNK;
        for (uint32_t i = 0U; i < SPECTRUM_BINS_PER_CHUNK; i++)
        {
            float val_x10 = s_snap_fft.mag_mv[base_bin + i] * 10.0f + 0.5f;
            if (val_x10 > 65535.0f)
            {
                val_x10 = 65535.0f;
            }
            bins_out[i] = (uint16_t)val_x10;
        }
        taskEXIT_CRITICAL();

        prv_udp_send_bytes(pcb, addr, port, s_spec_chunk_buf, (u16_t)sizeof(s_spec_chunk_buf));
    }
}

/* ---- Private Helper: Send 56B Header + 512 Raw Samples ('WAVE') ---------- */
static void prv_send_wave(struct udp_pcb *pcb,
                          const ip_addr_t *addr,
                          u16_t port)
{
    UDP_DSP_WaveformHeader_t *hdr = (UDP_DSP_WaveformHeader_t *)s_wave_pkt_buf;
    uint16_t *samples_out = (uint16_t *)(s_wave_pkt_buf + sizeof(UDP_DSP_WaveformHeader_t));

    memset(hdr, 0, sizeof(*hdr));
    hdr->magic              = UDP_DSP_WAVE_MAGIC;
    hdr->version            = 1U;
    hdr->header_len         = (uint16_t)sizeof(UDP_DSP_WaveformHeader_t);
    hdr->sampling_rate_hz   = g_shared.dsp.fs_hz;
    hdr->num_samples        = UDP_DSP_WAVE_SAMPLES;
    hdr->vref_mv            = (uint16_t)BOARD_ADC_VREF_MV;
    hdr->seq                = g_shared.dsp.block_id;
    hdr->min_raw            = g_shared.dsp.min_raw;
    hdr->max_raw            = g_shared.dsp.max_raw;
    hdr->avg_raw            = g_shared.dsp.average_raw;
    hdr->channel            = (uint16_t)g_shared.dsp.channel;
    hdr->millivolts         = g_shared.dsp.millivolts;
    hdr->sample_count       = g_shared.dsp.sample_count;
    hdr->dma_tc_count       = g_shared.dsp.dma_tc_count;
    hdr->half_overrun_count = g_shared.dsp.overrun_count;
    hdr->m4_heartbeat       = g_shared.m4_heartbeat;
    hdr->timestamp_ms       = g_shared.dsp.t_block_ms;
    hdr->adc_overrun_count  = g_shared.dsp.adc_overrun_count;

    taskENTER_CRITICAL();
    if (s_snap_valid != 0U)
    {
        memcpy(samples_out, s_snap_raw, UDP_DSP_WAVE_SAMPLES * sizeof(uint16_t));
    }
    else
    {
        uint32_t r_idx = g_shared.dsp.ready_idx & 1U;
        memcpy(samples_out,
               (const void *)g_shared.dsp.raw_buffer[r_idx],
               UDP_DSP_WAVE_SAMPLES * sizeof(uint16_t));
    }
    taskEXIT_CRITICAL();

    prv_udp_send_bytes(pcb, addr, port, s_wave_pkt_buf, (u16_t)sizeof(s_wave_pkt_buf));
}

/* ---- Private Helper: Send Human-Readable STATUS Summary ------------------ */
static void prv_send_status(struct udp_pcb *pcb,
                            const ip_addr_t *addr,
                            u16_t port)
{
    char *buf = s_ascii_tx_buf;
    const size_t buf_sz = sizeof(s_ascii_tx_buf);

    if (g_shared.dsp.fault_flag != 0U)
    {
        int n = snprintf(buf, buf_sz,
                         "=== CM4 HARDFAULT DETECTED ===\r\n"
                         "CFSR : 0x%08lX\r\n"
                         "HFSR : 0x%08lX\r\n"
                         "BFAR : 0x%08lX\r\n"
                         "PC   : 0x%08lX\r\n"
                         "LR   : 0x%08lX\r\n",
                         (unsigned long)g_shared.dsp.fault_cfsr,
                         (unsigned long)g_shared.dsp.fault_hfsr,
                         (unsigned long)g_shared.dsp.fault_bfar,
                         (unsigned long)g_shared.dsp.fault_pc,
                         (unsigned long)g_shared.dsp.fault_lr);
        if (n > 0)
        {
            prv_udp_send_bytes(pcb, addr, port, buf, (u16_t)n);
        }
        return;
    }

    uint32_t f0_int = g_shared.dsp.f0_hz_x10 / 10U;
    uint32_t f0_dec = g_shared.dsp.f0_hz_x10 % 10U;
    int16_t  snr_x10 = g_shared.dsp.snr_db_x10;
    int      snr_int = (int)(snr_x10 / 10);
    int      snr_dec = (int)(abs(snr_x10) % 10);
    uint32_t thd_int = g_shared.dsp.thd_pct_x10 / 10U;
    uint32_t thd_dec = g_shared.dsp.thd_pct_x10 % 10U;
    uint32_t ent_x1000 = g_shared.dsp.wpt_entropy_norm_x1000;
    uint32_t crest_int = g_shared.dsp.wpt_crest_x10 / 10U;
    uint32_t crest_dec = g_shared.dsp.wpt_crest_x10 % 10U;

    int len = snprintf(buf, buf_sz,
        "=== STM32H755 Dual-Core FFT + db4 WPT Analyzer (FreeRTOS CM7 + BM CM4) ===\r\n"
        "ADC Pin     : %s (Ch %lu) | Fs = %lu Hz | N = %lu\r\n"
        "Heartbeats  : CM7 = %lu | CM4 = %lu | Stream = %s\r\n"
        "Block ID    : %lu (exec = %lu us, overruns = %lu, adc_ovr = %lu, dma_err = %lu)\r\n"
        "Time Domain : DC = %u mV | Vpp = %u mV (min=%u max=%u avg=%u)\r\n"
        "FFT (4096)  : f0 = %lu.%lu Hz (%u mV) | Noise = %u mV | SNR = %d.%d dB | THD = %lu.%lu %%\r\n"
        "Harmonics   : H1=%u H2=%u H3=%u H4=%u H5=%u mV\r\n"
        "WPT (db4 L4): Entropy = 0.%03lu | HF Crest = %lu.%lu | Peak HF = %u mV @ sample %u | Glitches = %lu\r\n"
        "WPT E%% B0-7 : %u.%u %u.%u %u.%u %u.%u %u.%u %u.%u %u.%u %u.%u\r\n"
        "WPT E%% B8-15: %u.%u %u.%u %u.%u %u.%u %u.%u %u.%u %u.%u %u.%u\r\n",
        BOARD_ADC_PIN_NAME,
        (unsigned long)g_shared.dsp.channel,
        (unsigned long)g_shared.dsp.fs_hz,
        (unsigned long)g_shared.dsp.block_size,
        (unsigned long)g_shared.m7_heartbeat,
        (unsigned long)g_shared.m4_heartbeat,
        (s_stream_active != 0U) ? "ON" : "OFF",
        (unsigned long)g_shared.dsp.block_id,
        (unsigned long)s_last_exec_us,
        (unsigned long)g_shared.dsp.overrun_count,
        (unsigned long)g_shared.dsp.adc_overrun_count,
        (unsigned long)g_shared.dsp.dma_error_count,
        (unsigned)g_shared.dsp.dc_offset_mv,
        (unsigned)g_shared.dsp.v_pp_mv,
        (unsigned)g_shared.dsp.min_raw,
        (unsigned)g_shared.dsp.max_raw,
        (unsigned)g_shared.dsp.average_raw,
        (unsigned long)f0_int, (unsigned long)f0_dec,
        (unsigned)g_shared.dsp.f0_mag_mv,
        (unsigned)g_shared.dsp.noise_floor_mv,
        snr_int, snr_dec,
        (unsigned long)thd_int, (unsigned long)thd_dec,
        (unsigned)g_shared.dsp.harm_mag_mv[0],
        (unsigned)g_shared.dsp.harm_mag_mv[1],
        (unsigned)g_shared.dsp.harm_mag_mv[2],
        (unsigned)g_shared.dsp.harm_mag_mv[3],
        (unsigned)g_shared.dsp.harm_mag_mv[4],
        (unsigned long)ent_x1000,
        (unsigned long)crest_int, (unsigned long)crest_dec,
        (unsigned)g_shared.dsp.wpt_transient_mag_mv,
        (unsigned)g_shared.dsp.wpt_transient_sample,
        (unsigned long)g_shared.dsp.wpt_transient_count,
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[0] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[0] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[1] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[1] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[2] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[2] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[3] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[3] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[4] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[4] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[5] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[5] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[6] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[6] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[7] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[7] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[8] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[8] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[9] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[9] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[10] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[10] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[11] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[11] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[12] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[12] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[13] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[13] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[14] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[14] % 10U),
        (unsigned)(g_shared.dsp.wpt_energy_pct_x10[15] / 10U), (unsigned)(g_shared.dsp.wpt_energy_pct_x10[15] % 10U));

    if (len > 0)
    {
        if ((size_t)len >= buf_sz)
        {
            len = (int)(buf_sz - 1U);
        }
        prv_udp_send_bytes(pcb, addr, port, buf, (u16_t)len);
    }
}

/* ---- Private Helper: Send JSON Telemetry Summary ------------------------- */
static void prv_send_json(struct udp_pcb *pcb,
                          const ip_addr_t *addr,
                          u16_t port)
{
    char *buf = s_ascii_tx_buf;
    const size_t buf_sz = sizeof(s_ascii_tx_buf);
    uint32_t f0_int  = g_shared.dsp.f0_hz_x10 / 10U;
    uint32_t f0_dec  = g_shared.dsp.f0_hz_x10 % 10U;
    int16_t  snr_x10 = g_shared.dsp.snr_db_x10;
    int      snr_int = (int)(snr_x10 / 10);
    int      snr_dec = (int)(abs(snr_x10) % 10);
    uint32_t thd_int = g_shared.dsp.thd_pct_x10 / 10U;
    uint32_t thd_dec = g_shared.dsp.thd_pct_x10 % 10U;

    int len = snprintf(buf, buf_sz,
        "{\"block_id\":%lu,\"fs_hz\":%lu,\"block_size\":%lu,\"channel\":%lu,"
        "\"m7_hb\":%lu,\"m4_hb\":%lu,\"exec_us\":%lu,"
        "\"dc_mv\":%u,\"vpp_mv\":%u,\"f0_hz\":%lu.%lu,\"f0_mv\":%u,"
        "\"noise_mv\":%u,\"snr_db\":%d.%d,\"thd_pct\":%lu.%lu,"
        "\"wpt_entropy\":0.%03lu,\"wpt_crest\":%u.%u,"
        "\"wpt_transient_mv\":%u,\"wpt_transient_sample\":%u,"
        "\"wpt_transient_count\":%lu,\"overruns\":%lu}\r\n",
        (unsigned long)g_shared.dsp.block_id,
        (unsigned long)g_shared.dsp.fs_hz,
        (unsigned long)g_shared.dsp.block_size,
        (unsigned long)g_shared.dsp.channel,
        (unsigned long)g_shared.m7_heartbeat,
        (unsigned long)g_shared.m4_heartbeat,
        (unsigned long)s_last_exec_us,
        (unsigned)g_shared.dsp.dc_offset_mv,
        (unsigned)g_shared.dsp.v_pp_mv,
        (unsigned long)f0_int, (unsigned long)f0_dec,
        (unsigned)g_shared.dsp.f0_mag_mv,
        (unsigned)g_shared.dsp.noise_floor_mv,
        snr_int, snr_dec,
        (unsigned long)thd_int, (unsigned long)thd_dec,
        (unsigned long)g_shared.dsp.wpt_entropy_norm_x1000,
        (unsigned)(g_shared.dsp.wpt_crest_x10 / 10U),
        (unsigned)(g_shared.dsp.wpt_crest_x10 % 10U),
        (unsigned)g_shared.dsp.wpt_transient_mag_mv,
        (unsigned)g_shared.dsp.wpt_transient_sample,
        (unsigned long)g_shared.dsp.wpt_transient_count,
        (unsigned long)g_shared.dsp.overrun_count);

    if (len > 0)
    {
        if ((size_t)len >= buf_sz)
        {
            len = (int)(buf_sz - 1U);
        }
        prv_udp_send_bytes(pcb, addr, port, buf, (u16_t)len);
    }
}

/* ---- lwIP UDP Receive Callback (executes on `tcpip_thread`) -------------- */
static void udp_dsp_recv_cb(void *arg,
                            struct udp_pcb *pcb,
                            struct pbuf *p,
                            const ip_addr_t *addr,
                            u16_t port)
{
    LWIP_UNUSED_ARG(arg);

    if (p == NULL)
    {
        return;
    }

    if (Net_SourceIsAllowed(addr) == 0)
    {
        pbuf_free(p);
        return;
    }

    char cmd[64];
    u16_t copy_len = (p->tot_len < (sizeof(cmd) - 1U))
        ? p->tot_len
        : (u16_t)(sizeof(cmd) - 1U);
    pbuf_copy_partial(p, cmd, copy_len, 0U);
    cmd[copy_len] = '\0';
    pbuf_free(p);

    /* Trim trailing whitespace / CR / LF and convert command word to uppercase */
    while ((copy_len > 0U) &&
           ((cmd[copy_len - 1U] == '\r') ||
            (cmd[copy_len - 1U] == '\n') ||
            (cmd[copy_len - 1U] == ' ')  ||
            (cmd[copy_len - 1U] == '\t')))
    {
        copy_len--;
        cmd[copy_len] = '\0';
    }

    for (u16_t i = 0U; i < copy_len; i++)
    {
        cmd[i] = (char)toupper((unsigned char)cmd[i]);
    }

    if (strcmp(cmd, "PING") == 0)
    {
        const char *reply = "PONG\r\n";
        prv_udp_send_bytes(pcb, addr, port, reply, (u16_t)strlen(reply));
    }
    else if (strcmp(cmd, "STREAM") == 0)
    {
        ip_addr_copy(s_stream_ip, *addr);
        s_stream_port   = port;
        s_stream_active = 1U;
        prv_send_cm4_command(1U, 0U);

        const char *reply = "OK: STREAM STARTED (FFT 4096 + WPT db4 16-Band)\r\n";
        prv_udp_send_bytes(pcb, addr, port, reply, (u16_t)strlen(reply));
    }
    else if (strcmp(cmd, "STOP") == 0)
    {
        s_stream_active = 0U;

        const char *reply = "OK: STREAM STOPPED\r\n";
        prv_udp_send_bytes(pcb, addr, port, reply, (u16_t)strlen(reply));
    }
    else if (strcmp(cmd, "SPECTRUM") == 0)
    {
        prv_send_spectrum(pcb, addr, port);
    }
    else if (strcmp(cmd, "WAVE") == 0)
    {
        prv_send_wave(pcb, addr, port);
    }
    else if ((strcmp(cmd, "STATUS") == 0) ||
             (strcmp(cmd, "STAT") == 0)   ||
             (strcmp(cmd, "GET") == 0))
    {
        prv_send_status(pcb, addr, port);
    }
    else if (strcmp(cmd, "JSON") == 0)
    {
        prv_send_json(pcb, addr, port);
    }
    else if (strncmp(cmd, "FS ", 3U) == 0)
    {
        uint32_t req_fs = (uint32_t)strtoul(&cmd[3], NULL, 10);
        char reply[96];

        if ((req_fs >= BOARD_ADC_MIN_FS_HZ) && (req_fs <= BOARD_ADC_MAX_FS_HZ))
        {
            prv_send_cm4_command(1U, req_fs);
            int n = snprintf(reply, sizeof(reply),
                             "OK: FS=%lu Hz requested\r\n",
                             (unsigned long)req_fs);
            if (n > 0)
            {
                prv_udp_send_bytes(pcb, addr, port, reply, (u16_t)n);
            }
        }
        else
        {
            int n = snprintf(reply, sizeof(reply),
                             "ERR: FS out of range (%lu..%lu Hz)\r\n",
                             (unsigned long)BOARD_ADC_MIN_FS_HZ,
                             (unsigned long)BOARD_ADC_MAX_FS_HZ);
            if (n > 0)
            {
                prv_udp_send_bytes(pcb, addr, port, reply, (u16_t)n);
            }
        }
    }
    else
    {
        const char *help =
            "UDP :5002 Commands: STREAM | STOP | SPECTRUM | WAVE | STATUS | JSON | FS <1000..100000> | PING | HELP\r\n";
        prv_udp_send_bytes(pcb, addr, port, help, (u16_t)strlen(help));
    }
}

/* ---- Public API ---------------------------------------------------------- */

int UDP_DSP_Init(void)
{
    s_dsp_pcb = udp_new();
    if (s_dsp_pcb == NULL)
    {
        APP_LOG("[dsp_udp] ERROR: udp_new failed");
        return -1;
    }

    if (udp_bind(s_dsp_pcb, IP_ADDR_ANY, UDP_DSP_PORT) != ERR_OK)
    {
        udp_remove(s_dsp_pcb);
        s_dsp_pcb = NULL;
        APP_LOG("[dsp_udp] ERROR: udp_bind failed on port %u", (unsigned)UDP_DSP_PORT);
        return -1;
    }

    udp_recv(s_dsp_pcb, udp_dsp_recv_cb, NULL);
    APP_LOG("[dsp_udp] listening on UDP port %u (STREAM, STOP, SPECTRUM, WAVE, STATUS, JSON, FS)",
            (unsigned)UDP_DSP_PORT);
    return 0;
}

void UDP_DSP_PublishAndStream(const DSP_Result_t *fft_res,
                              const WPT_Result_t *wpt_res,
                              const uint16_t *raw_block,
                              uint32_t overrun_count)
{
    if ((fft_res == NULL) || (wpt_res == NULL) || (raw_block == NULL))
    {
        return;
    }

    /* 1. Snapshot latest results in AXI SRAM under a short critical section */
    taskENTER_CRITICAL();
    memcpy(&s_snap_fft, fft_res, sizeof(s_snap_fft));
    memcpy(&s_snap_wpt, wpt_res, sizeof(s_snap_wpt));
    memcpy(s_snap_raw, raw_block, sizeof(s_snap_raw));
    s_snap_valid = 1U;
    taskEXIT_CRITICAL();

    /* 2. If a UDP client subscribed via `STREAM`, emit the 144-byte packet */
    if ((s_stream_active == 0U) || (s_dsp_pcb == NULL))
    {
        return;
    }

    UDP_DSP_TelemetryPacket_t pkt;
    prv_build_telemetry_packet(&pkt, fft_res, wpt_res, raw_block, overrun_count);

    LOCK_TCPIP_CORE();
    prv_udp_send_bytes(s_dsp_pcb, &s_stream_ip, s_stream_port, &pkt, (u16_t)sizeof(pkt));
    UNLOCK_TCPIP_CORE();
}

int UDP_DSP_GetLatestResults(DSP_Result_t *fft_out,
                             WPT_Result_t *wpt_out,
                             uint32_t *exec_us_out)
{
    if (s_snap_valid == 0U)
    {
        return 0;
    }

    taskENTER_CRITICAL();
    if (fft_out != NULL)
    {
        memcpy(fft_out, &s_snap_fft, sizeof(*fft_out));
    }
    if (wpt_out != NULL)
    {
        memcpy(wpt_out, &s_snap_wpt, sizeof(*wpt_out));
    }
    if (exec_us_out != NULL)
    {
        *exec_us_out = s_last_exec_us;
    }
    taskEXIT_CRITICAL();

    return 1;
}

void UDP_DSP_SetLastExecTimeUs(uint32_t exec_us)
{
    s_last_exec_us = exec_us;
}
