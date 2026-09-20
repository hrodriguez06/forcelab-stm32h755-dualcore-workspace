/**
  ******************************************************************************
  * @file    ethernetif.h
  * @brief   lwIP network interface bound to Custom_Drivers/eth.
  ******************************************************************************
  */

#ifndef ETHERNETIF_H
#define ETHERNETIF_H

#include "lwip/err.h"
#include "lwip/netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  netif init callback, passed to netif_add().
  *
  *         Brings up the MAC and the LAN8742A, allocates the notification
  *         semaphores and starts the receive thread. The MAC itself is left
  *         stopped: ethernetif_link_thread() starts it once the PHY reports a
  *         usable link, because the negotiated speed and duplex have to be
  *         programmed into the MAC before reception begins.
  */
err_t ethernetif_init(struct netif *netif);

/**
  * @brief  Link supervision thread body. Polls the LAN8742A Basic Status
  *         Register over MDIO, reconfigures the MAC on every change and
  *         raises or drops the lwIP link flag accordingly.
  * @param  argument  Pointer to the netif to supervise.
  */
void ethernetif_link_thread(void *argument);

/**
  * @brief  Last link state observed by the supervision thread, for logging
  *         and for the verification guide.
  */
const char *ethernetif_link_state_string(void);

/** @brief  Non-zero when the PHY reports a usable link and the MAC is running. */
int ethernetif_link_is_up(void);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNETIF_H */
