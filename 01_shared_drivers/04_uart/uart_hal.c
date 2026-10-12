/**
  ******************************************************************************
  * @file    uart_hal.c
  * @brief   Hardware-agnostic UART driver, HAL tier.
  ******************************************************************************
  */

#include "uart_hal.h"
#include "gpio_hal.h"

#if defined(HAL_UART_MODULE_ENABLED)

#if __has_include("board_def.h")
#include "board_def.h"
#endif

#ifndef BOARD_CONSOLE_UART_INSTANCE
#error "BOARD_CONSOLE_UART_INSTANCE must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"
#endif

#ifndef BOARD_CONSOLE_TX_PORT
#error "BOARD_CONSOLE_TX_PORT must be defined in board_def.h"
#endif

#ifndef BOARD_CONSOLE_TX_PIN
#error "BOARD_CONSOLE_TX_PIN must be defined in board_def.h"
#endif

#ifndef BOARD_CONSOLE_TX_AF
#error "BOARD_CONSOLE_TX_AF must be defined in board_def.h"
#endif

#ifndef BOARD_CONSOLE_RX_PORT
#error "BOARD_CONSOLE_RX_PORT must be defined in board_def.h"
#endif

#ifndef BOARD_CONSOLE_RX_PIN
#error "BOARD_CONSOLE_RX_PIN must be defined in board_def.h"
#endif

#ifndef BOARD_CONSOLE_RX_AF
#error "BOARD_CONSOLE_RX_AF must be defined in board_def.h"
#endif

#define UART_CONSOLE_TX_TIMEOUT_MS      100U

static UART_HandleTypeDef g_console;

static void UART_HAL_EnableInstanceClock(USART_TypeDef *instance)
{
    if (instance == USART1) { __HAL_RCC_USART1_CLK_ENABLE(); }
    else if (instance == USART2) { __HAL_RCC_USART2_CLK_ENABLE(); }
    else if (instance == USART3) { __HAL_RCC_USART3_CLK_ENABLE(); }
    else if (instance == UART4)  { __HAL_RCC_UART4_CLK_ENABLE();  }
    else if (instance == UART5)  { __HAL_RCC_UART5_CLK_ENABLE();  }
    else if (instance == USART6) { __HAL_RCC_USART6_CLK_ENABLE(); }
    else if (instance == UART7)  { __HAL_RCC_UART7_CLK_ENABLE();  }
    else if (instance == UART8)  { __HAL_RCC_UART8_CLK_ENABLE();  }
#if defined(LPUART1)
    else if (instance == LPUART1) { __HAL_RCC_LPUART1_CLK_ENABLE(); }
#endif
}

static void UART_HAL_DisableInstanceClock(USART_TypeDef *instance)
{
    if (instance == USART1) { __HAL_RCC_USART1_CLK_DISABLE(); }
    else if (instance == USART2) { __HAL_RCC_USART2_CLK_DISABLE(); }
    else if (instance == USART3) { __HAL_RCC_USART3_CLK_DISABLE(); }
    else if (instance == UART4)  { __HAL_RCC_UART4_CLK_DISABLE();  }
    else if (instance == UART5)  { __HAL_RCC_UART5_CLK_DISABLE();  }
    else if (instance == USART6) { __HAL_RCC_USART6_CLK_DISABLE(); }
    else if (instance == UART7)  { __HAL_RCC_UART7_CLK_DISABLE();  }
    else if (instance == UART8)  { __HAL_RCC_UART8_CLK_DISABLE();  }
#if defined(LPUART1)
    else if (instance == LPUART1) { __HAL_RCC_LPUART1_CLK_DISABLE(); }
#endif
}

static HAL_StatusTypeDef s_msp_gpio_status = HAL_OK;

/**
  * @brief  Default MSP pinout and clock configuration for on-board ST-LINK VCP.
  *         Hardware instances and pins are read from board_def.h.
  * @note   Can be overridden by any application defining a strong UART_HAL_MspInit_User.
  */
