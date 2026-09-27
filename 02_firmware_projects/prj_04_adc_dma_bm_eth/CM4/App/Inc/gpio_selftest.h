/**
  ******************************************************************************
  * @file    gpio_selftest.h
  * @brief   GPIO driver self-test header for CM4 (prj_04).
  ******************************************************************************
  */

#ifndef GPIO_SELFTEST_CM4_H
#define GPIO_SELFTEST_CM4_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void GPIO_SelfTest_CM4_Run(void);
void GPIO_SelfTest_CM4_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_SELFTEST_CM4_H */
