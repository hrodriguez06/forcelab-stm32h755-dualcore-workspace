/**
  ******************************************************************************
  * @file    i2c_hal.h
  * @brief   I2C1-I2C4 (master, 7-bit addresses), HAL tier: a thin wrapper of
  *          the ST HAL for code that already keeps an I2C_HandleTypeDef.
  *
  *          Driver tier: *_hal. Needs HAL_I2C_MODULE_ENABLED in the project's
  *          stm32h7xx_hal_conf.h, and USE_HAL_I2C_REGISTER_CALLBACKS = 0 (else
  *          #error); without the module this header declares nothing and
  *          i2c_hal.c compiles to nothing (C8).
  *
  *          Contract (05_i2c plan, revision 4, "Contrato de API: i2c_hal"):
  *            - every call is validated by i2c_bm before the HAL is called (C6):
  *              h == NULL, an instance that is not an I2C, or h->Instance !=
  *              cfg->i2c is ERR_PARAM; then I2C_BM_CheckConfig or the
  *              I2C_BM_Check* of the transfer (one copy of the rules); then what
  *              this tier cannot do (ERR_UNSUPPORTED); then the handle's state
  *              (a transfer needs READY, else ERR_BUSY);
  *            - what the HAL call writes is read back (C12): a HAL error is
  *              ERR_TIMEOUT, ERR_ARLO, ERR_NACK or ERR_BUSY when the HAL status
  *              or h->ErrorCode says so, else ERR_HAL; HAL_OK with a read-back
  *              that differs is ERR_VERIFY;
  *            - no callback and no MSP (C14). HAL_I2C_Init calls the project's
  *              HAL_I2C_MspInit while h->State is RESET. For IT and DMA, the
  *              project's I2Cx_EV/ER handlers call HAL_I2C_EV_IRQHandler /
  *              HAL_I2C_ER_IRQHandler with h, its DMA handlers HAL_DMA_IRQHandler,
  *              and I2C_HAL_Wait polls h->State and h->ErrorCode. For DMA the
  *              caller links h->hdmatx / h->hdmarx (DMA_HAL_Init of 02_dma);
  *            - I2C_HAL_Init calls I2C_BM_Deinit first and puts the handle to
  *              RESET, then HAL_I2C_Init and HAL_I2CEx_ConfigAnalogFilter /
  *              ConfigDigitalFilter;
  *            - I2C_HAL_Deinit calls I2C_BM_Deinit and leaves the handle in RESET;
  *              it never calls HAL_I2C_DeInit (that would call the MSP).
  *
  *          Limitations of the HAL (STM32H7xx HAL V1.11.6), not repaired by this
  *          tier; line numbers of stm32h7xx_hal_i2c.c (and _ex.c where said):
  *            - IC1, ES0445 2.19.2: a BERR in master mode ends the transfer with
  *              HAL_I2C_ERROR_BERR (blocking: I2C_IsErrorOccurred l. 7235-7243;
  *              IT and DMA: HAL_I2C_ER_IRQHandler l. 4671-4678, 4704-4706, then
  *              I2C_ITError l. 6548, which stops serving the transfer without a
  *              STOP). i2c_bm and i2c_ll clear it and go on. Here: ERR_HAL; call
  *              I2C_HAL_Deinit before the next transfer;
  *            - TIMEOUTR: HAL_I2C_Init (l. 535-642) never writes it, and neither
  *              I2C_IsErrorOccurred nor HAL_I2C_ER_IRQHandler looks at the TIMEOUT
  *              flag. cfg->timeout_scl_low != 0 is refused (ERR_UNSUPPORTED; the
  *              owner's decision, 2026-10-09); a held SCL ends a HAL transfer only
  *              by timeout_ms (no TIMEOUT, plan R3-5 does not apply);
  *            - a HAL timeout leaves the peripheral as it is: no STOP, no software
  *              reset (I2C_WaitOnFlagUntilTimeout l. 6984-6992). ERR_TIMEOUT: call
  *              I2C_HAL_Deinit (i2c_bm resets the instance itself);
  *            - ES0445 2.19.6: the first byte goes to TXDR before START in
  *              HAL_I2C_Master_Transmit (l. 1162), _IT (l. 1737), _DMA (l. 2012)
  *              and HAL_I2C_Mem_Read_IT (l. 2922); not in the blocking
  *              HAL_I2C_Mem_Read, whose register address is written after START,
  *              on TXIS (I2C_RequestMemoryRead l. 5871, 5874-5883). The erratum
  *              needs APB/kernel between 1.5 and 3;
  *            - repeated start only through HAL_I2C_Mem_Read / _Mem_Read_IT, with a
  *              register address of 1 or 2 bytes (MemAddSize): ntx > 2 is refused
  *              (ERR_UNSUPPORTED; the owner's decision, 2026-10-09). A write-read
  *              by DMA is not offered;
  *            - more than 255 bytes: the HAL reloads NBYTES by 255 (MAX_NBYTE_SIZE
  *              l. 353; l. 1147-1218, 1289-1338, 2706-2756; by DMA, in its
  *              completions l. 6747-6779, 6827-6859), as i2c_bm does;
  *            - the image of HAL_I2C_Init differs from i2c_bm's by design (T2):
  *              OAR1 = OA1EN | OwnAddress1 (0) (l. 600), CR2 |= AUTOEND | NACK
  *              (l. 619). The filters are a CR1 read-modify-write with PE dropped
  *              and raised in it, not under PRIMASK, without reading PE = 0 back
  *              (stm32h7xx_hal_i2c_ex.c l. 110-118, 156-170);
  *            - the interrupt enables are written after START (l. 1745 -> 1765,
  *              2937 -> 2950), and TXDMAEN / RXDMAEN after START (l. 2059 -> 2075,
  *              2207 -> 2222); i2c_bm writes them before (plan rules 7, 8);
  *            - a blocking call waits up to 25 ms for BUSY = 0 (I2C_TIMEOUT_BUSY,
  *              l. 344, 1133): never reached here, BUSY = 1 is ERR_BUS first
  *              (I2C_BM_CheckTransfer);
  *            - every wait inside the HAL runs on HAL_GetTick(): the blocking
  *              calls have no exit if the tick stops; I2C_HAL_Wait has one.
  *
  *          Clock, TakeErrors, GetIRQn, GetDmaRequest, GetCaps, GetKernelSource,
  *          CalcTiming, SetFastModePlus, RecoverBus and the checks are i2c_bm's
  *          (plan "Contrato de API").
  ******************************************************************************
  */

