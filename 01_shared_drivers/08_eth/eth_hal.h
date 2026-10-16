/**
  ******************************************************************************
  * @file    eth_hal.h
  * @brief   Ethernet MAC + RMII + DMA driver for STM32H755ZI CPU1 (Cortex-M7).
  *
  *          Driver tier: *_hal (README.md section 10 - "Complex stacks"). The
  *          ETH MAC state machine, the MDIO controller and the descriptor
  *          rings are handled by the ST HAL; this file owns the board wiring,
  *          the memory placement and the ES0445 workarounds.
  *
  *          What this driver guarantees
  *          ---------------------------
  *          - Every byte the ETH DMA touches (descriptors, Rx buffers, Tx
  *            buffers) lives in D2 SRAM3, section .eth_ram / .RxDescripSection
  *            / .TxDescripSection / .RxArraySection, 32-byte aligned. The MPU
  *            marks that region Normal non-cacheable, so no cache maintenance
  *            is needed anywhere in the data path.
  *          - RMII only (ES0445 2.3.11: MII is not bonded out on this package).
  *          - ES0445 2.25.10 (FEP), 2.25.8 (Rx watchdog write order) and
  *            2.25.11 (ARP offload) are applied by ETH_Driver_Init().
  *
  *          Threading model
  *          ---------------
  *          The ETH interrupt only notifies; all descriptor work happens in
  *          task context. Install the notifications with
  *          ETH_Driver_SetEventCallbacks() before ETH_Driver_Start().
  ******************************************************************************
  */

#ifndef ETH_HAL_H
#define ETH_HAL_H

#include "stm32h7xx_hal.h"
#include "lan8742_phy.h"

#if defined(HAL_ETH_MODULE_ENABLED)

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Memory budget ----------------------------------------------------- *
 * .eth_ram is D2 SRAM3 at 0x30040000, 32 KB (see stm32h755zi_cm7.ld).
 * 1536 = 48 * 32: a whole number of cache lines and above the 1522-byte
 * maximum tagged Ethernet frame, so a frame never spans two descriptors.
 */
#define ETH_DRIVER_BUFFER_SIZE              1536U
#define ETH_DRIVER_RX_BUFFER_COUNT             8U
#define ETH_DRIVER_TX_BUFFER_COUNT             4U

/** Maximum payload this driver will transmit in one frame. */
#define ETH_DRIVER_MAX_FRAME_LEN  ETH_DRIVER_BUFFER_SIZE

/* ---- Checksum policy --------------------------------------------------- *
 * 0: lwIP computes and verifies every checksum in software.
 * 1: the MAC checksum offload engine does it.
 * The value has to agree with the CHECKSUM_* options in lwipopts.h, so it is
 * set once in CM7/CMakeLists.txt and seen by both. The default below only
 * applies when the build system says nothing.
 */
#ifndef ETH_USE_HARDWARE_CHECKSUM
#define ETH_USE_HARDWARE_CHECKSUM              0
#endif

typedef enum
{
    ETH_DRIVER_OK          =  0,
    ETH_DRIVER_ERROR       = -1,
    ETH_DRIVER_PARAM       = -2,
    ETH_DRIVER_NO_BUFFER   = -3,   /*!< Buffer pool exhausted               */
    ETH_DRIVER_NO_PACKET   = -4,   /*!< Nothing in the Rx ring              */
    ETH_DRIVER_PHY_ERROR   = -5
} eth_driver_status_t;

/**
  * @brief One received buffer. Frames up to ETH_DRIVER_BUFFER_SIZE arrive as a
  *        single segment; the chain exists so that an oversized frame or a
  *        smaller RxBuffLen still works.
  */
typedef struct eth_rx_segment_s
{
    struct eth_rx_segment_s *next;
    uint8_t                 *payload;
    uint16_t                 length;
} eth_rx_segment_t;

