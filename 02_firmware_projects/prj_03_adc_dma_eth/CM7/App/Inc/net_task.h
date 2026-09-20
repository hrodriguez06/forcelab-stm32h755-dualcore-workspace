/**
  ******************************************************************************
  * @file    net_task.h
  * @brief   Networking task of CPU1: lwIP bring-up and the bring-up services.
  ******************************************************************************
  */

#ifndef NET_TASK_H
#define NET_TASK_H

#include "lwip/netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Task body. Initialises the TCP/IP stack, attaches the Ethernet
  *         interface, starts link supervision and then reports status.
  * @note   Must run as a task: tcpip_init() creates threads and mailboxes, so
  *         it cannot be called before the scheduler is running.
  */
void Net_TaskEntry(void *argument);

/** @brief  The default network interface, for diagnostics. */
struct netif *Net_GetNetif(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_TASK_H */
