/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt handlers for CPU1 (Cortex-M7).
  *
  *          SVC_Handler, PendSV_Handler and SysTick_Handler are deliberately
  *          absent: FreeRTOSConfig.h maps them onto the FreeRTOS port
  *          handlers, and SysTick belongs to the scheduler. HAL_GetTick() runs
  *          on TIM6 instead (stm32h7xx_hal_timebase_tim.c).
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
    /* Most often an MPU permission fault. Region 0 of mpu_config.c denies all
       access to unmapped memory on purpose, so a stray pointer lands here. */
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

#if defined(HSEM_DRIVER_SELFTEST) && HSEM_DRIVER_SELFTEST
extern void HSEM_SelfTest_CM7_IRQHandler(void);
void HSEM1_IRQHandler(void)
{
    HSEM_SelfTest_CM7_IRQHandler();
}
#endif


