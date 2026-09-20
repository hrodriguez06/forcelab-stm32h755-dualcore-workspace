/**
  ******************************************************************************
  * @file    eth_hal.c
  * @brief   Ethernet MAC + RMII + DMA driver for STM32H755ZI CPU1 (Cortex-M7).
  *
  *          RMII pin map (NUCLEO-H755ZI-Q, all AF11, verified against
  *          00_docs_stm32h7/STM32H755_Alternate_Functions.md):
  *
  *            PA1  ETH_RMII_REF_CLK   50 MHz, driven by the LAN8742A
  *            PA2  ETH_MDIO
  *            PC1  ETH_MDC
  *            PA7  ETH_RMII_CRS_DV
  *            PC4  ETH_RMII_RXD0
  *            PC5  ETH_RMII_RXD1
  *            PG11 ETH_RMII_TX_EN
  *            PG13 ETH_RMII_TXD0
  *            PB13 ETH_RMII_TXD1
  ******************************************************************************
  */

#include "eth_hal.h"
#include <string.h>

#if defined(HAL_ETH_MODULE_ENABLED)

/* ---- Interrupt priority ------------------------------------------------ *
 * The ETH ISR signals FreeRTOS objects, so its priority must be numerically
 * greater than or equal to configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5).
 * With NVIC_PRIORITYGROUP_4 all four bits are preemption priority.
 * Override from the build system if the application needs a different slot.
 */
#ifndef ETH_DRIVER_IRQ_PRIORITY
#define ETH_DRIVER_IRQ_PRIORITY                 6U
#endif

/* ---- Register naming: RM0399 vs CMSIS ----------------------------------- *
 * The reference manual and the errata sheet spell two of the registers used
 * below with an "X" that the CMSIS header omits. They are the same registers:
 *
 *   RM0399 / ES0445      stm32h755xx.h        offset
 *   ETH_MTLRXQOMR        ETH->MTLRQOMR        0x0D30
 *   ETH_DMACRXIWTR       ETH->DMACRIWTR       0x1138
 *
 * Code uses the CMSIS spelling; comments quote the manual's, so that the
 * errata references stay greppable against the documentation hub.
 *
 * CMSIS defines ETH_DMACRIWTR_RWT but no mask for RWTU[1:0], so that one is
 * declared here from RM0399 bits 17:16.
 */
#define ETH_DMACRIWTR_RWTU_MASK        0x00030000UL
#define ETH_DMACRIWTR_RWTU_POS                  16U

/* ==========================================================================
 * DMA-visible memory. Everything below lands in D2 SRAM3 (0x30040000, 32 KB)
 * through the .eth_ram output section of CM7/linker/stm32h755zi_cm7.ld, and
 * the MPU marks that region Normal non-cacheable (mpu_config.c, region 2).
 * DTCM and AXI SRAM are both wrong here: DTCM is unreachable by the ETH DMA,
 * AXI SRAM is cacheable and would need manual maintenance on every frame.
 * ========================================================================== */

static ETH_DMADescTypeDef g_rx_desc[ETH_RX_DESC_CNT]
    __attribute__((section(".RxDescripSection"), aligned(32)));

static ETH_DMADescTypeDef g_tx_desc[ETH_TX_DESC_CNT]
    __attribute__((section(".TxDescripSection"), aligned(32)));

static uint8_t g_rx_pool[ETH_DRIVER_RX_BUFFER_COUNT][ETH_DRIVER_BUFFER_SIZE]
    __attribute__((section(".RxArraySection"), aligned(32)));

static uint8_t g_tx_pool[ETH_DRIVER_TX_BUFFER_COUNT][ETH_DRIVER_BUFFER_SIZE]
    __attribute__((section(".eth_ram"), aligned(32)));

/* Fail the build instead of silently overflowing SRAM3. */
_Static_assert((sizeof(g_rx_pool) + sizeof(g_tx_pool) +
                sizeof(g_rx_desc) + sizeof(g_tx_desc)) <= (32U * 1024U),
               "ETH descriptors and buffers do not fit in D2 SRAM3 (.eth_ram)");
_Static_assert((ETH_DRIVER_BUFFER_SIZE % 32U) == 0U,
               "ETH buffer size must be a whole number of 32-byte cache lines");
_Static_assert(ETH_DRIVER_RX_BUFFER_COUNT > ETH_RX_DESC_CNT,
               "Rx pool must exceed the descriptor count or the ring starves");

