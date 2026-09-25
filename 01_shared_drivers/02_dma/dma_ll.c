/**
  ******************************************************************************
  * @file    dma_ll.c
  * @brief   Low-Layer DMA driver implementation for STM32H755.
  *
  *          Driver tier: *_ll (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Based strictly on ST LL static inline functions and CMSIS core.
  ******************************************************************************
  */

#include "dma_ll.h"

static const uint8_t s_burst_beats[4] = {1U, 4U, 8U, 16U};

bool DMA_LL_Init(DmaLL_Handle_t *h, const DmaLL_Config_t *cfg)
{
    if ((h == NULL) || (cfg == NULL) || (cfg->stream == NULL))
    {
        return false;
    }

    DMA_TypeDef *dma = DMA_BM_GetController(cfg->stream);
    uint32_t stream_idx = DMA_BM_GetStreamIndex(cfg->stream);
    if ((dma == NULL) || (stream_idx > 7U))
    {
        return false;
    }

    /* A7: Refuse configuration if stream is active and fail_if_busy is requested */
    if (cfg->fail_if_busy && ((cfg->stream->CR & DMA_SxCR_EN) != 0U))
    {
        return false;
    }

    /* Transfer direction validation */
    if ((cfg->direction != LL_DMA_DIRECTION_PERIPH_TO_MEMORY) &&
        (cfg->direction != LL_DMA_DIRECTION_MEMORY_TO_PERIPH) &&
        (cfg->direction != LL_DMA_DIRECTION_MEMORY_TO_MEMORY))
    {
        return false;
    }

    /* Mode validation */
    if ((cfg->mode != LL_DMA_MODE_NORMAL) &&
        (cfg->mode != LL_DMA_MODE_CIRCULAR) &&
        (cfg->mode != LL_DMA_MODE_PFCTRL))
    {
        return false;
    }

    /* Memory-to-memory restrictions (RM0399 §16.3.7) */
    if (cfg->direction == LL_DMA_DIRECTION_MEMORY_TO_MEMORY)
    {
        if ((cfg->mode == LL_DMA_MODE_CIRCULAR) ||
            (cfg->mode == LL_DMA_MODE_PFCTRL) ||
            cfg->double_buffer)
        {
            return false;
        }
    }

    /* Double buffer restrictions */
    if (cfg->double_buffer)
    {
        if (cfg->direction == LL_DMA_DIRECTION_MEMORY_TO_MEMORY)
        {
            return false;
        }
    }

    /* Address increment options validation */
    if ((cfg->periph_inc != LL_DMA_PERIPH_NOINCREMENT) &&
        (cfg->periph_inc != LL_DMA_PERIPH_INCREMENT))
    {
        return false;
    }
    if ((cfg->mem_inc != LL_DMA_MEMORY_NOINCREMENT) &&
        (cfg->mem_inc != LL_DMA_MEMORY_INCREMENT))
    {
        return false;
    }

    /* Data alignment options validation (0..2) */
    if ((cfg->periph_size != LL_DMA_PDATAALIGN_BYTE) &&
        (cfg->periph_size != LL_DMA_PDATAALIGN_HALFWORD) &&
        (cfg->periph_size != LL_DMA_PDATAALIGN_WORD))
    {
        return false;
    }
    if ((cfg->mem_size != LL_DMA_MDATAALIGN_BYTE) &&
        (cfg->mem_size != LL_DMA_MDATAALIGN_HALFWORD) &&
        (cfg->mem_size != LL_DMA_MDATAALIGN_WORD))
    {
        return false;
    }

    /* A6: Direct mode validation: MSIZE must equal PSIZE */
    bool is_fifo = (cfg->direction == LL_DMA_DIRECTION_MEMORY_TO_MEMORY) || cfg->fifo_enable;
    uint32_t psize_shift = cfg->periph_size >> DMA_SxCR_PSIZE_Pos;
    uint32_t msize_shift = cfg->mem_size >> DMA_SxCR_MSIZE_Pos;
    if (!is_fifo && (psize_shift != msize_shift))
    {
        return false;
    }

    /* Priority level validation */
    if ((cfg->priority & ~DMA_SxCR_PL_Msk) != 0U)
    {
        return false;
    }

    /* A4: Request ID validation (7 bits: [6:0]) */
    if (cfg->request > 127U)
    {
        return false;
    }
    if ((cfg->direction != LL_DMA_DIRECTION_MEMORY_TO_MEMORY) && (cfg->request == 0U))
    {
        return false;
    }

    /* Burst validation */
    uint32_t mburst_shift = cfg->mem_burst >> DMA_SxCR_MBURST_Pos;
    uint32_t pburst_shift = cfg->periph_burst >> DMA_SxCR_PBURST_Pos;
    if ((mburst_shift > 3U) || (pburst_shift > 3U))
    {
        return false;
    }
    if ((mburst_shift != 0U) || (pburst_shift != 0U))
    {
        if (!is_fifo)
        {
            return false;
        }
        if (!DMA_BM_CheckFifoBurst(msize_shift, cfg->fifo_threshold, mburst_shift))
        {
            return false;
        }
        if (!DMA_BM_CheckFifoBurst(psize_shift, cfg->fifo_threshold, pburst_shift))
        {
            return false;
        }
    }

    /* FIFO threshold validation */
    if ((cfg->fifo_threshold & ~DMA_SxFCR_FTH_Msk) != 0U)
    {
        return false;
    }

    /* --- Validations passed: configure hardware registers --- */

    if (cfg->enable_clock)
    {
        if (dma == DMA1)
        {
            LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
        }
        else
        {
            LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA2);
        }
        volatile uint32_t dummy = RCC->AHB1ENR;
        (void)dummy;
    }

    /* Ensure stream is disabled before configuring */
    if ((cfg->stream->CR & DMA_SxCR_EN) != 0U)
    {
        if (!DMA_BM_StopEx(cfg->stream, 10000U))
        {
            return false;
        }
    }

    DMA_BM_TakeFlags(cfg->stream);

    /* Configure DMAMUX1 request routing */
    uint32_t dmamux_ch = (dma == DMA1) ? stream_idx : (stream_idx + 8U);
    LL_DMAMUX_SetRequestID(DMAMUX1, dmamux_ch, cfg->request);

    /* Build and write CR */
    cfg->stream->CR = 0U;
    uint32_t cr = cfg->direction | cfg->mode | cfg->periph_inc | cfg->mem_inc |
                  cfg->periph_size | cfg->mem_size | cfg->priority |
                  cfg->mem_burst | cfg->periph_burst;

    if (cfg->double_buffer)
    {
        cr |= LL_DMA_DOUBLEBUFFER_MODE_ENABLE;
    }

    /* A3: Automatic TRBUFF for UART and HASH_IN requests when REV_ID >= 0x2000 */
    if (DMA_BM_IsUartRequest(cfg->request) &&
        ((DBGMCU->IDCODE & 0xFFFF0000U) >= 0x20000000U))
    {
        cr |= DMA_SxCR_TRBUFF;
    }
    cfg->stream->CR = cr;

    /* Write FCR */
    uint32_t fcr = 0U;
    if (is_fifo)
    {
        fcr = LL_DMA_FIFOMODE_ENABLE | cfg->fifo_threshold;
    }
    else
    {
        fcr = LL_DMA_FIFOMODE_DISABLE;
    }
    cfg->stream->FCR = fcr;

    /* Initialize handle */
    h->dma = dma;
    h->ll_stream = stream_idx;
    h->stream = cfg->stream;
    h->error_flags = 0U;
    h->tc_count = 0U;
    h->ht_count = 0U;
    h->dbm_length = 0U;

    return true;
}

