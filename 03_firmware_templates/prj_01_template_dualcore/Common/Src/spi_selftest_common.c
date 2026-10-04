/**
  ******************************************************************************
  * @file    spi_selftest_common.c
  * @brief   06_spi selftest, code both cores compile: the bench step table, the
  *          line rules of bench_claim() and the "SPI at rest" rule
  *          (Common/Inc/spi_selftest.h).
  *
  *          Pure tables and logic: nothing here touches a register. Each core's
  *          App/Src/spi_selftest.c owns its writes, and calls sst_line_rule()
  *          (and, for an SPI pin in AF, sst_spi_at_rest() on the registers it
  *          read) from its bench_claim() before any pin becomes an output or AF.
  ******************************************************************************
  */

#include "spi_selftest.h"
#include "gpio_bm.h"

#include <string.h>

/* Bench lines (00_gpio README section 8; 06_spi plan "Uso de las lineas"). */
#define PE_ACK       7U      /* DIO13 MCU_ACK: driven by bench_safe()/ack() only, never claimed */
#define PE_BUS_MASK  0xFF00U                                         /* PE8..PE15 = DIO0..DIO7 */
#define PE_SPI4      ((1U << 11) | (1U << 12) | (1U << 13) | (1U << 14))   /* NSS, SCK, MISO, MOSI */
#define PA_MCO       8U      /* DIO14, MCU only: MCO1 (AF0), CPU1                             */
#define PA_J1_SCK    5U      /* CN7-10 (D13), unwired: SCK of SPI1/SPI6                       */
#define PA_J1_MISO   6U      /* CN7-12 (D12), J1-B: never an output (S14)                     */
#define PB_J1_MISO   4U      /* CN7-19, unwired: the D2 control MISO, never an output (S14)    */
#define PB_J1_MOSI   5U      /* CN7-14 (D11), J1-A: GPIO output in T0C only (S14)             */

#define E(n)   (1U << (n))

/* The single table of permissions (S4, S13, S14). Plan "Banco de pruebas":
   PE11..PE14 in AF5 only in a "bus = MCU" step (and only after CPU1 has read
   SPI4 back), PE8..PE10 and PE15 never in AF, PE0..PE7 never in AF nor output;
   the J1 pins belong to one core per step, PB5 is a GPIO output only in T0C.
   PA8 in AF0 is the accepted control of T0B(b) on CPU1 (MCO1 is not started:
   the claim is judged, nothing is written). */
const SstStep_t k_sst_steps[] = {
    /* name           bus           j1           j1_af pb5_out out_pe       af5_pe   af0_pa     */
    { "T0A",          SST_BUS_NONE, SST_J1_NONE, 0U,   0U,     0U,          0U,      0U         },
    { "T0C",          SST_BUS_NONE, SST_J1_CM7,  0U,   1U,     0U,          0U,      0U         },  /* J1 check */
    { "T0B",          SST_BUS_NONE, SST_J1_CM4,  1U,   0U,     0U,          0U,      E(PA_MCO)  },  /* (b), CPU2 J1 */
    { "T0B_S5NEG",    SST_BUS_MCU,  SST_J1_NONE, 0U,   0U,     PE_BUS_MASK, 0U,      0U         },  /* (c) */
    { "T0B_TIMEOUT",  SST_BUS_NONE, SST_J1_NONE, 0U,   0U,     0U,          0U,      0U         },  /* (d) */
    { "T0B_S13",      SST_BUS_MCU,  SST_J1_NONE, 0U,   0U,     0U,          PE_SPI4, 0U         },  /* (e) */
    { "T0B_S14",      SST_BUS_NONE, SST_J1_CM7,  1U,   0U,     0U,          0U,      0U         },  /* (f) CPU1 J1 */
    /* Phase F1, spi_bm. F1_NOPIN takes no pin: SPI2, SPI3, SPI5 have none on the bench,
       and SPI1, SPI4, SPI6 run there with their pins in input. J1_SPI6 is CPU1's J1 step
       (SPI6, AF8), J1_SPI1 CPU2's (SPI1, AF5). SPI4_AD2 puts PE11..PE14 in AF5 (S13) and
       PA8 in AF0 (MCO1, T5(g)), the AD2 only listening. */
    { "F1_NOPIN",     SST_BUS_NONE, SST_J1_NONE, 0U,   0U,     0U,          0U,      0U         },
    { "J1_SPI6",      SST_BUS_NONE, SST_J1_CM7,  1U,   0U,     0U,          0U,      0U         },
    { "J1_SPI1",      SST_BUS_NONE, SST_J1_CM4,  1U,   0U,     0U,          0U,      0U         },
    { "SPI4_AD2",     SST_BUS_MCU,  SST_J1_NONE, 0U,   0U,     0U,          PE_SPI4, E(PA_MCO)  },
    /* Phase F2, spi_ll (and T15 with spi_bm): the same owners as the F1 steps they
       mirror. SPI4_AD2_LL needs no MCO1. */
    { "F2_NOPIN",     SST_BUS_NONE, SST_J1_NONE, 0U,   0U,     0U,          0U,      0U         },
    { "J1_SPI6_LL",   SST_BUS_NONE, SST_J1_CM7,  1U,   0U,     0U,          0U,      0U         },
    { "J1_SPI1_LL",   SST_BUS_NONE, SST_J1_CM4,  1U,   0U,     0U,          0U,      0U         },
    { "SPI4_AD2_LL",  SST_BUS_MCU,  SST_J1_NONE, 0U,   0U,     0U,          PE_SPI4, 0U         },
    { "DONE",         SST_BUS_NONE, SST_J1_NONE, 0U,   0U,     0U,          0U,      0U         },
};

