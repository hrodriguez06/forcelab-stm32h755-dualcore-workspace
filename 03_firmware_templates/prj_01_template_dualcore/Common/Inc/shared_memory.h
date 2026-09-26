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

#endif /* SHARED_MEMORY_H */
