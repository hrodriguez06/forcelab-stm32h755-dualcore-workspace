/**
  ******************************************************************************
  * @file    uart_selftest_common.c
  * @brief   04_uart selftest: step table and line rules for bench_claim().
  *
  *          Pure logic shared by both cores:
  *          - k_ust_steps: authoritative list of steps and allowed pin owners.
  *          - ust_line_rule: validates pin claim against current step (S4, SU4).
  ******************************************************************************
  */

#include "uart_selftest.h"
#include "gpio_bm.h"

#include <string.h>

const UstStep_t k_ust_steps[] = {
    { "UT0",       UST_OWN_NONE, 0U },
    { "UT0_CTRL",  UST_OWN_NONE, (1U << UART_PIN_TX) }, /* PD5 output low for negative control */
    { "UT1",       UST_OWN_NONE, 0U },
    { "UT2",       UST_OWN_NONE, 0U },
    { "UT3",       UST_OWN_CM7,  (1U << UART_PIN_TX) | (1U << UART_PIN_RX) },
    { "UT4",       UST_OWN_CM7,  (1U << UART_PIN_TX) | (1U << UART_PIN_RX) },
    { "UT4_CM4",   UST_OWN_CM4,  (1U << UART_PIN_TX) | (1U << UART_PIN_RX) },
    { "UT5",       UST_OWN_CM7,  (1U << UART_PIN_TX) | (1U << UART_PIN_RX) },
    { "UT6",       UST_OWN_CM7,  (1U << UART_PIN_CTS) | (1U << UART_PIN_RTS) | (1U << UART_PIN_TX) | (1U << UART_PIN_RX) },
    { "UT7",       UST_OWN_CM7,  (1U << UART_PIN_TX) | (1U << UART_PIN_RX) },
    { "UT8",       UST_OWN_CM7,  (1U << UART_PIN_TX) | (1U << UART_PIN_RX) },
    { "UT9",       UST_OWN_CM7,  (1U << UART_CONSOLE_TX_PIN_IDX) | (1U << UART_CONSOLE_RX_PIN_IDX) },
    { "UT10",      UST_OWN_CM4,  (1U << UART_PIN_TX) | (1U << UART_PIN_RX) },
    { "DONE",      UST_OWN_NONE, 0U }
};

const uint32_t k_ust_step_count = (uint32_t)(sizeof(k_ust_steps) / sizeof(k_ust_steps[0]));

uint32_t ust_step_index(const char *name)
{
    if (name == NULL)
    {
        return UST_STEP_NONE;
    }
    for (uint32_t i = 0U; i < k_ust_step_count; i++)
    {
        if (strcmp(k_ust_steps[i].name, name) == 0)
        {
            return i;
        }
    }
    return UST_STEP_NONE;
}

bool ust_line_rule(uint32_t core, uint32_t step, const GPIO_TypeDef *port, uint32_t pin,
                   uint32_t mode, uint32_t af, uint32_t otype, uint32_t pull)
{
    (void)pull;
    (void)otype;

    if ((pin > 15U) || (core > 1U) || (port == NULL))
    {
        return false;
    }

    /* Input mode is always permitted on any pin */
    if (mode == GPIO_BM_MODE_INPUT)
    {
        return true;
    }

    /* Out of step boundaries, no outputs or alternate functions allowed */
    if (step >= k_ust_step_count)
    {
        return false;
    }

    const UstStep_t *s = &k_ust_steps[step];
    const uint32_t core_owner = (core == 0U) ? (uint32_t)UST_OWN_CM7 : (uint32_t)UST_OWN_CM4;

    /* Port E rules: Never allow PE0..PE4, PE7, or bus PE8..PE15 as output/AF */
    if (port == GPIOE)
    {
        /* S4: PE0-PE4, PE7 and bus PE8-PE15 strictly prohibited */
        return false;
    }

    /* Port A rules: PA8 MCO1 */
    if (port == GPIOA)
    {
        if (pin == UART_PIN_MCO)
        {
            return (mode == GPIO_BM_MODE_AF) && (af == 0U);
        }
        return false;
    }

    /* Port D rules: Bench pins PD3-PD6 and Console pins PD8-PD9 */
    if (port == GPIOD)
    {
        if ((pin == UART_CONSOLE_TX_PIN_IDX) || (pin == UART_CONSOLE_RX_PIN_IDX))
        {
            /* Console ST-LINK pins owned by CM7 */
            return (core == 0U) && (mode == GPIO_BM_MODE_AF) && (af == 7U);
        }

        if ((pin >= UART_PIN_CTS) && (pin <= UART_PIN_RX))
        {
            /* In UT0_CTRL, PD5 is driven as GPIO output low */
            if ((step == 1U) && (pin == UART_PIN_TX))
            {
                return (core == 0U) && (mode == GPIO_BM_MODE_OUTPUT);
            }

            /* Regular USART2 AF7 claim */
            if ((s->owner == core_owner) && (mode == GPIO_BM_MODE_AF) && (af == 7U))
            {
                return ((s->allow_pd & (1U << pin)) != 0U);
            }
        }
        return false;
    }

    return false;
}
