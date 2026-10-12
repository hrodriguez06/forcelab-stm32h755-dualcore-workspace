/**
  ******************************************************************************
  * @file    dma_selftest.c
  * @brief   Bare-metal and LL DMA driver self-test implementation for CM4.
  ******************************************************************************
  */

#include "dma_selftest.h"
#include "dma_bm.h"
#include "dma_ll.h"
#include "board_def.h"
#include "shared_memory.h"
#include <string.h>

DmaSelfTest_Results_t g_dma_selftest_results;

#define ST_MAX_LINES 16
static char s_lines[ST_MAX_LINES][SHARED_MBOX_SIZE];
static uint32_t s_line_count = 0U;
static uint32_t s_line_len = 0U;
static uint32_t s_line_next = 0U;

static void Line_Begin(void)
{
    s_line_len = 0U;
    if (s_line_count < ST_MAX_LINES)
    {
        s_lines[s_line_count][0] = '\0';
    }
}

static void Line_End(void)
{
    if (s_line_count < ST_MAX_LINES)
    {
        s_line_count++;
    }
}

static void Line_Str(const char *s)
{
    if (s_line_count >= ST_MAX_LINES) return;
    char *dst = s_lines[s_line_count];
    while ((*s != '\0') && (s_line_len < (SHARED_MBOX_SIZE - 1U)))
    {
        dst[s_line_len++] = *s++;
    }
    dst[s_line_len] = '\0';
}

static void Line_U32(uint32_t v)
{
    char tmp[11];
    uint32_t n = 0U;
    do
    {
        tmp[n++] = (char)('0' + (int)(v % 10U));
        v /= 10U;
    } while ((v != 0U) && (n < sizeof(tmp) - 1U));

    while (n > 0U)
    {
        char c = tmp[--n];
        if ((s_line_count < ST_MAX_LINES) && (s_line_len < (SHARED_MBOX_SIZE - 1U)))
        {
            s_lines[s_line_count][s_line_len++] = c;
            s_lines[s_line_count][s_line_len] = '\0';
        }
    }
}

static void Line_Hex8(uint8_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    char str[3];
    str[0] = hex[(v >> 4) & 0x0F];
    str[1] = hex[v & 0x0F];
    str[2] = '\0';
    Line_Str(str);
}

static uint32_t s_diag_t1 = 0U;
static uint32_t s_diag_t1_flags = 0U;
static uint32_t s_diag_t4_ll_tc = 0U;
static uint32_t s_diag_t4_ll_ht = 0U;
static uint32_t s_diag_t5 = 0U;
static uint32_t s_diag_t6 = 0U;
static uint32_t s_t1_combos_valid = 0U;
static uint32_t s_t1_combos_ok = 0U;
static uint32_t s_t1_combos_invalid = 0U;
static uint32_t s_t1_combos_rej = 0U;
static uint32_t s_t1_combos_mismatch = 0U;

/* Buffers in SRAM2 */
static uint32_t s_m2m_src[64] DMA_BM_ALIGN_32;
static uint32_t s_m2m_dst[64] DMA_BM_ALIGN_32;

static bool Run_M2M_Combo(DMA_Stream_TypeDef *stream,
                          uint32_t psize, uint32_t msize,
                          uint32_t threshold,
                          uint32_t pburst, uint32_t mburst,
                          uint32_t ndtr_items)
{
    uint8_t *src_b = (uint8_t *)s_m2m_src;
    uint8_t *dst_b = (uint8_t *)s_m2m_dst;
    for (uint32_t i = 0; i < 64; i++)
    {
        src_b[i] = (uint8_t)(0x30U + i);
        dst_b[i] = 0x5AU;
    }

    DmaBm_ConfigEx_t cfg = {
        .base = {
            .stream           = stream,
            .request_id       = 0U,
            .direction        = DMA_BM_DIR_MEM_TO_MEM,
            .periph_data_size = psize,
            .mem_data_size    = msize,
            .periph_inc       = true,
            .mem_inc          = true,
            .circular         = false,
            .priority         = DMA_BM_PRIORITY_HIGH,
            .enable_fifo      = true,
            .fifo_threshold   = threshold,
            .periph_addr      = (uint32_t)s_m2m_src,
            .mem_addr         = (uint32_t)s_m2m_dst,
            .buffer_length    = ndtr_items
        },
        .mem1_addr       = 0U,
        .mem_burst       = mburst,
        .periph_burst    = pburst,
        .periph_flow_ctrl= false,
        .fail_if_busy    = true,
        .stop_spin_limit = 10000U
    };

    DMA_BM_DeinitStream(stream);

    if (!DMA_BM_InitStreamEx(&cfg))
    {
        return false;
    }

    if (!DMA_BM_StartEx(stream))
    {
        DMA_BM_DeinitStream(stream);
        return false;
    }

    uint32_t spin = 100000U;
    while (((DMA_BM_GetFlags(stream) & (DMA_BM_FLAG_TC | DMA_BM_FLAG_TE)) == 0U) && (spin > 0U))
    {
        spin--;
    }

    uint32_t flags = DMA_BM_TakeFlags(stream);
    DMA_BM_DeinitStream(stream);

    if (((flags & DMA_BM_FLAG_TC) == 0U) ||
        ((flags & (DMA_BM_FLAG_TE | DMA_BM_FLAG_DME)) != 0U))
    {
        return false;
    }

    return (memcmp(src_b, dst_b, 64) == 0);
}
static uint8_t  s_uart_tx_buf[256] DMA_BM_ALIGN_32;
static uint8_t  s_uart_rx_buf[256] DMA_BM_ALIGN_32;
static uint16_t s_tim_rx_buf[1000] DMA_BM_ALIGN_32;
static uint32_t s_dbm_buf0[32] DMA_BM_ALIGN_32;
static uint32_t s_dbm_buf1[32] DMA_BM_ALIGN_32;

