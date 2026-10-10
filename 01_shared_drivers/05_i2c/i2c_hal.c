/**
  ******************************************************************************
  * @file    i2c_hal.c
  * @brief   I2C1-I2C4, HAL tier. Contract and the HAL's limitations in i2c_hal.h.
  *
  *          Validation by i2c_bm before any HAL call (C6), then the HAL call,
  *          then the read-back of what that call writes (C12). Line numbers of
  *          stm32h7xx_hal_i2c.c below are those of the Cube the project builds
  *          with (STM32H7xx_HAL_Driver e3c518e, HAL V1.11.6).
  ******************************************************************************
  */

#include "i2c_hal.h"

#if defined(HAL_I2C_MODULE_ENABLED)

/* Plan rule 11: no callbacks registered by this tier, so none to register (C14). */
#if (USE_HAL_I2C_REGISTER_CALLBACKS != 0U)
#error "i2c_hal needs USE_HAL_I2C_REGISTER_CALLBACKS = 0 in stm32h7xx_hal_conf.h (05_i2c plan rule 11)"
#endif

/* C8: no <stddef.h>. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* The HAL constants are the register fields this tier reads back: the analog filter
   is ANFOFF (stm32h7xx_hal_i2c_ex.h), the 7-bit own address 0 enables OA1EN alone. */
_Static_assert((I2C_ANALOGFILTER_ENABLE == 0U) && (I2C_ANALOGFILTER_DISABLE == I2C_CR1_ANFOFF),
               "I2C_ANALOGFILTER_x is not ANFOFF");

#define CR1_IE_ALL      (I2C_CR1_TXIE | I2C_CR1_RXIE | I2C_CR1_ADDRIE | I2C_CR1_NACKIE | I2C_CR1_STOPIE | \
                         I2C_CR1_TCIE | I2C_CR1_ERRIE)
#define CR1_DMA_ALL     (I2C_CR1_TXDMAEN | I2C_CR1_RXDMAEN)
/* HAL_I2C_Init l. 619 writes AUTOEND | NACK with PE = 0 (l. 587 .. 634); NACK is held
   cleared by hardware while PE = 0 (RM0399 50:2621-2623), so only AUTOEND stays. */
#define CR2_HAL_INIT    I2C_CR2_AUTOEND
/* I2C_RESET_CR2 (stm32h7xx_hal_i2c.h l. 781-784) clears these at every close; START
   and STOP clear themselves (50:2620-2674). AUTOEND is not one of them: each
   I2C_TransferConfig rewrites it (its mask, l. 7321-7324, has AUTOEND) and the close
   leaves it as the last one did; the read-back does not look at it. */
#define CR2_XFER        (I2C_CR2_SADD | I2C_CR2_HEAD10R | I2C_CR2_NBYTES | I2C_CR2_RELOAD | I2C_CR2_RD_WRN | \
                         I2C_CR2_START | I2C_CR2_STOP)
#define REG_ADDR_MAX    2U      /* HAL_I2C_Mem_Read: I2C_MEMADD_SIZE_8BIT or _16BIT */

/* The HAL takes uint8_t * for data it only reads (V1.11.6): const kept in this API. */
static uint8_t *unconst(const uint8_t *p)
{
    union { const uint8_t *c; uint8_t *m; } u;
    u.c = p;
    return u.m;
}

/* A HAL status, with h->ErrorCode, as I2cBm_Status_t. TIMEOUT first: a NACK whose
   STOP never came is ERR_TIMEOUT, as in i2c_bm. */
