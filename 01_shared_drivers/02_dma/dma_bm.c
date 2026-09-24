/**
  ******************************************************************************
  * @file    dma_bm.c
  * @brief   Bare-metal DMA driver implementation for STM32H755.
  ******************************************************************************
  */

#include "dma_bm.h"
#include <stddef.h>

#define DMA_STREAM_STRIDE         0x18UL
#define DMA_DISABLE_TIMEOUT_LOOPS 10000UL

_Static_assert(sizeof(DMA_Stream_TypeDef) == DMA_STREAM_STRIDE,
               "DMA_Stream_TypeDef size does not match 0x18 stride");

static const uint8_t s_stream_bit_shifts[4] = {0U, 6U, 16U, 22U};
static const uint8_t s_burst_beats[4] = {1U, 4U, 8U, 16U};

DMA_TypeDef *DMA_BM_GetController(const DMA_Stream_TypeDef *stream)
{
    uint32_t addr = (uint32_t)stream;
    if ((addr >= (uint32_t)DMA1_Stream0) && (addr <= (uint32_t)DMA1_Stream7))
    {
        if (((addr - (uint32_t)DMA1_Stream0) % DMA_STREAM_STRIDE) == 0U)
        {
            return DMA1;
        }
    }
    if ((addr >= (uint32_t)DMA2_Stream0) && (addr <= (uint32_t)DMA2_Stream7))
    {
        if (((addr - (uint32_t)DMA2_Stream0) % DMA_STREAM_STRIDE) == 0U)
        {
            return DMA2;
        }
    }
    return NULL;
}

uint32_t DMA_BM_GetStreamIndex(const DMA_Stream_TypeDef *stream)
{
    uint32_t addr = (uint32_t)stream;
    if ((addr >= (uint32_t)DMA1_Stream0) && (addr <= (uint32_t)DMA1_Stream7))
    {
        uint32_t offset = addr - (uint32_t)DMA1_Stream0;
        if ((offset % DMA_STREAM_STRIDE) == 0U)
        {
            return offset / DMA_STREAM_STRIDE;
        }
    }
    if ((addr >= (uint32_t)DMA2_Stream0) && (addr <= (uint32_t)DMA2_Stream7))
    {
        uint32_t offset = addr - (uint32_t)DMA2_Stream0;
        if ((offset % DMA_STREAM_STRIDE) == 0U)
        {
            return offset / DMA_STREAM_STRIDE;
        }
    }
    return 0xFFFFFFFFUL;
}

static uint32_t DMA_BM_GetDmamuxChannel(const DMA_Stream_TypeDef *stream)
{
    uint32_t stream_idx = DMA_BM_GetStreamIndex(stream);
    DMA_TypeDef *dma = DMA_BM_GetController(stream);

    if (dma == DMA1)
    {
        return stream_idx; /* 0..7 */
    }
    else if (dma == DMA2)
    {
        return stream_idx + 8U; /* 8..15 */
    }
    return 0xFFFFFFFFUL;
}

bool DMA_BM_IsUartRequest(uint32_t request_id)
{
    return ((request_id >= 41U && request_id <= 46U) ||
            (request_id >= 63U && request_id <= 66U) ||
            (request_id >= 71U && request_id <= 72U) ||
            (request_id == 78U) ||
            (request_id >= 79U && request_id <= 82U));
}

IRQn_Type DMA_BM_GetIRQn(const DMA_Stream_TypeDef *s)
{
    DMA_TypeDef *dma = DMA_BM_GetController(s);
    uint32_t idx = DMA_BM_GetStreamIndex(s);

    if ((dma == NULL) || (idx > 7U))
    {
        return (IRQn_Type)(-1);
    }

    if (dma == DMA1)
    {
        if (idx < 7U)
        {
            return (IRQn_Type)(DMA1_Stream0_IRQn + idx); /* 11..17 */
        }
        else
        {
            return DMA1_Stream7_IRQn; /* 47 */
        }
    }
    else /* DMA2 */
    {
        if (idx < 5U)
        {
            return (IRQn_Type)(DMA2_Stream0_IRQn + idx); /* 56..60 */
        }
        else
        {
            return (IRQn_Type)(DMA2_Stream5_IRQn + (idx - 5U)); /* 68..70 */
        }
    }
}

