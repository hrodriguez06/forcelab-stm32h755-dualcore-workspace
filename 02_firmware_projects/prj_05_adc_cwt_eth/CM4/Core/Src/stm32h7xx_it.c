/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt handlers for CPU2 (Cortex-M4).
  ******************************************************************************
  */

#include "main.h"
#include "stm32h7xx_it.h"
#include "adc_acquisition.h"

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

/**
  * @brief  This function handles DMA1 Stream 0 interrupt.
  */
void DMA1_Stream0_IRQHandler(void)
{
    ADC_Acquisition_DMA_IRQHandler();
}

/**
  * @brief  This function handles ADC global interrupt.
  */
void ADC_IRQHandler(void)
{
    ADC_Acquisition_ADC_IRQHandler();
}
