/**
  ******************************************************************************
  * @file    dma_selftest.c
  * @brief   DMA driver self-test implementation for CM7 (T8-T11).
  ******************************************************************************
  */

#include "dma_selftest.h"
#include "dma_bm.h"
#include "dma_hal.h"
#include "dma_mem_bm.h"
#include "app_main.h"
#include "app_log.h"
#include <string.h>

__attribute__((section(".axi_ram"), aligned(32))) static uint32_t s_tx_buf[64];
__attribute__((section(".axi_ram"), aligned(32))) static uint32_t s_rx_buf[64];

/* Buffers for T11 (BDMA): SRAM4 (.d3_ram) and SRAM1 (.dma_buf) */
__attribute__((section(".d3_ram"), aligned(32))) static uint32_t s_bdma_src[32];
__attribute__((section(".d3_ram"), aligned(32))) static uint32_t s_bdma_dst[32];
__attribute__((section(".dma_buf"), aligned(32))) static uint32_t s_sram1_buf[32];

/* Buffer for T10 (USART3 DMA TX) */
__attribute__((section(".axi_ram"), aligned(32))) static char s_t10_msg[80];

/* Callback counter for T9 */
static volatile uint32_t s_t9_tc_callback_count = 0U;

static void T9_XferCpltCallback(DMA_HandleTypeDef *hdma)
{
    (void)hdma;
    s_t9_tc_callback_count++;
}

void DMA2_Stream1_IRQHandler(void)
{
    DMA_HAL_IRQDispatch(DMA2_Stream1);
}

/* ---- T8: D-Cache Coherency & Alignment Rejection ------------------------ */
static bool Test_T8(void)
{
    /* 1. Alignment rejection on unaligned pointer or unaligned length */
    bool unaligned_ptr_fail = !DMA_MEM_BM_PrepareRx((void *)((uintptr_t)s_rx_buf + 4U), 32U);
    bool unaligned_len_fail = !DMA_MEM_BM_PrepareRx(s_rx_buf, 30U);

    if (!unaligned_ptr_fail || !unaligned_len_fail)
    {
        return false;
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
        return false;
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
        return false;
    }

    if (!DMA_BM_StartEx(s))
    {
        return false;
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
        return false;
    }

    /* Complete RX buffer processing (invalidate to fetch from physical RAM) */
    if (!DMA_MEM_BM_CompleteRx(s_rx_buf, sizeof(s_rx_buf)))
    {
        return false;
    }

    return (memcmp(s_tx_buf, s_rx_buf, sizeof(s_tx_buf)) == 0);
}

