/**
  ******************************************************************************
  * @file    i2c_ll.c
  * @brief   I2C1-I2C4, LL tier. Contract in i2c_ll.h.
  *
  *          Every function lets i2c_bm validate it (I2C_BM_CheckConfig on the
  *          translated configuration, I2C_BM_Check* for the transfers: C6, one
  *          copy of the rules and of the capability table), then writes with the
  *          LL inlines and macros of stm32h7xx_ll_i2c.h in the order of the i2c_bm
  *          rules (plan "Reglas de implementación" 3-9) and reads back. Register
  *          facts: RM0399 rev 4, chapter 50, cited as in i2c_bm.c.
  ******************************************************************************
  */

#include "i2c_ll.h"

/* C8: no <stddef.h>. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- Register facts, as i2c_bm.c (RM0399 50.7) ------------------------------- */
#define ICR_ALL         (I2C_ICR_ADDRCF | I2C_ICR_NACKCF | I2C_ICR_STOPCF | I2C_ICR_BERRCF | I2C_ICR_ARLOCF | \
                         I2C_ICR_OVRCF | I2C_ICR_PECCF | I2C_ICR_TIMOUTCF | I2C_ICR_ALERTCF)
#define IE_ALL          (LL_I2C_CR1_TXIE | LL_I2C_CR1_RXIE | LL_I2C_CR1_ADDRIE | LL_I2C_CR1_NACKIE | \
                         LL_I2C_CR1_STOPIE | LL_I2C_CR1_TCIE | LL_I2C_CR1_ERRIE)
#define CHUNK           255U     /* NBYTES[7:0], 50:1081-1082 */
#define PE_LOW_READS    3U       /* PE = 0 for >= 3 APB cycles (50:441-445) */
#define PE_LOW_MAX      1000U
#define STALL_READS     1000U    /* equal CYCCNT reads that mean the counter stopped (C13) */
#define CLEAR_READS     256U     /* a flag a CR2 write clears may read set 2 PCLK + 6 kernel later (50:2442-2444) */
#define BM_BAD_DNF      16U      /* outside 0..15: ERR_PARAM in i2c_bm (an unknown LL constant) */

/* ---- PRIMASK section (C10): saved and restored, never unconditionally enabled - */
static inline uint32_t irq_save(void)
{
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    return pm;
}

static inline void irq_restore(uint32_t pm)
{
    __set_PRIMASK(pm);
}

/* ==========================================================================
 * LL constants -> i2c_bm configuration. analog_filter has two LL values; any
 * other becomes DNF 16, which I2C_BM_CheckConfig refuses with ERR_PARAM in its
 * field-range check, so the C6 order is i2c_bm's.
 * ========================================================================== */

static bool analog_known(uint32_t af)
{
    return (af == LL_I2C_ANALOGFILTER_ENABLE) || (af == LL_I2C_ANALOGFILTER_DISABLE);
}

static void to_bm(const I2cLl_Config_t *c, I2cBm_Config_t *b)
{
    b->i2c             = c->i2c;
    b->timingr         = c->timing;
    b->analog_filter   = (c->analog_filter == LL_I2C_ANALOGFILTER_ENABLE);
    b->dnf             = analog_known(c->analog_filter) ? c->digital_filter : BM_BAD_DNF;
    b->timeout_scl_low = c->timeout_scl_low;
}

