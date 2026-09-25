/**
  ******************************************************************************
  * @file    dma_ll.h
  * @brief   Low-Layer DMA driver for STM32H755 (DMA1, DMA2, and DMAMUX1).
  *
  *          Driver tier: *_ll (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Based strictly on ST LL static inline functions and CMSIS core.
  *          Self-contained header, does not require USE_FULL_LL_DRIVER, and
  *          does not include stm32h7xx_hal.h.
  ******************************************************************************
  */

#ifndef DMA_LL_H
#define DMA_LL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "stm32h7xx.h"
#include "stm32h7xx_ll_dma.h"
#include "stm32h7xx_ll_dmamux.h"
#include "stm32h7xx_ll_bus.h"
#include "dma_bm.h"
#include "dma_mem_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Abstracted Status / Event Flags ------------------------------------- */
#define DMA_LL_FLAG_TC                DMA_BM_FLAG_TC   /*!< Transfer Complete         */
#define DMA_LL_FLAG_HT                DMA_BM_FLAG_HT   /*!< Half Transfer Complete    */
#define DMA_LL_FLAG_TE                DMA_BM_FLAG_TE   /*!< Transfer Error            */
#define DMA_LL_FLAG_DME               DMA_BM_FLAG_DME  /*!< Direct Mode Error         */
#define DMA_LL_FLAG_FE                DMA_BM_FLAG_FE   /*!< FIFO Error                */
#define DMA_LL_FLAG_ALL               DMA_BM_FLAG_ALL

/* ---- Interrupt Enable Masks for DMA_LL_EnableIrq ------------------------- */
#define DMA_LL_IT_TC                  (1UL << 0)  /*!< Transfer Complete Interrupt    */
#define DMA_LL_IT_HT                  (1UL << 1)  /*!< Half Transfer Interrupt        */
#define DMA_LL_IT_TE                  (1UL << 2)  /*!< Transfer Error Interrupt       */
#define DMA_LL_IT_DME                 (1UL << 3)  /*!< Direct Mode Error Interrupt    */
#define DMA_LL_IT_FE                  (1UL << 4)  /*!< FIFO Error Interrupt           */
#define DMA_LL_IT_ALL                 (DMA_LL_IT_TC | DMA_LL_IT_HT | DMA_LL_IT_TE | \
                                       DMA_LL_IT_DME | DMA_LL_IT_FE)

/**
  * @brief DMA Stream LL configuration structure (Dependency Injection).
  */
typedef struct
{
    DMA_Stream_TypeDef *stream;        /*!< Same injection as BM/HAL (C7)              */
    uint32_t            request;       /*!< LL_DMAMUX1_REQ_* (numerically DMAMUX1 ID)  */
    uint32_t            direction;     /*!< LL_DMA_DIRECTION_*                         */
    uint32_t            mode;          /*!< LL_DMA_MODE_NORMAL / _CIRCULAR / _PFCTRL   */
    uint32_t            periph_inc;    /*!< LL_DMA_PERIPH_INCREMENT / _NOINCREMENT     */
    uint32_t            mem_inc;       /*!< LL_DMA_MEMORY_INCREMENT / _NOINCREMENT     */
    uint32_t            periph_size;   /*!< LL_DMA_PDATAALIGN_*                        */
    uint32_t            mem_size;      /*!< LL_DMA_MDATAALIGN_*                        */
    uint32_t            priority;      /*!< LL_DMA_PRIORITY_*                          */
    bool                fifo_enable;   /*!< Enable FIFO mode (forced for M2M)          */
    uint32_t            fifo_threshold;/*!< LL_DMA_FIFOTHRESHOLD_*                     */
    uint32_t            mem_burst;     /*!< LL_DMA_MBURST_SINGLE/INC4/INC8/INC16       */
    uint32_t            periph_burst;  /*!< LL_DMA_PBURST_SINGLE/INC4/INC8/INC16       */
    bool                double_buffer; /*!< Double buffer mode (DBM)                   */
    bool                enable_clock;  /*!< Enable AHB1 DMA peripheral clock in Init   */
    bool                fail_if_busy;  /*!< Refuse configuration if stream EN=1        */
} DmaLL_Config_t;

typedef struct DmaLL_Handle DmaLL_Handle_t;
typedef void (*DmaLL_Callback_t)(DmaLL_Handle_t *h, uint32_t flags, void *ctx);

/**
  * @brief DMA Stream LL runtime handle structure.
  */
struct DmaLL_Handle
{
    DMA_TypeDef        *dma;           /*!< DMA1 or DMA2 controller                    */
    uint32_t            ll_stream;     /*!< LL_DMA_STREAM_0..7                         */
    DMA_Stream_TypeDef *stream;        /*!< Stream hardware register pointer           */
    volatile uint32_t   error_flags;   /*!< Accumulated TE/DME/FE (retrieved by TakeErrors) */
    volatile uint32_t   tc_count;      /*!< Transfer Complete event count              */
    volatile uint32_t   ht_count;      /*!< Half Transfer event count                  */
    uint32_t            dbm_length;    /*!< Buffer length in items recorded in StartDoubleBuffer */
};

