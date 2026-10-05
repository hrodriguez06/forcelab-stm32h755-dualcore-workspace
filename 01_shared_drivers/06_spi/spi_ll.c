/**
  ******************************************************************************
  * @file    spi_ll.c
  * @brief   SPI1-SPI6, LL tier. Contract in spi_ll.h.
  *
  *          Every function translates its LL constants to spi_bm values, lets
  *          SPI_BM_Check* validate them (C6: one copy of the checks and of the
  *          capability table), then writes with the LL inlines and macros of
  *          stm32h7xx_ll_spi.h in the order of the spi_bm rules and reads back.
  *          Register facts: RM0399 rev 4, chapter 53, as spi_bm.c.
  ******************************************************************************
  */

#include "spi_ll.h"

/* C8: no <stddef.h>. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* Every flag of IFCR, written once (IFCR is write-only, 53:3732). */
#define IFCR_ALL        (SPI_IFCR_EOTC | SPI_IFCR_TXTFC | SPI_IFCR_UDRC | SPI_IFCR_OVRC | SPI_IFCR_CRCEC | \
                         SPI_IFCR_TIFREC | SPI_IFCR_MODFC | SPI_IFCR_TSERFC | SPI_IFCR_SUSPC)
#define STALL_READS     1000U    /* equal CYCCNT reads that mean the counter stopped (C13) */

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
 * LL constant -> spi_bm value. An unknown constant gives a value spi_bm refuses
 * with ERR_PARAM in its field-range check, so the C6 order is spi_bm's. The
 * LL constants with only two values (polarity, phase, bit order, SS polarity)
 * map to a bool or a bit of spi_bm: an unknown one becomes mode 4.
 * ========================================================================== */

#define BM_BAD_MODE     4U       /* outside 0..3: ERR_PARAM in spi_bm */

static void to_bm(const SpiLl_Config_t *c, SpiBm_Config_t *b)
{
    bool bad = false;
    uint32_t mode = 0U;
    if (c->clock_polarity == LL_SPI_POLARITY_HIGH)
    {
        mode |= 2U;
    }
    else if (c->clock_polarity != LL_SPI_POLARITY_LOW)
    {
        bad = true;
    }
    if (c->clock_phase == LL_SPI_PHASE_2EDGE)
    {
        mode |= 1U;
    }
    else if (c->clock_phase != LL_SPI_PHASE_1EDGE)
    {
        bad = true;
    }
    if ((c->bit_order != LL_SPI_MSB_FIRST) && (c->bit_order != LL_SPI_LSB_FIRST))
    {
        bad = true;
    }
    if ((c->nss_polarity != LL_SPI_NSS_POLARITY_LOW) && (c->nss_polarity != LL_SPI_NSS_POLARITY_HIGH))
    {
        bad = true;
    }
    b->spi             = c->spi;
    b->mbr             = ((c->baudrate & ~SPI_CFG1_MBR_Msk) == 0U) ? (c->baudrate >> SPI_CFG1_MBR_Pos) : 8U;
    b->mode            = bad ? BM_BAD_MODE : mode;
    /* DSIZE = bits - 1: DATAWIDTH_4BIT .. _32BIT; DSIZE 0..2 give 1..3 bits, refused */
    b->data_bits       = ((c->data_width & ~SPI_CFG1_DSIZE_Msk) == 0U) ?
                         (((c->data_width & SPI_CFG1_DSIZE_Msk) >> SPI_CFG1_DSIZE_Pos) + 1U) : 0U;
    b->lsb_first       = (c->bit_order == LL_SPI_LSB_FIRST);
    b->direction       = (c->transfer_direction == LL_SPI_FULL_DUPLEX) ? SPI_BM_DIR_FULL_DUPLEX :
                         ((c->transfer_direction == LL_SPI_SIMPLEX_TX) ? SPI_BM_DIR_TX_ONLY : 2U);
    /* FTHLV = frames - 1 */
    b->fifo_threshold  = ((c->fifo_threshold & ~SPI_CFG1_FTHLV_Msk) == 0U) ?
                         (((c->fifo_threshold & SPI_CFG1_FTHLV_Msk) >> SPI_CFG1_FTHLV_Pos) + 1U) : 0U;
    if ((c->nss == LL_SPI_NSS_SOFT) && !c->nss_pulse)
    {
        b->nss = SPI_BM_NSS_SOFT;
    }
    else if (c->nss == LL_SPI_NSS_HARD_OUTPUT)
    {
        b->nss = c->nss_pulse ? SPI_BM_NSS_HW_PULSE : SPI_BM_NSS_HW;
    }
    else
    {
        b->nss = 3U;               /* NSS_HARD_INPUT (slave), pulse with SOFT, unknown */
    }
    b->nss_active_high = (c->nss_polarity == LL_SPI_NSS_POLARITY_HIGH);
    b->mssi            = ((c->ss_idleness & ~SPI_CFG2_MSSI_Msk) == 0U) ?
                         ((c->ss_idleness & SPI_CFG2_MSSI_Msk) >> SPI_CFG2_MSSI_Pos) : 16U;
    b->midi            = ((c->interdata_idleness & ~SPI_CFG2_MIDI_Msk) == 0U) ?
                         ((c->interdata_idleness & SPI_CFG2_MIDI_Msk) >> SPI_CFG2_MIDI_Pos) : 16U;
    b->keep_pins       = c->keep_pins;
}

