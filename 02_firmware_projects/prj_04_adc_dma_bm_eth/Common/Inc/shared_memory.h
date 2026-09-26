/**
  ******************************************************************************
  * @file    shared_memory.h
  * @brief   Shared M7 <-> M4 data block for STM32H755ZI.
  *
  *          Backed by the .shared_ram section, which BOTH linker scripts place
  *          at 0x38008000 (SRAM4, D3 domain, 32 KB, non-cacheable via MPU).
  *
  *          Rules:
  *            1. This header is compiled into BOTH cores. Any change requires
  *               rebuilding and reflashing BOTH images.
  *            2. Every field is `volatile`.
  *            3. Multi-word / non-atomic access MUST be guarded by an HSEM.
  *            4. The struct is padded to a 32-byte boundary so it never shares
  *               a cache line with anything else.
  ******************************************************************************
  */

#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- HSEM identifiers ----------------------------------------------------
   HSEM 0 is reserved by ST for the classic boot handshake (M4 waits in
   HAL_PWREx_ClearPendingEvent/__WFE until M7 releases HSEM 0).             */
#define HSEM_ID_BOOT_SYNC       0U   /* reserved: M7 -> M4 boot release      */
#define HSEM_ID_SHARED_STATE    1U   /* guards SharedMemory_t.state          */
#define HSEM_ID_M7_TO_M4_MBOX   2U   /* guards m7_to_m4                      */
#define HSEM_ID_M4_TO_M7_MBOX   3U   /* guards m4_to_m7                      */
#define HSEM_ID_ADC_DATA        4U   /* guards adc telemetry data            */

#define SHARED_MBOX_SIZE        64U  /* multiple of 32 (cache line)          */
#define SHARED_ADC_WAVEFORM_SAMPLES 512U /* 512 samples = 1.024 ms @ 500 kSPS (10 cycles @ 10 kHz) */

typedef struct __attribute__((aligned(32)))
{
    volatile uint16_t latest_raw;        /* Most recent ADC raw sample (16-bit) */
    volatile uint16_t average_raw;       /* Moving average of buffer            */
    volatile uint16_t min_raw;           /* Minimum observed in buffer          */
    volatile uint16_t max_raw;           /* Maximum observed in buffer          */
    volatile uint32_t millivolts;        /* Scaled voltage (mV)                 */
    volatile uint32_t sample_count;      /* Total conversions/buffers processed */
    volatile uint32_t dma_tc_count;      /* DMA Transfer-Complete counter       */
    volatile uint32_t dma_ht_count;      /* DMA Half-Transfer counter           */
    volatile uint32_t dma_error_count;   /* DMA/ADC error counter               */
    volatile uint32_t half_overrun_count;/* Half-buffers dropped by consumer lag*/
    volatile uint32_t adc_overrun_count; /* ADC hardware overrun (OVR flag)     */
    volatile uint32_t channel;           /* Channel number (5 for PB1, 15 for PA3) */
    volatile uint32_t sampling_rate_hz;  /* Nominal rate (500000 Hz)            */
    volatile uint32_t timestamp_ms;      /* CM4 timestamp (HAL tick / loop)     */
    volatile uint32_t waveform_seq;      /* Monotonic waveform sequence ID      */
    volatile uint16_t waveform_len;      /* Number of valid samples in waveform */
    volatile uint16_t _reserved;         /* Alignment to 32-bit                 */
    volatile uint16_t waveform[SHARED_ADC_WAVEFORM_SAMPLES]; /* 512 raw samples (1024 B) */
} SharedAdcData_t;

typedef struct __attribute__((aligned(32)))
{
    volatile uint32_t magic;          /* 0xH755SHM sentinel, written by M7   */
    volatile uint32_t m7_heartbeat;   /* incremented by M7                   */
    volatile uint32_t m4_heartbeat;   /* incremented by M4                   */
    volatile uint32_t state;          /* application state, HSEM 1 guarded   */

    volatile uint16_t vrefint_cal;    /* Factory VREFINT calibration word (read by M7) */
    volatile uint16_t ts_cal1;        /* Factory TS_CAL1 calibration word (read by M7) */
    volatile uint16_t ts_cal2;        /* Factory TS_CAL2 calibration word (read by M7) */
    volatile uint16_t cal_valid;      /* 1 if calibration data populated by M7         */
    uint8_t           _pad[8];        /* Retains 32-byte alignment of SharedAdcData_t  */
    SharedAdcData_t   adc;            /* Protected by HSEM_ID_ADC_DATA (4U)  */

    volatile uint32_t m7_to_m4_len;
    volatile uint8_t  m7_to_m4[SHARED_MBOX_SIZE];

    volatile uint32_t m4_to_m7_len;
    volatile uint8_t  m4_to_m7[SHARED_MBOX_SIZE];

    volatile uint32_t adc_lock_fail_count; /* Failures in Shared_TryLockCycles */
    uint8_t           _pad2[20];           /* Pad struct to 32-byte multiple   */

} SharedMemory_t;

/* One instance in memory, but shared_memory.c is compiled into BOTH images.
   Each linker then reserves the same address for it, and because .shared_ram
   is NOLOAD in both scripts neither image carries any content for it - there
   is nothing to conflict. Leaving it out of the M4 build instead does not
   link: an extern with no definition in that image is an undefined symbol,
   whatever the other core contains.                                        */
extern SharedMemory_t g_shared __attribute__((section(".shared_ram")));

/* Zeroes the block and publishes SHARED_MAGIC last, so a reader that sees the
   magic can trust everything else. M7 only, and before the M7 releases HSEM 0:
   .shared_ram is NOLOAD in both linker scripts, so nothing else initialises
   it and after a warm reset it still holds the previous run's values.      */
void Shared_Init(void);

#define SHARED_MAGIC   0x48375353UL   /* "H7SS" */

#include "hsem_bm.h"

/* ---- HSEM take / release (1-step lock, process ID 0) ---------------------
   Requires HSEM peripheral clock enabled on the calling core.
   Note: FastTake is 1-step locking; process ID is always 0.                */
#define SHARED_LOCK(id)     do { } while (HSEM_BM_FastTake(id) != HSEM_BM_OK)
#define SHARED_UNLOCK(id)   (void)HSEM_BM_Release((id), 0U)

#define SHARED_ADC_TRYLOCK_CYCLES  50000UL /* ~125 µs @ 400 MHz, ~250 µs @ 200 MHz */

/**
  * @brief  Attempts to acquire an HSEM with a bounded number of retries (spin loops).
  * @param  id      HSEM channel ID (0..31).
  * @param  retries Maximum spin iterations before giving up.
  * @return true if semaphore was successfully taken, false on timeout.
  */
bool Shared_TryLock(uint32_t id, uint32_t retries);

/**
  * @brief  Attempts to acquire an HSEM with a bounded timeout in DWT cycles.
  * @param  id         HSEM channel ID (0..31).
  * @param  max_cycles Maximum DWT cycles before giving up.
  * @return true if semaphore was successfully taken, false on timeout.
  */
bool Shared_TryLockCycles(uint32_t id, uint32_t max_cycles);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_MEMORY_H */
