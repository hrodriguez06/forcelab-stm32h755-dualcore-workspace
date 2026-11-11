/**
  ******************************************************************************
  * @file    cwt_server.c
  * @brief   UDP server on port 5001 for Continuous Wavelet Transform (CWT)
  *          scalograms, raw/AC-coupled waveforms, and telemetry on Cortex-M7.
  *
  *          Supported UDP commands on `:5001`:
  *            - `PING`              : Replies `"PONG\r\n"`.
  *            - `STAT` / `GET`      : Human-readable telemetry summary including
  *                                    1.5 V DC offset metrics and CWT timing.
  *            - `JSON`              : JSON telemetry summary.
  *            - `WAVE`              : 56-byte `WaveformHeader_t` + 512 `uint16_t`
  *                                    raw samples (1080 B, backwards-compatible
  *                                    with `prj_04_adc_dma_bm_eth`).
  *            - `CWT` / `SCAL`      : Multi-packet UDP burst (or indexed chunk
  *                                    `CWT <idx>` / `CWT B <start> <count>`)
  *                                    transmitting the 64-byte `CwtPacketHeader_t`
  *                                    + metadata/waveform chunk 0 + 24 scale chunks.
  *            - `MODE AUTO` / `MODE FIXED`: Switches 1.5 V DC offset removal mode.
  ******************************************************************************
  */

#include "cwt_server.h"
#include "cwt_processor.h"
#include "net_config.h"
#include "board_def.h"
#include "stm32h7xx_hal.h"
#include "shared_memory.h"
#include "app_log.h"
#include "net_task.h"

#include "lwip/udp.h"
#include "lwip/pbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Backwards-compatible WAVE header (56 bytes) */
typedef struct __attribute__((packed))
{
    uint32_t magic;              /* 0x57415645 = 'WAVE' */
    uint16_t version;            /* WAVE_VERSION (1)    */
    uint16_t header_len;         /* 56                  */
    uint32_t sampling_rate_hz;
    uint16_t num_samples;
    uint16_t vref_mv;
    uint32_t seq;
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
} WaveformHeader_t;

_Static_assert(sizeof(WaveformHeader_t) == 56U,
               "WaveformHeader_t must be 56 bytes");

static struct udp_pcb *s_cwt_pcb = NULL;

/* Snapshot buffer in AXI SRAM used by the UDP server when serving CWT chunks */
static CwtFrameResult_t s_srv_frame __attribute__((section(".axi_ram"), aligned(32)));
static bool             s_srv_has_frame = false;

static void fill_cwt_common_header(CwtPacketHeader_t *hdr,
                                   const CwtFrameResult_t *frame,
                                   uint16_t chunk_idx,
                                   uint16_t scale_offset,
                                   uint16_t scales_in_chunk)
{
    hdr->magic              = CWT_PACKET_MAGIC;
    hdr->version            = CWT_PACKET_VERSION;
    hdr->header_len         = (uint16_t)sizeof(CwtPacketHeader_t);
    hdr->frame_seq          = frame->frame_seq;
    hdr->chunk_idx          = chunk_idx;
    hdr->total_chunks       = (uint16_t)(CWT_NUM_SCALES + 1U);
    hdr->sampling_rate_hz   = frame->sampling_rate_hz;
    hdr->num_scales         = frame->num_scales;
    hdr->num_time_bins      = frame->num_time_bins;
    hdr->scale_offset       = scale_offset;
    hdr->scales_in_chunk    = scales_in_chunk;
    hdr->nominal_offset_mv  = frame->nominal_offset_mv;
    hdr->measured_dc_mv     = frame->measured_dc_mv;
    hdr->vpp_mv             = frame->vpp_mv;
    hdr->min_raw            = frame->min_raw;
    hdr->max_raw            = frame->max_raw;
    hdr->average_raw        = frame->average_raw;
    hdr->adc_overrun_count  = frame->adc_overrun_count;
    hdr->dma_error_count    = frame->dma_error_count;
    hdr->cwt_exec_us        = frame->cwt_exec_us;
    hdr->magnitude_scale    = frame->magnitude_scale;
    hdr->dc_offset_error_mv = frame->dc_offset_error_mv;
    hdr->peak_scale_idx     = frame->peak_scale_idx;
    hdr->m4_heartbeat       = frame->m4_heartbeat;
}

