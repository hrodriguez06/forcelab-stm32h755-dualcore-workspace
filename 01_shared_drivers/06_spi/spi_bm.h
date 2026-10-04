/**
  ******************************************************************************
  * @file    spi_bm.h
  * @brief   SPI1-SPI6 (SPI mode, master), bare-metal tier.
  *
  *          Driver tier: *_bm. Direct register access only: stm32h7xx.h,
  *          <stdint.h> and <stdbool.h>, no ST HAL and no LL headers (C8), no
  *          state in RAM (C7). Every silicon fact the driver needs (frame width,
  *          FIFO size, RCC enable bit, kernel clock field, interrupt number,
  *          DMAMUX requests) lives in one table of capabilities per instance in
  *          spi_bm.c, each row citing RM0399 rev 4 (C1).
  *
  *          Scope (06_spi plan, revision 2): master, Motorola frames (SP = 000),
  *          full duplex or transmit only, modes 0-3, frames of 4 to 32 bits (4 to
  *          16 on SPI4-6), MSB or LSB first, SS by software or by hardware, FIFO
  *          threshold, AFCNTR, transfers of 1 to 65 535 frames by polling, by
  *          interrupt (state in the caller's SpiBm_Xfer_t) and by DMA (streams
  *          programmed by the caller). Slave, CRC, TI, half duplex, receive only,
  *          TSER, endless transfers, suspension and I2S are not in the API.
  *
  *          Contract:
  *            - every parameter is checked before the first access, RCC
  *              included, in a fixed order: NULL, instance, capability of the
  *              instance, field range, state (ERR_BUSY) (C6);
  *            - a write the hardware would ignore is refused with ERR_BUSY and
  *              nothing written (configuration or TSIZE with SPE = 1); every
  *              configuration write is read back (ERR_VERIFY) (C12);
  *            - in RCC only this core's enable bank is written, never a reset
  *              register; the kernel clock fields are only read; flags are
  *              cleared with one IFCR write; the read-modify-writes of CR1, CFG1
  *              and IER run with PRIMASK saved and restored (C10);
  *            - every wait is bounded by a number of DWT cycles the caller
  *              passes; a dead CYCCNT is a timeout (C13);
  *            - one transaction per enable: SPE = 0 after every EOT (ES0445
  *              2.22.1), IER = 0 before SPE = 0 (2.22.3), CRCSIZE kept at 00111
  *              (2.22.4), a guard of guard_cycles between EOT and SPE = 0 (2.22.5);
  *            - no interrupt handler, no callback, no interrupt controller call:
  *              SPI_BM_GetIRQn only returns the number (C14).
  *
  *          Buffers: one element per frame, right aligned: uint8_t for 4..8-bit
  *          frames, uint16_t for 9..16, uint32_t for 17..32; the data register
  *          is accessed with the width of the element, so frames are never
  *          packed (RM0399 53:1158-1165: with frames of up to 8 bits, packing
  *          needs a 16- or 32-bit access). Bits above the frame size are ignored
  *          on transmission and read as 0.
  *
  *          Pins (AF, the pull of SCK at CPOL and of SS at its inactive level)
  *          and DMA streams are the caller's. Ownership: one core per instance;
  *          the driver takes no cross-core lock.
  ******************************************************************************
  */

#ifndef SPI_BM_H
#define SPI_BM_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Status ------------------------------------------------------------------- */
typedef enum
{
    SPI_BM_OK = 0,
    SPI_BM_ERR_PARAM,        /*!< refused before any peripheral access (C6)                  */
    SPI_BM_ERR_UNSUPPORTED,  /*!< the instance lacks this feature (24-bit frames on SPI4)     */
    SPI_BM_ERR_RANGE,        /*!< not reachable with this prescaler or FIFO                   */
    SPI_BM_ERR_BUSY,         /*!< the hardware would ignore the write in this state (C12)     */
    SPI_BM_ERR_VERIFY,       /*!< read-back differs from what was written (C12)               */
    SPI_BM_ERR_TIMEOUT,      /*!< bounded wait expired, or CYCCNT not running (C13)           */
    SPI_BM_ERR_MODF,         /*!< mode fault: SPE and MASTER cleared by hardware              */
    SPI_BM_ERR_OVERRUN,      /*!< OVR: received data lost                                     */
    SPI_BM_PENDING,          /*!< interrupt transfer still running (SpiBm_Xfer_t.status)       */
    SPI_BM_ERR_HAL           /*!< spi_hal only: a HAL call returned an error                  */
} SpiBm_Status_t;

/* ---- Capabilities (silicon facts, RM0399 table 435) ---------------------------- */
#define SPI_BM_KSEL_SPI123  0U   /* RCC_D2CCIP1R.SPI123SEL */
#define SPI_BM_KSEL_SPI45   1U   /* RCC_D2CCIP1R.SPI45SEL  */
#define SPI_BM_KSEL_SPI6    2U   /* RCC_D3CCIPR.SPI6SEL    */

