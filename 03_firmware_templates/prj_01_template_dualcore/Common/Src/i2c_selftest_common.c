/**
  ******************************************************************************
  * @file    i2c_selftest_common.c
  * @brief   05_i2c selftest, code both cores compile: the bench step table and
  *          the line rules of bench_claim() (Common/Inc/i2c_selftest.h).
  *
  *          Pure tables and logic: nothing here touches a register. Each core's
  *          App/Src/i2c_selftest.c owns its writes, and calls ist_line_rule()
  *          and, for a bus pin in AF, ist_s16_pins_ok() on the registers it
  *          read and its own instance check, from its bench_claim(), before any
  *          pin becomes an output or AF or gets a pull.
  ******************************************************************************
  */

#include "i2c_selftest.h"
#include "gpio_bm.h"

#include <string.h>

/* The single table of permissions (S4, S15, S16). Plan "Banco de pruebas":
   in F0 the H755 never drives the bus; the bus pins go to AF (with the owner
   instance at PE = 0) only in T0B (CPU2, AF4) and T0B_CM7 (CPU1, AF6). */
const IstStep_t k_ist_steps[] = {
    /* name            owner         af  od_out */
    { "T0A",           IST_OWN_NONE, 0U, 0U },
    { "T0C",           IST_OWN_NONE, 0U, 0U },   /* levels and the F334's holds: inputs only */
    { "T0B",           IST_OWN_CM4,  1U, 0U },   /* claims of both cores; CPU2 writes AF4    */
    { "T0B_CM7",       IST_OWN_CM7,  1U, 0U },   /* CPU1 claims and writes AF6               */
    { "T0B_TIMEOUT",   IST_OWN_NONE, 0U, 0U },
    { "DONE",          IST_OWN_NONE, 0U, 0U },
};

const uint32_t k_ist_step_count = (uint32_t)(sizeof(k_ist_steps) / sizeof(k_ist_steps[0]));

uint32_t ist_step_index(const char *name)
{
    for (uint32_t i = 0U; i < k_ist_step_count; i++)
    {
        if (strcmp(k_ist_steps[i].name, name) == 0)
        {
            return i;
        }
    }
    return IST_STEP_NONE;
}

static bool rule_bus(uint32_t core, uint32_t step, uint32_t mode, uint32_t af, uint32_t otype, uint32_t pull)
{
    if (pull != GPIO_BM_PULL_NONE)
    {
        return false;                                   /* S15: never a pull           */
    }
    if (mode == GPIO_BM_MODE_INPUT)
    {
        return true;
    }
    if (otype != GPIO_BM_OTYPE_OD)
    {
        return false;                                   /* S15: never push-pull         */
    }
    if (step >= k_ist_step_count)
    {
        return false;                                   /* between steps: inputs only   */
    }
    const IstStep_t *s = &k_ist_steps[step];
    const uint32_t owner = (core == 0U) ? (uint32_t)IST_OWN_CM7 : (uint32_t)IST_OWN_CM4;
    if (s->owner != owner)
    {
        return false;                                   /* S16: the step's core only    */
    }
    if (mode == GPIO_BM_MODE_AF)
    {
        return (s->af != 0U) && (af == ((core == 0U) ? IST_AF_I2C4 : IST_AF_I2C1));
    }
    return s->od_out != 0U;                             /* GPIO open-drain: recovery    */
}

bool ist_line_rule(uint32_t core, uint32_t step, const GPIO_TypeDef *port, uint32_t pin,
                   uint32_t mode, uint32_t af, uint32_t otype, uint32_t pull)
{
    if ((pin > 15U) || (core > 1U) ||
        ((mode != GPIO_BM_MODE_INPUT) && (mode != GPIO_BM_MODE_OUTPUT) && (mode != GPIO_BM_MODE_AF)))
    {
        return false;
    }
    if ((port == GPIOB) && ((pin == IST_PIN_SCL) || (pin == IST_PIN_SDA)))
    {
        return rule_bus(core, step, mode, af, otype, pull);
    }
    if ((port == GPIOF) && ((pin == IST_PIN_TAP_SCL) || (pin == IST_PIN_TAP_SDA)))
    {
        /* S15: the taps are inputs without pull; never AF4 (I2C4_SCL/SDA). */
        return (mode == GPIO_BM_MODE_INPUT) && (pull == GPIO_BM_PULL_NONE);
    }
    /* Any other pin: an input drives nothing (T0A pulls the AD2 lines). No other
       pin of this bench is ever an output or AF: not the AD2 lines (S4), not
       SPI4 on PE11..PE14 (S13), not the J1 pins PA5/PA6/PB4/PB5 (S14), not
       PE0, PE1, PC13, PA4, PA7, PA13, PA14. MCU_ACK (PE7) is driven by ack()
       directly and never claimed. */
    return mode == GPIO_BM_MODE_INPUT;
}

/* MODER and AFR[1] images with the PB8 and PB9 fields given, the rest 0. */
#define IMG_M(m8, m9)    (((uint32_t)(m8) << (IST_PIN_SCL * 2U)) | ((uint32_t)(m9) << (IST_PIN_SDA * 2U)))
#define IMG_A(a8, a9)    (((uint32_t)(a8) << ((IST_PIN_SCL - 8U) * 4U)) | ((uint32_t)(a9) << ((IST_PIN_SDA - 8U) * 4U)))

uint32_t ist_s16_cases(uint32_t af, uint32_t other_af)
{
    const uint32_t I = GPIO_BM_MODE_INPUT;
    const uint32_t O = GPIO_BM_MODE_OUTPUT;
    const uint32_t A = GPIO_BM_MODE_AF;
    const uint32_t N = GPIO_BM_MODE_ANALOG;
    const uint32_t img[IST_S16_CASES][2] = {
        { IMG_M(I, A), IMG_A(0U, other_af) },     /* 0 PB9 in the other core's AF: refuse  */
        { IMG_M(I, O), IMG_A(0U, 0U) },           /* 1 PB9 a GPIO output: refuse           */
        { IMG_M(A, A), IMG_A(other_af, af) },     /* 2 PB8 in the other AF: refuse         */
        { IMG_M(O, I), IMG_A(0U, 0U) },           /* 3 PB8 a GPIO output: refuse           */
        { IMG_M(I, I), IMG_A(0U, 0U) },           /* 4 both input: accept                  */
        { IMG_M(N, N), IMG_A(0U, 0U) },           /* 5 both analog (reset): accept         */
        { IMG_M(A, A), IMG_A(af, af) },           /* 6 both already in this AF: accept     */
    };
    uint32_t acc = 0U;
    for (uint32_t k = 0U; k < IST_S16_CASES; k++)
    {
        if (ist_s16_pins_ok(img[k][0], img[k][1], af))
        {
            acc |= 1UL << k;
        }
    }
    return acc;
}

bool ist_s16_pins_ok(uint32_t moder, uint32_t afrh, uint32_t af)
{
    const uint32_t pins[2] = { IST_PIN_SCL, IST_PIN_SDA };
    for (uint32_t k = 0U; k < 2U; k++)
    {
        const uint32_t m = (moder >> (pins[k] * 2U)) & 3UL;
        const uint32_t a = (afrh >> ((pins[k] - 8U) * 4U)) & 0xFUL;
        if ((m == GPIO_BM_MODE_OUTPUT) || ((m == GPIO_BM_MODE_AF) && (a != af)))
        {
            return false;
        }
    }
    return true;
}