static I2cBm_Status_t from_hal(HAL_StatusTypeDef hs, const I2C_HandleTypeDef *h)
{
    const uint32_t e = h->ErrorCode;
    if (hs == HAL_OK)
    {
        return I2C_BM_OK;
    }
    if (hs == HAL_BUSY)
    {
        return I2C_BM_ERR_BUSY;
    }
    if ((hs == HAL_TIMEOUT) || ((e & HAL_I2C_ERROR_TIMEOUT) != 0U))
    {
        return I2C_BM_ERR_TIMEOUT;
    }
    if ((e & HAL_I2C_ERROR_ARLO) != 0U)
    {
        return I2C_BM_ERR_ARLO;
    }
    if ((e & HAL_I2C_ERROR_AF) != 0U)
    {
        return I2C_BM_ERR_NACK;
    }
    return I2C_BM_ERR_HAL;               /* BERR (IC1), OVR, DMA, SIZE, INVALID_PARAM */
}

static I2cBm_Status_t chk_handle(const I2C_HandleTypeDef *h)
{
    return ((h == NULL) || !I2C_BM_IsValidInstance(h->Instance)) ? I2C_BM_ERR_PARAM : I2C_BM_OK;
}

/* The close of a transfer as the HAL leaves it (I2C_ITMasterCplt l. 6138-6273, the
   blocking calls' end, e.g. l. 1230-1236): no interrupt or DMA enable, the transfer
   fields of CR2 cleared, STOPF cleared, the handle READY. */
static bool closed(const I2C_HandleTypeDef *h)
{
    const I2C_TypeDef *i2c = h->Instance;
    return ((i2c->CR1 & (CR1_IE_ALL | CR1_DMA_ALL)) == 0U) && ((i2c->CR2 & CR2_XFER) == 0U) &&
           ((i2c->ISR & I2C_ISR_STOPF) == 0U) && (h->State == HAL_I2C_STATE_READY);
}

/* ---- Init / Deinit ----------------------------------------------------------------- */

I2cBm_Status_t I2C_HAL_Init(I2C_HandleTypeDef *h, const I2cBm_Config_t *cfg)
{
    if ((h == NULL) || (cfg == NULL) || !I2C_BM_IsValidInstance(h->Instance) || (h->Instance != cfg->i2c))
    {
        return I2C_BM_ERR_PARAM;
    }
    /* C6, capability before range: the HAL never writes TIMEOUTR (HAL_I2C_Init
       l. 535-642), so timeout_scl_low cannot be honoured (owner, 2026-10-09). */
    if (cfg->timeout_scl_low != 0U)
    {
        return I2C_BM_ERR_UNSUPPORTED;
    }
    const I2cBm_Status_t st = I2C_BM_CheckConfig(cfg);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    I2C_TypeDef *i2c = h->Instance;
    /* From the reset image (i2c_bm's PE = 0 read back first), with the handle at RESET
       so HAL_I2C_Init runs its whole sequence (and the project's MSP, l. 580). */
    const I2cBm_Status_t sd = I2C_BM_Deinit(i2c);
    if (sd != I2C_BM_OK)
    {
        return sd;
    }
    h->State                 = HAL_I2C_STATE_RESET;
    h->Init.Timing           = cfg->timingr;
    h->Init.OwnAddress1      = 0U;
    h->Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    h->Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    h->Init.OwnAddress2      = 0U;
    h->Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    h->Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    h->Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;      /* NOSTRETCH must be 0 in master (50:2505) */
    HAL_StatusTypeDef hs = HAL_I2C_Init(h);
    if (hs == HAL_OK)
    {
        hs = HAL_I2CEx_ConfigAnalogFilter(h, cfg->analog_filter ? I2C_ANALOGFILTER_ENABLE : I2C_ANALOGFILTER_DISABLE);
    }
    if (hs == HAL_OK)
    {
        hs = HAL_I2CEx_ConfigDigitalFilter(h, cfg->dnf);
    }
    if (hs != HAL_OK)
    {
        return from_hal(hs, h);
    }
    /* Read-back of what HAL_I2C_Init (l. 591-634) and the two filter calls wrote. OAR1
       and CR2 are the HAL's own by design (l. 600, 619: AUTOEND); TIMEOUTR stays at reset. */
    const uint32_t cr1 = (cfg->analog_filter ? 0U : I2C_CR1_ANFOFF) | (cfg->dnf << I2C_CR1_DNF_Pos) | I2C_CR1_PE;
    if ((i2c->CR1 != cr1) || (i2c->TIMINGR != cfg->timingr) || (i2c->TIMEOUTR != 0U) ||
        (i2c->OAR1 != I2C_OAR1_OA1EN) || (i2c->OAR2 != 0U) || (i2c->CR2 != CR2_HAL_INIT) ||
        (h->State != HAL_I2C_STATE_READY))
    {
        return I2C_BM_ERR_VERIFY;
    }
    /* As I2C_BM_Init: a START seen since PE = 1 (R3-6: a line held from before is not). */
    return ((i2c->ISR & I2C_ISR_BUSY) != 0U) ? I2C_BM_ERR_BUS : I2C_BM_OK;
}

