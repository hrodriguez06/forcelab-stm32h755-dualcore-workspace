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

/* One single instance, placed by the linker. Defined in shared_memory.c on
   the M7 side only; the M4 sees it through the same address.               */
extern SharedMemory_t g_shared __attribute__((section(".shared_ram")));

#define SHARED_MAGIC   0x48375353UL   /* "H7SS" */

/* ---- HSEM take / release (2-step lock, process ID 0) ---------------------
   Requires the HSEM clock: __HAL_RCC_HSEM_CLK_ENABLE();                    */
#define SHARED_LOCK(id)     do { } while (HAL_HSEM_FastTake(id) != HAL_OK)
#define SHARED_UNLOCK(id)   HAL_HSEM_Release((id), 0U)

#endif /* SHARED_MEMORY_H */