__weak void UART_HAL_MspInit_User(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef gpio = {0};

    if (huart->Instance == BOARD_CONSOLE_UART_INSTANCE)
    {
        UART_HAL_EnableInstanceClock(BOARD_CONSOLE_UART_INSTANCE);

        gpio.Pin       = BOARD_CONSOLE_TX_PIN;
        gpio.Mode      = GPIO_MODE_AF_PP;
        gpio.Pull      = GPIO_PULLUP;
        gpio.Speed     = GPIO_SPEED_FREQ_LOW;
        gpio.Alternate = BOARD_CONSOLE_TX_AF;
        HAL_StatusTypeDef st_tx = GPIO_HAL_Init(BOARD_CONSOLE_TX_PORT, &gpio);
        if ((st_tx != HAL_OK) && (s_msp_gpio_status == HAL_OK))
        {
            s_msp_gpio_status = st_tx;
        }

        gpio.Pin       = BOARD_CONSOLE_RX_PIN;
        gpio.Alternate = BOARD_CONSOLE_RX_AF;
        HAL_StatusTypeDef st_rx = GPIO_HAL_Init(BOARD_CONSOLE_RX_PORT, &gpio);
        if ((st_rx != HAL_OK) && (s_msp_gpio_status == HAL_OK))
        {
            s_msp_gpio_status = st_rx;
        }
    }
}

