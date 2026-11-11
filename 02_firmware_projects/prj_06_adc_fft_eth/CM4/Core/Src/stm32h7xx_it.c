/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt handlers for CPU2 (Cortex-M4) with diagnostic HardFault
  *          reporting to D3 SRAM4 (`g_shared.dsp`).
  ******************************************************************************
  */

#include "main.h"
#include "stm32h7xx_it.h"
#include "shared_memory.h"
#include "adc_acquisition.h"

void NMI_Handler(void)
{
    for (;;)
    {
    }
}

/**
  * @brief  C helper invoked by the naked HardFault_Handler with the active
  *         exception stack frame pointer (`MSP` or `PSP`). Records SCB fault
  *         status registers and stacked PC/LR into `g_shared.dsp` so CM7 can
  *         log the exact fault telemetry over UART/UDP.
  */
void CM4_HardFault_Catcher(uint32_t *stack_frame)
{
    g_shared.dsp.fault_cfsr = SCB->CFSR;
    g_shared.dsp.fault_hfsr = SCB->HFSR;
    g_shared.dsp.fault_bfar = SCB->BFAR;

    if (stack_frame != NULL)
    {
        g_shared.dsp.fault_lr = stack_frame[5];
        g_shared.dsp.fault_pc = stack_frame[6];
    }

    g_shared.dsp.fault_flag = 1U;
    __DSB();

    for (;;)
    {
        __NOP();
    }
}

__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4                 \n"
        "ite eq                     \n"
        "mrseq r0, msp              \n"
        "mrsne r0, psp              \n"
        "b CM4_HardFault_Catcher    \n"
    );
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

/** Default 1 ms HAL timebase on Cortex-M4. */
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
  * @brief  This function handles ADC1/ADC2 global interrupt.
  */
void ADC_IRQHandler(void)
{
    ADC_Acquisition_ADC_IRQHandler();
}
