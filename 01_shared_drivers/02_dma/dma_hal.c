/**
  ******************************************************************************
  * @file    dma_hal.c
  * @brief   Hardware Abstraction Layer DMA driver implementation for STM32H755.
  *
  *          Driver tier: *_hal (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Wraps ST HAL for DMA1, DMA2 streams and BDMA channels.
  ******************************************************************************
  */

#include "dma_hal.h"

#if defined(HAL_DMA_MODULE_ENABLED)

#define DMA_HAL_REGISTRY_SIZE 24U
static DMA_HandleTypeDef *s_dma_registry[DMA_HAL_REGISTRY_SIZE];

static int32_t DMA_HAL_GetInstanceIndex(const void *instance)
{
    if (instance == NULL)
    {
        return -1;
    }

    DMA_TypeDef *dma = DMA_BM_GetController((const DMA_Stream_TypeDef *)instance);
    uint32_t stream_idx = DMA_BM_GetStreamIndex((const DMA_Stream_TypeDef *)instance);

    if ((dma != NULL) && (stream_idx <= 7U))
    {
        if (dma == DMA1)
        {
            return (int32_t)stream_idx; /* 0..7 */
        }
        else if (dma == DMA2)
        {
            return (int32_t)(8U + stream_idx); /* 8..15 */
        }
    }

    /* Check BDMA channels */
    uint32_t addr = (uint32_t)instance;
    if ((addr >= (uint32_t)BDMA_Channel0) && (addr <= (uint32_t)BDMA_Channel7))
    {
        uint32_t offset = addr - (uint32_t)BDMA_Channel0;
        if ((offset % sizeof(BDMA_Channel_TypeDef)) == 0U)
        {
            return (int32_t)(16U + (offset / sizeof(BDMA_Channel_TypeDef))); /* 16..23 */
        }
    }

    return -1;
}

IRQn_Type DMA_HAL_GetIRQn(const void *instance)
{
    if (instance == NULL)
    {
        return (IRQn_Type)(-1);
    }

    DMA_TypeDef *dma = DMA_BM_GetController((const DMA_Stream_TypeDef *)instance);
    if (dma != NULL)
    {
        return DMA_BM_GetIRQn((const DMA_Stream_TypeDef *)instance);
    }

    uint32_t addr = (uint32_t)instance;
    if ((addr >= (uint32_t)BDMA_Channel0) && (addr <= (uint32_t)BDMA_Channel7))
    {
        uint32_t offset = addr - (uint32_t)BDMA_Channel0;
        if ((offset % sizeof(BDMA_Channel_TypeDef)) == 0U)
        {
            return (IRQn_Type)(BDMA_Channel0_IRQn + (offset / sizeof(BDMA_Channel_TypeDef)));
        }
    }

    return (IRQn_Type)(-1);
}

void DMA_HAL_IRQDispatch(const void *instance)
{
    int32_t idx = DMA_HAL_GetInstanceIndex(instance);
    if ((idx >= 0) && (s_dma_registry[idx] != NULL))
    {
        HAL_DMA_IRQHandler(s_dma_registry[idx]);
    }
}

HAL_StatusTypeDef DMA_HAL_CheckErrata(const DMA_HandleTypeDef *hdma)
{
    if ((hdma == NULL) || (hdma->Instance == NULL))
    {
        return HAL_ERROR;
    }

    /* ES0445 2.20.3 / 2.3.12: TRBUFF bit 20 applies to DMA1/DMA2 streams */
    if (IS_DMA_STREAM_INSTANCE(hdma->Instance) != 0U)
    {
        if (DMA_BM_IsUartRequest(hdma->Init.Request))
        {
            if ((DBGMCU->IDCODE & 0xFFFF0000U) >= 0x20000000U)
            {
                DMA_Stream_TypeDef *s = (DMA_Stream_TypeDef *)hdma->Instance;
                if ((s->CR & DMA_BM_CR_TRBUFF) == 0U)
                {
                    s->CR |= DMA_BM_CR_TRBUFF;
                }
                if ((s->CR & DMA_BM_CR_TRBUFF) == 0U)
                {
                    return HAL_ERROR;
                }
            }
        }
    }

    return HAL_OK;
}

