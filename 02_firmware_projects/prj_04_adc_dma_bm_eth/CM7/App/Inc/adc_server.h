/**
  ******************************************************************************
  * @file    adc_server.h
  * @brief   UDP telemetry query service for ADC1 readings on Cortex-M7.
  ******************************************************************************
  */

#ifndef ADC_SERVER_H
#define ADC_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Initializes and starts the UDP ADC telemetry listener on NET_ADC_PORT (5001).
  * @note   Must be called in the tcpip thread context (e.g. via tcpip_callback()).
  */
void ADC_Server_Start(void);

#ifdef __cplusplus
}
#endif

#endif /* ADC_SERVER_H */
