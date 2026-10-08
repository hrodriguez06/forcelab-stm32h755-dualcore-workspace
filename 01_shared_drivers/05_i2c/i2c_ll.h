/**
  ******************************************************************************
  * @file    i2c_ll.h
  * @brief   I2C1-I2C4 (master, 7-bit addresses), LL tier: the configuration and
  *          the transfers of i2c_bm with the constants and inlines of the ST LL
  *          header (LL_I2C_ANALOGFILTER_*, LL_I2C_ADDRSLAVE_7BIT, LL_I2C_MODE_*,
  *          LL_I2C_GENERATE_START_*, LL_I2C_SMBUS_TIMEOUTA*).
  *
  *          Driver tier: *_ll. Adds only the static inline functions and macros
  *          of stm32h7xx_ll_i2c.h to i2c_bm (C8): no USE_FULL_LL_DRIVER, no
  *          LL_I2C_Init, no state in RAM (C7).
  *
  *          Contract (05_i2c plan, revision 3, "Contrato de API: i2c_ll"):
  *            - every call is validated by i2c_bm before anything is written:
  *              I2C_BM_CheckConfig for the translated configuration, and the
  *              I2C_BM_Check* of each transfer (one copy of the rules and of the
  *              capability table). An LL constant this API does not know becomes
  *              an out-of-range i2c_bm value (DNF 16), so it is refused with
  *              I2C_BM_ERR_PARAM where the C6 order puts a field error;
  *            - the registers are written with the LL inlines, and with
  *              LL_I2C_WriteReg where no inline writes the whole register (CR1 at
  *              Init, OAR1, OAR2, CR2 = 0, ICR); in the order of the i2c_bm rules
  *              3-9. ICR is always one LL_I2C_WriteReg: the LL_I2C_ClearFlag_*
  *              inlines are SET_BIT, a read-modify-write of a write-only register.
  *              Every read-modify-write (the LL SET_BIT/CLEAR_BIT/MODIFY_REG
  *              inlines of CR1 and CR2, LL_I2C_HandleTransfer included) runs with
  *              PRIMASK saved and restored (C10); every configuration write is
  *              read back (C12);
  *            - for the same configuration the register image is the one i2c_bm
  *              leaves, bit for bit (T2);
  *            - the transfers behave as i2c_bm's, with the same codes
  *              (I2cBm_Status_t) and the same documented meaning (i2c_bm.h): the
  *              first byte of a write in TXDR before START, also with DMA (ES0445
  *              2.19.6); BERR in master mode cleared, the transfer goes on (2.19.2);
  *              a TIMEOUT ends with ERR_BUS and the software reset (plan R3-5);
  *              every wait bounded by max_cycles and by a stopped CYCCNT (C13);
  *              a bus held since before Init is not seen (R3-6);
  *            - XferIrq runs from both vectors of the instance, at the same
  *              priority; the interrupt state is the caller's I2cBm_Xfer_t.
  *
  *          Clock, Deinit, TakeErrors, GetIRQn, GetDmaRequest, GetCaps,
  *          GetKernelSource, CalcTiming, SetFastModePlus, RecoverBus and the
  *          checks take no LL constant: use I2C_BM_* for them (plan "Contrato de
  *          API: i2c_ll").
  ******************************************************************************
  */

#ifndef I2C_LL_H
#define I2C_LL_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"
#include "stm32h7xx_ll_i2c.h"
#include "i2c_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Configuration ------------------------------------------------------------- */
typedef struct
{
    I2C_TypeDef *i2c;
    uint32_t     timing;           /*!< TIMINGR (LL_I2C_SetTiming): I2C_BM_CalcTiming's timingr, or
                                        the caller's own; bits 27:24 = 0                           */
    uint32_t     analog_filter;    /*!< LL_I2C_ANALOGFILTER_ENABLE / LL_I2C_ANALOGFILTER_DISABLE    */
    uint32_t     digital_filter;   /*!< 0x00..0x0F (DNF, LL_I2C_SetDigitalFilter's range)          */
    uint32_t     timeout_scl_low;  /*!< as I2cBm_Config_t: 0 = off; else TIMEOUTA + 1 (1..4096),
                                        TIDLE = LL_I2C_SMBUS_TIMEOUTA_MODE_SCL_LOW, enabled with
                                        LL_I2C_EnableSMBusTimeout(LL_I2C_SMBUS_TIMEOUTA)            */
} I2cLl_Config_t;

/** I2C_BM_CheckConfig of the translation: the checks of I2C_LL_Init, nothing written.
    ERR_PARAM also for NULL and for an analog_filter that is not one of the two LL
    constants. */
I2cBm_Status_t I2C_LL_CheckConfig(const I2cLl_Config_t *cfg);
/** As I2C_BM_Init (plan rule 3), validated by I2C_LL_CheckConfig; the same codes. */
I2cBm_Status_t I2C_LL_Init(const I2cLl_Config_t *cfg);

/* ---- Blocking transfers (as i2c_bm; validated by I2C_BM_CheckWrite/Read/WriteRead) -- */
I2cBm_Status_t I2C_LL_Write(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t n, uint32_t max_cycles);
I2cBm_Status_t I2C_LL_Read(I2C_TypeDef *i2c, uint32_t addr7, uint8_t *rx, uint32_t n, uint32_t max_cycles);
I2cBm_Status_t I2C_LL_WriteRead(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t ntx,
                                uint8_t *rx, uint32_t nrx, uint32_t max_cycles);

/* ---- Interrupt transfers (as i2c_bm; validated by I2C_BM_CheckStartIt) ---------- */
I2cBm_Status_t I2C_LL_StartIt(I2C_TypeDef *i2c, I2cBm_Xfer_t *x);
bool           I2C_LL_XferIrq(I2C_TypeDef *i2c, I2cBm_Xfer_t *x);   /* both vectors, same priority;
                                                                       true when x->status is final */

/* ---- DMA (as i2c_bm; validated by I2C_BM_CheckDmaStart* / _CheckWaitEnd) -------- */
I2cBm_Status_t I2C_LL_DmaStartWrite(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *buf, uint32_t n);
I2cBm_Status_t I2C_LL_DmaStartRead(I2C_TypeDef *i2c, uint32_t addr7, uint32_t n);
I2cBm_Status_t I2C_LL_WaitEnd(I2C_TypeDef *i2c, uint32_t n, uint32_t max_cycles);
I2cBm_Status_t I2C_LL_DmaEnd(I2C_TypeDef *i2c);

#ifdef __cplusplus
}
#endif

#endif /* I2C_LL_H */
