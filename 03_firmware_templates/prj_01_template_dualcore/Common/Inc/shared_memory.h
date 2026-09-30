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

/* ---- HSEM identifiers ----------------------------------------------------
   HSEM 0 is reserved by ST for the classic boot handshake (M4 waits in
   HAL_PWREx_ClearPendingEvent/__WFE until M7 releases HSEM 0).             */
#define HSEM_ID_BOOT_SYNC       0U   /* reserved: M7 -> M4 boot release      */
#define HSEM_ID_SHARED_STATE    1U   /* guards SharedMemory_t.state          */
#define HSEM_ID_M7_TO_M4_MBOX   2U   /* guards m7_to_m4                      */
#define HSEM_ID_M4_TO_M7_MBOX   3U   /* guards m4_to_m7                      */
#define BOARD_GPIO_HSEM_ID      5U   /* 00_gpio cross-core lock: 1-step, taken
                                        only inside the gpio_bm section, so at
                                        most one context per core holds it
                                        (PRIMASK is set while it is held)    */

/* ---- 00_gpio lock limit ---------------------------------------------------
   How long a core waits for the other to finish one pin configuration. It is
   written in time and converted per core, because each core's DWT counts its
   own clock (10_hsem/README.md section 3.5). The value comes from the hold
   times measured by the selftest (00_gpio/README.md, B1): the longest hold is
   far below it, so a TIMEOUT means a stuck core, not contention.           */
#define SHARED_GPIO_LOCK_US            20U
#define SHARED_GPIO_LOCK_CYCLES_CM7   (SHARED_GPIO_LOCK_US * 400U)   /* 8 000 at 400 MHz */
#define SHARED_GPIO_LOCK_CYCLES_CM4   (SHARED_GPIO_LOCK_US * 200U)   /* 4 000 at 200 MHz */

#define SHARED_MBOX_SIZE        64U  /* multiple of 32 (cache line)          */

typedef struct
{
    volatile uint32_t magic;          /* 0xH755SHM sentinel, written by M7   */
    volatile uint32_t m7_heartbeat;   /* incremented by M7                   */
    volatile uint32_t m4_heartbeat;   /* incremented by M4                   */
    volatile uint32_t state;          /* application state, HSEM 1 guarded   */

    volatile uint32_t m7_to_m4_len;
    volatile uint8_t  m7_to_m4[SHARED_MBOX_SIZE];

    volatile uint32_t m4_to_m7_len;
    volatile uint8_t  m4_to_m7[SHARED_MBOX_SIZE];

    /* One counter per core, each written only by its own core, so neither is
       a cross-core read-modify-write. A non-zero value is a GPIO configuration
       that was refused with GPIO_BM_ERR_BUSY because the lock timed out.    */
    volatile uint32_t m7_gpio_lock_fail_count;
    volatile uint32_t m4_gpio_lock_fail_count;

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

#include "gpio_bm.h"

/* ---- 00_gpio cross-core lock hook ----------------------------------------
   The GPIO configuration registers (MODER, OTYPER, OSPEEDR, PUPDR, AFR) have
   one copy for both cores. gpio_bm, gpio_ll and gpio_hal run every write in a
   section that masks this core's interrupts and then calls this hook, which
   takes BOARD_GPIO_HSEM_ID with a bounded wait (HSEM_BM_TakeTimeout).

   Install it on each core after HSEM_BM_Init() and HSEM_BM_TimebaseStart(),
   before the first GPIO configuration that can overlap with the other core:
   on CPU1 before it releases CPU2, on CPU2 right after it wakes. Code that
   configures pins without the driver (HAL_GPIO_Init in an MSP, a raw MODER
   write) must be wrapped in GPIO_BM_SectionEnter/Exit to be covered.       */
extern const GpioBm_SharedLock_t g_shared_gpio_lock;
void Shared_GpioLockInstall(void);

#endif /* SHARED_MEMORY_H */