/* ---- T9: DMA_HAL_MemCopy & HAL_DMA_Start_IT with Callbacks -------------- */
static bool Test_T9(void)
{
    for (uint32_t i = 0; i < 64; i++)
    {
        s_tx_buf[i] = 0x55AA0000UL + i;
        s_rx_buf[i] = 0U;
    }

    /* 1. DMA_HAL_MemCopy test on DMA2_Stream0 */
    DMA_HandleTypeDef hdma_m2m;
    memset(&hdma_m2m, 0, sizeof(hdma_m2m));

    DmaHal_Config_t cfg_m2m = {
        .instance       = DMA2_Stream0,
        .request        = DMA_REQUEST_MEM2MEM,
        .direction      = DMA_MEMORY_TO_MEMORY,
        .periph_inc     = DMA_PINC_ENABLE,
        .mem_inc        = DMA_MINC_ENABLE,
        .periph_align   = DMA_PDATAALIGN_WORD,
        .mem_align      = DMA_MDATAALIGN_WORD,
        .mode           = DMA_NORMAL,
        .priority       = DMA_PRIORITY_HIGH,
        .fifo_mode      = DMA_FIFOMODE_ENABLE,
        .fifo_threshold = DMA_FIFO_THRESHOLD_FULL,
        .mem_burst      = DMA_MBURST_SINGLE,
        .periph_burst   = DMA_PBURST_SINGLE,
        .enable_clock   = true,
        .irq_priority   = -1
    };

    if (DMA_HAL_Init(&hdma_m2m, &cfg_m2m) != HAL_OK)
    {
        return false;
    }

    if (DMA_HAL_MemCopy(&hdma_m2m, s_rx_buf, s_tx_buf, sizeof(s_tx_buf), 1000U) != HAL_OK)
    {
        DMA_HAL_DeInit(&hdma_m2m);
        return false;
    }

    if (memcmp(s_tx_buf, s_rx_buf, sizeof(s_tx_buf)) != 0)
    {
        DMA_HAL_DeInit(&hdma_m2m);
        return false;
    }
    DMA_HAL_DeInit(&hdma_m2m);

    /* 1b. DMA_HAL_MemCopy with PSIZE != MSIZE (WORD -> BYTE) (X1) */
    memset(&hdma_m2m, 0, sizeof(hdma_m2m));
    DmaHal_Config_t cfg_m2m_w2b = {
        .instance       = DMA2_Stream0,
        .request        = DMA_REQUEST_MEM2MEM,
        .direction      = DMA_MEMORY_TO_MEMORY,
        .periph_inc     = DMA_PINC_ENABLE,
        .mem_inc        = DMA_MINC_ENABLE,
        .periph_align   = DMA_PDATAALIGN_WORD,
        .mem_align      = DMA_MDATAALIGN_BYTE,
        .mode           = DMA_NORMAL,
        .priority       = DMA_PRIORITY_HIGH,
        .fifo_mode      = DMA_FIFOMODE_ENABLE,
        .fifo_threshold = DMA_FIFO_THRESHOLD_FULL,
        .mem_burst      = DMA_MBURST_SINGLE,
        .periph_burst   = DMA_PBURST_SINGLE,
        .enable_clock   = true,
        .irq_priority   = -1
    };

    if (DMA_HAL_Init(&hdma_m2m, &cfg_m2m_w2b) != HAL_OK)
    {
        return false;
    }

    for (uint32_t i = 0; i < 16; i++)
    {
        s_tx_buf[i] = 0xA1B2C3D4UL + i;
    }
    memset(s_rx_buf, 0, sizeof(s_rx_buf));
    /* Place canary at word index 16 (offset 64 bytes) */
    s_rx_buf[16] = 0x55AA55AAUL;

    /* Y1: Flush the entire buffer (including the canary at offset 64) from
     * D-Cache to RAM so the DMA sees the canary in physical memory. Without
     * this, the canary stays dirty in the cache and the post-DMA read would
     * hit the cached (unchanged) value, defeating the overflow check. */
    DMA_MEM_BM_PrepareRx(s_rx_buf, sizeof(s_rx_buf));

    if (DMA_HAL_MemCopy(&hdma_m2m, s_rx_buf, s_tx_buf, 64U, 1000U) != HAL_OK)
    {
        DMA_HAL_DeInit(&hdma_m2m);
        return false;
    }

    /* Y1: Invalidate the entire buffer so the CPU re-reads from RAM,
     * picking up whatever the DMA actually wrote (or didn't write). */
    DMA_MEM_BM_CompleteRx(s_rx_buf, sizeof(s_rx_buf));

    if (memcmp(s_tx_buf, s_rx_buf, 64) != 0)
    {
        DMA_HAL_DeInit(&hdma_m2m);
        return false;
    }
    if (s_rx_buf[16] != 0x55AA55AAUL) /* Canary intact: no overflow */
    {
        DMA_HAL_DeInit(&hdma_m2m);
        return false;
    }
    DMA_HAL_DeInit(&hdma_m2m);

    /* 1c. Rejection: items > 65535 in DMA_HAL_MemCopy (X2) */
    memset(&hdma_m2m, 0, sizeof(hdma_m2m));
    DmaHal_Config_t cfg_m2m_b = cfg_m2m;
    cfg_m2m_b.periph_align = DMA_PDATAALIGN_BYTE;
    cfg_m2m_b.mem_align    = DMA_MDATAALIGN_BYTE;
    if (DMA_HAL_Init(&hdma_m2m, &cfg_m2m_b) != HAL_OK)
    {
        return false;
    }
    /* 65536 bytes with byte alignment -> 65536 items > 65535 */
    if (DMA_HAL_MemCopy(&hdma_m2m, s_rx_buf, s_tx_buf, 65536U, 1000U) != HAL_ERROR)
    {
        DMA_HAL_DeInit(&hdma_m2m);
        return false;
    }
    DMA_HAL_DeInit(&hdma_m2m);

    /* 2. HAL_DMA_Start_IT with IRQ callback dispatch on DMA2_Stream1 */
    for (uint32_t i = 0; i < 64; i++)
    {
        s_tx_buf[i] = 0x77880000UL + i;
        s_rx_buf[i] = 0U;
    }

    DMA_HandleTypeDef hdma_it;
    memset(&hdma_it, 0, sizeof(hdma_it));

    DmaHal_Config_t cfg_it = {
        .instance       = DMA2_Stream1,
        .request        = DMA_REQUEST_MEM2MEM,
        .direction      = DMA_MEMORY_TO_MEMORY,
        .periph_inc     = DMA_PINC_ENABLE,
        .mem_inc        = DMA_MINC_ENABLE,
        .periph_align   = DMA_PDATAALIGN_WORD,
        .mem_align      = DMA_MDATAALIGN_WORD,
        .mode           = DMA_NORMAL,
        .priority       = DMA_PRIORITY_HIGH,
        .fifo_mode      = DMA_FIFOMODE_ENABLE,
        .fifo_threshold = DMA_FIFO_THRESHOLD_FULL,
        .mem_burst      = DMA_MBURST_SINGLE,
        .periph_burst   = DMA_PBURST_SINGLE,
        .enable_clock   = true,
        .irq_priority   = 5
    };

    if (DMA_HAL_Init(&hdma_it, &cfg_it) != HAL_OK)
    {
        return false;
    }

    hdma_it.XferCpltCallback = T9_XferCpltCallback;
    s_t9_tc_callback_count = 0U;

    DMA_MEM_BM_CleanForTx(s_tx_buf, sizeof(s_tx_buf));
    if (!DMA_MEM_BM_PrepareRx(s_rx_buf, sizeof(s_rx_buf)))
    {
        DMA_HAL_DeInit(&hdma_it);
        return false;
    }

    if (HAL_DMA_Start_IT(&hdma_it, (uint32_t)s_tx_buf, (uint32_t)s_rx_buf, 64U) != HAL_OK)
    {
        DMA_HAL_DeInit(&hdma_it);
        return false;
    }

    uint32_t spin = 200000U;
    while ((s_t9_tc_callback_count == 0U) && (spin > 0U))
    {
        spin--;
    }

    DMA_HAL_DeInit(&hdma_it);
    DMA_MEM_BM_CompleteRx(s_rx_buf, sizeof(s_rx_buf));

    if (s_t9_tc_callback_count != 1U)
    {
        return false;
    }

    return (memcmp(s_tx_buf, s_rx_buf, sizeof(s_tx_buf)) == 0);
}