/* ==========================================================================
 * Pool bookkeeping. Kept out of .eth_ram: the DMA never reads it.
 * ========================================================================== */

typedef struct
{
    eth_rx_segment_t segment;
    volatile uint8_t in_use;
} eth_rx_slot_t;

static eth_rx_slot_t       g_rx_slots[ETH_DRIVER_RX_BUFFER_COUNT];
static volatile uint8_t    g_tx_slot_in_use[ETH_DRIVER_TX_BUFFER_COUNT];

static ETH_HandleTypeDef       g_heth;
static lan8742_t               g_phy;
static uint8_t                 g_mac_addr[6];
static eth_driver_callbacks_t  g_callbacks;
static eth_driver_stats_t      g_stats;

/* Re-entrant critical section: usable from thread context, from an ISR and
 * from inside a FreeRTOS critical section, because it restores the previous
 * PRIMASK instead of unconditionally re-enabling interrupts. */
static inline uint32_t eth_lock(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static inline void eth_unlock(uint32_t primask)
{
    __set_PRIMASK(primask);
}

/* Both pools are contiguous arrays, so a buffer address maps straight back to
 * its slot index. Anything outside the pool returns -1, which is how a bogus
 * pointer coming back from the HAL is rejected instead of corrupting a slot. */
static int32_t eth_rx_index_of(const uint8_t *buffer)
{
    const uint8_t *base = &g_rx_pool[0][0];

    if ((buffer < base) || (buffer >= (base + sizeof(g_rx_pool))))
    {
        return -1;
    }

    return (int32_t)((uint32_t)(buffer - base) / ETH_DRIVER_BUFFER_SIZE);
}

static int32_t eth_tx_index_of(const uint8_t *buffer)
{
    const uint8_t *base = &g_tx_pool[0][0];

    if ((buffer < base) || (buffer >= (base + sizeof(g_tx_pool))))
    {
        return -1;
    }

    return (int32_t)((uint32_t)(buffer - base) / ETH_DRIVER_BUFFER_SIZE);
}

static void eth_pools_reset(void)
{
    uint32_t i;

    for (i = 0U; i < ETH_DRIVER_RX_BUFFER_COUNT; i++)
    {
        g_rx_slots[i].segment.next    = NULL;
        g_rx_slots[i].segment.payload = &g_rx_pool[i][0];
        g_rx_slots[i].segment.length  = 0U;
        g_rx_slots[i].in_use          = 0U;
    }

    for (i = 0U; i < ETH_DRIVER_TX_BUFFER_COUNT; i++)
    {
        g_tx_slot_in_use[i] = 0U;
    }
}

/* ==========================================================================
 * Low level MSP: clocks, RMII pins, NVIC.
 * ========================================================================== */

/**
  * @brief  Default MSP pinout and clock configuration for Nucleo-144 on-board
  *         Ethernet RMII interface.
  * @note   Declared as __weak so any project or custom board can override it in
  *         Core/Src/stm32h7xx_hal_msp.c or BSP without modifying this driver.
  */
__weak void HAL_ETH_MspInit(ETH_HandleTypeDef *heth)
{
    GPIO_InitTypeDef gpio = {0};

    if (heth->Instance != ETH)
    {
        return;
    }

    /* The RMII/MII selection lives in SYSCFG_PMCR.EPIS and RM0399 requires it
       to be settled before the MAC leaves reset. HAL_ETH_Init() programs it
       again from Init.MediaInterface right before asserting DMAMR.SWR; doing
       it here as well guarantees the ordering whatever the HAL revision.
       ES0445 2.3.11: RMII is the only usable mode on this package. */
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    HAL_SYSCFG_ETHInterfaceSelect(SYSCFG_ETH_RMII);

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;   /* 50 MHz RMII timing */
    gpio.Alternate = GPIO_AF11_ETH;

    /* PA1 REF_CLK, PA2 MDIO, PA7 CRS_DV */
    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PB13 TXD1 */
    gpio.Pin = GPIO_PIN_13;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* PC1 MDC, PC4 RXD0, PC5 RXD1 */
    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* PG11 TX_EN, PG13 TXD0 */
    gpio.Pin = GPIO_PIN_11 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOG, &gpio);

    /* Enabling the ETH clocks from CPU1 also allocates the D2 domain clock to
       CPU1, which is what keeps the MAC alive when CPU2 is not running. */
    __HAL_RCC_ETH1MAC_CLK_ENABLE();
    __HAL_RCC_ETH1TX_CLK_ENABLE();
    __HAL_RCC_ETH1RX_CLK_ENABLE();

    HAL_NVIC_SetPriority(ETH_IRQn, ETH_DRIVER_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);
}

