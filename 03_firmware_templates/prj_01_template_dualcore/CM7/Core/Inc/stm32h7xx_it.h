/**
  ******************************************************************************
  * @file    stm32h7xx_it.h
  * @brief   Interrupt handler prototypes for CPU1 (Cortex-M7).
  ******************************************************************************
  */

#ifndef STM32H7xx_IT_H
#define STM32H7xx_IT_H

#ifdef __cplusplus
extern "C" {
#endif

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void DebugMon_Handler(void);

void TIM6_DAC_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* STM32H7xx_IT_H */
