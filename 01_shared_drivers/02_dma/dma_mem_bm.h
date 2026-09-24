/**
  ******************************************************************************
  * @file    dma_mem_bm.h
  * @brief   Bare-metal memory alignment, cache maintenance, and domain reachability
  *          helpers for STM32H755 DMA controllers.
  *
  *          Tier: *_bm (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Header-only, CMSIS-only, all static inline functions.
  ******************************************************************************
  */

#ifndef DMA_MEM_BM_H
#define DMA_MEM_BM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DMA_MEM_BM_LINE          32U
#define DMA_MEM_BM_ALIGN         __attribute__((aligned(32)))
#define DMA_MEM_BM_SIZE(bytes)   ((((uint32_t)(bytes) + 31U) / 32U) * 32U)

typedef enum {
    DMA_MEM_BM_MASTER_DMA  = 0,  /*!< DMA1 and DMA2 controllers */
    DMA_MEM_BM_MASTER_BDMA = 1   /*!< Basic DMA controller (D3 domain) */
} DmaMemBm_Master_t;

/**
  * @brief  Verifies if a memory address and buffer length can be reached by the given master.
  * @param  m     DMA master (DMA_MEM_BM_MASTER_DMA or DMA_MEM_BM_MASTER_BDMA).
  * @param  addr  Base memory address.
  * @param  bytes Buffer length in bytes.
  * @return true if memory region is valid and reachable, false otherwise.
  */
static inline bool DMA_MEM_BM_IsReachable(DmaMemBm_Master_t m, uint32_t addr, uint32_t bytes)
{
    if (bytes == 0U)
    {
        return false;
    }

    uint32_t end = addr + bytes;
    if (end < addr)
    {
        return false; /* 32-bit unsigned overflow */
    }

    if (m == DMA_MEM_BM_MASTER_DMA)
    {
        /* DMA1 / DMA2 rejection list:
         * Cannot access Cortex-M7 tightly-coupled memories:
         * - ITCM: [0x00000000, 0x00010000) (64 KB)
         * - DTCM: [0x20000000, 0x20020000) (128 KB)
         */
        if ((addr < 0x00010000U) && (end > 0x00000000U))
        {
            return false;
        }
        if ((addr < 0x20020000U) && (end > 0x20000000U))
        {
            return false;
        }
        return true;
    }
    else if (m == DMA_MEM_BM_MASTER_BDMA)
    {
        /* BDMA allow list (RM0399 §2 & §17, D3 32-bit AHB bus matrix):
         * - SRAM4:         [0x38000000, 0x38010000) (64 KB)
         * - Backup SRAM:   [0x38800000, 0x38801000) (4 KB)
         * - D3 Peripherals:[0x58000000, 0x58030000)
         */
        if ((addr >= 0x38000000U) && (end <= 0x38010000U))
        {
            return true;
        }
        if ((addr >= 0x38800000U) && (end <= 0x38801000U))
        {
            return true;
        }
        if ((addr >= 0x58000000U) && (end <= 0x58030000U))
        {
            return true;
        }
        return false;
    }

    return false;
}

/**
  * @brief  Verifies if pointer and length are both aligned to 32-byte cache line boundary.
  * @param  p     Pointer to buffer.
  * @param  bytes Buffer length in bytes.
  * @return true if 32-byte aligned, false otherwise.
  */
static inline bool DMA_MEM_BM_IsLineAligned(const void *p, uint32_t bytes)
{
    return (((uintptr_t)p & 0x1FU) == 0U) && ((bytes & 0x1FU) == 0U);
}

/**
  * @brief  Cleans D-Cache lines before DMA TX (CPU -> Memory -> Peripheral).
  *         Safe to round out to entire cache lines.
  * @param  p     Pointer to buffer.
  * @param  bytes Number of bytes to clean.
  */
static inline void DMA_MEM_BM_CleanForTx(const void *p, uint32_t bytes)
{
    if ((p == NULL) || (bytes == 0U))
    {
        return;
    }

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        uintptr_t start = (uintptr_t)p & ~((uintptr_t)0x1FU);
        uintptr_t end = (((uintptr_t)p + bytes + 31U) & ~((uintptr_t)0x1FU));
        SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
        __DMB();
    }
#endif
}

/**
  * @brief  Prepares buffer before starting DMA RX (Clean + Invalidate).
  * @param  p     Pointer to buffer (must be 32-byte line aligned).
  * @param  bytes Buffer length in bytes (must be multiple of 32).
  * @return true on success, false if unaligned.
  */
static inline bool DMA_MEM_BM_PrepareRx(void *p, uint32_t bytes)
{
    if ((p == NULL) || (bytes == 0U))
    {
        return false;
    }
    if (!DMA_MEM_BM_IsLineAligned(p, bytes))
    {
        return false;
    }

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        SCB_CleanInvalidateDCache_by_Addr((uint32_t *)p, (int32_t)bytes);
        __DMB();
    }
#endif
    return true;
}

/**
  * @brief  Completes buffer processing after DMA RX finishes (Invalidate D-Cache).
  * @param  p     Pointer to buffer (must be 32-byte line aligned).
  * @param  bytes Buffer length in bytes (must be multiple of 32).
  * @return true on success, false if unaligned.
  */
static inline bool DMA_MEM_BM_CompleteRx(void *p, uint32_t bytes)
{
    if ((p == NULL) || (bytes == 0U))
    {
        return false;
    }
    if (!DMA_MEM_BM_IsLineAligned(p, bytes))
    {
        return false;
    }

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        SCB_InvalidateDCache_by_Addr((uint32_t *)p, (int32_t)bytes);
        __DMB();
    }
#endif
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* DMA_MEM_BM_H */