const uint32_t k_sst_step_count = (uint32_t)(sizeof(k_sst_steps) / sizeof(k_sst_steps[0]));

uint32_t sst_step_index(const char *name)
{
    for (uint32_t i = 0U; i < k_sst_step_count; i++)
    {
        if (strcmp(k_sst_steps[i].name, name) == 0)
        {
            return i;
        }
    }
    return SST_STEP_NONE;
}

/* PE8..PE15 (the AD2 bus), CPU1 only. */
bool sst_spi_at_rest(uint32_t cr1, uint32_t cfg2)
{
    if (((cr1 & SPI_CR1_SPE) != 0U) || ((cfg2 & (SPI_CFG2_AFCNTR | SPI_CFG2_IOSWP)) != 0U))
    {
        return false;
    }
    return ((cfg2 & SPI_CFG2_MASTER) != 0U) || (cfg2 == 0U);
}

static bool rule_bus(uint32_t core, const SstStep_t *s, uint32_t pin, uint32_t mode, uint32_t af)
{
    const uint16_t bit = (uint16_t)(1U << pin);
    if ((core != 0U) || (s->bus != (uint8_t)SST_BUS_MCU))
    {
        return false;
    }
    if (mode == GPIO_BM_MODE_OUTPUT)
    {
        return (s->out_pe & bit) != 0U;
    }
    /* AF: SPI4 in AF5 on PE11..PE14 only (S13); PE8..PE10, PE15 never. */
    return (af == SST_AF_SPI4) && ((PE_SPI4 & bit) != 0U) && ((s->af5_pe & bit) != 0U);
}

/* PA5, PA6, PB4, PB5: one core per step (S14). */
static bool rule_j1(uint32_t core, const SstStep_t *s, bool is_pb5, uint32_t mode, uint32_t af)
{
    const uint32_t owner = (core == 0U) ? (uint32_t)SST_J1_CM7 : (uint32_t)SST_J1_CM4;
    if (s->j1 != (uint8_t)owner)
    {
        return false;
    }
    if (mode == GPIO_BM_MODE_OUTPUT)
    {
        return is_pb5 && (s->pb5_out != 0U);      /* PA5, PA6, PB4 never; PB5 only in T0C */
    }
    return (s->j1_af != 0U) && (af == ((core == 0U) ? SST_AF_SPI6 : SST_AF_SPI1));
}

bool sst_line_rule(uint32_t core, uint32_t step, const GPIO_TypeDef *port, uint32_t pin,
                   uint32_t mode, uint32_t af)
{
    if ((pin > 15U) || ((mode != GPIO_BM_MODE_OUTPUT) && (mode != GPIO_BM_MODE_AF)) || (core > 1U))
    {
        return false;
    }
    if (step >= k_sst_step_count)
    {
        return false;   /* between steps nothing may be driven */
    }
    const SstStep_t *s = &k_sst_steps[step];
    if (port == GPIOE)
    {
        /* PE0 (R4), PE1 (LD2), PE2 HOST_REQ, PE3/PE4 AD2 only, PE5/PE6 unused
           MCU-only lines, PE7 MCU_ACK: never AF nor output (S13; PE2/PE4/PE5/PE6
           in AF5 would be the second SPI4 set). */
        if (pin <= PE_ACK)
        {
            return false;
        }
        return rule_bus(core, s, pin, mode, af);
    }
    if (port == GPIOA)
    {
        if ((pin == PA_J1_SCK) || (pin == PA_J1_MISO))
        {
            return rule_j1(core, s, false, mode, af);
        }
        if (pin == PA_MCO)
        {
            return (core == 0U) && (mode == GPIO_BM_MODE_AF) && (af == SST_AF_MCO1) &&
                   ((s->af0_pa & (1U << PA_MCO)) != 0U);
        }
        return false;   /* PA4 (R4), PA7 (RMII_CRS_DV), PA13/PA14 (SWD), the rest: not bench lines */
    }
    if ((port == GPIOB) && ((pin == PB_J1_MISO) || (pin == PB_J1_MOSI)))
    {
        return rule_j1(core, s, pin == PB_J1_MOSI, mode, af);
    }
    return false;       /* PC13 (R4), PG13 (RMII_TXD0), PF14/PF15 (scope loads), any other pin */
}