I2cBm_Status_t I2C_LL_CheckConfig(const I2cLl_Config_t *cfg)
{
    if (cfg == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    I2cBm_Config_t b;
    to_bm(cfg, &b);
    return I2C_BM_CheckConfig(&b);
}

/* ==========================================================================
 * Init (plan rule 3), the image made of the LL constants
 * ========================================================================== */

/* PE = 0 (LL_I2C_Disable, a CLEAR_BIT of CR1, under PRIMASK), read back until it
   reads 0 at least PE_LOW_READS times: the software reset (50:441-445); SCL and SDA
   let go (50:429-431). */
static bool pe_off(I2C_TypeDef *i2c)
{
    const uint32_t pm = irq_save();
    LL_I2C_Disable(i2c);
    irq_restore(pm);
    uint32_t zeros = 0U;
    for (uint32_t i = 0U; i < PE_LOW_MAX; i++)
    {
        if (LL_I2C_IsEnabled(i2c) == 0U)
        {
            if (++zeros >= PE_LOW_READS)
            {
                return true;
            }
        }
    }
    return false;
}

static void pe_on(I2C_TypeDef *i2c)
{
    const uint32_t pm = irq_save();
    LL_I2C_Enable(i2c);
    irq_restore(pm);
}

/* The software reset of rule 6: PE = 0 read back, CR2 = 0 (START is 0 with PE = 0,
   50:2645), PE = 1. */
static void soft_reset(I2C_TypeDef *i2c)
{
    (void)pe_off(i2c);
    LL_I2C_WriteReg(i2c, CR2, 0U);
    pe_on(i2c);
}

I2cBm_Status_t I2C_LL_Init(const I2cLl_Config_t *cfg)
{
    const I2cBm_Status_t st = I2C_LL_CheckConfig(cfg);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    I2C_TypeDef *i2c = cfg->i2c;
    /* CR1: ANFOFF (LL_I2C_ANALOGFILTER_DISABLE is I2C_CR1_ANFOFF) and DNF; NOSTRETCH,
       every interrupt and DMA enable 0: CR1 is written whole, no inline does that. */
    const uint32_t filters = cfg->analog_filter | (cfg->digital_filter << I2C_CR1_DNF_Pos);
    const uint32_t tmo_a = (cfg->timeout_scl_low == 0U) ? 0U
                           : (((cfg->timeout_scl_low - 1U) << I2C_TIMEOUTR_TIMEOUTA_Pos) |
                              LL_I2C_SMBUS_TIMEOUTA_MODE_SCL_LOW);
    const uint32_t tmo = (cfg->timeout_scl_low == 0U) ? 0U : (tmo_a | LL_I2C_SMBUS_TIMEOUTA);

    if (!pe_off(i2c))                                  /* 50:404-425, figure 585 */
    {
        return I2C_BM_ERR_VERIFY;
    }
    LL_I2C_WriteReg(i2c, CR1, filters);
    LL_I2C_SetTiming(i2c, cfg->timing);
    /* TIMEOUTA only with TIMOUTEN = 0 (50:2861): TIMOUTEN off first (a TIMEOUTA written
       with it on would be ignored), then TIMEOUTA and TIDLE = SCL low with the LL
       inline (a WRITE_REG of TIMEOUTR, TIMOUTEN 0), then TIMOUTEN with the LL inline
       (a SET_BIT, under PRIMASK). The image is i2c_bm's three writes. */
    LL_I2C_WriteReg(i2c, TIMEOUTR, 0U);
    LL_I2C_SetSMBusTimeoutA(i2c, tmo_a);
    if (tmo != 0U)
    {
        const uint32_t pm = irq_save();
        LL_I2C_EnableSMBusTimeout(i2c, LL_I2C_SMBUS_TIMEOUTA);
        irq_restore(pm);
    }
    LL_I2C_WriteReg(i2c, OAR1, 0U);                    /* OA1EN = 0 first: then OA1 may be written */
    LL_I2C_WriteReg(i2c, OAR1, 0U);
    LL_I2C_WriteReg(i2c, OAR2, 0U);
    LL_I2C_WriteReg(i2c, OAR2, 0U);
    LL_I2C_WriteReg(i2c, CR2, 0U);
    LL_I2C_WriteReg(i2c, ICR, ICR_ALL);
    pe_on(i2c);
    __DSB();
    if ((LL_I2C_ReadReg(i2c, CR1) != (filters | I2C_CR1_PE)) || (LL_I2C_ReadReg(i2c, TIMINGR) != cfg->timing) ||
        (LL_I2C_ReadReg(i2c, TIMEOUTR) != tmo) || (LL_I2C_ReadReg(i2c, OAR1) != 0U) ||
        (LL_I2C_ReadReg(i2c, OAR2) != 0U))
    {
        return I2C_BM_ERR_VERIFY;
    }
    return (LL_I2C_IsActiveFlag_BUSY(i2c) != 0U) ? I2C_BM_ERR_BUS : I2C_BM_OK;
}

/* ==========================================================================
 * Transfers (plan rules 5-8), with the LL inlines
 * ========================================================================== */

static bool cyccnt_running(void)
{
    return ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) != 0U) &&
           ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
}

typedef struct
{
    uint32_t t0;
    uint32_t last;
    uint32_t stalled;
    uint32_t limit;
} Clock_t;

static void clk_start(Clock_t *c, uint32_t limit)
{
    c->t0 = DWT->CYCCNT;
    c->last = c->t0;
    c->stalled = 0U;
    c->limit = limit;
}

