/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt handlers for CPU1 (Cortex-M7).
  ******************************************************************************
  */

#include "main.h"
#include "stm32h7xx_it.h"
#include "led_bm.h"
#include "eth_hal.h"

extern TIM_HandleTypeDef htim6;

void NMI_Handler(void)
{
    LED_PanicRed();
    for (;;)
    {
    }
}

void HardFault_Handler(void)
{
    LED_PanicRed();
    for (;;)
    {
    }
}

void MemManage_Handler(void)
{
    LED_PanicRed();
    for (;;)
    {
    }
}

void BusFault_Handler(void)
{
    LED_PanicRed();
    for (;;)
    {
    }
}

void UsageFault_Handler(void)
{
    LED_PanicRed();
    for (;;)
    {
    }
}

void DebugMon_Handler(void)
{
}

/** HAL timebase. */
void TIM6_DAC_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim6);
}

/** Ethernet global interrupt. */
void ETH_IRQHandler(void)
{
    HAL_ETH_IRQHandler(ETH_Driver_GetHandle());
}