__weak void HAL_ETH_MspDeInit(ETH_HandleTypeDef *heth)
{
    if (heth->Instance != ETH)
    {
        return;
    }

    HAL_NVIC_DisableIRQ(ETH_IRQn);

    __HAL_RCC_ETH1MAC_CLK_DISABLE();
    __HAL_RCC_ETH1TX_CLK_DISABLE();
    __HAL_RCC_ETH1RX_CLK_DISABLE();

    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_13);
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);
    HAL_GPIO_DeInit(GPIOG, GPIO_PIN_11 | GPIO_PIN_13);
}

/* ==========================================================================
 * ES0445 workarounds.
 * ========================================================================== */

void ETH_Driver_ApplyErrataRxErrorPackets(void)
{
    /* ES0445 2.25.10 - "Packets dropped in RMII 10 Mbps mode due to fake
       dribble and CRC error". At 10 Mbps the RMII block can append a phantom
       nibble, which raises dribble + CRC and makes the error-packet drop
       mechanism discard a perfectly good frame. Setting FEP forwards those
       frames to the application instead. Harmless at 100 Mbps, so it is
       applied unconditionally rather than only on a 10 Mbps link.
       ETH_MTLRXQOMR in the manual; MTLRQOMR in the CMSIS header. */
    ETH->MTLRQOMR |= ETH_MTLRQOMR_FEP;
}

static void eth_disable_arp_offload(void)
{
    /* ES0445 2.25.11 - "ARP offload function not effective". The hardware
       responder must stay off; lwIP answers ARP in software (etharp). The ARP
       offload enable is bit 31 of ETH_MACCR (ARPEN in the manual, ETH_MACCR_ARP
       in the CMSIS header) and is zero out of reset; it is cleared here so the
       intent survives any future MAC reconfiguration. */
    ETH->MACCR &= ~ETH_MACCR_ARP;
}

void ETH_Driver_SetRxWatchdog(uint8_t rwt, uint8_t rwtu)
{
    uint32_t reg;

    /* ES0445 2.25.8 - "Spurious receive watchdog timeout interrupt". A
       non-zero RWTU while RWT is zero fires an interrupt with no packet to
       process. Two successive writes are mandated: RWT first with RWTU held
       at zero, then RWTU on its own with RWT unchanged. */

    if (rwt == 0U)
    {
        /* Watchdog off: RWTU must go to zero as well, never on its own. */
        ETH->DMACRIWTR &= ~(ETH_DMACRIWTR_RWT | ETH_DMACRIWTR_RWTU_MASK);
        return;
    }

    /* First write: RWT[7:0], RWTU[1:0] kept at zero. */
    reg  = ETH->DMACRIWTR & ~(ETH_DMACRIWTR_RWT | ETH_DMACRIWTR_RWTU_MASK);
    reg |= ((uint32_t)rwt & ETH_DMACRIWTR_RWT);
    ETH->DMACRIWTR = reg;

    /* Second write: RWTU[1:0], RWT[7:0] left untouched. */
    reg  = ETH->DMACRIWTR & ~ETH_DMACRIWTR_RWTU_MASK;
    reg |= (((uint32_t)rwtu << ETH_DMACRIWTR_RWTU_POS) & ETH_DMACRIWTR_RWTU_MASK);
    ETH->DMACRIWTR = reg;
}

/* ==========================================================================
 * Initialisation.
 * ========================================================================== */