SpiBm_Status_t SPI_LL_CheckConfig(const SpiLl_Config_t *cfg)
{
    if (cfg == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    SpiBm_Config_t b;
    to_bm(cfg, &b);
    return SPI_BM_CheckConfig(&b);
}

/* ==========================================================================
 * Init (plan rule 3 with R2-1), the image made of the LL constants
 * ========================================================================== */

SpiBm_Status_t SPI_LL_Init(const SpiLl_Config_t *cfg)
{
    const SpiBm_Status_t st = SPI_LL_CheckConfig(cfg);
    if (st != SPI_BM_OK)
    {
        return st;
    }
    SPI_TypeDef *spi = cfg->spi;
    /* CFG1: MBR, CRCSIZE = 00111 (ES0445 2.22.4), FTHLV, DSIZE; CRCEN, DMA, UDR at 0 */
    const uint32_t cfg1 = cfg->baudrate | LL_SPI_CRC_8BIT | cfg->fifo_threshold | cfg->data_width;
    /* CFG2: master, Motorola, IOSWP = 0, AFCNTR apart (C11) */
    const uint32_t cfg2 = LL_SPI_MODE_MASTER | LL_SPI_PROTOCOL_MOTOROLA | cfg->transfer_direction |
                          cfg->clock_polarity | cfg->clock_phase | cfg->bit_order | cfg->nss_polarity | cfg->nss |
                          (cfg->nss_pulse ? SPI_CFG2_SSOM : 0U) | cfg->ss_idleness | cfg->interdata_idleness;
    const uint32_t cfg2_final = cfg2 | (cfg->keep_pins ? SPI_CFG2_AFCNTR : 0U);
    /* SSI inactive is SSI != SSIOP (53:566-567); written with hardware SS too (R2-1) */
    const bool ssiop = (cfg->nss_polarity == LL_SPI_NSS_POLARITY_HIGH);
    const uint32_t cr1 = ssiop ? LL_SPI_SS_LEVEL_LOW : LL_SPI_SS_LEVEL_HIGH;
    const uint32_t old = LL_SPI_ReadReg(spi, CFG2);
    const bool old_afcntr = (old & SPI_CFG2_AFCNTR) != 0U;

    LL_SPI_WriteReg(spi, IER, 0U);                          /* SPE = 0 here (C12) */
    LL_SPI_WriteReg(spi, IFCR, IFCR_ALL);
    /* A master with SSM = 1 and another SSIOP leaves master first (R2-1, 53:1464-1476);
       with AFCNTR = 1 in force the SSIOP is the same (SPI_BM_CheckConfig). */
    if (!old_afcntr && ((old & (SPI_CFG2_MASTER | SPI_CFG2_SSM)) == (SPI_CFG2_MASTER | SPI_CFG2_SSM)) &&
        (((old & SPI_CFG2_SSIOP) != 0U) != ssiop))
    {
        LL_SPI_WriteReg(spi, CFG2, 0U);
    }
    LL_SPI_WriteReg(spi, CR1, cr1);                         /* MASRX = IOLOCK = 0, SPE = 0 */
    LL_SPI_WriteReg(spi, CFG1, cfg1);
    if (old_afcntr)
    {
        LL_SPI_WriteReg(spi, CFG2, cfg2_final);             /* one write: the pins stay driven (R2-1) */
    }
    else
    {
        LL_SPI_WriteReg(spi, CFG2, cfg2);                   /* polarity first, pins not driven */
        if (cfg->keep_pins)
        {
            LL_SPI_WriteReg(spi, CFG2, cfg2_final);         /* then AFCNTR: straight to idle (C11) */
        }
    }
    __DSB();
    if (LL_SPI_IsActiveFlag_MODF(spi) != 0U)
    {
        LL_SPI_WriteReg(spi, IFCR, SPI_IFCR_MODFC);
        return SPI_BM_ERR_MODF;
    }
    if ((LL_SPI_ReadReg(spi, CFG1) != cfg1) || (LL_SPI_ReadReg(spi, CFG2) != cfg2_final) ||
        (LL_SPI_ReadReg(spi, CR1) != cr1))
    {
        return SPI_BM_ERR_VERIFY;
    }
    return SPI_BM_OK;
}

/* ==========================================================================
 * Transfers (plan rules 4, 5, 7), with the LL inlines
 * ========================================================================== */

static void spe_set(SPI_TypeDef *spi)
{
    const uint32_t pm = irq_save();
    LL_SPI_Enable(spi);
    irq_restore(pm);
}

static void spe_clear(SPI_TypeDef *spi)
{
    const uint32_t pm = irq_save();
    LL_SPI_Disable(spi);
    irq_restore(pm);
}

static void cstart(SPI_TypeDef *spi)
{
    const uint32_t pm = irq_save();
    LL_SPI_StartMasterTransfer(spi);
    irq_restore(pm);
}

static void ier_write(SPI_TypeDef *spi, uint32_t ier)
{
    const uint32_t pm = irq_save();
    LL_SPI_WriteReg(spi, IER, ier);
    irq_restore(pm);
}

static bool cyccnt_running(void)
{
    return ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) != 0U) &&
           ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
}

