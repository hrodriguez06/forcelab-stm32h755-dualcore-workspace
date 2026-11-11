/**
  ******************************************************************************
  * @file    shared_memory.h
  * @brief   Inter-core shared memory contract in D3 SRAM4 (0x38008000, 32 KB)
  *          for prj_06_adc_fft_eth (Dual-Core 4096-pt FFT + 16-Band db4 WPT).
  *
  * Both cores map the section `.shared_ram` to the same physical address via
  * their linker scripts. CM7 marks the region non-cacheable in its MPU before
  * enabling D-Cache (see CM7/App/Src/mpu_config.c). CM4 has no data cache, so
  * regular volatile accesses + DMB are sufficient.
  *
  * Layout budget in `.shared_ram` (32,768 bytes available):
  *   - Ping-pong ADC blocks: 2 * 4096 * sizeof(uint16_t) = 16,384 bytes
  *   - Control, telemetry, FFT & WPT summary fields:        ~288 bytes
  *   - Total footprint:                                   ~16,672 bytes (< 52%)
  ******************************************************************************
  */

#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdint.h>
#include <stdbool.h>
#include "board_def.h"
#include "gpio_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Semaphore Allocation & Ownership Table (RM0399 §11)
 * ============================================================================
 * ID  | Name                    | Owner / Master | Context / Procedure
 * ----+-------------------------+----------------+----------------------------
 *  0  | HSEM_ID_BOOT_SYNC       | CPU1 (CM7)     | Boot: CM7 1-step Take, CM4 WFE wait
 *  1  | HSEM_ID_SHARED_STATE    | Mutex (Dual)   | General shared state lock
 *  4  | HSEM_ID_ADC_DATA        | Mutex (Dual)   | Protects SharedDspData_t updates
 *  5  | BOARD_GPIO_HSEM_ID      | Mutex (Dual)   | 00_gpio cross-core atomic lock
 * ============================================================================
 */
#define HSEM_ID_BOOT_SYNC           0U  /* CM7 -> CM4 boot release signal    */
#define HSEM_ID_SHARED_STATE        1U  /* General shared struct lock        */
#define HSEM_ID_ADC_DATA            4U  /* Protects SharedDspData_t updates  */
#define BOARD_GPIO_HSEM_ID          5U  /* Cross-core GPIO atomic RMW lock   */

#define SHARED_GPIO_LOCK_US          60U
#define SHARED_GPIO_LOCK_CYCLES_CM7  (SHARED_GPIO_LOCK_US * 400U)   /* 24,000 */
#define SHARED_GPIO_LOCK_CYCLES_CM4  (SHARED_GPIO_LOCK_US * 200U)   /* 12,000 */

#define SHARED_ADC_TRYLOCK_US            100U
#define SHARED_ADC_TRYLOCK_CYCLES_CM7   (SHARED_ADC_TRYLOCK_US * 400U) /* 40,000 */
#define SHARED_ADC_TRYLOCK_CYCLES_CM4   (SHARED_ADC_TRYLOCK_US * 200U) /* 20,000 */
#if defined(CORE_CM4)
#define SHARED_ADC_TRYLOCK_CYCLES        SHARED_ADC_TRYLOCK_CYCLES_CM4
#else
#define SHARED_ADC_TRYLOCK_CYCLES        SHARED_ADC_TRYLOCK_CYCLES_CM7
#endif

#define SHARED_RAM_BASE             0x38008000UL
#define SHARED_RAM_SIZE             0x00008000UL

#define SHARED_MAGIC                0x48375353UL  /* "H7SS" */
#define DSP_SHARED_MAGIC            0x44535032UL  /* "DSP2" */

/* ---- Shared DSP Status Flags --------------------------------------------- */
#define SHARED_ADC_FLAG_RUNNING     (1UL << 0)
#define SHARED_ADC_FLAG_CALIBRATED  (1UL << 1)
#define SHARED_ADC_FLAG_DATA_VALID  (1UL << 2)
#define SHARED_ADC_FLAG_OVERRUN     (1UL << 3)
#define SHARED_ADC_FLAG_DMA_ERROR   (1UL << 4)
#define SHARED_DSP_FLAG_FFT_READY   (1UL << 5)

