/**
  ******************************************************************************
  * @file    app_log.h
  * @brief   Line-oriented console log for the bring-up, over the ST-LINK VCP.
  ******************************************************************************
  */

#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Opens the console and creates the mutex that serialises writers.
  * @note   Call before the scheduler starts. Logging is a no-op until then.
  */
void App_Log_Init(void);

/** @brief  printf-style line logger. A newline is appended automatically. */
void App_Log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#define APP_LOG(...)   App_Log(__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* APP_LOG_H */
