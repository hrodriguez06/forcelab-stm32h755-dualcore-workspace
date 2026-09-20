/**
  ******************************************************************************
  * @file    lan8742_phy.c
  * @brief   Microchip LAN8742A transceiver driver (SMI / MDIO), HAL tier.
  ******************************************************************************
  */

#include "lan8742_phy.h"

/* A clause-22 register access over MDIO takes a few microseconds. The reset
 * bit of the LAN8742A self-clears well inside 1 ms; 500 ms is a generous
 * bound that still fails fast when the transceiver is absent or held in
 * reset by the board. */
#define LAN8742_RESET_TIMEOUT_MS         500U

lan8742_status_t LAN8742_ReadRegister(lan8742_t *obj, uint32_t reg, uint32_t *value)
{
    if ((obj == NULL) || (obj->heth == NULL) || (value == NULL))
    {
        return LAN8742_ERROR_PARAM;
    }

    if (HAL_ETH_ReadPHYRegister(obj->heth, obj->address, reg, value) != HAL_OK)
    {
        return LAN8742_ERROR_MDIO;
    }

    return LAN8742_OK;
}

lan8742_status_t LAN8742_WriteRegister(lan8742_t *obj, uint32_t reg, uint32_t value)
{
    if ((obj == NULL) || (obj->heth == NULL))
    {
        return LAN8742_ERROR_PARAM;
    }

    if (HAL_ETH_WritePHYRegister(obj->heth, obj->address, reg, value) != HAL_OK)
    {
        return LAN8742_ERROR_MDIO;
    }

    return LAN8742_OK;
}

lan8742_status_t LAN8742_Init(lan8742_t *obj, ETH_HandleTypeDef *heth, uint32_t address)
{
    uint32_t reg = 0U;
    uint32_t tickstart;

    if ((obj == NULL) || (heth == NULL))
    {
        return LAN8742_ERROR_PARAM;
    }

    obj->heth        = heth;
    obj->address     = address;
    obj->initialized = 0U;

    /* An absent or mis-strapped transceiver reads back 0xFFFF on every
       register, so the identifier check is also the MDIO bus check. */
    if (LAN8742_ReadRegister(obj, LAN8742_PHYI1R, &obj->id1) != LAN8742_OK)
    {
        return LAN8742_ERROR_MDIO;
    }
    if (LAN8742_ReadRegister(obj, LAN8742_PHYI2R, &obj->id2) != LAN8742_OK)
    {
        return LAN8742_ERROR_MDIO;
    }

    if ((obj->id1 != LAN8742_ID1) ||
        ((obj->id2 & LAN8742_ID2_MODEL_MASK) != LAN8742_ID2_MODEL))
    {
        return LAN8742_ERROR_ID;
    }

    /* Software reset. The bit is self-clearing. */
    if (LAN8742_WriteRegister(obj, LAN8742_BCR, LAN8742_BCR_SOFT_RESET) != LAN8742_OK)
    {
        return LAN8742_ERROR_MDIO;
    }

    tickstart = HAL_GetTick();
    do
    {
        if (LAN8742_ReadRegister(obj, LAN8742_BCR, &reg) != LAN8742_OK)
        {
            return LAN8742_ERROR_MDIO;
        }

        if ((HAL_GetTick() - tickstart) > LAN8742_RESET_TIMEOUT_MS)
        {
            return LAN8742_ERROR_RESET;
        }
    } while ((reg & LAN8742_BCR_SOFT_RESET) != 0U);

    /* Advertise every 10/100 mode and start negotiating. The MAC is
       reconfigured from the outcome by the link supervision thread, so no
       forced speed or duplex is programmed here. */
    if (LAN8742_WriteRegister(obj, LAN8742_BCR,
                              LAN8742_BCR_AUTONEGO_EN |
                              LAN8742_BCR_RESTART_AUTONEGO) != LAN8742_OK)
    {
        return LAN8742_ERROR_MDIO;
    }

    obj->initialized = 1U;
    return LAN8742_OK;
}

lan8742_status_t LAN8742_RestartAutoNegotiation(lan8742_t *obj)
{
    uint32_t reg = 0U;
    lan8742_status_t status;

    status = LAN8742_ReadRegister(obj, LAN8742_BCR, &reg);
    if (status != LAN8742_OK)
    {
        return status;
    }

    reg |= (LAN8742_BCR_AUTONEGO_EN | LAN8742_BCR_RESTART_AUTONEGO);

    return LAN8742_WriteRegister(obj, LAN8742_BCR, reg);
}

lan8742_link_state_t LAN8742_GetLinkState(lan8742_t *obj)
{
    uint32_t bsr    = 0U;
    uint32_t physcsr = 0U;

    /* BSR bit 2 (link status) latches low: a link that dropped and came back
       between two polls still reports "down" on the first read. Read the
       register twice and keep the second value, which is the present state. */
    if (LAN8742_ReadRegister(obj, LAN8742_BSR, &bsr) != LAN8742_OK)
    {
        return LAN8742_LINK_ERROR;
    }
    if (LAN8742_ReadRegister(obj, LAN8742_BSR, &bsr) != LAN8742_OK)
    {
        return LAN8742_LINK_ERROR;
    }

    if ((bsr & LAN8742_BSR_LINK_STATUS) == 0U)
    {
        return LAN8742_LINK_DOWN;
    }

    if ((bsr & LAN8742_BSR_AUTONEGO_CPLT) == 0U)
    {
        return LAN8742_LINK_AUTONEG_ONGOING;
    }

    if (LAN8742_ReadRegister(obj, LAN8742_PHYSCSR, &physcsr) != LAN8742_OK)
    {
        return LAN8742_LINK_ERROR;
    }

    switch (physcsr & LAN8742_PHYSCSR_HCDSPEED_MASK)
    {
        case LAN8742_PHYSCSR_100BTX_FD: return LAN8742_LINK_100M_FULL;
        case LAN8742_PHYSCSR_100BTX_HD: return LAN8742_LINK_100M_HALF;
        case LAN8742_PHYSCSR_10BT_FD:   return LAN8742_LINK_10M_FULL;
        case LAN8742_PHYSCSR_10BT_HD:   return LAN8742_LINK_10M_HALF;
        default:                        return LAN8742_LINK_AUTONEG_ONGOING;
    }
}

const char *LAN8742_LinkStateToString(lan8742_link_state_t state)
{
    switch (state)
    {
        case LAN8742_LINK_DOWN:             return "down";
        case LAN8742_LINK_AUTONEG_ONGOING:  return "auto-negotiating";
        case LAN8742_LINK_10M_HALF:         return "10 Mbps half duplex";
        case LAN8742_LINK_10M_FULL:         return "10 Mbps full duplex";
        case LAN8742_LINK_100M_HALF:        return "100 Mbps half duplex";
        case LAN8742_LINK_100M_FULL:        return "100 Mbps full duplex";
        case LAN8742_LINK_ERROR:            return "MDIO error";
        default:                            return "unknown";
    }
}
