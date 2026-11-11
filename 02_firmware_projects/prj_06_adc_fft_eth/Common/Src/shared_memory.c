/**
  ******************************************************************************
  * @file    shared_memory.c
  * @brief   The shared data block between CPU1 (M7) and CPU2 (M4) in D3 SRAM4.
  ******************************************************************************
  */

#include "shared_memory.h"
#include "hsem_bm.h"
#include "stm32h7xx.h"

#include <string.h>

SharedMemory_t g_shared __attribute__((section(".shared_ram"), aligned(32)));

void Shared_Init(void)
{
    memset((void *)&g_shared, 0, sizeof(g_shared));

#ifndef VREFINT_CAL_ADDR
#define VREFINT_CAL_ADDR     ((const uint16_t *)(0x1FF1E860UL))
#endif
#ifndef TEMPSENSOR_CAL1_ADDR
#define TEMPSENSOR_CAL1_ADDR ((const uint16_t *)(0x1FF1E820UL))
#endif
#ifndef TEMPSENSOR_CAL2_ADDR
#define TEMPSENSOR_CAL2_ADDR ((const uint16_t *)(0x1FF1E840UL))
#endif

    g_shared.vrefint_cal    = *VREFINT_CAL_ADDR;
    g_shared.ts_cal1        = *TEMPSENSOR_CAL1_ADDR;
    g_shared.ts_cal2        = *TEMPSENSOR_CAL2_ADDR;
    g_shared.cal_valid      = 1U;

    g_shared.dsp.dsp_magic  = DSP_SHARED_MAGIC;
    g_shared.dsp.cmd_run    = 1U;
    g_shared.dsp.cmd_fs_hz  = BOARD_ADC_SAMPLING_RATE_HZ;
    g_shared.dsp.fs_hz      = BOARD_ADC_SAMPLING_RATE_HZ;
    g_shared.dsp.block_size = DSP_BLOCK_SIZE;
    g_shared.dsp.channel    = BOARD_ADC_CHANNEL;

    __DMB();
    g_shared.magic = SHARED_MAGIC;
    __DSB();
}

void Shared_Lock(uint32_t id)
{
    while (HSEM_BM_FastTake(id) != HSEM_BM_OK)
    {
        __NOP();
    }
}

void Shared_Unlock(uint32_t id)
{
    (void)HSEM_BM_Release(id, 0U);
}

bool Shared_TryLock(uint32_t id, uint32_t retries)
{
    while (retries > 0U)
    {
        if (HSEM_BM_FastTake(id) == HSEM_BM_OK)
        {
            return true;
        }
        retries--;
    }
    return false;
}

bool Shared_TryLockCycles(uint32_t id, uint32_t max_cycles)
{
    return (HSEM_BM_TakeTimeout(id, 0U, max_cycles) == HSEM_BM_OK);
}

/* ---- 00_gpio cross-core lock hook ---------------------------------------- */

#if defined(CORE_CM4)
#define SHARED_GPIO_LOCK_CYCLES   SHARED_GPIO_LOCK_CYCLES_CM4
#define SHARED_GPIO_FAIL_COUNT    g_shared.m4_gpio_lock_fail_count
#define SHARED_GPIO_HOLD_MAX      g_shared.m4_gpio_lock_hold_cycles_max
#else
#define SHARED_GPIO_LOCK_CYCLES   SHARED_GPIO_LOCK_CYCLES_CM7
#define SHARED_GPIO_FAIL_COUNT    g_shared.m7_gpio_lock_fail_count
#define SHARED_GPIO_HOLD_MAX      g_shared.m7_gpio_lock_hold_cycles_max
#endif

static uint32_t s_gpio_lock_start_cyc;

static bool shared_gpio_lock(void *ctx)
{
    (void)ctx;
    if (HSEM_BM_TakeTimeout(BOARD_GPIO_HSEM_ID, 0U, SHARED_GPIO_LOCK_CYCLES) == HSEM_BM_OK)
    {
        s_gpio_lock_start_cyc = DWT->CYCCNT;
        return true;
    }
    SHARED_GPIO_FAIL_COUNT++;
    return false;
}

static void shared_gpio_unlock(void *ctx)
{
    (void)ctx;
    (void)HSEM_BM_Release(BOARD_GPIO_HSEM_ID, 0U);
    uint32_t hold = DWT->CYCCNT - s_gpio_lock_start_cyc;
    if (hold > SHARED_GPIO_HOLD_MAX)
    {
        SHARED_GPIO_HOLD_MAX = hold;
    }
}

const GpioBm_SharedLock_t g_shared_gpio_lock = { shared_gpio_lock, shared_gpio_unlock, NULL };

void Shared_GpioLockInstall(void)
{
    GPIO_BM_SetSharedLock(&g_shared_gpio_lock);
}