/* ==========================================================================
 * Data pattern and oracles (phase F1). Pure: buffers and arithmetic only.
 * ========================================================================== */

uint32_t sst_mask(uint32_t bits)
{
    return (bits >= 32U) ? 0xFFFFFFFFUL : ((1UL << bits) - 1U);
}

uint32_t sst_elem_bytes(uint32_t bits)
{
    return (bits <= 8U) ? 1U : ((bits <= 16U) ? 2U : 4U);
}

static void put_elem(void *buf, uint32_t es, uint32_t i, uint32_t v)
{
    if (es == 1U)
    {
        ((uint8_t *)buf)[i] = (uint8_t)v;
    }
    else if (es == 2U)
    {
        ((uint16_t *)buf)[i] = (uint16_t)v;
    }
    else
    {
        ((uint32_t *)buf)[i] = v;
    }
}

uint32_t sst_get(const void *buf, uint32_t es, uint32_t i)
{
    if (es == 1U)
    {
        return ((const uint8_t *)buf)[i];
    }
    if (es == 2U)
    {
        return ((const uint16_t *)buf)[i];
    }
    return ((const uint32_t *)buf)[i];
}

void sst_fill(void *buf, uint32_t es, uint32_t frames, uint32_t bits, uint32_t seed)
{
    const uint32_t m = sst_mask(bits);
    uint32_t x = seed;
    for (uint32_t i = 0U; i < frames; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        put_elem(buf, es, i, x & m);
    }
}

void sst_fill_const(void *buf, uint32_t es, uint32_t frames, uint32_t value)
{
    for (uint32_t i = 0U; i < frames; i++)
    {
        put_elem(buf, es, i, value);
    }
}

