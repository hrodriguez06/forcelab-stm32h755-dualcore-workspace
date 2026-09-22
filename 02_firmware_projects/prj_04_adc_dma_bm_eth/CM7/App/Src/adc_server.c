/**
  ******************************************************************************
  * @file    adc_server.c
  * @brief   UDP telemetry query server implementation on Cortex-M7.
  *
  *          Listens on NET_ADC_PORT (5001). When a query packet arrives:
  *            - Takes SHARED_LOCK(HSEM_ID_ADC_DATA)
  *            - Copies snapshot of g_shared.adc
  *            - Releases SHARED_UNLOCK(HSEM_ID_ADC_DATA)
  *            - Responds immediately with plain text or JSON
  ******************************************************************************
  */

#include "adc_server.h"
#include "net_config.h"
#include "board_def.h"
#include "stm32h7xx_hal.h"
#include "shared_memory.h"
#include "app_log.h"

#include "lwip/udp.h"
#include "lwip/pbuf.h"

#include <stdio.h>
#include <string.h>

#define WAVE_MAGIC              0x57415645U /* 'WAVE' */

typedef struct __attribute__((packed))
{
    uint32_t magic;              /* 0x57415645 = 'WAVE' */
    uint32_t sampling_rate_hz;   /* Nominal rate, e.g. 500,000 Hz */
    uint16_t num_samples;        /* e.g. 512 samples */
    uint16_t vref_mv;            /* e.g. 3300 mV */
    uint32_t seq;                /* Monotonic waveform sequence ID */
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
} WaveformHeader_t;

static struct udp_pcb *s_adc_pcb = NULL;