/**
  * @brief  Initializes DMA stream and DMAMUX channel via ST LL API.
  * @param  h   Pointer to driver handle.
  * @param  cfg Pointer to configuration structure.
  * @return true if configured successfully, false on invalid parameters.
  */
bool DMA_LL_Init(DmaLL_Handle_t *h, const DmaLL_Config_t *cfg);

/**
  * @brief  Deinitializes DMA stream, DMAMUX channel, and clears handle.
  * @param  h          Pointer to driver handle.
  * @param  spin_limit Maximum iterations to wait for stream disable.
  * @return true on success, false on timeout or invalid handle.
  */
bool DMA_LL_Deinit(DmaLL_Handle_t *h, uint32_t spin_limit);

/**
  * @brief  Arms transfer addresses and count, and starts the stream.
  * @param  h           Pointer to driver handle.
  * @param  periph_addr Peripheral register / source address.
  * @param  mem_addr    Memory buffer / destination address.
  * @param  length      Number of data units to transfer (NDTR: 1..65535).
  * @return true if started successfully, false on invalid parameters or bus error.
  */
bool DMA_LL_Start(DmaLL_Handle_t *h, uint32_t periph_addr, uint32_t mem_addr, uint32_t length);

/**
  * @brief  Arms double-buffer transfer (DBM) and starts the stream.
  * @param  h           Pointer to driver handle.
  * @param  periph_addr Peripheral register address.
  * @param  mem0        Memory 0 buffer address (M0AR).
  * @param  mem1        Memory 1 buffer address (M1AR).
  * @param  length      Number of data units per buffer (NDTR: 1..65535).
  * @return true if started successfully, false on invalid parameters or bus error.
  */
bool DMA_LL_StartDoubleBuffer(DmaLL_Handle_t *h, uint32_t periph_addr,
                              uint32_t mem0, uint32_t mem1, uint32_t length);

/**
  * @brief  Updates the idle memory buffer address in double-buffer mode using recorded dbm_length.
  * @param  h    Pointer to driver handle.
  * @param  addr New buffer physical address (must be aligned to MBURST x MSIZE).
  * @return true on success, false if not in DBM or address invalid/unreachable.
  */
bool DMA_LL_SetIdleMemory(DmaLL_Handle_t *h, uint32_t addr);

/**
  * @brief  Stops DMA stream transfer and polls until EN bit is cleared.
  * @param  h          Pointer to driver handle.
  * @param  spin_limit Maximum iterations to wait (0 -> default timeout).
  * @return true if stream disabled successfully, false on timeout.
  */
bool DMA_LL_Stop(DmaLL_Handle_t *h, uint32_t spin_limit);

/**
  * @brief  Reads remaining data items to transfer (NDTR).
  * @param  h Pointer to driver handle.
  * @return Remaining items count.
  */
uint32_t DMA_LL_GetRemaining(const DmaLL_Handle_t *h);

/**
  * @brief  Reads and clears current active status flags for the stream.
  * @param  h Pointer to driver handle.
  * @return Bitmask of flags cleared (DMA_LL_FLAG_*).
  */
uint32_t DMA_LL_TakeFlags(DmaLL_Handle_t *h);

/**
  * @brief  Atomically reads and clears accumulated error flags inside a PRIMASK critical section.
  * @param  h Pointer to driver handle.
  * @return Bitmask of accumulated errors (DMA_LL_FLAG_TE | DMA_LL_FLAG_DME | DMA_LL_FLAG_FE).
  */
uint32_t DMA_LL_TakeErrors(DmaLL_Handle_t *h);

/**
  * @brief  Enables stream interrupt bits and configures/enables NVIC IRQ.
  * @param  h        Pointer to driver handle.
  * @param  it_mask  Bitmask of DMA_LL_IT_* to enable.
  * @param  priority NVIC priority level for stream IRQ.
  * @return true on success, false on invalid handle or IRQn lookup failure.
  */
bool DMA_LL_EnableIrq(DmaLL_Handle_t *h, uint32_t it_mask, uint32_t priority);

/**
  * @brief  Disables stream interrupt bits and disables NVIC IRQ.
  * @param  h Pointer to driver handle.
  */
void DMA_LL_DisableIrq(DmaLL_Handle_t *h);

/**
  * @brief  ISR service helper: reads and clears flags, updates counts, dispatches callback.
  * @param  h   Pointer to driver handle.
  * @param  cb  User callback invoked with active flags (can be NULL).
  * @param  ctx User context pointer passed to callback.
  */
void DMA_LL_IrqService(DmaLL_Handle_t *h, DmaLL_Callback_t cb, void *ctx);

/**
  * @brief  Synchronous memory-to-memory block transfer helper with cache maintenance.
  * @param  h          Pointer to driver handle initialized in M2M direction.
  * @param  dst        Destination memory buffer pointer.
  * @param  src        Source memory buffer pointer.
  * @param  length     Number of elements to transfer per configured MSIZE.
  * @param  spin_limit Maximum iterations to poll for completion (0 -> default).
  * @return true if transfer completed successfully, false on error or timeout.
  */
bool DMA_LL_MemCopy(DmaLL_Handle_t *h, void *dst, const void *src, uint32_t length, uint32_t spin_limit);

#ifdef __cplusplus
}
#endif

#endif /* DMA_LL_H */