/**
  * @brief  Sends a single CWT chunk (`chunk_idx` in `0 .. CWT_NUM_SCALES`) over UDP.
  *         Each datagram is <= 1184 bytes (well below the 1400-byte limit).
  */
static void send_cwt_chunk(struct udp_pcb *pcb,
                           const ip_addr_t *addr,
                           u16_t port,
                           const CwtFrameResult_t *frame,
                           uint16_t chunk_idx)
{
    const uint16_t total_chunks = (uint16_t)(CWT_NUM_SCALES + 1U);
    if (chunk_idx >= total_chunks)
    {
        return;
    }

    if (chunk_idx == 0U)
    {
        /* Chunk 0: 64B header + 96B f_center_hz[24] + 1024B raw_waveform[512] = 1184 bytes */
        const u16_t freqs_bytes = (u16_t)(sizeof(float) * CWT_NUM_SCALES);
        const u16_t wave_bytes  = (u16_t)(sizeof(uint16_t) * CWT_WINDOW_SAMPLES);
        const u16_t pkt_len     = (u16_t)(sizeof(CwtPacketHeader_t) + freqs_bytes + wave_bytes);

        struct pbuf *cp = pbuf_alloc(PBUF_TRANSPORT, pkt_len, PBUF_RAM);
        if (cp != NULL)
        {
            CwtPacketHeader_t *hdr = (CwtPacketHeader_t *)cp->payload;
            uint8_t *body = (uint8_t *)cp->payload + sizeof(CwtPacketHeader_t);

            fill_cwt_common_header(hdr, frame, 0U, 0U, 0U);
            memcpy(body, frame->f_center_hz, freqs_bytes);
            memcpy(body + freqs_bytes, frame->raw_waveform, wave_bytes);

            (void)udp_sendto(pcb, cp, addr, port);
            pbuf_free(cp);
        }
    }
    else
    {
        /* Chunk 1..24: 64B header + 1024B scalogram[scale_idx][512] = 1088 bytes */
        const uint16_t scale_idx = (uint16_t)(chunk_idx - 1U);
        const u16_t row_bytes    = (u16_t)(sizeof(uint16_t) * CWT_WINDOW_SAMPLES);
        const u16_t pkt_len      = (u16_t)(sizeof(CwtPacketHeader_t) + row_bytes);

        struct pbuf *cp = pbuf_alloc(PBUF_TRANSPORT, pkt_len, PBUF_RAM);
        if (cp != NULL)
        {
            CwtPacketHeader_t *hdr = (CwtPacketHeader_t *)cp->payload;
            uint8_t *body = (uint8_t *)cp->payload + sizeof(CwtPacketHeader_t);

            fill_cwt_common_header(hdr, frame, chunk_idx, scale_idx, 1U);
            memcpy(body, frame->scalogram[scale_idx], row_bytes);

            (void)udp_sendto(pcb, cp, addr, port);
            pbuf_free(cp);
        }
    }
}

static bool match_prefix_ci(const char *buf, size_t len, const char *kw)
{
    size_t kw_len = strlen(kw);
    if (len < kw_len)
    {
        return false;
    }
    for (size_t i = 0; i < kw_len; i++)
    {
        char c = buf[i];
        if (c >= 'a' && c <= 'z')
        {
            c = (char)(c - ('a' - 'A'));
        }
        if (c != kw[i])
        {
            return false;
        }
    }
    return true;
}