eth_driver_status_t ETH_Driver_Init(const uint8_t *mac_addr, uint32_t phy_address)
{
    ETH_MACFilterConfigTypeDef filter = {0};
    ETH_MACConfigTypeDef       mac    = {0};

    if (mac_addr == NULL)
    {
        return ETH_DRIVER_PARAM;
    }

    memcpy(g_mac_addr, mac_addr, sizeof(g_mac_addr));
    memset(&g_stats, 0, sizeof(g_stats));
    eth_pools_reset();

    g_heth.Instance            = ETH;
    g_heth.Init.MACAddr        = g_mac_addr;
    g_heth.Init.MediaInterface = HAL_ETH_RMII_MODE;   /* ES0445 2.3.11 */
    g_heth.Init.TxDesc         = g_tx_desc;
    g_heth.Init.RxDesc         = g_rx_desc;
    g_heth.Init.RxBuffLen      = ETH_DRIVER_BUFFER_SIZE;

    if (HAL_ETH_Init(&g_heth) != HAL_OK)
    {
        return ETH_DRIVER_ERROR;
    }

    /* Derive the MDIO clock divider from the current HCLK. HAL_ETH_Init()
       already does this; repeating it keeps MDC correct if the clock tree is
       reconfigured and the driver re-initialised. */
    HAL_ETH_SetMDIOClockRange(&g_heth);

    /* Errata applied after HAL_ETH_Init(): the software reset it performs
       (DMAMR.SWR) returns MTL and DMA registers to their reset values. */
    ETH_Driver_ApplyErrataRxErrorPackets();       /* 2.25.10 */
    eth_disable_arp_offload();                    /* 2.25.11 */
    ETH_Driver_SetRxWatchdog(0U, 0U);             /* 2.25.8, watchdog off  */

    /* Checksum offload follows the application policy. Software checksums are
       the default (net_config.h): they keep the MAC checksum engine out of
       the picture while the link is being brought up. */
    if (HAL_ETH_GetMACConfig(&g_heth, &mac) != HAL_OK)
    {
        return ETH_DRIVER_ERROR;
    }
#if defined(ETH_USE_HARDWARE_CHECKSUM) && (ETH_USE_HARDWARE_CHECKSUM == 1)
    mac.ChecksumOffload = ENABLE;
#else
    mac.ChecksumOffload = DISABLE;
#endif
    if (HAL_ETH_SetMACConfig(&g_heth, &mac) != HAL_OK)
    {
        return ETH_DRIVER_ERROR;
    }

    /* Perfect filtering on the station address, and broadcasts accepted so
       that ARP and DHCP work. No promiscuous mode.

       BroadcastFilter is inverted logic, and getting it backwards is silent:
       the HAL writes it straight into ETH_MACPFR bit 5, which is DBF,
       "Disable Broadcast Packets". ENABLE therefore *blocks* broadcast, the
       board never sees an ARP request, never answers one, and nothing can
       reach it - while every counter and register still looks healthy.
       DISABLE is what lets broadcast through. */
    filter.PromiscuousMode     = DISABLE;
    filter.ReceiveAllMode      = DISABLE;
    filter.HachOrPerfectFilter = DISABLE;
    filter.PassAllMulticast    = DISABLE;
    filter.BroadcastFilter     = DISABLE;
    if (HAL_ETH_SetMACFilterConfig(&g_heth, &filter) != HAL_OK)
    {
        return ETH_DRIVER_ERROR;
    }

    if (LAN8742_Init(&g_phy, &g_heth, phy_address) != LAN8742_OK)
    {
        return ETH_DRIVER_PHY_ERROR;
    }

    return ETH_DRIVER_OK;
}

void ETH_Driver_SetEventCallbacks(const eth_driver_callbacks_t *callbacks)
{
    uint32_t primask = eth_lock();

    if (callbacks == NULL)
    {
        memset(&g_callbacks, 0, sizeof(g_callbacks));
    }
    else
    {
        g_callbacks = *callbacks;
    }

    eth_unlock(primask);
}

eth_driver_status_t ETH_Driver_Start(void)
{
    /* Re-assert FEP: a link that renegotiated down to 10 Mbps is exactly the
       case ES0445 2.25.10 describes, and Start is the point where the new
       speed has just been programmed. */
    ETH_Driver_ApplyErrataRxErrorPackets();

    if (HAL_ETH_Start_IT(&g_heth) != HAL_OK)
    {
        return ETH_DRIVER_ERROR;
    }

    return ETH_DRIVER_OK;
}

eth_driver_status_t ETH_Driver_Stop(void)
{
    uint32_t i;

    if (HAL_ETH_Stop_IT(&g_heth) != HAL_OK)
    {
        return ETH_DRIVER_ERROR;
    }

    /* Recover Tx buffers the DMA will now never finish.
     *
     * HAL_ETH_Stop_IT() flushes the Tx FIFO and halts the DMA, so any
     * descriptor still marked OWN stays marked OWN for good.
     * HAL_ETH_ReleaseTxPacket() only frees a descriptor whose OWN bit is
     * clear, so it stops at the first stranded one and never calls
     * HAL_ETH_TxFreeCallback() for it - the matching g_tx_slot_in_use[] flag
     * would stay set forever. A few link drops with traffic in flight and the
     * Tx pool is permanently empty, which shows up much later as
     * tx_alloc_failures climbing with no obvious cause.
     *
     * Clearing OWN here hands those descriptors back through the HAL's own
     * bookkeeping (BuffersInUse, releaseIndex, PacketAddress[]) rather than
     * poking the pool directly, so the driver and the HAL stay in agreement.
     * Safe only because the DMA is already stopped. */
    for (i = 0U; i < (uint32_t)ETH_TX_DESC_CNT; i++)
    {
        g_tx_desc[i].DESC3 &= ~ETH_DMATXNDESCRF_OWN;
    }

    (void)HAL_ETH_ReleaseTxPacket(&g_heth);

    return ETH_DRIVER_OK;
}