/* True when the budget is spent or CYCCNT has stopped (C13). */
static bool clk_expired(Clock_t *c)
{
    const uint32_t now = DWT->CYCCNT;
    c->stalled = (now == c->last) ? (c->stalled + 1U) : 0U;
    c->last = now;
    return ((now - c->t0) >= c->limit) || (c->stalled > STALL_READS);
}

/* The chunk of `left` bytes: its NBYTES and LL_I2C_MODE_RELOAD while more than 255
   remain; on the last chunk LL_I2C_MODE_AUTOEND (STOP after it) or
   LL_I2C_MODE_SOFTEND (TC: the repeated start of WriteRead) (50:517-559). */
static uint32_t chunk_size(uint32_t left)
{
    return (left > CHUNK) ? CHUNK : left;
}

static uint32_t chunk_mode(uint32_t left, bool autoend)
{
    if (left > CHUNK)
    {
        return LL_I2C_MODE_RELOAD;
    }
    return autoend ? LL_I2C_MODE_AUTOEND : LL_I2C_MODE_SOFTEND;
}

/* LL_I2C_HandleTransfer (a MODIFY_REG of CR2, under PRIMASK) with a START request:
   address, 7-bit, NBYTES, end mode and START in one write; START was 0 before, so
   nothing changes with START = 1 (C12). */
static void start(I2C_TypeDef *i2c, uint32_t addr7, uint32_t left, bool autoend, bool read)
{
    const uint32_t pm = irq_save();
    LL_I2C_HandleTransfer(i2c, addr7 << 1, LL_I2C_ADDRSLAVE_7BIT, chunk_size(left), chunk_mode(left, autoend),
                          read ? LL_I2C_GENERATE_START_READ : LL_I2C_GENERATE_START_WRITE);
    irq_restore(pm);
}

static void wait_cleared(const I2C_TypeDef *i2c, uint32_t flag)
{
    for (uint32_t i = 0U; (i < CLEAR_READS) && ((LL_I2C_ReadReg(i2c, ISR) & flag) != 0U); i++)
    {
    }
}

/* The next chunk at TCR: NBYTES, RELOAD and AUTOEND of CR2 rewritten in one write
   (TCR clears when NBYTES is written non-zero, 50:526-534); START = 0 here (C12).
   One read-modify-write with LL_I2C_ReadReg/WriteReg: the LL inlines would change
   the three fields in three writes, and NBYTES must come with the mode. */
static void reload(I2C_TypeDef *i2c, uint32_t left, bool autoend)
{
    const uint32_t pm = irq_save();
    const uint32_t cr2 = LL_I2C_ReadReg(i2c, CR2) & ~(I2C_CR2_NBYTES | I2C_CR2_RELOAD | I2C_CR2_AUTOEND);
    LL_I2C_WriteReg(i2c, CR2, cr2 | (chunk_size(left) << I2C_CR2_NBYTES_Pos) | chunk_mode(left, autoend));
    irq_restore(pm);
    wait_cleared(i2c, I2C_ISR_TCR);
}

static void stop(I2C_TypeDef *i2c)
{
    const uint32_t pm = irq_save();
    LL_I2C_GenerateStopCondition(i2c);
    irq_restore(pm);
    wait_cleared(i2c, I2C_ISR_TC);
}

/* The end of a transfer: CR2 = 0, the given flags cleared in one ICR write, TXDR
   flushed (LL_I2C_ClearFlag_TXE is the one-write TXE = 1 of ISR, 50:2976). */
static void xfer_close(I2C_TypeDef *i2c, uint32_t icr)
{
    LL_I2C_WriteReg(i2c, CR2, 0U);
    LL_I2C_WriteReg(i2c, ICR, icr);
    LL_I2C_ClearFlag_TXE(i2c);
    __DSB();
}

/* TIMEOUT in master mode: the software reset at once, both lines let go, no STOP
   (plan R3-5; i2c_bm.c timeout_end). */
static void timeout_end(I2C_TypeDef *i2c)
{
    soft_reset(i2c);
    xfer_close(i2c, ICR_ALL);
}

static bool wait_stop(const I2C_TypeDef *i2c, Clock_t *c)
{
    while (LL_I2C_IsActiveFlag_STOP(i2c) == 0U)
    {
        if (clk_expired(c))
        {
            return false;
        }
    }
    return true;
}