bool DMA_LL_Deinit(DmaLL_Handle_t *h, uint32_t spin_limit)
{
    if ((h == NULL) || (h->stream == NULL))
    {
        return false;
    }

    if (!DMA_LL_Stop(h, spin_limit))
    {
        return false;
    }
    DMA_LL_DisableIrq(h);
    DMA_BM_TakeFlags(h->stream);

    h->stream->CR = 0U;
    h->stream->NDTR = 0U;
    h->stream->PAR = 0U;
    h->stream->M0AR = 0U;
    h->stream->M1AR = 0U;
    h->stream->FCR = 0x00000021U;

    if (h->dma != NULL)
    {
        uint32_t dmamux_ch = (h->dma == DMA1) ? h->ll_stream : (h->ll_stream + 8U);
        LL_DMAMUX_SetRequestID(DMAMUX1, dmamux_ch, 0U);
    }

    h->stream = NULL;
    h->dma = NULL;
    h->ll_stream = 0U;
    h->error_flags = 0U;
    h->tc_count = 0U;
    h->ht_count = 0U;
    h->dbm_length = 0U;

    return true;
}

bool DMA_LL_Start(DmaLL_Handle_t *h, uint32_t periph_addr, uint32_t mem_addr, uint32_t length)
{
    if ((h == NULL) || (h->stream == NULL))
    {
        return false;
    }

    /* A4: NDTR validation (0 < length <= 65535) */
    if ((length == 0U) || (length > 65535U))
    {
        return false;
    }

    /* Stream must be disabled before configuring addresses and counter */
    if ((h->stream->CR & DMA_SxCR_EN) != 0U)
    {
        return false;
    }

    uint32_t psize = (h->stream->CR & DMA_SxCR_PSIZE) >> DMA_SxCR_PSIZE_Pos;
    uint32_t msize = (h->stream->CR & DMA_SxCR_MSIZE) >> DMA_SxCR_MSIZE_Pos;
    uint32_t pburst = (h->stream->CR & DMA_SxCR_PBURST) >> DMA_SxCR_PBURST_Pos;
    uint32_t mburst = (h->stream->CR & DMA_SxCR_MBURST) >> DMA_SxCR_MBURST_Pos;
    uint32_t dir = h->stream->CR & DMA_SxCR_DIR;

    /* RM0399 Table 115 (A4 / X3): If PSIZE < MSIZE, length must be a multiple of (MSIZE / PSIZE) */
    if (psize < msize)
    {
        uint32_t ratio = (1U << msize) / (1U << psize);
        if ((length % ratio) != 0U)
        {
            return false;
        }
    }

    uint32_t p_elem = 1U << psize;
    uint32_t m_elem = 1U << msize;
    uint32_t pburst_beats = s_burst_beats[pburst];
    uint32_t mburst_beats = s_burst_beats[mburst];
    uint32_t p_align = pburst_beats * p_elem;
    uint32_t m_align = mburst_beats * m_elem;

    /* A4/A9: Alignment checks */
    if ((periph_addr & (p_align - 1U)) != 0U)
    {
        return false;
    }
    if ((mem_addr & (m_align - 1U)) != 0U)
    {
        return false;
    }

    /* A5: Reachability validation.
     * NDTR counts in PSIZE units (RM0399 §16.3.12), so byte span = NDT × PSIZE. */
    if (!DMA_MEM_BM_IsReachable(DMA_MEM_BM_MASTER_DMA, mem_addr, length * p_elem))
    {
        return false;
    }
    if (dir == LL_DMA_DIRECTION_MEMORY_TO_MEMORY)
    {
        if (!DMA_MEM_BM_IsReachable(DMA_MEM_BM_MASTER_DMA, periph_addr, length * p_elem))
        {
            return false;
        }
    }

    h->stream->PAR = periph_addr;
    h->stream->M0AR = mem_addr;
    h->stream->NDTR = length;

    /* A1: Clear flags before enabling */
    DMA_BM_TakeFlags(h->stream);

    return DMA_BM_StartEx(h->stream);
}

