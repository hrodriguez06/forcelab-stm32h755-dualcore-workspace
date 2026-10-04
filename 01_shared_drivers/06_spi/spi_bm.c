/**
  ******************************************************************************
  * @file    spi_bm.c
  * @brief   SPI1-SPI6, bare-metal tier. Contract in spi_bm.h.
  *
  *          Sources: RM0399 rev 4, as the .txt chapters in
  *          00_docs_stm32h7/docs/peripherals/ (SPI 53_*.txt, RCC 09_*.txt,
  *          DMAMUX 18_*.txt; the line numbers below are of those files, written
  *          53:<line>, 09:<line>, 18:<line>), ES0445 rev 6 2.22 and the CMSIS
  *          device header stm32h755xx.h (interrupt numbers).
  ******************************************************************************
  */

#include "spi_bm.h"

/* C8: stm32h7xx.h, <stdint.h> and <stdbool.h> only, so no <stddef.h>. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* ==========================================================================
 * Capability table (C1, plan rule 2): one row per instance, the only place the
 * driver learns a silicon fact. Every validation below reads it.
 * ========================================================================== */

#define BUS_APB1L   0U          /* RCC_APB1LENR (this core's bank) */
#define BUS_APB2    1U          /* RCC_APB2ENR                     */
#define BUS_APB4    2U          /* RCC_APB4ENR                     */

typedef struct
{
    uint32_t  base;
    uint8_t   bus;             /* BUS_*                                             */
    uint8_t   rcc_bit;         /* enable bit in that register                       */
    uint8_t   max_bits;        /* largest frame (and CRC) size                      */
    uint8_t   fifo_bytes;      /* RxFIFO and TxFIFO size, bytes each                */
    uint8_t   ksel;            /* SPI_BM_KSEL_*                                     */
    uint8_t   dmamux;          /* 1: DMAMUX1 (DMA1/DMA2), 2: DMAMUX2 (BDMA)          */
    uint8_t   req_rx;          /* DMAMUX request of the RX stream                   */
    uint8_t   req_tx;          /* DMAMUX request of the TX stream                   */
    IRQn_Type irq;
} SpiEntry_t;

/* Frame and FIFO sizes: table 435 (53:53-71: FIFO 53:56, data and CRC size 53:68-69).
   RCC: §9.7.44 RCC_APB1LENR (09:7106, SPI3EN 09:7277, SPI2EN 09:7283), §9.7.46
   RCC_APB2ENR (09:7419, SPI5EN 09:7516, SPI4EN 09:7536, SPI1EN 09:7542), §9.7.47
   RCC_APB4ENR (09:7577, SPI6EN 09:7690). Kernel clock: SPI123SEL 09:4653, SPI45SEL
   09:4643 (RCC_D2CCIP1R), SPI6SEL 09:4802 (RCC_D3CCIPR). DMAMUX1, table 126
   (18:98-99, 18:127-133); DMAMUX2, table 130 (18:233-234). IRQn: stm32h755xx.h
   (SPI1 35, SPI2 36, SPI3 51, SPI4 84, SPI5 85, SPI6 86). */
static const SpiEntry_t k_spi[] = {
    /* base       bus        bit  bits fifo ksel                 mux rx  tx   irq       */
    { SPI1_BASE, BUS_APB2,  12U, 32U, 16U, SPI_BM_KSEL_SPI123, 1U, 37U, 38U, SPI1_IRQn },
    { SPI2_BASE, BUS_APB1L, 14U, 32U, 16U, SPI_BM_KSEL_SPI123, 1U, 39U, 40U, SPI2_IRQn },
    { SPI3_BASE, BUS_APB1L, 15U, 32U, 16U, SPI_BM_KSEL_SPI123, 1U, 61U, 62U, SPI3_IRQn },
    { SPI4_BASE, BUS_APB2,  13U, 16U,  8U, SPI_BM_KSEL_SPI45,  1U, 83U, 84U, SPI4_IRQn },
    { SPI5_BASE, BUS_APB2,  20U, 16U,  8U, SPI_BM_KSEL_SPI45,  1U, 85U, 86U, SPI5_IRQn },
    { SPI6_BASE, BUS_APB4,   5U, 16U,  8U, SPI_BM_KSEL_SPI6,   2U, 11U, 12U, SPI6_IRQn },
};
#define SPI_COUNT   ((uint32_t)(sizeof(k_spi) / sizeof(k_spi[0])))

/* ---- Register facts (RM0399 53.11) ----------------------------------------------- */
#define CFG1_RESET      0x00070007UL                        /* 53:3309 */
#define CFG1_CRCSIZE_8  (7UL << SPI_CFG1_CRCSIZE_Pos)        /* 00111: ES0445 2.22.4, 53:3364-3365 */
#define CRCPOLY_RESET   0x00000107UL                        /* 53:3815 */
#define IFCR_ALL        (SPI_IFCR_EOTC | SPI_IFCR_TXTFC | SPI_IFCR_UDRC | SPI_IFCR_OVRC | SPI_IFCR_CRCEC | \
                         SPI_IFCR_TIFREC | SPI_IFCR_MODFC | SPI_IFCR_TSERFC | SPI_IFCR_SUSPC)