eth_driver_status_t ETH_Driver_ConfigureLink(lan8742_link_state_t state)
{
    ETH_MACConfigTypeDef mac = {0};

    if (HAL_ETH_GetMACConfig(&g_heth, &mac) != HAL_OK)
    {
        return ETH_DRIVER_ERROR;
    }

    switch (state)
    {
        case LAN8742_LINK_100M_FULL:
            mac.Speed      = ETH_SPEED_100M;
            mac.DuplexMode = ETH_FULLDUPLEX_MODE;
            break;
        case LAN8742_LINK_100M_HALF:
            mac.Speed      = ETH_SPEED_100M;
            mac.DuplexMode = ETH_HALFDUPLEX_MODE;
            break;
        case LAN8742_LINK_10M_FULL:
            mac.Speed      = ETH_SPEED_10M;
            mac.DuplexMode = ETH_FULLDUPLEX_MODE;
            break;
        case LAN8742_LINK_10M_HALF:
            mac.Speed      = ETH_SPEED_10M;
            mac.DuplexMode = ETH_HALFDUPLEX_MODE;
            break;
        default:
            return ETH_DRIVER_PARAM;
    }

    if (HAL_ETH_SetMACConfig(&g_heth, &mac) != HAL_OK)
    {
        return ETH_DRIVER_ERROR;
    }

    return ETH_DRIVER_OK;
}

/* ==========================================================================
 * Transmit path.
 * ========================================================================== */

uint8_t *ETH_Driver_TxBufferAcquire(void)
{
    uint8_t *buffer = NULL;
    uint32_t primask;
    uint32_t i;

    primask = eth_lock();
    for (i = 0U; i < ETH_DRIVER_TX_BUFFER_COUNT; i++)
    {
        if (g_tx_slot_in_use[i] == 0U)
        {
            g_tx_slot_in_use[i] = 1U;
            buffer = &g_tx_pool[i][0];
            break;
        }
    }
    eth_unlock(primask);

    if (buffer == NULL)
    {
        g_stats.tx_alloc_failures++;
    }

    return buffer;
}

void ETH_Driver_TxBufferRelease(uint8_t *buffer)
{
    int32_t  index = eth_tx_index_of(buffer);
    uint32_t primask;

    if (index < 0)
    {
        return;
    }

    primask = eth_lock();
    g_tx_slot_in_use[index] = 0U;
    eth_unlock(primask);
}

eth_driver_status_t ETH_Driver_Transmit(uint8_t *buffer, uint16_t length)
{
    ETH_TxPacketConfigTypeDef config = {0};
    ETH_BufferTypeDef         txbuf  = {0};

    if ((eth_tx_index_of(buffer) < 0) || (length == 0U) ||
        (length > ETH_DRIVER_MAX_FRAME_LEN))
    {
        return ETH_DRIVER_PARAM;
    }

    txbuf.buffer = buffer;
    txbuf.len    = length;
    txbuf.next   = NULL;

    config.Length     = length;
    config.TxBuffer   = &txbuf;
    /* Handed back verbatim to HAL_ETH_TxFreeCallback() once the DMA is done,
       which is how the buffer finds its way back to the pool. */
    config.pData      = buffer;
    config.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD;
    config.CRCPadCtrl = ETH_CRC_PAD_INSERT;
#if defined(ETH_USE_HARDWARE_CHECKSUM) && (ETH_USE_HARDWARE_CHECKSUM == 1)
    config.Attributes  |= ETH_TX_PACKETS_FEATURES_CSUM;
    config.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
#endif

    if (HAL_ETH_Transmit_IT(&g_heth, &config) != HAL_OK)
    {
        return ETH_DRIVER_ERROR;
    }

    g_stats.tx_frames++;
    return ETH_DRIVER_OK;
}

