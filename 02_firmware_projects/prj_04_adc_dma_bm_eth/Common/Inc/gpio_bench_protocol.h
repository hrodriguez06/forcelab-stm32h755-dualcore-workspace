/**
  ******************************************************************************
  * @file    gpio_bench_protocol.h
  * @brief   GPIO Test Bench protocol definitions, step IDs, and safety constants
  *          shared between STM32H755 firmware (CM7/CM4) and host test runner.
  *
  *          Rules (IMPLEMENTATION_PLAN.md §Banco de pruebas & §Seguridad eléctrica):
  *            - PE8..PE15: 8-bit bus (DIO0..DIO7 on AD2)
  *            - PE2 (HOST_REQ), PE3 (EXTI3), PE4 (EXTI4): Solo AD2
  *            - PE5 (CM7 ISR), PE6 (CM4 ISR), PE7 (MCU_ACK), PA8 (MCO1): Solo MCU
  *            - PE0 (SWIER EXTI0), PE1 (LD2 Yellow LED): NEVER TOUCH
  *            - PF14 (Scope 1+), PF15 (Scope 2+): Analog probes
  *            - PF6..PF11: Unwired pins (PF10 CM7, PF11 CM4 for T15 race)
  ******************************************************************************
  */

#ifndef GPIO_BENCH_PROTOCOL_H
#define GPIO_BENCH_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "shared_memory.h"
#include "hsem_selftest_shared.h"

#define GPIO_BENCH_PROTOCOL_VERSION     2U

/* ---- Bus Ownership (S1, S5) ---------------------------------------------- */
#define GPIO_BENCH_BUS_NONE             0U  /*!< Bus is loose: both sides in input */
#define GPIO_BENCH_BUS_MCU              1U  /*!< MCU drives PE8..PE15, AD2 in input */
#define GPIO_BENCH_BUS_AD2              2U  /*!< AD2 drives DIO0..DIO7, MCU in input */

/* ---- Protocol Test Steps (S7) -------------------------------------------- */
#define GPIO_BENCH_STEP_T15_RACE        1U  /*!< T15: dual-core race PF10/PF11 (unwired, bus=NONE) */
#define GPIO_BENCH_STEP_IDLE            2U  /*!< Ready for host session (bus=NONE) */
#define GPIO_BENCH_STEP_T0B_GUARDS      3U  /*!< T0b(b): guards verification (bus=NONE) */
#define GPIO_BENCH_STEP_T0B_S5_NEG      4U  /*!< T0b(c): S5 negative control (bus=MCU) */
#define GPIO_BENCH_STEP_T0B_TIMEOUT     5U  /*!< T0b(d): timeout test (bus=NONE) */
#define GPIO_BENCH_STEP_T0A_LOOSE       6U  /*!< T0a: loose line check 13 lines (bus=NONE) */
#define GPIO_BENCH_STEP_T0A_HANDSHAKE   7U  /*!< T0a: PE2/PE7 handshake lines test (bus=NONE) */
#define GPIO_BENCH_STEP_T0A_MCU_TO_AD2  8U  /*!< T0a: walking-1 MCU -> AD2 11 lines (bus=MCU) */
#define GPIO_BENCH_STEP_T0A_AD2_TO_MCU  9U  /*!< T0a: walking-1 AD2 -> MCU 10 lines (bus=AD2) */
#define GPIO_BENCH_STEP_T0A_SCOPE       10U /*!< T0a: Scope 1/2 DC levels (bus=NONE) */
#define GPIO_BENCH_STEP_T1_REJECT       11U /*!< T1: refusals leave every port untouched (bus=NONE) */
#define GPIO_BENCH_STEP_T2_PARITY       12U /*!< T2: same bits as HAL_GPIO_Init / RM table, PF6..PF9 (bus=NONE) */
#define GPIO_BENCH_STEP_B1_MEASURE      13U /*!< B1: ConfigPin cycles, PRIMASK and lock hold times (bus=NONE) */
#define GPIO_BENCH_STEP_T16_LCKR        20U /*!< T16: LCKR on PF7..PF9, last test of the image (bus=NONE) */
#define GPIO_BENCH_STEP_DONE            99U /*!< All tests completed (bus=NONE) */