typedef struct
{
    uint8_t  max_data_bits;    /*!< 32 (SPI1-3) or 16 (SPI4-6)                               */
    uint8_t  fifo_bytes;       /*!< 16 (SPI1-3) or 8 (SPI4-6)                                */
    uint8_t  kernel_select;    /*!< SPI_BM_KSEL_*                                            */
    uint8_t  dmamux;           /*!< 1 (DMA1/DMA2) or 2 (BDMA, SPI6)                          */
} SpiBm_Caps_t;

/** true only for the exact base address of SPI1-SPI6 (SPI1 + 4 is not an SPI). */
bool           SPI_BM_IsValidInstance(const SPI_TypeDef *spi);
SpiBm_Status_t SPI_BM_GetCaps(const SPI_TypeDef *spi, SpiBm_Caps_t *out);

/* ---- Clock -------------------------------------------------------------------- */
SpiBm_Status_t SPI_BM_EnableClock(SPI_TypeDef *spi);    /* this core's RCC bank, read back  */
SpiBm_Status_t SPI_BM_DisableClock(SPI_TypeDef *spi);   /* this core's bank only (§9.5.10)  */
SpiBm_Status_t SPI_BM_GetKernelSource(const SPI_TypeDef *spi, uint32_t *field); /* raw SPIxSEL, read only */

/* ---- Pure arithmetic (C15) ------------------------------------------------------ */
/** The smallest mbr 0..7 with kernel_hz / 2^(mbr+1) <= max_sck_hz (exact, 64-bit);
    *sck_hz = kernel_hz >> (mbr + 1). ERR_PARAM: a zero input or a NULL output;
    ERR_RANGE: not reachable with /256. */
SpiBm_Status_t SPI_BM_CalcPrescaler(uint32_t kernel_hz, uint32_t max_sck_hz,
                                    uint32_t *mbr, uint32_t *sck_hz);
/** One SCK period in CPU cycles, rounded up: ceil(cpu_hz * 2^(mbr+1) / kernel_hz).
    ERR_PARAM: cpu_hz or kernel_hz 0, mbr > 7 or NULL output; ERR_RANGE: > 32 bits. */
SpiBm_Status_t SPI_BM_CalcGuardCycles(uint32_t cpu_hz, uint32_t kernel_hz, uint32_t mbr,
                                      uint32_t *cycles);   /* one SCK period in CPU cycles, rounded up */

/* ---- Configuration -------------------------------------------------------------- */
#define SPI_BM_DIR_FULL_DUPLEX  0U   /* COMM = 00 */
#define SPI_BM_DIR_TX_ONLY      1U   /* COMM = 01 */
#define SPI_BM_NSS_SOFT         0U   /* SSM = 1, SSI = inactive; CS is the caller's GPIO         */
#define SPI_BM_NSS_HW           1U   /* SSM = 0, SSOE = 1, SSOM = 0: active from CSTART to EOT   */
#define SPI_BM_NSS_HW_PULSE     2U   /* SSM = 0, SSOE = 1, SSOM = 1: inactive between frames     */

typedef struct
{
    SPI_TypeDef *spi;
    uint32_t     mbr;            /*!< 0..7: SCK = kernel / 2^(mbr + 1)                         */
    uint32_t     mode;           /*!< 0..3 = CPOL << 1 | CPHA                                  */
    uint32_t     data_bits;      /*!< 4..caps.max_data_bits                                    */
    bool         lsb_first;      /*!< LSBFRST                                                  */
    uint32_t     direction;      /*!< SPI_BM_DIR_*                                             */
    uint32_t     fifo_threshold; /*!< frames per RXP/TXP event, 1..16; frames x bytes per frame
                                      (1, 2, 3 or 4) <= fifo_bytes / 2 (RM0399 53:3394)      */
    uint32_t     nss;            /*!< SPI_BM_NSS_*                                             */
    bool         nss_active_high;/*!< SSIOP                                                    */
    uint32_t     mssi;           /*!< 0..15 SCK periods, hardware NSS only (0 with NSS_SOFT)   */
    uint32_t     midi;           /*!< 0..15 SCK periods between frames                         */
    bool         keep_pins;      /*!< AFCNTR (C11)                                             */
} SpiBm_Config_t;

/** The checks of SPI_BM_Init, in the C6 order, nothing written. ERR_BUSY: SPE = 1,
    IOLOCK = 1, or AFCNTR = 1 in force with another CPOL or SSIOP (plan R2-1). */
SpiBm_Status_t SPI_BM_CheckConfig(const SpiBm_Config_t *cfg);   /* C6 order and states, no write */
/** Plan rule 3 with R2-1: IER = 0, IFCR, CFG2 = 0 if the CFG2 in force is a master
    with SSM = 1 and another SSIOP, CR1 = SSI inactive, CFG1, CFG2 (AFCNTR per C11),
    read-back of CFG1, CFG2, CR1 and SR.MODF = 0. */
