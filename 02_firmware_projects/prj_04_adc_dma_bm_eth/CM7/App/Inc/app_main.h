/**
  ******************************************************************************
  * @file    app_main.h
  * @brief   Application entry point of CPU1, called from Core/Src/main.c.
  ******************************************************************************
  */

#ifndef APP_MAIN_H
#define APP_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Creates the application tasks and starts the scheduler.
  * @note   Never returns. The clock tree, the MPU, the caches and the HAL are
  *         already initialised when this is called.
  */
void app_main(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MAIN_H */
