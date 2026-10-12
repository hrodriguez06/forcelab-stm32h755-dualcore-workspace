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

/* ============================================================================
 * Semaphore Allocation & Ownership Table (RM0399 §11, S1 mitigation, M6, R-m2)
 * ============================================================================
 * ID  | Name                    | Owner / Master | Context / Procedure
 * ----+-------------------------+----------------+----------------------------
 *  0  | HSEM_ID_BOOT_SYNC       | CPU1 (CM7)     | Boot: CM7 1-step Take, CM4 WFE wait
 *  1  | HSEM_ID_SHARED_STATE    | Mutex (Dual)   | Superloop: Shared_TryLockCycles
 *  2  | HSEM_ID_M7_TO_M4_MBOX   | CPU1 (CM7)     | Task/Loop: Shared_TryLockCycles
 *  3  | HSEM_ID_M4_TO_M7_MBOX   | CPU2 (CM4)     | Superloop: Shared_TryLockCycles
 *  4  | HSEM_ID_ADC_DATA        | CPU2 (CM4)     | CM4 loop / CM7 ETH: Shared_TryLockCycles (100 µs)
 *  5  | BOARD_GPIO_HSEM_ID      | Mutex (Dual)   | Pin lock: FastTake / Release
 * 6..23 | Reserved              | —              | Reserved for future shared drivers
 * 24  | HSEM_TEST_M7_LOCK1      | Dual (Selftest)| Boot H0/H3/H7/H10 (CM7), H1 1-step, H1b; cleared H8
 * 25  | HSEM_TEST_M7_LOCK2      | Dual (Selftest)| Boot H10 (CM7), H1 2-step; cleared in H8
 * 26  | HSEM_TEST_ID_NOTIFY     | Dual (Selftest)| H9 cross-core notification (NVIC IRQ); cleared in H8
 * 27  | HSEM_TEST_ID_TIMEOUT    | Dual (Selftest)| H4 bounded timeout (10 ms) CM7 & CM4; cleared in H8
 * 28  | HSEM_TEST_M4_LOCK1      | Dual (Selftest)| Boot H0/H3/H7/H10 (CM4); H8 canary held by CM7
 * 29  | HSEM_TEST_M4_LOCK2      | CPU2 (Selftest)| Boot H10 (CM4); B1 raw timing baseline
 * 30  | HSEM_TEST_M4_NOTIFY     | CPU2 (Selftest)| H10 notification enable/disable on CM4
 * 31  | Reserved (Selftest)     | —              | Reserved
 * ============================================================================
 */
#define HSEM_ID_BOOT_SYNC       0U   /* reserved: M7 -> M4 boot release      */
#define HSEM_ID_SHARED_STATE    1U   /* guards SharedMemory_t.state          */
#define HSEM_ID_M7_TO_M4_MBOX   2U   /* guards m7_to_m4                      */
#define HSEM_ID_M4_TO_M7_MBOX   3U   /* guards m4_to_m7                      */
#define HSEM_ID_ADC_DATA        4U   /* guards adc telemetry data            */
#define BOARD_GPIO_HSEM_ID      5U   /* 00_gpio cross-core lock: 1-step, taken only inside the gpio_bm section */
#define SHARED_GPIO_LOCK_US     60U  /* >= 10x the longest hold measured */
#define SHARED_GPIO_LOCK_CYCLES_CM7  (SHARED_GPIO_LOCK_US * 400U)   /* 24 000 */
#define SHARED_GPIO_LOCK_CYCLES_CM4  (SHARED_GPIO_LOCK_US * 200U)   /* 12 000 */
#define SHARED_RAM_BASE         0x38008000UL /* SRAM4 base address (D3 domain) */
#define SHARED_RAM_SIZE         0x00008000UL /* 32 KB (RM0399 §2.3.2) */
#define SHARED_RAM_END          (SHARED_RAM_BASE + SHARED_RAM_SIZE)

#ifndef GPIO_TEST_SHARED_ADDR
#define GPIO_TEST_SHARED_ADDR   0x3800A000UL /* 00_gpio dual-core selftest SRAM4 area */
#endif

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

    volatile uint32_t m7_adc_lock_fail_count;      /* Failures in Shared_TryLockCycles on CM7 (m3) */
    volatile uint32_t m4_adc_lock_fail_count;      /* Failures in Shared_TryLockCycles on CM4 (m3) */
    volatile uint32_t cpu2_wake_count;             /* WFE wakeups during boot handshake (B2) */
    volatile uint32_t m7_adc_lock_hold_cycles_max; /* Peak HSEM 4 hold time (DWT cycles @ 400 MHz) (R-B1) */
    volatile uint32_t m4_adc_lock_hold_cycles_max; /* Peak HSEM 4 hold time (DWT cycles @ 200 MHz) (R-B1) */

    /* 00_gpio cross-core lock fail counters (H1) and peak hold cycles (H1, decision 3) */
    volatile uint32_t m7_gpio_lock_fail_count;
    volatile uint32_t m4_gpio_lock_fail_count;
    volatile uint32_t m7_gpio_lock_hold_cycles_max;
    volatile uint32_t m4_gpio_lock_hold_cycles_max;
    uint8_t           _pad2[20];                   /* Pad struct to 32-byte multiple (1312 B total) */

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

/* ---- HSEM take / release (m4: pure C function declarations, no hsem_bm.h) -- */
void Shared_Lock(uint32_t id);
void Shared_Unlock(uint32_t id);
#define SHARED_LOCK(id)     Shared_Lock(id)
#define SHARED_UNLOCK(id)   Shared_Unlock(id)

/* R-B1: Derived from measured SRAM4 retention under contention:
 *   - CM4 holds HSEM 4 up to 7,525 cycles @ 200 MHz (37.6 µs) in ADC_Acquisition_Process.
 *   - CM7 holds HSEM 4 up to 17,069 cycles @ 400 MHz (42.7 µs) during WAVE serialization.
 *   - Limit calibrated to 100 µs (>= 2x measured retention margin), eliminating contention drops.
 */
#define SHARED_ADC_TRYLOCK_US            100U
#define SHARED_ADC_TRYLOCK_CYCLES_CM7   (SHARED_ADC_TRYLOCK_US * 400U) /* 40,000 cycles @ 400 MHz */
#define SHARED_ADC_TRYLOCK_CYCLES_CM4   (SHARED_ADC_TRYLOCK_US * 200U) /* 20,000 cycles @ 200 MHz */
#if defined(CORE_CM4)
#define SHARED_ADC_TRYLOCK_CYCLES        SHARED_ADC_TRYLOCK_CYCLES_CM4
#else
#define SHARED_ADC_TRYLOCK_CYCLES        SHARED_ADC_TRYLOCK_CYCLES_CM7
#endif

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

#include "gpio_bm.h"

/* ---- 00_gpio cross-core lock hook ----------------------------------------
   The GPIO configuration registers (MODER, OTYPER, OSPEEDR, PUPDR, AFR) have
   one copy for both cores. gpio_bm, gpio_ll and gpio_hal run every write in a
   section that masks this core's interrupts and then calls this hook, which
   takes BOARD_GPIO_HSEM_ID with a bounded wait (HSEM_BM_TakeTimeout).

   Install it on each core after HSEM_BM_Init() and HSEM_BM_TimebaseStart(),
   before the first GPIO configuration that can overlap with the other core:
   on CPU1 before it releases CPU2, on CPU2 right after it wakes.           */
extern const GpioBm_SharedLock_t g_shared_gpio_lock;
void Shared_GpioLockInstall(void);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_MEMORY_H */