bool DMA_BM_CheckFifoBurst(uint32_t data_size, uint32_t threshold, uint32_t burst)
{
    if (burst == DMA_BM_BURST_SINGLE)
    {
        return true;
    }

    if (data_size == DMA_BM_DATA_SIZE_BYTE)
    {
        switch (threshold)
        {
            case DMA_BM_FIFO_TH_1_4:
            case DMA_BM_FIFO_TH_3_4:
                return (burst == DMA_BM_BURST_INCR4);
            case DMA_BM_FIFO_TH_1_2:
                return (burst == DMA_BM_BURST_INCR4 || burst == DMA_BM_BURST_INCR8);
            case DMA_BM_FIFO_TH_FULL:
                return true;
            default:
                return false;
        }
    }
    else if (data_size == DMA_BM_DATA_SIZE_HALFWORD)
    {
        switch (threshold)
        {
            case DMA_BM_FIFO_TH_1_4:
            case DMA_BM_FIFO_TH_3_4:
                return false;
            case DMA_BM_FIFO_TH_1_2:
                return (burst == DMA_BM_BURST_INCR4);
            case DMA_BM_FIFO_TH_FULL:
                return (burst == DMA_BM_BURST_INCR4 || burst == DMA_BM_BURST_INCR8);
            default:
                return false;
        }
    }
    else /* Word */
    {
        switch (threshold)
        {
            case DMA_BM_FIFO_TH_1_4:
            case DMA_BM_FIFO_TH_1_2:
            case DMA_BM_FIFO_TH_3_4:
                return false;
            case DMA_BM_FIFO_TH_FULL:
                return (burst == DMA_BM_BURST_INCR4);
            default:
                return false;
        }
    }
}

