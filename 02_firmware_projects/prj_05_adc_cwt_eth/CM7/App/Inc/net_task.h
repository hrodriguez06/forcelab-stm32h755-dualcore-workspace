/**
  ******************************************************************************
  * @file    net_task.h
  * @brief   Networking task of CPU1: lwIP bring-up and UDP services.
  ******************************************************************************
  */

#ifndef NET_TASK_H
#define NET_TASK_H

#include "lwip/netif.h"

#ifdef __cplusplus
extern "C" {
#endif

void Net_TaskEntry(void *argument);
struct netif *Net_GetNetif(void);
int Net_SourceIsAllowed(const ip_addr_t *addr);

#ifdef __cplusplus
}
#endif

#endif /* NET_TASK_H */