/* What a transfer needs from the configuration in force: no RAM state (C7). */
typedef struct
{
    uint32_t es;               /* element and access bytes: 1, 2 or 4               */
    uint32_t t;                /* frames per packet (FTHLV + 1)                     */
    uint32_t cap;              /* frames the RxFIFO holds (53:927-929)              */
    bool     fd;               /* full duplex: frames come back                     */
} Geom_t;

static void geom_read(const SPI_TypeDef *spi, Geom_t *g)
{
    const uint32_t bits = ((LL_SPI_GetDataWidth(spi) & SPI_CFG1_DSIZE_Msk) >> SPI_CFG1_DSIZE_Pos) + 1U;
    SpiBm_Caps_t caps = { 0U, 0U, 0U, 0U };
    (void)SPI_BM_GetCaps(spi, &caps);           /* the instance was validated by the caller */
    const uint32_t bpf = (bits <= 8U) ? 1U : ((bits <= 16U) ? 2U : ((bits <= 24U) ? 3U : 4U));
    g->es  = (bits <= 8U) ? 1U : ((bits <= 16U) ? 2U : 4U);
    g->t   = ((LL_SPI_GetFIFOThreshold(spi) & SPI_CFG1_FTHLV_Msk) >> SPI_CFG1_FTHLV_Pos) + 1U;
    g->cap = (uint32_t)caps.fifo_bytes / bpf;
    g->fd  = LL_SPI_GetTransferDirection(spi) == LL_SPI_FULL_DUPLEX;
}