static void adc_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port)
{
    int is_wave = 0;
    int is_json = 0;

    LWIP_UNUSED_ARG(arg);

    if (p == NULL)
    {
        return;
    }

    /* Inspect incoming command */
    if (p->payload != NULL && p->len >= 4)
    {
        const char *req = (const char *)p->payload;
        if ((req[0] == 'W' || req[0] == 'w') &&
            (req[1] == 'A' || req[1] == 'a') &&
            (req[2] == 'V' || req[2] == 'v') &&
            (req[3] == 'E' || req[3] == 'e'))
        {
            is_wave = 1;
        }
        else if ((req[0] == 'J' || req[0] == 'j') &&
                 (req[1] == 'S' || req[1] == 's') &&
                 (req[2] == 'O' || req[2] == 'o') &&
                 (req[3] == 'N' || req[3] == 'n'))
        {
            is_json = 1;
        }
    }
    pbuf_free(p);

    if (is_wave)
    {
        /* Binary waveform response: 48-byte header + 512 uint16_t samples = 1072 bytes */
        const u16_t total_wave_len = (u16_t)(sizeof(WaveformHeader_t) +
                                            sizeof(uint16_t) * SHARED_ADC_WAVEFORM_SAMPLES);
        struct pbuf *wp = pbuf_alloc(PBUF_TRANSPORT, total_wave_len, PBUF_RAM);

        if (wp != NULL)
        {
            WaveformHeader_t *hdr = (WaveformHeader_t *)wp->payload;
            uint8_t *sample_dest = (uint8_t *)wp->payload + sizeof(WaveformHeader_t);

            if (Shared_TryLock(HSEM_ID_ADC_DATA, 1000U))
            {
                hdr->magic              = WAVE_MAGIC;
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
                hdr->timestamp_ms       = g_shared.adc.timestamp_ms;
                memcpy(sample_dest, (const void *)g_shared.adc.waveform,
                       sizeof(uint16_t) * SHARED_ADC_WAVEFORM_SAMPLES);
                SHARED_UNLOCK(HSEM_ID_ADC_DATA);
                hdr->m4_heartbeat       = g_shared.m4_heartbeat;

                (void)udp_sendto(pcb, wp, addr, port);
            }
            pbuf_free(wp);
        }
        return;
    }

    /* Scalar Telemetry (JSON or Text) */
    char     resp[256];
    int      len = 0;
    uint32_t ch, latest, avg, min_val, max_val, mv, samples, tc, ht, err, ovr, adc_ovr, rate, seq, m4_hb;

    if (!Shared_TryLock(HSEM_ID_ADC_DATA, 1000U))
    {
        return; /* Failed to acquire HSEM: do not stall FreeRTOS tcpip thread */
    }
    ch      = g_shared.adc.channel;
    latest  = g_shared.adc.latest_raw;
    avg     = g_shared.adc.average_raw;
    min_val = g_shared.adc.min_raw;
    max_val = g_shared.adc.max_raw;
    mv      = g_shared.adc.millivolts;
    samples = g_shared.adc.sample_count;
    tc      = g_shared.adc.dma_tc_count;
    ht      = g_shared.adc.dma_ht_count;
    err     = g_shared.adc.dma_error_count;
    ovr     = g_shared.adc.half_overrun_count;
    adc_ovr = g_shared.adc.adc_overrun_count;
    rate    = g_shared.adc.sampling_rate_hz;
    seq     = g_shared.adc.waveform_seq;
    SHARED_UNLOCK(HSEM_ID_ADC_DATA);
    m4_hb = g_shared.m4_heartbeat;

    if (is_json)
    {
        len = snprintf(resp, sizeof(resp),
                       "{\"ch\":%lu,\"raw\":%lu,\"avg\":%lu,\"min\":%lu,\"max\":%lu,\"mv\":%lu,"
                       "\"rate\":%lu,\"seq\":%lu,\"samples\":%lu,\"tc\":%lu,\"ht\":%lu,"
                       "\"err\":%lu,\"ovr\":%lu,\"adc_ovr\":%lu,\"m4_hb\":%lu}\r\n",
                       (unsigned long)ch,
                       (unsigned long)latest,
                       (unsigned long)avg,
                       (unsigned long)min_val,
                       (unsigned long)max_val,
                       (unsigned long)mv,
                       (unsigned long)rate,
                       (unsigned long)seq,
                       (unsigned long)samples,
                       (unsigned long)tc,
                       (unsigned long)ht,
                       (unsigned long)err,
                       (unsigned long)ovr,
                       (unsigned long)adc_ovr,
                       (unsigned long)m4_hb);
    }
    else
    {
        len = snprintf(resp, sizeof(resp),
                       "ADC1_CH%lu [%s]: raw=%lu, avg=%lu (%lu.%03lu V), min=%lu, max=%lu, "
                       "rate=%lu, seq=%lu, samples=%lu, dma_tc=%lu, dma_ht=%lu, err=%lu, ovr=%lu, adc_ovr=%lu, m4_hb=%lu\r\n",
                       (unsigned long)ch,
                       BOARD_ADC_PIN_NAME,
                       (unsigned long)latest,
                       (unsigned long)avg,
                       (unsigned long)(mv / 1000U),
                       (unsigned long)(mv % 1000U),
                       (unsigned long)min_val,
                       (unsigned long)max_val,
                       (unsigned long)rate,
                       (unsigned long)seq,
                       (unsigned long)samples,
                       (unsigned long)tc,
                       (unsigned long)ht,
                       (unsigned long)err,
                       (unsigned long)ovr,
                       (unsigned long)adc_ovr,
                       (unsigned long)m4_hb);
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

void ADC_Server_Start(void)
{
    if (s_adc_pcb != NULL)
    {
        return;
    }

    s_adc_pcb = udp_new();
    if (s_adc_pcb == NULL)
    {
        APP_LOG("[adc_srv] ERROR: failed to allocate udp pcb");
        return;
    }

    if (udp_bind(s_adc_pcb, IP_ADDR_ANY, NET_ADC_PORT) != ERR_OK)
    {
        udp_remove(s_adc_pcb);
        s_adc_pcb = NULL;
        APP_LOG("[adc_srv] ERROR: bind failed on port %u", (unsigned)NET_ADC_PORT);
        return;
    }

    udp_recv(s_adc_pcb, adc_server_recv, NULL);
    APP_LOG("[adc_srv] listening on UDP port %u", (unsigned)NET_ADC_PORT);
}