/* ---- T10: USART3 DMA TX & TRBUFF read-back ------------------------------ */
static bool Test_T10(void)
{
    DMA_HandleTypeDef hdma_usart;
    memset(&hdma_usart, 0, sizeof(hdma_usart));

    DmaHal_Config_t cfg_usart = {
        .instance       = DMA2_Stream2,
        .request        = DMA_REQUEST_USART3_TX,
        .direction      = DMA_MEMORY_TO_PERIPH,
        .periph_inc     = DMA_PINC_DISABLE,
        .mem_inc        = DMA_MINC_ENABLE,
        .periph_align   = DMA_PDATAALIGN_BYTE,
        .mem_align      = DMA_MDATAALIGN_BYTE,
        .mode           = DMA_NORMAL,
        .priority       = DMA_PRIORITY_LOW,
        .fifo_mode      = DMA_FIFOMODE_DISABLE,
        .fifo_threshold = DMA_FIFO_THRESHOLD_HALFFULL,
        .mem_burst      = DMA_MBURST_SINGLE,
        .periph_burst   = DMA_PBURST_SINGLE,
        .enable_clock   = true,
        .irq_priority   = -1
    };

    hdma_usart.Instance                 = cfg_usart.instance;
    hdma_usart.Init.Request             = cfg_usart.request;
    hdma_usart.Init.Direction           = cfg_usart.direction;
    hdma_usart.Init.PeriphInc           = cfg_usart.periph_inc;
    hdma_usart.Init.MemInc              = cfg_usart.mem_inc;
    hdma_usart.Init.PeriphDataAlignment = cfg_usart.periph_align;
    hdma_usart.Init.MemDataAlignment    = cfg_usart.mem_align;
    hdma_usart.Init.Mode                = cfg_usart.mode;
    hdma_usart.Init.Priority            = cfg_usart.priority;
    hdma_usart.Init.FIFOMode            = cfg_usart.fifo_mode;
    hdma_usart.Init.FIFOThreshold       = cfg_usart.fifo_threshold;
    hdma_usart.Init.MemBurst            = cfg_usart.mem_burst;
    hdma_usart.Init.PeriphBurst         = cfg_usart.periph_burst;

    __HAL_RCC_DMA2_CLK_ENABLE();
    if (HAL_DMA_Init(&hdma_usart) != HAL_OK)
    {
        return false;
    }

    /* GATE F3 / X4: Read CR immediately after HAL_DMA_Init() and BEFORE any CheckErrata
     * to verify that HAL_DMA_Init applied the TRBUFF workaround (bit 20). */
    DMA_Stream_TypeDef *s = (DMA_Stream_TypeDef *)hdma_usart.Instance;
    if ((s->CR & DMA_BM_CR_TRBUFF) == 0U)
    {
        (void)HAL_DMA_DeInit(&hdma_usart);
        return false;
    }

    /* Verify DMA_HAL_CheckErrata returns HAL_OK on idle stream */
    if (DMA_HAL_CheckErrata(&hdma_usart) != HAL_OK)
    {
        (void)HAL_DMA_DeInit(&hdma_usart);
        return false;
    }

    const char *text = "[m7_dma_test] T10_USART3_DMA: PASS (TRBUFF verified)\r\n";
    uint32_t len = 0U;
    while ((text[len] != '\0') && (len < sizeof(s_t10_msg) - 1U))
    {
        s_t10_msg[len] = text[len];
        len++;
    }
    s_t10_msg[len] = '\0';

    DMA_MEM_BM_CleanForTx(s_t10_msg, len);

    /* Enable USART3 DMA TX */
    USART3->CR3 |= USART_CR3_DMAT;

    if (HAL_DMA_Start(&hdma_usart, (uint32_t)s_t10_msg, (uint32_t)&(USART3->TDR), len) != HAL_OK)
    {
        DMA_HAL_DeInit(&hdma_usart);
        return false;
    }

    if (HAL_DMA_PollForTransfer(&hdma_usart, HAL_DMA_FULL_TRANSFER, 500U) != HAL_OK)
    {
        (void)HAL_DMA_Abort(&hdma_usart);
        DMA_HAL_DeInit(&hdma_usart);
        return false;
    }

    DMA_HAL_DeInit(&hdma_usart);
    return true;
}