/* The data register with the width of the buffer element (53:3783; one frame per
   access, plan rule 6 with R3-2). */
static void put_frame(SPI_TypeDef *spi, const void *buf, uint32_t i, uint32_t es)
{
    if (es == 1U)
    {
        LL_SPI_TransmitData8(spi, ((const uint8_t *)buf)[i]);
    }
    else if (es == 2U)
    {
        LL_SPI_TransmitData16(spi, ((const uint16_t *)buf)[i]);
    }
    else
    {
        LL_SPI_TransmitData32(spi, ((const uint32_t *)buf)[i]);
    }
}

static void get_frame(SPI_TypeDef *spi, void *buf, uint32_t i, uint32_t es)
{
    if (es == 1U)
    {
        const uint8_t v = LL_SPI_ReceiveData8(spi);
        if (buf != NULL)
        {
            ((uint8_t *)buf)[i] = v;
        }
    }
    else if (es == 2U)
    {
        const uint16_t v = LL_SPI_ReceiveData16(spi);
        if (buf != NULL)
        {
            ((uint16_t *)buf)[i] = v;
        }
    }
    else
    {
        const uint32_t v = LL_SPI_ReceiveData32(spi);
        if (buf != NULL)
        {
            ((uint32_t *)buf)[i] = v;
        }
    }
}

/* Every complete packet the RxFIFO holds (RXP, 53:930-942). */
static void service_rx(SPI_TypeDef *spi, SpiBm_Xfer_t *x, const Geom_t *g)
{
    while ((LL_SPI_IsActiveFlag_RXP(spi) != 0U) && (x->rx_done < x->frames))
    {
        uint32_t n = x->frames - x->rx_done;
        n = (n < g->t) ? n : g->t;
        for (uint32_t k = 0U; k < n; k++)
        {
            get_frame(spi, x->rx, x->rx_done + k, g->es);
        }
        x->rx_done += n;
    }
}

/* Packets while TXP says one fits (53:947-954); in full duplex no more than the
   RxFIFO holds in flight. True when a packet is due but held back by that limit. */
static bool service_tx(SPI_TypeDef *spi, SpiBm_Xfer_t *x, const Geom_t *g)
{
    while (x->tx_done < x->frames)
    {
        uint32_t n = x->frames - x->tx_done;
        n = (n < g->t) ? n : g->t;
        if (g->fd && ((x->tx_done + n - x->rx_done) > g->cap))
        {
            return true;
        }
        if (LL_SPI_IsActiveFlag_TXP(spi) == 0U)
        {
            return false;
        }
        for (uint32_t k = 0U; k < n; k++)
        {
            put_frame(spi, x->tx, x->tx_done + k, g->es);
        }
        x->tx_done += n;
    }
    return false;
}

/* `cycles` CPU cycles from `t0`; false when CYCCNT stopped meanwhile (C13). */
static bool wait_from(uint32_t t0, uint32_t cycles)
{
    uint32_t last = DWT->CYCCNT;
    uint32_t stalled = 0U;
    while ((last - t0) < cycles)
    {
        const uint32_t now = DWT->CYCCNT;
        stalled = (now == last) ? (stalled + 1U) : 0U;
        if (stalled > STALL_READS)
        {
            return false;
        }
        last = now;
    }
    return true;
}

/* The end of a transaction (rule 4): the RxFIFO drained (RXP is not set with EOT,
   53:1197-1202), the guard from EOT (ES0445 2.22.5), IER = 0 before SPE = 0 (2.22.3),
   SPE = 0 after every EOT (2.22.1), every flag cleared in one write. */
