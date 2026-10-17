/**
  ******************************************************************************
  * @file    uart_selftest.h
  * @brief   04_uart selftest (preset selftest-uart): entry points, the SRAM4
  *          block the two cores coordinate through, the bench step table and
  *          the line rules both cores' bench_claim() apply.
  *
  *          Built only with UART_DRIVER_SELFTEST.
  *          Wiring and safety rules:
  *            - JU1: PD5 (USART2_TX) -> 1 kOhm -> PD6 (USART2_RX)
  *            - JU2: PD4 (USART2_RTS) -> PD3 (USART2_CTS)
  *            - DIO15: AD2 DIO15 -> PD6 (CN9-4)
  *            - DIO14: PA8 (MCO1) clock reference
  *            - PE2: HOST_REQ (DIO12)
  *            - PE7: MCU_ACK (DIO13)
  ******************************************************************************
  */

#ifndef UART_SELFTEST_H
#define UART_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"
#include "uart_bm.h"
#include "uart_ll.h"
#include "uart_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UART_BENCH_VERSION       1U

/* ---- Entry points ---------------------------------------------------------- */
void UART_Selftest_CM7_PreRelease(void);   /* CPU1, before HSEM_BM_BootReleaseCpu2 */
void UART_Selftest_CM7_Run(void);          /* CPU1, from Status_Task               */
void UART_Selftest_CM4_Run(void);          /* CPU2, from CM4 main()                */

/* ---- Bench pin definitions (CN9 connector, USART2) ------------------------- */
#define UART_BENCH_PORT_USART    GPIOD
#define UART_PIN_CTS             3U   /* PD3: USART2_CTS (AF7) */
#define UART_PIN_RTS             4U   /* PD4: USART2_RTS (AF7) */
#define UART_PIN_TX              5U   /* PD5: USART2_TX  (AF7) */
#define UART_PIN_RX              6U   /* PD6: USART2_RX  (AF7) */

#define UART_PINS_USART_MASK     ((1UL << UART_PIN_CTS) | (1UL << UART_PIN_RTS) | \
                                  (1UL << UART_PIN_TX)  | (1UL << UART_PIN_RX))

#define UART_BENCH_PORT_SYNC     GPIOE
#define UART_PIN_REQ             2U   /* PE2: HOST_REQ (DIO12, input)  */
#define UART_PIN_ACK             7U   /* PE7: MCU_ACK  (DIO13, output) */

#define UART_BENCH_PORT_MCO      GPIOA
#define UART_PIN_MCO             8U   /* PA8: MCO1 (DIO14, AF0)        */

/* Console pins on Port D (board_def.h default ST-LINK VCP) */
#define UART_CONSOLE_TX_PIN_IDX  8U   /* PD8: USART3_TX (AF7) */
#define UART_CONSOLE_RX_PIN_IDX  9U   /* PD9: USART3_RX (AF7) */

/* ---- Step table ------------------------------------------------------------ */
typedef enum
{
    UST_OWN_NONE = 0,
    UST_OWN_CM7,
    UST_OWN_CM4
} UstOwner_t;

typedef enum
{
    UST_DIO15_NONE = 0,
    UST_DIO15_OE   = 1, /* Static OE output (UT0 resistor check) */
    UST_DIO15_DOUT = 2  /* Digital Out pattern generator output (UT5 error injection) */
} UstDio15Mode_t;

typedef struct
{
    const char *name;
    uint8_t     owner;          /* UstOwner_t: core that may claim USART2 pins in AF */
    uint16_t    allow_pd;       /* bitmask of PD pins allowed (e.g. 1<<PD5 for UT0_CTRL) */
    uint8_t     dio15_mode;     /* UstDio15Mode_t: AD2 DIO15 permission for SU1 */
} UstStep_t;

extern const UstStep_t k_ust_steps[];
extern const uint32_t  k_ust_step_count;

#define UST_STEP_NONE        0xFFFFFFFFUL

uint32_t ust_step_index(const char *name);

/* Pure line rule checking if a pin may be configured */
bool ust_line_rule(uint32_t core, uint32_t step, const GPIO_TypeDef *port, uint32_t pin,
                   uint32_t mode, uint32_t af, uint32_t otype, uint32_t pull);

/* ---- SRAM4 coordination block ---------------------------------------------- */
/* Linker citation (C-m):
 * 03_firmware_templates/prj_01_template_dualcore/CM7/linker/stm32h755zi_cm7.ld:43 and
 * CM4/linker/stm32h755zi_cm4.ld:34 define RAM_SHARED: ORIGIN = 0x38008000, LENGTH = 32K
 * (SRAM4 in D3 domain, non-cacheable shared memory accessible by both Cortex-M7 and M4).
 * Offset 0x3800E000 (+24 KB) provides a dedicated window for selftest inter-core
 * coordination without colliding with primary IPC at 0x38008000. */
#define UST_AREA_ADDR        0x3800E000UL
#define UST_AREA_MAGIC       0x55535431UL   /* "UST1" */

typedef enum
{
    UST_CMD_NONE = 0,
    UST_CMD_UT4,              /* CM4 executes UT4 LL IRQ loopback */
    UST_CMD_LOCK_ACQUIRE,     /* CM4 acquires cross-core GPIO lock (HSEM 2) */
    UST_CMD_LOCK_RELEASE,     /* CM4 releases cross-core GPIO lock */
    UST_CMD_DONE              /* Selftest finished */
} UstCmd_t;

typedef struct
{
    volatile uint32_t magic;
    volatile uint32_t session;
    volatile char     m4_rev[24];
    volatile char     m4_build_id[24];
    volatile uint32_t m4_ready;
    volatile uint32_t m4_timeout;

    volatile uint32_t cmd;
    volatile uint32_t arg0;
    volatile uint32_t arg1;
    volatile uint32_t cmd_seq;
    volatile uint32_t ack_seq;

    volatile uint32_t m4_ut4_ok;
    volatile uint32_t lock_held;
} UstArea_t;

#define UST_AREA             ((UstArea_t *)UST_AREA_ADDR)

#ifdef __cplusplus
}
#endif

#endif /* UART_SELFTEST_H */
