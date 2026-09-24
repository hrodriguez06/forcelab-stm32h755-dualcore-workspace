/**
  ******************************************************************************
  * @file    dma_selftest.c
  * @brief   DMA driver self-test implementation for CM7 (T8 D-cache coherency).
  ******************************************************************************
  */

#include "dma_selftest.h"
#include "dma_bm.h"
#include "dma_mem_bm.h"
#include "app_main.h"
#include "app_log.h"
#include <string.h>

__attribute__((section(".axi_ram"), aligned(32))) static uint32_t s_tx_buf[64];
__attribute__((section(".axi_ram"), aligned(32))) static uint32_t s_rx_buf[64];

void DMA_SelfTest_CM7_Run(void)
{
    /* T8: Verify PrepareRx fails on unaligned pointer or unaligned length */
    bool unaligned_ptr_fail = !DMA_MEM_BM_PrepareRx((void *)((uintptr_t)s_rx_buf + 4U), 32U);
    bool unaligned_len_fail = !DMA_MEM_BM_PrepareRx(s_rx_buf, 30U);

    if (!unaligned_ptr_fail || !unaligned_len_fail)
    {
        APP_LOG("[m7_dma_test] T8_ALIGNMENT: FAIL (ptr=%d, len=%d)", unaligned_ptr_fail, unaligned_len_fail);
        return;
    }

    for (uint32_t i = 0; i < 64; i++)
    {
        s_tx_buf[i] = 0x12340000UL + i;
        s_rx_buf[i] = 0xDEADBEEFUL;
    }

    /* Clean TX buffer out to physical RAM */
    DMA_MEM_BM_CleanForTx(s_tx_buf, sizeof(s_tx_buf));

    /* Prepare RX buffer (clean + invalidate) */
    if (!DMA_MEM_BM_PrepareRx(s_rx_buf, sizeof(s_rx_buf)))
    {
        APP_LOG("[m7_dma_test] T8_PREPARE_RX: FAIL");
        return;
    }

    /* Configure DMA2_Stream0 for M2M transfer */
    DMA_Stream_TypeDef *s = DMA2_Stream0;
    DMA_BM_DeinitStream(s);

    DmaBm_ConfigEx_t cfg = {
        .base = {
            .stream           = s,
            .request_id       = 0U,
            .direction        = DMA_BM_DIR_MEM_TO_MEM,
            .periph_data_size = DMA_BM_DATA_SIZE_WORD,
            .mem_data_size    = DMA_BM_DATA_SIZE_WORD,
            .periph_inc       = true,
            .mem_inc          = true,
            .circular         = false,
            .priority         = DMA_BM_PRIORITY_HIGH,
            .enable_fifo      = true,
            .fifo_threshold   = DMA_BM_FIFO_TH_FULL,
            .periph_addr      = (uint32_t)s_tx_buf,
            .mem_addr         = (uint32_t)s_rx_buf,
            .buffer_length    = 64U
        },
        .mem1_addr       = 0U,
        .mem_burst       = DMA_BM_BURST_INCR4,
        .periph_burst    = DMA_BM_BURST_INCR4,
        .periph_flow_ctrl= false,
        .fail_if_busy    = true,
        .stop_spin_limit = 10000U
    };

    if (!DMA_BM_InitStreamEx(&cfg))
    {
        APP_LOG("[m7_dma_test] T8_INIT: FAIL");
        return;
    }

    if (!DMA_BM_StartEx(s))
    {
        APP_LOG("[m7_dma_test] T8_START: FAIL");
        return;
    }

    uint32_t spin = 100000U;
    while (((DMA_BM_GetFlags(s) & DMA_BM_FLAG_TC) == 0U) && (spin > 0U))
    {
        spin--;
    }

    uint32_t flags = DMA_BM_TakeFlags(s);
    DMA_BM_Stop(s);
    DMA_BM_DeinitStream(s);

    if ((flags & DMA_BM_FLAG_TC) == 0U)
    {
        APP_LOG("[m7_dma_test] T8_TRANSFER: FAIL flags=0x%lX", (unsigned long)flags);
        return;
    }

    /* Complete RX buffer processing (invalidate to fetch from physical RAM) */
    if (!DMA_MEM_BM_CompleteRx(s_rx_buf, sizeof(s_rx_buf)))
    {
        APP_LOG("[m7_dma_test] T8_COMPLETE_RX: FAIL");
        return;
    }

    if (memcmp(s_tx_buf, s_rx_buf, sizeof(s_tx_buf)) != 0)
    {
        APP_LOG("[m7_dma_test] T8_MEMCMP: FAIL (data mismatch)");
        return;
    }

    APP_LOG("[m7_dma_test] T8_DCACHE_COHERENCY: PASS");
}
