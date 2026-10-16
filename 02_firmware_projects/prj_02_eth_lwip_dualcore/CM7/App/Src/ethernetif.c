/**
  ******************************************************************************
  * @file    ethernetif.c
  * @brief   lwIP network interface for the STM32H755ZI Ethernet MAC.
  *
  *          Layering
  *          --------
  *            lwIP  <->  this file  <->  08_eth/eth_hal.c  <->  MAC
  *
  *          The driver is the shared one under 01_shared_drivers/, linked
  *          through the CMake target stm32h7_driver_eth.
  *
  *          This file is the only place that knows about both lwIP and
  *          FreeRTOS. The driver below it knows about neither: it notifies
  *          through the plain function pointers installed here.
  *
  *          Data path
  *          ---------
  *          Frames are copied between pbufs and the driver's .eth_ram buffers.
  *          That keeps every DMA-visible byte inside the non-cacheable D2
  *          SRAM3 window and keeps every pbuf in fast DTCM, at the cost of one
  *          memcpy per frame in each direction.
  *
  *          Concurrency
  *          -----------
  *          Three contexts touch the ETH handle: the Rx thread (ReadData), the
  *          tcpip thread (Transmit / ReleaseTxPacket) and the link thread
  *          (Stop / SetMACConfig / Start). Tx and Rx use separate descriptor
  *          rings and separate HAL state, so those two do not collide. The
  *          link thread does stop the MAC underneath the other two, which is
  *          only safe because a link transition means no frames are moving;
  *          the HAL rejects the calls that arrive in the wrong state and both
  *          paths treat that as a dropped frame. A product that renegotiates
  *          under load should guard ETH_Driver_Start/Stop and the two data
  *          paths with a mutex.
  ******************************************************************************
  */

#include "ethernetif.h"
#include "eth_hal.h"
#include "board_def.h"
#include "net_config.h"
#include "app_log.h"

#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include "lwip/tcpip.h"
#include "lwip/sys.h"
#include "netif/etharp.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <string.h>

#if defined(ETH_HAL_RX_ZERO_COPY) && (ETH_HAL_RX_ZERO_COPY != 0)
struct rx_custom_pbuf
{
    struct pbuf_custom custom;
    void              *dma_buffer;
};

LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_DRIVER_RX_BUFFER_COUNT, sizeof(struct rx_custom_pbuf), "Zero-copy RX PBUF pool");

static void ethernetif_rx_pbuf_free_custom(struct pbuf *p)
{
    struct rx_custom_pbuf *cp = (struct rx_custom_pbuf *)p;

    ETH_Driver_ReleaseRxBuffer(cp->dma_buffer);
    LWIP_MEMPOOL_FREE(RX_POOL, cp);
}
#endif

/* Interface name reported by lwIP: "st0", "st1", ... */
#define IFNAME0                         's'
#define IFNAME1                         't'

/* ==========================================================================
 * lwIP port function.
 * ========================================================================== */

/* lwIP counts timeouts in milliseconds and ST's sys_arch.c measures its own
 * waits with the CMSIS-RTOS tick, which is the FreeRTOS tick. Reading the same
 * counter here keeps the whole stack on one time base; HAL_GetTick() would be
 * a second, independent 1 kHz source (TIM6) that slowly drifts against it. */
_Static_assert(configTICK_RATE_HZ == 1000,
               "sys_now() returns milliseconds and assumes a 1 kHz FreeRTOS tick");

u32_t sys_now(void)
{
    return (u32_t)xTaskGetTickCount();
}

static SemaphoreHandle_t              s_rx_semaphore;
static SemaphoreHandle_t              s_tx_semaphore;
static volatile uint8_t               s_mac_started;
/* Written by the link thread, read by any task that wants to log it. */
static volatile lan8742_link_state_t  s_link_state = LAN8742_LINK_DOWN;

/* ==========================================================================
 * Driver notifications. These run in the ETH interrupt (priority 6, which is
 * numerically above configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, so the
 * ...FromISR API is legal here).
 * ========================================================================== */

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

    /* A DMA error aborts whatever transmission was in flight. Wake the
       transmitter so it stops waiting for a completion that will not come;
       the error counter in eth_hal.c records the event. */
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

/* ==========================================================================
 * Receive path.
 * ========================================================================== */

/**
  * @brief  Moves one frame from the DMA ring into lwIP.
  * @retval 1 when a frame was taken out of the ring, 0 when it was empty.
  */
