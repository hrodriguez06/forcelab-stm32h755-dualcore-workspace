/**
  ******************************************************************************
  * @file    led_selftest.h
  * @brief   01_led driver selftest header (preset selftest-led).
  ******************************************************************************
  */

#ifndef LED_SELFTEST_H
#define LED_SELFTEST_H

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Runs the 01_led selftest suite on CM7 (steps LT1 to LT5).
  */
void LED_Selftest_Run(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_SELFTEST_H */