bool DMA_BM_InitStreamEx(const DmaBm_ConfigEx_t *cfg)
{
    if ((cfg == NULL) || (cfg->base.stream == NULL))
    {
        return false;
    }

    const DmaBm_Config_t *c = &cfg->base;

    /* A4: NDTR validation (0 < buffer_length <= 65535) */
    if ((c->buffer_length == 0U) || (c->buffer_length > 65535U))
    {
        return false;
    }

    DMA_TypeDef *dma = DMA_BM_GetController(c->stream);
    uint32_t stream_idx = DMA_BM_GetStreamIndex(c->stream);
    uint32_t dmamux_ch = DMA_BM_GetDmamuxChannel(c->stream);

    if ((dma == NULL) || (stream_idx > 7U) || (dmamux_ch > 15U))
    {
        return false;
    }

    /* A4: Direction validation */
    if ((c->direction != DMA_BM_DIR_PERIPH_TO_MEM) &&
        (c->direction != DMA_BM_DIR_MEM_TO_PERIPH) &&
        (c->direction != DMA_BM_DIR_MEM_TO_MEM))
    {
        return false;
    }

    /* Circular mode is forbidden in memory-to-memory transfers (RM0399 §16.3.7) */
    if ((c->direction == DMA_BM_DIR_MEM_TO_MEM) && c->circular)
    {
        return false;
    }

    /* A4: Data sizes validation (0..2, 3 is reserved) */
    if ((c->periph_data_size > DMA_BM_DATA_SIZE_WORD) ||
        (c->mem_data_size > DMA_BM_DATA_SIZE_WORD))
    {
        return false;
    }

    /* Priority level validation */
    if ((c->priority & ~DMA_SxCR_PL_Msk) != 0U)
    {
        return false;
    }

    /* A4: Request ID validation (7 bits: [6:0]).
       In peripheral transfers (P2M/M2P), request 0 never starts and is rejected. */
    if (c->request_id > 127U)
    {
        return false;
    }
    if ((c->direction != DMA_BM_DIR_MEM_TO_MEM) && (c->request_id == 0U))
    {
        return false;
    }

    /* A6: Direct mode validation: MSIZE must equal PSIZE */
    bool is_fifo = (c->direction == DMA_BM_DIR_MEM_TO_MEM) || c->enable_fifo;
    if (!is_fifo && (c->periph_data_size != c->mem_data_size))
    {
        return false;
    }

    /* DBM validation (forbidden with M2M) */
    if (cfg->mem1_addr != 0U)
    {
        if (c->direction == DMA_BM_DIR_MEM_TO_MEM)
        {
            return false;
        }
    }

    /* PFCTRL validation (forbidden with M2M or with circular) */
    if (cfg->periph_flow_ctrl)
    {
        if ((c->direction == DMA_BM_DIR_MEM_TO_MEM) || c->circular)
        {
            return false;
        }
    }

    /* Burst validation */
    if ((cfg->mem_burst > DMA_BM_BURST_INCR16) || (cfg->periph_burst > DMA_BM_BURST_INCR16))
    {
        return false;
    }
    if ((cfg->mem_burst != DMA_BM_BURST_SINGLE) || (cfg->periph_burst != DMA_BM_BURST_SINGLE))
    {
        if (!is_fifo)
        {
            return false; /* Burst is forbidden in direct mode */
        }
        if (!DMA_BM_CheckFifoBurst(c->mem_data_size, c->fifo_threshold, cfg->mem_burst))
        {
            return false;
        }
        if (!DMA_BM_CheckFifoBurst(c->periph_data_size, c->fifo_threshold, cfg->periph_burst))
        {
            return false;
        }
    }

    uint32_t periph_elem_size = (1U << c->periph_data_size);
    uint32_t mem_elem_size = (1U << c->mem_data_size);

    uint32_t pburst_beats = s_burst_beats[cfg->periph_burst];
    uint32_t mburst_beats = s_burst_beats[cfg->mem_burst];
    uint32_t periph_align = pburst_beats * periph_elem_size;
    uint32_t mem_align = mburst_beats * mem_elem_size;

    /* A4/A9: Alignment checks */
    if ((c->periph_addr & (periph_align - 1U)) != 0U)
    {
        return false;
    }
    if ((c->mem_addr & (mem_align - 1U)) != 0U)
    {
        return false;
    }
    if ((cfg->mem1_addr != 0U) && ((cfg->mem1_addr & (mem_align - 1U)) != 0U))
    {
        return false;
    }

    /* 1 KB boundary check for burst */
    if (((c->periph_addr & 0x3FFU) + periph_align) > 0x400U)
    {
        return false;
    }
    if (((c->mem_addr & 0x3FFU) + mem_align) > 0x400U)
    {
        return false;
    }
    if (cfg->mem1_addr != 0U)
    {
        if (((cfg->mem1_addr & 0x3FFU) + mem_align) > 0x400U)
        {
            return false;
        }
    }

    /* A5: Reachability validation.
     * NDTR counts in PSIZE units (RM0399 §16.3.12), so total bytes = NDT × PSIZE
     * for both peripheral and memory sides (the FIFO handles width conversion). */
    if (!DMA_MEM_BM_IsReachable(DMA_MEM_BM_MASTER_DMA, c->mem_addr, c->buffer_length * periph_elem_size))
    {
        return false;
    }
    if (cfg->mem1_addr != 0U)
    {
        if (!DMA_MEM_BM_IsReachable(DMA_MEM_BM_MASTER_DMA, cfg->mem1_addr, c->buffer_length * periph_elem_size))
        {
            return false;
        }
    }
    if (c->direction == DMA_BM_DIR_MEM_TO_MEM)
    {
        if (!DMA_MEM_BM_IsReachable(DMA_MEM_BM_MASTER_DMA, c->periph_addr, c->buffer_length * periph_elem_size))
        {
            return false;
        }
    }

    /* A7: fail_if_busy check */
    if (cfg->fail_if_busy && ((c->stream->CR & DMA_SxCR_EN) != 0U))
    {
        return false;
    }

    /* --- All validations passed. Proceed to hardware registers write --- */

    /* 1. Ensure DMA and DMAMUX clocks are enabled. */
    if (dma == DMA1)
    {
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    }
    else
    {
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
    }
    volatile uint32_t dummy = RCC->AHB1ENR;
    (void)dummy;

    /* 2. Disable stream before configuring registers */
    if (!DMA_BM_StopEx(c->stream, cfg->stop_spin_limit))
    {
        return false;
    }

    /* 3. Configure DMAMUX1 channel */
    DMAMUX_Channel_TypeDef *mux = (DMAMUX_Channel_TypeDef *)((uint32_t)DMAMUX1_Channel0 + (dmamux_ch * 4U));
    mux->CCR = (c->request_id & DMAMUX_CxCR_DMAREQ_ID);
    DMAMUX1_ChannelStatus->CFR = (1UL << dmamux_ch);

    /* 4. Clear all pending interrupt flags for this stream */
    DMA_BM_ClearFlags(c->stream, DMA_BM_FLAG_ALL);

    /* 5. Set addresses and length */
    c->stream->PAR  = c->periph_addr;
    c->stream->M0AR = c->mem_addr;
    if (cfg->mem1_addr != 0U)
    {
        c->stream->M1AR = cfg->mem1_addr;
    }
    c->stream->NDTR = c->buffer_length;

    /* 6. Configure FIFO control register (FCR).
     * RM0399 line 796 mandates FIFO mode (DMDIS = 1) for memory-to-memory transfers.
     */
    uint32_t fcr = 0U;
    if (is_fifo)
    {
        fcr |= DMA_SxFCR_DMDIS; /* Enable FIFO mode */
        fcr |= (c->fifo_threshold & DMA_SxFCR_FTH_Msk);
    }
    if (c->enable_it_fe)
    {
        fcr |= DMA_SxFCR_FEIE;
    }
    c->stream->FCR = fcr;

    /* 7. Build control register (CR) */
    uint32_t cr = 0U;
    cr |= c->direction;
    if (c->circular || (cfg->mem1_addr != 0U))
    {
        cr |= DMA_SxCR_CIRC;
    }
    if (cfg->mem1_addr != 0U)
    {
        cr |= DMA_SxCR_DBM;
    }
    if (cfg->periph_flow_ctrl)
    {
        cr |= DMA_SxCR_PFCTRL;
    }
    if (c->periph_inc)
    {
        cr |= DMA_SxCR_PINC;
    }
    if (c->mem_inc)
    {
        cr |= DMA_SxCR_MINC;
    }
    cr |= ((c->periph_data_size & 0x03UL) << DMA_SxCR_PSIZE_Pos);
    cr |= ((c->mem_data_size & 0x03UL) << DMA_SxCR_MSIZE_Pos);
    cr |= (c->priority & DMA_SxCR_PL_Msk);
    cr |= ((cfg->periph_burst & 0x03UL) << DMA_SxCR_PBURST_Pos);
    cr |= ((cfg->mem_burst & 0x03UL) << DMA_SxCR_MBURST_Pos);

    if (c->enable_it_tc)
    {
        cr |= DMA_SxCR_TCIE;
    }
    if (c->enable_it_ht)
    {
        cr |= DMA_SxCR_HTIE;
    }
    if (c->enable_it_te)
    {
        cr |= DMA_SxCR_TEIE;
    }
    if (c->enable_it_dme)
    {
        cr |= DMA_SxCR_DMEIE;
    }

    /* A3: Automatic TRBUFF for UART and HASH_IN requests when REV_ID >= 0x2000 */
    bool need_trbuff = c->enable_trbuff;
    if (DMA_BM_IsUartRequest(c->request_id) && ((DBGMCU->IDCODE & 0xFFFF0000U) >= 0x20000000U))
    {
        need_trbuff = true;
    }
    if (need_trbuff)
    {
        cr |= DMA_BM_CR_TRBUFF;
    }

    c->stream->CR = cr;

    return true;
}