SpiBm_Status_t SPI_BM_Init(const SpiBm_Config_t *cfg);          /* SPE = 0 required (ERR_BUSY)   */
/** Plan rule 9 with R2-2: IER = 0, SPE = 0 keeping SSI, CFG2 = 0 (pins released),
    CR1 = 0, CR2 = 0, CFG1 = 0x00070007, IFCR, read-back (MODF = 0, CRCPOLY, UDRDR
    and I2SCFGR at reset). No RCC write. */
SpiBm_Status_t SPI_BM_Deinit(SPI_TypeDef *spi);   /* reset values, pins released; no RCC write  */

/* ---- Blocking transfer -------------------------------------------------------- */
/* Buffer element per frame: uint8_t (4..8 bits), uint16_t (9..16), uint32_t (17..32),
   right-aligned; bits above data_bits are ignored on tx and zero on rx.                   */
/** ERR_PARAM: invalid instance, tx NULL, frames 0 or > 65 535, a buffer not aligned to
    its element, rx != NULL in TX_ONLY; ERR_MODF: a mode fault in force (SR.MODF; the
    calls that may write clear it, plan rule 7); ERR_BUSY: SPE = 1 or not a master (no
    Init). Reads CFG1, CFG2, CR1 and SR, writes nothing. */
SpiBm_Status_t SPI_BM_CheckTransfer(const SPI_TypeDef *spi, const void *tx, const void *rx,
                                    uint32_t frames);
/** One transaction (plan rule 4): TSIZE, SPE = 1, TxFIFO preload, CSTART; then EOT,
    the RxFIFO drained, guard_cycles CPU cycles after EOT (ES0445 2.22.5), IER = 0,
    SPE = 0, IFCR. guard_cycles >= 1 (SPI_BM_CalcGuardCycles), max_cycles >= 1 bounds
    the whole call. */
SpiBm_Status_t SPI_BM_Transfer(SPI_TypeDef *spi, const void *tx, void *rx, uint32_t frames,
                               uint32_t guard_cycles, uint32_t max_cycles);
                               /* rx may be NULL (discarded); NULL in TX_ONLY */

/* ---- Interrupt transfer (state in the caller's struct, C7) --------------------- */
typedef struct
{
    const void              *tx;
    void                    *rx;
    uint32_t                 frames;
    uint32_t                 guard_cycles; /*!< >= 1: CPU cycles between EOT and SPE = 0
                                                 (ES0445 2.22.5), applied in the ISR    */
    uint32_t                 tx_done;   /*!< driver-maintained                              */
    uint32_t                 rx_done;   /*!< driver-maintained                              */
    volatile SpiBm_Status_t  status;    /*!< SPI_BM_PENDING until the end                   */
} SpiBm_Xfer_t;

/** Checks like SPI_BM_CheckTransfer (and x != NULL, guard_cycles >= 1), then TSIZE,
    SPE = 1, TxFIFO preload, IER (after SPE = 1, ES0445 2.22.3), CSTART. */
SpiBm_Status_t SPI_BM_TransferStartIt(SPI_TypeDef *spi, SpiBm_Xfer_t *x);
bool           SPI_BM_TransferIrq(SPI_TypeDef *spi, SpiBm_Xfer_t *x);   /* from the project's ISR;
                                                                           true when x->status is final */
/* ---- DMA (streams and channels belong to the caller, the DMA driver) ----------- */
SpiBm_Status_t SPI_BM_DmaPrepare(SPI_TypeDef *spi, uint32_t frames, bool rx);   /* TSIZE, RXDMAEN */
SpiBm_Status_t SPI_BM_DmaStart(SPI_TypeDef *spi);                    /* TXDMAEN, SPE, CSTART     */
SpiBm_Status_t SPI_BM_WaitEnd(SPI_TypeDef *spi, uint32_t max_cycles);   /* EOT or error, no disable */
SpiBm_Status_t SPI_BM_DmaEnd(SPI_TypeDef *spi, uint32_t guard_cycles);
                                                  /* after the caller stopped its streams: guard,
                                                     SPE = 0, TXDMAEN = RXDMAEN = 0, flags        */
SpiBm_Status_t SPI_BM_GetDmaRequest(const SPI_TypeDef *spi, bool tx, uint32_t *request_id);
SpiBm_Status_t SPI_BM_GetIRQn(const SPI_TypeDef *spi, IRQn_Type *out);

/* ---- Errors --------------------------------------------------------------------- */
#define SPI_BM_ERRF_OVR    SPI_SR_OVR      /* bit positions of SR / IFCR */
#define SPI_BM_ERRF_MODF   SPI_SR_MODF
#define SPI_BM_ERRF_TIFRE  SPI_SR_TIFRE
uint32_t       SPI_BM_TakeErrors(SPI_TypeDef *spi);   /* IFCR = taken (one write), then DSB */

#ifdef __cplusplus
}
#endif

#endif /* SPI_BM_H */
