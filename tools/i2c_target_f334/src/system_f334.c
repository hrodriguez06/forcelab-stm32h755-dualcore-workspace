/**
  ******************************************************************************
  * @file    system_f334.c
  * @brief   SystemInit of the F334 bench target, called by the startup file
  *          (cmsis/Source/gcc/startup_stm32f334x8.s) before .data and .bss are
  *          set up. Runs on the HSI (8 MHz, the reset clock); main() sets the
  *          PLL. Replaces the template system_stm32f3xx.c, which is not copied.
  ******************************************************************************
  */

#include "stm32f3xx.h"

uint32_t SystemCoreClock = 8000000UL;     /* HSI after reset; main() updates it */

void SystemInit(void)
{
    SCB->VTOR = FLASH_BASE;               /* vector table at the start of flash */
    __DSB();
    __ISB();
}