bool DMA_BM_InitStream(const DmaBm_Config_t *config)
{
    if (config == NULL)
    {
        return false;
    }

    DmaBm_ConfigEx_t ex = {
        .base             = *config,
        .mem1_addr        = 0U,
        .mem_burst        = DMA_BM_BURST_SINGLE,
        .periph_burst     = DMA_BM_BURST_SINGLE,
        .periph_flow_ctrl = false,
        .fail_if_busy     = false,
        .stop_spin_limit  = DMA_DISABLE_TIMEOUT_LOOPS
    };

    return DMA_BM_InitStreamEx(&ex);
}

bool DMA_BM_SetTransfer(DMA_Stream_TypeDef *s, uint32_t periph_addr,
                        uint32_t mem_addr, uint32_t length)
{
    if ((s == NULL) || (length == 0U) || (length > 65535U))
    {
        return false;
    }

    /* Transfer parameters can only be updated when stream is disabled */
    if ((s->CR & DMA_SxCR_EN) != 0U)
    {
        return false;
    }

    DMA_TypeDef *dma = DMA_BM_GetController(s);
    if (dma == NULL)
    {
        return false;
    }

    uint32_t cr = s->CR;
    uint32_t psize = (cr & DMA_SxCR_PSIZE_Msk) >> DMA_SxCR_PSIZE_Pos;
    uint32_t msize = (cr & DMA_SxCR_MSIZE_Msk) >> DMA_SxCR_MSIZE_Pos;
    uint32_t pburst = (cr & DMA_SxCR_PBURST_Msk) >> DMA_SxCR_PBURST_Pos;
    uint32_t mburst = (cr & DMA_SxCR_MBURST_Msk) >> DMA_SxCR_MBURST_Pos;
    uint32_t dir = (cr & DMA_SxCR_DIR_Msk);

    uint32_t periph_elem_size = (1U << psize);
    uint32_t mem_elem_size = (1U << msize);

    uint32_t pburst_beats = s_burst_beats[pburst];
    uint32_t mburst_beats = s_burst_beats[mburst];
    uint32_t periph_align = pburst_beats * periph_elem_size;
    uint32_t mem_align = mburst_beats * mem_elem_size;

    if ((periph_addr & (periph_align - 1U)) != 0U)
    {
        return false;
    }
    if ((mem_addr & (mem_align - 1U)) != 0U)
    {
        return false;
    }

    if (((periph_addr & 0x3FFU) + periph_align) > 0x400U)
    {
        return false;
    }
    if (((mem_addr & 0x3FFU) + mem_align) > 0x400U)
    {
        return false;
    }

    if (!DMA_MEM_BM_IsReachable(DMA_MEM_BM_MASTER_DMA, mem_addr, length * periph_elem_size))
    {
        return false;
    }
    if (dir == DMA_BM_DIR_MEM_TO_MEM)
    {
        if (!DMA_MEM_BM_IsReachable(DMA_MEM_BM_MASTER_DMA, periph_addr, length * periph_elem_size))
        {
            return false;
        }
    }

    s->PAR  = periph_addr;
    s->M0AR = mem_addr;
    s->NDTR = length;

    return true;
}