static int ethernetif_receive_one(struct netif *netif)
{
    eth_rx_segment_t *packet  = NULL;
    eth_rx_segment_t *segment;
#if defined(ETH_HAL_RX_ZERO_COPY) && (ETH_HAL_RX_ZERO_COPY != 0)
    struct pbuf      *p_head = NULL;
    struct rx_custom_pbuf *cp;
    struct pbuf      *p;
#else
    struct pbuf      *p;
    uint16_t          total   = 0U;
    uint16_t          offset  = 0U;
#endif

    if (ETH_Driver_ReadPacket(&packet) != ETH_DRIVER_OK)
    {
        return 0;
    }

#if defined(ETH_HAL_RX_ZERO_COPY) && (ETH_HAL_RX_ZERO_COPY != 0)
    for (segment = packet; segment != NULL; segment = segment->next)
    {
        cp = (struct rx_custom_pbuf *)LWIP_MEMPOOL_ALLOC(RX_POOL);
        if (cp == NULL)
        {
            if (p_head != NULL)
            {
                pbuf_free(p_head);
                p_head = NULL;
            }
            while (segment != NULL)
            {
                ETH_Driver_ReleaseRxBuffer(segment->payload);
                segment = segment->next;
            }
            break;
        }

        cp->custom.custom_free_function = ethernetif_rx_pbuf_free_custom;
        cp->dma_buffer = segment->payload;
        p = pbuf_alloced_custom(PBUF_RAW, segment->length, PBUF_REF,
                                &cp->custom, segment->payload, ETH_DRIVER_BUFFER_SIZE);
        if (p == NULL)
        {
            LWIP_MEMPOOL_FREE(RX_POOL, cp);
            if (p_head != NULL)
            {
                pbuf_free(p_head);
                p_head = NULL;
            }
            while (segment != NULL)
            {
                ETH_Driver_ReleaseRxBuffer(segment->payload);
                segment = segment->next;
            }
            break;
        }

        if (p_head == NULL)
        {
            p_head = p;
        }
        else
        {
            pbuf_cat(p_head, p);
        }
    }

    if (p_head == NULL)
    {
        LINK_STATS_INC(link.memerr);
        LINK_STATS_INC(link.drop);
        return 1;   /* the ring did hold a frame, so keep draining */
    }

    LINK_STATS_INC(link.recv);

    if (netif->input(p_head, netif) != ERR_OK)
    {
        pbuf_free(p_head);
    }

    return 1;
#else
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

    /* Return the DMA buffers as early as possible: the ring only has
       ETH_RX_DESC_CNT descriptors and they are refilled from this pool. */
    ETH_Driver_ReleasePacket(packet);

    if (p == NULL)
    {
        LINK_STATS_INC(link.memerr);
        LINK_STATS_INC(link.drop);
        return 1;   /* the ring did hold a frame, so keep draining */
    }

    LINK_STATS_INC(link.recv);

    if (netif->input(p, netif) != ERR_OK)
    {
        pbuf_free(p);
    }

    return 1;
#endif
}

static void ethernetif_input_thread(void *argument)
{
    struct netif *netif = (struct netif *)argument;

    for (;;)
    {
        if (xSemaphoreTake(s_rx_semaphore, portMAX_DELAY) == pdTRUE)
        {
            /* One interrupt can cover several descriptors, and more frames can
               arrive while this loop runs. Drain until the ring is empty. */
            while (ethernetif_receive_one(netif) != 0)
            {
            }
        }
    }
}

/* ==========================================================================
 * Transmit path.
 * ========================================================================== */

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

    /* Reclaim whatever the DMA has already sent, then take a slot. */
    (void)ETH_Driver_ReclaimTxBuffers();
    buffer = ETH_Driver_TxBufferAcquire();

    if (buffer == NULL)
    {
        /* Every slot is still owned by the DMA. Wait for one completion
           rather than dropping the frame outright. */
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

/* ==========================================================================
 * Link supervision.
 * ========================================================================== */

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

    /* netif state belongs to the tcpip thread; take the core lock because
       this runs in the link supervision thread. */
    LOCK_TCPIP_CORE();
    netif_set_link_down(netif);
    UNLOCK_TCPIP_CORE();
}

static void ethernetif_link_up(struct netif *netif, lan8742_link_state_t state)
{
    /* Speed and duplex can only be programmed while the MAC is stopped. */
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

/**
  * @brief  Dumps the clause-22 registers that explain a link.
  * @note   ANLPAR (reg 5) is the interesting one when the negotiated speed is
  *         not what you expected: it is what the link partner advertised, so
  *         it separates "the partner is a 10 Mbps device" from "our PHY failed
  *         to read the partner's capabilities".
  */
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
    APP_LOG("[phy] partner advertised:%s%s%s%s",
            ((anlpar & 0x0100U) != 0U) ? " 100BASE-TX-FD" : "",
            ((anlpar & 0x0080U) != 0U) ? " 100BASE-TX-HD" : "",
            ((anlpar & 0x0040U) != 0U) ? " 10BASE-T-FD"   : "",
            ((anlpar & 0x0020U) != 0U) ? " 10BASE-T-HD"   : "");
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
    /* Both halves matter: the PHY has a usable link AND the MAC was
       successfully reconfigured and restarted for it. */
    return ((ethernetif_link_is_usable(s_link_state) != 0) && (s_mac_started != 0U)) ? 1 : 0;
}

/* ==========================================================================
 * netif initialisation.
 * ========================================================================== */

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

#if defined(ETH_HAL_RX_ZERO_COPY) && (ETH_HAL_RX_ZERO_COPY != 0)
    LWIP_MEMPOOL_INIT(RX_POOL);
#endif

    /* Install the notifications before the MAC can raise an interrupt. */
    ETH_Driver_SetEventCallbacks(&s_driver_callbacks);

    if (ETH_Driver_Init(mac_address, BOARD_ETH_PHY_ADDRESS) != ETH_DRIVER_OK)
    {
        return ERR_IF;
    }

    netif->hwaddr_len = ETHARP_HWADDR_LEN;
    memcpy(netif->hwaddr, mac_address, ETHARP_HWADDR_LEN);
    netif->mtu = 1500;

    /* NETIF_FLAG_LINK_UP is deliberately absent: it is raised by
       ethernetif_link_thread() once the PHY has finished negotiating. */
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
    netif->hostname = "stm32h755-cm7";
#endif

    netif->name[0]    = IFNAME0;
    netif->name[1]    = IFNAME1;
    netif->output     = etharp_output;   /* IPv4 -> ARP -> linkoutput */
    netif->linkoutput = low_level_output;

    return low_level_init(netif);
}
