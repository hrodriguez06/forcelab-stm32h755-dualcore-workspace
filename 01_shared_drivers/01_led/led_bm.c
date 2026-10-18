/**
  ******************************************************************************
  * @file    led_bm.c
  * @brief   User LEDs driver, bare-metal tier.
  ******************************************************************************
  */

#include "led_bm.h"
#include "gpio_bm.h"
#include "stm32h7xx.h"
#include <stddef.h>

#include "board_def.h"

#ifndef BOARD_LED_GREEN_PORT
#error "BOARD_LED_GREEN_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"
#endif
#ifndef BOARD_LED_GREEN_BIT
#error "BOARD_LED_GREEN_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"
#endif
#if (BOARD_LED_GREEN_BIT) > 15U
#error "BOARD_LED_GREEN_BIT must be in range 0..15"
#endif

#ifndef BOARD_LED_YELLOW_PORT
#error "BOARD_LED_YELLOW_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"
#endif
#ifndef BOARD_LED_YELLOW_BIT
#error "BOARD_LED_YELLOW_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"
#endif
#if (BOARD_LED_YELLOW_BIT) > 15U
#error "BOARD_LED_YELLOW_BIT must be in range 0..15"
#endif

#ifndef BOARD_LED_RED_PORT
#error "BOARD_LED_RED_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"
#endif
#ifndef BOARD_LED_RED_BIT
#error "BOARD_LED_RED_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"
#endif
#if (BOARD_LED_RED_BIT) > 15U
#error "BOARD_LED_RED_BIT must be in range 0..15"
#endif

static led_driver_config_t g_led_config = {
    .leds = {
        [LED_GREEN]  = { .port = BOARD_LED_GREEN_PORT,  .pin_num = BOARD_LED_GREEN_BIT },
        [LED_YELLOW] = { .port = BOARD_LED_YELLOW_PORT, .pin_num = BOARD_LED_YELLOW_BIT },
        [LED_RED]    = { .port = BOARD_LED_RED_PORT,    .pin_num = BOARD_LED_RED_BIT }
    }
};

uint32_t LED_Init_ConfigEx(const led_driver_config_t *config)
{
    if (config == NULL)
    {
        return 0U;
    }

    g_led_config = *config;
    uint32_t mask = 0U;

    for (uint32_t i = 0; i < (uint32_t)LED_COUNT; i++)
    {
        GPIO_TypeDef *port = g_led_config.leds[i].port;
        uint32_t bit = g_led_config.leds[i].pin_num;

        /* Configure LED pin as output: push-pull, low speed, no pull, initially LOW (off).
         * GPIO_BM_ConfigOutput validates port and pin before touching registers, sets BSRR
         * before MODER within the critical section and cross-core lock (00_gpio/README.md §3.3),
         * and performs read-back verification. */
        if (GPIO_BM_ConfigOutput(port, bit, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW,
                                 GPIO_BM_PULL_NONE, 0U) == GPIO_BM_OK)
        {
            mask |= (1UL << i);
        }
        else
        {
            /* If configuration fails (invalid port/pin, SWD pin, LCKR frozen,
             * cross-core lock timeout, or read-back mismatch), disable the LED
             * so subsequent operations (Set, Toggle, BootStage, PanicRed) ignore it. */
            g_led_config.leds[i].port = NULL;
        }
    }

    return mask;
}

void LED_Init_Config(const led_driver_config_t *config)
{
    (void)LED_Init_ConfigEx(config);
}

void LED_Init(void)
{
    const led_driver_config_t default_cfg = {
        .leds = {
            [LED_GREEN]  = { .port = BOARD_LED_GREEN_PORT,  .pin_num = BOARD_LED_GREEN_BIT },
            [LED_YELLOW] = { .port = BOARD_LED_YELLOW_PORT, .pin_num = BOARD_LED_YELLOW_BIT },
            [LED_RED]    = { .port = BOARD_LED_RED_PORT,    .pin_num = BOARD_LED_RED_BIT }
        }
    };
    LED_Init_Config(&default_cfg);
}

void LED_Set(led_t led, uint32_t on)
{
    if ((uint32_t)led >= (uint32_t)LED_COUNT)
    {
        return;
    }

    GPIO_TypeDef *port = g_led_config.leds[led].port;
    uint32_t bit = g_led_config.leds[led].pin_num;

    if (port == NULL)
    {
        return;
    }

    if (on)
    {
        port->BSRR = (1UL << bit);
    }
    else
    {
        port->BSRR = (1UL << (bit + 16U));
    }
}

void LED_Toggle(led_t led)
{
    if ((uint32_t)led >= (uint32_t)LED_COUNT)
    {
        return;
    }

    GPIO_TypeDef *port = g_led_config.leds[led].port;
    uint32_t bit = g_led_config.leds[led].pin_num;

    if (port == NULL)
    {
        return;
    }

    /* Atomic on calling core: mask interrupts to prevent race condition with
     * ISRs or other threads on this core (01_led/IMPLEMENTATION_PLAN.md L4).
     * Cross-core toggle remains non-atomic (no HSEM). */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (port->ODR & (1UL << bit))
    {
        port->BSRR = (1UL << (bit + 16U));
    }
    else
    {
        port->BSRR = (1UL << bit);
    }

    __set_PRIMASK(primask);
}

void LED_BootStage(led_boot_stage_t stage)
{
    uint32_t s = (uint32_t)stage;

    LED_Set(LED_GREEN,  (s & 1U) ? 1U : 0U);
    LED_Set(LED_YELLOW, (s & 2U) ? 1U : 0U);
    LED_Set(LED_RED,    (s & 4U) ? 1U : 0U);
}

void LED_PanicRed(void)
{
    GPIO_TypeDef *red_port = g_led_config.leds[LED_RED].port;
    uint32_t red_bit = g_led_config.leds[LED_RED].pin_num;

    if (red_port != NULL)
    {
        (void)GPIO_BM_EnablePortClock(red_port);
        /* Exception G14: emergency panic path is terminal and must not wait
         * on locks or critical sections (00_gpio/README.md §3.3, G14). Direct RMW. */
        MODIFY_REG(red_port->MODER, (3UL << (red_bit * 2U)), (1UL << (red_bit * 2U)));
        red_port->BSRR = (1UL << red_bit);
    }
}