/* One blocking transfer: ntx bytes written, then (repeated start) nrx read; ntx =
   nrx = 0 is the probe. Plan rules 5 and 6, as i2c_bm.c xfer_poll: one ISR read per
   pass, the flags served in its order. */
static I2cBm_Status_t xfer_poll(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t ntx,
                                uint8_t *rx, uint32_t nrx, uint32_t max_cycles)
{
    if (!cyccnt_running())
    {
        return I2C_BM_ERR_TIMEOUT;                  /* no time base: no wait could end (C13) */
    }
    Clock_t c;
    clk_start(&c, max_cycles);
    LL_I2C_WriteReg(i2c, ICR, ICR_ALL);             /* stale flags of an earlier transfer */
    LL_I2C_ClearFlag_TXE(i2c);

    uint32_t sent = 0U;
    uint32_t got = 0U;
    bool reading = (ntx == 0U) && (nrx != 0U);
    if (reading)
    {
        start(i2c, addr7, nrx, true, true);
    }
    else
    {
        if (ntx != 0U)
        {
            LL_I2C_TransmitData8(i2c, tx[0]);       /* ES0445 2.19.6: the first byte before START */
            sent = 1U;
        }
        start(i2c, addr7, ntx, nrx == 0U, false);
    }

    for (;;)
    {
        const uint32_t isr = LL_I2C_ReadReg(i2c, ISR);
        if ((isr & I2C_ISR_BERR) != 0U)
        {
            LL_I2C_WriteReg(i2c, ICR, I2C_ICR_BERRCF);   /* ES0445 2.19.2: spurious in master mode */
        }
        if ((isr & I2C_ISR_ARLO) != 0U)
        {
            xfer_close(i2c, ICR_ALL);
            return I2C_BM_ERR_ARLO;
        }
        if ((isr & I2C_ISR_TIMEOUT) != 0U)
        {
            timeout_end(i2c);
            return I2C_BM_ERR_BUS;
        }
        if ((isr & I2C_ISR_NACKF) != 0U)
        {
            if (!wait_stop(i2c, &c))
            {
                soft_reset(i2c);
                xfer_close(i2c, ICR_ALL);
                return I2C_BM_ERR_TIMEOUT;
            }
            xfer_close(i2c, ICR_ALL);
            return I2C_BM_ERR_NACK;
        }
        if (reading && ((isr & I2C_ISR_RXNE) != 0U))
        {
            const uint8_t b = LL_I2C_ReceiveData8(i2c);
            if (got < nrx)
            {
                rx[got] = b;
            }
            got++;
            continue;                               /* RXNE before TCR and STOPF */
        }
        if (!reading && ((isr & I2C_ISR_TXIS) != 0U) && (sent < ntx))
        {
            LL_I2C_TransmitData8(i2c, tx[sent]);
            sent++;
        }
        if ((isr & I2C_ISR_TCR) != 0U)
        {
            if (reading)
            {
                reload(i2c, nrx - got, true);
            }
            else
            {
                reload(i2c, ntx - sent, nrx == 0U);
            }
        }
        if ((isr & I2C_ISR_TC) != 0U)
        {
            if (!reading && (nrx != 0U))
            {
                reading = true;                     /* the repeated start (50:1185-1188) */
                start(i2c, addr7, nrx, true, true);
                wait_cleared(i2c, I2C_ISR_TC);
            }
            else
            {
                stop(i2c);                          /* not reached with AUTOEND = 1 */
            }
        }
        if ((isr & I2C_ISR_STOPF) != 0U)
        {
            if (LL_I2C_IsActiveFlag_RXNE(i2c) != 0U)
            {
                continue;                           /* the last byte first */
            }
            xfer_close(i2c, ICR_ALL);
            const bool done = reading ? (got == nrx) : ((sent == ntx) && (nrx == 0U));
            return done ? I2C_BM_OK : I2C_BM_ERR_BUS;
        }
        if (clk_expired(&c))
        {
            soft_reset(i2c);                        /* rule 6 */
            xfer_close(i2c, ICR_ALL);
            return I2C_BM_ERR_TIMEOUT;
        }
    }
}

I2cBm_Status_t I2C_LL_Write(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t n, uint32_t max_cycles)
{
    const I2cBm_Status_t st = I2C_BM_CheckWrite(i2c, addr7, tx, n, max_cycles);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    return xfer_poll(i2c, addr7, tx, n, NULL, 0U, max_cycles);
}