I2cBm_Status_t I2C_HAL_Deinit(I2C_HandleTypeDef *h)
{
    if (chk_handle(h) != I2C_BM_OK)
    {
        return I2C_BM_ERR_PARAM;
    }
    const I2cBm_Status_t st = I2C_BM_Deinit(h->Instance);   /* not HAL_I2C_DeInit: no MSP (C14) */
    h->XferISR       = NULL;          /* HAL_I2C_EV_IRQHandler does nothing without it (l. 4652) */
    h->ErrorCode     = HAL_I2C_ERROR_NONE;
    h->Mode          = HAL_I2C_MODE_NONE;
    h->PreviousState = (uint32_t)HAL_I2C_MODE_NONE;          /* I2C_STATE_NONE (l. 362) */
    h->State         = HAL_I2C_STATE_RESET;
    h->Lock          = HAL_UNLOCKED;
    return st;
}

/* ---- Blocking transfers -------------------------------------------------------------- */

/* The order of every transfer (C6): the handle and its instance; what this tier cannot
   do (ERR_UNSUPPORTED, capability); timeout_ms and i2c_bm's check of the call (range,
   and the instance's state); then the handle's state. */
static I2cBm_Status_t chk_state(const I2C_HandleTypeDef *h)
{
    return (h->State == HAL_I2C_STATE_READY) ? I2C_BM_OK : I2C_BM_ERR_BUSY;
}

/* timeout_ms 1 .. HAL_MAX_DELAY - 1 (audit F3 part 1, M4): with HAL_MAX_DELAY the HAL's
   waits never time out (I2C_WaitOnFlagUntilTimeout l. 6980, I2C_WaitOnTXISFlagUntilTimeout
   l. 7020), and without TIMEOUTR (refused in I2C_HAL_Init) a held SCL would hang the
   call for ever. I2C_HAL_Wait takes the same range: its own wait has no other bound. */
static bool timeout_ok(uint32_t timeout_ms)
{
    return (timeout_ms != 0U) && (timeout_ms != HAL_MAX_DELAY);
}

static I2cBm_Status_t blocking_end(HAL_StatusTypeDef hs, const I2C_HandleTypeDef *h)
{
    const I2cBm_Status_t st = from_hal(hs, h);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    return closed(h) ? I2C_BM_OK : I2C_BM_ERR_VERIFY;
}

I2cBm_Status_t I2C_HAL_Write(I2C_HandleTypeDef *h, uint32_t addr7, const uint8_t *tx, uint32_t n,
                             uint32_t timeout_ms)
{
    I2cBm_Status_t st = chk_handle(h);
    if ((st == I2C_BM_OK) && !timeout_ok(timeout_ms))
    {
        st = I2C_BM_ERR_PARAM;
    }
    st = (st == I2C_BM_OK) ? I2C_BM_CheckWrite(h->Instance, addr7, tx, n, timeout_ms) : st;
    st = (st == I2C_BM_OK) ? chk_state(h) : st;
    if (st != I2C_BM_OK)
    {
        return st;
    }
    /* l. 1119-1247: TXDR preloaded before START (l. 1162, ES0445 2.19.6). */
    const HAL_StatusTypeDef hs = HAL_I2C_Master_Transmit(h, (uint16_t)(addr7 << 1), unconst(tx), (uint16_t)n,
                                                         timeout_ms);
    return blocking_end(hs, h);
}