static void cwt_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port)
{
    char cmd_buf[32];
    size_t copy_len = 0U;

    LWIP_UNUSED_ARG(arg);

    if (p == NULL)
    {
        return;
    }

    /* Enforce on-subnet ACL source filter */
    if (Net_SourceIsAllowed(addr) == 0)
    {
        pbuf_free(p);
        return;
    }

    if ((p->payload != NULL) && (p->len > 0U))
    {
        copy_len = (p->len < (sizeof(cmd_buf) - 1U)) ? p->len : (sizeof(cmd_buf) - 1U);
        memcpy(cmd_buf, p->payload, copy_len);
    }
    cmd_buf[copy_len] = '\0';
    pbuf_free(p);

    /* 1. PING command */
    if (match_prefix_ci(cmd_buf, copy_len, "PING"))
    {
        static const char pong_msg[] = "PONG\r\n";
        struct pbuf *rp = pbuf_alloc(PBUF_TRANSPORT, (u16_t)(sizeof(pong_msg) - 1U), PBUF_RAM);
        if (rp != NULL)
        {
            memcpy(rp->payload, pong_msg, sizeof(pong_msg) - 1U);
            (void)udp_sendto(pcb, rp, addr, port);
            pbuf_free(rp);
        }
        return;
    }

    /* 2. Offset removal mode command: "MODE AUTO" or "MODE FIXED" */
    if (match_prefix_ci(cmd_buf, copy_len, "MODE"))
    {
        if (match_prefix_ci(cmd_buf, copy_len, "MODE FIXED"))
        {
            CWT_SetOffsetMode(CWT_OFFSET_MODE_FIXED_1V5);
        }
        else
        {
            CWT_SetOffsetMode(CWT_OFFSET_MODE_AUTO_MEAN);
        }
        char ack[64];
        int n = snprintf(ack, sizeof(ack), "{\"ok\":1,\"offset_mode\":\"%s\"}\r\n",
                         (CWT_GetOffsetMode() == CWT_OFFSET_MODE_FIXED_1V5) ? "FIXED_1V5" : "AUTO_MEAN");
        if (n > 0)
        {
            struct pbuf *rp = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
            if (rp != NULL)
            {
                memcpy(rp->payload, ack, (size_t)n);
                (void)udp_sendto(pcb, rp, addr, port);
                pbuf_free(rp);
            }
        }
        return;
    }

    /* 3. CWT / SCAL command:
     *    - "CWT" or "SCAL": latches latest CWT frame and transmits chunks.
     *      Note: ETH_DRIVER_TX_BUFFER_COUNT is 4 descriptors in SRAM3.
     *      To support both single-request bursts and zero-loss windowed fetching:
     *        * "CWT" or "SCAL" (with no args) refreshes the snapshot and sends a
     *          burst of chunks (or all chunks that fit in the TX ring).
     *        * "CWT SNAP" refreshes the latched CWT frame and sends Chunk 0.
     *        * "CWT <idx>" sends chunk `<idx>` (0..24) from the latched frame.
     *        * "CWT B <start> <count>" sends `<count>` chunks starting at `<start>`
     *          (e.g. `count <= 4` guarantees zero TX descriptor exhaustion on a 4-buffer ring).
     */
    if (match_prefix_ci(cmd_buf, copy_len, "CWT") || match_prefix_ci(cmd_buf, copy_len, "SCAL"))
    {
        const char *arg_ptr = cmd_buf + (match_prefix_ci(cmd_buf, copy_len, "SCAL") ? 4 : 3);
        while (*arg_ptr == ' ')
        {
            arg_ptr++;
        }

        if (*arg_ptr == '\0')
        {
            /* Unparameterized "CWT" or "SCAL": latch latest CWT frame and transmit chunks */
            if (CWT_GetLatestFrame(&s_srv_frame))
            {
                s_srv_has_frame = true;
            }
            if (!s_srv_has_frame)
            {
                return;
            }

            const uint16_t total_chunks = (uint16_t)(CWT_NUM_SCALES + 1U);
            for (uint16_t c = 0U; c < total_chunks; c++)
            {
                send_cwt_chunk(pcb, addr, port, &s_srv_frame, c);
            }
            return;
        }

        if (match_prefix_ci(arg_ptr, strlen(arg_ptr), "SNAP"))
        {
            if (CWT_GetLatestFrame(&s_srv_frame))
            {
                s_srv_has_frame = true;
            }
            if (s_srv_has_frame)
            {
                send_cwt_chunk(pcb, addr, port, &s_srv_frame, 0U);
            }
            return;
        }

        if ((*arg_ptr == 'B') || (*arg_ptr == 'b'))
        {
            arg_ptr++;
            while (*arg_ptr == ' ') { arg_ptr++; }
            char *endp = NULL;
            unsigned long start_c = strtoul(arg_ptr, &endp, 10);
            unsigned long count_c = 1UL;
            if ((endp != NULL) && (*endp != '\0'))
            {
                count_c = strtoul(endp, NULL, 10);
            }
            if ((start_c == 0UL) || (!s_srv_has_frame))
            {
                if (CWT_GetLatestFrame(&s_srv_frame))
                {
                    s_srv_has_frame = true;
                }
            }
            if (!s_srv_has_frame)
            {
                return;
            }
            if (count_c > 4UL)
            {
                count_c = 4UL;
            }
            for (unsigned long k = 0UL; k < count_c; k++)
            {
                uint16_t cid = (uint16_t)(start_c + k);
                if (cid < (CWT_NUM_SCALES + 1U))
                {
                    send_cwt_chunk(pcb, addr, port, &s_srv_frame, cid);
                }
            }
            return;
        }

        /* "CWT <chunk_idx>" */
        char *endp = NULL;
        unsigned long req_idx = strtoul(arg_ptr, &endp, 10);
        if ((req_idx == 0UL) || (!s_srv_has_frame))
        {
            if (CWT_GetLatestFrame(&s_srv_frame))
            {
                s_srv_has_frame = true;
            }
        }
        if (s_srv_has_frame && (req_idx < (unsigned long)(CWT_NUM_SCALES + 1U)))
        {
            send_cwt_chunk(pcb, addr, port, &s_srv_frame, (uint16_t)req_idx);
        }
        return;
    }

    /* 4. WAVE command (56B header + 512 uint16_t raw samples = 1080 bytes) */
    if (match_prefix_ci(cmd_buf, copy_len, "WAVE"))
    {
        const u16_t total_wave_len = (u16_t)(sizeof(WaveformHeader_t) +
                                             sizeof(uint16_t) * SHARED_ADC_WAVEFORM_SAMPLES);
        struct pbuf *wp = pbuf_alloc(PBUF_TRANSPORT, total_wave_len, PBUF_RAM);
        if (wp != NULL)
        {
            WaveformHeader_t *hdr = (WaveformHeader_t *)wp->payload;
            uint8_t *sample_dest = (uint8_t *)wp->payload + sizeof(WaveformHeader_t);

            if (Shared_TryLockCycles(HSEM_ID_ADC_DATA, SHARED_ADC_TRYLOCK_CYCLES))
            {
                hdr->magic              = WAVE_MAGIC;
                hdr->version            = WAVE_VERSION;
                hdr->header_len         = (uint16_t)sizeof(WaveformHeader_t);
                hdr->sampling_rate_hz   = g_shared.adc.sampling_rate_hz;
                hdr->num_samples        = (uint16_t)g_shared.adc.waveform_len;
                hdr->vref_mv            = (uint16_t)BOARD_ADC_VREF_MV;
                hdr->seq                = g_shared.adc.waveform_seq;
                hdr->min_raw            = g_shared.adc.min_raw;
                hdr->max_raw            = g_shared.adc.max_raw;
                hdr->avg_raw            = g_shared.adc.average_raw;
                hdr->channel            = (uint16_t)g_shared.adc.channel;
                hdr->millivolts         = g_shared.adc.millivolts;
                hdr->sample_count       = g_shared.adc.sample_count;
                hdr->dma_tc_count       = g_shared.adc.dma_tc_count;
                hdr->half_overrun_count = g_shared.adc.half_overrun_count;
                hdr->adc_overrun_count  = g_shared.adc.adc_overrun_count;
                hdr->timestamp_ms       = g_shared.adc.timestamp_ms;
                memcpy(sample_dest, (const void *)g_shared.adc.waveform,
                       sizeof(uint16_t) * SHARED_ADC_WAVEFORM_SAMPLES);
                SHARED_UNLOCK(HSEM_ID_ADC_DATA);

                hdr->m4_heartbeat       = g_shared.m4_heartbeat;
                (void)udp_sendto(pcb, wp, addr, port);
            }
            else
            {
                g_shared.m7_adc_lock_fail_count++;
            }
            pbuf_free(wp);
        }
        return;
    }

    /* 5. Scalar Telemetry: JSON or STAT / GET */
    bool is_json = match_prefix_ci(cmd_buf, copy_len, "JSON");

    if (CWT_GetLatestFrame(&s_srv_frame))
    {
        s_srv_has_frame = true;
    }

    char resp[512];
    int  len = 0;

    if (s_srv_has_frame)
    {
        uint32_t peak_f_hz = (uint32_t)(s_srv_frame.peak_freq_hz + 0.5f);
        uint32_t peak_m_mv = (uint32_t)(s_srv_frame.max_magnitude_mv * 100.0f + 0.5f);

        if (is_json)
        {
            len = snprintf(resp, sizeof(resp),
                           "{\"ch\":%u,\"pin\":\"%s\",\"raw\":%u,\"avg\":%u,\"min\":%u,\"max\":%u,"
                           "\"nominal_offset_mv\":%u,\"measured_dc_mv\":%u,\"dc_offset_error_mv\":%d,"
                           "\"vpp_mv\":%u,\"ac_min_mv\":%d,\"ac_max_mv\":%d,"
                           "\"rate\":%lu,\"seq\":%lu,\"samples\":%lu,\"tc\":%lu,\"ht\":%lu,"
                           "\"err\":%lu,\"ovr\":%lu,\"adc_ovr\":%lu,"
                           "\"cwt_scales\":%u,\"cwt_bins\":%u,\"cwt_exec_us\":%lu,\"cwt_exec_cycles\":%lu,"
                           "\"cwt_frames\":%lu,\"peak_scale\":%u,\"peak_freq_hz\":%lu,"
                           "\"peak_mag_mv\":%lu.%02lu,\"offset_mode\":\"%s\",\"m4_hb\":%lu}\r\n",
                           (unsigned)s_srv_frame.channel,
                           BOARD_ADC_PIN_NAME,
                           (unsigned)s_srv_frame.latest_raw,
                           (unsigned)s_srv_frame.average_raw,
                           (unsigned)s_srv_frame.min_raw,
                           (unsigned)s_srv_frame.max_raw,
                           (unsigned)s_srv_frame.nominal_offset_mv,
                           (unsigned)s_srv_frame.measured_dc_mv,
                           (int)s_srv_frame.dc_offset_error_mv,
                           (unsigned)s_srv_frame.vpp_mv,
                           (int)s_srv_frame.ac_min_mv,
                           (int)s_srv_frame.ac_max_mv,
                           (unsigned long)s_srv_frame.sampling_rate_hz,
                           (unsigned long)s_srv_frame.frame_seq,
                           (unsigned long)s_srv_frame.sample_count,
                           (unsigned long)s_srv_frame.dma_tc_count,
                           (unsigned long)s_srv_frame.dma_ht_count,
                           (unsigned long)s_srv_frame.dma_error_count,
                           (unsigned long)s_srv_frame.half_overrun_count,
                           (unsigned long)s_srv_frame.adc_overrun_count,
                           (unsigned)s_srv_frame.num_scales,
                           (unsigned)s_srv_frame.num_time_bins,
                           (unsigned long)s_srv_frame.cwt_exec_us,
                           (unsigned long)s_srv_frame.cwt_exec_cycles,
                           (unsigned long)s_srv_frame.cwt_frames_count,
                           (unsigned)s_srv_frame.peak_scale_idx,
                           (unsigned long)peak_f_hz,
                           (unsigned long)(peak_m_mv / 100UL),
                           (unsigned long)(peak_m_mv % 100UL),
                           (s_srv_frame.offset_mode == CWT_OFFSET_MODE_FIXED_1V5) ? "FIXED_1V5" : "AUTO_MEAN",
                           (unsigned long)s_srv_frame.m4_heartbeat);
        }
        else
        {
            len = snprintf(resp, sizeof(resp),
                           "ADC1_CH%u [%s] CWT: dc=%u mV (nom=%u mV, err=%+d mV), vpp=%u mV (AC %+d..%+d mV), "
                           "raw=%u (min=%u, max=%u, avg=%u), rate=%lu Hz, seq=%lu, "
                           "cwt=%ux%u in %lu us (peak=%lu Hz @ scale %u, %lu.%02lu mV), "
                           "tc=%lu, err=%lu, adc_ovr=%lu, m4_hb=%lu\r\n",
                           (unsigned)s_srv_frame.channel,
                           BOARD_ADC_PIN_NAME,
                           (unsigned)s_srv_frame.measured_dc_mv,
                           (unsigned)s_srv_frame.nominal_offset_mv,
                           (int)s_srv_frame.dc_offset_error_mv,
                           (unsigned)s_srv_frame.vpp_mv,
                           (int)s_srv_frame.ac_min_mv,
                           (int)s_srv_frame.ac_max_mv,
                           (unsigned)s_srv_frame.latest_raw,
                           (unsigned)s_srv_frame.min_raw,
                           (unsigned)s_srv_frame.max_raw,
                           (unsigned)s_srv_frame.average_raw,
                           (unsigned long)s_srv_frame.sampling_rate_hz,
                           (unsigned long)s_srv_frame.frame_seq,
                           (unsigned)s_srv_frame.num_scales,
                           (unsigned)s_srv_frame.num_time_bins,
                           (unsigned long)s_srv_frame.cwt_exec_us,
                           (unsigned long)peak_f_hz,
                           (unsigned)s_srv_frame.peak_scale_idx,
                           (unsigned long)(peak_m_mv / 100UL),
                           (unsigned long)(peak_m_mv % 100UL),
                           (unsigned long)s_srv_frame.dma_tc_count,
                           (unsigned long)s_srv_frame.dma_error_count,
                           (unsigned long)s_srv_frame.adc_overrun_count,
                           (unsigned long)s_srv_frame.m4_heartbeat);
        }
    }
    else
    {
        len = snprintf(resp, sizeof(resp),
                       "{\"ok\":0,\"status\":\"waiting_for_cm4_adc_window\",\"m4_hb\":%lu}\r\n",
                       (unsigned long)g_shared.m4_heartbeat);
    }

    if (len >= (int)sizeof(resp))
    {
        len = (int)sizeof(resp) - 1;
    }

    if (len > 0)
    {
        struct pbuf *rp = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
        if (rp != NULL)
        {
            memcpy(rp->payload, resp, (size_t)len);
            (void)udp_sendto(pcb, rp, addr, port);
            pbuf_free(rp);
        }
    }
}

void CWT_Server_Start(void)
{
    if (s_cwt_pcb != NULL)
    {
        return;
    }

    s_cwt_pcb = udp_new();
    if (s_cwt_pcb == NULL)
    {
        APP_LOG("[cwt_srv] ERROR: failed to allocate udp pcb");
        return;
    }

    if (udp_bind(s_cwt_pcb, IP_ADDR_ANY, NET_CWT_PORT) != ERR_OK)
    {
        udp_remove(s_cwt_pcb);
        s_cwt_pcb = NULL;
        APP_LOG("[cwt_srv] ERROR: bind failed on port %u", (unsigned)NET_CWT_PORT);
        return;
    }

    udp_recv(s_cwt_pcb, cwt_server_recv, NULL);
    APP_LOG("[cwt_srv] listening on UDP port %u (commands: STAT, JSON, WAVE, CWT/SCAL, PING)",
            (unsigned)NET_CWT_PORT);
}
