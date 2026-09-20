/**
  ******************************************************************************
  * @file    uart_hal.c
  * @brief   Hardware-agnostic UART driver, HAL tier.
  ******************************************************************************
  */

#include "uart_hal.h"

#if defined(HAL_UART_MODULE_ENABLED)

#if __has_include("board_def.h")
#include "board_def.h"
#endif

#ifndef BOARD_CONSOLE_UART_INSTANCE
#define BOARD_CONSOLE_UART_INSTANCE     USART3
#endif

#define UART_CONSOLE_TX_TIMEOUT_MS      100U

static UART_HandleTypeDef g_console;

/**
  * @brief  Default MSP pinout and clock configuration for Nucleo-144 on-board
  *         ST-LINK VCP (USART3 on PD8/PD9).
  * @note   Can be overridden by any application defining a strong UART_HAL_MspInit_User.
  */
__weak void UART_HAL_MspInit_User(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef gpio = {0};

    if (huart->Instance == USART3)
    {
        __HAL_RCC_GPIOD_CLK_ENABLE();
        __HAL_RCC_USART3_CLK_ENABLE();

        gpio.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
        gpio.Mode      = GPIO_MODE_AF_PP;
        gpio.Pull      = GPIO_PULLUP;
        gpio.Speed     = GPIO_SPEED_FREQ_LOW;
        gpio.Alternate = GPIO_AF7_USART3;
        HAL_GPIO_Init(GPIOD, &gpio);
    }
}

__weak void UART_HAL_MspDeInit_User(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        __HAL_RCC_USART3_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOD, GPIO_PIN_8 | GPIO_PIN_9);
    }
}

#ifndef UART_HAL_CUSTOM_MSP
/**
  * @brief  Standard HAL callback dispatching to weak UART_HAL_MspInit_User().
  * @note   To supply a full custom HAL_UART_MspInit() across multiple UARTs,
  *         define UART_HAL_CUSTOM_MSP to suppress this function.
  */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    UART_HAL_MspInit_User(huart);
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
    UART_HAL_MspDeInit_User(huart);
}
#endif /* UART_HAL_CUSTOM_MSP */

HAL_StatusTypeDef UART_HAL_Init(UART_HandleTypeDef *huart, const uart_hal_config_t *config)
{
    if ((huart == NULL) || (config == NULL) || (config->instance == NULL))
    {
        return HAL_ERROR;
    }

    huart->Instance                    = config->instance;
    huart->Init.BaudRate               = config->baudrate;
    huart->Init.WordLength             = UART_WORDLENGTH_8B;
    huart->Init.StopBits               = UART_STOPBITS_1;
    huart->Init.Parity                 = UART_PARITY_NONE;
    huart->Init.Mode                   = UART_MODE_TX_RX;
    huart->Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart->Init.OverSampling           = UART_OVERSAMPLING_16;
    huart->Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart->Init.ClockPrescaler         = UART_PRESCALER_DIV1;
    huart->AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    return HAL_UART_Init(huart);
}

void UART_HAL_Write(UART_HandleTypeDef *huart, const uint8_t *data, uint16_t length)
{
    if ((huart == NULL) || (data == NULL) || (length == 0U) || (huart->Instance == NULL))
    {
        return;
    }

    (void)HAL_UART_Transmit(huart, (uint8_t *)data, length, UART_CONSOLE_TX_TIMEOUT_MS);
}

HAL_StatusTypeDef UART_Console_Init(uint32_t baudrate)
{
    uart_hal_config_t config = {
        .instance = BOARD_CONSOLE_UART_INSTANCE,
        .baudrate = baudrate
    };

    return UART_HAL_Init(&g_console, &config);
}

void UART_Console_Write(const uint8_t *data, uint16_t length)
{
    UART_HAL_Write(&g_console, data, length);
}

UART_HandleTypeDef *UART_Console_GetHandle(void)
{
    return &g_console;
}

#endif /* HAL_UART_MODULE_ENABLED */