/* ---- Handshake timing ---------------------------------------------------- */
#define GPIO_BENCH_DEADTIME_MS          1U     /*!< S6: minimum deadtime between release and drive */
#define GPIO_BENCH_TIMEOUT_MS           10000U /*!< S7: 10-second timeout */

/* ---- AD2 Digital IO Masks (S3) ------------------------------------------- */
#define AD2_DIO_BUS_MASK                0x00FFU    /*!< DIO0..DIO7 -> PE8..PE15 */
#define AD2_DIO_EXTI3_CM7               (1U << 8)  /*!< DIO8 -> PE3 (Solo AD2) */
#define AD2_DIO_CM7_ACK                 (1U << 9)  /*!< DIO9 -> PE5 (Solo MCU) */
#define AD2_DIO_EXTI4_CM4               (1U << 10) /*!< DIO10 -> PE4 (Solo AD2) */
#define AD2_DIO_CM4_ACK                 (1U << 11) /*!< DIO11 -> PE6 (Solo MCU) */
#define AD2_DIO_HOST_REQ                (1U << 12) /*!< DIO12 -> PE2 (Solo AD2) */
#define AD2_DIO_MCU_ACK                 (1U << 13) /*!< DIO13 -> PE7 (Solo MCU) */
#define AD2_DIO_MCO1_PA8                (1U << 14) /*!< DIO14 -> PA8 (Solo MCU) */

#define AD2_ALLOWED_STATIC_OUTPUTS      (AD2_DIO_EXTI3_CM7 | AD2_DIO_EXTI4_CM4 | AD2_DIO_HOST_REQ)

/* ---- Handshake Primitives (R-B1) ----------------------------------------- */
#define GPIO_BENCH_PRIM_NONE            0U
#define GPIO_BENCH_PRIM_HS_MCU          1U /* MCU asserts ACK -> Host asserts REQ -> MCU drops ACK -> Host drops REQ */
#define GPIO_BENCH_PRIM_HS_HOST         2U /* Host asserts REQ -> MCU asserts ACK -> Host drops REQ -> MCU drops ACK */

/* ---- 13-Line Loose Check Mask (B2) --------------------------------------- */
#define AD2_DIO_LOOSE_13_MASK           (AD2_DIO_BUS_MASK | AD2_DIO_EXTI3_CM7 | AD2_DIO_CM7_ACK | \
                                         AD2_DIO_EXTI4_CM4 | AD2_DIO_CM4_ACK | AD2_DIO_MCO1_PA8)

/* ========================================================================== */
/* ---- Benchmark Protocol Step & Sequence Table (R2-M3) --------------------- */
/* ========================================================================== */

/* Item Types */
#define BENCH_ITEM_LINE       1U  /*!< Firmware protocol log line */
#define BENCH_ITEM_PRIM       2U  /*!< Handshake primitive: HS_MCU or HS_HOST */
#define BENCH_ITEM_MEASURE    3U  /*!< Measurement, stimulus, or verification point */

/* Execution order: the host and the firmware walk the steps in this order. */
#define GPIO_BENCH_STEPS_TABLE(X) \
    X(1,  "T15_RACE",       GPIO_BENCH_BUS_NONE) \
    X(11, "T1_REJECT",      GPIO_BENCH_BUS_NONE) \
    X(12, "T2_PARITY",      GPIO_BENCH_BUS_NONE) \
    X(13, "B1_MEASURE",     GPIO_BENCH_BUS_NONE) \
    X(2,  "IDLE",           GPIO_BENCH_BUS_NONE) \
    X(3,  "T0B_GUARDS",     GPIO_BENCH_BUS_NONE) \
    X(4,  "T0B_S5_NEG",     GPIO_BENCH_BUS_MCU)  \
    X(5,  "T0B_TIMEOUT",    GPIO_BENCH_BUS_NONE) \
    X(6,  "T0A_LOOSE",      GPIO_BENCH_BUS_NONE) \
    X(7,  "T0A_HANDSHAKE",  GPIO_BENCH_BUS_NONE) \
    X(8,  "T0A_MCU_TO_AD2", GPIO_BENCH_BUS_MCU)  \
    X(9,  "T0A_AD2_TO_MCU", GPIO_BENCH_BUS_AD2)  \
    X(10, "T0A_SCOPE",      GPIO_BENCH_BUS_NONE) \
    X(20, "T16_LCKR",       GPIO_BENCH_BUS_NONE) \
    X(99, "DONE",           GPIO_BENCH_BUS_NONE)

