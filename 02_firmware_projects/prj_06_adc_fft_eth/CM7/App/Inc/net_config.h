/**
  ******************************************************************************
  * @file    net_config.h
  * @brief   Single place where the networking stack of CPU1 is tuned for
  *          `prj_06_adc_fft_eth`.
  ******************************************************************************
  */

#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Addressing -------------------------------------------------------- */
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

#define NET_DHCP_TIMEOUT_S                10U

/* ---- Threading --------------------------------------------------------- */
#define NET_RX_THREAD_STACK_WORDS        512
#define NET_RX_THREAD_PRIO                24   /* > TCPIP_THREAD_PRIO       */
#define NET_LINK_THREAD_STACK_WORDS      256
#define NET_LINK_THREAD_PRIO              16
#define NET_APP_THREAD_STACK_WORDS       512
#define NET_APP_THREAD_PRIO                8

#define NET_LINK_POLL_PERIOD_MS          100U

/* UDP echo service used by `tools/net_check.py` to verify Ethernet link/DMA */
#define NET_ECHO_PORT                      7U

/* UDP FFT + WPT DSP telemetry & spectrum streaming service on port 5002 */
#define NET_DSP_PORT                    5002U

/* ---- Source filtering --------------------------------------------------- */
#define NET_ACL_ANY                        0
#define NET_ACL_SAME_SUBNET                1

#define NET_ACL_MODE                       NET_ACL_SAME_SUBNET

#define NET_TX_TIMEOUT_MS                  5U

#ifdef __cplusplus
}
#endif

#endif /* NET_CONFIG_H */