eth_driver_status_t ETH_Driver_ReclaimTxBuffers(void)
{
    if (HAL_ETH_ReleaseTxPacket(&g_heth) != HAL_OK)
    {
        return ETH_DRIVER_ERROR;
    }

    return ETH_DRIVER_OK;
}

/* ==========================================================================
 * Receive path.
 * ========================================================================== */

eth_driver_status_t ETH_Driver_ReadPacket(eth_rx_segment_t **packet)
{
    void *head = NULL;

    if (packet == NULL)
    {
        return ETH_DRIVER_PARAM;
    }

    *packet = NULL;

    if (HAL_ETH_ReadData(&g_heth, &head) != HAL_OK)
    {
        return ETH_DRIVER_NO_PACKET;
    }

    *packet = (eth_rx_segment_t *)head;
    g_stats.rx_frames++;

    return ETH_DRIVER_OK;
}

void ETH_Driver_ReleasePacket(eth_rx_segment_t *packet)
{
    eth_rx_segment_t *segment = packet;

    while (segment != NULL)
    {
        eth_rx_segment_t *next  = segment->next;
        int32_t           index = eth_rx_index_of(segment->payload);
        uint32_t          primask;

        if (index >= 0)
        {
            primask = eth_lock();
            g_rx_slots[index].segment.next   = NULL;
            g_rx_slots[index].segment.length = 0U;
            g_rx_slots[index].in_use         = 0U;
            eth_unlock(primask);
        }

        segment = next;
    }
}

/* ==========================================================================
 * HAL buffer-management hooks.
 *
 * With USE_HAL_ETH_REGISTER_CALLBACKS == 0 (see stm32h7xx_hal_conf.h) the HAL
 * calls these __weak symbols directly, so overriding them here is the whole
 * registration mechanism. Turning register-callbacks on additionally requires
 * HAL_ETH_RegisterRxAllocateCallback() and friends in ETH_Driver_Init().
 * ========================================================================== */

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
    uint32_t primask;
    uint32_t i;

    *buff = NULL;

    primask = eth_lock();
    for (i = 0U; i < ETH_DRIVER_RX_BUFFER_COUNT; i++)
    {
        if (g_rx_slots[i].in_use == 0U)
        {
            g_rx_slots[i].in_use         = 1U;
            g_rx_slots[i].segment.next   = NULL;
            g_rx_slots[i].segment.length = 0U;
            *buff = &g_rx_pool[i][0];
            break;
        }
    }
    eth_unlock(primask);

    if (*buff == NULL)
    {
        /* The DMA leaves the descriptor unowned and the HAL retries on the
           next HAL_ETH_ReadData(). A rising counter here means the input
           thread is not releasing packets fast enough, or the pool is too
           small for the traffic. */
        g_stats.rx_alloc_failures++;
    }
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
    int32_t index = eth_rx_index_of(buff);

    if (index < 0)
    {
        return;
    }

    g_rx_slots[index].segment.next    = NULL;
    g_rx_slots[index].segment.payload = buff;
    g_rx_slots[index].segment.length  = Length;

    if (*pStart == NULL)
    {
        *pStart = &g_rx_slots[index].segment;
    }
    else
    {
        ((eth_rx_segment_t *)*pEnd)->next = &g_rx_slots[index].segment;
    }

    *pEnd = &g_rx_slots[index].segment;
}

void HAL_ETH_TxFreeCallback(uint32_t *buff)
{
    ETH_Driver_TxBufferRelease((uint8_t *)buff);
}

/* ==========================================================================
 * HAL event callbacks. These run in the ETH interrupt: notify and return.
 * ========================================================================== */

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth)
{
    (void)heth;

    if (g_callbacks.on_rx_complete != NULL)
    {
        g_callbacks.on_rx_complete();
    }
}

void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth)
{
    (void)heth;

    if (g_callbacks.on_tx_complete != NULL)
    {
        g_callbacks.on_tx_complete();
    }
}

void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth)
{
    uint32_t error = HAL_ETH_GetError(heth);

    g_stats.dma_errors++;

    if (g_callbacks.on_error != NULL)
    {
        g_callbacks.on_error(error);
    }
}

/* ==========================================================================
 * Diagnostics.
 * ========================================================================== */

ETH_HandleTypeDef *ETH_Driver_GetHandle(void)
{
    return &g_heth;
}

lan8742_t *ETH_Driver_GetPhy(void)
{
    return &g_phy;
}

const eth_driver_stats_t *ETH_Driver_GetStats(void)
{
    return &g_stats;
}

#endif /* HAL_ETH_MODULE_ENABLED */
