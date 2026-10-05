/**
  ******************************************************************************
  * @file    spi_ll.h
  * @brief   SPI1-SPI6 (SPI mode, master), LL tier: the operations of spi_bm
  *          with the constants of the ST LL header (LL_SPI_BAUDRATEPRESCALER_*,
  *          LL_SPI_POLARITY_*, LL_SPI_PHASE_*, LL_SPI_DATAWIDTH_*, ...).
  *
  *          Driver tier: *_ll. Adds only the static inline functions and macros
  *          of stm32h7xx_ll_spi.h to spi_bm (C8): no USE_FULL_LL_DRIVER, no
  *          LL_SPI_Init, no state in RAM (C7).
  *
  *          Contract (06_spi plan, revision 3, "Contrato de API: spi_ll"):
  *            - every LL constant is translated to the spi_bm value and the call
  *              is validated by spi_bm (SPI_BM_Check*): one copy of the checks and
  *              of the capability table. An LL constant this API does not know
  *              becomes an out-of-range spi_bm value, so it is refused with
  *              SPI_BM_ERR_PARAM at the place the C6 order gives a field error;
  *            - the registers are written with the LL inlines, and with
  *              LL_SPI_WriteReg where no inline writes the whole register (CR1,
  *              CFG1, CFG2, IER, IFCR), in the order of the spi_bm rules 3-9 (R2-1,
  *              R2-2); read-modify-writes of CR1, CFG1 and IER under PRIMASK saved
  *              and restored (C10); every configuration write read back (C12).
  *              IFCR is always one LL_SPI_WriteReg: the LL_SPI_ClearFlag_* inlines
  *              are SET_BIT, a read-modify-write of a write-only register;
  *            - for the same configuration the register image is the one spi_bm
  *              leaves, bit for bit (T2);
  *            - guard_cycles is passed per call, as in spi_bm (plan R3-1);
  *            - the status is SpiBm_Status_t.
  *
  *          Clock, Deinit, TakeErrors, GetIRQn, GetDmaRequest, GetCaps,
  *          GetKernelSource, CheckTransfer and the pure functions take no LL
  *          constant: use SPI_BM_* for them (plan "Contrato de API: spi_ll").
  *
  *          As spi_bm: the SCK pin must be in its AF for a transfer to end (plan
  *          R3-3). Pins and DMA streams are the caller's.
  ******************************************************************************
  */

#ifndef SPI_LL_H
#define SPI_LL_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"
#include "stm32h7xx_ll_spi.h"
#include "spi_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Configuration ------------------------------------------------------------- */
typedef struct
{
    SPI_TypeDef *spi;
    uint32_t     baudrate;         /*!< LL_SPI_BAUDRATEPRESCALER_DIV2 .. _DIV256 (MBR)          */
    uint32_t     clock_polarity;   /*!< LL_SPI_POLARITY_LOW / _HIGH (CPOL)                      */
    uint32_t     clock_phase;      /*!< LL_SPI_PHASE_1EDGE / _2EDGE (CPHA)                      */
    uint32_t     data_width;       /*!< LL_SPI_DATAWIDTH_4BIT .. _32BIT (16 on SPI4-6) (DSIZE)  */
    uint32_t     bit_order;        /*!< LL_SPI_MSB_FIRST / LL_SPI_LSB_FIRST (LSBFRST)           */
    uint32_t     transfer_direction; /*!< LL_SPI_FULL_DUPLEX / LL_SPI_SIMPLEX_TX (COMM)         */
    uint32_t     fifo_threshold;   /*!< LL_SPI_FIFO_TH_01DATA .. _16DATA (FTHLV), at most half
                                        of the FIFO in bytes                                    */
    uint32_t     nss;              /*!< LL_SPI_NSS_SOFT (SSM; CS is the caller's GPIO) or
                                        LL_SPI_NSS_HARD_OUTPUT (SSOE)                          */
    bool         nss_pulse;        /*!< SSOM: SS inactive between frames; NSS_HARD_OUTPUT only */
    uint32_t     nss_polarity;     /*!< LL_SPI_NSS_POLARITY_LOW / _HIGH (SSIOP)                 */
    uint32_t     ss_idleness;      /*!< LL_SPI_SS_IDLENESS_00CYCLE .. _15CYCLE (MSSI);
                                        00CYCLE with LL_SPI_NSS_SOFT                            */
    uint32_t     interdata_idleness; /*!< LL_SPI_ID_IDLENESS_00CYCLE .. _15CYCLE (MIDI)         */
    bool         keep_pins;        /*!< AFCNTR: the SPI keeps its pins at their idle level
                                        while disabled (C11)                                    */
} SpiLl_Config_t;

/** SPI_BM_CheckConfig of the translation: the checks of SPI_LL_Init, nothing written. */
SpiBm_Status_t SPI_LL_CheckConfig(const SpiLl_Config_t *cfg);
/** As SPI_BM_Init (rule 3, R2-1), validated by SPI_BM_CheckConfig. */
SpiBm_Status_t SPI_LL_Init(const SpiLl_Config_t *cfg);

/* ---- Transfers (as spi_bm; validated by SPI_BM_CheckTransfer) ------------------ */
SpiBm_Status_t SPI_LL_Transfer(SPI_TypeDef *spi, const void *tx, void *rx, uint32_t frames,
                               uint32_t guard_cycles, uint32_t max_cycles);
SpiBm_Status_t SPI_LL_TransferStartIt(SPI_TypeDef *spi, SpiBm_Xfer_t *x);
bool           SPI_LL_TransferIrq(SPI_TypeDef *spi, SpiBm_Xfer_t *x);   /* from the project's ISR;
                                                                           true when x->status is final */

/* ---- DMA (as spi_bm; validated by SPI_BM_CheckDmaPrepare / _CheckDmaStart) ----- */
SpiBm_Status_t SPI_LL_DmaPrepare(SPI_TypeDef *spi, uint32_t frames, bool rx);   /* TSIZE, RXDMAEN */
SpiBm_Status_t SPI_LL_DmaStart(SPI_TypeDef *spi);                    /* TXDMAEN, SPE, CSTART     */
SpiBm_Status_t SPI_LL_WaitEnd(SPI_TypeDef *spi, uint32_t max_cycles);   /* EOT or error, no disable */
SpiBm_Status_t SPI_LL_DmaEnd(SPI_TypeDef *spi, uint32_t guard_cycles);  /* guard, SPE = 0, DMA bits */

#ifdef __cplusplus
}
#endif

#endif /* SPI_LL_H */