#define TSIZE_MAX       0xFFFFUL                            /* CR2.TSIZE[15:0], 53:3285 */

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

static void cr1_set(SPI_TypeDef *spi, uint32_t bits)
{
    const uint32_t pm = irq_save();
    spi->CR1 |= bits;
    irq_restore(pm);
}

static void cr1_clear(SPI_TypeDef *spi, uint32_t bits)
{
    const uint32_t pm = irq_save();
    spi->CR1 &= ~bits;
    irq_restore(pm);
}

static void cfg1_update(SPI_TypeDef *spi, uint32_t clear, uint32_t set)
{
    const uint32_t pm = irq_save();
    spi->CFG1 = (spi->CFG1 & ~clear) | set;
    irq_restore(pm);
}

static void ier_write(SPI_TypeDef *spi, uint32_t ier)
{
    const uint32_t pm = irq_save();
    spi->IER = ier;
    irq_restore(pm);
}

/* ==========================================================================
 * Validation (C6): pure, no peripheral access
 * ========================================================================== */

static const SpiEntry_t *entry(const SPI_TypeDef *spi)
{
    if (spi == NULL)
    {
        return NULL;
    }
    for (uint32_t i = 0U; i < SPI_COUNT; i++)
    {
        if (k_spi[i].base == (uint32_t)spi)
        {
            return &k_spi[i];
        }
    }
    return NULL;
}

/* Bytes of the buffer element, and of the data register access, per frame. */
static uint32_t elem_bytes(uint32_t bits)
{
    return (bits <= 8U) ? 1U : ((bits <= 16U) ? 2U : 4U);
}

/* Bytes a frame takes in the FIFO: the RxFIFO holds N, N/2, N/3 or N/4 frames of up
   to 8, 16, 24 and 32 bits (53:927-929). */
static uint32_t fifo_bytes_per_frame(uint32_t bits)
{
    return (bits <= 8U) ? 1U : ((bits <= 16U) ? 2U : ((bits <= 24U) ? 3U : 4U));
}

bool SPI_BM_IsValidInstance(const SPI_TypeDef *spi)
{
    return entry(spi) != NULL;
}

SpiBm_Status_t SPI_BM_GetCaps(const SPI_TypeDef *spi, SpiBm_Caps_t *out)
{
    const SpiEntry_t *e = entry(spi);
    if ((out == NULL) || (e == NULL))
    {
        return SPI_BM_ERR_PARAM;
    }
    out->max_data_bits = e->max_bits;
    out->fifo_bytes    = e->fifo_bytes;
    out->kernel_select = e->ksel;
    out->dmamux        = e->dmamux;
    return SPI_BM_OK;
}

/* ==========================================================================
 * Clock
 * ========================================================================== */

/* This core's enable register of the instance: RCC->APB1LENR / APB2ENR / APB4ENR
   sit in the per-core bank RM0399 9.7.1 maps at one address: CPU1 lands in RCC_C1,
   CPU2 in RCC_C2, and the peripheral runs while either bank enables it (9.5.10). */
static volatile uint32_t *enr_reg(const SpiEntry_t *e)
{
    if (e->bus == BUS_APB1L)
    {
        return &RCC->APB1LENR;
    }
    return (e->bus == BUS_APB2) ? &RCC->APB2ENR : &RCC->APB4ENR;
}

