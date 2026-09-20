/**
  ******************************************************************************
  * @file    uart_hal.c
  * @brief   Bring-up console on the ST-LINK virtual COM port, HAL tier.
  ******************************************************************************
  */

#include "uart_hal.h"

/* 100 ms is far longer than the ~1 ms a full log line takes at 115200 baud;
 * it only matters if the peripheral is wedged, in which case dropping the
 * line is better than hanging the caller. */
#define UART_CONSOLE_TX_TIMEOUT_MS       100U

static UART_HandleTypeDef g_console;

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef gpio = {0};

    if (huart->Instance != USART3)
    {
        return;
    }

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_USART3_CLK_ENABLE();

    gpio.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOD, &gpio);
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART3)
    {
        return;
    }

    __HAL_RCC_USART3_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_8 | GPIO_PIN_9);
}

HAL_StatusTypeDef UART_Console_Init(uint32_t baudrate)
{
    g_console.Instance                    = USART3;
    g_console.Init.BaudRate               = baudrate;
    g_console.Init.WordLength             = UART_WORDLENGTH_8B;
    g_console.Init.StopBits               = UART_STOPBITS_1;
    g_console.Init.Parity                 = UART_PARITY_NONE;
    g_console.Init.Mode                   = UART_MODE_TX_RX;
    g_console.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    g_console.Init.OverSampling           = UART_OVERSAMPLING_16;
    g_console.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    g_console.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
    g_console.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    return HAL_UART_Init(&g_console);
}

void UART_Console_Write(const uint8_t *data, uint16_t length)
{
    if ((data == NULL) || (length == 0U) || (g_console.Instance == NULL))
    {
        return;
    }

    (void)HAL_UART_Transmit(&g_console, (uint8_t *)data, length,
                            UART_CONSOLE_TX_TIMEOUT_MS);
}

UART_HandleTypeDef *UART_Console_GetHandle(void)
{
    return &g_console;
}