#ifndef I2C_HAL_H
#define I2C_HAL_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx_hal.h"
#include "i2c_bm.h"

#if defined(HAL_I2C_MODULE_ENABLED)

#ifdef __cplusplus
extern "C" {
#endif

/** h->Instance must be cfg->i2c; cfg->timeout_scl_low must be 0 (ERR_UNSUPPORTED).
    Validated by I2C_BM_CheckConfig; then I2C_BM_Deinit, the handle to RESET, h->Init
    filled from cfg (7-bit, own address 0, no dual address, no general call, clock
    stretching on), HAL_I2C_Init, the two filter calls; read-back of CR1, TIMINGR,
    TIMEOUTR (0), OAR1 (OA1EN), OAR2 (0), CR2 (AUTOEND | NACK); ERR_BUS if ISR.BUSY
    is then 1, as I2C_BM_Init. */
I2cBm_Status_t I2C_HAL_Init(I2C_HandleTypeDef *h, const I2cBm_Config_t *cfg);
/** HAL_I2C_Master_Transmit; n 0..65 535 (0 = the address probe, tx may be NULL);
    timeout_ms >= 1. Read-back: no interrupt or DMA enable, CR2 transfer fields
    cleared, STOPF cleared, h->State READY. */
I2cBm_Status_t I2C_HAL_Write(I2C_HandleTypeDef *h, uint32_t addr7, const uint8_t *tx, uint32_t n,
                             uint32_t timeout_ms);
/** HAL_I2C_Master_Receive; n 1..65 535; timeout_ms >= 1. Read-back as I2C_HAL_Write. */
I2cBm_Status_t I2C_HAL_Read(I2C_HandleTypeDef *h, uint32_t addr7, uint8_t *rx, uint32_t n, uint32_t timeout_ms);
/** HAL_I2C_Mem_Read with tx as a register address of ntx = 1 or 2 bytes (tx[0] first);
    ntx > 2 ERR_UNSUPPORTED; nrx 1..65 535. Read-back as I2C_HAL_Write. */
I2cBm_Status_t I2C_HAL_WriteRead(I2C_HandleTypeDef *h, uint32_t addr7, const uint8_t *tx, uint32_t ntx,
                                 uint8_t *rx, uint32_t nrx, uint32_t timeout_ms);
/** HAL_I2C_Master_Transmit_IT (nrx = 0; ntx = 0 is the probe), HAL_I2C_Master_Receive_IT
    (ntx = 0) or HAL_I2C_Mem_Read_IT (both; ntx 1 or 2). Validated by
    I2C_BM_CheckStartIt. No read-back here: the transfer may already be over;
    I2C_HAL_Wait reads back the close. */
I2cBm_Status_t I2C_HAL_StartIt(I2C_HandleTypeDef *h, uint32_t addr7, const uint8_t *tx, uint32_t ntx,
                               uint8_t *rx, uint32_t nrx);
/** HAL_I2C_Master_Transmit_DMA (tx, n 2..65 535, I2C_BM_CheckDmaStartWrite) or
    HAL_I2C_Master_Receive_DMA (rx, n 1..65 535, I2C_BM_CheckDmaStartRead): exactly
    one of tx and rx, with h->hdmatx or h->hdmarx linked by the caller (else
    ERR_PARAM). No read-back here, as I2C_HAL_StartIt. */
I2cBm_Status_t I2C_HAL_StartDma(I2C_HandleTypeDef *h, uint32_t addr7, const uint8_t *tx, uint8_t *rx, uint32_t n);
/** Polls h->State until READY, at most timeout_ms (>= 1) of HAL_GetTick; then
    h->ErrorCode, and the read-back of the close (no interrupt or DMA enable, CR2
    transfer fields cleared). ERR_TIMEOUT leaves the transfer as it is.
    Precondition: the HAL tick and the I2C (and DMA) interrupts must be able to run,
    so call it from thread mode with PRIMASK clear, or from an ISR of lower priority
    than all of them. Otherwise h->State never changes and the tick stops: after
    2 x (SystemCoreClock / 1000) x the tick period in ms polls with the same
    HAL_GetTick() value (two tick periods at least) it returns ERR_TIMEOUT. */
I2cBm_Status_t I2C_HAL_Wait(I2C_HandleTypeDef *h, uint32_t timeout_ms);
/** I2C_BM_Deinit(h->Instance); the handle back to RESET (State, Mode, ErrorCode, Lock,
    no transfer routine: a stray interrupt then does nothing in the HAL). */
I2cBm_Status_t I2C_HAL_Deinit(I2C_HandleTypeDef *h);

#ifdef __cplusplus
}
#endif

#endif /* HAL_I2C_MODULE_ENABLED */

#endif /* I2C_HAL_H */
