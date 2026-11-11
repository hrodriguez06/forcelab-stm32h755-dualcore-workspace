/**
  ******************************************************************************
  * @file    ethernetif.h
  * @brief   lwIP network interface bound to 01_shared_drivers/08_eth.
  ******************************************************************************
  */

#ifndef ETHERNETIF_H
#define ETHERNETIF_H

#include "lwip/err.h"
#include "lwip/netif.h"

#ifdef __cplusplus
extern "C" {
#endif

err_t ethernetif_init(struct netif *netif);
void ethernetif_link_thread(void *argument);
const char *ethernetif_link_state_string(void);
int ethernetif_link_is_up(void);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNETIF_H */