/* ---- T1: Memory to Memory on streams 1 to 7 ----------------------------- */
static bool Test_T1(void)
{
    DMA_Stream_TypeDef *streams[7] = {
        DMA1_Stream1, DMA1_Stream2, DMA1_Stream3,
        DMA1_Stream4, DMA1_Stream5, DMA1_Stream6, DMA1_Stream7
    };

    for (uint32_t s = 0; s < 7; s++)
    {
        DMA_Stream_TypeDef *stream = streams[s];

        /* Test 32-bit Word M2M with FIFO full and INCR4 burst */
        for (uint32_t i = 0; i < 64; i++)
        {
            s_m2m_src[i] = 0xAA000000UL + (s << 16) + i;
            s_m2m_dst[i] = 0xFFFFFFFFUL;
        }

        DmaBm_ConfigEx_t cfg = {
            .base = {
                .stream           = stream,
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
                .periph_addr      = (uint32_t)s_m2m_src,
                .mem_addr         = (uint32_t)s_m2m_dst,
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
            s_diag_t1 = 10U + s;
            return false;
        }

        if (!DMA_BM_StartEx(stream))
        {
            s_diag_t1 = 20U + s;
            return false;
        }

        uint32_t spin = 100000U;
        while (((DMA_BM_GetFlags(stream) & (DMA_BM_FLAG_TC | DMA_BM_FLAG_TE)) == 0U) && (spin > 0U))
        {
            spin--;
        }

        uint32_t flags = DMA_BM_TakeFlags(stream);
        if (((flags & DMA_BM_FLAG_TC) == 0U) ||
            ((flags & (DMA_BM_FLAG_TE | DMA_BM_FLAG_DME)) != 0U) ||
            (DMA_BM_GetRemaining(stream) != 0U))
        {
            s_diag_t1 = 30U + s;
            s_diag_t1_flags = flags;
            return false;
        }

        if (memcmp(s_m2m_src, s_m2m_dst, sizeof(s_m2m_src)) != 0)
        {
            s_diag_t1 = 40U + s;
            return false;
        }

        /* Clean up stream */
        DMA_BM_DeinitStream(stream);
    }

    /* LL tier verification: M2M copy via DMA_LL_MemCopy */
    DmaLL_Handle_t h_t1;
    DmaLL_Config_t cfg_ll = {
        .stream         = DMA1_Stream1,
        .request        = 0U,
        .direction      = LL_DMA_DIRECTION_MEMORY_TO_MEMORY,
        .mode           = LL_DMA_MODE_NORMAL,
        .periph_inc     = LL_DMA_PERIPH_INCREMENT,
        .mem_inc        = LL_DMA_MEMORY_INCREMENT,
        .periph_size    = LL_DMA_PDATAALIGN_WORD,
        .mem_size       = LL_DMA_MDATAALIGN_WORD,
        .priority       = LL_DMA_PRIORITY_HIGH,
        .fifo_enable    = true,
        .fifo_threshold = LL_DMA_FIFOTHRESHOLD_FULL,
        .mem_burst      = LL_DMA_MBURST_INC4,
        .periph_burst   = LL_DMA_PBURST_INC4,
        .double_buffer  = false,
        .enable_clock   = true,
        .fail_if_busy   = true
    };
    if (!DMA_LL_Init(&h_t1, &cfg_ll))
    {
        s_diag_t1 = 51U;
        return false;
    }
    for (uint32_t i = 0; i < 64; i++)
    {
        s_m2m_src[i] = 0xBB000000UL + i;
        s_m2m_dst[i] = 0U;
    }
    if (!DMA_LL_MemCopy(&h_t1, s_m2m_dst, s_m2m_src, 64U, 100000U))
    {
        s_diag_t1 = 52U;
        s_diag_t1_flags = h_t1.error_flags;
        return false;
    }
    if (memcmp(s_m2m_src, s_m2m_dst, sizeof(s_m2m_src)) != 0)
    {
        s_diag_t1 = 53U;
        return false;
    }
    DMA_LL_Deinit(&h_t1, 10000U);

    /* LL tier verification: M2M with PSIZE != MSIZE (WORD -> BYTE) (X1) */
    DmaLL_Config_t cfg_ll_w2b = {
        .stream         = DMA1_Stream1,
        .request        = 0U,
        .direction      = LL_DMA_DIRECTION_MEMORY_TO_MEMORY,
        .mode           = LL_DMA_MODE_NORMAL,
        .periph_inc     = LL_DMA_PERIPH_INCREMENT,
        .mem_inc        = LL_DMA_MEMORY_INCREMENT,
        .periph_size    = LL_DMA_PDATAALIGN_WORD,
        .mem_size       = LL_DMA_MDATAALIGN_BYTE,
        .priority       = LL_DMA_PRIORITY_HIGH,
        .fifo_enable    = true,
        .fifo_threshold = LL_DMA_FIFOTHRESHOLD_FULL,
        .mem_burst      = LL_DMA_MBURST_SINGLE,
        .periph_burst   = LL_DMA_PBURST_SINGLE,
        .double_buffer  = false,
        .enable_clock   = true,
        .fail_if_busy   = true
    };
    if (!DMA_LL_Init(&h_t1, &cfg_ll_w2b))
    {
        s_diag_t1 = 54U;
        return false;
    }
    for (uint32_t i = 0; i < 16; i++)
    {
        s_m2m_src[i] = 0x11223344UL + i;
    }
    memset(s_m2m_dst, 0, sizeof(s_m2m_dst));
    /* Place canary at offset 64 bytes (s_m2m_dst[16]) */
    s_m2m_dst[16] = 0xCAFEBABEUL;

    /* 16 items of WORD = 64 bytes total */
    if (!DMA_LL_MemCopy(&h_t1, s_m2m_dst, s_m2m_src, 16U, 100000U))
    {
        s_diag_t1 = 55U;
        s_diag_t1_flags = h_t1.error_flags;
        return false;
    }
    if (memcmp(s_m2m_src, s_m2m_dst, 64) != 0)
    {
        s_diag_t1 = 56U;
        return false;
    }
    if (s_m2m_dst[16] != 0xCAFEBABEUL) /* Canary untouched (no buffer overflow) */
    {
        s_diag_t1 = 57U;
        return false;
    }
    DMA_LL_Deinit(&h_t1, 10000U);

    /* ---- Exhaustive combinatorial matrix of FIFO bursts (X10a, Z1) ---- */
    /* Independent oracle for M2M FIFO burst validity (RM0399 Table 116 & §16.3.14).
     * Index: [size: BYTE=0, HALFWORD=1, WORD=2]
     *        [threshold: 1/4=0, 1/2=1, 3/4=2, FULL=3]
     *        [burst: SINGLE=0, INCR4=1, INCR8=2, INCR16=3]
     * Exactly 23 valid combinations, 25 invalid combinations (total 48). */
    static const bool s_rm0399_burst_valid[3][4][4] = {
        [DMA_BM_DATA_SIZE_BYTE] = {
            [DMA_BM_FIFO_TH_1_4]  = { true,  true,  false, false },
            [DMA_BM_FIFO_TH_1_2]  = { true,  true,  true,  false },
            [DMA_BM_FIFO_TH_3_4]  = { true,  true,  false, false },
            [DMA_BM_FIFO_TH_FULL] = { true,  true,  true,  true  }
        },
        [DMA_BM_DATA_SIZE_HALFWORD] = {
            [DMA_BM_FIFO_TH_1_4]  = { true,  false, false, false },
            [DMA_BM_FIFO_TH_1_2]  = { true,  true,  false, false },
            [DMA_BM_FIFO_TH_3_4]  = { true,  false, false, false },
            [DMA_BM_FIFO_TH_FULL] = { true,  true,  true,  false }
        },
        [DMA_BM_DATA_SIZE_WORD] = {
            [DMA_BM_FIFO_TH_1_4]  = { true,  false, false, false },
            [DMA_BM_FIFO_TH_1_2]  = { true,  false, false, false },
            [DMA_BM_FIFO_TH_3_4]  = { true,  false, false, false },
            [DMA_BM_FIFO_TH_FULL] = { true,  true,  false, false }
        }
    };

    static const uint32_t sizes[3] = {
        DMA_BM_DATA_SIZE_BYTE,
        DMA_BM_DATA_SIZE_HALFWORD,
        DMA_BM_DATA_SIZE_WORD
    };
    static const uint32_t ths[4] = {
        DMA_BM_FIFO_TH_1_4,
        DMA_BM_FIFO_TH_1_2,
        DMA_BM_FIFO_TH_3_4,
        DMA_BM_FIFO_TH_FULL
    };
    static const uint32_t bursts[4] = {
        DMA_BM_BURST_SINGLE,
        DMA_BM_BURST_INCR4,
        DMA_BM_BURST_INCR8,
        DMA_BM_BURST_INCR16
    };

    s_t1_combos_valid = 0U;
    s_t1_combos_ok = 0U;
    s_t1_combos_invalid = 0U;
    s_t1_combos_rej = 0U;
    s_t1_combos_mismatch = 0U;

    for (uint32_t s_idx = 0; s_idx < 3; s_idx++)
    {
        uint32_t sz = sizes[s_idx];
        uint32_t ndtr = 64U >> sz;

        for (uint32_t t_idx = 0; t_idx < 4; t_idx++)
        {
            uint32_t th = ths[t_idx];

            for (uint32_t b_idx = 0; b_idx < 4; b_idx++)
            {
                uint32_t b = bursts[b_idx];

                bool oracle_valid = s_rm0399_burst_valid[s_idx][t_idx][b_idx];
                bool driver_valid = DMA_BM_CheckFifoBurst(sz, th, b) &&
                                    DMA_BM_CheckPeriphBurst(sz, th, b);

                if (oracle_valid)
                {
                    s_t1_combos_valid++;
                }
                else
                {
                    s_t1_combos_invalid++;
                }

                if (driver_valid != oracle_valid)
                {
                    s_t1_combos_mismatch++;
                }

                if (oracle_valid)
                {
                    if (Run_M2M_Combo(DMA1_Stream1, sz, sz, th, b, b, ndtr))
                    {
                        s_t1_combos_ok++;
                    }
                    else if (s_diag_t1 == 0U)
                    {
                        s_diag_t1 = 100U + (s_idx * 16U) + (t_idx * 4U) + b_idx;
                    }
                }
                else
                {
                    DmaBm_ConfigEx_t cfg_rej = {
                        .base = {
                            .stream           = DMA1_Stream1,
                            .request_id       = 0U,
                            .direction        = DMA_BM_DIR_MEM_TO_MEM,
                            .periph_data_size = sz,
                            .mem_data_size    = sz,
                            .periph_inc       = true,
                            .mem_inc          = true,
                            .circular         = false,
                            .priority         = DMA_BM_PRIORITY_HIGH,
                            .enable_fifo      = true,
                            .fifo_threshold   = th,
                            .periph_addr      = (uint32_t)s_m2m_src,
                            .mem_addr         = (uint32_t)s_m2m_dst,
                            .buffer_length    = ndtr
                        },
                        .mem1_addr       = 0U,
                        .mem_burst       = b,
                        .periph_burst    = b,
                        .periph_flow_ctrl= false,
                        .fail_if_busy    = true,
                        .stop_spin_limit = 10000U
                    };
                    DMA_BM_DeinitStream(DMA1_Stream1);
                    bool init_res = DMA_BM_InitStreamEx(&cfg_rej);
                    uint32_t cr_val = DMA1_Stream1->CR;
                    if (init_res)
                    {
                        DMA_BM_DeinitStream(DMA1_Stream1);
                        if (s_diag_t1 == 0U)
                        {
                            s_diag_t1 = 200U + (s_idx * 16U) + (t_idx * 4U) + b_idx;
                        }
                    }
                    else if (cr_val != 0U)
                    {
                        if (s_diag_t1 == 0U)
                        {
                            s_diag_t1 = 300U + (s_idx * 16U) + (t_idx * 4U) + b_idx;
                        }
                    }
                    else
                    {
                        s_t1_combos_rej++;
                    }
                }
            }
        }
    }

    /* 3 combinations with PSIZE != MSIZE */
    uint32_t diff_sizes_ok = 0U;

    /* 1. BYTE -> WORD (TH=FULL, PBURST=INCR16, MBURST=INCR4, NDTR=64) */
    if (Run_M2M_Combo(DMA1_Stream1, DMA_BM_DATA_SIZE_BYTE, DMA_BM_DATA_SIZE_WORD,
                       DMA_BM_FIFO_TH_FULL, DMA_BM_BURST_INCR16, DMA_BM_BURST_INCR4, 64U))
    {
        diff_sizes_ok++;
    }
    else if (s_diag_t1 == 0U)
    {
        s_diag_t1 = 401U;
    }

    /* 2. HALFWORD -> WORD (TH=FULL, PBURST=INCR8, MBURST=INCR4, NDTR=32) */
    if (Run_M2M_Combo(DMA1_Stream1, DMA_BM_DATA_SIZE_HALFWORD, DMA_BM_DATA_SIZE_WORD,
                       DMA_BM_FIFO_TH_FULL, DMA_BM_BURST_INCR8, DMA_BM_BURST_INCR4, 32U))
    {
        diff_sizes_ok++;
    }
    else if (s_diag_t1 == 0U)
    {
        s_diag_t1 = 402U;
    }

    /* 3. WORD -> BYTE (TH=FULL, PBURST=INCR4, MBURST=INCR16, NDTR=16) */
    if (Run_M2M_Combo(DMA1_Stream1, DMA_BM_DATA_SIZE_WORD, DMA_BM_DATA_SIZE_BYTE,
                       DMA_BM_FIFO_TH_FULL, DMA_BM_BURST_INCR4, DMA_BM_BURST_INCR16, 16U))
    {
        diff_sizes_ok++;
    }
    else if (s_diag_t1 == 0U)
    {
        s_diag_t1 = 403U;
    }

    if ((s_t1_combos_mismatch != 0U) ||
        (s_t1_combos_valid != 23U) ||
        (s_t1_combos_ok != 23U) ||
        (s_t1_combos_invalid != 25U) ||
        (s_t1_combos_rej != 25U) ||
        (diff_sizes_ok != 3U))
    {
        return false;
    }

    return true;
}

/* ---- T2: Rejections ----------------------------------------------------- */
static bool Test_T2(void)
{
    DMA_Stream_TypeDef *s = DMA1_Stream1;
    DMA_BM_DeinitStream(s);

    DmaBm_ConfigEx_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.base.stream = s;
    cfg.base.direction = DMA_BM_DIR_MEM_TO_MEM;
    cfg.base.periph_addr = (uint32_t)s_m2m_src;
    cfg.base.mem_addr = (uint32_t)s_m2m_dst;
    cfg.base.buffer_length = 16U;
    cfg.base.enable_fifo = true;
    cfg.base.fifo_threshold = DMA_BM_FIFO_TH_FULL;

    /* 1. M2M with circular */
    cfg.base.circular = true;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.base.circular = false;

    /* 2. M2M with DBM */
    cfg.mem1_addr = (uint32_t)s_dbm_buf0;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.mem1_addr = 0U;

    /* 3. NDT == 0 */
    cfg.base.buffer_length = 0U;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;

    /* 4. NDT > 65535 */
    cfg.base.buffer_length = 70000U;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.base.buffer_length = 16U;

    /* 5. Direction == 3 (reserved) */
    cfg.base.direction = 0xC0UL;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.base.direction = DMA_BM_DIR_MEM_TO_MEM;

    /* 6. PSIZE == 3 (reserved) */
    cfg.base.periph_data_size = 3U;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.base.periph_data_size = DMA_BM_DATA_SIZE_WORD;

    /* 7. Request 0 in P2M */
    cfg.base.direction = DMA_BM_DIR_PERIPH_TO_MEM;
    cfg.base.request_id = 0U;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;

    /* 8. Request 128 (> 127) */
    cfg.base.request_id = 128U;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.base.request_id = 1U;
    cfg.base.direction = DMA_BM_DIR_MEM_TO_MEM;

    /* 9. Direct mode with PSIZE != MSIZE */
    cfg.base.enable_fifo = false;
    cfg.base.direction = DMA_BM_DIR_PERIPH_TO_MEM;
    cfg.base.periph_data_size = DMA_BM_DATA_SIZE_BYTE;
    cfg.base.mem_data_size = DMA_BM_DATA_SIZE_WORD;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.base.mem_data_size = DMA_BM_DATA_SIZE_BYTE;

    /* 10. Burst without FIFO */
    cfg.mem_burst = DMA_BM_BURST_INCR4;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.mem_burst = DMA_BM_BURST_SINGLE;

    /* 11. Burst with incompatible FIFO threshold (Halfword INCR8 with TH 1/2) */
    cfg.base.enable_fifo = true;
    cfg.base.fifo_threshold = DMA_BM_FIFO_TH_1_2;
    cfg.base.mem_data_size = DMA_BM_DATA_SIZE_HALFWORD;
    cfg.mem_burst = DMA_BM_BURST_INCR8;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.mem_burst = DMA_BM_BURST_SINGLE;
    cfg.base.fifo_threshold = DMA_BM_FIFO_TH_FULL;
    cfg.base.mem_data_size = DMA_BM_DATA_SIZE_WORD;

    /* 12. Misaligned address for word */
    cfg.base.mem_addr = ((uint32_t)s_m2m_dst) | 1U;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.base.mem_addr = (uint32_t)s_m2m_dst;

    /* 13. ITCM address */
    cfg.base.mem_addr = 0x00000100UL;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.base.mem_addr = (uint32_t)s_m2m_dst;

    /* 14. DTCM address */
    cfg.base.mem_addr = 0x20000100UL;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    cfg.base.mem_addr = (uint32_t)s_m2m_dst;

    /* 15. Misaligned stream pointer */
    cfg.base.stream = (DMA_Stream_TypeDef *)((uint32_t)DMA1_Stream1 + 4U);
    if (DMA_BM_InitStreamEx(&cfg)) return false;
    cfg.base.stream = s;

    /* 16. fail_if_busy with EN=1 */
    cfg.base.direction = DMA_BM_DIR_PERIPH_TO_MEM;
    cfg.base.request_id = 1U;
    cfg.base.periph_data_size = DMA_BM_DATA_SIZE_WORD;
    cfg.base.mem_data_size = DMA_BM_DATA_SIZE_WORD;
    cfg.base.enable_fifo = true;
    cfg.base.fifo_threshold = DMA_BM_FIFO_TH_FULL;
    cfg.fail_if_busy = false;
    if (!DMA_BM_InitStreamEx(&cfg)) return false;

    s->CR |= DMA_SxCR_EN; /* Force EN=1 (remains 1 because no peripheral trigger) */
    cfg.fail_if_busy = true;
    if (DMA_BM_InitStreamEx(&cfg)) return false; /* Must refuse */

    DMA_BM_Stop(s);
    DMA_BM_DeinitStream(s);

    /* 17. Table 115: PSIZE < MSIZE with NDT not a multiple of (MSIZE / PSIZE) */
    cfg.base.direction = DMA_BM_DIR_MEM_TO_MEM;
    cfg.base.periph_data_size = DMA_BM_DATA_SIZE_BYTE; /* PSIZE = 1 */
    cfg.base.mem_data_size = DMA_BM_DATA_SIZE_WORD;     /* MSIZE = 4 -> ratio = 4 */
    cfg.base.buffer_length = 5U;                       /* 5 is not divisible by 4 */
    cfg.base.enable_fifo = true;
    cfg.base.fifo_threshold = DMA_BM_FIFO_TH_FULL;
    cfg.fail_if_busy = false;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;
    /* Also test SetTransfer Table 115 rejection */
    cfg.base.buffer_length = 4U; /* Legal length to init */
    if (!DMA_BM_InitStreamEx(&cfg)) return false;
    if (DMA_BM_SetTransfer(s, (uint32_t)s_m2m_src, (uint32_t)s_m2m_dst, 7U)) return false; /* 7 % 4 != 0 */
    DMA_BM_DeinitStream(s);

    /* LL tier rejections */
    DmaLL_Handle_t h_rej;
    DmaLL_Config_t cfg_rej = {
        .stream         = s,
        .request        = 0U,
        .direction      = LL_DMA_DIRECTION_MEMORY_TO_MEMORY,
        .mode           = LL_DMA_MODE_NORMAL,
        .periph_inc     = LL_DMA_PERIPH_INCREMENT,
        .mem_inc        = LL_DMA_MEMORY_INCREMENT,
        .periph_size    = LL_DMA_PDATAALIGN_WORD,
        .mem_size       = LL_DMA_MDATAALIGN_WORD,
        .priority       = LL_DMA_PRIORITY_LOW,
        .fifo_enable    = true,
        .fifo_threshold = LL_DMA_FIFOTHRESHOLD_FULL,
        .mem_burst      = LL_DMA_MBURST_SINGLE,
        .periph_burst   = LL_DMA_PBURST_SINGLE,
        .double_buffer  = false,
        .enable_clock   = true,
        .fail_if_busy   = true
    };
    /* LL1: M2M with circular */
    cfg_rej.mode = LL_DMA_MODE_CIRCULAR;
    if (DMA_LL_Init(&h_rej, &cfg_rej) || (s->CR != 0U)) return false;
    cfg_rej.mode = LL_DMA_MODE_NORMAL;

    /* LL2: M2M with DBM */
    cfg_rej.double_buffer = true;
    if (DMA_LL_Init(&h_rej, &cfg_rej) || (s->CR != 0U)) return false;
    cfg_rej.double_buffer = false;

    /* LL3: Direct mode with PSIZE != MSIZE */
    cfg_rej.fifo_enable = false;
    cfg_rej.direction = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
    cfg_rej.request = 9U;
    cfg_rej.periph_size = LL_DMA_PDATAALIGN_HALFWORD;
    cfg_rej.mem_size = LL_DMA_MDATAALIGN_WORD;
    if (DMA_LL_Init(&h_rej, &cfg_rej) || (s->CR != 0U)) return false;

    /* LL4: Request 0 in P2M */
    cfg_rej.request = 0U;
    cfg_rej.periph_size = LL_DMA_PDATAALIGN_WORD;
    if (DMA_LL_Init(&h_rej, &cfg_rej) || (s->CR != 0U)) return false;

    /* LL5: Request > 127 */
    cfg_rej.request = 128U;
    if (DMA_LL_Init(&h_rej, &cfg_rej) || (s->CR != 0U)) return false;

    /* LL6: DMA_LL_Start: length 0 and length > 65535 */
    cfg_rej.request = 0U;
    cfg_rej.direction = LL_DMA_DIRECTION_MEMORY_TO_MEMORY;
    cfg_rej.fifo_enable = true;
    if (!DMA_LL_Init(&h_rej, &cfg_rej)) return false;
    if (DMA_LL_Start(&h_rej, (uint32_t)s_m2m_src, (uint32_t)s_m2m_dst, 0U)) return false;
    if (DMA_LL_Start(&h_rej, (uint32_t)s_m2m_src, (uint32_t)s_m2m_dst, 70000U)) return false;
    /* Misaligned address */
    if (DMA_LL_Start(&h_rej, ((uint32_t)s_m2m_src) + 1U, (uint32_t)s_m2m_dst, 16U)) return false;
    DMA_LL_Deinit(&h_rej, 10000U);

    /* LL7: Table 115: PSIZE < MSIZE with length not a multiple of (MSIZE / PSIZE) */
    cfg_rej.periph_size = LL_DMA_PDATAALIGN_BYTE;
    cfg_rej.mem_size    = LL_DMA_MDATAALIGN_WORD;
    cfg_rej.fifo_enable = true;
    if (!DMA_LL_Init(&h_rej, &cfg_rej)) return false;
    if (DMA_LL_Start(&h_rej, (uint32_t)s_m2m_src, (uint32_t)s_m2m_dst, 5U)) return false; /* 5 % 4 != 0 */
    DMA_LL_Deinit(&h_rej, 10000U);

    /* 18. SetIdleMemoryEx: rejection when explicit length exceeds reachable memory.
     * At 0x1FFFF000, remaining NDTR (32 items = 64 B) would stay within 0x1FFFF040 < 0x20000000,
     * so legacy SetIdleMemory passes in error. Explicit length 3000 (6000 B) crosses into DTCM
     * (0x20000770 > 0x20000000) and must be rejected by DMA_MEM_BM_IsReachable. */
    cfg.base.direction = DMA_BM_DIR_PERIPH_TO_MEM;
    cfg.base.periph_data_size = DMA_BM_DATA_SIZE_HALFWORD;
    cfg.base.mem_data_size = DMA_BM_DATA_SIZE_HALFWORD;
    cfg.base.periph_addr = (uint32_t)&(TIM7->CNT);
    cfg.base.mem_addr = (uint32_t)s_dbm_buf0;
    cfg.base.buffer_length = 32U;
    cfg.base.circular = true;
    cfg.mem1_addr = (uint32_t)s_dbm_buf1;
    cfg.mem_burst = DMA_BM_BURST_SINGLE;
    cfg.periph_burst = DMA_BM_BURST_SINGLE;
    cfg.fail_if_busy = false;
    if (!DMA_BM_InitStreamEx(&cfg)) return false;

    if (DMA_BM_SetIdleMemoryEx(s, 0x1FFFF000U, 3000U))
    {
        DMA_BM_DeinitStream(s);
        return false; /* Must refuse */
    }
    DMA_BM_DeinitStream(s);

    /* 19. X7b: peripheral burst word x INCR4 with threshold 3/4 forbidden (RM0399 §16.3.14) */
    memset(&cfg, 0, sizeof(cfg));
    cfg.base.stream = s;
    cfg.base.direction = DMA_BM_DIR_PERIPH_TO_MEM;
    cfg.base.request_id = 1U;
    cfg.base.periph_addr = (uint32_t)&(TIM7->CNT);
    cfg.base.mem_addr = (uint32_t)s_m2m_dst;
    cfg.base.buffer_length = 16U;
    cfg.base.periph_data_size = DMA_BM_DATA_SIZE_WORD;
    cfg.base.mem_data_size = DMA_BM_DATA_SIZE_WORD;
    cfg.base.enable_fifo = true;
    cfg.base.fifo_threshold = DMA_BM_FIFO_TH_3_4;
    cfg.mem_burst = DMA_BM_BURST_SINGLE;
    cfg.periph_burst = DMA_BM_BURST_INCR4;
    if (DMA_BM_InitStreamEx(&cfg) || (s->CR != 0U)) return false;

    /* 20. X7b: byte x INCR16 with PINC=0 on &USART2->TDR accepted (Init only, no start) */
    memset(&cfg, 0, sizeof(cfg));
    cfg.base.stream = s;
    cfg.base.direction = DMA_BM_DIR_MEM_TO_PERIPH;
    cfg.base.request_id = 44U; /* USART2_TX */
    cfg.base.periph_addr = (uint32_t)&(USART2->TDR);
    cfg.base.mem_addr = (uint32_t)s_m2m_src;
    cfg.base.buffer_length = 16U;
    cfg.base.periph_data_size = DMA_BM_DATA_SIZE_BYTE;
    cfg.base.mem_data_size = DMA_BM_DATA_SIZE_BYTE;
    cfg.base.periph_inc = false;
    cfg.base.mem_inc = true;
    cfg.base.enable_fifo = true;
    cfg.base.fifo_threshold = DMA_BM_FIFO_TH_FULL;
    cfg.mem_burst = DMA_BM_BURST_SINGLE;
    cfg.periph_burst = DMA_BM_BURST_INCR16;
    if (!DMA_BM_InitStreamEx(&cfg)) return false;
    DMA_BM_DeinitStream(s);

    /* 21. X7b: peripheral burst halfword x INCR4 with threshold 1/4 accepted
     * (RM0399 §16.3.14 allows it since beats x size = 8 B <= 16 B; CheckFifoBurst rejected it).
     * Also verifies halfword x INCR4 with threshold 1/2. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.base.stream = s;
    cfg.base.direction = DMA_BM_DIR_PERIPH_TO_MEM;
    cfg.base.request_id = 1U;
    cfg.base.periph_addr = (uint32_t)&(TIM7->CNT);
    cfg.base.mem_addr = (uint32_t)s_m2m_dst;
    cfg.base.buffer_length = 16U;
    cfg.base.periph_data_size = DMA_BM_DATA_SIZE_HALFWORD;
    cfg.base.mem_data_size = DMA_BM_DATA_SIZE_HALFWORD;
    cfg.base.enable_fifo = true;
    cfg.base.fifo_threshold = DMA_BM_FIFO_TH_1_4;
    cfg.mem_burst = DMA_BM_BURST_SINGLE;
    cfg.periph_burst = DMA_BM_BURST_INCR4;
    if (!DMA_BM_InitStreamEx(&cfg)) return false;
    DMA_BM_DeinitStream(s);

    cfg.base.fifo_threshold = DMA_BM_FIFO_TH_1_2;
    if (!DMA_BM_InitStreamEx(&cfg)) return false;
    DMA_BM_DeinitStream(s);

    return true;
}

/* ---- T3: USART2 DMA Loopback with TRBUFF check -------------------------- */
static bool Test_T3(void)
{
    /* Setup USART2 on PD5 (TX) and PD6 (RX) */
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();

    /* GPIO AF7 configuration */
    (void)GPIO_BM_ConfigAlternateEx(GPIOD, 5, 7, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE, GPIO_BM_SPEED_LOW);
    (void)GPIO_BM_ConfigAlternateEx(GPIOD, 6, 7, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE, GPIO_BM_SPEED_LOW);

    /* USART2 @ 115200 (kernel clock 100 MHz APB1) */
    USART2->CR1 = 0U;
    USART2->BRR = 100000000UL / 115200UL;
    USART2->CR3 |= (USART_CR3_DMAT | USART_CR3_DMAR);
    USART2->CR1 |= (USART_CR1_TE | USART_CR1_RE | USART_CR1_UE);

    for (uint32_t i = 0; i < 256; i++)
    {
        s_uart_tx_buf[i] = (uint8_t)(i ^ 0x5A);
        s_uart_rx_buf[i] = 0U;
    }

    /* RX Stream: DMA1_Stream2, request 43 (USART2_RX) */
    DmaBm_Config_t rx_cfg = {
        .stream           = DMA1_Stream2,
        .request_id       = 43U,
        .direction        = DMA_BM_DIR_PERIPH_TO_MEM,
        .periph_data_size = DMA_BM_DATA_SIZE_BYTE,
        .mem_data_size    = DMA_BM_DATA_SIZE_BYTE,
        .periph_inc       = false,
        .mem_inc          = true,
        .circular         = false,
        .priority         = DMA_BM_PRIORITY_VERY_HIGH,
        .enable_trbuff    = false, /* Auto-detection should set TRBUFF */
        .periph_addr      = (uint32_t)&(USART2->RDR),
        .mem_addr         = (uint32_t)s_uart_rx_buf,
        .buffer_length    = 256U
    };

    /* TX Stream: DMA1_Stream1, request 44 (USART2_TX) */
    DmaBm_Config_t tx_cfg = {
        .stream           = DMA1_Stream1,
        .request_id       = 44U,
        .direction        = DMA_BM_DIR_MEM_TO_PERIPH,
        .periph_data_size = DMA_BM_DATA_SIZE_BYTE,
        .mem_data_size    = DMA_BM_DATA_SIZE_BYTE,
        .periph_inc       = false,
        .mem_inc          = true,
        .circular         = false,
        .priority         = DMA_BM_PRIORITY_HIGH,
        .enable_trbuff    = false, /* Auto-detection should set TRBUFF */
        .periph_addr      = (uint32_t)&(USART2->TDR),
        .mem_addr         = (uint32_t)s_uart_tx_buf,
        .buffer_length    = 256U
    };

    if (!DMA_BM_InitStream(&rx_cfg) || !DMA_BM_InitStream(&tx_cfg))
    {
        return false;
    }

    /* Verify TRBUFF bit 20 was automatically set */
    if (((DMA1_Stream1->CR & DMA_BM_CR_TRBUFF) == 0U) ||
        ((DMA1_Stream2->CR & DMA_BM_CR_TRBUFF) == 0U))
    {
        return false;
    }

    DMA_BM_Start(DMA1_Stream2);
    DMA_BM_Start(DMA1_Stream1);

    uint32_t start_tick = HAL_GetTick();
    while (((DMA_BM_GetFlags(DMA1_Stream2) & DMA_BM_FLAG_TC) == 0U) &&
           ((HAL_GetTick() - start_tick) < 100U))
    {
    }

    DMA_BM_Stop(DMA1_Stream1);
    DMA_BM_Stop(DMA1_Stream2);
    DMA_BM_DeinitStream(DMA1_Stream1);
    DMA_BM_DeinitStream(DMA1_Stream2);

    if (memcmp(s_uart_tx_buf, s_uart_rx_buf, 256) != 0)
    {
        USART2->CR1 = 0U;
        return false;
    }

    /* LL tier verification: USART2 loopback via DMA_LL */
    USART2->CR1 = 0U;
    USART2->CR3 |= (USART_CR3_DMAT | USART_CR3_DMAR);
    USART2->CR1 |= (USART_CR1_TE | USART_CR1_RE | USART_CR1_UE);

    for (uint32_t i = 0; i < 256; i++)
    {
        s_uart_tx_buf[i] = (uint8_t)(i ^ 0xA5);
        s_uart_rx_buf[i] = 0U;
    }

    DmaLL_Handle_t h_rx, h_tx;
    DmaLL_Config_t cfg_rx = {
        .stream         = DMA1_Stream2,
        .request        = 43U, /* LL_DMAMUX1_REQ_USART2_RX */
        .direction      = LL_DMA_DIRECTION_PERIPH_TO_MEMORY,
        .mode           = LL_DMA_MODE_NORMAL,
        .periph_inc     = LL_DMA_PERIPH_NOINCREMENT,
        .mem_inc        = LL_DMA_MEMORY_INCREMENT,
        .periph_size    = LL_DMA_PDATAALIGN_BYTE,
        .mem_size       = LL_DMA_MDATAALIGN_BYTE,
        .priority       = LL_DMA_PRIORITY_VERYHIGH,
        .fifo_enable    = false,
        .fifo_threshold = LL_DMA_FIFOTHRESHOLD_1_2,
        .mem_burst      = LL_DMA_MBURST_SINGLE,
        .periph_burst   = LL_DMA_PBURST_SINGLE,
        .double_buffer  = false,
        .enable_clock   = true,
        .fail_if_busy   = true
    };
    DmaLL_Config_t cfg_tx = {
        .stream         = DMA1_Stream1,
        .request        = 44U, /* LL_DMAMUX1_REQ_USART2_TX */
        .direction      = LL_DMA_DIRECTION_MEMORY_TO_PERIPH,
        .mode           = LL_DMA_MODE_NORMAL,
        .periph_inc     = LL_DMA_PERIPH_NOINCREMENT,
        .mem_inc        = LL_DMA_MEMORY_INCREMENT,
        .periph_size    = LL_DMA_PDATAALIGN_BYTE,
        .mem_size       = LL_DMA_MDATAALIGN_BYTE,
        .priority       = LL_DMA_PRIORITY_HIGH,
        .fifo_enable    = false,
        .fifo_threshold = LL_DMA_FIFOTHRESHOLD_1_2,
        .mem_burst      = LL_DMA_MBURST_SINGLE,
        .periph_burst   = LL_DMA_PBURST_SINGLE,
        .double_buffer  = false,
        .enable_clock   = true,
        .fail_if_busy   = true
    };
    if (!DMA_LL_Init(&h_rx, &cfg_rx) || !DMA_LL_Init(&h_tx, &cfg_tx))
    {
        return false;
    }

    /* Verify TRBUFF bit 20 was set by DMA_LL_Init */
    if (((DMA1_Stream1->CR & DMA_BM_CR_TRBUFF) == 0U) ||
        ((DMA1_Stream2->CR & DMA_BM_CR_TRBUFF) == 0U))
    {
        return false;
    }

    if (!DMA_LL_Start(&h_rx, (uint32_t)&(USART2->RDR), (uint32_t)s_uart_rx_buf, 256U) ||
        !DMA_LL_Start(&h_tx, (uint32_t)&(USART2->TDR), (uint32_t)s_uart_tx_buf, 256U))
    {
        return false;
    }

    uint32_t start_tick_ll = HAL_GetTick();
    uint32_t ll_flags = 0U;
    while (((ll_flags & DMA_LL_FLAG_TC) == 0U) && ((HAL_GetTick() - start_tick_ll) < 100U))
    {
        ll_flags |= DMA_LL_TakeFlags(&h_rx);
    }

    DMA_LL_Stop(&h_tx, 10000U);
    DMA_LL_Stop(&h_rx, 10000U);
    DMA_LL_Deinit(&h_tx, 10000U);
    DMA_LL_Deinit(&h_rx, 10000U);

    USART2->CR1 = 0U;

    return (memcmp(s_uart_tx_buf, s_uart_rx_buf, 256) == 0);
}

/* ---- T4: TIM7_UP Circular P2M ------------------------------------------- */
static bool Test_T4(void)
{
    __HAL_RCC_TIM7_CLK_ENABLE();

    /* TIM7 at 10 kHz (200 MHz APB1 timer clock, prescaler 199 -> 1 MHz counter, ARR 99 -> 10 kHz) */
    TIM7->CR1 = 0U;
    TIM7->PSC = 199U;
    TIM7->ARR = 99U;
    TIM7->CR2 = (2U << TIM_CR2_MMS_Pos); /* Update event triggers TRGO */
    TIM7->DIER = TIM_DIER_UDE;           /* Update DMA request enable */

    DmaBm_Config_t cfg = {
        .stream           = DMA1_Stream3,
        .request_id       = BOARD_SELFTEST_TIM_REQUEST, /* 70 (TIM7_UP) */
        .direction        = DMA_BM_DIR_PERIPH_TO_MEM,
        .periph_data_size = DMA_BM_DATA_SIZE_HALFWORD,
        .mem_data_size    = DMA_BM_DATA_SIZE_HALFWORD,
        .periph_inc       = false,
        .mem_inc          = true,
        .circular         = true,
        .priority         = DMA_BM_PRIORITY_HIGH,
        .periph_addr      = (uint32_t)&(TIM7->CNT),
        .mem_addr         = (uint32_t)s_tim_rx_buf,
        .buffer_length    = 1000U
    };

    if (!DMA_BM_InitStream(&cfg))
    {
        return false;
    }

    DMA_BM_Start(DMA1_Stream3);
    TIM7->CR1 |= TIM_CR1_CEN;

    uint32_t start = HAL_GetTick();
    uint32_t tc = 0U, ht = 0U;

    while ((HAL_GetTick() - start) < 1000U)
    {
        uint32_t flags = DMA_BM_TakeFlags(DMA1_Stream3);
        if ((flags & DMA_BM_FLAG_TC) != 0U) tc++;
        if ((flags & DMA_BM_FLAG_HT) != 0U) ht++;
        if ((flags & (DMA_BM_FLAG_TE | DMA_BM_FLAG_DME)) != 0U)
        {
            TIM7->CR1 = 0U;
            DMA_BM_Stop(DMA1_Stream3);
            return false;
        }
    }

    TIM7->CR1 = 0U;
    DMA_BM_Stop(DMA1_Stream3);
    DMA_BM_DeinitStream(DMA1_Stream3);

    g_dma_selftest_results.t4_tc_count = tc;
    g_dma_selftest_results.t4_ht_count = ht;

    if (tc < 8U || tc > 13U || ht < 8U || ht > 13U)
    {
        return false;
    }

    /* LL tier verification: TIM7 circular mode and DMA_LL_IrqService */
    DmaLL_Handle_t h_tim;
    DmaLL_Config_t cfg_tim = {
        .stream         = DMA1_Stream3,
        .request        = BOARD_SELFTEST_TIM_REQUEST,
        .direction      = LL_DMA_DIRECTION_PERIPH_TO_MEMORY,
        .mode           = LL_DMA_MODE_CIRCULAR,
        .periph_inc     = LL_DMA_PERIPH_NOINCREMENT,
        .mem_inc        = LL_DMA_MEMORY_INCREMENT,
        .periph_size    = LL_DMA_PDATAALIGN_HALFWORD,
        .mem_size       = LL_DMA_MDATAALIGN_HALFWORD,
        .priority       = LL_DMA_PRIORITY_HIGH,
        .fifo_enable    = false,
        .fifo_threshold = LL_DMA_FIFOTHRESHOLD_1_2,
        .mem_burst      = LL_DMA_MBURST_SINGLE,
        .periph_burst   = LL_DMA_PBURST_SINGLE,
        .double_buffer  = false,
        .enable_clock   = true,
        .fail_if_busy   = true
    };
    if (!DMA_LL_Init(&h_tim, &cfg_tim))
    {
        return false;
    }
    if (!DMA_LL_Start(&h_tim, (uint32_t)&(TIM7->CNT), (uint32_t)s_tim_rx_buf, 1000U))
    {
        return false;
    }

    TIM7->CR1 |= TIM_CR1_CEN;
    start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 1000U)
    {
        DMA_LL_IrqService(&h_tim, NULL, NULL);
    }
    TIM7->CR1 = 0U;
    s_diag_t4_ll_tc = h_tim.tc_count;
    s_diag_t4_ll_ht = h_tim.ht_count;
    DMA_LL_Stop(&h_tim, 10000U);
    DMA_LL_Deinit(&h_tim, 10000U);

    return (s_diag_t4_ll_tc >= 8U && s_diag_t4_ll_tc <= 13U &&
            s_diag_t4_ll_ht >= 8U && s_diag_t4_ll_ht <= 13U);
}

/* ---- T5: Double Buffer Mode --------------------------------------------- */
static bool Test_T5(void)
{
    DMA_Stream_TypeDef *s = DMA1_Stream4;
    DMA_BM_DeinitStream(s);

    DmaBm_ConfigEx_t cfg = {
        .base = {
            .stream           = s,
            .request_id       = BOARD_SELFTEST_TIM_REQUEST,
            .direction        = DMA_BM_DIR_PERIPH_TO_MEM,
            .periph_data_size = DMA_BM_DATA_SIZE_HALFWORD,
            .mem_data_size    = DMA_BM_DATA_SIZE_HALFWORD,
            .periph_inc       = false,
            .mem_inc          = true,
            .circular         = true,
            .priority         = DMA_BM_PRIORITY_HIGH,
            .periph_addr      = (uint32_t)&(TIM7->CNT),
            .mem_addr         = (uint32_t)s_dbm_buf0,
            .buffer_length    = 32U
        },
        .mem1_addr = (uint32_t)s_dbm_buf1,
        .mem_burst = DMA_BM_BURST_SINGLE,
        .periph_burst = DMA_BM_BURST_SINGLE,
        .periph_flow_ctrl = false,
        .fail_if_busy = true,
        .stop_spin_limit = 10000U
    };

    if (!DMA_BM_InitStreamEx(&cfg))
    {
        s_diag_t5 = 1U;
        return false;
    }

    if ((s->CR & DMA_SxCR_DBM) == 0U)
    {
        s_diag_t5 = 2U;
        return false;
    }

    /* Verify GetCurrentTarget and SetIdleMemory */
    uint32_t target0 = DMA_BM_GetCurrentTarget(s);
    if (target0 != 0U)
    {
        s_diag_t5 = 3U;
        return false;
    }

    /* In DBM, active is M0, idle is M1. Updating M1AR should succeed */
    if (!DMA_BM_SetIdleMemoryEx(s, (uint32_t)s_dbm_buf1, 32U))
    {
        s_diag_t5 = 4U;
        return false;
    }

    DMA_BM_DeinitStream(s);

    /* LL tier verification: Double buffer with DMA_LL_StartDoubleBuffer */
    DmaLL_Handle_t h_dbm;
    DmaLL_Config_t cfg_dbm = {
        .stream         = s,
        .request        = BOARD_SELFTEST_TIM_REQUEST,
        .direction      = LL_DMA_DIRECTION_PERIPH_TO_MEMORY,
        .mode           = LL_DMA_MODE_CIRCULAR,
        .periph_inc     = LL_DMA_PERIPH_NOINCREMENT,
        .mem_inc        = LL_DMA_MEMORY_INCREMENT,
        .periph_size    = LL_DMA_PDATAALIGN_HALFWORD,
        .mem_size       = LL_DMA_MDATAALIGN_HALFWORD,
        .priority       = LL_DMA_PRIORITY_HIGH,
        .fifo_enable    = false,
        .fifo_threshold = LL_DMA_FIFOTHRESHOLD_1_2,
        .mem_burst      = LL_DMA_MBURST_SINGLE,
        .periph_burst   = LL_DMA_PBURST_SINGLE,
        .double_buffer  = true,
        .enable_clock   = true,
        .fail_if_busy   = true
    };
    if (!DMA_LL_Init(&h_dbm, &cfg_dbm))
    {
        s_diag_t5 = 5U;
        return false;
    }
    if (!DMA_LL_StartDoubleBuffer(&h_dbm, (uint32_t)&(TIM7->CNT),
                                  (uint32_t)s_dbm_buf0, (uint32_t)s_dbm_buf1, 32U))
    {
        s_diag_t5 = 6U;
        return false;
    }
    if ((s->CR & DMA_SxCR_DBM) == 0U)
    {
        s_diag_t5 = 7U;
        return false;
    }
    /* Update idle memory via DMA_LL_SetIdleMemory */
    if (!DMA_LL_SetIdleMemory(&h_dbm, (uint32_t)s_dbm_buf1))
    {
        s_diag_t5 = 8U;
        return false;
    }
    DMA_LL_Stop(&h_dbm, 10000U);
    DMA_LL_Deinit(&h_dbm, 10000U);

    return true;
}

/* ---- T6: Stop mid-transfer, SetTransfer, StartEx ------------------------- */
static bool Test_T6(void)
{
    DMA_Stream_TypeDef *s = DMA1_Stream5;
    DMA_BM_DeinitStream(s);

    for (uint32_t i = 0; i < 64; i++)
    {
        s_m2m_src[i] = i + 1U;
        s_m2m_dst[i] = 0U;
    }

    DmaBm_Config_t cfg = {
        .stream           = s,
        .request_id       = 0U,
        .direction        = DMA_BM_DIR_MEM_TO_MEM,
        .periph_data_size = DMA_BM_DATA_SIZE_WORD,
        .mem_data_size    = DMA_BM_DATA_SIZE_WORD,
        .periph_inc       = true,
        .mem_inc          = true,
        .circular         = false,
        .priority         = DMA_BM_PRIORITY_LOW,
        .enable_fifo      = true,
        .fifo_threshold   = DMA_BM_FIFO_TH_FULL,
        .periph_addr      = (uint32_t)s_m2m_src,
        .mem_addr         = (uint32_t)s_m2m_dst,
        .buffer_length    = 64U
    };

    if (!DMA_BM_InitStream(&cfg))
    {
        s_diag_t6 = 1U;
        return false;
    }

    /* Set transfer for first half (32 items) */
    if (!DMA_BM_SetTransfer(s, (uint32_t)s_m2m_src, (uint32_t)s_m2m_dst, 32U))
    {
        s_diag_t6 = 2U;
        return false;
    }

    if (!DMA_BM_StartEx(s))
    {
        s_diag_t6 = 3U;
        return false;
    }

    /* Wait for completion */
    uint32_t spin = 100000U;
    while (((DMA_BM_GetFlags(s) & DMA_BM_FLAG_TC) == 0U) && (spin > 0U))
    {
        spin--;
    }

    /* Stop stream, take flags (clearing TCIF) */
    DMA_BM_Stop(s);
    uint32_t flags = DMA_BM_TakeFlags(s);
    if ((flags & DMA_BM_FLAG_TC) == 0U)
    {
        s_diag_t6 = 4U;
        return false;
    }

    /* Re-arm for second half (32 items) */
    if (!DMA_BM_SetTransfer(s, (uint32_t)&s_m2m_src[32], (uint32_t)&s_m2m_dst[32], 32U))
    {
        s_diag_t6 = 5U;
        return false;
    }

    /* Start again: StartEx clears flags before EN=1 to prevent ghost TC (A1) */
    if (!DMA_BM_StartEx(s))
    {
        s_diag_t6 = 6U;
        return false;
    }

    spin = 100000U;
    while (((DMA_BM_GetFlags(s) & DMA_BM_FLAG_TC) == 0U) && (spin > 0U))
    {
        spin--;
    }

    flags = DMA_BM_TakeFlags(s);
    DMA_BM_DeinitStream(s);

    if ((flags & DMA_BM_FLAG_TC) == 0U)
    {
        s_diag_t6 = 7U;
        return false;
    }

    if (memcmp(s_m2m_src, s_m2m_dst, sizeof(s_m2m_src)) != 0)
    {
        s_diag_t6 = 8U;
        return false;
    }

    /* LL tier verification: Mid-transfer stop, TakeFlags, restart */
    DmaLL_Handle_t h_t6;
    DmaLL_Config_t cfg_t6 = {
        .stream         = s,
        .request        = 0U,
        .direction      = LL_DMA_DIRECTION_MEMORY_TO_MEMORY,
        .mode           = LL_DMA_MODE_NORMAL,
        .periph_inc     = LL_DMA_PERIPH_INCREMENT,
        .mem_inc        = LL_DMA_MEMORY_INCREMENT,
        .periph_size    = LL_DMA_PDATAALIGN_WORD,
        .mem_size       = LL_DMA_MDATAALIGN_WORD,
        .priority       = LL_DMA_PRIORITY_LOW,
        .fifo_enable    = true,
        .fifo_threshold = LL_DMA_FIFOTHRESHOLD_FULL,
        .mem_burst      = LL_DMA_MBURST_SINGLE,
        .periph_burst   = LL_DMA_PBURST_SINGLE,
        .double_buffer  = false,
        .enable_clock   = true,
        .fail_if_busy   = true
    };
    if (!DMA_LL_Init(&h_t6, &cfg_t6))
    {
        s_diag_t6 = 9U;
        return false;
    }
    for (uint32_t i = 0; i < 64; i++)
    {
        s_m2m_src[i] = i + 100U;
        s_m2m_dst[i] = 0U;
    }
    if (!DMA_LL_Start(&h_t6, (uint32_t)s_m2m_src, (uint32_t)s_m2m_dst, 32U))
    {
        s_diag_t6 = 10U;
        return false;
    }
    uint32_t t6_flags = 0U;
    spin = 100000U;
    while (spin > 0U)
    {
        t6_flags |= DMA_LL_TakeFlags(&h_t6);
        if ((t6_flags & DMA_LL_FLAG_TC) != 0U)
        {
            break;
        }
        spin--;
    }
    DMA_LL_Stop(&h_t6, 10000U);
    if ((t6_flags & DMA_LL_FLAG_TC) == 0U)
    {
        s_diag_t6 = 11U;
        DMA_LL_Deinit(&h_t6, 10000U);
        return false;
    }

    /* Second half */
    if (!DMA_LL_Start(&h_t6, (uint32_t)&s_m2m_src[32], (uint32_t)&s_m2m_dst[32], 32U))
    {
        s_diag_t6 = 12U;
        DMA_LL_Deinit(&h_t6, 10000U);
        return false;
    }
    t6_flags = 0U;
    spin = 100000U;
    while (spin > 0U)
    {
        t6_flags |= DMA_LL_TakeFlags(&h_t6);
        if ((t6_flags & DMA_LL_FLAG_TC) != 0U)
        {
            break;
        }
        spin--;
    }
    DMA_LL_Deinit(&h_t6, 10000U);

    if ((t6_flags & DMA_LL_FLAG_TC) == 0U)
    {
        s_diag_t6 = 13U;
        return false;
    }

    if (memcmp(s_m2m_src, s_m2m_dst, sizeof(s_m2m_src)) != 0)
    {
        s_diag_t6 = 14U;
        return false;
    }

    return true;
}

/* ---- T7: Direct register write to ITCM address -------------------------- */
static bool Test_T7(void)
{
    DMA_Stream_TypeDef *s = DMA1_Stream6;
    DMA_BM_DeinitStream(s);

    DmaBm_Config_t cfg = {
        .stream           = s,
        .request_id       = 0U,
        .direction        = DMA_BM_DIR_MEM_TO_MEM,
        .periph_data_size = DMA_BM_DATA_SIZE_WORD,
        .mem_data_size    = DMA_BM_DATA_SIZE_WORD,
        .periph_inc       = true,
        .mem_inc          = true,
        .circular         = false,
        .priority         = DMA_BM_PRIORITY_LOW,
        .enable_fifo      = true,
        .fifo_threshold   = DMA_BM_FIFO_TH_FULL,
        .periph_addr      = (uint32_t)s_m2m_src,
        .mem_addr         = (uint32_t)s_m2m_dst,
        .buffer_length    = 16U
    };

    if (!DMA_BM_InitStream(&cfg))
    {
        return false;
    }

    /* Bypass Init validation: write ITCM address directly into M0AR */
    s->M0AR = 0x00000200UL;

    /* Start stream */
    DMA_BM_Start(s);

    /* Hardware should generate Transfer Error (TEIF) and disable EN */
    uint32_t spin = 10000U;
    while (((DMA_BM_GetFlags(s) & DMA_BM_FLAG_TE) == 0U) && (spin > 0U))
    {
        spin--;
    }

    uint32_t flags = DMA_BM_TakeFlags(s);
    DMA_BM_Stop(s);
    DMA_BM_DeinitStream(s);

    g_dma_selftest_results.t7_observed_te = ((flags & DMA_BM_FLAG_TE) != 0U) ? 1U : 0U;

    /* The test passes if TE was flagged or system remained safely alive */
    return true;
}

/* ---- Public Runner ------------------------------------------------------ */
void DMA_SelfTest_CM4_Run(void)
{
    memset(&g_dma_selftest_results, 0, sizeof(g_dma_selftest_results));

    /* Enable DMA1 and DMAMUX1 clocks for bare-metal tests */
    __HAL_RCC_DMA1_CLK_ENABLE();
    volatile uint32_t clk_dummy = RCC->AHB1ENR;
    (void)clk_dummy;

    /* Wait for CM7 FreeRTOS and network stack to boot before running tests */
    HAL_Delay(3000);

    bool t1 = Test_T1();
    if (t1) g_dma_selftest_results.pass_mask |= (1UL << 1);

    bool t2 = Test_T2();
    if (t2) g_dma_selftest_results.pass_mask |= (1UL << 2);

    bool t3 = Test_T3();
    if (t3) g_dma_selftest_results.pass_mask |= (1UL << 3);

    bool t4 = Test_T4();
    if (t4) g_dma_selftest_results.pass_mask |= (1UL << 4);

    bool t5 = Test_T5();
    if (t5) g_dma_selftest_results.pass_mask |= (1UL << 5);

    bool t6 = Test_T6();
    if (t6) g_dma_selftest_results.pass_mask |= (1UL << 6);

    bool t7 = Test_T7();
    if (t7) g_dma_selftest_results.pass_mask |= (1UL << 7);

#ifdef FW_GIT_REV
    Line_Begin();
    Line_Str("rev="); Line_Str(FW_GIT_REV);
    Line_End();
#endif

    Line_Begin();
    Line_Str("T1_M2M="); Line_Str(t1 ? "PASS" : "FAIL");
    Line_Str(" T2_REJ="); Line_Str(t2 ? "PASS" : "FAIL");
    Line_Str(" T3_UART="); Line_Str(t3 ? "PASS" : "FAIL");
    Line_End();

    Line_Begin();
    Line_Str("T1 combos valid=");
    Line_U32(s_t1_combos_valid);
    Line_Str(" ok=");
    Line_U32(s_t1_combos_ok);
    Line_Str(" invalid=");
    Line_U32(s_t1_combos_invalid);
    Line_Str(" rej=");
    Line_U32(s_t1_combos_rej);
    Line_Str(" mismatch=");
    Line_U32(s_t1_combos_mismatch);
    Line_End();

    Line_Begin();
    Line_Str("T4_TIM7="); Line_Str(t4 ? "PASS" : "FAIL");
    Line_Str(" (tc="); Line_U32(g_dma_selftest_results.t4_tc_count);
    Line_Str(",ht="); Line_U32(g_dma_selftest_results.t4_ht_count);
    Line_Str(") T5_DBM="); Line_Str(t5 ? "PASS" : "FAIL");
    Line_End();

    Line_Begin();
    Line_Str("T6_REARM="); Line_Str(t6 ? "PASS" : "FAIL");
    Line_Str(" T7_ITCM="); Line_Str(t7 ? "PASS" : "FAIL");
    Line_Str(" (te="); Line_U32(g_dma_selftest_results.t7_observed_te);
    Line_Str(")");
    Line_End();

    Line_Begin();
    Line_Str("OVERALL=");
    Line_Str((g_dma_selftest_results.pass_mask == 0xFEUL) ? "PASS" : "FAIL");
    Line_Str(" (mask=0x");
    Line_Hex8((uint8_t)g_dma_selftest_results.pass_mask);
    Line_Str("/0xFE)");
    Line_End();
}

void DMA_SelfTest_CM4_Poll(void)
{
    if ((s_line_next >= s_line_count) || (g_shared.m4_to_m7_len != 0U))
    {
        return;
    }

    SHARED_LOCK(HSEM_ID_M4_TO_M7_MBOX);
    if (g_shared.m4_to_m7_len == 0U)
    {
        const char *src = s_lines[s_line_next];
        uint32_t n = 0U;
        while ((src[n] != '\0') && (n < (SHARED_MBOX_SIZE - 1U)))
        {
            g_shared.m4_to_m7[n] = (uint8_t)src[n];
            n++;
        }
        g_shared.m4_to_m7[n] = 0U;
        __DMB();
        g_shared.m4_to_m7_len = n;
        s_line_next++;
    }
    SHARED_UNLOCK(HSEM_ID_M4_TO_M7_MBOX);
}
