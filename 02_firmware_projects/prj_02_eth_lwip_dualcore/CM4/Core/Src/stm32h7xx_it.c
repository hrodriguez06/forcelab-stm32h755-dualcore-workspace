/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt handlers for CPU2 (Cortex-M4).
  *
  *          Deliberately short. CPU2 keeps the default SysTick time base -
  *          unlike CPU1, which hands SysTick to FreeRTOS and moves the HAL
  *          tick onto TIM6 - so SysTick_Handler is all the HAL needs here.
  *
  *          The fault handlers park instead of resetting. On a dual-core part
  *          that is the useful behaviour: a core that resets itself erases the
  *          evidence, and the other core carries on as if nothing happened.
  *          Attach a debugger and read SCB->CFSR and SCB->HFSR - CPU2's, not
  *          CPU1's. See 00_docs_stm32h7/docs/00_core_and_system/cache_mpu_rules.md.
  ******************************************************************************
  */

#include "main.h"
#include "stm32h7xx_it.h"

void NMI_Handler(void)
{
    for (;;)
    {
    }
}

void HardFault_Handler(void)
{
    for (;;)
    {
    }
}

void MemManage_Handler(void)
{
    for (;;)
    {
    }
}

void BusFault_Handler(void)
{
    for (;;)
    {
    }
}

void UsageFault_Handler(void)
{
    for (;;)
    {
    }
}

void DebugMon_Handler(void)
{
}

void SVC_Handler(void)
{
}

void PendSV_Handler(void)
{
}

/** Default HAL time base on this core. */
void SysTick_Handler(void)
{
    HAL_IncTick();
}