SpiBm_Status_t SPI_BM_EnableClock(SPI_TypeDef *spi)
{
    const SpiEntry_t *e = entry(spi);
    if (e == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    volatile uint32_t *reg = enr_reg(e);
    const uint32_t bit = 1UL << e->rcc_bit;
    const uint32_t pm = irq_save();
    *reg |= bit;
    const uint32_t rb = *reg;                  /* the read that lets the enable land */
    irq_restore(pm);
    return ((rb & bit) != 0U) ? SPI_BM_OK : SPI_BM_ERR_VERIFY;
}

SpiBm_Status_t SPI_BM_DisableClock(SPI_TypeDef *spi)
{
    const SpiEntry_t *e = entry(spi);
    if (e == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    volatile uint32_t *reg = enr_reg(e);
    const uint32_t bit = 1UL << e->rcc_bit;
    const uint32_t pm = irq_save();
    *reg &= ~bit;
    const uint32_t rb = *reg;
    irq_restore(pm);
    return ((rb & bit) == 0U) ? SPI_BM_OK : SPI_BM_ERR_VERIFY;
}

/* The raw kernel clock field of the instance; never written (C10). */
SpiBm_Status_t SPI_BM_GetKernelSource(const SPI_TypeDef *spi, uint32_t *field)
{
    const SpiEntry_t *e = entry(spi);
    if ((field == NULL) || (e == NULL))
    {
        return SPI_BM_ERR_PARAM;
    }
    if (e->ksel == SPI_BM_KSEL_SPI123)
    {
        *field = (RCC->D2CCIP1R & RCC_D2CCIP1R_SPI123SEL_Msk) >> RCC_D2CCIP1R_SPI123SEL_Pos;
    }
    else if (e->ksel == SPI_BM_KSEL_SPI45)
    {
        *field = (RCC->D2CCIP1R & RCC_D2CCIP1R_SPI45SEL_Msk) >> RCC_D2CCIP1R_SPI45SEL_Pos;
    }
    else
    {
        *field = (RCC->D3CCIPR & RCC_D3CCIPR_SPI6SEL_Msk) >> RCC_D3CCIPR_SPI6SEL_Pos;
    }
    return SPI_BM_OK;
}

/* ==========================================================================
 * Pure arithmetic (C15, plan "Reglas de cálculo")
 * ========================================================================== */

SpiBm_Status_t SPI_BM_CalcPrescaler(uint32_t kernel_hz, uint32_t max_sck_hz,
                                    uint32_t *mbr, uint32_t *sck_hz)
{
    if ((mbr == NULL) || (sck_hz == NULL) || (kernel_hz == 0U) || (max_sck_hz == 0U))
    {
        return SPI_BM_ERR_PARAM;
    }
    for (uint32_t m = 0U; m <= 7U; m++)
    {
        if ((uint64_t)kernel_hz <= ((uint64_t)max_sck_hz << (m + 1U)))
        {
            *mbr = m;
            *sck_hz = kernel_hz >> (m + 1U);
            return SPI_BM_OK;
        }
    }
    return SPI_BM_ERR_RANGE;
}

SpiBm_Status_t SPI_BM_CalcGuardCycles(uint32_t cpu_hz, uint32_t kernel_hz, uint32_t mbr,
                                      uint32_t *cycles)
{
    if ((cycles == NULL) || (cpu_hz == 0U) || (kernel_hz == 0U) || (mbr > 7U))
    {
        return SPI_BM_ERR_PARAM;
    }
    const uint64_t num = (uint64_t)cpu_hz << (mbr + 1U);
    const uint64_t c = (num + (uint64_t)kernel_hz - 1U) / (uint64_t)kernel_hz;
    if (c > 0xFFFFFFFFULL)
    {
        return SPI_BM_ERR_RANGE;
    }
    *cycles = (uint32_t)c;
    return SPI_BM_OK;
}

/* ==========================================================================
 * Configuration
 * ========================================================================== */

static SpiBm_Status_t chk_config(const SpiBm_Config_t *cfg, const SpiEntry_t **pe)
{
    if (cfg == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    const SpiEntry_t *e = entry(cfg->spi);
    if (e == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    /* Capability: a frame size that exists, but not on this instance. */
    if ((cfg->data_bits >= 4U) && (cfg->data_bits <= 32U) && (cfg->data_bits > e->max_bits))
    {
        return SPI_BM_ERR_UNSUPPORTED;
    }
    /* Field range. */
    if ((cfg->mbr > 7U) || (cfg->mode > 3U) || (cfg->data_bits < 4U) || (cfg->data_bits > 32U) ||
        (cfg->direction > SPI_BM_DIR_TX_ONLY) || (cfg->nss > SPI_BM_NSS_HW_PULSE) ||
        (cfg->mssi > 15U) || (cfg->midi > 15U) || (cfg->fifo_threshold < 1U) || (cfg->fifo_threshold > 16U))
    {
        return SPI_BM_ERR_PARAM;
    }
    if ((cfg->nss == SPI_BM_NSS_SOFT) && (cfg->mssi != 0U))
    {
        return SPI_BM_ERR_PARAM;              /* MSSI acts with SSOE = 1 only (53:3536-3541) */
    }
    /* A packet must not exceed half of the FIFO (53:3394-3395). */
    if ((cfg->fifo_threshold * fifo_bytes_per_frame(cfg->data_bits)) > ((uint32_t)e->fifo_bytes / 2U))
    {
        return SPI_BM_ERR_RANGE;
    }
    /* State: what the hardware would ignore (C12; 53:3273-3275 and IOLOCK, 53:3200-3206),
       and a CFG2 in force with AFCNTR = 1 that the new configuration would glitch
       (plan C11 and R2-1). Reads only. */
    const SPI_TypeDef *spi = cfg->spi;
    const uint32_t cr1  = spi->CR1;
    const uint32_t cfg2 = spi->CFG2;
    if ((cr1 & (SPI_CR1_SPE | SPI_CR1_IOLOCK)) != 0U)
    {
        return SPI_BM_ERR_BUSY;
    }
    if ((cfg2 & SPI_CFG2_AFCNTR) != 0U)
    {
        const bool cpol = (cfg->mode & 2U) != 0U;
        if ((((cfg2 & SPI_CFG2_CPOL) != 0U) != cpol) || (((cfg2 & SPI_CFG2_SSIOP) != 0U) != cfg->nss_active_high))
        {
            return SPI_BM_ERR_BUSY;
        }
    }
    if (pe != NULL)
    {
        *pe = e;
    }
    return SPI_BM_OK;
}

SpiBm_Status_t SPI_BM_CheckConfig(const SpiBm_Config_t *cfg)
{
    return chk_config(cfg, NULL);
}

static uint32_t cfg1_of(const SpiBm_Config_t *cfg)
{
    return (cfg->mbr << SPI_CFG1_MBR_Pos) | CFG1_CRCSIZE_8 |
           ((cfg->fifo_threshold - 1U) << SPI_CFG1_FTHLV_Pos) | ((cfg->data_bits - 1U) << SPI_CFG1_DSIZE_Pos);
}

/* CFG2 without AFCNTR: master, Motorola (SP = 000), IOSWP = 0. */
static uint32_t cfg2_of(const SpiBm_Config_t *cfg)
{
    uint32_t v = SPI_CFG2_MASTER | (cfg->direction << SPI_CFG2_COMM_Pos) |
                 (cfg->midi << SPI_CFG2_MIDI_Pos) | (cfg->mssi << SPI_CFG2_MSSI_Pos);
    if ((cfg->mode & 2U) != 0U)
    {
        v |= SPI_CFG2_CPOL;
    }
    if ((cfg->mode & 1U) != 0U)
    {
        v |= SPI_CFG2_CPHA;
    }
    if (cfg->lsb_first)
    {
        v |= SPI_CFG2_LSBFRST;
    }
    if (cfg->nss_active_high)
    {
        v |= SPI_CFG2_SSIOP;
    }
    if (cfg->nss == SPI_BM_NSS_SOFT)
    {
        v |= SPI_CFG2_SSM;
    }
    else
    {
        v |= SPI_CFG2_SSOE;
        if (cfg->nss == SPI_BM_NSS_HW_PULSE)
        {
            v |= SPI_CFG2_SSOM;
        }
    }
    return v;
}

SpiBm_Status_t SPI_BM_Init(const SpiBm_Config_t *cfg)
{
    const SpiEntry_t *e = NULL;
    const SpiBm_Status_t st = chk_config(cfg, &e);
    if (st != SPI_BM_OK)
    {
        return st;
    }
    (void)e;
    SPI_TypeDef *spi = cfg->spi;
    const uint32_t cfg1 = cfg1_of(cfg);
    const uint32_t cfg2 = cfg2_of(cfg);
    /* SSI inactive is SSI != SSIOP (53:566-567); written with hardware SS too, where
       SSI does not act (R2-1). */
    const uint32_t cr1 = cfg->nss_active_high ? 0U : SPI_CR1_SSI;
    const uint32_t old = spi->CFG2;
    const bool old_afcntr = (old & SPI_CFG2_AFCNTR) != 0U;

    spi->IER  = 0U;                                         /* SPE = 0 here (C12) */
    spi->IFCR = IFCR_ALL;
    /* A master with SSM = 1 whose SS input would read active once SSI follows the new
       SSIOP is a mode fault (53:1464-1476): leave master first (R2-1). With AFCNTR = 1
       in force the SSIOP is the same (chk_config), so this never releases the pins. */
    if (!old_afcntr && ((old & (SPI_CFG2_MASTER | SPI_CFG2_SSM)) == (SPI_CFG2_MASTER | SPI_CFG2_SSM)) &&
        (((old & SPI_CFG2_SSIOP) != 0U) != cfg->nss_active_high))
    {
        spi->CFG2 = 0U;
    }
    spi->CR1  = cr1;                                        /* MASRX = IOLOCK = 0, SPE = 0 */
    spi->CFG1 = cfg1;
    uint32_t cfg2_final = cfg2;
    if (cfg->keep_pins)
    {
        cfg2_final |= SPI_CFG2_AFCNTR;
    }
    if (old_afcntr)
    {
        spi->CFG2 = cfg2_final;                             /* one write: the pins stay driven (R2-1) */
    }
    else
    {
        spi->CFG2 = cfg2;                                   /* polarity first, pins not driven */
        if (cfg->keep_pins)
        {
            spi->CFG2 = cfg2_final;                         /* then AFCNTR: straight to idle (C11) */
        }
    }
    __DSB();
    if ((spi->SR & SPI_SR_MODF) != 0U)
    {
        spi->IFCR = SPI_IFCR_MODFC;
        return SPI_BM_ERR_MODF;
    }
    if ((spi->CFG1 != cfg1) || (spi->CFG2 != cfg2_final) || (spi->CR1 != cr1))
    {
        return SPI_BM_ERR_VERIFY;
    }
    return SPI_BM_OK;
}

SpiBm_Status_t SPI_BM_Deinit(SPI_TypeDef *spi)
{
    if (entry(spi) == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    /* Plan rule 9 with R2-2: IER = 0 before SPE = 0 (ES0445 2.22.3); SSI kept while
       CFG2 is still a master's, CFG2 = 0 (AFCNTR = 0 releases the pins; a slave has no
       mode fault, 53:1475-1476), then CR1 = 0. */
    const uint32_t pm = irq_save();
    spi->IER  = 0U;
    spi->CR1  = spi->CR1 & SPI_CR1_SSI;
    irq_restore(pm);
    spi->CFG2 = 0U;
    spi->CR1  = 0U;
    spi->CR2  = 0U;
    spi->CFG1 = CFG1_RESET;
    spi->IFCR = IFCR_ALL;
    __DSB();
    const bool ok = (spi->CR1 == 0U) && (spi->CR2 == 0U) && (spi->CFG1 == CFG1_RESET) && (spi->CFG2 == 0U) &&
                    (spi->IER == 0U) && ((spi->SR & SPI_SR_MODF) == 0U) && (spi->CRCPOLY == CRCPOLY_RESET) &&
                    (spi->UDRDR == 0U) && (spi->I2SCFGR == 0U);
    return ok ? SPI_BM_OK : SPI_BM_ERR_VERIFY;
}

/* ==========================================================================
 * Transfers
 * ========================================================================== */

/* CYCCNT is counting: trace enabled in DEMCR and CYCCNTENA in DWT_CTRL. */
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
    uint32_t cap;              /* frames the RxFIFO holds                           */
    bool     fd;               /* full duplex: frames come back                     */
} Geom_t;

static void geom_read(const SPI_TypeDef *spi, const SpiEntry_t *e, Geom_t *g)
{
    const uint32_t cfg1 = spi->CFG1;
    const uint32_t bits = ((cfg1 & SPI_CFG1_DSIZE_Msk) >> SPI_CFG1_DSIZE_Pos) + 1U;
    g->es  = elem_bytes(bits);
    g->t   = ((cfg1 & SPI_CFG1_FTHLV_Msk) >> SPI_CFG1_FTHLV_Pos) + 1U;
    g->cap = (uint32_t)e->fifo_bytes / fifo_bytes_per_frame(bits);
    g->fd  = (spi->CFG2 & SPI_CFG2_COMM_Msk) == 0U;
}

/* The checks of a transfer, in the C6 order; `extra_ok` is the caller's own field
   range (guard_cycles, max_cycles), checked before the state. Reads CFG1, CFG2, CR1
   and SR, writes nothing. */
static SpiBm_Status_t chk_transfer(const SPI_TypeDef *spi, const void *tx, const void *rx, uint32_t frames,
                                   bool extra_ok, const SpiEntry_t **pe)
{
    if ((spi == NULL) || (tx == NULL))
    {
        return SPI_BM_ERR_PARAM;
    }
    const SpiEntry_t *e = entry(spi);
    if (e == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    if ((frames == 0U) || (frames > TSIZE_MAX) || !extra_ok)
    {
        return SPI_BM_ERR_PARAM;
    }
    const uint32_t bits = ((spi->CFG1 & SPI_CFG1_DSIZE_Msk) >> SPI_CFG1_DSIZE_Pos) + 1U;
    const uintptr_t es = elem_bytes(bits);
    if ((((uintptr_t)tx % es) != 0U) || ((rx != NULL) && (((uintptr_t)rx % es) != 0U)))
    {
        return SPI_BM_ERR_PARAM;
    }
    const uint32_t cfg2 = spi->CFG2;
    if ((rx != NULL) && ((cfg2 & SPI_CFG2_COMM_Msk) != 0U))
    {
        return SPI_BM_ERR_PARAM;                /* TX_ONLY receives nothing */
    }
    if ((spi->SR & SPI_SR_MODF) != 0U)
    {
        return SPI_BM_ERR_MODF;                 /* a mode fault in force: SPE cannot be set (53:3280) */
    }
    if (((spi->CR1 & SPI_CR1_SPE) != 0U) || ((cfg2 & SPI_CFG2_MASTER) == 0U))
    {
        return SPI_BM_ERR_BUSY;                 /* TSIZE needs SPE = 0 (53:3302); not a master: no Init */
    }
    if (pe != NULL)
    {
        *pe = e;
    }
    return SPI_BM_OK;
}

/* A pending mode fault found by a check, in a call that may write: MODF cleared, MASTER
   left as the hardware left it (plan rule 7). */
static SpiBm_Status_t pending_modf(SPI_TypeDef *spi, SpiBm_Status_t st)
{
    if (st == SPI_BM_ERR_MODF)
    {
        spi->IFCR = SPI_IFCR_MODFC;
        __DSB();
    }
    return st;
}

SpiBm_Status_t SPI_BM_CheckTransfer(const SPI_TypeDef *spi, const void *tx, const void *rx,
                                    uint32_t frames)
{
    return chk_transfer(spi, tx, rx, frames, true, NULL);
}

/* TXDR and RXDR accessed with the width of the buffer element (53:3783: never less
   than the frame; with frames of up to 8 bits an 8-bit access holds one frame,
   53:1158-1165). The narrow accesses take the address as the instance base plus the
   register offset (TXDR 0x20, 53:3762; RXDR 0x30, 53:3786): a byte or half-word
   access to the register, without type punning a member of SPI_TypeDef. */
#define TXDR_OFFSET   0x20U
#define RXDR_OFFSET   0x30U
#define DR8(spi, ofs)   (*(volatile uint8_t *)((uintptr_t)(spi) + (ofs)))
#define DR16(spi, ofs)  (*(volatile uint16_t *)((uintptr_t)(spi) + (ofs)))

static void put_frame(SPI_TypeDef *spi, const void *buf, uint32_t i, uint32_t es)
{
    if (es == 1U)
    {
        DR8(spi, TXDR_OFFSET) = ((const uint8_t *)buf)[i];
    }
    else if (es == 2U)
    {
        DR16(spi, TXDR_OFFSET) = ((const uint16_t *)buf)[i];
    }
    else
    {
        spi->TXDR = ((const uint32_t *)buf)[i];
    }
}

static void get_frame(SPI_TypeDef *spi, void *buf, uint32_t i, uint32_t es)
{
    if (es == 1U)
    {
        const uint8_t v = DR8(spi, RXDR_OFFSET);
        if (buf != NULL)
        {
            ((uint8_t *)buf)[i] = v;
        }
    }
    else if (es == 2U)
    {
        const uint16_t v = DR16(spi, RXDR_OFFSET);
        if (buf != NULL)
        {
            ((uint16_t *)buf)[i] = v;
        }
    }
    else
    {
        const uint32_t v = spi->RXDR;
        if (buf != NULL)
        {
            ((uint32_t *)buf)[i] = v;
        }
    }
}

/* Reads every complete packet the RxFIFO holds (RXP, 53:930-942). */
static void service_rx(SPI_TypeDef *spi, SpiBm_Xfer_t *x, const Geom_t *g)
{
    while (((spi->SR & SPI_SR_RXP) != 0U) && (x->rx_done < x->frames))
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

/* Writes packets while TXP says one fits (53:947-954). In full duplex no more than
   the RxFIFO holds is in flight, so the RxFIFO cannot overrun even if it is read
   late. Returns true when a packet is due but held back by that limit. */
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
        if ((spi->SR & SPI_SR_TXP) == 0U)
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

/* Successive equal CYCCNT reads that mean the counter stopped (C13: no loop without
   an exit, even if CYCCNT stops after the check at the start of a call). */
#define STALL_READS     1000U

/* Waits `cycles` CPU cycles counted from `t0`. False when CYCCNT stopped meanwhile
   (audit F1, m1): the wait could not be measured. */
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

/* The end of a transaction (plan rule 4): after EOT, the frames left in the RxFIFO
   (fewer than a packet: RXP is not set with EOT, 53:1197-1202), the guard of ES0445
   2.22.5 counted from EOT, IER = 0 before SPE = 0 (2.22.3), SPE = 0 after every EOT
   (2.22.1), every flag cleared. OVR, sticky since the start, decides the status; a
   guard CYCCNT could not count is ERR_TIMEOUT, after the same disable. */
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
    const bool ovr = (spi->SR & SPI_SR_OVR) != 0U;
    ier_write(spi, 0U);
    cr1_clear(spi, SPI_CR1_SPE);
    spi->IFCR = IFCR_ALL;
    __DSB();
    if (!timed)
    {
        return SPI_BM_ERR_TIMEOUT;
    }
    return ovr ? SPI_BM_ERR_OVERRUN : SPI_BM_OK;
}

/* Abort (plan rule 7): IER = 0, SPE = 0 (the frame in progress is cut and the RxFIFO
   flushed, 53:1071-1073), every flag cleared. */
static void abort_xfer(SPI_TypeDef *spi)
{
    ier_write(spi, 0U);
    cr1_clear(spi, SPI_CR1_SPE);
    spi->IFCR = IFCR_ALL;
    __DSB();
}

/* A mode fault already cleared SPE and MASTER (53:1464-1472): MODF is cleared, MASTER
   left as the hardware left it, so a new Init is needed (plan rule 7). */
static SpiBm_Status_t end_modf(SPI_TypeDef *spi)
{
    ier_write(spi, 0U);
    spi->IFCR = IFCR_ALL;
    __DSB();
    return SPI_BM_ERR_MODF;
}

/* TSIZE with SPE = 0 (53:3302), read back (C12); stale flags cleared. */
static bool set_tsize(SPI_TypeDef *spi, uint32_t frames)
{
    spi->IFCR = IFCR_ALL;
    spi->CR2 = frames;
    return (spi->CR2 & SPI_CR2_TSIZE_Msk) == frames;
}

SpiBm_Status_t SPI_BM_Transfer(SPI_TypeDef *spi, const void *tx, void *rx, uint32_t frames,
                               uint32_t guard_cycles, uint32_t max_cycles)
{
    const SpiEntry_t *e = NULL;
    const SpiBm_Status_t st = chk_transfer(spi, tx, rx, frames, (guard_cycles >= 1U) && (max_cycles >= 1U), &e);
    if (st != SPI_BM_OK)
    {
        return pending_modf(spi, st);
    }
    if (!cyccnt_running())
    {
        return SPI_BM_ERR_TIMEOUT;             /* no time base: no wait could end (C13) */
    }
    Geom_t g;
    geom_read(spi, e, &g);
    SpiBm_Xfer_t x = { tx, rx, frames, guard_cycles, 0U, 0U, SPI_BM_PENDING };
    const uint32_t t0 = DWT->CYCCNT;
    if (!set_tsize(spi, frames))
    {
        return SPI_BM_ERR_VERIFY;
    }
    cr1_set(spi, SPI_CR1_SPE);
    (void)service_tx(spi, &x, &g);              /* preload before CSTART (plan rule 4) */
    cr1_set(spi, SPI_CR1_CSTART);

    uint32_t last = t0;
    uint32_t stalled = 0U;
    for (;;)
    {
        const uint32_t sr = spi->SR;
        if ((sr & SPI_SR_MODF) != 0U)
        {
            return end_modf(spi);
        }
        if (g.fd)
        {
            service_rx(spi, &x, &g);
        }
        (void)service_tx(spi, &x, &g);
        if ((sr & SPI_SR_EOT) != 0U)
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

SpiBm_Status_t SPI_BM_TransferStartIt(SPI_TypeDef *spi, SpiBm_Xfer_t *x)
{
    if (x == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    const SpiEntry_t *e = NULL;
    const SpiBm_Status_t st = chk_transfer(spi, x->tx, x->rx, x->frames, x->guard_cycles >= 1U, &e);
    if (st != SPI_BM_OK)
    {
        return pending_modf(spi, st);
    }
    if (!cyccnt_running())
    {
        return SPI_BM_ERR_TIMEOUT;             /* the guard in the ISR needs CYCCNT (C13) */
    }
    Geom_t g;
    geom_read(spi, e, &g);
    x->tx_done = 0U;
    x->rx_done = 0U;
    x->status  = SPI_BM_PENDING;
    if (!set_tsize(spi, x->frames))
    {
        x->status = SPI_BM_ERR_VERIFY;
        return SPI_BM_ERR_VERIFY;
    }
    cr1_set(spi, SPI_CR1_SPE);
    const bool held = service_tx(spi, x, &g);
    /* IER only while SPE = 1 (ES0445 2.22.3): written after SPE = 1. TXPIE while a
       packet can still go; TXTF clears it by hardware once all are pushed (53:3685). */
    ier_write(spi, ier_base(&g) | ((!held && (x->tx_done < x->frames)) ? SPI_IER_TXPIE : 0U));
    cr1_set(spi, SPI_CR1_CSTART);
    return SPI_BM_OK;
}

bool SPI_BM_TransferIrq(SPI_TypeDef *spi, SpiBm_Xfer_t *x)
{
    const SpiEntry_t *e = entry(spi);
    if ((e == NULL) || (x == NULL))
    {
        return false;
    }
    if (x->status != SPI_BM_PENDING)
    {
        return true;                            /* already final: nothing to serve */
    }
    Geom_t g;
    geom_read(spi, e, &g);
    const uint32_t sr = spi->SR;
    if ((sr & SPI_SR_MODF) != 0U)
    {
        x->status = end_modf(spi);
        return true;
    }
    if (g.fd)
    {
        service_rx(spi, x, &g);
    }
    const bool held = service_tx(spi, x, &g);
    if ((spi->SR & SPI_SR_EOT) != 0U)
    {
        x->status = finish(spi, x, &g, DWT->CYCCNT, x->guard_cycles);
        return true;
    }
    /* TXPIE off while the in-flight limit holds a packet back (else TXP would fire
       without end); RXPIE brings the ISR back once the RxFIFO is read. */
    ier_write(spi, ier_base(&g) | ((!held && (x->tx_done < x->frames)) ? SPI_IER_TXPIE : 0U));
    return false;
}

/* ---- DMA (plan rule 8, RM0399 53:1262-1274) --------------------------------------- */

SpiBm_Status_t SPI_BM_DmaPrepare(SPI_TypeDef *spi, uint32_t frames, bool rx)
{
    if (entry(spi) == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    if ((frames == 0U) || (frames > TSIZE_MAX))
    {
        return SPI_BM_ERR_PARAM;
    }
    const uint32_t cfg2 = spi->CFG2;
    if (rx && ((cfg2 & SPI_CFG2_COMM_Msk) != 0U))
    {
        return SPI_BM_ERR_PARAM;                /* TX_ONLY receives nothing */
    }
    if ((spi->SR & SPI_SR_MODF) != 0U)
    {
        return pending_modf(spi, SPI_BM_ERR_MODF);
    }
    if (((spi->CR1 & SPI_CR1_SPE) != 0U) || ((cfg2 & SPI_CFG2_MASTER) == 0U))
    {
        return SPI_BM_ERR_BUSY;
    }
    if (!set_tsize(spi, frames))
    {
        return SPI_BM_ERR_VERIFY;
    }
    /* Step 1 of the start order: RXDMAEN before the caller enables its streams. */
    cfg1_update(spi, SPI_CFG1_TXDMAEN | SPI_CFG1_RXDMAEN, rx ? SPI_CFG1_RXDMAEN : 0U);
    const uint32_t dma = spi->CFG1 & (SPI_CFG1_TXDMAEN | SPI_CFG1_RXDMAEN);
    return (dma == (rx ? SPI_CFG1_RXDMAEN : 0U)) ? SPI_BM_OK : SPI_BM_ERR_VERIFY;
}

SpiBm_Status_t SPI_BM_DmaStart(SPI_TypeDef *spi)
{
    if (entry(spi) == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    if ((spi->SR & SPI_SR_MODF) != 0U)
    {
        return pending_modf(spi, SPI_BM_ERR_MODF);
    }
    if (((spi->CR1 & SPI_CR1_SPE) != 0U) || ((spi->CFG2 & SPI_CFG2_MASTER) == 0U) ||
        ((spi->CR2 & SPI_CR2_TSIZE_Msk) == 0U))
    {
        return SPI_BM_ERR_BUSY;                 /* running, not a master, or not prepared */
    }
    /* Steps 3 and 4 (the caller did step 2): TXDMAEN, then SPE; then CSTART. */
    cfg1_update(spi, 0U, SPI_CFG1_TXDMAEN);
    cr1_set(spi, SPI_CR1_SPE);
    cr1_set(spi, SPI_CR1_CSTART);
    return ((spi->CFG1 & SPI_CFG1_TXDMAEN) != 0U) ? SPI_BM_OK : SPI_BM_ERR_VERIFY;
}

SpiBm_Status_t SPI_BM_WaitEnd(SPI_TypeDef *spi, uint32_t max_cycles)
{
    if (entry(spi) == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    if (max_cycles == 0U)
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
        const uint32_t sr = spi->SR;
        if ((sr & SPI_SR_MODF) != 0U)
        {
            spi->IFCR = SPI_IFCR_MODFC;          /* SPE and MASTER already 0 (53:1468-1471) */
            return SPI_BM_ERR_MODF;
        }
        if ((sr & SPI_SR_EOT) != 0U)
        {
            return ((sr & SPI_SR_OVR) != 0U) ? SPI_BM_ERR_OVERRUN : SPI_BM_OK;
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

SpiBm_Status_t SPI_BM_DmaEnd(SPI_TypeDef *spi, uint32_t guard_cycles)
{
    if (entry(spi) == NULL)
    {
        return SPI_BM_ERR_PARAM;
    }
    if (guard_cycles == 0U)
    {
        return SPI_BM_ERR_PARAM;
    }
    /* The closing order of 53:1268-1274: the caller stopped its streams; then the SPI
       (guard, IER = 0, SPE = 0), then TXDMAEN and RXDMAEN, then the flags. With no
       CYCCNT the SPI is still disabled, and the call reports the missing guard. */
    const bool timed = cyccnt_running() && wait_from(DWT->CYCCNT, guard_cycles);
    ier_write(spi, 0U);
    cr1_clear(spi, SPI_CR1_SPE);
    cfg1_update(spi, SPI_CFG1_TXDMAEN | SPI_CFG1_RXDMAEN, 0U);
    spi->IFCR = IFCR_ALL;
    __DSB();
    if (((spi->CR1 & SPI_CR1_SPE) != 0U) || ((spi->CFG1 & (SPI_CFG1_TXDMAEN | SPI_CFG1_RXDMAEN)) != 0U))
    {
        return SPI_BM_ERR_VERIFY;
    }
    return timed ? SPI_BM_OK : SPI_BM_ERR_TIMEOUT;
}

SpiBm_Status_t SPI_BM_GetDmaRequest(const SPI_TypeDef *spi, bool tx, uint32_t *request_id)
{
    const SpiEntry_t *e = entry(spi);
    if ((request_id == NULL) || (e == NULL))
    {
        return SPI_BM_ERR_PARAM;
    }
    *request_id = tx ? e->req_tx : e->req_rx;
    return SPI_BM_OK;
}

SpiBm_Status_t SPI_BM_GetIRQn(const SPI_TypeDef *spi, IRQn_Type *out)
{
    const SpiEntry_t *e = entry(spi);
    if ((out == NULL) || (e == NULL))
    {
        return SPI_BM_ERR_PARAM;
    }
    *out = e->irq;
    return SPI_BM_OK;
}

/* ---- Errors --------------------------------------------------------------------- */

uint32_t SPI_BM_TakeErrors(SPI_TypeDef *spi)
{
    if (entry(spi) == NULL)
    {
        return 0U;
    }
    /* SR and IFCR share the bit positions of OVR, MODF and TIFRE (53:3606, 53:3732). */
    const uint32_t taken = spi->SR & (SPI_BM_ERRF_OVR | SPI_BM_ERRF_MODF | SPI_BM_ERRF_TIFRE);
    if (taken != 0U)
    {
        spi->IFCR = taken;
        __DSB();
    }
    return taken;
}