/* ---- T11: BDMA Channel 0 M2M & SRAM1 Rejection -------------------------- */
static bool Test_T11(void)
{
    DMA_HandleTypeDef hdma_bdma;
    memset(&hdma_bdma, 0, sizeof(hdma_bdma));

    DmaHal_Config_t cfg_bdma = {
        .instance       = BDMA_Channel0,
        .request        = BDMA_REQUEST_MEM2MEM,
        .direction      = DMA_MEMORY_TO_MEMORY,
        .periph_inc     = DMA_PINC_ENABLE,
        .mem_inc        = DMA_MINC_ENABLE,
        .periph_align   = DMA_PDATAALIGN_WORD,
        .mem_align      = DMA_MDATAALIGN_WORD,
        .mode           = DMA_NORMAL,
        .priority       = DMA_PRIORITY_LOW,
        .fifo_mode      = DMA_FIFOMODE_DISABLE,
        .fifo_threshold = 0U,
        .mem_burst      = 0U,
        .periph_burst   = 0U,
        .enable_clock   = true,
        .irq_priority   = -1
    };

    if (DMA_HAL_Init(&hdma_bdma, &cfg_bdma) != HAL_OK)
    {
        return false;
    }

    /* 1. Valid M2M in SRAM4 (.d3_ram) */
    for (uint32_t i = 0; i < 32; i++)
    {
        s_bdma_src[i] = 0xCAFE0000UL + i;
        s_bdma_dst[i] = 0U;
    }

    if (DMA_HAL_MemCopy(&hdma_bdma, s_bdma_dst, s_bdma_src, sizeof(s_bdma_src), 1000U) != HAL_OK)
    {
        DMA_HAL_DeInit(&hdma_bdma);
        return false;
    }

    if (memcmp(s_bdma_src, s_bdma_dst, sizeof(s_bdma_src)) != 0)
    {
        DMA_HAL_DeInit(&hdma_bdma);
        return false;
    }

    /* 2. Rejection test: Buffer in SRAM1 (.dma_buf) is unreachable by BDMA */
    HAL_StatusTypeDef rej = DMA_HAL_MemCopy(&hdma_bdma, s_bdma_dst, s_sram1_buf, sizeof(s_sram1_buf), 1000U);
    DMA_HAL_DeInit(&hdma_bdma);

    return (rej == HAL_ERROR);
}

/* ---- Public Runner ------------------------------------------------------ */
void DMA_SelfTest_CM7_Run(void)
{
#ifdef FW_GIT_REV
    APP_LOG("[m7_dma_test] rev=%s", FW_GIT_REV);
#endif
    bool t8 = Test_T8();
    APP_LOG("[m7_dma_test] T8_DCACHE_COHERENCY: %s", t8 ? "PASS" : "FAIL");

    bool t9 = Test_T9();
    APP_LOG("[m7_dma_test] T9_HAL_MEMCOPY_IT: %s", t9 ? "PASS" : "FAIL");

    bool t10 = Test_T10();
    APP_LOG("[m7_dma_test] T10_USART3_DMA: %s", t10 ? "PASS" : "FAIL");

    bool t11 = Test_T11();
    APP_LOG("[m7_dma_test] T11_BDMA_SRAM4: %s", t11 ? "PASS" : "FAIL");

    bool all_pass = t8 && t9 && t10 && t11;
    APP_LOG("[m7_dma_test] OVERALL: %s (T8-T11)", all_pass ? "PASS" : "FAIL");
}
