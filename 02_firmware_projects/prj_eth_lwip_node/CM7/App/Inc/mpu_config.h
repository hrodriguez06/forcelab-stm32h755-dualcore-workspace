/**
  ******************************************************************************
  * @file    mpu_config.h
  * @brief   MPU setup for STM32H755ZI CPU1 (Cortex-M7).
  ******************************************************************************
  */

#ifndef MPU_CONFIG_H
#define MPU_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Programs the MPU regions that mirror the memory partition declared
  *         in stm32h755zi_cm7.ld.
  * @note   MUST be called BEFORE SCB_EnableDCache(). Enabling the D-Cache with
  *         the default memory attributes would leave .eth_ram cacheable, and
  *         the Ethernet DMA would then read stale descriptors.
  */
void MPU_Config(void);

#ifdef __cplusplus
}
#endif

#endif /* MPU_CONFIG_H */
