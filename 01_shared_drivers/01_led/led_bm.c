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

#if __has_include("board_def.h")
#include "board_def.h"
#endif

/* Fallback default board pin definitions (Nucleo-H755ZI-Q) if board_def.h does not supply them */
#ifndef BOARD_LED_GREEN_PORT
#define BOARD_LED_GREEN_PORT    GPIOB
#endif
#ifndef BOARD_LED_GREEN_BIT
#define BOARD_LED_GREEN_BIT     0U
#endif

#ifndef BOARD_LED_YELLOW_PORT
#define BOARD_LED_YELLOW_PORT   GPIOE
#endif
#ifndef BOARD_LED_YELLOW_BIT
#define BOARD_LED_YELLOW_BIT    1U
#endif

#ifndef BOARD_LED_RED_PORT
#define BOARD_LED_RED_PORT      GPIOB
#endif
#ifndef BOARD_LED_RED_BIT
#define BOARD_LED_RED_BIT       14U
#endif

static led_driver_config_t g_led_config = {
    .leds = {
        [LED_GREEN]  = { .port = BOARD_LED_GREEN_PORT,  .pin_num = BOARD_LED_GREEN_BIT },
        [LED_YELLOW] = { .port = BOARD_LED_YELLOW_PORT, .pin_num = BOARD_LED_YELLOW_BIT },
        [LED_RED]    = { .port = BOARD_LED_RED_PORT,    .pin_num = BOARD_LED_RED_BIT }
    }
};

void LED_Init_Config(const led_driver_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    g_led_config = *config;

    for (uint32_t i = 0; i < (uint32_t)LED_COUNT; i++)
    {
        GPIO_TypeDef *port = g_led_config.leds[i].port;
        uint32_t bit = g_led_config.leds[i].pin_num;

        if (port != NULL)
        {
            (void)GPIO_BM_EnablePortClock(port);

            /* MODER: 0b01 = general purpose output. */
            MODIFY_REG(port->MODER, (3UL << (bit * 2U)), (1UL << (bit * 2U)));

            /* Turn off initially: active high, BSRR high half resets the pin */
            port->BSRR = (1UL << (bit + 16U));
        }
    }
}

void LED_Init(void)
{
    LED_Init_Config(&g_led_config);
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

    if (port->ODR & (1UL << bit))
    {
        port->BSRR = (1UL << (bit + 16U));
    }
    else
    {
        port->BSRR = (1UL << bit);
    }
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
        MODIFY_REG(red_port->MODER, (3UL << (red_bit * 2U)), (1UL << (red_bit * 2U)));
        red_port->BSRR = (1UL << red_bit);
    }
}
