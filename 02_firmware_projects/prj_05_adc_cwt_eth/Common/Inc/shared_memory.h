/**
  ******************************************************************************
  * @file    shared_memory.h
  * @brief   Shared M7 <-> M4 data block for STM32H755ZI (`prj_05_adc_cwt_eth`).
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
 * Semaphore Allocation & Ownership Table (RM0399 §11)
 * ============================================================================
 * ID  | Name                    | Owner / Master | Context / Procedure
 * ----+-------------------------+----------------+----------------------------
 *  0  | HSEM_ID_BOOT_SYNC       | CPU1 (CM7)     | Boot: CM7 1-step Take, CM4 WFE wait
 *  1  | HSEM_ID_SHARED_STATE    | Mutex (Dual)   | Superloop: Shared_TryLockCycles
 *  2  | HSEM_ID_M7_TO_M4_MBOX   | CPU1 (CM7)     | Task/Loop: Shared_TryLockCycles
 *  3  | HSEM_ID_M4_TO_M7_MBOX   | CPU2 (CM4)     | Superloop: Shared_TryLockCycles
 *  4  | HSEM_ID_ADC_DATA        | CPU2 (CM4)     | CM4 loop / CM7 CWT & ETH: Shared_TryLockCycles (100 us)
 *  5  | BOARD_GPIO_HSEM_ID      | Mutex (Dual)   | Pin lock: FastTake / Release
 * ============================================================================
 */
#define HSEM_ID_BOOT_SYNC       0U   /* reserved: M7 -> M4 boot release      */
#define HSEM_ID_SHARED_STATE    1U   /* guards SharedMemory_t.state          */
#define HSEM_ID_M7_TO_M4_MBOX   2U   /* guards m7_to_m4                      */
#define HSEM_ID_M4_TO_M7_MBOX   3U   /* guards m4_to_m7                      */
#define HSEM_ID_ADC_DATA        4U   /* guards adc telemetry data            */
#define BOARD_GPIO_HSEM_ID      5U   /* 00_gpio cross-core lock              */

#define SHARED_GPIO_LOCK_US          60U
#define SHARED_GPIO_LOCK_CYCLES_CM7  (SHARED_GPIO_LOCK_US * 400U)   /* 24,000 */
#define SHARED_GPIO_LOCK_CYCLES_CM4  (SHARED_GPIO_LOCK_US * 200U)   /* 12,000 */

#define SHARED_RAM_BASE         0x38008000UL /* SRAM4 base address (D3 domain) */
#define SHARED_RAM_SIZE         0x00008000UL /* 32 KB (RM0399 §2.3.2) */
#define SHARED_RAM_END          (SHARED_RAM_BASE + SHARED_RAM_SIZE)

#define SHARED_MBOX_SIZE        64U  /* multiple of 32 (cache line)          */
#define SHARED_ADC_WAVEFORM_SAMPLES 512U /* 512 samples = 51.2 ms @ 10 kSPS */

typedef struct __attribute__((aligned(32)))
{
    volatile uint16_t latest_raw;        /* Most recent ADC raw sample (16-bit) */
    volatile uint16_t average_raw;       /* Mean raw code across half-buffer    */
    volatile uint16_t min_raw;           /* Minimum raw code in half-buffer     */
    volatile uint16_t max_raw;           /* Maximum raw code in half-buffer     */
    volatile uint32_t millivolts;        /* Measured DC bias voltage (mV, ~1500)*/
    volatile uint32_t vpp_mv;            /* Peak-to-peak AC swing (mV)          */
    volatile int32_t  ac_min_mv;         /* Min signed AC voltage vs 1500 mV    */
    volatile int32_t  ac_max_mv;         /* Max signed AC voltage vs 1500 mV    */
    volatile int32_t  dc_offset_error_mv;/* millivolts - 1500 mV bias drift     */
    volatile uint32_t sample_count;      /* Total conversions processed         */
    volatile uint32_t dma_tc_count;      /* DMA Transfer-Complete counter       */
    volatile uint32_t dma_ht_count;      /* DMA Half-Transfer counter           */
    volatile uint32_t dma_error_count;   /* DMA error counter                   */
    volatile uint32_t half_overrun_count;/* Half-buffers dropped by consumer lag*/
    volatile uint32_t adc_overrun_count; /* ADC hardware overrun (OVR flag)     */
    volatile uint32_t channel;           /* Channel number (5 for PB1)          */
    volatile uint32_t sampling_rate_hz;  /* Nominal rate (500000 Hz)            */
    volatile uint32_t timestamp_ms;      /* CM4 timestamp (HAL tick)            */
    volatile uint32_t waveform_seq;      /* Monotonic waveform sequence ID      */
    volatile uint16_t waveform_len;      /* Number of valid samples (512)       */
    volatile uint16_t _reserved;         /* Alignment to 32-bit                 */
    volatile uint16_t waveform[SHARED_ADC_WAVEFORM_SAMPLES]; /* 512 raw samples (1024 B) */
    uint8_t           _pad_adc[16];      /* Pad SharedAdcData_t to 32B multiple (1120 B) */
} SharedAdcData_t;