bool DMA_BM_DeinitStream(DMA_Stream_TypeDef *stream)
{
    if (stream == NULL)
    {
        return false;
    }

    /* 1. Disable stream */
    if (!DMA_BM_Stop(stream))
    {
        return false;
    }

    /* 2. Reset stream registers to reset values (RM0399) */
    stream->CR   = 0U;
    stream->NDTR = 0U;
    stream->PAR  = 0U;
    stream->M0AR = 0U;
    stream->M1AR = 0U;
    stream->FCR  = 0x00000021UL;

    /* 3. Clear all pending status/interrupt flags */
    DMA_BM_ClearFlags(stream, DMA_BM_FLAG_ALL);

    /* 4. Reset associated DMAMUX channel */
    uint32_t dmamux_ch = DMA_BM_GetDmamuxChannel(stream);
    if (dmamux_ch <= 15U)
    {
        DMAMUX_Channel_TypeDef *mux = (DMAMUX_Channel_TypeDef *)((uint32_t)DMAMUX1_Channel0 + (dmamux_ch * 4U));
        mux->CCR = 0U;
        DMAMUX1_ChannelStatus->CFR = (1UL << dmamux_ch);
    }

    return true;
}

void DMA_BM_Start(DMA_Stream_TypeDef *stream)
{
    if (stream != NULL)
    {
        /* A1: Clear stream status flags before setting EN */
        DMA_BM_ClearFlags(stream, DMA_BM_FLAG_ALL);
        stream->CR |= DMA_SxCR_EN;
    }
}