uint32_t sst_crc32_frames(const void *buf, uint32_t es, uint32_t frames, uint32_t bits)
{
    const uint32_t m = sst_mask(bits);
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0U; i < frames; i++)
    {
        const uint32_t v = sst_get(buf, es, i) & m;
        for (uint32_t b = 0U; b < 4U; b++)
        {
            crc ^= (v >> (8U * b)) & 0xFFUL;
            for (uint32_t k = 0U; k < 8U; k++)
            {
                crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
            }
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

uint32_t sst_mismatch(const void *a, const void *b, uint32_t es, uint32_t frames, uint32_t bits)
{
    const uint32_t m = sst_mask(bits);
    uint32_t n = 0U;
    for (uint32_t i = 0U; i < frames; i++)
    {
        if (((sst_get(a, es, i) ^ sst_get(b, es, i)) & m) != 0U)
        {
            n++;
        }
    }
    return n;
}

uint32_t sst_coverage(const void *buf, uint32_t es, uint32_t frames, uint32_t bits)
{
    const uint32_t m = sst_mask(bits);
    uint32_t ones = 0U;
    uint32_t zeros = 0U;
    for (uint32_t i = 0U; i < frames; i++)
    {
        const uint32_t v = sst_get(buf, es, i) & m;
        ones |= v;
        zeros |= ~v & m;
    }
    return ones & zeros;
}

/* RM0399 rev 4, 53.11.1 SPI_CR1 (SSI bit 12), 53.11.3 SPI_CFG1 (MBR 30:28, CRCEN 22,
   CRCSIZE 20:16, TXDMAEN 15, RXDMAEN 14, UDRDET 12:11, UDRCFG 10:9, FTHLV 8:5, DSIZE
   4:0) and 53.11.4 SPI_CFG2 (AFCNTR 31, SSOM 30, SSOE 29, SSIOP 28, SSM 26, CPOL 25,
   CPHA 24, LSBFRST 23, MASTER 22, SP 21:19, COMM 18:17, IOSWP 15, MIDI 7:4, MSSI
   3:0). The positions are typed here from the register drawings; the encodings are
   the RM's: DSIZE = bits - 1, FTHLV = frames - 1, CRCSIZE kept at 00111 (8 bits,
   ES0445 2.22.4), COMM 00 full duplex / 01 transmitter, SP 000 Motorola. */
void sst_t2_expect(const SpiBm_Config_t *cfg, uint32_t *cfg1, uint32_t *cfg2, uint32_t *cr1)
{
    *cfg1 = (cfg->mbr << 28) | (7UL << 16) | ((cfg->fifo_threshold - 1U) << 5) | (cfg->data_bits - 1U);
    uint32_t v = (1UL << 22) | (cfg->direction << 17) | (cfg->midi << 4) | cfg->mssi;   /* MASTER */
    v |= ((cfg->mode >> 1) & 1U) << 25;                                               /* CPOL   */
    v |= (cfg->mode & 1U) << 24;                                                      /* CPHA   */
    v |= (cfg->lsb_first ? 1UL : 0UL) << 23;
    v |= (cfg->nss_active_high ? 1UL : 0UL) << 28;
    if (cfg->nss == SPI_BM_NSS_SOFT)
    {
        v |= 1UL << 26;                                                               /* SSM    */
    }
    else
    {
        v |= 1UL << 29;                                                               /* SSOE   */
        if (cfg->nss == SPI_BM_NSS_HW_PULSE)
        {
            v |= 1UL << 30;                                                           /* SSOM   */
        }
    }
    if (cfg->keep_pins)
    {
        v |= 1UL << 31;                                                               /* AFCNTR */
    }
    *cfg2 = v;
    *cr1 = cfg->nss_active_high ? 0U : (1UL << 12);    /* SSI inactive: SSI != SSIOP (53:566-567) */
}

/* ==========================================================================
 * The spi_bm -> spi_ll configuration of the F2 tests (pure). Written here by
 * hand from the constants of stm32h7xx_ll_spi.h, not taken from spi_ll.c: the
 * LL tier's own translation is what T1 and T2 test, so it cannot be the oracle.
 * ========================================================================== */

static const uint32_t k_ll_mbr[8] = {
    LL_SPI_BAUDRATEPRESCALER_DIV2,  LL_SPI_BAUDRATEPRESCALER_DIV4,  LL_SPI_BAUDRATEPRESCALER_DIV8,
    LL_SPI_BAUDRATEPRESCALER_DIV16, LL_SPI_BAUDRATEPRESCALER_DIV32, LL_SPI_BAUDRATEPRESCALER_DIV64,
    LL_SPI_BAUDRATEPRESCALER_DIV128, LL_SPI_BAUDRATEPRESCALER_DIV256 };

static const uint32_t k_ll_width[29] = {          /* 4 .. 32 bits */
    LL_SPI_DATAWIDTH_4BIT,  LL_SPI_DATAWIDTH_5BIT,  LL_SPI_DATAWIDTH_6BIT,  LL_SPI_DATAWIDTH_7BIT,
    LL_SPI_DATAWIDTH_8BIT,  LL_SPI_DATAWIDTH_9BIT,  LL_SPI_DATAWIDTH_10BIT, LL_SPI_DATAWIDTH_11BIT,
    LL_SPI_DATAWIDTH_12BIT, LL_SPI_DATAWIDTH_13BIT, LL_SPI_DATAWIDTH_14BIT, LL_SPI_DATAWIDTH_15BIT,
    LL_SPI_DATAWIDTH_16BIT, LL_SPI_DATAWIDTH_17BIT, LL_SPI_DATAWIDTH_18BIT, LL_SPI_DATAWIDTH_19BIT,
    LL_SPI_DATAWIDTH_20BIT, LL_SPI_DATAWIDTH_21BIT, LL_SPI_DATAWIDTH_22BIT, LL_SPI_DATAWIDTH_23BIT,
    LL_SPI_DATAWIDTH_24BIT, LL_SPI_DATAWIDTH_25BIT, LL_SPI_DATAWIDTH_26BIT, LL_SPI_DATAWIDTH_27BIT,
    LL_SPI_DATAWIDTH_28BIT, LL_SPI_DATAWIDTH_29BIT, LL_SPI_DATAWIDTH_30BIT, LL_SPI_DATAWIDTH_31BIT,
    LL_SPI_DATAWIDTH_32BIT };

static const uint32_t k_ll_fifo[16] = {
    LL_SPI_FIFO_TH_01DATA, LL_SPI_FIFO_TH_02DATA, LL_SPI_FIFO_TH_03DATA, LL_SPI_FIFO_TH_04DATA,
    LL_SPI_FIFO_TH_05DATA, LL_SPI_FIFO_TH_06DATA, LL_SPI_FIFO_TH_07DATA, LL_SPI_FIFO_TH_08DATA,
    LL_SPI_FIFO_TH_09DATA, LL_SPI_FIFO_TH_10DATA, LL_SPI_FIFO_TH_11DATA, LL_SPI_FIFO_TH_12DATA,
    LL_SPI_FIFO_TH_13DATA, LL_SPI_FIFO_TH_14DATA, LL_SPI_FIFO_TH_15DATA, LL_SPI_FIFO_TH_16DATA };

static const uint32_t k_ll_mssi[16] = {
    LL_SPI_SS_IDLENESS_00CYCLE, LL_SPI_SS_IDLENESS_01CYCLE, LL_SPI_SS_IDLENESS_02CYCLE, LL_SPI_SS_IDLENESS_03CYCLE,
    LL_SPI_SS_IDLENESS_04CYCLE, LL_SPI_SS_IDLENESS_05CYCLE, LL_SPI_SS_IDLENESS_06CYCLE, LL_SPI_SS_IDLENESS_07CYCLE,
    LL_SPI_SS_IDLENESS_08CYCLE, LL_SPI_SS_IDLENESS_09CYCLE, LL_SPI_SS_IDLENESS_10CYCLE, LL_SPI_SS_IDLENESS_11CYCLE,
    LL_SPI_SS_IDLENESS_12CYCLE, LL_SPI_SS_IDLENESS_13CYCLE, LL_SPI_SS_IDLENESS_14CYCLE, LL_SPI_SS_IDLENESS_15CYCLE };

static const uint32_t k_ll_midi[16] = {
    LL_SPI_ID_IDLENESS_00CYCLE, LL_SPI_ID_IDLENESS_01CYCLE, LL_SPI_ID_IDLENESS_02CYCLE, LL_SPI_ID_IDLENESS_03CYCLE,
    LL_SPI_ID_IDLENESS_04CYCLE, LL_SPI_ID_IDLENESS_05CYCLE, LL_SPI_ID_IDLENESS_06CYCLE, LL_SPI_ID_IDLENESS_07CYCLE,
    LL_SPI_ID_IDLENESS_08CYCLE, LL_SPI_ID_IDLENESS_09CYCLE, LL_SPI_ID_IDLENESS_10CYCLE, LL_SPI_ID_IDLENESS_11CYCLE,
    LL_SPI_ID_IDLENESS_12CYCLE, LL_SPI_ID_IDLENESS_13CYCLE, LL_SPI_ID_IDLENESS_14CYCLE, LL_SPI_ID_IDLENESS_15CYCLE };

void sst_ll_of(const SpiBm_Config_t *b, SpiLl_Config_t *l)
{
    l->spi                = b->spi;
    l->baudrate           = k_ll_mbr[b->mbr & 7U];
    l->clock_polarity     = ((b->mode & 2U) != 0U) ? LL_SPI_POLARITY_HIGH : LL_SPI_POLARITY_LOW;
    l->clock_phase        = ((b->mode & 1U) != 0U) ? LL_SPI_PHASE_2EDGE : LL_SPI_PHASE_1EDGE;
    l->data_width         = ((b->data_bits >= 4U) && (b->data_bits <= 32U)) ? k_ll_width[b->data_bits - 4U] : 0U;
    l->bit_order          = b->lsb_first ? LL_SPI_LSB_FIRST : LL_SPI_MSB_FIRST;
    l->transfer_direction = (b->direction == SPI_BM_DIR_TX_ONLY) ? LL_SPI_SIMPLEX_TX : LL_SPI_FULL_DUPLEX;
    l->fifo_threshold     = ((b->fifo_threshold >= 1U) && (b->fifo_threshold <= 16U)) ?
                            k_ll_fifo[b->fifo_threshold - 1U] : 0U;
    l->nss                = (b->nss == SPI_BM_NSS_SOFT) ? LL_SPI_NSS_SOFT : LL_SPI_NSS_HARD_OUTPUT;
    l->nss_pulse          = (b->nss == SPI_BM_NSS_HW_PULSE);
    l->nss_polarity       = b->nss_active_high ? LL_SPI_NSS_POLARITY_HIGH : LL_SPI_NSS_POLARITY_LOW;
    l->ss_idleness        = k_ll_mssi[b->mssi & 15U];
    l->interdata_idleness = k_ll_midi[b->midi & 15U];
    l->keep_pins          = b->keep_pins;
}