bool DMA_LL_StartDoubleBuffer(DmaLL_Handle_t *h, uint32_t periph_addr,
                              uint32_t mem0, uint32_t mem1, uint32_t length)
{
    if ((h == NULL) || (h->stream == NULL) || (mem1 == 0U))
    {
        return false;
    }

    if ((length == 0U) || (length > 65535U))
    {
        return false;
    }

    if ((h->stream->CR & DMA_SxCR_EN) != 0U)
    {
        return false;
    }

    if ((h->stream->CR & DMA_SxCR_DBM) == 0U)
    {
        return false;
    }

    if ((h->stream->CR & DMA_SxCR_DIR) == DMA_SxCR_DIR_1)
    {
        return false;
    }

    uint32_t psize = (h->stream->CR & DMA_SxCR_PSIZE) >> DMA_SxCR_PSIZE_Pos;
    uint32_t msize = (h->stream->CR & DMA_SxCR_MSIZE) >> DMA_SxCR_MSIZE_Pos;
    uint32_t pburst = (h->stream->CR & DMA_SxCR_PBURST) >> DMA_SxCR_PBURST_Pos;
    uint32_t mburst = (h->stream->CR & DMA_SxCR_MBURST) >> DMA_SxCR_MBURST_Pos;

    /* RM0399 Table 115 (A4 / X3): If PSIZE < MSIZE, length must be a multiple of (MSIZE / PSIZE) */
    if (psize < msize)
    {
        uint32_t ratio = (1U << msize) / (1U << psize);
        if ((length % ratio) != 0U)
        {
            return false;
        }
    }

    uint32_t p_elem = 1U << psize;
    uint32_t m_elem = 1U << msize;
    uint32_t pburst_beats = s_burst_beats[pburst];
    uint32_t mburst_beats = s_burst_beats[mburst];
    uint32_t p_align = pburst_beats * p_elem;
    uint32_t m_align = mburst_beats * m_elem;

    if (((periph_addr & (p_align - 1U)) != 0U) ||
        ((mem0 & (m_align - 1U)) != 0U) ||
        ((mem1 & (m_align - 1U)) != 0U))
    {
        return false;
    }

    /* NDTR counts in PSIZE units (RM0399 §16.3.12) */
    if (!DMA_MEM_BM_IsReachable(DMA_MEM_BM_MASTER_DMA, mem0, length * p_elem) ||
        !DMA_MEM_BM_IsReachable(DMA_MEM_BM_MASTER_DMA, mem1, length * p_elem))
    {
        return false;
    }

    h->stream->PAR = periph_addr;
    h->stream->M0AR = mem0;
    h->stream->M1AR = mem1;
    h->stream->NDTR = length;
    h->dbm_length = length;

    DMA_BM_TakeFlags(h->stream);

    return DMA_BM_StartEx(h->stream);
}

