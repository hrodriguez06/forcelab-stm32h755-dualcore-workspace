/**
  ******************************************************************************
  * @file    lwipopts.h
  * @brief   lwIP 2.1.x configuration for STM32H755ZI CPU1 (Cortex-M7).
  ******************************************************************************
  */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#include "net_config.h"

/* ---- Environment ------------------------------------------------------- */
#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TCPIP_CORE_LOCKING         1

/* ---- Heap and pools ---------------------------------------------------- */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                   (24 * 1024)

#define MEMP_NUM_PBUF                  24
#define MEMP_NUM_UDP_PCB                6
#define MEMP_NUM_TCP_PCB                8
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG               16
#define MEMP_NUM_SYS_TIMEOUT           10

#define PBUF_POOL_SIZE                 16
#define PBUF_POOL_BUFSIZE            1536

/* ---- Protocols --------------------------------------------------------- */
#define LWIP_ARP                        1
#define ARP_TABLE_SIZE                 10
#define LWIP_ETHERNET                   1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DHCP           (NET_USE_DHCP)
#define LWIP_DNS            (NET_USE_DHCP)
#define LWIP_NETIF_HOSTNAME             1

#define ETHARP_SUPPORT_STATIC_ENTRIES   0

#define TCP_MSS                      1460
#define TCP_SND_BUF               (4 * TCP_MSS)
#define TCP_WND                   (4 * TCP_MSS)

/* ---- API selection ----------------------------------------------------- */
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0

/* ---- Callbacks --------------------------------------------------------- */
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_EXT_STATUS_CALLBACK  0

/* ---- Checksums --------------------------------------------------------- */
#ifndef ETH_USE_HARDWARE_CHECKSUM
#define ETH_USE_HARDWARE_CHECKSUM       0
#endif

#if (ETH_USE_HARDWARE_CHECKSUM == 1)
#define CHECKSUM_BY_HARDWARE
#define CHECKSUM_GEN_IP                 0
#define CHECKSUM_GEN_UDP                0
#define CHECKSUM_GEN_TCP                0
#define CHECKSUM_GEN_ICMP               0
#define CHECKSUM_CHECK_IP               0
#define CHECKSUM_CHECK_UDP              0
#define CHECKSUM_CHECK_TCP              0
#define CHECKSUM_CHECK_ICMP             0
#else
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_ICMP             1
#endif

/* ---- Threads ----------------------------------------------------------- */
#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE       2048
#define TCPIP_THREAD_PRIO              16
#define TCPIP_MBOX_SIZE                16
#define DEFAULT_THREAD_STACKSIZE     1024
#define DEFAULT_THREAD_PRIO             8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_TCP_RECVMBOX_SIZE       8
#define DEFAULT_ACCEPTMBOX_SIZE         8

/* ---- Statistics and debug ---------------------------------------------- */
#define LWIP_STATS                      1
#define LINK_STATS                      1
#define ETHARP_STATS                    1
#define IP_STATS                        1
#define ICMP_STATS                      1
#define LWIP_STATS_DISPLAY              1

#define LWIP_DEBUG                      0

#endif /* LWIPOPTS_H */
