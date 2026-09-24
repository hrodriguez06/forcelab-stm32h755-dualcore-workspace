/**
  ******************************************************************************
  * @file    stm32h7xx_hal_conf.h
  * @brief   HAL configuration for STM32H755ZI CPU1 (Cortex-M7).
  *
  *          Only the modules this firmware actually uses are enabled. Adding a
  *          peripheral means enabling its module here as well as writing the
  *          driver under 01_shared_drivers/.
  ******************************************************************************
  */

#ifndef STM32H7xx_HAL_CONF_H
#define STM32H7xx_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Enabled modules --------------------------------------------------- */
#define HAL_MODULE_ENABLED
#define HAL_ADC_MODULE_ENABLED
#define USE_HAL_ADC_REGISTER_CALLBACKS     0U
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_ETH_MODULE_ENABLED
#define HAL_EXTI_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_HSEM_MODULE_ENABLED
#define HAL_MDMA_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

/* ---- Oscillators ------------------------------------------------------- *
 * The NUCLEO-H755ZI-Q has no MCU crystal: HSE is the 8 MHz ST-LINK MCO fed in
 * bypass mode. See Common/Inc/board_def.h.
 */
#if !defined(HSE_VALUE)
#define HSE_VALUE               8000000UL
#endif
#if !defined(HSE_STARTUP_TIMEOUT)
#define HSE_STARTUP_TIMEOUT     100UL
#endif
#if !defined(CSI_VALUE)
#define CSI_VALUE               4000000UL
#endif
#if !defined(HSI_VALUE)
#define HSI_VALUE              64000000UL
#endif
#if !defined(LSI_VALUE)
#define LSI_VALUE                 32000UL
#endif
#if !defined(LSE_VALUE)
#define LSE_VALUE                 32768UL
#endif
#if !defined(LSE_STARTUP_TIMEOUT)
#define LSE_STARTUP_TIMEOUT      5000UL
#endif
#if !defined(EXTERNAL_CLOCK_VALUE)
#define EXTERNAL_CLOCK_VALUE  12288000UL
#endif

/* ---- System configuration ---------------------------------------------- */
#define VDD_VALUE                  3300UL
#define TICK_INT_PRIORITY             0UL   /* TIM6 tick, see the note below */
#define USE_RTOS                        0
#define USE_SD_TRANSCEIVER              0
#define USE_SPI_CRC                     0
#define USE_FLASH_ECC                   0U  /* flash ECC error management off */
#define USE_HAL_ETH_REGISTER_CALLBACKS  0U
#define USE_HAL_UART_REGISTER_CALLBACKS 0U
#define USE_HAL_TIM_REGISTER_CALLBACKS  0U

/* HAL_GetTick() runs on TIM6, not SysTick: FreeRTOS owns SysTick. The HAL
 * timebase interrupt has to stay above the FreeRTOS syscall ceiling so that
 * HAL_Delay() keeps working inside critical sections, hence priority 0 above
 * combined with an ISR that touches no RTOS object.
 * See CM7/Core/Src/stm32h7xx_hal_timebase_tim.c. */

/* ---- Ethernet ----------------------------------------------------------- *
 * Left in place, commented, because the linker script already reserves SRAM3
 * for the descriptor rings (.eth_ram) and the MPU marks that region
 * non-cacheable. Uncomment these together with HAL_ETH_MODULE_ENABLED above
 * when the product uses Ethernet; 02_firmware_projects/prj_eth_lwip_node is a
 * working example of what goes with them.
 */
/* #define ETH_TX_DESC_CNT              4U */
/* #define ETH_RX_DESC_CNT              4U */

#define PREFETCH_ENABLE                 0U
#define INSTRUCTION_CACHE_ENABLE        1U
#define DATA_CACHE_ENABLE               1U

/* ---- assert_param ------------------------------------------------------- */
/* #define USE_FULL_ASSERT    1U */

#ifdef USE_FULL_ASSERT
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
void assert_failed(uint8_t *file, uint32_t line);
#else
#define assert_param(expr) ((void)0U)
#endif

/* ---- Module headers ----------------------------------------------------- */
#ifdef HAL_RCC_MODULE_ENABLED
#include "stm32h7xx_hal_rcc.h"
#endif
#ifdef HAL_GPIO_MODULE_ENABLED
#include "stm32h7xx_hal_gpio.h"
#endif
#ifdef HAL_DMA_MODULE_ENABLED
#include "stm32h7xx_hal_dma.h"
#endif
#ifdef HAL_ADC_MODULE_ENABLED
#include "stm32h7xx_hal_adc.h"
#endif
#ifdef HAL_MDMA_MODULE_ENABLED
#include "stm32h7xx_hal_mdma.h"
#endif
#ifdef HAL_CORTEX_MODULE_ENABLED
#include "stm32h7xx_hal_cortex.h"
#endif
#ifdef HAL_ETH_MODULE_ENABLED
#include "stm32h7xx_hal_eth.h"
#endif
#ifdef HAL_EXTI_MODULE_ENABLED
#include "stm32h7xx_hal_exti.h"
#endif
#ifdef HAL_FLASH_MODULE_ENABLED
#include "stm32h7xx_hal_flash.h"
#endif
#ifdef HAL_HSEM_MODULE_ENABLED
#include "stm32h7xx_hal_hsem.h"
#endif
#ifdef HAL_PWR_MODULE_ENABLED
#include "stm32h7xx_hal_pwr.h"
#endif
#ifdef HAL_TIM_MODULE_ENABLED
#include "stm32h7xx_hal_tim.h"
#endif
#ifdef HAL_UART_MODULE_ENABLED
#include "stm32h7xx_hal_uart.h"
#endif

#ifdef __cplusplus
}
#endif

#endif /* STM32H7xx_HAL_CONF_H */
