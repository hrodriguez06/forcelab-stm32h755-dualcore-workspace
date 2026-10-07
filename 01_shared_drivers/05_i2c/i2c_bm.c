/**
  ******************************************************************************
  * @file    i2c_bm.c
  * @brief   I2C1-I2C4, bare-metal tier. Contract in i2c_bm.h.
  *
  *          Sources: RM0399 rev 4, as the .txt chapters in
  *          00_docs_stm32h7/docs/peripherals/ (I2C 50_*.txt, RCC 09_*.txt,
  *          SYSCFG 13_*.txt, DMAMUX 18_*.txt; the line numbers below are of those
  *          files, written 50:<line>, 09:<line>, 13:<line>, 18:<line>), ES0445
  *          rev 6 2.19, DS12923 table 105 (analog filter) and the CMSIS device
  *          header stm32h755xx.h (interrupt numbers).
  ******************************************************************************
  */

#include "i2c_bm.h"

/* C8: stm32h7xx.h, <stdint.h> and <stdbool.h> only, so no <stddef.h>. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* ==========================================================================
 * Capability table (C1, plan rule 2): one row per instance, the only place the
 * driver learns a silicon fact. Every validation below reads it.
 * ========================================================================== */

#define BUS_APB1L   0U          /* RCC_APB1LENR (this core's bank) */
#define BUS_APB4    1U          /* RCC_APB4ENR                     */

typedef struct
{
    uint32_t  base;
    uint8_t   bus;             /* BUS_*                                            */
    uint8_t   rcc_bit;         /* enable bit in that register                      */
    uint8_t   ksel;            /* I2C_BM_KSEL_*                                    */
    uint8_t   fmp_bit;         /* SYSCFG_PMCR.I2CxFMP                              */
    uint8_t   dmamux;          /* 1: DMAMUX1 (DMA1/DMA2), 2: DMAMUX2 (BDMA)         */
    uint8_t   req_rx;          /* DMAMUX request of the RX stream                  */
    uint8_t   req_tx;          /* DMAMUX request of the TX stream                  */
    IRQn_Type irq_ev;
    IRQn_Type irq_er;
} I2cEntry_t;

/* Base: stm32h755xx.h (I2C1..3 on D2 APB1, I2C4 on D3 APB4). RCC: §9.7.44
   RCC_APB1LENR (09:7106; I2C3EN bit 23 09:7224, I2C2EN 22 09:7230, I2C1EN 21
   09:7236), §9.7.47 RCC_APB4ENR (09:7577; I2C4EN bit 7 09:7683). Kernel clock:
   I2C123SEL [13:12] of RCC_D2CCIP2R (09:4744-4749), I2C4SEL [9:8] of RCC_D3CCIPR
   (09:4868-4873). SYSCFG_PMCR I2C1FMP..I2C4FMP bits 0..3 (13:33, 13:122-137).
   DMAMUX1, table 126 (i2c1 33/34, i2c2 35/36 18:113-116, i2c3 73/74 18:110-111);
   DMAMUX2, table 130 (i2c4 13/14, 18:235-236). IRQn: stm32h755xx.h (I2C1 31/32,
   I2C2 33/34, I2C3 72/73, I2C4 95/96). */
static const I2cEntry_t k_i2c[] = {
    /* base       bus        bit  ksel               fmp mux rx   tx   ev            er           */
    { I2C1_BASE, BUS_APB1L, 21U, I2C_BM_KSEL_I2C123, 0U, 1U, 33U, 34U, I2C1_EV_IRQn, I2C1_ER_IRQn },
    { I2C2_BASE, BUS_APB1L, 22U, I2C_BM_KSEL_I2C123, 1U, 1U, 35U, 36U, I2C2_EV_IRQn, I2C2_ER_IRQn },
    { I2C3_BASE, BUS_APB1L, 23U, I2C_BM_KSEL_I2C123, 2U, 1U, 73U, 74U, I2C3_EV_IRQn, I2C3_ER_IRQn },
    { I2C4_BASE, BUS_APB4,   7U, I2C_BM_KSEL_I2C4,   3U, 2U, 13U, 14U, I2C4_EV_IRQn, I2C4_ER_IRQn },
};
#define I2C_COUNT   ((uint32_t)(sizeof(k_i2c) / sizeof(k_i2c[0])))

/* ---- Register facts (RM0399 50.7) ------------------------------------------------- */
#define TIMINGR_RESERVED  0x0F000000UL                       /* bits 27:24, 50:2782 */
#define ICR_ALL           (I2C_ICR_ADDRCF | I2C_ICR_NACKCF | I2C_ICR_STOPCF | I2C_ICR_BERRCF | I2C_ICR_ARLOCF | \
                           I2C_ICR_OVRCF | I2C_ICR_PECCF | I2C_ICR_TIMOUTCF | I2C_ICR_ALERTCF)
#define CR1_IE_ALL        (I2C_CR1_TXIE | I2C_CR1_RXIE | I2C_CR1_ADDRIE | I2C_CR1_NACKIE | I2C_CR1_STOPIE | \
                           I2C_CR1_TCIE | I2C_CR1_ERRIE)
#define CR1_DMA_ALL       (I2C_CR1_TXDMAEN | I2C_CR1_RXDMAEN)
#define CR1_FILTERS       (I2C_CR1_ANFOFF | I2C_CR1_DNF_Msk)
#define ISR_RESET         I2C_ISR_TXE                        /* 0x00000001, 50:2866 */
#define ADDR_MIN          0x08U                              /* 7-bit addresses outside the reserved */
#define ADDR_MAX          0x77U                              /* groups 0000xxx and 1111xxx           */
#define N_MAX             0xFFFFU
#define CHUNK             255U                               /* NBYTES[7:0], 50:1081-1082 */
#define TIMEOUT_MAX       4096U                              /* TIMEOUTA[11:0] + 1, 50:2858 */
#define PE_LOW_READS      3U                                 /* PE = 0 for >= 3 APB cycles (50:441-445) */
#define PE_LOW_MAX        1000U

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

