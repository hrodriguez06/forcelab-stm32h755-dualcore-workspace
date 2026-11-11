/**
  ******************************************************************************
  * @file    ethernetif.c
  * @brief   lwIP network interface for the STM32H755ZI Ethernet MAC.
  ******************************************************************************
  */

#include "ethernetif.h"
#include "eth_hal.h"
#include "board_def.h"
#include "net_config.h"
#include "app_log.h"

#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/tcpip.h"
#include "lwip/sys.h"
#include "netif/etharp.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <string.h>

#define IFNAME0                         's'
#define IFNAME1                         't'

_Static_assert(configTICK_RATE_HZ == 1000,
               "sys_now() returns milliseconds and assumes a 1 kHz FreeRTOS tick");

u32_t sys_now(void)
{
    return (u32_t)xTaskGetTickCount();
}

static SemaphoreHandle_t              s_rx_semaphore;
static SemaphoreHandle_t              s_tx_semaphore;
static volatile uint8_t               s_mac_started;
static volatile lan8742_link_state_t  s_link_state = LAN8742_LINK_DOWN;

static void ethernetif_on_rx(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    xSemaphoreGiveFromISR(s_rx_semaphore, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static void ethernetif_on_tx(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    xSemaphoreGiveFromISR(s_tx_semaphore, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static void ethernetif_on_error(uint32_t error_code)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    (void)error_code;
    xSemaphoreGiveFromISR(s_tx_semaphore, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static const eth_driver_callbacks_t s_driver_callbacks =
{
    .on_rx_complete = ethernetif_on_rx,
    .on_tx_complete = ethernetif_on_tx,
    .on_error       = ethernetif_on_error
};

static int ethernetif_receive_one(struct netif *netif)
{
    eth_rx_segment_t *packet  = NULL;
    eth_rx_segment_t *segment;
    struct pbuf      *p;
    uint16_t          total   = 0U;
    uint16_t          offset  = 0U;

    if (ETH_Driver_ReadPacket(&packet) != ETH_DRIVER_OK)
    {
        return 0;
    }

    for (segment = packet; segment != NULL; segment = segment->next)
    {
        total = (uint16_t)(total + segment->length);
    }

    p = pbuf_alloc(PBUF_RAW, total, PBUF_POOL);
    if (p != NULL)
    {
        for (segment = packet; segment != NULL; segment = segment->next)
        {
            if (pbuf_take_at(p, segment->payload, segment->length, offset) != ERR_OK)
            {
                pbuf_free(p);
                p = NULL;
                break;
            }
            offset = (uint16_t)(offset + segment->length);
        }
    }

    ETH_Driver_ReleasePacket(packet);

    if (p == NULL)
    {
        LINK_STATS_INC(link.memerr);
        LINK_STATS_INC(link.drop);
        return 1;
    }

    LINK_STATS_INC(link.recv);

    if (netif->input(p, netif) != ERR_OK)
    {
        pbuf_free(p);
    }

    return 1;
}

static void ethernetif_input_thread(void *argument)
{
    struct netif *netif = (struct netif *)argument;

    for (;;)
    {
        if (xSemaphoreTake(s_rx_semaphore, portMAX_DELAY) == pdTRUE)
        {
            while (ethernetif_receive_one(netif) != 0)
            {
            }
        }
    }
}

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    uint8_t *buffer;
    uint16_t copied;

    LWIP_UNUSED_ARG(netif);

    if ((p->tot_len == 0U) || (p->tot_len > ETH_DRIVER_MAX_FRAME_LEN))
    {
        LINK_STATS_INC(link.lenerr);
        LINK_STATS_INC(link.drop);
        return ERR_BUF;
    }

    (void)ETH_Driver_ReclaimTxBuffers();
    buffer = ETH_Driver_TxBufferAcquire();

    if (buffer == NULL)
    {
        if (xSemaphoreTake(s_tx_semaphore, pdMS_TO_TICKS(NET_TX_TIMEOUT_MS)) == pdTRUE)
        {
            (void)ETH_Driver_ReclaimTxBuffers();
            buffer = ETH_Driver_TxBufferAcquire();
        }
    }

    if (buffer == NULL)
    {
        LINK_STATS_INC(link.memerr);
        LINK_STATS_INC(link.drop);
        return ERR_MEM;
    }

    copied = pbuf_copy_partial(p, buffer, p->tot_len, 0U);
    if (copied != p->tot_len)
    {
        ETH_Driver_TxBufferRelease(buffer);
        LINK_STATS_INC(link.err);
        LINK_STATS_INC(link.drop);
        return ERR_BUF;
    }

    if (ETH_Driver_Transmit(buffer, copied) != ETH_DRIVER_OK)
    {
        ETH_Driver_TxBufferRelease(buffer);
        LINK_STATS_INC(link.err);
        LINK_STATS_INC(link.drop);
        return ERR_IF;
    }

    LINK_STATS_INC(link.xmit);
    return ERR_OK;
}

static int ethernetif_link_is_usable(lan8742_link_state_t state)
{
    return ((state == LAN8742_LINK_10M_HALF)  ||
            (state == LAN8742_LINK_10M_FULL)  ||
            (state == LAN8742_LINK_100M_HALF) ||
            (state == LAN8742_LINK_100M_FULL)) ? 1 : 0;
}

static void ethernetif_link_down(struct netif *netif)
{
    if (s_mac_started != 0U)
    {
        (void)ETH_Driver_Stop();
        s_mac_started = 0U;
    }

    LOCK_TCPIP_CORE();
    netif_set_link_down(netif);
    UNLOCK_TCPIP_CORE();
}

static void ethernetif_link_up(struct netif *netif, lan8742_link_state_t state)
{
    if (s_mac_started != 0U)
    {
        (void)ETH_Driver_Stop();
        s_mac_started = 0U;
    }

    if (ETH_Driver_ConfigureLink(state) != ETH_DRIVER_OK)
    {
        return;
    }

    if (ETH_Driver_Start() != ETH_DRIVER_OK)
    {
        return;
    }

    s_mac_started = 1U;

    LOCK_TCPIP_CORE();
    netif_set_link_up(netif);
    UNLOCK_TCPIP_CORE();
}

static void ethernetif_log_phy(lan8742_t *phy)
{
    uint32_t bcr = 0U, bsr = 0U, anar = 0U, anlpar = 0U, physcsr = 0U;

    (void)LAN8742_ReadRegister(phy, LAN8742_BCR,     &bcr);
    (void)LAN8742_ReadRegister(phy, LAN8742_BSR,     &bsr);
    (void)LAN8742_ReadRegister(phy, LAN8742_ANAR,    &anar);
    (void)LAN8742_ReadRegister(phy, 0x05U,           &anlpar);
    (void)LAN8742_ReadRegister(phy, LAN8742_PHYSCSR, &physcsr);

    APP_LOG("[phy] bcr=%04lX bsr=%04lX anar=%04lX anlpar=%04lX physcsr=%04lX",
            (unsigned long)bcr, (unsigned long)bsr, (unsigned long)anar,
            (unsigned long)anlpar, (unsigned long)physcsr);
}

void ethernetif_link_thread(void *argument)
{
    struct netif *netif    = (struct netif *)argument;
    lan8742_t    *phy      = ETH_Driver_GetPhy();
    lan8742_link_state_t previous = LAN8742_LINK_ERROR;

    for (;;)
    {
        lan8742_link_state_t current = LAN8742_GetLinkState(phy);

        s_link_state = current;

        if (current != previous)
        {
            ethernetif_log_phy(phy);

            if (ethernetif_link_is_usable(current) != 0)
            {
                ethernetif_link_up(netif, current);
            }
            else
            {
                ethernetif_link_down(netif);
            }

            previous = current;
        }

        vTaskDelay(pdMS_TO_TICKS(NET_LINK_POLL_PERIOD_MS));
    }
}

const char *ethernetif_link_state_string(void)
{
    return LAN8742_LinkStateToString(s_link_state);
}

int ethernetif_link_is_up(void)
{
    return ((ethernetif_link_is_usable(s_link_state) != 0) && (s_mac_started != 0U)) ? 1 : 0;
}

static err_t low_level_init(struct netif *netif)
{
    static const uint8_t mac_address[6] =
    {
        BOARD_ETH_MAC_ADDR0, BOARD_ETH_MAC_ADDR1, BOARD_ETH_MAC_ADDR2,
        BOARD_ETH_MAC_ADDR3, BOARD_ETH_MAC_ADDR4, BOARD_ETH_MAC_ADDR5
    };

    s_rx_semaphore = xSemaphoreCreateBinary();
    s_tx_semaphore = xSemaphoreCreateBinary();
    if ((s_rx_semaphore == NULL) || (s_tx_semaphore == NULL))
    {
        return ERR_MEM;
    }

    ETH_Driver_SetEventCallbacks(&s_driver_callbacks);

    if (ETH_Driver_Init(mac_address, BOARD_ETH_PHY_ADDRESS) != ETH_DRIVER_OK)
    {
        return ERR_IF;
    }

    netif->hwaddr_len = ETHARP_HWADDR_LEN;
    memcpy(netif->hwaddr, mac_address, ETHARP_HWADDR_LEN);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;

    if (sys_thread_new("eth_rx", ethernetif_input_thread, netif,
                       NET_RX_THREAD_STACK_WORDS * sizeof(StackType_t),
                       NET_RX_THREAD_PRIO) == NULL)
    {
        return ERR_MEM;
    }

    return ERR_OK;
}

err_t ethernetif_init(struct netif *netif)
{
    LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
    netif->hostname = "stm32h755-cwt";
#endif

    netif->name[0]    = IFNAME0;
    netif->name[1]    = IFNAME1;
    netif->output     = etharp_output;
    netif->linkoutput = low_level_output;

    return low_level_init(netif);
}
