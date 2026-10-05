/**
  ******************************************************************************
  * @file    spi_hal.h
  * @brief   SPI1-SPI6 (SPI mode, master), HAL tier: a thin wrapper of the ST
  *          HAL for code that already uses SPI_HandleTypeDef.
  *
  *          Driver tier: *_hal. Needs HAL_SPI_MODULE_ENABLED in the project's
  *          stm32h7xx_hal_conf.h; without it this header declares nothing and
  *          spi_hal.c compiles to nothing (C8).
  *
  *          Contract (06_spi plan, revision 3, "Contrato de API: spi_hal"):
  *            - every call is validated with spi_bm before the HAL is called
  *              (C6): h == NULL, an instance that is not an SPI, or
  *              h->Instance != cfg->spi is ERR_PARAM; the rest is
  *              SPI_BM_CheckConfig / SPI_BM_CheckTransfer;
  *            - what each HAL call writes is read back (C12): a HAL error is
  *              ERR_HAL (ERR_TIMEOUT, ERR_BUSY, ERR_MODF or ERR_OVERRUN when the
  *              HAL status or h->ErrorCode says so), HAL_OK with a read-back that
  *              differs is ERR_VERIFY;
  *            - no callback and no MSP (C14). HAL_SPI_Init calls the project's
  *              MSP while h->State is RESET; for IT and DMA the project's SPI
  *              interrupt handler calls the HAL's SPI interrupt routine with h, and
  *              its DMA handlers the HAL's DMA one, and SPI_HAL_Wait polls h->State and
  *              h->ErrorCode. For DMA the caller links h->hdmatx / h->hdmarx
  *              (DMA_HAL_Init of 02_dma);
  *            - SPI_HAL_Init calls SPI_BM_Deinit first, then HAL_SPI_Init: the HAL
  *              then always starts from the reset image, where its own order is
  *              the one of plan rule 3 with R2-1 (SSI before CFG2), and finds
  *              CRCSIZE = 00111 (ES0445 2.22.4), which it keeps with
  *              USE_SPI_CRC = 0 (owner's decision, 2026-10-04);
  *            - SPI_HAL_Deinit calls SPI_BM_Deinit and leaves the handle in RESET;
  *              it never calls HAL_SPI_DeInit (that would call the MSP).
  *
  *          Limitations of the HAL, not of this wrapper (README):
  *            - ES0445 2.22.5: SPI_CloseTransfer clears SPE right after EOT, with
  *              no guard: with CPHA = 0 and a slow SCK the last SCK pulse can be
  *              short. There is no guard_cycles in this tier (plan decision 5);
  *              use spi_bm or spi_ll for that case;
  *            - keep_pins (AFCNTR): HAL_SPI_Init writes CFG2, then AFCNTR, so an
  *              Init releases the pins for a few cycles, as the Deinit before it
  *              does;
  *            - with hardware SS the HAL leaves CR1.SSI at 0, where spi_bm writes
  *              it inactive: SSI does not act with SSM = 0 (T2, a difference by
  *              design);
  *            - ES0445 2.22.3 in the HAL's interrupt path: HAL_SPI_TransmitReceive_IT
  *              fills the FIFO before it sets DXPIE; when every frame fits, TXTF
  *              came first and DXPIE stays set (RM0399 53.11.5), and the HAL's
  *              close clears SPE before IER. One SPI interrupt then follows the
  *              close, with h->State READY: the project's handler must tolerate
  *              it (owner's decision, 2026-10-04: the HAL is kept as it is);
  *            - after an interrupt transfer the HAL leaves IER.TSERFIE at 1 (its
  *              close does not clear it); TSERF needs a TSER reload, which this
  *              tier never programs, and SPI_HAL_Deinit clears it;
  *            - with rx == NULL a full-duplex configuration goes through
  *              HAL_SPI_Transmit, which sets CFG2.COMM to transmit only
  *              (SPI_2LINES_TX); the next transfer with rx sets it back.
  *
  *          Clock, TakeErrors, GetIRQn, GetDmaRequest and the pure functions are
  *          spi_bm's. The SCK pin must be in its AF for a transfer to end (R3-3).
  ******************************************************************************
  */

#ifndef SPI_HAL_H
#define SPI_HAL_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx_hal.h"
#include "spi_bm.h"

#if defined(HAL_SPI_MODULE_ENABLED)

#ifdef __cplusplus
extern "C" {
#endif

/** h->Instance must be cfg->spi. Validated by SPI_BM_CheckConfig; then SPI_BM_Deinit,
    h->Init filled from cfg, HAL_SPI_Init, read-back of CR1, CFG1, CFG2 and SR.MODF. */
SpiBm_Status_t SPI_HAL_Init(SPI_HandleTypeDef *h, const SpiBm_Config_t *cfg);
/** HAL_SPI_TransmitReceive (rx != NULL) or HAL_SPI_Transmit; frames 1..65 535, the
    element per frame of spi_bm; timeout_ms >= 1. Read-back: SPE = 0, IER = 0 but
    TSERFIE, DMA requests off, TSIZE, COMM. */
SpiBm_Status_t SPI_HAL_Transfer(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames,
                                uint32_t timeout_ms);
/** HAL_SPI_TransmitReceive_IT or HAL_SPI_Transmit_IT; read-back of TSIZE and COMM. */
SpiBm_Status_t SPI_HAL_StartTransferIt(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames);
/** HAL_SPI_TransmitReceive_DMA or HAL_SPI_Transmit_DMA; h->hdmatx (and h->hdmarx with
    rx) linked by the caller, with a memory size of at least a half-word for frames of
    9..16 bits and a word above, else ERR_PARAM (the HAL's own refusal would leave
    h->State busy); read-back of TSIZE and COMM. */
SpiBm_Status_t SPI_HAL_StartTransferDma(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames);
/** Polls h->State until READY, at most timeout_ms (>= 1) of HAL_GetTick; then
    h->ErrorCode, and the read-back of the close (SPE = 0, IER = 0 but TSERFIE, DMA
    requests off).
    ERR_TIMEOUT leaves the transfer as it is. */
SpiBm_Status_t SPI_HAL_Wait(SPI_HandleTypeDef *h, uint32_t timeout_ms);
/** SPI_BM_Deinit(h->Instance); the handle back to RESET (State, ErrorCode, Lock). */
SpiBm_Status_t SPI_HAL_Deinit(SPI_HandleTypeDef *h);

#ifdef __cplusplus
}
#endif

#endif /* HAL_SPI_MODULE_ENABLED */

#endif /* SPI_HAL_H */