static void cr1_update(I2C_TypeDef *i2c, uint32_t clear, uint32_t set)
{
    const uint32_t pm = irq_save();
    i2c->CR1 = (i2c->CR1 & ~clear) | set;
    irq_restore(pm);
}

static void cr2_update(I2C_TypeDef *i2c, uint32_t clear, uint32_t set)
{
    const uint32_t pm = irq_save();
    i2c->CR2 = (i2c->CR2 & ~clear) | set;
    irq_restore(pm);
}

/* ==========================================================================
 * Validation (C6): pure, no peripheral access
 * ========================================================================== */

static const I2cEntry_t *entry(const I2C_TypeDef *i2c)
{
    if (i2c == NULL)
    {
        return NULL;
    }
    for (uint32_t i = 0U; i < I2C_COUNT; i++)
    {
        if (k_i2c[i].base == (uint32_t)i2c)
        {
            return &k_i2c[i];
        }
    }
    return NULL;
}

bool I2C_BM_IsValidInstance(const I2C_TypeDef *i2c)
{
    return entry(i2c) != NULL;
}

I2cBm_Status_t I2C_BM_GetCaps(const I2C_TypeDef *i2c, I2cBm_Caps_t *out)
{
    const I2cEntry_t *e = entry(i2c);
    if ((out == NULL) || (e == NULL))
    {
        return I2C_BM_ERR_PARAM;
    }
    out->kernel_select = e->ksel;
    out->dmamux        = e->dmamux;
    return I2C_BM_OK;
}

/* ==========================================================================
 * Clock and Fm+
 * ========================================================================== */

/* This core's enable register of the instance: RCC->APB1LENR / APB4ENR sit in the
   per-core bank RM0399 9.7.1 maps at one address: CPU1 lands in RCC_C1, CPU2 in
   RCC_C2, and the peripheral runs while either bank enables it (9.5.10). */
static volatile uint32_t *enr_reg(const I2cEntry_t *e)
{
    return (e->bus == BUS_APB1L) ? &RCC->APB1LENR : &RCC->APB4ENR;
}