/** Notification hooks, called from the ETH interrupt. Keep them short. */
typedef struct
{
    void (*on_rx_complete)(void);
    void (*on_tx_complete)(void);
    void (*on_error)(uint32_t error_code);
} eth_driver_callbacks_t;

/**
  * @brief  Configures the RMII pins, the MAC, the DMA descriptor rings and the
  *         LAN8742A, then applies the ES0445 workarounds.
  * @param  mac_addr     Six-byte station address, copied internally.
  * @param  phy_address  SMI address of the transceiver (0 on this board).
  * @note   Leaves the MAC stopped. Call ETH_Driver_Start() once the PHY
  *         reports a link. Requires board_def.h defining all 18 BOARD_ETH_<SIG>_PORT
  *         and _PIN macros for the RMII signals (Rule 1). Pins are configured
  *         using GPIO_HAL_Init.
  *         If GPIO_HAL_Init fails for any pin, ETH_Driver_Init returns
  *         ETH_DRIVER_ERROR. Because HAL_ETH_Init() has already run, the MAC
  *         peripheral was initialized and ETH_IRQn was enabled in the NVIC.
  *         The caller should call HAL_ETH_DeInit(ETH_Driver_GetHandle()) or
  *         reset the peripheral before retrying.
  */
eth_driver_status_t ETH_Driver_Init(const uint8_t *mac_addr, uint32_t phy_address);

/** @brief  Starts MAC and DMA with end-of-transfer interrupts enabled. */
eth_driver_status_t ETH_Driver_Start(void);

/**
  * @brief  Stops MAC and DMA.
  * @note   Rx buffers already attached to descriptors stay attached and are
  *         reused on the next start, which is what keeps the ring consistent
  *         across the stop/reconfigure/start cycle a link change triggers.
  */
eth_driver_status_t ETH_Driver_Stop(void);

/**
  * @brief  Applies a negotiated link mode to the MAC.
  * @param  state  Value returned by LAN8742_GetLinkState().
  * @note   Must be called while the MAC is stopped.
  */
eth_driver_status_t ETH_Driver_ConfigureLink(lan8742_link_state_t state);

/** @brief  Installs the interrupt notification hooks. Pass NULL to remove. */
void ETH_Driver_SetEventCallbacks(const eth_driver_callbacks_t *callbacks);

/* ---- Transmit path ----------------------------------------------------- */

/**
  * @brief  Takes a Tx buffer out of the .eth_ram pool.
  * @retval NULL when every buffer is still owned by the DMA.
  */
uint8_t *ETH_Driver_TxBufferAcquire(void);

/** @brief  Returns an unused Tx buffer to the pool (error paths only). */
void ETH_Driver_TxBufferRelease(uint8_t *buffer);

/**
  * @brief  Hands a filled Tx buffer to the DMA and returns immediately.
  *         The buffer is released back to the pool by
  *         ETH_Driver_ReclaimTxBuffers() once the DMA is done with it.
  */
eth_driver_status_t ETH_Driver_Transmit(uint8_t *buffer, uint16_t length);

/** @brief  Releases the Tx buffers the DMA has finished with. Call after the
  *         transmit-complete notification. */
eth_driver_status_t ETH_Driver_ReclaimTxBuffers(void);

/* ---- Receive path ------------------------------------------------------ */

/**
  * @brief  Pops one complete frame from the Rx ring.
  * @param  packet  Receives the head of the segment chain.
  * @retval ETH_DRIVER_NO_PACKET when the ring holds no complete frame.
  * @note   The caller owns the segments until it calls
  *         ETH_Driver_ReleasePacket(). Buffers are not recycled into the DMA
  *         ring until then, so release promptly.
  */
eth_driver_status_t ETH_Driver_ReadPacket(eth_rx_segment_t **packet);

/** @brief  Returns a whole segment chain to the Rx pool. */
void ETH_Driver_ReleasePacket(eth_rx_segment_t *packet);