I2cBm_Status_t I2C_LL_Read(I2C_TypeDef *i2c, uint32_t addr7, uint8_t *rx, uint32_t n, uint32_t max_cycles)
{
    const I2cBm_Status_t st = I2C_BM_CheckRead(i2c, addr7, rx, n, max_cycles);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    return xfer_poll(i2c, addr7, NULL, 0U, rx, n, max_cycles);
}

I2cBm_Status_t I2C_LL_WriteRead(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t ntx,
                                uint8_t *rx, uint32_t nrx, uint32_t max_cycles)
{
    const I2cBm_Status_t st = I2C_BM_CheckWriteRead(i2c, addr7, tx, ntx, rx, nrx, max_cycles);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    return xfer_poll(i2c, addr7, tx, ntx, rx, nrx, max_cycles);
}

/* ---- Interrupt transfers (plan rule 7) --------------------------------------------- */

/* The interrupt enables of CR1 set or cleared in one read-modify-write under PRIMASK. */
static void ie_set(I2C_TypeDef *i2c, uint32_t clear, uint32_t set)
{
    const uint32_t pm = irq_save();
    LL_I2C_WriteReg(i2c, CR1, (LL_I2C_ReadReg(i2c, CR1) & ~clear) | set);
    irq_restore(pm);
}

I2cBm_Status_t I2C_LL_StartIt(I2C_TypeDef *i2c, I2cBm_Xfer_t *x)
{
    const I2cBm_Status_t st = I2C_BM_CheckStartIt(i2c, x);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    x->done_tx = 0U;
    x->done_rx = 0U;
    x->status  = I2C_BM_PENDING;
    LL_I2C_WriteReg(i2c, ICR, ICR_ALL);
    LL_I2C_ClearFlag_TXE(i2c);
    uint32_t ie = LL_I2C_CR1_TCIE | LL_I2C_CR1_STOPIE | LL_I2C_CR1_NACKIE | LL_I2C_CR1_ERRIE;
    uint32_t cr2;
    if (x->ntx != 0U)
    {
        LL_I2C_TransmitData8(i2c, x->tx[0]);        /* ES0445 2.19.6 */
        x->done_tx = 1U;
        cr2 = ((x->addr7 << 1) & I2C_CR2_SADD) | LL_I2C_ADDRSLAVE_7BIT | (chunk_size(x->ntx) << I2C_CR2_NBYTES_Pos) |
              chunk_mode(x->ntx, x->nrx == 0U);
        ie |= LL_I2C_CR1_TXIE | ((x->nrx != 0U) ? LL_I2C_CR1_RXIE : 0U);
    }
    else if (x->nrx != 0U)
    {
        cr2 = ((x->addr7 << 1) & I2C_CR2_SADD) | LL_I2C_ADDRSLAVE_7BIT | I2C_CR2_RD_WRN |
              (chunk_size(x->nrx) << I2C_CR2_NBYTES_Pos) | chunk_mode(x->nrx, true);
        ie |= LL_I2C_CR1_RXIE;
    }
    else
    {
        cr2 = ((x->addr7 << 1) & I2C_CR2_SADD) | LL_I2C_ADDRSLAVE_7BIT | LL_I2C_MODE_AUTOEND;   /* the probe */
    }
    /* CR2 first without START (one LL_I2C_WriteReg: LL_I2C_HandleTransfer with
       LL_I2C_GENERATE_NOSTARTSTOP would keep the RD_WRN of the last transfer), then
       the enables, then START (LL_I2C_GenerateStartCondition, a SET_BIT). */
    LL_I2C_WriteReg(i2c, CR2, cr2);
    ie_set(i2c, 0U, ie);
    const uint32_t pm = irq_save();
    LL_I2C_GenerateStartCondition(i2c);
    irq_restore(pm);
    return I2C_BM_OK;
}

static bool it_finish(I2C_TypeDef *i2c, I2cBm_Xfer_t *x, I2cBm_Status_t st)
{
    ie_set(i2c, IE_ALL, 0U);
    xfer_close(i2c, ICR_ALL);
    x->status = st;
    return true;
}

