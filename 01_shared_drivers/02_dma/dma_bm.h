/**
  ******************************************************************************
  * @file    dma_bm.h
  * @brief   Bare-metal DMA driver for STM32H755 (DMA1, DMA2, and DMAMUX1).
  *
  *          Driver tier: *_bm (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Zero overhead, direct register manipulation, hardware-agnostic,
  *          suitable for deterministic Cortex-M4 loops and pre-RTOS bring-up.
  ******************************************************************************
  */

#ifndef DMA_BM_H
#define DMA_BM_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 32-byte alignment macro for AXI/AHB bus burst performance and cache safety */
#define DMA_BM_ALIGN_32   __attribute__((aligned(32)))

/* ---- Transfer Direction -------------------------------------------------- */
#define DMA_BM_DIR_PERIPH_TO_MEM      0x00000000UL
#define DMA_BM_DIR_MEM_TO_PERIPH      DMA_SxCR_DIR_0
#define DMA_BM_DIR_MEM_TO_MEM         DMA_SxCR_DIR_1

/* ---- Data Sizes ---------------------------------------------------------- */
#define DMA_BM_DATA_SIZE_BYTE         0x00000000UL
#define DMA_BM_DATA_SIZE_HALFWORD     0x00000001UL
#define DMA_BM_DATA_SIZE_WORD         0x00000002UL

/* ---- Priority Levels ----------------------------------------------------- */
#define DMA_BM_PRIORITY_LOW           0x00000000UL
#define DMA_BM_PRIORITY_MEDIUM        DMA_SxCR_PL_0
#define DMA_BM_PRIORITY_HIGH          DMA_SxCR_PL_1
#define DMA_BM_PRIORITY_VERY_HIGH     (DMA_SxCR_PL_0 | DMA_SxCR_PL_1)

/* ---- FIFO Thresholds (FCR FTH[1:0]) -------------------------------------- */
#define DMA_BM_FIFO_TH_1_4            0x00000000UL                        /*!< 1/4 full FIFO */
#define DMA_BM_FIFO_TH_1_2            DMA_SxFCR_FTH_0                     /*!< 1/2 full FIFO */
#define DMA_BM_FIFO_TH_3_4            DMA_SxFCR_FTH_1                     /*!< 3/4 full FIFO */
#define DMA_BM_FIFO_TH_FULL           (DMA_SxFCR_FTH_0 | DMA_SxFCR_FTH_1) /*!< Full FIFO     */

/* ---- Abstracted Stream Status / Interrupt Flags ------------------------- */
#define DMA_BM_FLAG_TC                (1UL << 0)  /*!< Transfer Complete         */
#define DMA_BM_FLAG_HT                (1UL << 1)  /*!< Half Transfer Complete    */
#define DMA_BM_FLAG_TE                (1UL << 2)  /*!< Transfer Error            */
#define DMA_BM_FLAG_DME               (1UL << 3)  /*!< Direct Mode Error         */
#define DMA_BM_FLAG_FE                (1UL << 4)  /*!< FIFO Error                */
#define DMA_BM_FLAG_ALL               (DMA_BM_FLAG_TC | DMA_BM_FLAG_HT | \
                                       DMA_BM_FLAG_TE | DMA_BM_FLAG_DME | \
                                       DMA_BM_FLAG_FE)

/* ---- Errata Workarounds -------------------------------------------------- */
#define DMA_BM_CR_TRBUFF              (1UL << 20) /*!< ES0445: Workaround for USART/LPUART DMA lock */

/**
  * @brief DMA Stream hardware configuration structure (Dependency Injection).
  */