I2cBm_Status_t I2C_HAL_Read(I2C_HandleTypeDef *h, uint32_t addr7, uint8_t *rx, uint32_t n, uint32_t timeout_ms)
{
    I2cBm_Status_t st = chk_handle(h);
    if ((st == I2C_BM_OK) && !timeout_ok(timeout_ms))
    {
        st = I2C_BM_ERR_PARAM;
    }
    st = (st == I2C_BM_OK) ? I2C_BM_CheckRead(h->Instance, addr7, rx, n, timeout_ms) : st;
    st = (st == I2C_BM_OK) ? chk_state(h) : st;
    if (st != I2C_BM_OK)
    {
        return st;
    }
    const HAL_StatusTypeDef hs = HAL_I2C_Master_Receive(h, (uint16_t)(addr7 << 1), rx, (uint16_t)n, timeout_ms);
    return blocking_end(hs, h);
}

/* The register address of HAL_I2C_Mem_Read from tx: 8 or 16 bits, tx[0] first
   (I2C_MEM_ADD_MSB is sent first, l. 5889). */
static uint16_t reg_addr(const uint8_t *tx, uint32_t ntx)
{
    return (ntx == 1U) ? (uint16_t)tx[0] : (uint16_t)(((uint32_t)tx[0] << 8) | (uint32_t)tx[1]);
}

I2cBm_Status_t I2C_HAL_WriteRead(I2C_HandleTypeDef *h, uint32_t addr7, const uint8_t *tx, uint32_t ntx,
                                 uint8_t *rx, uint32_t nrx, uint32_t timeout_ms)
{
    I2cBm_Status_t st = chk_handle(h);
    if ((st == I2C_BM_OK) && (ntx > REG_ADDR_MAX))
    {
        st = I2C_BM_ERR_UNSUPPORTED;                    /* owner, 2026-10-09; C6: capability first (m40) */
    }
    if ((st == I2C_BM_OK) && !timeout_ok(timeout_ms))
    {
        st = I2C_BM_ERR_PARAM;
    }
    st = (st == I2C_BM_OK) ? I2C_BM_CheckWriteRead(h->Instance, addr7, tx, ntx, rx, nrx, timeout_ms) : st;
    st = (st == I2C_BM_OK) ? chk_state(h) : st;
    if (st != I2C_BM_OK)
    {
        return st;
    }
    /* l. 2660-2784: the address phase with SOFTEND, TC, then a repeated start for the
       read (I2C_RequestMemoryRead l. 5867-5908). */
    const HAL_StatusTypeDef hs = HAL_I2C_Mem_Read(h, (uint16_t)(addr7 << 1), reg_addr(tx, ntx),
                                                  (ntx == 1U) ? I2C_MEMADD_SIZE_8BIT : I2C_MEMADD_SIZE_16BIT, rx,
                                                  (uint16_t)nrx, timeout_ms);
    return blocking_end(hs, h);
}

/* ---- Interrupt and DMA transfers ------------------------------------------------------ */