/**
  * @brief  Shared ADC + FFT + WPT ping-pong control & telemetry block.
  *
  * Producer of raw samples & ADC stats: Cortex-M4 (CPU2).
  * Producer of FFT/WPT metrics & runtime commands: Cortex-M7 (CPU1).
  */
typedef struct __attribute__((aligned(32)))
{
    /* --- Handshake & Dynamic Acquisition Configuration (CM7 <-> CM4) --- */
    volatile uint32_t dsp_magic;           /* DSP_SHARED_MAGIC ('DSP2')      */
    volatile uint32_t cmd_seq;             /* Incremented by CM7 on new cmd  */
    volatile uint32_t cmd_ack;             /* Updated by CM4 to match seq    */
    volatile uint32_t cmd_run;             /* 1 = capture active, 0 = paused */
    volatile uint32_t cmd_fs_hz;           /* Requested sampling rate (Hz)   */

    /* --- Active Acquisition Parameters (reported by CM4) --- */
    volatile uint32_t fs_hz;               /* Effective TIM3 trigger rate Hz */
    volatile uint32_t block_size;          /* Always DSP_BLOCK_SIZE (4096)   */
    volatile uint32_t channel;             /* Active ADC1 channel (5 or 15)  */
    volatile uint32_t active_idx;          /* Buffer currently filled (0/1)  */
    volatile uint32_t ready_idx;           /* Last completed buffer (0/1)    */
    volatile uint32_t block_id;            /* Monotonic completed block seq  */
    volatile uint32_t t_block_ms;          /* CM4 HAL_GetTick() at block end */
    volatile uint32_t overrun_count;       /* Ping-pong half-buffer overruns */
    volatile uint32_t adc_overrun_count;   /* Hardware ADC OVR count         */
    volatile uint32_t dma_error_count;     /* DMA transfer error count       */
    volatile uint32_t dma_ht_count;        /* DMA Half-Transfer count        */
    volatile uint32_t dma_tc_count;        /* DMA Transfer-Complete count    */
    volatile uint32_t sample_count;        /* Cumulative samples acquired    */
    volatile uint32_t flags;               /* SHARED_ADC_FLAG_* bitmask      */

    /* --- Time-Domain ADC Block Statistics (computed by CM4) --- */
    volatile uint16_t latest_raw;          /* Last sample code (0..65535)    */
    volatile uint16_t average_raw;         /* Block mean code (0..65535)     */
    volatile uint16_t min_raw;             /* Block minimum code             */
    volatile uint16_t max_raw;             /* Block maximum code             */
    volatile uint32_t millivolts;          /* Block mean voltage (mV)        */
    volatile uint32_t vpp_mv;              /* Peak-to-peak voltage (mV)      */

    /* --- CM4 HardFault Diagnostic Trap --- */
    volatile uint32_t fault_flag;          /* 1 if CM4 entered HardFault     */
    volatile uint32_t fault_cfsr;          /* SCB->CFSR                      */
    volatile uint32_t fault_hfsr;          /* SCB->HFSR                      */
    volatile uint32_t fault_bfar;          /* SCB->BFAR                      */
    volatile uint32_t fault_pc;            /* Stacked PC at fault            */
    volatile uint32_t fault_lr;            /* Stacked LR at fault            */

    /* --- Published FFT Metrics (written by CM7 vDspProcessTask) --- */
    volatile uint32_t fft_ready;           /* 1 once first FFT+WPT completes */
    volatile uint32_t f0_hz_x10;           /* Fundamental freq (Hz * 10)     */
    volatile uint16_t f0_mag_mv;           /* Fundamental peak amplitude mV  */
    volatile uint16_t noise_floor_mv;      /* Estimated noise floor (mV)     */
    volatile int16_t  snr_db_x10;          /* Signal-to-Noise Ratio (dB*10)  */
    volatile uint16_t thd_pct_x10;         /* Total Harmonic Dist. (%*10)    */
    volatile uint16_t harm_mag_mv[DSP_MAX_HARMONICS]; /* H1..H10 (mV)        */
    volatile uint16_t dc_offset_mv;        /* FFT DC offset (mV)             */
    volatile uint16_t v_pp_mv;             /* FFT peak-to-peak (mV)          */

    /* --- Published 4-Level db4 WPT Metrics (written by CM7) --- */
    volatile uint16_t wpt_energy_pct_x10[WPT_NUM_BANDS]; /* Band energy %*10 */
    volatile uint16_t wpt_rms_mv[WPT_NUM_BANDS];         /* Band RMS (mV)    */
    volatile uint16_t wpt_transient_mag_mv;/* Peak Level-1 HF detail (mV)    */
    volatile uint16_t wpt_transient_sample;/* Sample index (0..4095) of peak */
    volatile uint32_t wpt_transient_count; /* Cumulative HF transients       */
    volatile uint16_t wpt_crest_x10;       /* Level-1 HF crest factor * 10   */
    volatile uint16_t wpt_entropy_norm_x1000; /* Normalized Shannon * 1000   */

    /* --- Ping-Pong Raw Sample Buffers (2 x 4096 x uint16_t = 16,384 B) --- */
    __attribute__((aligned(32)))
    volatile uint16_t raw_buffer[2][DSP_BLOCK_SIZE];
} SharedDspData_t;