typedef struct {
    uint8_t step_id;
    uint8_t item_type;      /* BENCH_ITEM_LINE, BENCH_ITEM_PRIM, BENCH_ITEM_MEASURE */
    uint8_t primitive;      /* GPIO_BENCH_PRIM_NONE, HS_MCU, HS_HOST */
    const char *name;       /* Unique item name or measure point */
    const char *line_text;  /* Protocol line text / pattern */
    uint8_t bit_arg;        /* DIO line / bit argument, or 0xFF */
} BenchProtocolItem_t;

#define GPIO_BENCH_PROTOCOL_TABLE(X) \
    /* Step 1: T15 Race Control (bus=NONE) */ \
    X(1, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step1_announcement", "^\\[m7_gpio_test\\] STEP id=1 bus=NONE v=2$", 0xFF) \
    X(1,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step1_lckr_boot", "^\\[m7_gpio_test\\] LCKR at boot: GPIOF=0x[0-9A-F]{8} CLEAN$", 0xFF) \
    X(1, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step1_verdict",      "^\\[m7_gpio_test\\] T15 VERDICT: PASS$", 0xFF) \
    X(1, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_CHECK t15_pass", "", 0xFF) \
    \
    /* Step 11: T1 refusals, both cores (bus=NONE) */ \
    X(11,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step11_announcement", "^\\[m7_gpio_test\\] STEP id=11 bus=NONE v=2$", 0xFF) \
    X(11,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step11_verdict", "^\\[m7_gpio_test\\] T1 VERDICT: PASS$", 0xFF) \
    X(11,BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_CHECK t1_pass", "", 0xFF) \
    \
    /* Step 12: T2 parity, both cores (bus=NONE) */ \
    X(12,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step12_announcement", "^\\[m7_gpio_test\\] STEP id=12 bus=NONE v=2$", 0xFF) \
    X(12,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step12_verdict", "^\\[m7_gpio_test\\] T2 VERDICT: PASS$", 0xFF) \
    X(12,BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_CHECK t2_pass", "", 0xFF) \
    \
    /* Step 13: B1 measurements, both cores; recorded, no pass/fail (bus=NONE) */ \
    X(13,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step13_announcement", "^\\[m7_gpio_test\\] STEP id=13 bus=NONE v=2$", 0xFF) \
    X(13,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step13_done", "^\\[m7_gpio_test\\] B1 DONE: COMPLETE$", 0xFF) \
    X(13,BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_CHECK b1_complete", "", 0xFF) \
    \
    /* Step 2: Idle & Host Session Start (bus=NONE) */ \
    X(2, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step2_announcement", "^\\[m7_gpio_test\\] STEP id=2 bus=NONE v=2$", 0xFF) \
    X(2, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step2_idle",         "^\\[m7_gpio_test\\] GPIO_BENCH v2 idle$", 0xFF) \
    X(2, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_CHECK t0b_a_oe_guards", "", 0xFF) \
    X(2, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"session_start",      "", 0xFF) \
    \
    /* Step 3: T0b(b) Firmware Claim Guards (bus=NONE) */ \
    X(3, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step3_announcement", "^\\[m7_gpio_test\\] STEP id=3 bus=NONE v=2$", 0xFF) \
    X(3, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step3_guards_pass",  "^\\[m7_gpio_test\\] T0b_b guards verification: PASS$", 0xFF) \
    X(3, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_CHECK t0b_b_claim_guards", "", 0xFF) \
    \
    /* Step 4: T0b(c) S5 Negative Control (bus=MCU) */ \
    X(4, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step4_announcement", "^\\[m7_gpio_test\\] STEP id=4 bus=MCU v=2$", 0xFF) \
    X(4, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step4_buschk_drive", "^\\[m7_gpio_test\\] BUSCHK up drive=0x5A$", 0xFF) \
    X(4, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_CHECK t0b_c_contention", "", 0xFF) \
    X(4, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "t0b_c_sync",         "", 0xFF) \
    X(4, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step4_safe_verified","^\\[m7_gpio_test\\] T0b_c safe state verified: all wired pins input$", 0xFF) \
    \
    /* Step 5: T0b(d) 10s Timeout Test (bus=NONE) */ \
    X(5, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step5_announcement", "^\\[m7_gpio_test\\] STEP id=5 bus=NONE v=2$", 0xFF) \
    X(5, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_WAIT t0b_d_timeout", "", 0xFF) \
    X(5, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step5_timeout_pass", "^\\[m7_gpio_test\\] timeout expired -> bench_safe\\(\\) invoked \\(PASS\\)$", 0xFF) \
    X(5, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_CHECK t0b_d_timeout", "", 0xFF) \
    \
    /* Step 6: T0a Loose Lines Check (bus=NONE) */ \
    X(6, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step6_announcement", "^\\[m7_gpio_test\\] STEP id=6 bus=NONE v=2$", 0xFF) \
    X(6, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step6_buschk_up",    "^\\[m7_gpio_test\\] BUSCHK up$", 0xFF) \
    X(6, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ loose_up", "", 0xFF) \
    X(6, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step6_up",           "", 0xFF) \
    X(6, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step6_buschk_down",  "^\\[m7_gpio_test\\] BUSCHK down$", 0xFF) \
    X(6, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ loose_down", "", 0xFF) \
    X(6, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step6_down",         "", 0xFF) \
    X(6, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step6_loose_done",   "^\\[m7_gpio_test\\] T0a_loose: DONE \\(host verdict\\)$", 0xFF) \
    \
    /* Step 7: T0a Handshake Lines PE2/PE7 (bus=NONE) */ \
    X(7, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step7_announcement", "^\\[m7_gpio_test\\] STEP id=7 bus=NONE v=2$", 0xFF) \
    X(7, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step7_pe2",          "", 0xFF) \
    X(7, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step7_handshake_pe2","^\\[m7_gpio_test\\] Handshake PE2: OK$", 0xFF) \
    X(7, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step7_pe7",          "", 0xFF) \
    X(7, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step7_handshake_done","^\\[m7_gpio_test\\] T0a_handshake: DONE \\(host verdict\\)$", 0xFF) \
    \
    /* Step 8: T0a Walking-1 MCU->AD2 11 lines (bus=MCU) */ \
    X(8, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step8_announcement", "^\\[m7_gpio_test\\] STEP id=8 bus=MCU v=2$", 0xFF) \
    X(8, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step8_buschk_up",    "^\\[m7_gpio_test\\] BUSCHK up$", 0xFF) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_up",           "", 0xFF) \
    X(8, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step8_buschk_down",  "^\\[m7_gpio_test\\] BUSCHK down$", 0xFF) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_down",         "", 0xFF) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_bit",  "", 9) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_mcu_9",        "", 9) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_bit",  "", 11) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_mcu_11",       "", 11) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_bit",  "", 0) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_mcu_0",        "", 0) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_bit",  "", 1) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_mcu_1",        "", 1) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_bit",  "", 2) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_mcu_2",        "", 2) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_bit",  "", 3) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_mcu_3",        "", 3) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_bit",  "", 4) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_mcu_4",        "", 4) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_bit",  "", 5) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_mcu_5",        "", 5) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_bit",  "", 6) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_mcu_6",        "", 6) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_bit",  "", 7) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_mcu_7",        "", 7) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_bit",  "", 14) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_mcu_14",       "", 14) \
    X(8, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step8_control_ready","^\\[m7_gpio_test\\] T0a_mcu control ready$", 0xFF) \
    X(8, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_READ mcu_ctrl", "", 0xFF) \
    X(8, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step8_ctrl",         "", 0xFF) \
    X(8, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step8_mcu_done",     "^\\[m7_gpio_test\\] T0a_mcu: DONE \\(host verdict\\)$", 0xFF) \
    \
    /* Step 9: T0a Walking-1 AD2->MCU 10 lines (bus=AD2) */ \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_announcement", "^\\[m7_gpio_test\\] STEP id=9 bus=AD2 v=2$", 0xFF) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_buschk_up",    "^\\[m7_gpio_test\\] BUSCHK up$", 0xFF) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step9_up",           "", 0xFF) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_buschk_down",  "^\\[m7_gpio_test\\] BUSCHK down$", 0xFF) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step9_down",         "", 0xFF) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_ad2_ready",    "^\\[m7_gpio_test\\] T0a_ad2 ready$", 0xFF) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step9_ready",        "", 0xFF) \
    X(9, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_DRIVE ad2_bit", "", 8) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step9_ad2_8",        "", 8) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_ad2_8_res",    "^\\[m7_gpio_test\\] T0a_ad2 bit=8 idr=0x[0-9a-fA-F]+ exp=0x[0-9a-fA-F]+ OK$", 8) \
    X(9, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_DRIVE ad2_bit", "", 10) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step9_ad2_10",       "", 10) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_ad2_10_res",   "^\\[m7_gpio_test\\] T0a_ad2 bit=10 idr=0x[0-9a-fA-F]+ exp=0x[0-9a-fA-F]+ OK$", 10) \
    X(9, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_DRIVE ad2_bit", "", 0) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step9_ad2_0",        "", 0) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_ad2_0_res",    "^\\[m7_gpio_test\\] T0a_ad2 bit=0 idr=0x[0-9a-fA-F]+ exp=0x[0-9a-fA-F]+ OK$", 0) \
    X(9, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_DRIVE ad2_bit", "", 1) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step9_ad2_1",        "", 1) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_ad2_1_res",    "^\\[m7_gpio_test\\] T0a_ad2 bit=1 idr=0x[0-9a-fA-F]+ exp=0x[0-9a-fA-F]+ OK$", 1) \
    X(9, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_DRIVE ad2_bit", "", 2) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step9_ad2_2",        "", 2) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_ad2_2_res",    "^\\[m7_gpio_test\\] T0a_ad2 bit=2 idr=0x[0-9a-fA-F]+ exp=0x[0-9a-fA-F]+ OK$", 2) \
    X(9, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_DRIVE ad2_bit", "", 3) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step9_ad2_3",        "", 3) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_ad2_3_res",    "^\\[m7_gpio_test\\] T0a_ad2 bit=3 idr=0x[0-9a-fA-F]+ exp=0x[0-9a-fA-F]+ OK$", 3) \
    X(9, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_DRIVE ad2_bit", "", 4) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step9_ad2_4",        "", 4) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_ad2_4_res",    "^\\[m7_gpio_test\\] T0a_ad2 bit=4 idr=0x[0-9a-fA-F]+ exp=0x[0-9a-fA-F]+ OK$", 4) \
    X(9, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_DRIVE ad2_bit", "", 5) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step9_ad2_5",        "", 5) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_ad2_5_res",    "^\\[m7_gpio_test\\] T0a_ad2 bit=5 idr=0x[0-9a-fA-F]+ exp=0x[0-9a-fA-F]+ OK$", 5) \
    X(9, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_DRIVE ad2_bit", "", 6) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step9_ad2_6",        "", 6) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_ad2_6_res",    "^\\[m7_gpio_test\\] T0a_ad2 bit=6 idr=0x[0-9a-fA-F]+ exp=0x[0-9a-fA-F]+ OK$", 6) \
    X(9, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_DRIVE ad2_bit", "", 7) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step9_ad2_7",        "", 7) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_ad2_7_res",    "^\\[m7_gpio_test\\] T0a_ad2 bit=7 idr=0x[0-9a-fA-F]+ exp=0x[0-9a-fA-F]+ OK$", 7) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_analog_ready", "^\\[m7_gpio_test\\] T0a_ad2 analog control ready$", 0xFF) \
    X(9, BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_DRIVE ad2_analog","", 0xFF) \
    X(9, BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_HOST,"step9_analog",       "", 0xFF) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_analog_res",   "^\\[m7_gpio_test\\] T0a_ad2 analog control result: OK$", 0xFF) \
    X(9, BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step9_verdict",      "^\\[m7_gpio_test\\] T0a_ad2: PASS$", 0xFF) \
    \
    /* Step 10: T0a Scope Measurements (bus=NONE) */ \
    X(10,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step10_announcement","^\\[m7_gpio_test\\] STEP id=10 bus=NONE v=2$", 0xFF) \
    X(10,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step10_low_ready",   "^\\[m7_gpio_test\\] Scope LOW ready$", 0xFF) \
    X(10,BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_MEASURE scope_low","", 0xFF) \
    X(10,BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step10_low",         "", 0xFF) \
    X(10,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step10_high_ready",  "^\\[m7_gpio_test\\] Scope HIGH ready$", 0xFF) \
    X(10,BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_MEASURE scope_high","", 0xFF) \
    X(10,BENCH_ITEM_PRIM,    GPIO_BENCH_PRIM_HS_MCU, "step10_high",        "", 0xFF) \
    X(10,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step10_scope_done",  "^\\[m7_gpio_test\\] T0a_scope: DONE \\(host verdict\\)$", 0xFF) \
    \
    /* Step 20: T16 LCKR, the last test of the image (bus=NONE) */ \
    X(20,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step20_announcement", "^\\[m7_gpio_test\\] STEP id=20 bus=NONE v=2$", 0xFF) \
    X(20,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step20_verdict", "^\\[m7_gpio_test\\] T16 VERDICT: PASS$", 0xFF) \
    X(20,BENCH_ITEM_MEASURE, GPIO_BENCH_PRIM_NONE,   "HOST_CHECK t16_pass", "", 0xFF) \
    \
    /* Step 99: All Done (bus=NONE) */ \
    X(99,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step99_announcement","^\\[m7_gpio_test\\] STEP id=99 bus=NONE v=2$", 0xFF) \
    X(99,BENCH_ITEM_LINE,    GPIO_BENCH_PRIM_NONE,   "step99_verdict",     "^\\[m7_gpio_test\\] VERDICT: ALL GPIO SELF-TESTS PASSED$", 0xFF)

