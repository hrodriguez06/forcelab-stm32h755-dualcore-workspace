/**
  ******************************************************************************
  * @file    vectors.c
  * @brief   Every exception and interrupt vector this firmware does not use,
  *          as a strong definition that releases the bus pins (S15, S21) and
  *          waits for the watchdog: bench_fault_release() in main.c.
  *
  *          The startup file (cmsis/Source/gcc/startup_stm32f334x8.s, kept as
  *          shipped) points every unused vector at a weak alias of its
  *          Default_Handler, an endless loop that leaves a hold in place until
  *          the IWDG resets the chip (audit F0, m2). These definitions take
  *          precedence over the aliases. The list is the startup file's own
  *          .thumb_set list, minus the handlers defined in main.c (the five
  *          faults), serial.c (USART2) and target.c (I2C1_EV, I2C1_ER, TIM2,
  *          EXTI9_5). A vector the table leaves at 0 (reserved) faults into
  *          HardFault_Handler, which does the same.
  ******************************************************************************
  */

#include "bench.h"

#define UNUSED_VECTOR(name)   void name(void); void name(void) { bench_fault_release(); }

UNUSED_VECTOR(SVC_Handler)
UNUSED_VECTOR(DebugMon_Handler)
UNUSED_VECTOR(PendSV_Handler)
UNUSED_VECTOR(SysTick_Handler)
UNUSED_VECTOR(WWDG_IRQHandler)
UNUSED_VECTOR(PVD_IRQHandler)
UNUSED_VECTOR(TAMP_STAMP_IRQHandler)
UNUSED_VECTOR(RTC_WKUP_IRQHandler)
UNUSED_VECTOR(FLASH_IRQHandler)
UNUSED_VECTOR(RCC_IRQHandler)
UNUSED_VECTOR(EXTI0_IRQHandler)
UNUSED_VECTOR(EXTI1_IRQHandler)
UNUSED_VECTOR(EXTI2_TSC_IRQHandler)
UNUSED_VECTOR(EXTI3_IRQHandler)
UNUSED_VECTOR(EXTI4_IRQHandler)
UNUSED_VECTOR(DMA1_Channel1_IRQHandler)
UNUSED_VECTOR(DMA1_Channel2_IRQHandler)
UNUSED_VECTOR(DMA1_Channel3_IRQHandler)
UNUSED_VECTOR(DMA1_Channel4_IRQHandler)
UNUSED_VECTOR(DMA1_Channel5_IRQHandler)
UNUSED_VECTOR(DMA1_Channel6_IRQHandler)
UNUSED_VECTOR(DMA1_Channel7_IRQHandler)
UNUSED_VECTOR(ADC1_2_IRQHandler)
UNUSED_VECTOR(CAN_TX_IRQHandler)
UNUSED_VECTOR(CAN_RX0_IRQHandler)
UNUSED_VECTOR(CAN_RX1_IRQHandler)
UNUSED_VECTOR(CAN_SCE_IRQHandler)
UNUSED_VECTOR(TIM1_BRK_TIM15_IRQHandler)
UNUSED_VECTOR(TIM1_UP_TIM16_IRQHandler)
UNUSED_VECTOR(TIM1_TRG_COM_TIM17_IRQHandler)
UNUSED_VECTOR(TIM1_CC_IRQHandler)
UNUSED_VECTOR(TIM3_IRQHandler)
UNUSED_VECTOR(SPI1_IRQHandler)
UNUSED_VECTOR(USART1_IRQHandler)
UNUSED_VECTOR(USART3_IRQHandler)
UNUSED_VECTOR(EXTI15_10_IRQHandler)
UNUSED_VECTOR(RTC_Alarm_IRQHandler)
UNUSED_VECTOR(TIM6_DAC1_IRQHandler)
UNUSED_VECTOR(TIM7_DAC2_IRQHandler)
UNUSED_VECTOR(COMP2_IRQHandler)
UNUSED_VECTOR(COMP4_6_IRQHandler)
UNUSED_VECTOR(HRTIM1_Master_IRQHandler)
UNUSED_VECTOR(HRTIM1_TIMA_IRQHandler)
UNUSED_VECTOR(HRTIM1_TIMB_IRQHandler)
UNUSED_VECTOR(HRTIM1_TIMC_IRQHandler)
UNUSED_VECTOR(HRTIM1_TIMD_IRQHandler)
UNUSED_VECTOR(HRTIM1_TIME_IRQHandler)
UNUSED_VECTOR(HRTIM1_FLT_IRQHandler)
UNUSED_VECTOR(FPU_IRQHandler)