bool I2C_LL_XferIrq(I2C_TypeDef *i2c, I2cBm_Xfer_t *x)
{
    if (!I2C_BM_IsValidInstance(i2c) || (x == NULL))
    {
        return false;
    }
    if (x->status != I2C_BM_PENDING)
    {
        ie_set(i2c, IE_ALL, 0U);                    /* already final: nothing to serve */
        return true;
    }
    const uint32_t isr = LL_I2C_ReadReg(i2c, ISR);
    const bool reading = LL_I2C_GetTransferRequest(i2c) == LL_I2C_REQUEST_READ;
    if ((isr & I2C_ISR_BERR) != 0U)
    {
        LL_I2C_WriteReg(i2c, ICR, I2C_ICR_BERRCF);  /* ES0445 2.19.2 */
    }
    if ((isr & I2C_ISR_ARLO) != 0U)
    {
        return it_finish(i2c, x, I2C_BM_ERR_ARLO);
    }
    if ((isr & I2C_ISR_TIMEOUT) != 0U)
    {
        ie_set(i2c, IE_ALL, 0U);
        timeout_end(i2c);                           /* software reset: both lines let go */
        x->status = I2C_BM_ERR_BUS;
        return true;
    }
    /* NACKF: as i2c_bm.c. done_tx counted the bytes written to TXDR; acknowledged =
       written - 1 - (TXE ? 0 : 1); NACKIE off marks NACKF served (no RAM state). */
    if (((isr & I2C_ISR_NACKF) != 0U) && (LL_I2C_IsEnabledIT_NACK(i2c) != 0U))
    {
        if (!reading)
        {
            const uint32_t unsent = ((isr & I2C_ISR_TXE) == 0U) ? 2U : 1U;
            x->done_tx = (x->done_tx > unsent) ? (x->done_tx - unsent) : 0U;
        }
        ie_set(i2c, LL_I2C_CR1_NACKIE | LL_I2C_CR1_TXIE | LL_I2C_CR1_TCIE, 0U);
        LL_I2C_ClearFlag_TXE(i2c);
    }
    if ((isr & I2C_ISR_RXNE) != 0U)
    {
        const uint8_t b = LL_I2C_ReceiveData8(i2c);
        if (x->done_rx < x->nrx)
        {
            x->rx[x->done_rx] = b;
        }
        x->done_rx++;
    }
    const bool nacked = (isr & I2C_ISR_NACKF) != 0U;
    if (!reading && !nacked && ((isr & I2C_ISR_TXIS) != 0U) && (x->done_tx < x->ntx))
    {
        LL_I2C_TransmitData8(i2c, x->tx[x->done_tx]);
        x->done_tx++;
    }
    if (!nacked && ((isr & I2C_ISR_TCR) != 0U))
    {
        if (reading)
        {
            reload(i2c, x->nrx - x->done_rx, true);
        }
        else
        {
            reload(i2c, x->ntx - x->done_tx, x->nrx == 0U);
        }
    }
    if (!nacked && ((isr & I2C_ISR_TC) != 0U))
    {
        if (!reading && (x->nrx != 0U))
        {
            start(i2c, x->addr7, x->nrx, true, true);
            wait_cleared(i2c, I2C_ISR_TC);
        }
        else
        {
            stop(i2c);
        }
    }
    if ((isr & I2C_ISR_STOPF) != 0U)
    {
        if (LL_I2C_IsActiveFlag_RXNE(i2c) != 0U)
        {
            return false;                           /* RXNE pending: served on the next entry */
        }
        if (LL_I2C_IsActiveFlag_NACK(i2c) != 0U)
        {
            return it_finish(i2c, x, I2C_BM_ERR_NACK);
        }
        const bool done = (x->done_tx == x->ntx) && (x->done_rx == x->nrx) && (reading || (x->nrx == 0U));
        return it_finish(i2c, x, done ? I2C_BM_OK : I2C_BM_ERR_BUS);
    }
    return false;
}

/* ---- DMA (plan rule 8, 50:2300-2337) ------------------------------------------------- */

