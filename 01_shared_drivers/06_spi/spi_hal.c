/**
  ******************************************************************************
  * @file    spi_hal.c
  * @brief   SPI1-SPI6, HAL tier. Contract in spi_hal.h.
  *
  *          Validation by spi_bm before any HAL call (C6), then the HAL call,
  *          then the read-back of what that call writes (C12). Line numbers of
  *          stm32h7xx_hal_spi.c below are those of the Cube the project builds
  *          with (STM32H7xx_HAL_Driver e3c518e).
  ******************************************************************************
  */

#include "spi_hal.h"

#if defined(HAL_SPI_MODULE_ENABLED)

/* Plan rule 10 (ES0445 2.22.4): with USE_SPI_CRC = 1, HAL_SPI_Init sets CRCSIZE to
   the frame size even with the CRC off; with 0 it keeps the CRCSIZE it finds
   (stm32h7xx_hal_spi.c l. 367), which SPI_BM_Deinit leaves at 00111. */
#if (USE_SPI_CRC != 0U)
#error "spi_hal needs USE_SPI_CRC = 0 in stm32h7xx_hal_conf.h (06_spi plan rule 10, ES0445 2.22.4)"
#endif

/* C8: no <stddef.h>. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* The HAL constants are the register fields: DataSize = DSIZE (bits - 1), FifoThreshold
   = FTHLV << 5, BaudRatePrescaler = MBR << 28, MasterSSIdleness = MSSI, MasterInterData-
   Idleness = MIDI << 4 (stm32h7xx_hal_spi.h). Checked here at build time. */
_Static_assert((SPI_DATASIZE_4BIT == 3U) && (SPI_DATASIZE_32BIT == 31U), "SPI_DATASIZE_x is not bits - 1");
_Static_assert((SPI_FIFO_THRESHOLD_01DATA == 0U) && (SPI_FIFO_THRESHOLD_16DATA == (15UL << SPI_CFG1_FTHLV_Pos)),
               "SPI_FIFO_THRESHOLD_x is not FTHLV");
_Static_assert((SPI_BAUDRATEPRESCALER_2 == 0U) && (SPI_BAUDRATEPRESCALER_256 == (7UL << SPI_CFG1_MBR_Pos)),
               "SPI_BAUDRATEPRESCALER_x is not MBR");
_Static_assert((SPI_MASTER_SS_IDLENESS_15CYCLE == (15UL << SPI_CFG2_MSSI_Pos)) &&
               (SPI_MASTER_INTERDATA_IDLENESS_15CYCLE == (15UL << SPI_CFG2_MIDI_Pos)), "MSSI/MIDI constants");

#define CFG1_CRCSIZE_8  (7UL << SPI_CFG1_CRCSIZE_Pos)       /* what SPI_BM_Deinit leaves (53:3309) */
#define DMA_REQS        (SPI_CFG1_TXDMAEN | SPI_CFG1_RXDMAEN)

/* ---- spi_bm configuration -> h->Init ------------------------------------------- */
static void fill_init(SPI_InitTypeDef *in, const SpiBm_Config_t *cfg)
{
    in->Mode                       = SPI_MODE_MASTER;
    in->Direction                  = (cfg->direction == SPI_BM_DIR_TX_ONLY) ? SPI_DIRECTION_2LINES_TXONLY :
                                     SPI_DIRECTION_2LINES;
    in->DataSize                   = cfg->data_bits - 1U;
    in->CLKPolarity                = ((cfg->mode & 2U) != 0U) ? SPI_POLARITY_HIGH : SPI_POLARITY_LOW;
    in->CLKPhase                   = ((cfg->mode & 1U) != 0U) ? SPI_PHASE_2EDGE : SPI_PHASE_1EDGE;
    in->NSS                        = (cfg->nss == SPI_BM_NSS_SOFT) ? SPI_NSS_SOFT : SPI_NSS_HARD_OUTPUT;
    in->BaudRatePrescaler          = cfg->mbr << SPI_CFG1_MBR_Pos;
    in->FirstBit                   = cfg->lsb_first ? SPI_FIRSTBIT_LSB : SPI_FIRSTBIT_MSB;
    in->TIMode                     = SPI_TIMODE_DISABLE;
    in->CRCCalculation             = SPI_CRCCALCULATION_DISABLE;
    in->CRCPolynomial              = 0x7U;
    in->CRCLength                  = SPI_CRC_LENGTH_DATASIZE;
    in->NSSPMode                   = (cfg->nss == SPI_BM_NSS_HW_PULSE) ? SPI_NSS_PULSE_ENABLE : SPI_NSS_PULSE_DISABLE;
    in->NSSPolarity                = cfg->nss_active_high ? SPI_NSS_POLARITY_HIGH : SPI_NSS_POLARITY_LOW;
    in->FifoThreshold              = (cfg->fifo_threshold - 1U) << SPI_CFG1_FTHLV_Pos;
    in->TxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
    in->RxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
    in->MasterSSIdleness           = cfg->mssi << SPI_CFG2_MSSI_Pos;
    in->MasterInterDataIdleness    = cfg->midi << SPI_CFG2_MIDI_Pos;
    in->MasterReceiverAutoSusp     = SPI_MASTER_RX_AUTOSUSP_DISABLE;
    in->MasterKeepIOState          = cfg->keep_pins ? SPI_MASTER_KEEP_IO_STATE_ENABLE : SPI_MASTER_KEEP_IO_STATE_DISABLE;
    in->IOSwap                     = SPI_IO_SWAP_DISABLE;
}