_Static_assert((sizeof(SharedAdcData_t) % 32U) == 0U,
               "SharedAdcData_t must be a multiple of 32 bytes");

typedef struct __attribute__((aligned(32)))
{
    volatile uint32_t magic;          /* 0x48375353UL ("H7SS"), written by M7 */
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

    volatile uint32_t m7_adc_lock_fail_count;      /* Failures in Shared_TryLockCycles on CM7 */
    volatile uint32_t m4_adc_lock_fail_count;      /* Failures in Shared_TryLockCycles on CM4 */
    volatile uint32_t cpu2_wake_count;             /* WFE wakeups during boot handshake */
    volatile uint32_t m7_adc_lock_hold_cycles_max; /* Peak HSEM 4 hold time (DWT cycles @ 400 MHz) */
    volatile uint32_t m4_adc_lock_hold_cycles_max; /* Peak HSEM 4 hold time (DWT cycles @ 200 MHz) */

    /* 00_gpio cross-core lock fail counters and peak hold cycles */
    volatile uint32_t m7_gpio_lock_fail_count;
    volatile uint32_t m4_gpio_lock_fail_count;
    volatile uint32_t m7_gpio_lock_hold_cycles_max;
    volatile uint32_t m4_gpio_lock_hold_cycles_max;
    uint8_t           _pad2[20];                   /* Pad struct to 32-byte multiple */

} SharedMemory_t;

_Static_assert((sizeof(SharedMemory_t) % 32U) == 0U,
               "SharedMemory_t must be a multiple of 32 bytes");

extern SharedMemory_t g_shared __attribute__((section(".shared_ram")));

void Shared_Init(void);

#define SHARED_MAGIC   0x48375353UL   /* "H7SS" */

void Shared_Lock(uint32_t id);
void Shared_Unlock(uint32_t id);
#define SHARED_LOCK(id)     Shared_Lock(id)
#define SHARED_UNLOCK(id)   Shared_Unlock(id)

#define SHARED_ADC_TRYLOCK_US            100U
#define SHARED_ADC_TRYLOCK_CYCLES_CM7   (SHARED_ADC_TRYLOCK_US * 400U) /* 40,000 cycles @ 400 MHz */
#define SHARED_ADC_TRYLOCK_CYCLES_CM4   (SHARED_ADC_TRYLOCK_US * 200U) /* 20,000 cycles @ 200 MHz */
#if defined(CORE_CM4)
#define SHARED_ADC_TRYLOCK_CYCLES        SHARED_ADC_TRYLOCK_CYCLES_CM4
#else
#define SHARED_ADC_TRYLOCK_CYCLES        SHARED_ADC_TRYLOCK_CYCLES_CM7
#endif

bool Shared_TryLock(uint32_t id, uint32_t retries);
bool Shared_TryLockCycles(uint32_t id, uint32_t max_cycles);

#include "gpio_bm.h"

extern const GpioBm_SharedLock_t g_shared_gpio_lock;
void Shared_GpioLockInstall(void);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_MEMORY_H */