extern const BenchProtocolItem_t g_bench_protocol_items[];
extern const size_t g_bench_protocol_item_count;

static inline uint8_t bench_get_expected_primitive(uint32_t step_id, uint32_t prim_idx)
{
    uint32_t count = 0U;
    for (size_t i = 0U; i < g_bench_protocol_item_count; i++)
    {
        if (g_bench_protocol_items[i].step_id == (uint8_t)step_id &&
            g_bench_protocol_items[i].item_type == BENCH_ITEM_PRIM)
        {
            if (count == prim_idx)
            {
                return g_bench_protocol_items[i].primitive;
            }
            count++;
        }
    }
    return GPIO_BENCH_PRIM_NONE;
}

static inline uint8_t bench_get_step_bus_owner(uint8_t step_id)
{
    switch (step_id)
    {
#define EXPAND_STEP_BUS_CASE(id, name, bus) case (id): return (bus);
        GPIO_BENCH_STEPS_TABLE(EXPAND_STEP_BUS_CASE)
#undef EXPAND_STEP_BUS_CASE
        default: return GPIO_BENCH_BUS_NONE;
    }
}

static inline const char *bench_get_step_bus_name(uint8_t step_id)
{
    uint8_t bus = bench_get_step_bus_owner(step_id);
    switch (bus)
    {
        case GPIO_BENCH_BUS_MCU: return "MCU";
        case GPIO_BENCH_BUS_AD2: return "AD2";
        default:                 return "NONE";
    }
}



