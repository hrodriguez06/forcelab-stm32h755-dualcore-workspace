/**
  ******************************************************************************
  * @file    board_def.h
  * @brief   NUCLEO-H755ZI-Q board constants shared by CPU1 (M7) and CPU2 (M4).
  *
  *          Hardware reference:
  *            00_docs_stm32h7/docs/02_board_hardware/MB1363_nucleo_h755zi_q_schematics.pdf
  *            00_docs_stm32h7/docs/02_board_hardware/UM2408_nucleo144_board_manual.pdf
  ******************************************************************************
  */

#ifndef BOARD_DEF_H
#define BOARD_DEF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Clock tree -------------------------------------------------------- *
 * The NUCLEO-H755ZI-Q is shipped with SB placement that routes the 8 MHz
 * ST-LINK MCO to PH0 (HSE bypass). There is no on-board crystal for the MCU.
 *
 * ES0445 2.3.5: VOS0 / 480 MHz is not available on silicon revisions Y and W.
 * This project targets 400 MHz with VOS1, which is valid on every revision.
 * See SystemClock_Config() in CM7/Core/Src/main.c.
 */
#define BOARD_HSE_VALUE_HZ              8000000UL     /* ST-LINK MCO, bypass  */
#define BOARD_SYSCLK_HZ               400000000UL     /* pll1_p_ck            */
#define BOARD_CPU1_HCLK_HZ            200000000UL     /* AXI / AHB, HPRE = /2 */
#define BOARD_APB_HZ                  100000000UL     /* APB1..APB4, /2       */

/* ---- Ethernet (see docs/peripherals/ethernet/README.md) ---------------- *
 * PHY : Microchip LAN8742A, SMI (MDIO) address 0.
 * MAC : RMII only on this package (ES0445 2.3.11).
 * REF_CLK: 50 MHz produced by the PHY and fed into PA1. The MCU does not
 *          generate the RMII reference clock on this board.
 */
#define BOARD_ETH_PHY_ADDRESS                    0UL

/* Locally administered unicast MAC address (bit 1 of byte 0 set, bit 0 clear).
 * Replace with the address of your production OUI before shipping. */
#define BOARD_ETH_MAC_ADDR0                    0x02U
#define BOARD_ETH_MAC_ADDR1                    0x00U
#define BOARD_ETH_MAC_ADDR2                    0x00U
#define BOARD_ETH_MAC_ADDR3                    0x00U
#define BOARD_ETH_MAC_ADDR4                    0x00U
#define BOARD_ETH_MAC_ADDR5                    0x01U

/* ---- User LEDs (status reporting for the bring-up) --------------------- */
#define BOARD_LED_GREEN_PORT                  GPIOB   /* LD1 - PB0  */
#define BOARD_LED_GREEN_PIN               GPIO_PIN_0
#define BOARD_LED_YELLOW_PORT                 GPIOE   /* LD2 - PE1  */
#define BOARD_LED_YELLOW_PIN              GPIO_PIN_1
#define BOARD_LED_RED_PORT                    GPIOB   /* LD3 - PB14 */
#define BOARD_LED_RED_PIN                GPIO_PIN_14

#ifdef __cplusplus
}
#endif

#endif /* BOARD_DEF_H */
