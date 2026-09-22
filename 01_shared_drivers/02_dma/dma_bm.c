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

DMA_TypeDef *DMA_BM_GetController(const DMA_Stream_TypeDef *stream)
{
    uint32_t addr = (uint32_t)stream;
    if ((addr >= (uint32_t)DMA1_Stream0) && (addr <= (uint32_t)DMA1_Stream7))
    {
        return DMA1;
    }
    if ((addr >= (uint32_t)DMA2_Stream0) && (addr <= (uint32_t)DMA2_Stream7))
    {
        return DMA2;
    }
    return NULL;
}

uint32_t DMA_BM_GetStreamIndex(const DMA_Stream_TypeDef *stream)
{
    uint32_t addr = (uint32_t)stream;
    if ((addr >= (uint32_t)DMA1_Stream0) && (addr <= (uint32_t)DMA1_Stream7))
    {
        return (addr - (uint32_t)DMA1_Stream0) / DMA_STREAM_STRIDE;
    }
    if ((addr >= (uint32_t)DMA2_Stream0) && (addr <= (uint32_t)DMA2_Stream7))
    {
        return (addr - (uint32_t)DMA2_Stream0) / DMA_STREAM_STRIDE;
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

bool DMA_BM_InitStream(const DmaBm_Config_t *config)
{
    if ((config == NULL) || (config->stream == NULL) || (config->buffer_length == 0U))
    {
        return false;
    }

    DMA_TypeDef *dma = DMA_BM_GetController(config->stream);
    uint32_t stream_idx = DMA_BM_GetStreamIndex(config->stream);
    uint32_t dmamux_ch = DMA_BM_GetDmamuxChannel(config->stream);

    if ((dma == NULL) || (stream_idx > 7U) || (dmamux_ch > 15U))
    {
        return false;
    }

    /* Circular mode is forbidden in memory-to-memory transfers (RM0399) */
    if ((config->direction == DMA_BM_DIR_MEM_TO_MEM) && config->circular)
    {
        return false;
    }

    /* 1. Ensure DMA and DMAMUX clocks are enabled.
       AHB1ENR is per-core banked, not shared state: see the note in
       00_gpio/gpio_bm.c before reading this as a dual-core race. */
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
    if (!DMA_BM_Stop(config->stream))
    {
        return false;
    }

    /* 3. Configure DMAMUX1 channel */
    DMAMUX_Channel_TypeDef *mux = (DMAMUX_Channel_TypeDef *)((uint32_t)DMAMUX1_Channel0 + (dmamux_ch * 4U));
    mux->CCR = (config->request_id & DMAMUX_CxCR_DMAREQ_ID);
    DMAMUX1_ChannelStatus->CFR = (1UL << dmamux_ch);

    /* 4. Clear all pending interrupt flags for this stream */
    DMA_BM_ClearFlags(config->stream, DMA_BM_FLAG_ALL);

    /* 5. Set addresses and length */
    config->stream->PAR  = config->periph_addr;
    config->stream->M0AR = config->mem_addr;
    config->stream->NDTR = config->buffer_length;

    /* 6. Configure FIFO control register (FCR).
     * RM0399 line 796 mandates FIFO mode (DMDIS = 1) for memory-to-memory transfers.
     */
    uint32_t fcr = 0U;
    if ((config->direction == DMA_BM_DIR_MEM_TO_MEM) || config->enable_fifo)
    {
        fcr |= DMA_SxFCR_DMDIS; /* Enable FIFO mode */
        fcr |= (config->fifo_threshold & DMA_SxFCR_FTH_Msk);
    }
    if (config->enable_it_fe)
    {
        fcr |= DMA_SxFCR_FEIE;
    }
    config->stream->FCR = fcr;

    /* 7. Build control register (CR) */
    uint32_t cr = 0U;
    cr |= config->direction;
    if (config->circular)
    {
        cr |= DMA_SxCR_CIRC;
    }
    if (config->periph_inc)
    {
        cr |= DMA_SxCR_PINC;
    }
    if (config->mem_inc)
    {
        cr |= DMA_SxCR_MINC;
    }
    cr |= ((config->periph_data_size & 0x03UL) << DMA_SxCR_PSIZE_Pos);
    cr |= ((config->mem_data_size & 0x03UL) << DMA_SxCR_MSIZE_Pos);
    cr |= (config->priority & DMA_SxCR_PL_Msk);

    if (config->enable_it_tc)
    {
        cr |= DMA_SxCR_TCIE;
    }
    if (config->enable_it_ht)
    {
        cr |= DMA_SxCR_HTIE;
    }
    if (config->enable_it_te)
    {
        cr |= DMA_SxCR_TEIE;
    }
    if (config->enable_it_dme)
    {
        cr |= DMA_SxCR_DMEIE;
    }
    if (config->enable_trbuff)
    {
        cr |= DMA_BM_CR_TRBUFF; /* ES0445 TRBUFF workaround for UART/LPUART DMA */
    }

    config->stream->CR = cr;

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
        stream->CR |= DMA_SxCR_EN;
    }
}

bool DMA_BM_Stop(DMA_Stream_TypeDef *stream)
{
    if (stream == NULL)
    {
        return false;
    }

    stream->CR &= ~DMA_SxCR_EN;

    uint32_t timeout = DMA_DISABLE_TIMEOUT_LOOPS;
    while (((stream->CR & DMA_SxCR_EN) != 0U) && (timeout > 0U))
    {
        timeout--;
    }

    return ((stream->CR & DMA_SxCR_EN) == 0U);
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