typedef struct
{
    DMA_Stream_TypeDef *stream;            /*!< e.g. DMA1_Stream0, DMA2_Stream1    */
    uint32_t            request_id;        /*!< DMAMUX1 request ID (e.g. 9 for ADC1) */
    uint32_t            direction;         /*!< DMA_BM_DIR_*                       */
    uint32_t            periph_data_size;  /*!< DMA_BM_DATA_SIZE_*                 */
    uint32_t            mem_data_size;     /*!< DMA_BM_DATA_SIZE_*                 */
    bool                periph_inc;        /*!< Increment peripheral address       */
    bool                mem_inc;           /*!< Increment memory address           */
    bool                circular;          /*!< Circular mode                      */
    uint32_t            priority;          /*!< DMA_BM_PRIORITY_*                  */

    bool                enable_it_ht;      /*!< Half-transfer interrupt enable     */
    bool                enable_it_tc;      /*!< Transfer-complete interrupt enable */
    bool                enable_it_te;      /*!< Transfer-error interrupt enable    */
    bool                enable_it_dme;     /*!< Direct-mode error interrupt enable */
    bool                enable_it_fe;      /*!< FIFO error interrupt enable        */
    bool                enable_trbuff;     /*!< ES0445 TRBUFF workaround (bit 20)  */
    bool                enable_fifo;       /*!< Enable FIFO mode (mandatory in MEM2MEM) */
    uint32_t            fifo_threshold;    /*!< DMA_BM_FIFO_TH_*                   */

    uint32_t            periph_addr;       /*!< Peripheral register address        */
    uint32_t            mem_addr;          /*!< Memory buffer address              */
    uint32_t            buffer_length;     /*!< Transfer count in data units (NDTR)*/
} DmaBm_Config_t;

/**
  * @brief  Initializes and configures a DMA stream and its associated DMAMUX1 channel.
  * @param  config Pointer to hardware stream configuration structure.
  * @return true if initialized successfully, false on invalid parameters.
  */
bool DMA_BM_InitStream(const DmaBm_Config_t *config);

/**
  * @brief  Deinitializes a DMA stream and its associated DMAMUX1 channel.
  * @param  stream Pointer to DMA stream instance.
  * @return true on success, false on invalid parameters or stop failure.
  */
bool DMA_BM_DeinitStream(DMA_Stream_TypeDef *stream);

/**
  * @brief  Starts DMA stream transfer (enables stream).
  * @param  stream Pointer to DMA stream instance.
  */
void DMA_BM_Start(DMA_Stream_TypeDef *stream);

/**
  * @brief  Stops DMA stream transfer (disables stream and polls until idle).
  * @param  stream Pointer to DMA stream instance.
  * @return true if stream was successfully disabled, false on timeout or invalid param.
  */
bool DMA_BM_Stop(DMA_Stream_TypeDef *stream);

/**
  * @brief  Gets current remaining data items (NDTR).
  * @param  stream Pointer to DMA stream instance.
  * @return Remaining items to transfer.
  */
uint32_t DMA_BM_GetRemaining(const DMA_Stream_TypeDef *stream);

/**
  * @brief  Gets the abstracted active status flags for a stream.
  * @param  stream Pointer to DMA stream instance.
  * @return Bitmask combination of DMA_BM_FLAG_*
  */
uint32_t DMA_BM_GetFlags(const DMA_Stream_TypeDef *stream);

/**
  * @brief  Clears the specified status flags for a stream.
  * @param  stream Pointer to DMA stream instance.
  * @param  flags  Bitmask combination of DMA_BM_FLAG_* to clear.
  */
void DMA_BM_ClearFlags(DMA_Stream_TypeDef *stream, uint32_t flags);

/**
  * @brief  Resolves stream index (0..7) from stream pointer.
  * @param  stream Pointer to DMA stream instance.
  * @return Stream index (0..7), or 0xFFFFFFFFUL on error.
  */
uint32_t DMA_BM_GetStreamIndex(const DMA_Stream_TypeDef *stream);

/**
  * @brief  Resolves DMA controller (DMA1 or DMA2) from stream pointer.
  * @param  stream Pointer to DMA stream instance.
  * @return Pointer to DMA1 or DMA2, or NULL on error.
  */
DMA_TypeDef *DMA_BM_GetController(const DMA_Stream_TypeDef *stream);

#ifdef __cplusplus
}
#endif

#endif /* DMA_BM_H */