I2cBm_Status_t I2C_LL_DmaStartWrite(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *buf, uint32_t n)
{
    const I2cBm_Status_t st = I2C_BM_CheckDmaStartWrite(i2c, addr7, buf, n);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    LL_I2C_WriteReg(i2c, ICR, ICR_ALL);
    LL_I2C_ClearFlag_TXE(i2c);
    LL_I2C_TransmitData8(i2c, buf[0]);              /* ES0445 2.19.6, also with DMA */
    uint32_t pm = irq_save();
    LL_I2C_EnableDMAReq_TX(i2c);                    /* before START (50:2309-2311) */
    irq_restore(pm);
    if (LL_I2C_IsEnabledDMAReq_TX(i2c) == 0U)       /* read back before START (as i2c_bm, F1 m31) */
    {
        pm = irq_save();
        LL_I2C_DisableDMAReq_TX(i2c);
        irq_restore(pm);
        LL_I2C_ClearFlag_TXE(i2c);
        return I2C_BM_ERR_VERIFY;
    }
    start(i2c, addr7, n, true, false);
    return I2C_BM_OK;
}

I2cBm_Status_t I2C_LL_DmaStartRead(I2C_TypeDef *i2c, uint32_t addr7, uint32_t n)
{
    const I2cBm_Status_t st = I2C_BM_CheckDmaStartRead(i2c, addr7, n);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    LL_I2C_WriteReg(i2c, ICR, ICR_ALL);
    uint32_t pm = irq_save();
    LL_I2C_EnableDMAReq_RX(i2c);                    /* before START (50:2326-2328) */
    irq_restore(pm);
    if (LL_I2C_IsEnabledDMAReq_RX(i2c) == 0U)
    {
        pm = irq_save();
        LL_I2C_DisableDMAReq_RX(i2c);
        irq_restore(pm);
        return I2C_BM_ERR_VERIFY;
    }
    start(i2c, addr7, n, true, true);
    return I2C_BM_OK;
}

I2cBm_Status_t I2C_LL_WaitEnd(I2C_TypeDef *i2c, uint32_t n, uint32_t max_cycles)
{
    if (I2C_BM_CheckWaitEnd(i2c, n, max_cycles) != I2C_BM_OK)
    {
        return I2C_BM_ERR_PARAM;
    }
    if (!cyccnt_running())
    {
        soft_reset(i2c);                            /* DmaStart* already set START: end it (as i2c_bm, F1 m33) */
        return I2C_BM_ERR_TIMEOUT;
    }
    uint32_t left = (n > CHUNK) ? (n - CHUNK) : 0U; /* the first chunk is DmaStart*'s (C7) */
    Clock_t c;
    clk_start(&c, max_cycles);
    for (;;)
    {
        const uint32_t isr = LL_I2C_ReadReg(i2c, ISR);
        if ((isr & I2C_ISR_BERR) != 0U)
        {
            LL_I2C_WriteReg(i2c, ICR, I2C_ICR_BERRCF);  /* ES0445 2.19.2 */
        }
        if ((isr & I2C_ISR_ARLO) != 0U)
        {
            return I2C_BM_ERR_ARLO;
        }
        if ((isr & I2C_ISR_TIMEOUT) != 0U)
        {
            soft_reset(i2c);                        /* as timeout_end(); DmaEnd clears the flags */
            return I2C_BM_ERR_BUS;
        }
        if ((isr & I2C_ISR_NACKF) != 0U)
        {
            if (!wait_stop(i2c, &c))
            {
                soft_reset(i2c);
                return I2C_BM_ERR_TIMEOUT;
            }
            return I2C_BM_ERR_NACK;
        }
        if (((isr & I2C_ISR_TCR) != 0U) && (left != 0U))
        {
            const uint32_t chunk = chunk_size(left);
            reload(i2c, left, true);
            left -= chunk;
        }
        if ((isr & I2C_ISR_STOPF) != 0U)
        {
            return I2C_BM_OK;                       /* DmaEnd clears it */
        }
        if (clk_expired(&c))
        {
            soft_reset(i2c);                        /* rule 6; the caller stops its streams */
            return I2C_BM_ERR_TIMEOUT;
        }
    }
}

I2cBm_Status_t I2C_LL_DmaEnd(I2C_TypeDef *i2c)
{
    if (!I2C_BM_IsValidInstance(i2c))
    {
        return I2C_BM_ERR_PARAM;
    }
    const uint32_t pm = irq_save();
    LL_I2C_DisableDMAReq_TX(i2c);
    LL_I2C_DisableDMAReq_RX(i2c);
    irq_restore(pm);
    xfer_close(i2c, ICR_ALL);
    return ((LL_I2C_IsEnabledDMAReq_TX(i2c) == 0U) && (LL_I2C_IsEnabledDMAReq_RX(i2c) == 0U)) ? I2C_BM_OK
                                                                                             : I2C_BM_ERR_VERIFY;
}