/* A HAL status, with h->ErrorCode, as SpiBm_Status_t. */
static SpiBm_Status_t from_hal(HAL_StatusTypeDef hs, const SPI_HandleTypeDef *h)
{
    if (hs == HAL_OK)
    {
        return SPI_BM_OK;
    }
    if (hs == HAL_BUSY)
    {
        return SPI_BM_ERR_BUSY;
    }
    if ((hs == HAL_TIMEOUT) || ((h->ErrorCode & HAL_SPI_ERROR_TIMEOUT) != 0U))
    {
        return SPI_BM_ERR_TIMEOUT;
    }
    if ((h->ErrorCode & HAL_SPI_ERROR_MODF) != 0U)
    {
        return SPI_BM_ERR_MODF;
    }
    if ((h->ErrorCode & HAL_SPI_ERROR_OVR) != 0U)
    {
        return SPI_BM_ERR_OVERRUN;
    }
    return SPI_BM_ERR_HAL;
}

/* ---- Init / Deinit ----------------------------------------------------------------- */

SpiBm_Status_t SPI_HAL_Init(SPI_HandleTypeDef *h, const SpiBm_Config_t *cfg)
{
    if ((h == NULL) || (cfg == NULL) || !SPI_BM_IsValidInstance(h->Instance) || (h->Instance != cfg->spi))
    {
        return SPI_BM_ERR_PARAM;
    }
    const SpiBm_Status_t st = SPI_BM_CheckConfig(cfg);
    if (st != SPI_BM_OK)
    {
        return st;
    }
    SPI_TypeDef *spi = h->Instance;
    /* From the reset image the HAL's own order is R2-1 (SSI, l. 375-381, before CFG2,
       l. 398); the handle back to RESET so HAL_SPI_Init runs its whole sequence. */
    const SpiBm_Status_t sd = SPI_BM_Deinit(spi);
    if (sd != SPI_BM_OK)
    {
        return sd;
    }
    h->State = HAL_SPI_STATE_RESET;
    fill_init(&h->Init, cfg);
    const HAL_StatusTypeDef hs = HAL_SPI_Init(h);
    if (hs != HAL_OK)
    {
        return from_hal(hs, h);
    }
    /* Read-back of what HAL_SPI_Init wrote (l. 375-402, 467, 474): CR1 SSI only for a
       soft SS active low, MASRX = 0, SPE = 0; CFG1 with the CRCSIZE it found; CFG2,
       then AFCNTR. */
    const SPI_InitTypeDef *in = &h->Init;
    const uint32_t cfg1 = in->BaudRatePrescaler | in->CRCCalculation | CFG1_CRCSIZE_8 | in->FifoThreshold |
                          in->DataSize;
    const uint32_t cfg2 = in->NSSPMode | in->TIMode | in->NSSPolarity | in->NSS | in->CLKPolarity | in->CLKPhase |
                          in->FirstBit | in->Mode | in->MasterInterDataIdleness | in->Direction |
                          in->MasterSSIdleness | in->IOSwap | in->MasterKeepIOState;
    const uint32_t cr1 = ((in->NSS == SPI_NSS_SOFT) && (in->NSSPolarity == SPI_NSS_POLARITY_LOW)) ? SPI_CR1_SSI : 0U;
    if ((spi->SR & SPI_SR_MODF) != 0U)
    {
        spi->IFCR = SPI_IFCR_MODFC;
        return SPI_BM_ERR_MODF;
    }
    if ((spi->CFG1 != cfg1) || (spi->CFG2 != cfg2) || (spi->CR1 != cr1) || (h->State != HAL_SPI_STATE_READY))
    {
        return SPI_BM_ERR_VERIFY;
    }
    return SPI_BM_OK;
}