I2cBm_Status_t I2C_HAL_StartIt(I2C_HandleTypeDef *h, uint32_t addr7, const uint8_t *tx, uint32_t ntx,
                               uint8_t *rx, uint32_t nrx)
{
    I2cBm_Status_t st = chk_handle(h);
    if ((st == I2C_BM_OK) && (ntx > REG_ADDR_MAX) && (nrx != 0U))
    {
        st = I2C_BM_ERR_UNSUPPORTED;                    /* a write-read: as I2C_HAL_WriteRead (m40) */
    }
    if (st == I2C_BM_OK)
    {
        const I2cBm_Xfer_t x = { addr7, tx, ntx, rx, nrx, 0U, 0U, I2C_BM_PENDING };
        st = I2C_BM_CheckStartIt(h->Instance, &x);
    }
    st = (st == I2C_BM_OK) ? chk_state(h) : st;
    if (st != I2C_BM_OK)
    {
        return st;
    }
    HAL_StatusTypeDef hs;
    const uint16_t a = (uint16_t)(addr7 << 1);
    if ((ntx != 0U) && (nrx != 0U))
    {
        hs = HAL_I2C_Mem_Read_IT(h, a, reg_addr(tx, ntx), (ntx == 1U) ? I2C_MEMADD_SIZE_8BIT : I2C_MEMADD_SIZE_16BIT,
                                 rx, (uint16_t)nrx);
    }
    else if (nrx != 0U)
    {
        hs = HAL_I2C_Master_Receive_IT(h, a, rx, (uint16_t)nrx);
    }
    else
    {
        hs = HAL_I2C_Master_Transmit_IT(h, a, unconst(tx), (uint16_t)ntx);   /* ntx = 0: the probe */
    }
    return from_hal(hs, h);
}

I2cBm_Status_t I2C_HAL_StartDma(I2C_HandleTypeDef *h, uint32_t addr7, const uint8_t *tx, uint8_t *rx, uint32_t n)
{
    I2cBm_Status_t st = chk_handle(h);
    if ((st == I2C_BM_OK) && (((tx == NULL) == (rx == NULL)) || ((tx != NULL) && (h->hdmatx == NULL)) ||
                              ((rx != NULL) && (h->hdmarx == NULL))))
    {
        st = I2C_BM_ERR_PARAM;                         /* one direction, its DMA handle linked */
    }
    if (st == I2C_BM_OK)
    {
        st = (tx != NULL) ? I2C_BM_CheckDmaStartWrite(h->Instance, addr7, tx, n) :
                            I2C_BM_CheckDmaStartRead(h->Instance, addr7, n);
    }
    st = (st == I2C_BM_OK) ? chk_state(h) : st;
    if (st != I2C_BM_OK)
    {
        return st;
    }
    const uint16_t a = (uint16_t)(addr7 << 1);
    /* l. 1970-2121 (TXDR preloaded, l. 2012; the stream sends n - 1) and l. 2133-2268. */
    const HAL_StatusTypeDef hs = (tx != NULL) ? HAL_I2C_Master_Transmit_DMA(h, a, unconst(tx), (uint16_t)n) :
                                                HAL_I2C_Master_Receive_DMA(h, a, rx, (uint16_t)n);
    return from_hal(hs, h);
}

I2cBm_Status_t I2C_HAL_Wait(I2C_HandleTypeDef *h, uint32_t timeout_ms)
{
    if ((chk_handle(h) != I2C_BM_OK) || !timeout_ok(timeout_ms))
    {
        return I2C_BM_ERR_PARAM;
    }
    /* C13 (lesson m4): an exit when HAL_GetTick() stops (PRIMASK set, or a caller at or
       above the tick's priority). A poll takes at least one CPU cycle, so this many polls
       with the same tick are at least two tick periods (uwTickFreq ms each). */
    const uint32_t stall_max = 2U * (uint32_t)HAL_GetTickFreq() * (SystemCoreClock / 1000U);
    const uint32_t t0 = HAL_GetTick();
    uint32_t last = t0;
    uint32_t same = 0U;
    while (h->State != HAL_I2C_STATE_READY)
    {
        const uint32_t now = HAL_GetTick();
        if ((now - t0) >= timeout_ms)
        {
            return I2C_BM_ERR_TIMEOUT;
        }
        if (now != last)
        {
            last = now;
            same = 0U;
        }
        else if (++same > stall_max)
        {
            return I2C_BM_ERR_TIMEOUT;          /* the tick stopped */
        }
    }
    if (h->ErrorCode != HAL_I2C_ERROR_NONE)
    {
        return from_hal(HAL_ERROR, h);
    }
    return closed(h) ? I2C_BM_OK : I2C_BM_ERR_VERIFY;
}

#endif /* HAL_I2C_MODULE_ENABLED */