static SpiBm_Status_t finish(SPI_TypeDef *spi, SpiBm_Xfer_t *x, const Geom_t *g, uint32_t t_eot, uint32_t guard)
{
    if (g->fd)
    {
        while (x->rx_done < x->frames)
        {
            get_frame(spi, x->rx, x->rx_done, g->es);
            x->rx_done++;
        }
    }
    const bool timed = wait_from(t_eot, guard);
    const bool ovr = LL_SPI_IsActiveFlag_OVR(spi) != 0U;
    ier_write(spi, 0U);
    spe_clear(spi);
    LL_SPI_WriteReg(spi, IFCR, IFCR_ALL);
    __DSB();
    if (!timed)
    {
        return SPI_BM_ERR_TIMEOUT;
    }
    return ovr ? SPI_BM_ERR_OVERRUN : SPI_BM_OK;
}

/* Abort (rule 7): IER = 0, SPE = 0 (the frame is cut, the RxFIFO flushed,
   53:1071-1073), every flag cleared. */
static void abort_xfer(SPI_TypeDef *spi)
{
    ier_write(spi, 0U);
    spe_clear(spi);
    LL_SPI_WriteReg(spi, IFCR, IFCR_ALL);
    __DSB();
}

/* A mode fault cleared SPE and MASTER (53:1464-1472): MODF cleared, MASTER left. */
static SpiBm_Status_t end_modf(SPI_TypeDef *spi)
{
    ier_write(spi, 0U);
    LL_SPI_WriteReg(spi, IFCR, IFCR_ALL);
    __DSB();
    return SPI_BM_ERR_MODF;
}

/* A pending mode fault found by a check, in a call that may write (rule 7). */
static SpiBm_Status_t pending_modf(SPI_TypeDef *spi, SpiBm_Status_t st)
{
    if (st == SPI_BM_ERR_MODF)
    {
        LL_SPI_WriteReg(spi, IFCR, SPI_IFCR_MODFC);
        __DSB();
    }
    return st;
}

/* TSIZE with SPE = 0 (53:3302), read back (C12); stale flags cleared. */
static bool set_tsize(SPI_TypeDef *spi, uint32_t frames)
{
    LL_SPI_WriteReg(spi, IFCR, IFCR_ALL);
    LL_SPI_SetTransferSize(spi, frames);
    return LL_SPI_GetTransferSize(spi) == frames;
}

SpiBm_Status_t SPI_LL_Transfer(SPI_TypeDef *spi, const void *tx, void *rx, uint32_t frames,
                               uint32_t guard_cycles, uint32_t max_cycles)
{
    /* guard_cycles and max_cycles are field ranges: in spi_bm they come before the
       state checks too, so the status is the same for every combination of faults. */
    SpiBm_Status_t st = ((guard_cycles == 0U) || (max_cycles == 0U)) ? SPI_BM_ERR_PARAM :
                        SPI_BM_CheckTransfer(spi, tx, rx, frames);
    if (st != SPI_BM_OK)
    {
        return (st == SPI_BM_ERR_MODF) ? pending_modf(spi, st) : st;
    }
    if (!cyccnt_running())
    {
        return SPI_BM_ERR_TIMEOUT;             /* no time base: no wait could end (C13) */
    }
    Geom_t g;
    geom_read(spi, &g);
    SpiBm_Xfer_t x = { tx, rx, frames, guard_cycles, 0U, 0U, SPI_BM_PENDING };
    const uint32_t t0 = DWT->CYCCNT;
    if (!set_tsize(spi, frames))
    {
        return SPI_BM_ERR_VERIFY;
    }
    spe_set(spi);
    (void)service_tx(spi, &x, &g);              /* preload before CSTART (rule 4) */
    cstart(spi);

    uint32_t last = t0;
    uint32_t stalled = 0U;
    for (;;)
    {
        if (LL_SPI_IsActiveFlag_MODF(spi) != 0U)
        {
            return end_modf(spi);
        }
        const bool eot = LL_SPI_IsActiveFlag_EOT(spi) != 0U;
        if (g.fd)
        {
            service_rx(spi, &x, &g);
        }
        (void)service_tx(spi, &x, &g);
        if (eot)
        {
            return finish(spi, &x, &g, DWT->CYCCNT, guard_cycles);
        }
        const uint32_t now = DWT->CYCCNT;
        stalled = (now == last) ? (stalled + 1U) : 0U;
        last = now;
        if (((now - t0) >= max_cycles) || (stalled > STALL_READS))
        {
            abort_xfer(spi);
            return SPI_BM_ERR_TIMEOUT;
        }
    }
}

