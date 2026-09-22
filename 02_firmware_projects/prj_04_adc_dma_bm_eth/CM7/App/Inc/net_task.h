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

/**
  * @brief  Whether a UDP service should answer this source address.
  * @note   Policy lives in NET_ACL_MODE (net_config.h). Call it before doing
  *         any work for the request, and free the pbuf on a refusal.
  * @note   Must be called from the tcpip thread: it keeps an unguarded counter.
  * @param  addr Source address as lwIP hands it to a udp_recv callback.
  * @return 1 when the request may be answered, 0 when it must be dropped.
  */
int Net_SourceIsAllowed(const ip_addr_t *addr);

#ifdef __cplusplus
}
#endif

#endif /* NET_TASK_H */
