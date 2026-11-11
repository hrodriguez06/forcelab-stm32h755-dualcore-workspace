/**
  ******************************************************************************
  * @file    net_task.c
  * @brief   Networking task of CPU1 (Cortex-M7).
  ******************************************************************************
  */

#include "net_task.h"
#include "net_config.h"
#include "ethernetif.h"
#include "app_log.h"
#include "eth_hal.h"
#include "board_def.h"
#include "led_bm.h"
#include "cwt_server.h"

#include "lwip/tcpip.h"
#include "lwip/sys.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/udp.h"
#include "lwip/stats.h"

#include "FreeRTOS.h"
#include "task.h"

#define NET_STATUS_TICK_MS              100U
#define NET_STATUS_PERIOD_MS           5000U

static struct netif    s_netif;
static struct udp_pcb *s_echo_pcb;
static uint8_t         s_error_latched;
static uint32_t        s_acl_drops;

#if LWIP_DHCP
static volatile uint32_t s_dhcp_deadline;
#endif

static void net_link_callback(struct netif *netif)
{
    if (netif_is_link_up(netif) != 0)
    {
        APP_LOG("[net] link up: %s", ethernetif_link_state_string());

#if LWIP_DHCP
        if (dhcp_supplied_address(netif) == 0)
        {
            APP_LOG("[net] link is up, requesting a DHCP lease");
            (void)dhcp_start(netif);
            s_dhcp_deadline = xTaskGetTickCount() +
                              pdMS_TO_TICKS(NET_DHCP_TIMEOUT_S * 1000U);
        }
#endif
    }
    else
    {
        APP_LOG("[net] link down");
    }
}

static void net_status_callback(struct netif *netif)
{
    char ip[IP4ADDR_STRLEN_MAX];
    char mask[IP4ADDR_STRLEN_MAX];
    char gw[IP4ADDR_STRLEN_MAX];

    if (netif_is_up(netif) != 0)
    {
        APP_LOG("[net] interface up, ip=%s netmask=%s gw=%s",
                ip4addr_ntoa_r(netif_ip4_addr(netif),    ip,   sizeof(ip)),
                ip4addr_ntoa_r(netif_ip4_netmask(netif), mask, sizeof(mask)),
                ip4addr_ntoa_r(netif_ip4_gw(netif),      gw,   sizeof(gw)));
    }
    else
    {
        APP_LOG("[net] interface down");
    }
}

int Net_SourceIsAllowed(const ip_addr_t *addr)
{
#if (NET_ACL_MODE == NET_ACL_ANY)
    LWIP_UNUSED_ARG(addr);
    return 1;
#else
    const ip4_addr_t *mask = netif_ip4_netmask(&s_netif);
    int allowed;

    if ((addr == NULL) || !IP_IS_V4(addr) || ip4_addr_isany_val(*mask))
    {
        allowed = 0;
    }
    else
    {
        allowed = ip4_addr_netcmp(ip_2_ip4(addr), netif_ip4_addr(&s_netif), mask) ? 1 : 0;
    }

    if (allowed == 0)
    {
        s_acl_drops++;

        if (s_acl_drops == 1U)
        {
            char src[IP4ADDR_STRLEN_MAX];
            char own[IP4ADDR_STRLEN_MAX];

            APP_LOG("[net] source filter: dropped a UDP request from %s, which is "
                    "not on %s's subnet - see NET_ACL_MODE in net_config.h",
                    ((addr != NULL) && IP_IS_V4(addr))
                        ? ip4addr_ntoa_r(ip_2_ip4(addr), src, sizeof(src)) : "?",
                    ip4addr_ntoa_r(netif_ip4_addr(&s_netif), own, sizeof(own)));
        }
    }

    return allowed;
#endif
}

static void net_echo_received(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                              const ip_addr_t *addr, u16_t port)
{
    LWIP_UNUSED_ARG(arg);

    if (p == NULL)
    {
        return;
    }

    if (Net_SourceIsAllowed(addr) == 0)
    {
        pbuf_free(p);
        return;
    }

    (void)udp_sendto(pcb, p, addr, port);
    pbuf_free(p);
}

static void net_echo_start(void *arg)
{
    LWIP_UNUSED_ARG(arg);

    s_echo_pcb = udp_new();
    if (s_echo_pcb == NULL)
    {
        APP_LOG("[net] udp echo: out of pcbs");
        return;
    }

    if (udp_bind(s_echo_pcb, IP_ADDR_ANY, NET_ECHO_PORT) != ERR_OK)
    {
        udp_remove(s_echo_pcb);
        s_echo_pcb = NULL;
        APP_LOG("[net] udp echo: bind failed on port %u", (unsigned)NET_ECHO_PORT);
        return;
    }

    udp_recv(s_echo_pcb, net_echo_received, NULL);
    APP_LOG("[net] udp echo listening on port %u", (unsigned)NET_ECHO_PORT);
}

static void net_cwt_srv_start(void *arg)
{
    LWIP_UNUSED_ARG(arg);
    CWT_Server_Start();
}

#if LWIP_DHCP
static void net_apply_static_address(void)
{
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gateway;

    IP4_ADDR(&ipaddr,  NET_STATIC_IP_ADDR0,  NET_STATIC_IP_ADDR1,
                       NET_STATIC_IP_ADDR2,  NET_STATIC_IP_ADDR3);
    IP4_ADDR(&netmask, NET_STATIC_NETMASK0,  NET_STATIC_NETMASK1,
                       NET_STATIC_NETMASK2,  NET_STATIC_NETMASK3);
    IP4_ADDR(&gateway, NET_STATIC_GATEWAY0,  NET_STATIC_GATEWAY1,
                       NET_STATIC_GATEWAY2,  NET_STATIC_GATEWAY3);

    LOCK_TCPIP_CORE();
    netif_set_addr(&s_netif, &ipaddr, &netmask, &gateway);
    UNLOCK_TCPIP_CORE();
}

