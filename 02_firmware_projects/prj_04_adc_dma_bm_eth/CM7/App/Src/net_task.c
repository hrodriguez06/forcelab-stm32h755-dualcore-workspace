/**
  ******************************************************************************
  * @file    net_task.c
  * @brief   Networking task of CPU1 (Cortex-M7).
  *
  *          Responsibilities
  *          ----------------
  *            1. Start the lwIP TCP/IP thread.
  *            2. Attach the Ethernet interface (ethernetif.c) with either a
  *               static address or DHCP.
  *            3. Start the PHY link supervision thread.
  *            4. Register a UDP echo service, so the bring-up can be proven
  *               beyond ICMP.
  *            5. Report link, address and driver counters on the console.
  *
  *          With LWIP_TCPIP_CORE_LOCKING == 1 every lwIP call made from this
  *          task is bracketed by LOCK_TCPIP_CORE() / UNLOCK_TCPIP_CORE().
  ******************************************************************************
  */

#include "net_task.h"
#include "net_config.h"
#include "ethernetif.h"
#include "app_log.h"
#include "eth_hal.h"
#include "board_def.h"
#include "led_bm.h"
#include "adc_server.h"

#include "lwip/tcpip.h"
#include "lwip/sys.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/udp.h"
#include "lwip/stats.h"

#include "FreeRTOS.h"
#include "task.h"

/* How often the task prints a status line once everything is up. */
#define NET_STATUS_TICK_MS              100U   /* LED / supervision cadence */
#define NET_STATUS_PERIOD_MS           5000U   /* console status line        */

static struct netif    s_netif;
static struct udp_pcb *s_echo_pcb;

/* Latched once a driver counter goes non-zero; only a reset clears it. */
static uint8_t         s_error_latched;

/* Requests refused by the source filter. Written only from the tcpip thread,
   which is where both udp_recv callbacks run, so it needs no guard. */
static uint32_t        s_acl_drops;

#if LWIP_DHCP
/* Tick at which the current DHCP attempt gives up and the static address is
 * applied instead. 0 means no attempt is in flight. Written by the tcpip
 * thread (link callback), read and cleared by the net task. */
static volatile uint32_t s_dhcp_deadline;
#endif

/* ==========================================================================
 * lwIP callbacks. Both run in the tcpip thread.
 * ========================================================================== */

static void net_link_callback(struct netif *netif)
{
    if (netif_is_link_up(netif) != 0)
    {
        APP_LOG("[net] link up: %s", ethernetif_link_state_string());

#if LWIP_DHCP
        /* DHCP is driven from here rather than once at start-up, so that a
           cable plugged in minutes after boot still gets a lease. Without
           this the boot-time attempt times out, falls back to the static
           address, and nothing ever asks for a lease again short of a reset.
           Skipped while a lease is already held, so a brief link flap does
           not throw away a valid address. */
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
    /* ip4addr_ntoa() formats into one shared static buffer, so three calls in
       a single argument list all yield the same pointer and the line prints
       the last conversion three times. The _r form takes our own buffers. */
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

/* ==========================================================================
 * Source filtering. Shared by both UDP services; see NET_ACL_MODE.
 * ========================================================================== */

int Net_SourceIsAllowed(const ip_addr_t *addr)
{
#if (NET_ACL_MODE == NET_ACL_ANY)
    LWIP_UNUSED_ARG(addr);
    return 1;
#else
    const ip4_addr_t *mask = netif_ip4_netmask(&s_netif);
    int allowed;

    /* An interface with no address yet has a zero netmask, and comparing
       against that matches every source. Refuse instead: there is nothing this
       board can legitimately serve before it has an address of its own. */
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

        /* Once, on the first refusal. A board that has quietly stopped
           answering should say why without being asked, and a counter alone
           does not name the address. Further drops are in the status line. */
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

/* ==========================================================================
 * UDP echo service on NET_ECHO_PORT.
 * ========================================================================== */

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

/* Runs in the tcpip thread through tcpip_callback(). */
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

/* Runs in the tcpip thread through tcpip_callback(). */
static void net_adc_srv_start(void *arg)
{
    LWIP_UNUSED_ARG(arg);
    ADC_Server_Start();
}

/* ==========================================================================
 * Addressing.
 * ========================================================================== */

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

/**
  * @brief  Services an in-flight DHCP attempt: succeed, or fall back.
  * @note   Called from the net task loop. The attempt itself is started by
  *         net_link_callback() when the link comes up, so this covers every
  *         link-up, not just the first one after boot.
  */
static void net_service_dhcp(void)
{
    int supplied;

    if (s_dhcp_deadline == 0U)
    {
        return;   /* no attempt in flight */
    }

    LOCK_TCPIP_CORE();
    supplied = dhcp_supplied_address(&s_netif);
    UNLOCK_TCPIP_CORE();

    if (supplied != 0)
    {
        /* net_status_callback() already printed the address. */
        s_dhcp_deadline = 0U;
        return;
    }

    /* Signed comparison so the tick counter wrapping does not strand us. */
    if ((int32_t)(xTaskGetTickCount() - s_dhcp_deadline) < 0)
    {
        return;   /* still waiting */
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

/* ==========================================================================
 * Task body.
 * ========================================================================== */

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

    /* Creates the tcpip thread, its mailbox and the core lock. */
    tcpip_init(NULL, NULL);

#if LWIP_DHCP
    /* Start with an unspecified address: DHCP fills it in. */
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

    /* ethernetif_init() initialises the MAC and the PHY and starts the Rx
       thread. tcpip_input() hands received frames to the tcpip thread. */
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

    /* Administratively up. The interface only carries traffic once the link
       callback reports that the PHY has negotiated. */
    netif_set_up(&s_netif);

    UNLOCK_TCPIP_CORE();

    APP_LOG("[net] mac=%02X:%02X:%02X:%02X:%02X:%02X phy_addr=%u",
            s_netif.hwaddr[0], s_netif.hwaddr[1], s_netif.hwaddr[2],
            s_netif.hwaddr[3], s_netif.hwaddr[4], s_netif.hwaddr[5],
            (unsigned)BOARD_ETH_PHY_ADDRESS);

    /* Link supervision: polls the LAN8742A BSR and starts or stops the MAC. */
    if (sys_thread_new("eth_link", ethernetif_link_thread, &s_netif,
                       NET_LINK_THREAD_STACK_WORDS * sizeof(StackType_t),
                       NET_LINK_THREAD_PRIO) == NULL)
    {
        APP_LOG("[net] FATAL: could not start the link supervision thread");
    }

    (void)tcpip_callback(net_echo_start, NULL);
    (void)tcpip_callback(net_adc_srv_start, NULL);

    /* Hand the LEDs over from boot codes to runtime status. The last boot
       stage is 7 (0b111, all three lit), so the runtime state has to be
       established explicitly rather than latched on from boot.

       Runtime meaning (README section 8) - yellow and red together, so the
       four combinations are all distinguishable:

         green   heartbeat, 1 Hz, proves the scheduler still runs
         Y=1 R=0 link up, no faults: the healthy state
         Y=0 R=1 NO CONNECTION - cable unplugged, or nothing at the far end
         Y=1 R=1 link up, but a driver error has been latched
         Y=0 R=0 transient only, between the two polls of a link change     */
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

        /* Errors latch: a burst of starvation at 03:00 is still visible at
           09:00. The console counters say how many. Link state does not
           latch - it has to follow the cable. */
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