__weak void UART_HAL_MspDeInit_User(UART_HandleTypeDef *huart)
{
    if (huart->Instance == BOARD_CONSOLE_UART_INSTANCE)
    {
        UART_HAL_DisableInstanceClock(BOARD_CONSOLE_UART_INSTANCE);
        (void)GPIO_HAL_DeinitPin(BOARD_CONSOLE_TX_PORT, GPIO_BM_PIN_INDEX(BOARD_CONSOLE_TX_PIN));
        (void)GPIO_HAL_DeinitPin(BOARD_CONSOLE_RX_PORT, GPIO_BM_PIN_INDEX(BOARD_CONSOLE_RX_PIN));
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

HAL_StatusTypeDef UART_HAL_InitEx(UART_HandleTypeDef *huart, const uart_hal_config_ex_t *config)
{
    if ((huart == NULL) || (config == NULL) || (config->instance == NULL))
    {
        return HAL_ERROR;
    }

    if (config->enable_clock)
    {
        UART_HAL_EnableInstanceClock(config->instance);
    }

    if (config->tx_pin != NULL)
    {
        if (!GPIO_BM_ConfigAlternate(config->tx_pin->port, config->tx_pin->pin_idx,
                                     config->tx_pin->af, GPIO_BM_PULL_UP,
                                     GPIO_BM_SPEED_VERY_HIGH))
        {
            return HAL_ERROR;
        }
    }

    if (config->rx_pin != NULL)
    {
        if (!GPIO_BM_ConfigAlternate(config->rx_pin->port, config->rx_pin->pin_idx,
                                     config->rx_pin->af, GPIO_BM_PULL_UP,
                                     GPIO_BM_SPEED_VERY_HIGH))
        {
            return HAL_ERROR;
        }
    }

    huart->Instance                    = config->instance;
    huart->Init.BaudRate               = config->baudrate;
    huart->Init.WordLength             = config->word_length;
    huart->Init.StopBits               = config->stop_bits;
    huart->Init.Parity                 = config->parity;
    huart->Init.Mode                   = UART_MODE_TX_RX;
    huart->Init.HwFlowCtl              = config->hw_flow_ctl;
    huart->Init.OverSampling           = config->oversampling;
    huart->Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart->Init.ClockPrescaler         = UART_PRESCALER_DIV1;
    huart->AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    s_msp_gpio_status = HAL_OK;

    HAL_StatusTypeDef status = HAL_UART_Init(huart);
    if (status != HAL_OK)
    {
        return status;
    }

    if (s_msp_gpio_status != HAL_OK)
    {
        return s_msp_gpio_status;
    }

    return HAL_OK;
}

void UART_HAL_Write(UART_HandleTypeDef *huart, const uint8_t *data, uint16_t length)
{
    if ((huart == NULL) || (data == NULL) || (length == 0U) || (huart->Instance == NULL))
    {
        return;
    }

    (void)HAL_UART_Transmit(huart, (uint8_t *)data, length, UART_CONSOLE_TX_TIMEOUT_MS);
}

HAL_StatusTypeDef UART_HAL_WriteEx(UART_HandleTypeDef *huart, const uint8_t *data, uint16_t length, uint32_t timeout_ms)
{
    if ((huart == NULL) || (data == NULL) || (length == 0U) || (huart->Instance == NULL))
    {
        return HAL_ERROR;
    }

    return HAL_UART_Transmit(huart, (uint8_t *)data, length, timeout_ms);
}

#ifndef IS_DMA_STREAM_ALL_INSTANCE
#if defined(IS_DMA_STREAM_INSTANCE)
#define IS_DMA_STREAM_ALL_INSTANCE(INSTANCE) IS_DMA_STREAM_INSTANCE(INSTANCE)
#endif
#endif

#ifndef IS_DMA_UART_USART_REQUEST
#define IS_DMA_UART_USART_REQUEST(REQ) \
    (((REQ) == DMA_REQUEST_USART1_RX) || ((REQ) == DMA_REQUEST_USART1_TX) || \
     ((REQ) == DMA_REQUEST_USART2_RX) || ((REQ) == DMA_REQUEST_USART2_TX) || \
     ((REQ) == DMA_REQUEST_USART3_RX) || ((REQ) == DMA_REQUEST_USART3_TX) || \
     ((REQ) == DMA_REQUEST_UART4_RX)  || ((REQ) == DMA_REQUEST_UART4_TX)  || \
     ((REQ) == DMA_REQUEST_UART5_RX)  || ((REQ) == DMA_REQUEST_UART5_TX)  || \
     ((REQ) == DMA_REQUEST_USART6_RX) || ((REQ) == DMA_REQUEST_USART6_TX) || \
     ((REQ) == DMA_REQUEST_UART7_RX)  || ((REQ) == DMA_REQUEST_UART7_TX)  || \
     ((REQ) == DMA_REQUEST_UART8_RX)  || ((REQ) == DMA_REQUEST_UART8_TX))
#endif

HAL_StatusTypeDef UART_HAL_CheckDmaErrata(DMA_HandleTypeDef *hdma)
{
    if ((hdma == NULL) || (hdma->Instance == NULL))
    {
        return HAL_ERROR;
    }

    /* 1. Verify instance is a DMA1/DMA2 stream (BDMA channels in D3 do not have DMA_SxCR bit 20) */
#if defined(IS_DMA_STREAM_ALL_INSTANCE)
    if (!IS_DMA_STREAM_ALL_INSTANCE(hdma->Instance))
    {
        return HAL_ERROR;
    }
#endif

    /* 2. Verify request is a USART/UART DMAMUX request (ES0445 restricts workaround to USART/UART) */
    if (!IS_DMA_UART_USART_REQUEST(hdma->Init.Request))
    {
        return HAL_ERROR;
    }

    /* 3. Stream must be disabled before modifying DMA_SxCR; modifying CR while EN=1 is prohibited */
    DMA_Stream_TypeDef *stream = (DMA_Stream_TypeDef *)hdma->Instance;
    if ((stream->CR & DMA_SxCR_EN) != 0U)
    {
        return HAL_BUSY;
    }

    /* 5. Set bit 20 (alternative peripheral DMA channel protocol) */
    stream->CR |= (1UL << 20);

    /* 6. Verify read-back of bit 20 */
    if ((stream->CR & (1UL << 20)) == 0U)
    {
        return HAL_ERROR;
    }

    return HAL_OK;
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
