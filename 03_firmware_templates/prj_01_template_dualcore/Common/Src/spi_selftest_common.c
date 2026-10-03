/**
  ******************************************************************************
  * @file    spi_selftest_common.c
  * @brief   06_spi selftest, code both cores compile: the bench step table and
  *          the line rules of bench_claim() (Common/Inc/spi_selftest.h).
  *
  *          Pure tables and logic: nothing here touches a register. Each core's
  *          App/Src/spi_selftest.c owns its writes, and calls sst_line_rule()
  *          from its bench_claim() before any pin becomes an output or AF.
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
