/**
  ******************************************************************************
  * @file    board_def.h
  * @brief   NUCLEO-H755ZI-Q board pinout and hardware constants (MB1363-E01)
  *          for prj_06_adc_fft_eth (Dual-Core 4096-pt FFT + 16-Band db4 WPT).
  *
  *          Hardware reference:
  *            00_docs_stm32h7/docs/02_board_hardware/MB1363_nucleo_h755zi_q_schematics.pdf
  *            00_docs_stm32h7/docs/02_board_hardware/UM2408_nucleo144_board_manual.pdf
  ******************************************************************************
  */

#ifndef BOARD_DEF_H
#define BOARD_DEF_H

#include "stm32h7xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Clock tree (configured by CM7 in SystemClock_Config) --------------- */
#define BOARD_HSE_VALUE_HZ              8000000UL     /* ST-LINK MCO, bypass  */
#define BOARD_SYSCLK_HZ               400000000UL     /* pll1_p_ck, VOS1      */
#define BOARD_CPU1_HCLK_HZ            200000000UL     /* AXI / AHB, HPRE = /2 */
#define BOARD_APB_HZ                  100000000UL     /* APB1..APB4, /2       */
#define BOARD_TIM_APB1_CLK_HZ         200000000UL     /* APB1 timer kernel    */
#define BOARD_ADC_KER_CLK_HZ           25000000UL     /* PLL2P = 25 MHz       */

/* ---- Ethernet (Microchip LAN8742A, RMII, SMI addr 0) -------------------- */
#define BOARD_ETH_PHY_ADDRESS                    0UL

/* Locally administered unicast MAC address (02:00:00:00:00:01) */
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

/* ---- User LEDs (consumed by 01_led/led_bm.c) ---------------------------- */
#define BOARD_LED_GREEN_PORT                  GPIOB   /* LD1 - PB0  */
#define BOARD_LED_GREEN_BIT                      0U
#define BOARD_LED_YELLOW_PORT                 GPIOE   /* LD2 - PE1  */
#define BOARD_LED_YELLOW_BIT                     1U
#define BOARD_LED_RED_PORT                    GPIOB   /* LD3 - PB14 */
#define BOARD_LED_RED_BIT                       14U

/* ---- Console UART (Nucleo-144 ST-LINK VCP: USART3 PD8/PD9 AF7) ---------- */
#define BOARD_CONSOLE_UART_INSTANCE           USART3
#define BOARD_CONSOLE_TX_PORT                 GPIOD   /* PD8  - USART3_TX */
#define BOARD_CONSOLE_TX_PIN                  GPIO_PIN_8
#define BOARD_CONSOLE_TX_AF                   GPIO_AF7_USART3
#define BOARD_CONSOLE_RX_PORT                 GPIOD   /* PD9  - USART3_RX */
#define BOARD_CONSOLE_RX_PIN                  GPIO_PIN_9
#define BOARD_CONSOLE_RX_AF                   GPIO_AF7_USART3

/* ---- ADC1 + DMA1 + TIM3 (owned by Cortex-M4 in Bare-Metal mode) --------- *
 * Select input pin at compile time via BOARD_ADC_USE_FAST_CHANNEL_PB1:
 *   1 (default): PB1 -> ADC1_INP5 (Fast channel, Arduino A3 on CN9 pin 7)
 *   0          : PA3 -> ADC1_INP15 (Slow channel, Arduino A0 on CN9 pin 1)
 * ------------------------------------------------------------------------- */
#ifndef BOARD_ADC_USE_FAST_CHANNEL_PB1
#define BOARD_ADC_USE_FAST_CHANNEL_PB1        1U
#endif

#if (BOARD_ADC_USE_FAST_CHANNEL_PB1 != 0U)
#define BOARD_ADC_PORT                        GPIOB
#define BOARD_ADC_PIN                         GPIO_PIN_1
#define BOARD_ADC_CHANNEL                     5U
#define BOARD_ADC_PIN_NAME                    "PB1 [A3, ADC1_INP5 Fast]"
#else
#define BOARD_ADC_PORT                        GPIOA
#define BOARD_ADC_PIN                         GPIO_PIN_3
#define BOARD_ADC_CHANNEL                     15U
#define BOARD_ADC_PIN_NAME                    "PA3 [A0, ADC1_INP15]"
#endif

#define BOARD_ADC_SAMPLING_RATE_HZ            25600UL     /* Default 25.6 kSPS */
#define BOARD_ADC_MIN_FS_HZ                    1000UL     /* Minimum 1 kSPS    */
#define BOARD_ADC_MAX_FS_HZ                  100000UL     /* Maximum 100 kSPS  */
#define BOARD_ADC_VREF_MV                      3300UL     /* 3.3 V reference   */

/* ---- DMA Stream Allocation ---------------------------------------------- */
#define BOARD_ADC_DMA_STREAM                  DMA1_Stream0
#define BOARD_ADC_DMA_REQUEST                 9U          /* DMAMUX1: adc1_dma */

/* ---- DSP Block & Spectral Analysis Parameters --------------------------- *
 * Block size:    N = 4096 samples (power of 2 for CMSIS-DSP RFFT and WPT)
 * FFT bins:      N/2 = 2048 bins (0 .. fs/2; 6.25 Hz/bin at 25.6 kHz)
 * Block period:  N / fs = 160 ms (6.25 blocks/s at 25.6 kHz)
 * WPT levels:    J = 4 levels of Daubechies-4 (db4) -> 2^4 = 16 sub-bands
 *                (800 Hz/band, 256 coefficients/band at 25.6 kHz)
 * ------------------------------------------------------------------------- */
#define DSP_BLOCK_SIZE                        4096U
#define DSP_NUM_BINS                          (DSP_BLOCK_SIZE / 2U)
#define DSP_MAX_HARMONICS                     10U

#define WPT_LEVELS                            4U
#define WPT_NUM_BANDS                         (1U << WPT_LEVELS)            /* 16  */
#define WPT_COEFFS_PER_BAND                   (DSP_BLOCK_SIZE >> WPT_LEVELS) /* 256 */

/* ---- Hardware Semaphore Clear Key (RM0399 §11.3.6) ---------------------- */
#define BOARD_HSEM_CLEAR_KEY                  0xA5C3U

#ifdef __cplusplus
}
#endif

#endif /* BOARD_DEF_H */