/* ---- Dual-Core SRAM4 Communication Structure (m2: defined in shared_memory.h) - */
_Static_assert((SHARED_RAM_BASE + sizeof(SharedMemory_t)) <= HSEM_TEST_SHARED_ADDR,
               "SharedMemory_t overlaps HSEM_TEST_SHARED_ADDR in SRAM4 (m2)");

_Static_assert((HSEM_TEST_SHARED_ADDR + sizeof(HsemTestShared_t)) <= GPIO_TEST_SHARED_ADDR,
               "HsemTestShared_t overlaps GPIO_TEST_SHARED_ADDR in SRAM4 (m2)");

typedef struct __attribute__((aligned(32)))
{
    volatile uint32_t session_id;          /*!< Unique session ID set by CM7 on cold boot */
    volatile uint32_t cm4_ready;           /*!< Set to session_id when CM4 is ready */
    volatile uint32_t go_signal;           /*!< Unused since v2 */
    volatile uint32_t current_step;        /*!< Step ID currently being executed */
    volatile uint32_t bus_owner;           /*!< GPIO_BENCH_BUS_NONE, MCU, or AD2 */

    /* CM7 -> CM4 command channel (v2). CM7 writes cmd, CM4 runs that job and
       writes the same code to ack. Codes are issued in a fixed order, and
       CPU1 zeroes the block before releasing CPU2, so no stale code is seen. */
    volatile uint32_t cmd;                 /*!< GPIO_ST_CMD_* posted by CM7 */
    volatile uint32_t ack;                 /*!< GPIO_ST_CMD_* completed by CM4 */

    /* T15 loop markers */
    volatile uint32_t in_loop_cm7;         /*!< 1 while CM7 is inside a concurrent loop */
    volatile uint32_t in_loop_cm4;         /*!< 1 while CM4 is inside a concurrent loop */

    /* T15 CM4 results, per phase: [0]=b, [1]=c, [2]=d, [3]=e */
    volatile uint32_t t15_cm4_corr[4];
    volatile uint32_t t15_cm4_overlap[4];  /*!< [1..3] used */
    volatile uint32_t t15_cm4_calls[4];    /*!< loop iterations */
    volatile uint32_t t15_cm4_ok[4];       /*!< ConfigPin OK count, (d)/(e) */
    volatile uint32_t t15_cm4_busy[4];     /*!< ConfigPin ERR_BUSY count, (d)/(e) */
    volatile uint32_t t15_cm4_verify[4];   /*!< ConfigPin ERR_VERIFY count, (d)/(e) */
    volatile uint32_t t15_locked_calls;    /*!< (e): incremented by both test hooks while holding the lock */
    volatile uint32_t t15_cm4_hook_fail;   /*!< (e): TakeTimeout failures in the CM4 test hook */

    /* B1, CM4 side (DWT cycles at 200 MHz) */
    volatile uint32_t b1_cm4_cfg_nohook_min;
    volatile uint32_t b1_cm4_cfg_nohook_max;
    volatile uint32_t b1_cm4_cfg_hook_min;
    volatile uint32_t b1_cm4_cfg_hook_max;
    volatile uint32_t b1_cm4_hold_max;     /*!< lock taken -> unlock, max over T15 (e) */
    volatile uint32_t b1_cm4_primask_max;  /*!< lock() entry -> unlock, max over T15 (e) */
    volatile uint32_t b1_cm4_done;

    /* T1 / T2, CM4 side */
    volatile uint32_t t1_cm4_pass;
    volatile uint32_t t1_cm4_cases;
    volatile uint32_t t1_cm4_failed_cases;
    volatile uint32_t t1_cm4_control_changed;
    volatile uint32_t t2_cm4_pass;
    volatile uint32_t t2_cm4_combos;
    volatile uint32_t t2_cm4_mismatches;
    volatile uint32_t t2_cm4_control_detected;

    /* Global verdict */
    volatile uint32_t cm4_done;
    volatile uint32_t cm4_all_passed;
} GpioSelfTestShared_t;

