/**
  ******************************************************************************
  * @file    cwt_server.h
  * @brief   UDP server on port 5001 for Continuous Wavelet Transform (CWT)
  *          scalograms, raw/AC-coupled waveforms, and telemetry on Cortex-M7.
  ******************************************************************************
  */

#ifndef CWT_SERVER_H
#define CWT_SERVER_H

#include <stdint.h>
#include "cwt_processor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Binary Wire Protocol Constants ------------------------------------ */
#define CWT_PACKET_MAGIC            0x31545743UL /* "CWT1" in little-endian ('C','W','T','1') */
#define CWT_PACKET_VERSION          1U

#define WAVE_MAGIC                  0x57415645UL /* "WAVE" */
#define WAVE_VERSION                1U

/**
  * @brief Versioned binary header for each UDP chunk of a CWT response (`CWT` / `SCAL` / `CWT <idx>`).
  *        Size is compile-time asserted to 64 bytes.
  *
  *        Chunk layout (Total chunks = CWT_NUM_SCALES + 1 = 25):
  *          - chunk_idx == 0 (Metadata + Time-Domain Waveform Chunk):
  *              scale_offset    = 0
  *              scales_in_chunk = 0
  *              Payload (1120 bytes):
  *                - float    f_center_hz[CWT_NUM_SCALES]       (24 * 4 = 96 bytes)
  *                - uint16_t raw_waveform[CWT_WINDOW_SAMPLES]  (512 * 2 = 1024 bytes)
  *              Total UDP payload = 64 + 1120 = 1184 bytes (<= 1400 bytes).
  *
  *          - chunk_idx == 1 .. CWT_NUM_SCALES (Scalogram Row Chunks):
  *              scale_offset    = chunk_idx - 1  (0 .. 23)
  *              scales_in_chunk = 1
  *              Payload (1024 bytes):
  *                - uint16_t scalogram[scale_offset][512]      (512 * 2 = 1024 bytes)
  *              Total UDP payload = 64 + 1024 = 1088 bytes (<= 1400 bytes).
  */
typedef struct __attribute__((packed))
{
    uint32_t magic;              /*  0: 0x31545743 ("CWT1")                       */
    uint16_t version;            /*  4: CWT_PACKET_VERSION (1)                    */
    uint16_t header_len;         /*  6: sizeof(CwtPacketHeader_t) (64)            */
    uint32_t frame_seq;          /*  8: Monotonic sequence ID (= waveform_seq)    */
    uint16_t chunk_idx;          /* 12: 0 = metadata+wave, 1..24 = scale rows     */
    uint16_t total_chunks;       /* 14: CWT_NUM_SCALES + 1 (25)                   */
    uint32_t sampling_rate_hz;   /* 16: Nominal ADC sampling rate (500,000 Hz)    */
    uint16_t num_scales;         /* 20: CWT_NUM_SCALES (24)                       */
    uint16_t num_time_bins;      /* 22: CWT_WINDOW_SAMPLES (512)                  */
    uint16_t scale_offset;       /* 24: First scale index in this chunk           */
    uint16_t scales_in_chunk;    /* 26: Number of scale rows in this chunk (0/1)  */
    uint16_t nominal_offset_mv;  /* 28: Nominal DC bias (1500 mV)                 */
    uint16_t measured_dc_mv;     /* 30: Measured mean DC voltage in mV (~1500 mV) */
    uint16_t vpp_mv;             /* 32: Peak-to-peak AC swing in mV               */
    uint16_t min_raw;            /* 34: Minimum raw 16-bit ADC code               */
    uint16_t max_raw;            /* 36: Maximum raw 16-bit ADC code               */
    uint16_t average_raw;        /* 38: Mean raw 16-bit ADC code                  */
    uint32_t adc_overrun_count;  /* 40: Hardware ADC overrun counter (OVR)        */
    uint32_t dma_error_count;    /* 44: DMA error counter                         */
    uint32_t cwt_exec_us;        /* 48: CWT execution time on CM7 in microseconds */
    float    magnitude_scale;    /* 52: Multiplier to convert uint16 CWT to mV    */
    int16_t  dc_offset_error_mv; /* 56: measured_dc_mv - 1500 mV                  */
    uint16_t peak_scale_idx;     /* 58: Scale index (0..23) of peak wavelet energy*/
    uint32_t m4_heartbeat;       /* 60: CM4 super-loop liveness counter           */
} CwtPacketHeader_t;

_Static_assert(sizeof(CwtPacketHeader_t) == 64U,
               "CwtPacketHeader_t must be exactly 64 bytes (shared with tools/query_cwt.py and tools/cwt_dashboard.py)");

/**
  * @brief  Initializes and starts the UDP CWT & ADC telemetry listener on port 5001.
  * @note   Must be called in the tcpip thread context (e.g. via tcpip_callback()).
  */
void CWT_Server_Start(void);

#ifdef __cplusplus
}
#endif

#endif /* CWT_SERVER_H */
