/**
  ******************************************************************************
  * @file    shared_memory.c
  * @brief   The shared data block between CPU1 (M7) and CPU2 (M4).
  *
  *          Compiled into BOTH images (CPU1 and CPU2). Neither image carries
  *          content for it because .shared_ram is NOLOAD in both linker scripts,
  *          reserving 0x38008000 (SRAM4) without symbol or allocation conflict.
  *
  *          Why SRAM4 and not AXI SRAM: SRAM4 sits in the D3 domain, is not
  *          cached by the M7, and the M4 has no data cache at all. Neither
  *          core needs a clean or invalidate to see what the other wrote, so
  *          the whole class of "I wrote it but the other core reads stale
  *          data" bugs does not arise. The MPU region covering it is
  *          configured non-cacheable in mpu_config.c to keep it that way.
  ******************************************************************************
  */

#include "shared_memory.h"
#include "hsem_bm.h"
#include "gpio_bm.h"

/* For __DMB() / __DSB(). Included here rather than in shared_memory.h because
   that header is the golden copy shared with the documentation hub and is
   compiled into both cores: keeping it dependent on nothing but <stdint.h>
   means it can also be included by a host-side test or a tool. */
#include "stm32h7xx.h"

#include <string.h>

/* The section is NOLOAD in both linker scripts: it occupies no space in the
   flash image and the startup code does not zero it. That is deliberate -
   zeroing it from the CPU2 startup would race CPU1 - and it is exactly why
   Shared_Init() exists. */
SharedMemory_t g_shared __attribute__((section(".shared_ram")));

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

    g_shared.vrefint_cal = *VREFINT_CAL_ADDR;
    g_shared.ts_cal1     = *TEMPSENSOR_CAL1_ADDR;
    g_shared.ts_cal2     = *TEMPSENSOR_CAL2_ADDR;
    g_shared.cal_valid   = 1U;

    /* Publish last. The barrier matters: without it the compiler or the store
       buffer may make the magic visible to CPU2 before the zeroing is, and
       CPU2 would accept a block that is still holding the previous run's
       values. __DMB() orders the stores; the volatile write stops the
       compiler reordering them. */
    __DMB();
    g_shared.magic = SHARED_MAGIC;
    __DSB();
}

/* ---- 00_gpio cross-core lock hook ------------------------------------------ */

#if defined(CORE_CM4)
#define SHARED_GPIO_LOCK_CYCLES   SHARED_GPIO_LOCK_CYCLES_CM4
#define SHARED_GPIO_FAIL_COUNT    g_shared.m4_gpio_lock_fail_count
#define SHARED_GPIO_HOLD_MAX      g_shared.m4_gpio_lock_hold_cycles_max
#else
#define SHARED_GPIO_LOCK_CYCLES   SHARED_GPIO_LOCK_CYCLES_CM7
#define SHARED_GPIO_FAIL_COUNT    g_shared.m7_gpio_lock_fail_count
#define SHARED_GPIO_HOLD_MAX      g_shared.m7_gpio_lock_hold_cycles_max
#endif

/* DWT cycle counter snapshot taken at lock entry */
static uint32_t s_gpio_lock_start_cyc;

/* Called by gpio_bm with this core's interrupts already masked, at nesting
   level 1 only, so this core never holds the 1-step lock twice and
   TakeTimeout never sees BUSY_SELF because of the driver. CYCCNT keeps
   counting with PRIMASK set, so the bound holds inside the section. */
static bool shared_gpio_lock(void *ctx)
{
    (void)ctx;
    if (HSEM_BM_TakeTimeout(BOARD_GPIO_HSEM_ID, 0U, SHARED_GPIO_LOCK_CYCLES) == HSEM_BM_OK)
    {
        s_gpio_lock_start_cyc = DWT->CYCCNT;
        return true;
    }
    SHARED_GPIO_FAIL_COUNT++;   /* this core's own counter: no cross-core RMW */
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

