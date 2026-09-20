/**
  ******************************************************************************
  * @file    led_bm.h
  * @brief   NUCLEO-H755ZI-Q user LEDs, bare-metal tier.
  *
  *          Driver tier: *_bm (README.md section 10). Direct register access,
  *          no HAL, no clock-tree assumptions - deliberately, because the
  *          whole point of this driver is to say something useful at moments
  *          when the HAL is not up yet, or has stopped working:
  *
  *            - before MPU_Config() and the caches,
  *            - before HAL_Init() and the clock tree,
  *            - inside a fault handler.
  *
  *          Board wiring (UM2408):
  *            LD1 green  PB0
  *            LD2 yellow PE1
  *            LD3 red    PB14
  ******************************************************************************
  */

#ifndef LED_BM_H
#define LED_BM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    LED_GREEN = 0,
    LED_YELLOW,
    LED_RED
} led_t;

/**
  * @brief  Boot progress codes, shown as a 3-bit pattern while the firmware
  *         starts: green = bit 0, yellow = bit 1, red = bit 2.
  *
  *         If the board freezes during start-up the LEDs stop on the last
  *         stage that completed, which names the culprit without a debugger
  *         and without a working console. A board stuck on
  *         LED_BOOT_LEDS_ALIVE, for example, died inside MPU_Config() or on
  *         the first instruction fetch after HAL_MPU_Enable().
  */
typedef enum
{
    LED_BOOT_LEDS_ALIVE   = 1,  /*!< GPIO up, nothing else attempted yet    */
    LED_BOOT_MPU_DONE     = 2,  /*!< MPU_Config() returned                  */
    LED_BOOT_CACHES_DONE  = 3,  /*!< I-cache and D-cache enabled            */
    LED_BOOT_HAL_DONE     = 4,  /*!< HAL_Init() returned                    */
    LED_BOOT_CLOCKS_DONE  = 5,  /*!< SystemClock_Config() returned, 400 MHz */
    LED_BOOT_APP_ENTERED  = 6,  /*!< app_main() reached, console alive      */
    LED_BOOT_SCHEDULER    = 7   /*!< handing over to FreeRTOS               */
} led_boot_stage_t;

/**
  * @brief  Enables the GPIO clocks and drives the three LEDs as outputs.
  * @note   Uses RCC and GPIO registers directly, so it is safe to call as the
  *         very first statement of main(). Call it before MPU_Config().
  */
void LED_Init(void);

void LED_Set(led_t led, uint32_t on);
void LED_Toggle(led_t led);

/** @brief  Shows a boot stage as a 3-bit pattern across the three LEDs. */
void LED_BootStage(led_boot_stage_t stage);

/**
  * @brief  Lights the red LED and nothing else. Safe from a fault handler:
  *         no stack frame beyond the call, no HAL, no interrupts.
  */
void LED_PanicRed(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_BM_H */
