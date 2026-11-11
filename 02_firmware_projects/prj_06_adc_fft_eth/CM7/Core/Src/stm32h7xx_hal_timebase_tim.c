/**
  ******************************************************************************
  * @file    stm32h7xx_hal_timebase_tim.c
  * @brief   HAL time base on TIM6, replacing the default SysTick source.
  ******************************************************************************
  */

#include "stm32h7xx_hal.h"

TIM_HandleTypeDef htim6;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    RCC_ClkInitTypeDef clkconfig;
    uint32_t           pFLatency;
    uint32_t           pclk1_hz;
    uint32_t           prescaler;

    __HAL_RCC_TIM6_CLK_ENABLE();

    HAL_RCC_GetClockConfig(&clkconfig, &pFLatency);
    pclk1_hz = HAL_RCC_GetPCLK1Freq();

    if (clkconfig.APB1CLKDivider != RCC_HCLK_DIV1)
    {
        pclk1_hz *= 2U;
    }

    prescaler = (pclk1_hz / 1000000U) - 1U;

    htim6.Instance               = TIM6;
    htim6.Init.Prescaler         = prescaler;
    htim6.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim6.Init.Period            = 1000U - 1U;
    htim6.Init.ClockDivision     = 0;
    htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_Base_Init(&htim6) != HAL_OK)
    {
        return HAL_ERROR;
    }

    if (HAL_TIM_Base_Start_IT(&htim6) != HAL_OK)
    {
        return HAL_ERROR;
    }

    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TickPriority, 0U);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
    uwTickPrio = TickPriority;

    return HAL_OK;
}

void HAL_SuspendTick(void)
{
    __HAL_TIM_DISABLE_IT(&htim6, TIM_IT_UPDATE);
}

void HAL_ResumeTick(void)
{
    __HAL_TIM_ENABLE_IT(&htim6, TIM_IT_UPDATE);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6)
    {
        HAL_IncTick();
    }
}