HAL_StatusTypeDef DMA_HAL_Init(DMA_HandleTypeDef *hdma, const DmaHal_Config_t *cfg)
{
    if ((hdma == NULL) || (cfg == NULL) || (cfg->instance == NULL))
    {
        return HAL_ERROR;
    }

    int32_t inst_idx = DMA_HAL_GetInstanceIndex(cfg->instance);
    if (inst_idx < 0)
    {
        return HAL_ERROR;
    }

    /* Clock enabling */
    if (cfg->enable_clock)
    {
        if (inst_idx < 8)
        {
            __HAL_RCC_DMA1_CLK_ENABLE();
        }
        else if (inst_idx < 16)
        {
            __HAL_RCC_DMA2_CLK_ENABLE();
        }
        else
        {
            __HAL_RCC_BDMA_CLK_ENABLE();
        }
    }

    /* Validation: DMA1/DMA2 streams */
    uint32_t fifo_mode = cfg->fifo_mode;
    if (inst_idx < 16)
    {
        if (cfg->direction == DMA_MEMORY_TO_MEMORY)
        {
            if (cfg->mode != DMA_NORMAL)
            {
                return HAL_ERROR; /* Circular or PFCTRL forbidden with M2M */
            }
            /* M2M mandates FIFO mode (RM0399 §16.3.7) */
            fifo_mode = DMA_FIFOMODE_ENABLE;
        }
        else
        {
            /* Direct mode requires MSIZE == PSIZE (A6) */
            if ((fifo_mode == DMA_FIFOMODE_DISABLE) &&
                (cfg->periph_align != cfg->mem_align))
            {
                return HAL_ERROR;
            }
        }
    }
    else /* BDMA channels */
    {
        if ((cfg->direction == DMA_MEMORY_TO_MEMORY) && (cfg->mode != DMA_NORMAL))
        {
            return HAL_ERROR;
        }
    }

    hdma->Instance                 = cfg->instance;
    hdma->Init.Request             = cfg->request;
    hdma->Init.Direction           = cfg->direction;
    hdma->Init.PeriphInc           = cfg->periph_inc;
    hdma->Init.MemInc              = cfg->mem_inc;
    hdma->Init.PeriphDataAlignment = cfg->periph_align;
    hdma->Init.MemDataAlignment    = cfg->mem_align;
    hdma->Init.Mode                = cfg->mode;
    hdma->Init.Priority            = cfg->priority;
    hdma->Init.FIFOMode            = fifo_mode;
    hdma->Init.FIFOThreshold       = cfg->fifo_threshold;
    hdma->Init.MemBurst            = cfg->mem_burst;
    hdma->Init.PeriphBurst         = cfg->periph_burst;

    HAL_StatusTypeDef status = HAL_DMA_Init(hdma);
    if (status != HAL_OK)
    {
        return status;
    }

    /* Ensure TRBUFF errata fix was applied */
    (void)DMA_HAL_CheckErrata(hdma);

    /* NVIC configuration if requested */
    if (cfg->irq_priority >= 0)
    {
        IRQn_Type irqn = DMA_HAL_GetIRQn(cfg->instance);
        if ((int32_t)irqn >= 0)
        {
            HAL_NVIC_SetPriority(irqn, (uint32_t)cfg->irq_priority, 0U);
            HAL_NVIC_EnableIRQ(irqn);
        }
    }

    /* Register handle in dispatch table */
    s_dma_registry[inst_idx] = hdma;

    return HAL_OK;
}

HAL_StatusTypeDef DMA_HAL_DeInit(DMA_HandleTypeDef *hdma)
{
    if ((hdma == NULL) || (hdma->Instance == NULL))
    {
        return HAL_ERROR;
    }

    int32_t inst_idx = DMA_HAL_GetInstanceIndex(hdma->Instance);
    if (inst_idx >= 0)
    {
        IRQn_Type irqn = DMA_HAL_GetIRQn(hdma->Instance);
        if ((int32_t)irqn >= 0)
        {
            HAL_NVIC_DisableIRQ(irqn);
        }
        s_dma_registry[inst_idx] = NULL;
    }

    return HAL_DMA_DeInit(hdma);
}

HAL_StatusTypeDef DMA_HAL_MemCopy(DMA_HandleTypeDef *hdma, void *dst, const void *src,
                                  uint32_t bytes, uint32_t timeout_ms)
{
    if ((hdma == NULL) || (hdma->Instance == NULL) || (dst == NULL) || (src == NULL) || (bytes == 0U))
    {
        return HAL_ERROR;
    }

    int32_t inst_idx = DMA_HAL_GetInstanceIndex(hdma->Instance);
    if (inst_idx < 0)
    {
        return HAL_ERROR;
    }

    /* Reachability check */
    DmaMemBm_Master_t master = (inst_idx >= 16) ? DMA_MEM_BM_MASTER_BDMA : DMA_MEM_BM_MASTER_DMA;
    if (!DMA_MEM_BM_IsReachable(master, (uint32_t)src, bytes))
    {
        return HAL_ERROR;
    }
    if (!DMA_MEM_BM_IsReachable(master, (uint32_t)dst, bytes))
    {
        return HAL_ERROR;
    }

    /* X1: NDTR counts in PSIZE units (RM0399 §16.3.12).
     * items = bytes / psize_bytes.  Also validate memory alignment. */
    uint32_t psize_bytes = 1U;
    if (hdma->Init.PeriphDataAlignment == DMA_PDATAALIGN_WORD)
    {
        psize_bytes = 4U;
    }
    else if (hdma->Init.PeriphDataAlignment == DMA_PDATAALIGN_HALFWORD)
    {
        psize_bytes = 2U;
    }

    uint32_t msize_bytes = 1U;
    if (hdma->Init.MemDataAlignment == DMA_MDATAALIGN_WORD)
    {
        msize_bytes = 4U;
    }
    else if (hdma->Init.MemDataAlignment == DMA_MDATAALIGN_HALFWORD)
    {
        msize_bytes = 2U;
    }

    if (((bytes % psize_bytes) != 0U) || ((bytes % msize_bytes) != 0U))
    {
        return HAL_ERROR;
    }
    uint32_t items = bytes / psize_bytes;

    /* A4 / X2: NDTR is a 16-bit register (1..65535 items) */
    if ((items == 0U) || (items > 65535U))
    {
        return HAL_ERROR;
    }

    /* D-Cache maintenance (no-op on CM4 or when D-Cache is disabled) */
    DMA_MEM_BM_CleanForTx(src, bytes);
    if (!DMA_MEM_BM_PrepareRx(dst, bytes))
    {
        return HAL_ERROR;
    }

    HAL_StatusTypeDef status = HAL_DMA_Start(hdma, (uint32_t)src, (uint32_t)dst, items);
    if (status != HAL_OK)
    {
        return status;
    }

    status = HAL_DMA_PollForTransfer(hdma, HAL_DMA_FULL_TRANSFER, timeout_ms);
    if (status != HAL_OK)
    {
        (void)HAL_DMA_Abort(hdma);
        return status;
    }

    (void)DMA_MEM_BM_CompleteRx(dst, bytes);
    return HAL_OK;
}

#endif /* HAL_DMA_MODULE_ENABLED */
