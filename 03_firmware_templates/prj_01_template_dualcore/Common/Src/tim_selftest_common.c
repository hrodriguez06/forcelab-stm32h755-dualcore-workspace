/**
  ******************************************************************************
  * @file    tim_selftest_common.c
  * @brief   03_tim selftest, code both cores compile: the bench step table and
  *          the line rules of bench_claim() (Common/Inc/tim_selftest.h).
  *
  *          Pure tables and logic: nothing here touches a register. Each core's
  *          App/Src/tim_selftest.c owns its writes, and calls tst_line_rule()
  *          from its bench_claim() before any pin becomes an output or AF.
  ******************************************************************************
  */

#include "tim_selftest.h"
#include "gpio_bm.h"

#include <string.h>

/* Bench lines (00_gpio README section 8, 03_tim plan "Uso de las lineas"). */
#define PE_REQ     2U       /* DIO12 HOST_REQ, AD2 only                         */
#define PE_DIO8    3U       /* DIO8, AD2 only (TIM15_BKIN in AF4: never)        */
#define PE_DIO10   4U       /* DIO10, AD2 only (TIM15_CH1N in AF4: never)       */
#define PE_T15C1   5U       /* DIO9, MCU only: TIM15_CH1 (AF4), CPU2            */
#define PE_T15C2   6U       /* DIO11, MCU only: TIM15_CH2 (AF4), CPU2           */
#define PE_ACK     7U       /* DIO13 MCU_ACK, MCU only, owned by bench_safe/ack */
#define PA_MCO     8U       /* DIO14, MCU only: MCO1 (AF0), CPU1                */

#define PE_BUS_MASK      0xFF00U                                       /* PE8..PE15 = DIO0..DIO7 */
#define PE_BUS_TIM1      ((1U << 9) | (1U << 11) | (1U << 13) | (1U << 14))  /* TIM1_CH1..CH4 */
#define PE_BUS_CAPTURE   ((1U << 9) | (1U << 11))                     /* S12: AF while the AD2 drives */

#define E(n)   (1U << (n))

/* The single table of permissions (S4). Plan "Banco de pruebas": PE8, PE10,
   PE12 and PE15 are never in AF; PE9/PE11 in AF1 with the AD2 driving only
   under S12; PE5/PE6 in AF4 only on CPU2; PA8 in AF0 only on CPU1, and in AF1
   (TIM1_CH1, TIM1 stopped) only for the TREF control. PA8 is an MCU-only
   line: the AD2 never drives DIO14. */
const TstStep_t k_tst_steps[] = {
    /* name            bus            out_pe        af_pe            af_pa       af1_pa     */
    { "T0A",           TST_BUS_NONE,  0U,           0U,              0U,         0U         },
    { "TREF",          TST_BUS_NONE,  0U,           0U,              E(PA_MCO),  E(PA_MCO)  },  /* control: PA8 AF1 */
    { "T0B",           TST_BUS_NONE,  0U,           E(PE_T15C1),     E(PA_MCO),  0U         },  /* the accepted controls */
    { "T0B_S5NEG",     TST_BUS_MCU,   PE_BUS_MASK,  0U,              0U,         0U         },  /* 00_gpio T0b(c) */
    { "T0B_TIMEOUT",   TST_BUS_NONE,  0U,           0U,              0U,         0U         },
    { "T0B_DOUT",      TST_BUS_AD2,   0U,           0U,              0U,         0U         },  /* S11: MCU inputs only */
    { "T0B_S12",       TST_BUS_AD2,   0U,           PE_BUS_CAPTURE,  0U,         0U         },  /* S12 */
    { "DONE",          TST_BUS_NONE,  0U,           0U,              0U,         0U         },
};

const uint32_t k_tst_step_count = (uint32_t)(sizeof(k_tst_steps) / sizeof(k_tst_steps[0]));

uint32_t tst_step_index(const char *name)
{
    for (uint32_t i = 0U; i < k_tst_step_count; i++)
    {
        if (strcmp(k_tst_steps[i].name, name) == 0)
        {
            return i;
        }
    }
    return TST_STEP_NONE;
}

static bool rule_gpioe(uint32_t core, const TstStep_t *s, uint32_t pin, uint32_t mode, uint32_t af)
{
    const uint16_t bit = (uint16_t)(1U << pin);

    if ((pin <= 1U) || (pin == PE_REQ) || (pin == PE_DIO8) || (pin == PE_DIO10) || (pin == PE_ACK))
    {
        return false;   /* PE0 (R4), PE1 (LD2), the AD2-only lines (PE4 in AF4 = TIM15_CH1N), MCU_ACK */
    }
    if ((pin == PE_T15C1) || (pin == PE_T15C2))
    {
        /* MCU only, TIM15 of CPU2: AF4 and nothing else, never from CPU1. */
        return (core == 1U) && (mode == GPIO_BM_MODE_AF) && (af == TST_AF_TIM15) && ((s->af_pe & bit) != 0U);
    }
    /* The bus, PE8..PE15: CPU1 only. */
    if (core != 0U)
    {
        return false;
    }
    if (mode == GPIO_BM_MODE_OUTPUT)
    {
        return (s->bus == (uint8_t)TST_BUS_MCU) && ((s->out_pe & bit) != 0U);
    }
    /* AF: only TIM1_CH1..CH4 in AF1 (PE8, PE10, PE12, PE15 never). */
    if ((af != TST_AF_TIM1) || ((PE_BUS_TIM1 & bit) == 0U) || ((s->af_pe & bit) == 0U))
    {
        return false;
    }
    if (s->bus == (uint8_t)TST_BUS_MCU)
    {
        return true;
    }
    /* "bus = AD2": only the two capture inputs, and only as an S12 candidate. */
    return (s->bus == (uint8_t)TST_BUS_AD2) && ((PE_BUS_CAPTURE & bit) != 0U);
}

bool tst_line_rule(uint32_t core, uint32_t step, const GPIO_TypeDef *port, uint32_t pin,
                   uint32_t mode, uint32_t af)
{
    if ((pin > 15U) || ((mode != GPIO_BM_MODE_OUTPUT) && (mode != GPIO_BM_MODE_AF)))
    {
        return false;
    }
    if ((port == GPIOC) && (pin == 13U))
    {
        return false;   /* PC13 (user button): never touched by this selftest (R4) */
    }
    if ((port == GPIOA) && ((pin == 13U) || (pin == 14U)))
    {
        return false;   /* SWD */
    }
    if ((port == GPIOF) && ((pin == 14U) || (pin == 15U)))
    {
        return false;   /* scope lines with their 22k loads: not used by the TIM bench */
    }
    const bool wired = (port == GPIOE) || ((port == GPIOA) && (pin == PA_MCO));
    if (!wired)
    {
        return true;    /* nothing outside drives an unwired pin */
    }
    if (step >= k_tst_step_count)
    {
        return false;   /* between steps nothing wired may be driven */
    }
    const TstStep_t *s = &k_tst_steps[step];
    if (port == GPIOA)
    {
        /* PA8, CPU1 only: MCO1 in AF0, or TIM1_CH1 in AF1 for the TREF control. */
        if ((core != 0U) || (mode != GPIO_BM_MODE_AF))
        {
            return false;
        }
        return ((af == TST_AF_MCO1) && ((s->af_pa & (1U << PA_MCO)) != 0U)) ||
               ((af == TST_AF_TIM1) && ((s->af1_pa & (1U << PA_MCO)) != 0U));
    }
    return rule_gpioe(core, s, pin, mode, af);
}
