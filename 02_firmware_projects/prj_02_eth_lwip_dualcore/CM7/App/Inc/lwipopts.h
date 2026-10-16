/**
  ******************************************************************************
  * @file    lwipopts.h
  * @brief   lwIP 2.1.x configuration for STM32H755ZI CPU1 (Cortex-M7).
  *
  *          Memory note: none of the pools declared here has to be reachable
  *          by the Ethernet DMA. The shared driver 08_eth/eth_hal.c owns
  *          dedicated Rx and Tx buffers in D2 SRAM3 (.eth_ram) and copies
  *          frames in and out of pbufs, so the lwIP heap and pbuf pool can
  *          live in ordinary .bss (DTCM), the fastest memory the M7 has.
  *
  *          That copy costs roughly 1.5 kB of memcpy per frame. It is the
  *          deliberate trade for a data path where no buffer is ever both
  *          cacheable and DMA-visible. See docs/ETHERNET_BRINGUP.md, section
  *          "Moving to a zero-copy data path".
  ******************************************************************************
  */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#include "net_config.h"

/* ---- Environment ------------------------------------------------------- */
#define NO_SYS                          0   /* FreeRTOS underneath          */
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TCPIP_CORE_LOCKING         1   /* required by net_task.c and by
                                               the link supervision thread,
                                               which touch netif state from
                                               outside the tcpip thread      */
/* LWIP_PROVIDE_ERRNO is not set here: ST's port already defines it in
 * LwIP/system/arch/cc.h, and defining it again only produces a redefinition
 * warning. */

/* ---- Heap and pools ---------------------------------------------------- */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                   (16 * 1024)

#define MEMP_NUM_PBUF                  16
#define MEMP_NUM_UDP_PCB                6
#define MEMP_NUM_TCP_PCB                8
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG               16
#define MEMP_NUM_SYS_TIMEOUT           10

#define PBUF_POOL_SIZE                 16
#define PBUF_POOL_BUFSIZE            1536   /* one whole Ethernet frame     */

/* ---- Zero-copy Rx (E6) ------------------------------------------------- */
#define LWIP_SUPPORT_CUSTOM_PBUF        1

/* ---- Protocols --------------------------------------------------------- */
#define LWIP_ARP                        1
#define ARP_TABLE_SIZE                 10
#define LWIP_ETHERNET                   1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ICMP                       1   /* the ping acceptance test     */
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DHCP           (NET_USE_DHCP)
#define LWIP_DNS            (NET_USE_DHCP)
#define LWIP_NETIF_HOSTNAME             1

/* ES0445 2.25.11: the MAC ARP offload responder is not effective and stays
 * disabled in eth_hal.c. ARP is handled here, in software, which is lwIP's
 * default behaviour anyway. */
#define ETHARP_SUPPORT_STATIC_ENTRIES   0

#define TCP_MSS                      1460
#define TCP_SND_BUF               (4 * TCP_MSS)
#define TCP_WND                   (4 * TCP_MSS)

/* ---- API selection ----------------------------------------------------- *
 * The bring-up application uses the raw/callback API from inside the tcpip
 * thread, so neither netconn nor the BSD socket layer is compiled in. Turn
 * LWIP_NETCONN and LWIP_SOCKET on when the product needs them.
 */
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0

/* ---- Callbacks --------------------------------------------------------- */
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_EXT_STATUS_CALLBACK  0

/* ---- Checksums --------------------------------------------------------- *
 * ETH_USE_HARDWARE_CHECKSUM comes from CM7/CMakeLists.txt and must match the
 * MAC ChecksumOffload setting applied in eth_hal.c.
 */
#ifndef ETH_USE_HARDWARE_CHECKSUM
#define ETH_USE_HARDWARE_CHECKSUM       0
#endif

#if (ETH_USE_HARDWARE_CHECKSUM == 1)
/* CHECKSUM_GEN_ICMP MUST STAY 0 HERE. It looks like an oversight and it is
 * not - setting it to 1 is what breaks ping, not what fixes it.
 *
 * RM0399, "TCP/UDP/ICMP checksum engine": the STM32H7 Tx offload engine does
 * compute ICMP payload checksums, but "for ICMP-over-IPv4 packets, the
 * Checksum field in the ICMP packet must always be 0x0000 in both modes,
 * because pseudo-headers are not defined for such packets. If it does not
 * equal 0x0000, an incorrect checksum may be inserted into the packet."
 *
 * lwIP satisfies that precondition only while CHECKSUM_GEN_ICMP is 0. In
 * src/core/ipv4/icmp.c the echo-reply path writes `iecho->chksum = 0` in the
 * #else branch; with CHECKSUM_GEN_ICMP == 1 it instead writes a non-zero
 * incrementally-adjusted checksum, the hardware then overwrites it wrongly,
 * and every echo reply is discarded by the host.
 *
 * So: hardware offload on -> software ICMP generation off. The two are a
 * matched pair. Verify with a ping after ever changing this block.
 */
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

/* ---- Threads ----------------------------------------------------------- *
 * Stack sizes are in bytes here because ST's sys_arch passes them straight to
 * osThreadNew(). The Rx thread created by ethernetif.c sits above the tcpip
 * thread so the descriptor ring is drained before protocol work begins.
 */
#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE       2048
#define TCPIP_THREAD_PRIO              16
#define TCPIP_MBOX_SIZE                16
#define DEFAULT_THREAD_STACKSIZE     1024
#define DEFAULT_THREAD_PRIO             8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_TCP_RECVMBOX_SIZE       8
#define DEFAULT_ACCEPTMBOX_SIZE         8

/* ---- Statistics and debug ---------------------------------------------- *
 * Statistics stay on: link.drop, link.memerr and ip.drop are the first things
 * the bring-up guide asks you to look at when ping does not answer.
 */
#define LWIP_STATS                      1
#define LINK_STATS                      1
#define ETHARP_STATS                    1
#define IP_STATS                        1
#define ICMP_STATS                      1
#define LWIP_STATS_DISPLAY              1

#define LWIP_DEBUG                      0

#endif /* LWIPOPTS_H */