/**
  * @brief  Top-level shared SRAM4 block at 0x38008000.
  */
typedef struct __attribute__((aligned(32)))
{
    volatile uint32_t magic;            /* Set to SHARED_MAGIC by CM7 at boot */
    volatile uint32_t m7_heartbeat;     /* Incremented by CM7 Heartbeat_Task  */
    volatile uint32_t m4_heartbeat;     /* Incremented by CM4 super-loop      */
    volatile uint32_t state;            /* Application state                  */

    volatile uint16_t vrefint_cal;      /* Factory VREFINT calibration word   */
    volatile uint16_t ts_cal1;          /* Factory TS_CAL1 calibration word   */
    volatile uint16_t ts_cal2;          /* Factory TS_CAL2 calibration word   */
    volatile uint16_t cal_valid;        /* 1 if calibration data populated    */

    volatile uint32_t cpu2_wake_count;  /* WFE wakeups during boot handshake  */
    volatile uint32_t m7_adc_lock_fail_count;
    volatile uint32_t m4_adc_lock_fail_count;
    volatile uint32_t m7_adc_lock_hold_cycles_max;
    volatile uint32_t m4_adc_lock_hold_cycles_max;
    volatile uint32_t m7_gpio_lock_fail_count;
    volatile uint32_t m4_gpio_lock_fail_count;
    volatile uint32_t m7_gpio_lock_hold_cycles_max;
    volatile uint32_t m4_gpio_lock_hold_cycles_max;

    SharedDspData_t   dsp;              /* Dual-core ADC + FFT + WPT state    */
} SharedMemory_t;

/* Compile-time verification that SharedMemory_t fits in the 32 KB .shared_ram */
_Static_assert(sizeof(SharedMemory_t) <= SHARED_RAM_SIZE,
               "SharedMemory_t exceeds the 32 KB .shared_ram section in SRAM4");
_Static_assert((sizeof(SharedMemory_t) % 32U) == 0U,
               "SharedMemory_t must be a multiple of 32 bytes");

extern SharedMemory_t g_shared __attribute__((section(".shared_ram")));

void Shared_Init(void);
void Shared_Lock(uint32_t id);
void Shared_Unlock(uint32_t id);
#define SHARED_LOCK(id)     Shared_Lock(id)
#define SHARED_UNLOCK(id)   Shared_Unlock(id)

bool Shared_TryLock(uint32_t id, uint32_t retries);
bool Shared_TryLockCycles(uint32_t id, uint32_t max_cycles);

extern const GpioBm_SharedLock_t g_shared_gpio_lock;
void Shared_GpioLockInstall(void);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_MEMORY_H */