bool DMA_BM_StartEx(DMA_Stream_TypeDef *s)
{
    if (s == NULL)
    {
        return false;
    }

    /* A1: Clear stream status flags before setting EN */
    DMA_BM_ClearFlags(s, DMA_BM_FLAG_ALL);
    s->CR |= DMA_SxCR_EN;

    /* Verified start: wait for bus write latch / EN readback or immediate TC/TE */
    uint32_t spin = 100U;
    while (spin > 0U)
    {
        if (((s->CR & DMA_SxCR_EN) != 0U) ||
            ((DMA_BM_GetFlags(s) & (DMA_BM_FLAG_TC | DMA_BM_FLAG_TE)) != 0U))
        {
            return true;
        }
        spin--;
    }

    return false;
}

bool DMA_BM_StopEx(DMA_Stream_TypeDef *s, uint32_t spin_limit)
{
    if (s == NULL)
    {
        return false;
    }

    s->CR &= ~DMA_SxCR_EN;

    uint32_t timeout = (spin_limit == 0U) ? DMA_DISABLE_TIMEOUT_LOOPS : spin_limit;
    while (((s->CR & DMA_SxCR_EN) != 0U) && (timeout > 0U))
    {
        timeout--;
    }

    return ((s->CR & DMA_SxCR_EN) == 0U);
}

bool DMA_BM_Stop(DMA_Stream_TypeDef *stream)
{
    return DMA_BM_StopEx(stream, DMA_DISABLE_TIMEOUT_LOOPS);
}

uint32_t DMA_BM_GetRemaining(const DMA_Stream_TypeDef *stream)
{
    return (stream != NULL) ? stream->NDTR : 0U;
}

uint32_t DMA_BM_GetFlags(const DMA_Stream_TypeDef *stream)
{
    DMA_TypeDef *dma = DMA_BM_GetController(stream);
    uint32_t stream_idx = DMA_BM_GetStreamIndex(stream);

    if ((dma == NULL) || (stream_idx > 7U))
    {
        return 0U;
    }

    uint32_t isr_val;
    uint32_t shift;

    if (stream_idx < 4U)
    {
        isr_val = dma->LISR;
        shift = s_stream_bit_shifts[stream_idx];
    }
    else
    {
        isr_val = dma->HISR;
        shift = s_stream_bit_shifts[stream_idx - 4U];
    }

    uint32_t raw = (isr_val >> shift) & 0x3FUL;
    uint32_t flags = 0U;

    if ((raw & (1UL << 5)) != 0U)
    {
        flags |= DMA_BM_FLAG_TC;
    }
    if ((raw & (1UL << 4)) != 0U)
    {
        flags |= DMA_BM_FLAG_HT;
    }
    if ((raw & (1UL << 3)) != 0U)
    {
        flags |= DMA_BM_FLAG_TE;
    }
    if ((raw & (1UL << 2)) != 0U)
    {
        flags |= DMA_BM_FLAG_DME;
    }
    if ((raw & (1UL << 0)) != 0U)
    {
        flags |= DMA_BM_FLAG_FE;
    }

    return flags;
}