I2cBm_Status_t I2C_BM_EnableClock(I2C_TypeDef *i2c)
{
    const I2cEntry_t *e = entry(i2c);
    if (e == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    volatile uint32_t *reg = enr_reg(e);
    const uint32_t bit = 1UL << e->rcc_bit;
    const uint32_t pm = irq_save();
    *reg |= bit;
    const uint32_t rb = *reg;                  /* the read that lets the enable land */
    irq_restore(pm);
    return ((rb & bit) != 0U) ? I2C_BM_OK : I2C_BM_ERR_VERIFY;
}

I2cBm_Status_t I2C_BM_DisableClock(I2C_TypeDef *i2c)
{
    const I2cEntry_t *e = entry(i2c);
    if (e == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    volatile uint32_t *reg = enr_reg(e);
    const uint32_t bit = 1UL << e->rcc_bit;
    const uint32_t pm = irq_save();
    *reg &= ~bit;
    const uint32_t rb = *reg;
    irq_restore(pm);
    return ((rb & bit) == 0U) ? I2C_BM_OK : I2C_BM_ERR_VERIFY;
}

/* The raw kernel clock field of the instance; never written (C10). */
I2cBm_Status_t I2C_BM_GetKernelSource(const I2C_TypeDef *i2c, uint32_t *field)
{
    const I2cEntry_t *e = entry(i2c);
    if ((field == NULL) || (e == NULL))
    {
        return I2C_BM_ERR_PARAM;
    }
    if (e->ksel == I2C_BM_KSEL_I2C123)
    {
        *field = (RCC->D2CCIP2R & RCC_D2CCIP2R_I2C123SEL_Msk) >> RCC_D2CCIP2R_I2C123SEL_Pos;
    }
    else
    {
        *field = (RCC->D3CCIPR & RCC_D3CCIPR_I2C4SEL_Msk) >> RCC_D3CCIPR_I2C4SEL_Pos;
    }
    return I2C_BM_OK;
}

/* SYSCFG_PMCR is shared with the analog switches, BOOSTE, EPIS (08_eth) and the
   PBxFMP bits (13:33-137): only this instance's bit moves, under PRIMASK, read
   back. The SYSCFG runs while either core's bank enables its clock. */
I2cBm_Status_t I2C_BM_SetFastModePlus(I2C_TypeDef *i2c, bool enable)
{
    const I2cEntry_t *e = entry(i2c);
    if (e == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if (((RCC_C1->APB4ENR | RCC_C2->APB4ENR) & RCC_APB4ENR_SYSCFGEN) == 0U)
    {
        return I2C_BM_ERR_BUSY;                /* an unclocked SYSCFG ignores the write */
    }
    const uint32_t bit = 1UL << e->fmp_bit;
    const uint32_t pm = irq_save();
    SYSCFG->PMCR = enable ? (SYSCFG->PMCR | bit) : (SYSCFG->PMCR & ~bit);
    const uint32_t rb = SYSCFG->PMCR;
    irq_restore(pm);
    return (((rb & bit) != 0U) == enable) ? I2C_BM_OK : I2C_BM_ERR_VERIFY;
}

/* ==========================================================================
 * Pure arithmetic (C15, plan "Reglas de calculo")
 * ========================================================================== */

/* Tables 405 and 407 (50:356-384, 1043-1064), in ns, and the kernel minimum of
   ES0445 2.19.1, per mode. */
typedef struct
{
    uint32_t scl_max, t_low, t_high, t_su_dat, t_hd_dat, t_vd_dat, t_r_max, t_f_max, kernel_min;
} ModeRow_t;

static const ModeRow_t k_mode[3] = {
    /*  scl_max  tLOW  tHIGH tSU;DAT tHD;DAT tVD;DAT t_r   t_f  kernel min */
    {  100000U, 4700U, 4000U, 250U, 0U, 3450U, 1000U, 300U,  4000000U },   /* Sm  */
    {  400000U, 1300U,  600U, 100U, 0U,  900U,  300U, 300U, 10000000U },   /* Fm  */
    { 1000000U,  500U,  260U,  50U, 0U,  450U,  120U, 120U, 20000000U },   /* Fm+ */
};

#define NS_PER_S        ((int64_t)1000 * 1000 * 1000)
#define T_AF_MIN_NS     50                     /* DS12923 table 105 */
#define T_AF_MAX_NS     80

/* Integer division rounded up / down for a positive divisor and any sign. */
static int64_t div_up(int64_t a, int64_t b)
{
    return (a >= 0) ? ((a + b - 1) / b) : -((-a) / b);
}

static int64_t div_down(int64_t a, int64_t b)
{
    return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b);
}

static int64_t max64(int64_t a, int64_t b)
{
    return (a > b) ? a : b;
}

I2cBm_Status_t I2C_BM_CalcTiming(const I2cBm_TimingIn_t *in, I2cBm_TimingOut_t *out)
{
    /* Rule 1: parameters, then what no TIMINGR can meet. */
    if ((in == NULL) || (out == NULL))
    {
        return I2C_BM_ERR_PARAM;
    }
    if ((in->kernel_hz == 0U) || (in->scl_hz == 0U) || (in->scl_hz > 1000000U) || (in->dnf > 15U))
    {
        return I2C_BM_ERR_PARAM;
    }
    const uint32_t mode = (in->scl_hz <= 100000U) ? I2C_BM_SPEED_SM
                        : ((in->scl_hz <= 400000U) ? I2C_BM_SPEED_FM : I2C_BM_SPEED_FMP);
    const ModeRow_t *m = &k_mode[mode];
    if ((in->rise_ns > m->t_r_max) || (in->fall_ns > m->t_f_max) || (in->kernel_hz < m->kernel_min))
    {
        return I2C_BM_ERR_RANGE;
    }

    /* Rule 2: k is kernel cycles per ns, kept as kernel_hz / 10^9 inside each
       expression; t_af only with the analog filter. */
    const int64_t k     = (int64_t)in->kernel_hz;
    const int64_t f     = (int64_t)in->scl_hz;
    const int64_t tr    = (int64_t)in->rise_ns;
    const int64_t tf    = (int64_t)in->fall_ns;
    const int64_t dnf   = (int64_t)in->dnf;
    const int64_t afmin = in->analog_filter ? T_AF_MIN_NS : 0;
    const int64_t afmax = in->analog_filter ? T_AF_MAX_NS : 0;
    const int64_t tlow  = (int64_t)m->t_low;
    const int64_t thigh = (int64_t)m->t_high;

    /* Common to every PRESC: the model of tSYNC1 + tSYNC2 (50:977-989), in kernel
       cycles, and the kernel cycles of one SCL period, rounded up so the period of
       the model is never shorter than 1/scl_hz (rule 3, N rounded up as approved
       2026-10-07: with N rounded down scl_hz_model could exceed scl_hz). */
    const int64_t sync   = div_up((tr + tf + (2 * afmin)) * k, NS_PER_S) + (2 * (dnf + 3));
    const int64_t period = div_up(k, f);

    /* Rule 3: the first PRESC that meets every condition (maximum resolution). */
    for (int64_t p = 0; p <= 15; p++)
    {
        const int64_t d = NS_PER_S * (p + 1);          /* ns x Hz per PRESC tick */
        const int64_t scldel = max64(0, div_up((tr + (int64_t)m->t_su_dat) * k, d) - 1);   /* 50:342 */
        if (scldel > 15)
        {
            continue;
        }
        const int64_t sdadel_min = max64(0, div_up(((tf + (int64_t)m->t_hd_dat - afmin) * k) -
                                                   ((dnf + 3) * NS_PER_S), d));            /* 50:319 */
        const int64_t sdadel_max = div_down((((int64_t)m->t_vd_dat - tr - afmax) * k) -
                                            ((dnf + 4) * NS_PER_S), d);                    /* 50:331 */
        if ((sdadel_min > sdadel_max) || (sdadel_min > 15))
        {
            continue;
        }
        const int64_t n     = div_up(period - sync, p + 1);
        const int64_t l_min = div_up(tlow * k, d);
        const int64_t h_min = div_up(thigh * k, d);
        const int64_t l     = max64(l_min, div_up(n * tlow, tlow + thigh));
        const int64_t h     = max64(h_min, n - l);
        if ((l > 256) || (h > 256))
        {
            continue;
        }
        /* 50:162-163 with the programmed SCL low and high times, the filters at
           their largest delay (t_af,max + DNF kernel cycles):
           t_kernel < (tLOW - t_filters) / 4 and t_kernel < tHIGH. */
        if ((((l * (p + 1)) - dnf - 4) * NS_PER_S) <= (afmax * k))
        {
            continue;
        }
        if ((h * (p + 1)) <= 1)
        {
            continue;
        }
        out->timingr = ((uint32_t)p << I2C_TIMINGR_PRESC_Pos) | ((uint32_t)scldel << I2C_TIMINGR_SCLDEL_Pos) |
                       ((uint32_t)sdadel_min << I2C_TIMINGR_SDADEL_Pos) | ((uint32_t)(h - 1) << I2C_TIMINGR_SCLH_Pos) |
                       ((uint32_t)(l - 1) << I2C_TIMINGR_SCLL_Pos);
        out->mode = mode;
        out->scl_hz_model = (uint32_t)(k / (sync + ((l + h) * (p + 1))));
        return I2C_BM_OK;
    }
    return I2C_BM_ERR_RANGE;                           /* rule 4 */
}

/* ==========================================================================
 * Configuration
 * ========================================================================== */

static uint32_t cr1_filters_of(const I2cBm_Config_t *cfg)
{
    return (cfg->analog_filter ? 0U : I2C_CR1_ANFOFF) | (cfg->dnf << I2C_CR1_DNF_Pos);
}

static uint32_t timeoutr_of(const I2cBm_Config_t *cfg)
{
    return (cfg->timeout_scl_low == 0U) ? 0U
           : (((cfg->timeout_scl_low - 1U) << I2C_TIMEOUTR_TIMEOUTA_Pos) | I2C_TIMEOUTR_TIMOUTEN);
}

/* A transfer owns the instance: START or STOP pending (50:2620-2674), or an
   interrupt or DMA transfer enabled. */
static bool running(const I2C_TypeDef *i2c)
{
    return ((i2c->CR2 & (I2C_CR2_START | I2C_CR2_STOP)) != 0U) || ((i2c->CR1 & (CR1_IE_ALL | CR1_DMA_ALL)) != 0U);
}

I2cBm_Status_t I2C_BM_CheckConfig(const I2cBm_Config_t *cfg)
{
    if (cfg == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    const I2cEntry_t *e = entry(cfg->i2c);
    if (e == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if ((cfg->dnf > 15U) || (cfg->timeout_scl_low > TIMEOUT_MAX) || ((cfg->timingr & TIMINGR_RESERVED) != 0U))
    {
        return I2C_BM_ERR_PARAM;
    }
    /* State (C12): with PE = 1 the hardware ignores TIMINGR, ANFOFF and DNF
       (50:2505-2529, 2808). The same configuration in force is a software reset. */
    const I2C_TypeDef *i2c = cfg->i2c;
    if ((i2c->CR1 & I2C_CR1_PE) != 0U)
    {
        if ((i2c->TIMINGR != cfg->timingr) || ((i2c->CR1 & CR1_FILTERS) != cr1_filters_of(cfg)) ||
            (i2c->TIMEOUTR != timeoutr_of(cfg)) || ((i2c->CR1 & I2C_CR1_NOSTRETCH) != 0U) || running(i2c))
        {
            return I2C_BM_ERR_BUSY;
        }
    }
    return I2C_BM_OK;
}

/* PE = 0, then read back until it reads 0, at least PE_LOW_READS times: the
   software reset needs PE low for three APB cycles (50:441-445). Releases SCL and
   SDA (50:429-431). */
static bool pe_off(I2C_TypeDef *i2c)
{
    cr1_update(i2c, I2C_CR1_PE, 0U);
    uint32_t zeros = 0U;
    for (uint32_t i = 0U; i < PE_LOW_MAX; i++)
    {
        if ((i2c->CR1 & I2C_CR1_PE) == 0U)
        {
            if (++zeros >= PE_LOW_READS)
            {
                return true;
            }
        }
    }
    return false;
}

/* The software reset of rule 6: PE = 0 read back, then PE = 1. */
static void soft_reset(I2C_TypeDef *i2c)
{
    (void)pe_off(i2c);
    i2c->CR2 = 0U;                                   /* START = 0 now (50:2645) */
    cr1_update(i2c, 0U, I2C_CR1_PE);
}

I2cBm_Status_t I2C_BM_Init(const I2cBm_Config_t *cfg)
{
    const I2cBm_Status_t st = I2C_BM_CheckConfig(cfg);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    I2C_TypeDef *i2c = cfg->i2c;
    const uint32_t filters = cr1_filters_of(cfg);
    const uint32_t tmo = timeoutr_of(cfg);

    if (!pe_off(i2c))                                /* 50:404-425, figure 585 */
    {
        return I2C_BM_ERR_VERIFY;
    }
    i2c->CR1 = filters;                              /* ANFOFF, DNF; NOSTRETCH = 0, IE = DMA = 0 */
    i2c->TIMINGR = cfg->timingr;
    /* TIMEOUTA is written only with TIMOUTEN = 0 (50:2861): the first write clears
       TIMOUTEN (a TIMEOUTA written with it would be ignored), the second writes
       TIMEOUTA, the third sets TIMOUTEN. Always all three, also with the timeout off,
       so a TIMEOUTA left from before (no Deinit) does not survive (audit F1, m29). */
    i2c->TIMEOUTR = 0U;
    i2c->TIMEOUTR = tmo & ~I2C_TIMEOUTR_TIMOUTEN;
    i2c->TIMEOUTR = tmo;
    i2c->OAR1 = 0U;                                  /* OA1EN = 0 first: then OA1 may be written */
    i2c->OAR1 = 0U;
    i2c->OAR2 = 0U;
    i2c->OAR2 = 0U;
    i2c->CR2  = 0U;
    i2c->ICR  = ICR_ALL;
    cr1_update(i2c, 0U, I2C_CR1_PE);
    __DSB();
    if ((i2c->CR1 != (filters | I2C_CR1_PE)) || (i2c->TIMINGR != cfg->timingr) || (i2c->TIMEOUTR != tmo) ||
        (i2c->OAR1 != 0U) || (i2c->OAR2 != 0U))
    {
        return I2C_BM_ERR_VERIFY;
    }
    return ((i2c->ISR & I2C_ISR_BUSY) != 0U) ? I2C_BM_ERR_BUS : I2C_BM_OK;
}

I2cBm_Status_t I2C_BM_Deinit(I2C_TypeDef *i2c)
{
    if (entry(i2c) == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    cr1_update(i2c, CR1_IE_ALL | CR1_DMA_ALL, 0U);
    const bool off = pe_off(i2c);                    /* lets go of SCL and SDA */
    i2c->CR1      = 0U;
    i2c->CR2      = 0U;
    i2c->TIMEOUTR = 0U;                              /* TIMOUTEN = 0, then TIMEOUTA (50:2861) */
    i2c->TIMEOUTR = 0U;
    i2c->TIMINGR  = 0U;
    i2c->OAR1     = 0U;
    i2c->OAR1     = 0U;
    i2c->OAR2     = 0U;
    i2c->OAR2     = 0U;
    i2c->ICR      = ICR_ALL;
    __DSB();
    const bool ok = off && (i2c->CR1 == 0U) && (i2c->CR2 == 0U) && (i2c->OAR1 == 0U) && (i2c->OAR2 == 0U) &&
                    (i2c->TIMINGR == 0U) && (i2c->TIMEOUTR == 0U) && (i2c->ISR == ISR_RESET);
    return ok ? I2C_BM_OK : I2C_BM_ERR_VERIFY;
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

/* Successive equal CYCCNT reads that mean the counter stopped (C13: no loop without
   an exit, even if CYCCNT stops after the check at the start of a call). */
#define STALL_READS     1000U

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

/* True when the budget is spent or CYCCNT has stopped. */
static bool clk_expired(Clock_t *c)
{
    const uint32_t now = DWT->CYCCNT;
    c->stalled = (now == c->last) ? (c->stalled + 1U) : 0U;
    c->last = now;
    return ((now - c->t0) >= c->limit) || (c->stalled > STALL_READS);
}

static I2cBm_Status_t chk_transfer(const I2C_TypeDef *i2c, uint32_t addr7, uint32_t ntx, uint32_t nrx)
{
    if (entry(i2c) == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if ((addr7 < ADDR_MIN) || (addr7 > ADDR_MAX) || (ntx > N_MAX) || (nrx > N_MAX))
    {
        return I2C_BM_ERR_PARAM;
    }
    if (((i2c->CR1 & I2C_CR1_PE) == 0U) || running(i2c))
    {
        return I2C_BM_ERR_BUSY;                     /* no Init, or a transfer owns it (C12) */
    }
    if ((i2c->ISR & I2C_ISR_BUSY) != 0U)
    {
        return I2C_BM_ERR_BUS;                      /* rule 5: the bus is not free */
    }
    return I2C_BM_OK;
}

I2cBm_Status_t I2C_BM_CheckTransfer(const I2C_TypeDef *i2c, uint32_t addr7, uint32_t ntx, uint32_t nrx)
{
    return chk_transfer(i2c, addr7, ntx, nrx);
}

/* CR2 for one chunk of `left` bytes still to go in this direction: NBYTES, and
   RELOAD while more than 255 remain; on the last chunk AUTOEND as asked (1: STOP
   after it; 0: TC, the repeated start of WriteRead) (50:517-559, table 406). */
static uint32_t cr2_chunk(uint32_t left, bool autoend)
{
    if (left > CHUNK)
    {
        return (CHUNK << I2C_CR2_NBYTES_Pos) | I2C_CR2_RELOAD;
    }
    return (left << I2C_CR2_NBYTES_Pos) | (autoend ? I2C_CR2_AUTOEND : 0U);
}

static uint32_t cr2_addr(uint32_t addr7, bool read)
{
    return ((addr7 << 1) & I2C_CR2_SADD_Msk) | (read ? I2C_CR2_RD_WRN : 0U);
}

/* A flag the last register write clears may still read set for a few cycles:
   a write reaches the kernel-clock side of the peripheral up to 2 PCLK + 6 kernel
   cycles later (50:2442-2444). Read until it is clear, CLEAR_READS reads at most,
   so the next ISR read of a loop does not serve the same event twice. */
#define CLEAR_READS     256U

static void wait_cleared(const I2C_TypeDef *i2c, uint32_t flag)
{
    for (uint32_t i = 0U; (i < CLEAR_READS) && ((i2c->ISR & flag) != 0U); i++)
    {
    }
}

/* The next chunk at TCR: CR2's NBYTES, RELOAD and AUTOEND rewritten (TCR clears
   when NBYTES is written non-zero, 50:526-534); START = 0 here, so nothing that
   must not change with START = 1 does (C12). */
static void reload(I2C_TypeDef *i2c, uint32_t left, bool autoend)
{
    cr2_update(i2c, I2C_CR2_NBYTES_Msk | I2C_CR2_RELOAD | I2C_CR2_AUTOEND, cr2_chunk(left, autoend));
    wait_cleared(i2c, I2C_ISR_TCR);
}

/* The repeated start of WriteRead at TC (50:1185-1188): CR2 written whole with
   START = 0 before the write (C12); setting START clears TC. */
static void restart_read(I2C_TypeDef *i2c, uint32_t addr7, uint32_t nrx)
{
    i2c->CR2 = cr2_addr(addr7, true) | cr2_chunk(nrx, true) | I2C_CR2_START;
    wait_cleared(i2c, I2C_ISR_TC);
}

/* TXDR flushed (TXE = 1 written, 50:2976): a byte preloaded but never sent. */
static void tx_flush(I2C_TypeDef *i2c)
{
    i2c->ISR = I2C_ISR_TXE;
}

/* The end of a transfer: CR2 = 0 (START is 0 once a STOP was detected), the given
   flags cleared in one ICR write, TXDR flushed. */
static void xfer_close(I2C_TypeDef *i2c, uint32_t icr)
{
    i2c->CR2 = 0U;
    i2c->ICR = icr;
    tx_flush(i2c);
    __DSB();
}

/* TIMEOUT in master mode (SCL low longer than TIMEOUTA, 50:1793-1800): RM0399 says
   the hardware then sends a STOP (50:2286-2287), but on the bench (05_i2c F1,
   docs/i2c_dev_f1_bench_notaps_*) the master went on holding SCL and SDA low after
   the other device had let SCL go, until the caller's budget ran out. So the
   instance is software-reset at once (PE = 0 lets go of both lines, rule 6) and the
   flags cleared; the caller gets ERR_BUS. */
static void timeout_end(I2C_TypeDef *i2c)
{
    soft_reset(i2c);
    xfer_close(i2c, ICR_ALL);
}

/* After NACKF the hardware sends the STOP itself (50:1192-1194): waited for within
   the budget. False if it did not come. */
static bool wait_stop(I2C_TypeDef *i2c, Clock_t *c)
{
    while ((i2c->ISR & I2C_ISR_STOPF) == 0U)
    {
        if (clk_expired(c))
        {
            return false;
        }
    }
    return true;
}

/* One blocking transfer: ntx bytes written, then (repeated start) nrx read; ntx =
   nrx = 0 is the probe. Plan rules 5 and 6. */
static I2cBm_Status_t xfer_poll(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t ntx,
                                uint8_t *rx, uint32_t nrx, uint32_t max_cycles)
{
    if (!cyccnt_running())
    {
        return I2C_BM_ERR_TIMEOUT;                  /* no time base: no wait could end (C13) */
    }
    Clock_t c;
    clk_start(&c, max_cycles);
    i2c->ICR = ICR_ALL;                             /* stale flags of an earlier transfer */
    tx_flush(i2c);

    uint32_t sent = 0U;                             /* bytes written to TXDR */
    uint32_t got = 0U;                              /* bytes read from RXDR  */
    bool reading = (ntx == 0U) && (nrx != 0U);
    if (reading)
    {
        i2c->CR2 = cr2_addr(addr7, true) | cr2_chunk(nrx, true) | I2C_CR2_START;
    }
    else
    {
        if (ntx != 0U)
        {
            i2c->TXDR = tx[0];                      /* ES0445 2.19.6: the first byte before START */
            sent = 1U;
        }
        i2c->CR2 = cr2_addr(addr7, false) | cr2_chunk(ntx, nrx == 0U) | I2C_CR2_START;
    }

    for (;;)
    {
        const uint32_t isr = i2c->ISR;
        if ((isr & I2C_ISR_BERR) != 0U)
        {
            i2c->ICR = I2C_ICR_BERRCF;              /* ES0445 2.19.2: spurious in master mode */
        }
        if ((isr & I2C_ISR_ARLO) != 0U)
        {
            xfer_close(i2c, ICR_ALL);               /* already a slave, lines released (50:2239-2249) */
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
            const uint8_t b = (uint8_t)i2c->RXDR;
            if (got < nrx)
            {
                rx[got] = b;
            }
            got++;
            continue;                               /* RXNE before TCR and STOPF */
        }
        if (!reading && ((isr & I2C_ISR_TXIS) != 0U) && (sent < ntx))
        {
            i2c->TXDR = tx[sent];
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
                /* The write is done (AUTOEND = 0, SCL stretched): the read starts
                   with a repeated start; CR2 is written with START = 0 (C12). */
                reading = true;
                restart_read(i2c, addr7, nrx);
            }
            else
            {
                cr2_update(i2c, 0U, I2C_CR2_STOP);  /* not reached with AUTOEND = 1 */
                wait_cleared(i2c, I2C_ISR_TC);
            }
        }
        if ((isr & I2C_ISR_STOPF) != 0U)
        {
            if ((i2c->ISR & I2C_ISR_RXNE) != 0U)
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

I2cBm_Status_t I2C_BM_Write(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t n, uint32_t max_cycles)
{
    if ((tx == NULL) && (n != 0U))
    {
        return I2C_BM_ERR_PARAM;
    }
    if (entry(i2c) == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if (max_cycles == 0U)
    {
        return I2C_BM_ERR_PARAM;
    }
    const I2cBm_Status_t st = chk_transfer(i2c, addr7, n, 0U);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    return xfer_poll(i2c, addr7, tx, n, NULL, 0U, max_cycles);
}

I2cBm_Status_t I2C_BM_Read(I2C_TypeDef *i2c, uint32_t addr7, uint8_t *rx, uint32_t n, uint32_t max_cycles)
{
    if (rx == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if (entry(i2c) == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if ((n == 0U) || (max_cycles == 0U))
    {
        return I2C_BM_ERR_PARAM;
    }
    const I2cBm_Status_t st = chk_transfer(i2c, addr7, 0U, n);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    return xfer_poll(i2c, addr7, NULL, 0U, rx, n, max_cycles);
}

I2cBm_Status_t I2C_BM_WriteRead(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t ntx,
                                uint8_t *rx, uint32_t nrx, uint32_t max_cycles)
{
    if ((tx == NULL) || (rx == NULL))
    {
        return I2C_BM_ERR_PARAM;
    }
    if (entry(i2c) == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if ((ntx == 0U) || (nrx == 0U) || (max_cycles == 0U))
    {
        return I2C_BM_ERR_PARAM;
    }
    const I2cBm_Status_t st = chk_transfer(i2c, addr7, ntx, nrx);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    return xfer_poll(i2c, addr7, tx, ntx, rx, nrx, max_cycles);
}

/* ---- Interrupt transfers (plan rule 7) --------------------------------------------- */

I2cBm_Status_t I2C_BM_StartIt(I2C_TypeDef *i2c, I2cBm_Xfer_t *x)
{
    if (x == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if (((x->tx == NULL) && (x->ntx != 0U)) || ((x->rx == NULL) && (x->nrx != 0U)))
    {
        return I2C_BM_ERR_PARAM;
    }
    const I2cBm_Status_t st = chk_transfer(i2c, x->addr7, x->ntx, x->nrx);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    x->done_tx = 0U;
    x->done_rx = 0U;
    x->status  = I2C_BM_PENDING;
    i2c->ICR = ICR_ALL;
    tx_flush(i2c);
    uint32_t cr2;
    uint32_t ie = I2C_CR1_TCIE | I2C_CR1_STOPIE | I2C_CR1_NACKIE | I2C_CR1_ERRIE;
    if (x->ntx != 0U)
    {
        i2c->TXDR = x->tx[0];                       /* ES0445 2.19.6 */
        x->done_tx = 1U;                            /* written; acknowledged unless NACKed */
        cr2 = cr2_addr(x->addr7, false) | cr2_chunk(x->ntx, x->nrx == 0U);
        ie |= I2C_CR1_TXIE | ((x->nrx != 0U) ? I2C_CR1_RXIE : 0U);
    }
    else if (x->nrx != 0U)
    {
        cr2 = cr2_addr(x->addr7, true) | cr2_chunk(x->nrx, true);
        ie |= I2C_CR1_RXIE;
    }
    else
    {
        cr2 = cr2_addr(x->addr7, false) | I2C_CR2_AUTOEND;   /* the probe */
    }
    i2c->CR2 = cr2;                                 /* CR2 first, then the enables, then START */
    cr1_update(i2c, 0U, ie);
    cr2_update(i2c, 0U, I2C_CR2_START);
    return I2C_BM_OK;
}

/* Every enable off, CR2 = 0, flags cleared, TXDR flushed; then the status. */
static bool it_finish(I2C_TypeDef *i2c, I2cBm_Xfer_t *x, I2cBm_Status_t st)
{
    cr1_update(i2c, CR1_IE_ALL, 0U);
    xfer_close(i2c, ICR_ALL);
    x->status = st;
    return true;
}

bool I2C_BM_XferIrq(I2C_TypeDef *i2c, I2cBm_Xfer_t *x)
{
    if ((entry(i2c) == NULL) || (x == NULL))
    {
        return false;
    }
    if (x->status != I2C_BM_PENDING)
    {
        cr1_update(i2c, CR1_IE_ALL, 0U);            /* already final: nothing to serve */
        return true;
    }
    const uint32_t isr = i2c->ISR;
    const bool reading = (i2c->CR2 & I2C_CR2_RD_WRN) != 0U;
    if ((isr & I2C_ISR_BERR) != 0U)
    {
        i2c->ICR = I2C_ICR_BERRCF;                  /* ES0445 2.19.2 */
    }
    if ((isr & I2C_ISR_ARLO) != 0U)
    {
        return it_finish(i2c, x, I2C_BM_ERR_ARLO);
    }
    if ((isr & I2C_ISR_TIMEOUT) != 0U)
    {
        cr1_update(i2c, CR1_IE_ALL, 0U);
        timeout_end(i2c);                           /* software reset: both lines let go */
        x->status = I2C_BM_ERR_BUS;
        return true;
    }
    /* NACKF. done_tx counted the bytes written to TXDR; the one in the shift
       register was not acknowledged, and one more may wait unsent in TXDR (TXE = 0:
       TXIS asks for byte j + 1 as soon as byte j starts, 50:1170-1180), or the
       address was refused with byte 0 still there. Acknowledged = written - 1 - (TXE
       ? 0 : 1), at least 0. NACKF stays set until the STOP, which decides the status;
       NACKIE off marks it served (no RAM state). */
    if (((isr & I2C_ISR_NACKF) != 0U) && ((i2c->CR1 & I2C_CR1_NACKIE) != 0U))
    {
        if (!reading)
        {
            const uint32_t unsent = ((isr & I2C_ISR_TXE) == 0U) ? 2U : 1U;
            x->done_tx = (x->done_tx > unsent) ? (x->done_tx - unsent) : 0U;
        }
        cr1_update(i2c, I2C_CR1_NACKIE | I2C_CR1_TXIE | I2C_CR1_TCIE, 0U);
        tx_flush(i2c);
    }
    if ((isr & I2C_ISR_RXNE) != 0U)
    {
        const uint8_t b = (uint8_t)i2c->RXDR;
        if (x->done_rx < x->nrx)
        {
            x->rx[x->done_rx] = b;
        }
        x->done_rx++;
    }
    const bool nacked = (isr & I2C_ISR_NACKF) != 0U;    /* nothing more goes out after a NACK */
    if (!reading && !nacked && ((isr & I2C_ISR_TXIS) != 0U) && (x->done_tx < x->ntx))
    {
        i2c->TXDR = x->tx[x->done_tx];
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
            restart_read(i2c, x->addr7, x->nrx);
        }
        else
        {
            cr2_update(i2c, 0U, I2C_CR2_STOP);
            wait_cleared(i2c, I2C_ISR_TC);
        }
    }
    if ((isr & I2C_ISR_STOPF) != 0U)
    {
        if ((i2c->ISR & I2C_ISR_RXNE) != 0U)
        {
            return false;                           /* RXNE pending: served on the next entry */
        }
        if ((i2c->ISR & I2C_ISR_NACKF) != 0U)
        {
            return it_finish(i2c, x, I2C_BM_ERR_NACK);
        }
        const bool done = (x->done_tx == x->ntx) && (x->done_rx == x->nrx) &&
                          (reading || (x->nrx == 0U));
        return it_finish(i2c, x, done ? I2C_BM_OK : I2C_BM_ERR_BUS);
    }
    return false;
}

/* ---- DMA (plan rule 8, 50:2300-2337) ------------------------------------------------- */

I2cBm_Status_t I2C_BM_DmaStartWrite(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *buf, uint32_t n)
{
    if (buf == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if (entry(i2c) == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if (n < 2U)
    {
        return I2C_BM_ERR_PARAM;                    /* the stream carries buf[1..n-1]: at least one */
    }
    const I2cBm_Status_t st = chk_transfer(i2c, addr7, n, 0U);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    i2c->ICR = ICR_ALL;
    tx_flush(i2c);
    i2c->TXDR = buf[0];                             /* ES0445 2.19.6, also with DMA */
    cr1_update(i2c, 0U, I2C_CR1_TXDMAEN);           /* before START (50:2309-2311) */
    if ((i2c->CR1 & I2C_CR1_TXDMAEN) == 0U)
    {
        cr1_update(i2c, I2C_CR1_TXDMAEN, 0U);       /* read back before START (audit F1, m31) */
        tx_flush(i2c);
        return I2C_BM_ERR_VERIFY;
    }
    i2c->CR2 = cr2_addr(addr7, false) | cr2_chunk(n, true) | I2C_CR2_START;
    return I2C_BM_OK;
}

I2cBm_Status_t I2C_BM_DmaStartRead(I2C_TypeDef *i2c, uint32_t addr7, uint32_t n)
{
    if (entry(i2c) == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if (n == 0U)
    {
        return I2C_BM_ERR_PARAM;
    }
    const I2cBm_Status_t st = chk_transfer(i2c, addr7, 0U, n);
    if (st != I2C_BM_OK)
    {
        return st;
    }
    i2c->ICR = ICR_ALL;
    cr1_update(i2c, 0U, I2C_CR1_RXDMAEN);           /* before START (50:2326-2328) */
    if ((i2c->CR1 & I2C_CR1_RXDMAEN) == 0U)
    {
        cr1_update(i2c, I2C_CR1_RXDMAEN, 0U);       /* read back before START (audit F1, m31) */
        return I2C_BM_ERR_VERIFY;
    }
    i2c->CR2 = cr2_addr(addr7, true) | cr2_chunk(n, true) | I2C_CR2_START;
    return I2C_BM_OK;
}

I2cBm_Status_t I2C_BM_WaitEnd(I2C_TypeDef *i2c, uint32_t n, uint32_t max_cycles)
{
    if (entry(i2c) == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    if ((n == 0U) || (n > N_MAX) || (max_cycles == 0U))
    {
        return I2C_BM_ERR_PARAM;
    }
    if (!cyccnt_running())
    {
        return I2C_BM_ERR_TIMEOUT;
    }
    /* The first chunk is the one DmaStart* programmed (min(n, 255)); the rest is
       counted here, on the stack (C7). */
    uint32_t left = (n > CHUNK) ? (n - CHUNK) : 0U;
    Clock_t c;
    clk_start(&c, max_cycles);
    for (;;)
    {
        const uint32_t isr = i2c->ISR;
        if ((isr & I2C_ISR_BERR) != 0U)
        {
            i2c->ICR = I2C_ICR_BERRCF;              /* ES0445 2.19.2 */
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
            const uint32_t chunk = (left > CHUNK) ? CHUNK : left;
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

I2cBm_Status_t I2C_BM_DmaEnd(I2C_TypeDef *i2c)
{
    if (entry(i2c) == NULL)
    {
        return I2C_BM_ERR_PARAM;
    }
    cr1_update(i2c, CR1_DMA_ALL, 0U);
    xfer_close(i2c, ICR_ALL);
    return ((i2c->CR1 & CR1_DMA_ALL) == 0U) ? I2C_BM_OK : I2C_BM_ERR_VERIFY;
}

I2cBm_Status_t I2C_BM_GetDmaRequest(const I2C_TypeDef *i2c, bool tx, uint32_t *request_id)
{
    const I2cEntry_t *e = entry(i2c);
    if ((request_id == NULL) || (e == NULL))
    {
        return I2C_BM_ERR_PARAM;
    }
    *request_id = tx ? e->req_tx : e->req_rx;
    return I2C_BM_OK;
}

I2cBm_Status_t I2C_BM_GetIRQn(const I2C_TypeDef *i2c, IRQn_Type *ev, IRQn_Type *er)
{
    const I2cEntry_t *e = entry(i2c);
    if ((ev == NULL) || (er == NULL) || (e == NULL))
    {
        return I2C_BM_ERR_PARAM;
    }
    *ev = e->irq_ev;
    *er = e->irq_er;
    return I2C_BM_OK;
}

/* ---- Bus recovery (plan rule 9) ----------------------------------------------------- */

/* Waits `cycles` from now within the overall budget; false if the budget ran out
   or CYCCNT stopped. */
static bool rec_wait(Clock_t *c, uint32_t cycles)
{
    const uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < cycles)
    {
        if (clk_expired(c))
        {
            return false;
        }
    }
    return true;
}

I2cBm_Status_t I2C_BM_RecoverBus(const I2cBm_Recovery_t *r, uint32_t max_cycles, uint32_t *pulses)
{
    if ((r == NULL) || (pulses == NULL) || (r->scl_write == NULL) || (r->sda_write == NULL) ||
        (r->scl_read == NULL) || (r->sda_read == NULL))
    {
        return I2C_BM_ERR_PARAM;
    }
    if ((r->half_period_cycles == 0U) || (max_cycles == 0U))
    {
        return I2C_BM_ERR_PARAM;
    }
    *pulses = 0U;
    if (!cyccnt_running())
    {
        return I2C_BM_ERR_TIMEOUT;
    }
    if (r->sda_read(r->ctx))
    {
        return I2C_BM_OK;                           /* SDA free: nothing driven */
    }
    Clock_t c;
    clk_start(&c, max_cycles);
    const uint32_t half = r->half_period_cycles;
    r->sda_write(r->ctx, true);
    r->scl_write(r->ctx, true);
    if (!rec_wait(&c, half))
    {
        return I2C_BM_ERR_TIMEOUT;
    }
    if (!r->scl_read(r->ctx))
    {
        return I2C_BM_ERR_BUS;                      /* someone holds SCL: nothing to clock */
    }
    for (uint32_t k = 0U; (k < 9U) && !r->sda_read(r->ctx); k++)
    {
        r->scl_write(r->ctx, false);
        if (!rec_wait(&c, half))
        {
            r->scl_write(r->ctx, true);
            return I2C_BM_ERR_TIMEOUT;
        }
        r->scl_write(r->ctx, true);
        (*pulses)++;
        if (!rec_wait(&c, half))
        {
            return I2C_BM_ERR_TIMEOUT;
        }
        if (!r->scl_read(r->ctx))
        {
            return I2C_BM_ERR_BUS;                  /* released but low: held by another */
        }
    }
    if (!r->sda_read(r->ctx))
    {
        return I2C_BM_ERR_BUS;                      /* 9 pulses and SDA still low */
    }
    /* STOP: SDA low while SCL is high, then SDA high (SDA rising with SCL high). */
    r->sda_write(r->ctx, false);
    const bool w1 = rec_wait(&c, half);
    r->sda_write(r->ctx, true);
    const bool w2 = w1 && rec_wait(&c, half);
    if (!w2)
    {
        return I2C_BM_ERR_TIMEOUT;
    }
    return (r->scl_read(r->ctx) && r->sda_read(r->ctx)) ? I2C_BM_OK : I2C_BM_ERR_BUS;
}

/* ---- Errors --------------------------------------------------------------------- */

uint32_t I2C_BM_TakeErrors(I2C_TypeDef *i2c)
{
    if (entry(i2c) == NULL)
    {
        return 0U;
    }
    /* ISR and ICR share the bit positions of BERR, ARLO, OVR and TIMEOUT (50:2864-2978,
       2979-3027). */
    const uint32_t taken = i2c->ISR & (I2C_BM_ERRF_BERR | I2C_BM_ERRF_ARLO | I2C_BM_ERRF_OVR | I2C_BM_ERRF_TIMEOUT);
    if (taken != 0U)
    {
        i2c->ICR = taken;
        __DSB();
    }
    return taken;
}