bool DMA_LL_SetIdleMemory(DmaLL_Handle_t *h, uint32_t addr)
{
    if ((h == NULL) || (h->stream == NULL))
    {
        return false;
    }

    return DMA_BM_SetIdleMemoryEx(h->stream, addr, h->dbm_length);
}

bool DMA_LL_Stop(DmaLL_Handle_t *h, uint32_t spin_limit)
{
    if ((h == NULL) || (h->stream == NULL))
    {
        return false;
    }

    return DMA_BM_StopEx(h->stream, spin_limit);
}

uint32_t DMA_LL_GetRemaining(const DmaLL_Handle_t *h)
{
    if ((h == NULL) || (h->stream == NULL))
    {
        return 0U;
    }

    return DMA_BM_GetRemaining(h->stream);
}

uint32_t DMA_LL_TakeFlags(DmaLL_Handle_t *h)
{
    if ((h == NULL) || (h->stream == NULL))
    {
        return 0U;
    }

    return DMA_BM_TakeFlags(h->stream);
}

uint32_t DMA_LL_TakeErrors(DmaLL_Handle_t *h)
{
    if (h == NULL)
    {
        return 0U;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t flags = h->error_flags;
    h->error_flags = 0U;
    __set_PRIMASK(primask);

    return flags;
}

bool DMA_LL_EnableIrq(DmaLL_Handle_t *h, uint32_t it_mask, uint32_t priority)
{
    if ((h == NULL) || (h->stream == NULL))
    {
        return false;
    }

    IRQn_Type irqn = DMA_BM_IRQN_INVALID;
    if (!DMA_BM_GetIRQnEx(h->stream, &irqn))
    {
        return false;
    }

    uint32_t cr_it = 0U;
    if ((it_mask & DMA_LL_IT_TC) != 0U)  cr_it |= DMA_SxCR_TCIE;
    if ((it_mask & DMA_LL_IT_HT) != 0U)  cr_it |= DMA_SxCR_HTIE;
    if ((it_mask & DMA_LL_IT_TE) != 0U)  cr_it |= DMA_SxCR_TEIE;
    if ((it_mask & DMA_LL_IT_DME) != 0U) cr_it |= DMA_SxCR_DMEIE;
    h->stream->CR |= cr_it;

    if ((it_mask & DMA_LL_IT_FE) != 0U)
    {
        h->stream->FCR |= DMA_SxFCR_FEIE;
    }

    NVIC_SetPriority(irqn, priority);
    NVIC_EnableIRQ(irqn);

    return true;
}

void DMA_LL_DisableIrq(DmaLL_Handle_t *h)
{
    if ((h == NULL) || (h->stream == NULL))
    {
        return;
    }

    h->stream->CR &= ~(DMA_SxCR_TCIE | DMA_SxCR_HTIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE);
    h->stream->FCR &= ~DMA_SxFCR_FEIE;

    IRQn_Type irqn = DMA_BM_IRQN_INVALID;
    if (DMA_BM_GetIRQnEx(h->stream, &irqn))
    {
        NVIC_DisableIRQ(irqn);
    }
}

void DMA_LL_IrqService(DmaLL_Handle_t *h, DmaLL_Callback_t cb, void *ctx)
{
    if ((h == NULL) || (h->stream == NULL))
    {
        return;
    }

    uint32_t flags = DMA_BM_TakeFlags(h->stream);
    if (flags == 0U)
    {
        return;
    }

    if ((flags & DMA_LL_FLAG_TC) != 0U)
    {
        h->tc_count++;
    }
    if ((flags & DMA_LL_FLAG_HT) != 0U)
    {
        h->ht_count++;
    }

    /* RM0399 §16.3.20: TE and DME are fatal transfer errors that automatically disable stream */
    uint32_t errs = flags & (DMA_LL_FLAG_TE | DMA_LL_FLAG_DME | DMA_LL_FLAG_FE);
    if (errs != 0U)
    {
        h->error_flags |= errs;
    }

    if (cb != NULL)
    {
        cb(h, flags, ctx);
    }
}

bool DMA_LL_MemCopy(DmaLL_Handle_t *h, void *dst, const void *src, uint32_t length, uint32_t spin_limit)
{
    if ((h == NULL) || (h->stream == NULL) || (dst == NULL) || (src == NULL) || (length == 0U))
    {
        return false;
    }

    if ((h->stream->CR & DMA_SxCR_DIR) != DMA_SxCR_DIR_1)
    {
        return false;
    }

    uint32_t psize = (h->stream->CR & DMA_SxCR_PSIZE) >> DMA_SxCR_PSIZE_Pos;
    uint32_t bytes = length * (1U << psize);

    DMA_MEM_BM_CleanForTx(src, bytes);
    if (!DMA_MEM_BM_PrepareRx(dst, bytes))
    {
        return false;
    }

    if (!DMA_LL_Start(h, (uint32_t)src, (uint32_t)dst, length))
    {
        return false;
    }

    uint32_t spin = (spin_limit == 0U) ? 1000000UL : spin_limit;
    bool ok = false;

    while (spin > 0U)
    {
        uint32_t flags = DMA_BM_TakeFlags(h->stream);
        if ((flags & (DMA_LL_FLAG_TE | DMA_LL_FLAG_DME)) != 0U)
        {
            h->error_flags |= flags;
            break;
        }
        if ((flags & DMA_LL_FLAG_TC) != 0U)
        {
            ok = true;
            break;
        }
        spin--;
    }

    if (!ok)
    {
        DMA_LL_Stop(h, 10000U);
        return false;
    }

    DMA_MEM_BM_CompleteRx(dst, bytes);
    return true;
}
