/**
  ******************************************************************************
  * @file    dma_hal.h
  * @brief   Hardware Abstraction Layer DMA driver for STM32H755.
  *
  *          Driver tier: *_hal (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Wraps ST HAL for DMA1, DMA2 streams and BDMA channels.
  *          Provides unified configuration, errata checking, IRQ dispatching,
  *          and memory copy helpers.
  ******************************************************************************
  */

#ifndef DMA_HAL_H
#define DMA_HAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "stm32h7xx_hal.h"
#include "dma_bm.h"
#include "dma_mem_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(HAL_DMA_MODULE_ENABLED)

/**
  * @brief DMA HAL configuration structure (Dependency Injection).
  */
typedef struct
{
    void     *instance;       /*!< DMA1_Stream0..DMA2_Stream7 or BDMA_Channel0..7         */
    uint32_t  request;        /*!< DMA_REQUEST_* / BDMA_REQUEST_*                          */
    uint32_t  direction;      /*!< DMA_PERIPH_TO_MEMORY / DMA_MEMORY_TO_PERIPH / _TO_MEMORY */
    uint32_t  periph_inc;     /*!< DMA_PINC_ENABLE / DMA_PINC_DISABLE                      */
    uint32_t  mem_inc;        /*!< DMA_MINC_ENABLE / DMA_MINC_DISABLE                      */
    uint32_t  periph_align;   /*!< DMA_PDATAALIGN_BYTE / _HALFWORD / _WORD                 */
    uint32_t  mem_align;      /*!< DMA_MDATAALIGN_BYTE / _HALFWORD / _WORD                 */
    uint32_t  mode;           /*!< DMA_NORMAL / DMA_CIRCULAR / DMA_PFCTRL                  */
    uint32_t  priority;       /*!< DMA_PRIORITY_LOW / _MEDIUM / _HIGH / _VERY_HIGH         */
    uint32_t  fifo_mode;      /*!< DMA_FIFOMODE_DISABLE / DMA_FIFOMODE_ENABLE              */
    uint32_t  fifo_threshold; /*!< DMA_FIFO_THRESHOLD_1QUARTERFULL..FULL                   */
    uint32_t  mem_burst;      /*!< DMA_MBURST_SINGLE / DMA_MBURST_INC4 / _INC8 / _INC16    */
    uint32_t  periph_burst;   /*!< DMA_PBURST_SINGLE / DMA_PBURST_INC4 / _INC8 / _INC16    */
    bool      enable_clock;   /*!< DMA1/DMA2 on AHB1, BDMA on AHB4                         */
    int32_t   irq_priority;   /*!< < 0: leave NVIC untouched                               */
} DmaHal_Config_t;

/**
  * @brief  Initializes DMA stream or BDMA channel via ST HAL.
  * @param  hdma Pointer to HAL DMA handle.
  * @param  cfg  Pointer to configuration structure.
  * @return HAL status.
  */
HAL_StatusTypeDef DMA_HAL_Init(DMA_HandleTypeDef *hdma, const DmaHal_Config_t *cfg);

/**
  * @brief  Deinitializes DMA stream or BDMA channel via ST HAL and disables IRQ.
  * @param  hdma Pointer to HAL DMA handle.
  * @return HAL status.
  */
HAL_StatusTypeDef DMA_HAL_DeInit(DMA_HandleTypeDef *hdma);

/**
  * @brief  Verifies and ensures that ES0445 TRBUFF (bit 20) is set for UART/HASH DMA streams.
  * @param  hdma Pointer to initialized HAL DMA handle.
  * @return HAL_OK if TRBUFF is verified or set, HAL_ERROR on invalid handle.
  */
HAL_StatusTypeDef DMA_HAL_CheckErrata(const DMA_HandleTypeDef *hdma);

/**
  * @brief  Resolves NVIC IRQn for DMA1/DMA2 stream or BDMA channel instance.
  * @warning Returns DMA_BM_IRQN_INVALID ((IRQn_Type)(-1)) on failure, which aliases
  *          SysTick_IRQn. Never pass unchecked return values to NVIC_* functions.
  *          Use DMA_HAL_GetIRQnEx() instead for safe resolution.
  * @param  instance Pointer to stream (DMA1_Stream0..) or channel (BDMA_Channel0..).
  * @return IRQn_Type or DMA_BM_IRQN_INVALID ((IRQn_Type)(-1)) if invalid.
  */
IRQn_Type DMA_HAL_GetIRQn(const void *instance);

/**
  * @brief  Resolves NVIC IRQn for DMA1/DMA2 stream or BDMA channel instance (safe variant).
  * @param  instance Pointer to stream (DMA1_Stream0..) or channel (BDMA_Channel0..).
  * @param  out      Pointer to store resolved IRQn_Type (untouched on failure).
  * @return true if instance is valid, false otherwise (*out untouched).
  */
bool DMA_HAL_GetIRQnEx(const void *instance, IRQn_Type *out);

/**
  * @brief  Dispatches HAL_DMA_IRQHandler for the registered handle associated with instance.
  * @param  instance Pointer to stream or channel.
  */
void DMA_HAL_IRQDispatch(const void *instance);

/**
  * @brief  Performs synchronous memory-to-memory copy with D-Cache maintenance and reachability validation.
  * @param  hdma       Pointer to initialized HAL DMA handle in M2M mode.
  * @param  dst        Destination memory buffer.
  * @param  src        Source memory buffer.
  * @param  bytes      Number of bytes to copy.
  * @param  timeout_ms Timeout in milliseconds (HAL timebase).
  * @return HAL status.
  */
HAL_StatusTypeDef DMA_HAL_MemCopy(DMA_HandleTypeDef *hdma, void *dst, const void *src,
                                  uint32_t bytes, uint32_t timeout_ms);

#endif /* HAL_DMA_MODULE_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* DMA_HAL_H */
