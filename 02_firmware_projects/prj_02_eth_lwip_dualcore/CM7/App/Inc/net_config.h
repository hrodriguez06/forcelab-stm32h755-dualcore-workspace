/**
  ******************************************************************************
  * @file    net_config.h
  * @brief   Single place where the networking stack of CPU1 is tuned.
  ******************************************************************************
  */

#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Addressing -------------------------------------------------------- *
 * Set NET_USE_DHCP to 1 to obtain the address from a DHCP server. The static
 * values below are then only used as a fallback if the lease never arrives.
 */
#define NET_USE_DHCP                       0

#define NET_STATIC_IP_ADDR0              192
#define NET_STATIC_IP_ADDR1              168
#define NET_STATIC_IP_ADDR2                1
#define NET_STATIC_IP_ADDR3              150

#define NET_STATIC_NETMASK0              255
#define NET_STATIC_NETMASK1              255
#define NET_STATIC_NETMASK2              255
#define NET_STATIC_NETMASK3                0

#define NET_STATIC_GATEWAY0              192
#define NET_STATIC_GATEWAY1              168
#define NET_STATIC_GATEWAY2                1
#define NET_STATIC_GATEWAY3                1

/* Number of seconds to wait for a DHCP lease before falling back to the
 * static address above. Ignored when NET_USE_DHCP == 0. */
#define NET_DHCP_TIMEOUT_S                10U

/* ---- Checksum strategy ------------------------------------------------- *
 * ETH_USE_HARDWARE_CHECKSUM is NOT defined here. It has to be seen by both
 * lwipopts.h and Custom_Drivers/08_eth/eth_hal.c, so it is set once as a compile
 * definition in CM7/CMakeLists.txt:
 *
 *   0 : lwIP computes and verifies every checksum in software. Slower, but it
 *       removes a whole class of silent bring-up failures, and it is what the
 *       verification guide assumes. This is the default.
 *   1 : the MAC checksum offload engine inserts Tx checksums and verifies Rx
 *       checksums, and lwIP skips both.
 */

/* ---- Threading --------------------------------------------------------- *
 * The ETH interrupt only signals semaphores; all the work happens in these
 * threads. Keep the Rx thread above the tcpip thread so the DMA descriptor
 * ring is drained before the protocol work starts.
 *
 * Note on the numbers: threads created through lwIP's sys_thread_new() use
 * the CMSIS-RTOS v2 scale (osPriorityIdle = 1 .. osPriorityISR = 56), while
 * NET_APP_THREAD_PRIO goes straight to xTaskCreate() on the raw FreeRTOS
 * scale (0 .. configMAX_PRIORITIES-1). The two scales differ by one, which is
 * immaterial at this spacing: what matters is the ordering
 * eth_rx (24) > tcpip / eth_link (16) > net (8).
 */
#define NET_RX_THREAD_STACK_WORDS        512
#define NET_RX_THREAD_PRIO                24   /* > TCPIP_THREAD_PRIO       */
#define NET_LINK_THREAD_STACK_WORDS      256
#define NET_LINK_THREAD_PRIO              16
#define NET_APP_THREAD_STACK_WORDS       512
#define NET_APP_THREAD_PRIO                8

/* PHY link supervision period, in milliseconds. */
#define NET_LINK_POLL_PERIOD_MS          100U

/* UDP echo service used as the "the stack really works" test beyond ping. */
#define NET_ECHO_PORT                      7U

/* Maximum time low_level_output() waits for the Tx DMA to release a
 * descriptor before dropping the frame.
 *
 * This wait happens inside the tcpip thread while it holds the lwIP core
 * lock, so it stalls the whole stack, not just this frame. Keep it close to
 * the time the hardware actually needs: a 1522-byte frame takes ~122 us to
 * clock out at 100 Mbps and ~1.2 ms at 10 Mbps, so draining all
 * ETH_DRIVER_TX_BUFFER_COUNT (4) buffers is ~0.5 ms and ~4.9 ms
 * respectively. 5 ms covers the worst case at 10 Mbps; beyond that the frame
 * is better dropped so TCP's own flow control and retransmission can react. */
#define NET_TX_TIMEOUT_MS                  5U

#ifdef __cplusplus
}
#endif

#endif /* NET_CONFIG_H */