void DMA_BM_ClearFlags(DMA_Stream_TypeDef *stream, uint32_t flags)
{
    DMA_TypeDef *dma = DMA_BM_GetController(stream);
    uint32_t stream_idx = DMA_BM_GetStreamIndex(stream);

    if ((dma == NULL) || (stream_idx > 7U))
    {
        return;
    }

    uint32_t shift = (stream_idx < 4U) ? s_stream_bit_shifts[stream_idx] :
                                          s_stream_bit_shifts[stream_idx - 4U];
    uint32_t raw_clear = 0U;

    if ((flags & DMA_BM_FLAG_TC) != 0U)
    {
        raw_clear |= (1UL << 5);
    }
    if ((flags & DMA_BM_FLAG_HT) != 0U)
    {
        raw_clear |= (1UL << 4);
    }
    if ((flags & DMA_BM_FLAG_TE) != 0U)
    {
        raw_clear |= (1UL << 3);
    }
    if ((flags & DMA_BM_FLAG_DME) != 0U)
    {
        raw_clear |= (1UL << 2);
    }
    if ((flags & DMA_BM_FLAG_FE) != 0U)
    {
        raw_clear |= (1UL << 0);
    }

    uint32_t write_val = (raw_clear << shift);

    if (stream_idx < 4U)
    {
        dma->LIFCR = write_val;
    }
    else
    {
        dma->HIFCR = write_val;
    }
}

uint32_t DMA_BM_TakeFlags(DMA_Stream_TypeDef *s)
{
    uint32_t flags = DMA_BM_GetFlags(s);
    if (flags != 0U)
    {
        DMA_BM_ClearFlags(s, flags);
    }
    return flags;
}

uint32_t DMA_BM_GetCurrentTarget(const DMA_Stream_TypeDef *s)
{
    if (s == NULL)
    {
        return 0xFFFFFFFFUL;
    }
    return ((s->CR & DMA_SxCR_CT) != 0U) ? 1U : 0U;
}

bool DMA_BM_SetIdleMemory(DMA_Stream_TypeDef *s, uint32_t addr)
{
    if (s == NULL)
    {
        return false;
    }
    if ((s->CR & DMA_SxCR_DBM) == 0U)
    {
        return false; /* Not in double buffer mode */
    }

    uint32_t psize = (s->CR & DMA_SxCR_PSIZE_Msk) >> DMA_SxCR_PSIZE_Pos;
    uint32_t periph_elem_size = (1U << psize);
    uint32_t msize = (s->CR & DMA_SxCR_MSIZE_Msk) >> DMA_SxCR_MSIZE_Pos;
    uint32_t mem_elem_size = (1U << msize);
    uint32_t mburst = (s->CR & DMA_SxCR_MBURST_Msk) >> DMA_SxCR_MBURST_Pos;
    uint32_t burst_bytes = s_burst_beats[mburst] * mem_elem_size;

    if ((addr & (burst_bytes - 1U)) != 0U)
    {
        return false;
    }

    uint32_t length = s->NDTR;
    if (length == 0U)
    {
        length = 1U;
    }
    /* NDTR counts in PSIZE units (RM0399 §16.3.12) */
    if (!DMA_MEM_BM_IsReachable(DMA_MEM_BM_MASTER_DMA, addr, length * periph_elem_size))
    {
        return false;
    }

    /* CT=0 -> active is Memory 0, idle is Memory 1 (M1AR).
       CT=1 -> active is Memory 1, idle is Memory 0 (M0AR). */
    if ((s->CR & DMA_SxCR_CT) == 0U)
    {
        s->M1AR = addr;
    }
    else
    {
        s->M0AR = addr;
    }

    return true;
}