static void net_service_dhcp(void)
{
    int supplied;

    if (s_dhcp_deadline == 0U)
    {
        return;
    }

    LOCK_TCPIP_CORE();
    supplied = dhcp_supplied_address(&s_netif);
    UNLOCK_TCPIP_CORE();

    if (supplied != 0)
    {
        s_dhcp_deadline = 0U;
        return;
    }

    if ((int32_t)(xTaskGetTickCount() - s_dhcp_deadline) < 0)
    {
        return;
    }

    APP_LOG("[net] no DHCP lease after %us, falling back to the static address",
            (unsigned)NET_DHCP_TIMEOUT_S);

    LOCK_TCPIP_CORE();
    dhcp_stop(&s_netif);
    UNLOCK_TCPIP_CORE();

    net_apply_static_address();
    s_dhcp_deadline = 0U;
}
#endif /* LWIP_DHCP */

static void net_report_status(void)
{
    const eth_driver_stats_t *stats = ETH_Driver_GetStats();

    APP_LOG("[net] link=%s ip=%s rx=%lu tx=%lu rx_nobuf=%lu tx_nobuf=%lu dma_err=%lu acl_drop=%lu",
            ethernetif_link_state_string(),
            ip4addr_ntoa(netif_ip4_addr(&s_netif)),
            (unsigned long)stats->rx_frames,
            (unsigned long)stats->tx_frames,
            (unsigned long)stats->rx_alloc_failures,
            (unsigned long)stats->tx_alloc_failures,
            (unsigned long)stats->dma_errors,
            (unsigned long)s_acl_drops);
}

void Net_TaskEntry(void *argument)
{
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    uint32_t   ticks = 0U;

    (void)argument;

    APP_LOG("[net] starting TCP/IP stack");

    tcpip_init(NULL, NULL);

#if LWIP_DHCP
    ip4_addr_set_zero(&ipaddr);
    ip4_addr_set_zero(&netmask);
    ip4_addr_set_zero(&gateway);
#else
    IP4_ADDR(&ipaddr,  NET_STATIC_IP_ADDR0,  NET_STATIC_IP_ADDR1,
                       NET_STATIC_IP_ADDR2,  NET_STATIC_IP_ADDR3);
    IP4_ADDR(&netmask, NET_STATIC_NETMASK0,  NET_STATIC_NETMASK1,
                       NET_STATIC_NETMASK2,  NET_STATIC_NETMASK3);
    IP4_ADDR(&gateway, NET_STATIC_GATEWAY0,  NET_STATIC_GATEWAY1,
                       NET_STATIC_GATEWAY2,  NET_STATIC_GATEWAY3);
#endif

    LOCK_TCPIP_CORE();

    if (netif_add(&s_netif, &ipaddr, &netmask, &gateway, NULL,
                  &ethernetif_init, &tcpip_input) == NULL)
    {
        UNLOCK_TCPIP_CORE();
        APP_LOG("[net] FATAL: netif_add failed, check the MAC and the PHY");
        for (;;)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    netif_set_default(&s_netif);
    netif_set_status_callback(&s_netif, net_status_callback);
    netif_set_link_callback(&s_netif, net_link_callback);

    netif_set_up(&s_netif);

    UNLOCK_TCPIP_CORE();

    APP_LOG("[net] mac=%02X:%02X:%02X:%02X:%02X:%02X phy_addr=%u",
            s_netif.hwaddr[0], s_netif.hwaddr[1], s_netif.hwaddr[2],
            s_netif.hwaddr[3], s_netif.hwaddr[4], s_netif.hwaddr[5],
            (unsigned)BOARD_ETH_PHY_ADDRESS);

    if (sys_thread_new("eth_link", ethernetif_link_thread, &s_netif,
                       NET_LINK_THREAD_STACK_WORDS * sizeof(StackType_t),
                       NET_LINK_THREAD_PRIO) == NULL)
    {
        APP_LOG("[net] FATAL: could not start the link supervision thread");
    }

    (void)tcpip_callback(net_echo_start, NULL);
    (void)tcpip_callback(net_cwt_srv_start, NULL);

    LED_Set(LED_RED, 0U);

    for (;;)
    {
        const eth_driver_stats_t *stats;
        uint32_t                  link_up;

        vTaskDelay(pdMS_TO_TICKS(NET_STATUS_TICK_MS));
        ticks++;

        if ((ticks % (500U / NET_STATUS_TICK_MS)) == 0U)
        {
            LED_Toggle(LED_GREEN);
        }

        stats = ETH_Driver_GetStats();
        if ((stats->dma_errors | stats->rx_alloc_failures | stats->tx_alloc_failures) != 0U)
        {
            s_error_latched = 1U;
        }

#if LWIP_DHCP
        net_service_dhcp();
#endif

        link_up = (uint32_t)ethernetif_link_is_up();

        LED_Set(LED_YELLOW, link_up);
        LED_Set(LED_RED, ((link_up == 0U) || (s_error_latched != 0U)) ? 1U : 0U);

        if ((ticks % (NET_STATUS_PERIOD_MS / NET_STATUS_TICK_MS)) == 0U)
        {
            net_report_status();
        }
    }
}

struct netif *Net_GetNetif(void)
{
    return &s_netif;
}
