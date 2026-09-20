/**
  ******************************************************************************
  * @file    shared_memory.c
  * @brief   The single definition of the M7 <-> M4 shared block.
  *
  *          Compiled into the CPU1 image only. CPU2 declares the same extern
  *          from the same header and reaches the same bytes because both
  *          linker scripts place .shared_ram at 0x38008000.
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

    /* Publish last. The barrier matters: without it the compiler or the store
       buffer may make the magic visible to CPU2 before the zeroing is, and
       CPU2 would accept a block that is still holding the previous run's
       values. __DMB() orders the stores; the volatile write stops the
       compiler reordering them. */
    __DMB();
    g_shared.magic = SHARED_MAGIC;
    __DSB();
}
