/**
  ******************************************************************************
  * @file    lan8742_phy.h
  * @brief   Microchip LAN8742A transceiver driver (SMI / MDIO), HAL tier.
  *
  *          Driver tier: *_hal (see README.md section 10). The PHY is reached
  *          through HAL_ETH_ReadPHYRegister() / HAL_ETH_WritePHYRegister(),
  *          which drive the MAC station-management interface on PA2 (MDIO) and
  *          PC1 (MDC).
  *
  *          On the NUCLEO-H755ZI-Q the LAN8742A answers at SMI address 0 and
  *          generates the 50 MHz RMII reference clock itself (into PA1).
  ******************************************************************************
  */

#ifndef LAN8742_PHY_H
#define LAN8742_PHY_H

#include "stm32h7xx_hal.h"

#if defined(HAL_ETH_MODULE_ENABLED)

#ifdef __cplusplus
extern "C" {
#endif

/* ---- IEEE 802.3 clause 22 register map --------------------------------- */
#define LAN8742_BCR                          0x00U  /* Basic control        */
#define LAN8742_BSR                          0x01U  /* Basic status         */
#define LAN8742_PHYI1R                       0x02U  /* PHY identifier 1     */
#define LAN8742_PHYI2R                       0x03U  /* PHY identifier 2     */
#define LAN8742_ANAR                         0x04U  /* Auto-neg advertise   */
#define LAN8742_PHYSCSR                      0x1FU  /* Special ctrl/status  */

/* Basic control register (0x00) */
#define LAN8742_BCR_SOFT_RESET           0x8000U
#define LAN8742_BCR_LOOPBACK             0x4000U
#define LAN8742_BCR_SPEED_SELECT         0x2000U
#define LAN8742_BCR_AUTONEGO_EN          0x1000U
#define LAN8742_BCR_POWER_DOWN           0x0800U
#define LAN8742_BCR_ISOLATE              0x0400U
#define LAN8742_BCR_RESTART_AUTONEGO     0x0200U
#define LAN8742_BCR_DUPLEX_MODE          0x0100U

/* Basic status register (0x01). LINK_STATUS latches low: it must be read
 * twice to report the present state of the link. */
#define LAN8742_BSR_100BASE_TX_FD        0x4000U
#define LAN8742_BSR_100BASE_TX_HD        0x2000U
#define LAN8742_BSR_10BASE_T_FD          0x1000U
#define LAN8742_BSR_10BASE_T_HD          0x0800U
#define LAN8742_BSR_AUTONEGO_CPLT        0x0020U
#define LAN8742_BSR_REMOTE_FAULT         0x0010U
#define LAN8742_BSR_AUTONEGO_ABILITY     0x0008U
#define LAN8742_BSR_LINK_STATUS          0x0004U
#define LAN8742_BSR_JABBER_DETECT        0x0002U

/* PHY special control/status register (0x1F) */
#define LAN8742_PHYSCSR_AUTONEGO_DONE    0x1000U
#define LAN8742_PHYSCSR_HCDSPEED_MASK    0x001CU
#define LAN8742_PHYSCSR_10BT_HD          0x0004U
#define LAN8742_PHYSCSR_100BTX_HD        0x0008U
#define LAN8742_PHYSCSR_10BT_FD          0x0014U
#define LAN8742_PHYSCSR_100BTX_FD        0x0018U

/* Expected identifier of a LAN8742A. PHYI2R model/revision nibbles are masked
 * out so that any silicon revision of the part is accepted. */
#define LAN8742_ID1                      0x0007U
#define LAN8742_ID2_MODEL_MASK           0xFFF0U
#define LAN8742_ID2_MODEL                0xC130U

/** Result of a link interrogation. */
typedef enum
{
    LAN8742_LINK_DOWN = 0,          /*!< No link partner detected           */
    LAN8742_LINK_AUTONEG_ONGOING,   /*!< Link up, auto-negotiation running  */
    LAN8742_LINK_10M_HALF,
    LAN8742_LINK_10M_FULL,
    LAN8742_LINK_100M_HALF,
    LAN8742_LINK_100M_FULL,
    LAN8742_LINK_ERROR              /*!< MDIO transaction failed            */
} lan8742_link_state_t;

/** Driver instance. One per transceiver. */
typedef struct
{
    ETH_HandleTypeDef *heth;        /*!< MAC that owns the MDIO bus         */
    uint32_t           address;     /*!< SMI address, 0 on this board       */
    uint32_t           id1;         /*!< Identifier read back at init       */
    uint32_t           id2;
    uint8_t            initialized;
} lan8742_t;

typedef enum
{
    LAN8742_OK          =  0,
    LAN8742_ERROR_PARAM = -1,
    LAN8742_ERROR_MDIO  = -2,   /*!< No answer on the management interface  */
    LAN8742_ERROR_ID    = -3,   /*!< Answering device is not a LAN8742      */
    LAN8742_ERROR_RESET = -4    /*!< Soft reset did not complete            */
} lan8742_status_t;

/**
  * @brief  Resets the transceiver, checks its identifier and starts
  *         auto-negotiation. HAL_ETH_Init() must already have run: the MDIO
  *         clock divider is derived from HCLK by the MAC.
  */
lan8742_status_t LAN8742_Init(lan8742_t *obj, ETH_HandleTypeDef *heth, uint32_t address);

/**
  * @brief  Reads the Basic Status Register and, when the link is up and
  *         auto-negotiation has finished, decodes the negotiated speed and
  *         duplex from the PHY special control/status register.
  */
lan8742_link_state_t LAN8742_GetLinkState(lan8742_t *obj);

/** @brief  Restarts auto-negotiation (for example after a cable swap). */
lan8742_status_t LAN8742_RestartAutoNegotiation(lan8742_t *obj);

/** @brief  Raw register access, exported so the bring-up guide can dump the
  *         PHY from a debugger expression or a diagnostics command. */
lan8742_status_t LAN8742_ReadRegister(lan8742_t *obj, uint32_t reg, uint32_t *value);
lan8742_status_t LAN8742_WriteRegister(lan8742_t *obj, uint32_t reg, uint32_t value);

/** @brief  Human readable form of a link state, for logs. */
const char *LAN8742_LinkStateToString(lan8742_link_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* HAL_ETH_MODULE_ENABLED */

#endif /* LAN8742_PHY_H */