/* ---- Interrupt transfer --------------------------------------------------------- */

static uint32_t ier_base(const Geom_t *g)
{
    return SPI_IER_EOTIE | SPI_IER_MODFIE | (g->fd ? SPI_IER_RXPIE : 0U);
}

SpiBm_Status_t SPI_LL_TransferStartIt(SPI_TypeDef *spi, SpiBm_Xfer_t *x)
{
    if (x == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    const SpiBm_Status_t st = (x->guard_cycles == 0U) ? SPI_BM_ERR_PARAM :
                              SPI_BM_CheckTransfer(spi, x->tx, x->rx, x->frames);
    if (st != SPI_BM_OK)
    {
        return (st == SPI_BM_ERR_MODF) ? pending_modf(spi, st) : st;
    }
    if (!cyccnt_running())
    {
        return SPI_BM_ERR_TIMEOUT;             /* the guard in the ISR needs CYCCNT (C13) */
    }
    Geom_t g;
    geom_read(spi, &g);
    x->tx_done = 0U;
    x->rx_done = 0U;
    x->status  = SPI_BM_PENDING;
    if (!set_tsize(spi, x->frames))
    {
        x->status = SPI_BM_ERR_VERIFY;
        return SPI_BM_ERR_VERIFY;
    }
    spe_set(spi);
    const bool held = service_tx(spi, x, &g);
    /* IER only while SPE = 1 (ES0445 2.22.3): written after SPE = 1 */
    ier_write(spi, ier_base(&g) | ((!held && (x->tx_done < x->frames)) ? SPI_IER_TXPIE : 0U));
    cstart(spi);
    return SPI_BM_OK;
}

bool SPI_LL_TransferIrq(SPI_TypeDef *spi, SpiBm_Xfer_t *x)
{
    if (!SPI_BM_IsValidInstance(spi) || (x == NULL))
    {
        return false;
    }
    if (x->status != SPI_BM_PENDING)
    {
        return true;                            /* already final: nothing to serve */
    }
    Geom_t g;
    geom_read(spi, &g);
    if (LL_SPI_IsActiveFlag_MODF(spi) != 0U)
    {
        x->status = end_modf(spi);
        return true;
    }
    if (g.fd)
    {
        service_rx(spi, x, &g);
    }
    const bool held = service_tx(spi, x, &g);
    if (LL_SPI_IsActiveFlag_EOT(spi) != 0U)
    {
        x->status = finish(spi, x, &g, DWT->CYCCNT, x->guard_cycles);
        return true;
    }
    /* TXPIE off while the in-flight limit holds a packet back; RXPIE brings the ISR
       back once the RxFIFO is read. */
    ier_write(spi, ier_base(&g) | ((!held && (x->tx_done < x->frames)) ? SPI_IER_TXPIE : 0U));
    return false;
}

/* ==========================================================================
 * DMA (plan rule 8, RM0399 53:1262-1274)
 * ========================================================================== */

SpiBm_Status_t SPI_LL_DmaPrepare(SPI_TypeDef *spi, uint32_t frames, bool rx)
{
    const SpiBm_Status_t st = SPI_BM_CheckDmaPrepare(spi, frames, rx);
    if (st != SPI_BM_OK)
    {
        return (st == SPI_BM_ERR_MODF) ? pending_modf(spi, st) : st;
    }
    if (!set_tsize(spi, frames))
    {
        return SPI_BM_ERR_VERIFY;
    }
    /* Step 1 of the start order: RXDMAEN before the caller enables its streams. */
    const uint32_t pm = irq_save();
    LL_SPI_DisableDMAReq_TX(spi);
    if (rx)
    {
        LL_SPI_EnableDMAReq_RX(spi);
    }
    else
    {
        LL_SPI_DisableDMAReq_RX(spi);
    }
    irq_restore(pm);
    const bool ok = (LL_SPI_IsEnabledDMAReq_TX(spi) == 0U) && ((LL_SPI_IsEnabledDMAReq_RX(spi) != 0U) == rx);
    return ok ? SPI_BM_OK : SPI_BM_ERR_VERIFY;
}

SpiBm_Status_t SPI_LL_DmaStart(SPI_TypeDef *spi)
{
    const SpiBm_Status_t st = SPI_BM_CheckDmaStart(spi);
    if (st != SPI_BM_OK)
    {
        return (st == SPI_BM_ERR_MODF) ? pending_modf(spi, st) : st;
    }
    /* Steps 3 and 4 (the caller did step 2): TXDMAEN, then SPE; then CSTART. */
    const uint32_t pm = irq_save();
    LL_SPI_EnableDMAReq_TX(spi);
    irq_restore(pm);
    spe_set(spi);
    cstart(spi);
    return (LL_SPI_IsEnabledDMAReq_TX(spi) != 0U) ? SPI_BM_OK : SPI_BM_ERR_VERIFY;
}

SpiBm_Status_t SPI_LL_WaitEnd(SPI_TypeDef *spi, uint32_t max_cycles)
{
    if (!SPI_BM_IsValidInstance(spi) || (max_cycles == 0U))
    {
        return SPI_BM_ERR_PARAM;
    }
    if (!cyccnt_running())
    {
        return SPI_BM_ERR_TIMEOUT;
    }
    const uint32_t t0 = DWT->CYCCNT;
    uint32_t last = t0;
    uint32_t stalled = 0U;
    for (;;)
    {
        if (LL_SPI_IsActiveFlag_MODF(spi) != 0U)
        {
            LL_SPI_WriteReg(spi, IFCR, SPI_IFCR_MODFC);     /* SPE and MASTER already 0 */
            return SPI_BM_ERR_MODF;
        }
        if (LL_SPI_IsActiveFlag_EOT(spi) != 0U)
        {
            return (LL_SPI_IsActiveFlag_OVR(spi) != 0U) ? SPI_BM_ERR_OVERRUN : SPI_BM_OK;
        }
        const uint32_t now = DWT->CYCCNT;
        stalled = (now == last) ? (stalled + 1U) : 0U;
        last = now;
        if (((now - t0) >= max_cycles) || (stalled > STALL_READS))
        {
            return SPI_BM_ERR_TIMEOUT;           /* no disable: the caller stops its streams first */
        }
    }
}

SpiBm_Status_t SPI_LL_DmaEnd(SPI_TypeDef *spi, uint32_t guard_cycles)
{
    if (!SPI_BM_IsValidInstance(spi) || (guard_cycles == 0U))
    {
        return SPI_BM_ERR_PARAM;
    }
    /* The closing order of 53:1268-1274: the caller stopped its streams; then the SPI
       (guard, IER = 0, SPE = 0), then TXDMAEN and RXDMAEN, then the flags. */
    const bool timed = cyccnt_running() && wait_from(DWT->CYCCNT, guard_cycles);
    ier_write(spi, 0U);
    spe_clear(spi);
    const uint32_t pm = irq_save();
    LL_SPI_DisableDMAReq_TX(spi);
    LL_SPI_DisableDMAReq_RX(spi);
    irq_restore(pm);
    LL_SPI_WriteReg(spi, IFCR, IFCR_ALL);
    __DSB();
    if ((LL_SPI_IsEnabled(spi) != 0U) || (LL_SPI_IsEnabledDMAReq_TX(spi) != 0U) ||
        (LL_SPI_IsEnabledDMAReq_RX(spi) != 0U))
    {
        return SPI_BM_ERR_VERIFY;
    }
    return timed ? SPI_BM_OK : SPI_BM_ERR_TIMEOUT;
}