SpiBm_Status_t SPI_HAL_Deinit(SPI_HandleTypeDef *h)
{
    if ((h == NULL) || !SPI_BM_IsValidInstance(h->Instance))
    {
        return SPI_BM_ERR_PARAM;
    }
    const SpiBm_Status_t st = SPI_BM_Deinit(h->Instance);   /* not HAL_SPI_DeInit: no MSP (C14) */
    h->ErrorCode = HAL_SPI_ERROR_NONE;
    h->State     = HAL_SPI_STATE_RESET;
    h->Lock      = HAL_UNLOCKED;
    return st;
}

/* ---- Transfers ----------------------------------------------------------------------- */

/* The checks shared by the three starts (C6): handle, instance, then spi_bm's own
   checks of the transfer, then the handle's state. */
static SpiBm_Status_t chk_xfer(const SPI_HandleTypeDef *h, const void *tx, const void *rx, uint32_t frames)
{
    if ((h == NULL) || !SPI_BM_IsValidInstance(h->Instance))
    {
        return SPI_BM_ERR_PARAM;
    }
    const SpiBm_Status_t st = SPI_BM_CheckTransfer(h->Instance, tx, rx, frames);
    if (st != SPI_BM_OK)
    {
        return st;
    }
    return (h->State == HAL_SPI_STATE_READY) ? SPI_BM_OK : SPI_BM_ERR_BUSY;
}

/* CFG2.COMM as the HAL call leaves it: SPI_2LINES (TransmitReceive*, l. 1398, 1918,
   2441) or SPI_2LINES_TX (Transmit*, l. 872, 1730, 2153). */
static uint32_t comm_of(const void *rx)
{
    return (rx != NULL) ? 0U : SPI_CFG2_COMM_0;
}

/* After the HAL closed a transfer (SPI_CloseTransfer, l. 3937): SPE = 0, the DMA
   requests off, and IER clear except TSERFIE: the IT starts enable it (l. 1743, 1837,
   1964) and the close's mask (l. 3946) leaves it out. TSERF is set only by a TSER
   reload, which this tier never programs; SPI_HAL_Deinit clears it. */
static bool closed(const SPI_TypeDef *spi)
{
    return ((spi->CR1 & SPI_CR1_SPE) == 0U) && ((spi->IER & ~SPI_IER_TSERFIE_Msk) == 0U) &&
           ((spi->CFG1 & DMA_REQS) == 0U);
}

SpiBm_Status_t SPI_HAL_Transfer(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames,
                                uint32_t timeout_ms)
{
    SpiBm_Status_t st = (timeout_ms == 0U) ? SPI_BM_ERR_PARAM : chk_xfer(h, tx, rx, frames);
    if (st != SPI_BM_OK)
    {
        return st;
    }
    HAL_StatusTypeDef hs;
    if (rx != NULL)
    {
        hs = HAL_SPI_TransmitReceive(h, (const uint8_t *)tx, (uint8_t *)rx, (uint16_t)frames, timeout_ms);
    }
    else
    {
        hs = HAL_SPI_Transmit(h, (const uint8_t *)tx, (uint16_t)frames, timeout_ms);
    }
    st = from_hal(hs, h);
    if (st != SPI_BM_OK)
    {
        return st;
    }
    const SPI_TypeDef *spi = h->Instance;
    if (!closed(spi) || ((spi->CR2 & SPI_CR2_TSIZE_Msk) != frames) ||
        ((spi->CFG2 & SPI_CFG2_COMM_Msk) != comm_of(rx)) || (h->State != HAL_SPI_STATE_READY))
    {
        return SPI_BM_ERR_VERIFY;
    }
    return SPI_BM_OK;
}

