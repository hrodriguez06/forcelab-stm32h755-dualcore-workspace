/**
  ******************************************************************************
  * @file    hsem_selftest.h
  * @brief   HSEM driver self-test header for CM4 (prj_04).
  ******************************************************************************
  */

#ifndef HSEM_SELFTEST_CM4_H
#define HSEM_SELFTEST_CM4_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void HSEM_SelfTest_CM4_Run(void);
void HSEM_SelfTest_CM4_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* HSEM_SELFTEST_CM4_H */
