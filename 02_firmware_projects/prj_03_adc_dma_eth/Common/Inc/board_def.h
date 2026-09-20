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

/* ---- Ethernet RMII Pins (Nucleo-144) ----------------------------------- */
#define BOARD_ETH_REF_CLK_PORT                GPIOA   /* PA1  - REF_CLK */
#define BOARD_ETH_REF_CLK_PIN             GPIO_PIN_1
#define BOARD_ETH_MDIO_PORT                   GPIOA   /* PA2  - MDIO    */
#define BOARD_ETH_MDIO_PIN                GPIO_PIN_2
#define BOARD_ETH_CRS_DV_PORT                 GPIOA   /* PA7  - CRS_DV  */
#define BOARD_ETH_CRS_DV_PIN              GPIO_PIN_7
#define BOARD_ETH_TXD1_PORT                   GPIOB   /* PB13 - TXD1    */
#define BOARD_ETH_TXD1_PIN               GPIO_PIN_13
#define BOARD_ETH_MDC_PORT                    GPIOC   /* PC1  - MDC     */
#define BOARD_ETH_MDC_PIN                 GPIO_PIN_1
#define BOARD_ETH_RXD0_PORT                   GPIOC   /* PC4  - RXD0    */
#define BOARD_ETH_RXD0_PIN                GPIO_PIN_4
#define BOARD_ETH_RXD1_PORT                   GPIOC   /* PC5  - RXD1    */
#define BOARD_ETH_RXD1_PIN                GPIO_PIN_5
#define BOARD_ETH_TX_EN_PORT                  GPIOG   /* PG11 - TX_EN   */
#define BOARD_ETH_TX_EN_PIN              GPIO_PIN_11
#define BOARD_ETH_TXD0_PORT                   GPIOG   /* PG13 - TXD0    */
#define BOARD_ETH_TXD0_PIN               GPIO_PIN_13

/* ---- User LEDs (status reporting for the bring-up) --------------------- */
#define BOARD_LED_GREEN_PORT                  GPIOB   /* LD1 - PB0  */
#define BOARD_LED_GREEN_PIN               GPIO_PIN_0
#define BOARD_LED_YELLOW_PORT                 GPIOE   /* LD2 - PE1  */
#define BOARD_LED_YELLOW_PIN              GPIO_PIN_1
#define BOARD_LED_RED_PORT                    GPIOB   /* LD3 - PB14 */
#define BOARD_LED_RED_PIN                GPIO_PIN_14

/* ---- Hardware Ownership Contract --------------------------------------- *
 * CPU1 (Cortex-M7):
 *   - Clocks (RCC, PLL1 @ 400 MHz, PLL2 @ 200 MHz / adc_ker_ck @ 25 MHz)
 *   - Ethernet MAC & LAN8742A PHY (RMII)
 *   - VCP UART (USART3)
 *   - HAL Timebase: TIM6
 * CPU2 (Cortex-M4):
 *   - ADC1 (16-bit, BOOST=10) & DMA1 Stream 0
 *   - ADC Sampling Timer: TIM3 (TRGO @ 500,000 Hz)
 *   - SRAM2 (0x30020000) DMA circular buffer (1024 samples)
 */

/* ---- Analog Input Selection --------------------------------------------- *
 * Option 1 (Default - Recommended for high-frequency square waves):
 *   PB1: ADC12_INP5 (Fast Channel) on Arduino A3 (CN9 pin 4).
 *   Low internal switch resistance (~200 Ohm) prevents rise/fall edge rounding.
 *
 * Option 2 (Legacy - Slow Channel):
 *   PA3: ADC12_INP15 (Slow Channel) on Arduino A0 (CN9 pin 1).
 */
#define BOARD_ADC_USE_FAST_CHANNEL_PB1        1U

#if (BOARD_ADC_USE_FAST_CHANNEL_PB1 == 1U)
  #define BOARD_ADC_PORT                      GPIOB   /* PB1 - ADC12_INP5 (Fast Channel) */
  #define BOARD_ADC_PIN                       GPIO_PIN_1
  #define BOARD_ADC_CHANNEL                   ADC_CHANNEL_5
  #define BOARD_ADC_PIN_NAME                  "PB1 [A3]"
#else
  #define BOARD_ADC_PORT                      GPIOA   /* PA3 - ADC12_INP15 (Slow Channel) */
  #define BOARD_ADC_PIN                       GPIO_PIN_3
  #define BOARD_ADC_CHANNEL                   ADC_CHANNEL_15
  #define BOARD_ADC_PIN_NAME                  "PA3 [A0]"
#endif

#define BOARD_ADC_SAMPLING_RATE_HZ            500000UL /* 500 kSPS (50 samples/cycle @ 10 kHz) */
#define BOARD_ADC_VREF_MV                     3300UL

#ifdef __cplusplus
}
#endif

#endif /* BOARD_DEF_H */