/* ---- Diagnostics ------------------------------------------------------- */

/** @brief  The HAL handle, for HAL_ETH_ReadPHYRegister() and for debugging. */
ETH_HandleTypeDef *ETH_Driver_GetHandle(void);

/** @brief  The PHY instance owned by this driver. */
lan8742_t *ETH_Driver_GetPhy(void);

/**
  * @brief  Counters that make a silent failure visible from a debugger or a log.
  * @note   Individual fields are incremented atomically (__atomic_fetch_add, relaxed).
  *         Reading an individual 32-bit field is atomic; reading the whole struct
  *         through ETH_Driver_GetStats() is not a coherent multi-word snapshot.
  */
typedef struct
{
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_alloc_failures;   /*!< Rx pool empty when the DMA needed one */
    uint32_t tx_alloc_failures;   /*!< Tx pool empty when lwIP needed one    */
    uint32_t dma_errors;          /*!< HAL_ETH_ErrorCallback invocations     */
} eth_driver_stats_t;

const eth_driver_stats_t *ETH_Driver_GetStats(void);

/* ---- Silicon errata, exposed for the bring-up guide -------------------- */

/**
  * @brief  ES0445 2.25.10 - sets FEP in ETH_MTLRXQOMR so that the error-packet
  *         drop mechanism does not discard good frames that a 10 Mbps RMII
  *         link falsely flagged with dribble + CRC errors.
  * @note   Already called by ETH_Driver_Init() and ETH_Driver_Start(). Exposed
  *         so it can be re-applied after a manual MAC reconfiguration.
  */
void ETH_Driver_ApplyErrataRxErrorPackets(void);

/**
  * @brief  ES0445 2.25.8 - programs the Rx interrupt watchdog (ETH_DMACRXIWTR)
  *         with the mandated two-write sequence: RWT[7:0] first while
  *         RWTU[1:0] stays zero, then RWTU[1:0] on its own.
  * @param  rwt   Timer count, 0 disables the watchdog.
  * @param  rwtu  Count units, 0..3 (256, 512, 1024, 2048 clock cycles).
  *               Forced to 0 when rwt is 0, which is the condition the errata
  *               forbids.
  */
void ETH_Driver_SetRxWatchdog(uint8_t rwt, uint8_t rwtu);

/* ---- MSP Override Hooks ------------------------------------------------ */
/**
  * @brief  Weak user hook for Ethernet RMII GPIO pinout and clock initialization.
  * @note   Two ways to customize Ethernet hardware:
  *         1. Implement a strong ETH_HAL_MspInit_User() in application code.
  *         2. Define ETH_HAL_CUSTOM_MSP at compile time (or before including eth_hal.h):
  *            suppresses HAL_ETH_MspInit() in eth_hal.c entirely, allowing full
  *            control from a project-local stm32h7xx_hal_msp.c without duplicate symbol errors.
  */
void ETH_HAL_MspInit_User(ETH_HandleTypeDef *heth);

/**
  * @brief  Weak user hook for Ethernet MSP de-initialization.
  */
void ETH_HAL_MspDeInit_User(ETH_HandleTypeDef *heth);

/* ---- Custom Callbacks Hook --------------------------------------------- */
/**
  * @note   Define ETH_HAL_CUSTOM_CALLBACKS at compile time to suppress definition
  *         of the six HAL ETH callbacks (HAL_ETH_RxAllocateCallback,
  *         HAL_ETH_RxLinkCallback, HAL_ETH_TxFreeCallback, HAL_ETH_RxCpltCallback,
  *         HAL_ETH_TxCpltCallback, HAL_ETH_ErrorCallback) in eth_hal.c, allowing
  *         the consuming project to provide its own HAL callbacks without duplicate
  *         symbol errors.
  */

#ifdef __cplusplus
}
#endif

#endif /* HAL_ETH_MODULE_ENABLED */

#endif /* ETH_HAL_H */