SpiBm_Status_t SPI_HAL_StartTransferIt(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames)
{
    SpiBm_Status_t st = chk_xfer(h, tx, rx, frames);
    if (st != SPI_BM_OK)
    {
        return st;
    }
    HAL_StatusTypeDef hs;
    if (rx != NULL)
    {
        hs = HAL_SPI_TransmitReceive_IT(h, (const uint8_t *)tx, (uint8_t *)rx, (uint16_t)frames);
    }
    else
    {
        hs = HAL_SPI_Transmit_IT(h, (const uint8_t *)tx, (uint16_t)frames);
    }
    st = from_hal(hs, h);
    if (st != SPI_BM_OK)
    {
        return st;
    }
    const SPI_TypeDef *spi = h->Instance;
    return (((spi->CR2 & SPI_CR2_TSIZE_Msk) == frames) && ((spi->CFG2 & SPI_CFG2_COMM_Msk) == comm_of(rx))) ?
           SPI_BM_OK : SPI_BM_ERR_VERIFY;
}

/* The HAL refuses a DMA memory size narrower than the frame (l. 2157, 2304, 2446) after
   it has set h->State busy, and leaves it so: refused here first (C6). */
static bool dma_fits(const SPI_HandleTypeDef *h, const DMA_HandleTypeDef *d)
{
    const uint32_t m = d->Init.MemDataAlignment;
    if (h->Init.DataSize > SPI_DATASIZE_16BIT)
    {
        return m == DMA_MDATAALIGN_WORD;
    }
    if (h->Init.DataSize > SPI_DATASIZE_8BIT)
    {
        return (m == DMA_MDATAALIGN_HALFWORD) || (m == DMA_MDATAALIGN_WORD);
    }
    return true;
}

SpiBm_Status_t SPI_HAL_StartTransferDma(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames)
{
    if ((h != NULL) && ((h->hdmatx == NULL) || ((rx != NULL) && (h->hdmarx == NULL)) || !dma_fits(h, h->hdmatx) ||
                        ((rx != NULL) && !dma_fits(h, h->hdmarx))))
    {
        return SPI_BM_ERR_PARAM;            /* the caller links its DMA handles, sized for the frame */
    }
    SpiBm_Status_t st = chk_xfer(h, tx, rx, frames);
    if (st != SPI_BM_OK)
    {
        return st;
    }
    HAL_StatusTypeDef hs;
    if (rx != NULL)
    {
        hs = HAL_SPI_TransmitReceive_DMA(h, (const uint8_t *)tx, (uint8_t *)rx, (uint16_t)frames);
    }
    else
    {
        hs = HAL_SPI_Transmit_DMA(h, (const uint8_t *)tx, (uint16_t)frames);
    }
    st = from_hal(hs, h);
    if (st != SPI_BM_OK)
    {
        return st;
    }
    const SPI_TypeDef *spi = h->Instance;
    return (((spi->CR2 & SPI_CR2_TSIZE_Msk) == frames) && ((spi->CFG2 & SPI_CFG2_COMM_Msk) == comm_of(rx))) ?
           SPI_BM_OK : SPI_BM_ERR_VERIFY;
}

SpiBm_Status_t SPI_HAL_Wait(SPI_HandleTypeDef *h, uint32_t timeout_ms)
{
    if ((h == NULL) || !SPI_BM_IsValidInstance(h->Instance) || (timeout_ms == 0U))
    {
        return SPI_BM_ERR_PARAM;
    }
    const uint32_t t0 = HAL_GetTick();
    while (h->State != HAL_SPI_STATE_READY)
    {
        if ((HAL_GetTick() - t0) >= timeout_ms)
        {
            return SPI_BM_ERR_TIMEOUT;
        }
    }
    if (h->ErrorCode != HAL_SPI_ERROR_NONE)
    {
        return from_hal(HAL_ERROR, h);
    }
    return closed(h->Instance) ? SPI_BM_OK : SPI_BM_ERR_VERIFY;
}

#endif /* HAL_SPI_MODULE_ENABLED */
