/**
  ******************************************************************************
  * @file    led_bm.c
  * @brief   NUCLEO-H755ZI-Q user LEDs, bare-metal tier.
  ******************************************************************************
  */

#include "led_bm.h"
#include "stm32h7xx.h"

/* Board wiring (UM2408). Kept here rather than in board_def.h because that
 * header spells pins as HAL GPIO_PIN_x masks, and this driver predates the
 * HAL being initialised. */
#define LED_GREEN_PORT      GPIOB
#define LED_GREEN_BIT       0U
#define LED_YELLOW_PORT     GPIOE
#define LED_YELLOW_BIT      1U
#define LED_RED_PORT        GPIOB
#define LED_RED_BIT         14U

/* RCC_AHB4ENR: GPIOA = bit 0, GPIOB = bit 1, ... GPIOE = bit 4. */
#define RCC_GPIOB_EN        (1UL << 1)
#define RCC_GPIOE_EN        (1UL << 4)

static GPIO_TypeDef *led_port(led_t led)
{
    switch (led)
    {
        case LED_GREEN:  return LED_GREEN_PORT;
        case LED_YELLOW: return LED_YELLOW_PORT;
        case LED_RED:    return LED_RED_PORT;
        default:         return LED_GREEN_PORT;
    }
}

static uint32_t led_bit(led_t led)
{
    switch (led)
    {
        case LED_GREEN:  return LED_GREEN_BIT;
        case LED_YELLOW: return LED_YELLOW_BIT;
        case LED_RED:    return LED_RED_BIT;
        default:         return LED_GREEN_BIT;
    }
}

void LED_Init(void)
{
    volatile uint32_t dummy;

    RCC->AHB4ENR |= (RCC_GPIOB_EN | RCC_GPIOE_EN);
    /* Read back: the RCC enable takes a couple of cycles to land, and the
       first register write to a freshly clocked block can otherwise be lost. */
    dummy = RCC->AHB4ENR;
    (void)dummy;

    /* MODER: 0b01 = general purpose output. The reset state of OTYPER
       (push-pull), OSPEEDR (low) and PUPDR (no pull) is already what an LED
       wants, so only MODER is touched. */
    MODIFY_REG(LED_GREEN_PORT->MODER,
               (3UL << (LED_GREEN_BIT * 2U)), (1UL << (LED_GREEN_BIT * 2U)));
    MODIFY_REG(LED_RED_PORT->MODER,
               (3UL << (LED_RED_BIT * 2U)),   (1UL << (LED_RED_BIT * 2U)));
    MODIFY_REG(LED_YELLOW_PORT->MODER,
               (3UL << (LED_YELLOW_BIT * 2U)), (1UL << (LED_YELLOW_BIT * 2U)));

    /* All off. */
    LED_GREEN_PORT->BSRR  = (1UL << (LED_GREEN_BIT + 16U)) |
                            (1UL << (LED_RED_BIT + 16U));
    LED_YELLOW_PORT->BSRR = (1UL << (LED_YELLOW_BIT + 16U));
}

void LED_Set(led_t led, uint32_t on)
{
    GPIO_TypeDef *port = led_port(led);
    uint32_t      bit  = led_bit(led);

    /* BSRR is write-only and atomic: set in the low half, reset in the high
       half. No read-modify-write, so this is safe from any context. */
    port->BSRR = (on != 0U) ? (1UL << bit) : (1UL << (bit + 16U));
}

void LED_Toggle(led_t led)
{
    GPIO_TypeDef *port = led_port(led);
    uint32_t      bit  = led_bit(led);

    port->BSRR = ((port->ODR & (1UL << bit)) != 0U)
                 ? (1UL << (bit + 16U))
                 : (1UL << bit);
}

void LED_BootStage(led_boot_stage_t stage)
{
    uint32_t code = (uint32_t)stage;

    LED_Set(LED_GREEN,  (code >> 0) & 1U);
    LED_Set(LED_YELLOW, (code >> 1) & 1U);
    LED_Set(LED_RED,    (code >> 2) & 1U);
}

void LED_PanicRed(void)
{
    LED_GREEN_PORT->BSRR  = (1UL << (LED_GREEN_BIT + 16U));
    LED_YELLOW_PORT->BSRR = (1UL << (LED_YELLOW_BIT + 16U));
    LED_RED_PORT->BSRR    = (1UL << LED_RED_BIT);
}