_Static_assert((GPIO_TEST_SHARED_ADDR + sizeof(GpioSelfTestShared_t)) <= SHARED_RAM_END,
               "GpioSelfTestShared_t exceeds SHARED_RAM_END in SRAM4 (m2)");

#define GPIO_TEST_SHARED  ((GpioSelfTestShared_t *)GPIO_TEST_SHARED_ADDR)

/* CM7 -> CM4 jobs, in the order CM7 issues them (v2). */
#define GPIO_ST_CMD_NONE      0U
#define GPIO_ST_CMD_T15_B     1U  /*!< CM4 solo, hand RMW */
#define GPIO_ST_CMD_T15_C     2U  /*!< both cores, hand RMW */
#define GPIO_ST_CMD_T15_D     3U  /*!< both cores, gpio_bm without hook */
#define GPIO_ST_CMD_T15_E     4U  /*!< both cores, gpio_bm with the test hook */
#define GPIO_ST_CMD_T1        5U  /*!< CM4 T1 while CM7 is parked with PRIMASK set */
#define GPIO_ST_CMD_T2        6U  /*!< CM4 T2 */
#define GPIO_ST_CMD_B1        7U  /*!< CM4 B1; CM4 finishes after it */

#ifdef __cplusplus
}
#endif

#endif /* GPIO_BENCH_PROTOCOL_H */
