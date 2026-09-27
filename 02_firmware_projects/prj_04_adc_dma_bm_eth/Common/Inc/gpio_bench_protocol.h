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

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_BENCH_PROTOCOL_VERSION     1U

/* ---- Bus Ownership (S1, S5) ---------------------------------------------- */
#define GPIO_BENCH_BUS_NONE             0U  /*!< Bus is loose: both sides in input */
#define GPIO_BENCH_BUS_MCU              1U  /*!< MCU drives PE8..PE15, AD2 in input */
#define GPIO_BENCH_BUS_AD2              2U  /*!< AD2 drives DIO0..DIO7, MCU in input */

/* ---- Protocol Test Steps (S7) -------------------------------------------- */
#define GPIO_BENCH_STEP_IDLE            0U  /*!< Safe state S2: all pins input */
#define GPIO_BENCH_STEP_T0B_GUARDS      1U  /*!< T0b(b): guards verification without real collision */
#define GPIO_BENCH_STEP_T0B_S5_NEG      2U  /*!< T0b(c): negative control for S5 handover */
#define GPIO_BENCH_STEP_T0B_TIMEOUT     3U  /*!< T0b(d): 10-second timeout test */
#define GPIO_BENCH_STEP_T0A_LOOSE       4U  /*!< T0a: bus loose check (pullup/pulldown/analog) */
#define GPIO_BENCH_STEP_T0A_MCU_TO_AD2  5U  /*!< T0a: walking-1 MCU -> AD2 (Solo MCU + bus) */
#define GPIO_BENCH_STEP_T0A_AD2_TO_MCU  6U  /*!< T0a: walking-1 AD2 -> MCU (Solo AD2 + bus) */
#define GPIO_BENCH_STEP_T0A_SCOPE       7U  /*!< T0a: Scope 1/2 DC levels on PF14/PF15 */
#define GPIO_BENCH_STEP_T15_RACE        15U /*!< T15: dual-core race on PF10/PF11 (unwired) */
#define GPIO_BENCH_STEP_DONE            99U /*!< All tests completed */

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

/* ---- Dual-Core SRAM4 Communication Structure (0x3800A000) ---------------- */
#ifndef GPIO_TEST_SHARED_ADDR
#define GPIO_TEST_SHARED_ADDR           (0x3800A000UL)
#endif

typedef struct __attribute__((aligned(32)))
{
    volatile uint32_t cm7_ready;
    volatile uint32_t cm4_ready;
    volatile uint32_t go_signal;
    volatile uint32_t current_step;
    volatile uint32_t bus_owner;

    /* T0a results */
    volatile uint32_t t0a_loose_pass;
    volatile uint32_t t0a_mcu_pass;
    volatile uint32_t t0a_ad2_pass;
    volatile uint32_t t0a_scope_pass;

    /* T0b results */
    volatile uint32_t t0b_claim_pass;     /*!< (b) bench_claim_output rejections */
    volatile uint32_t t0b_s5_neg_pass;    /*!< (c) S5 negative control */
    volatile uint32_t t0b_timeout_pass;   /*!< (d) 10s timeout to bench_safe */

    /* T15 race negative control results (PF10 CM7, PF11 CM4) */
    volatile uint32_t t15_cm7_count;
    volatile uint32_t t15_cm4_count;
    volatile uint32_t t15_corruptions_cm7;
    volatile uint32_t t15_corruptions_cm4;
    volatile uint32_t t15_total_corruptions;
    volatile uint32_t t15_done_cm7;
    volatile uint32_t t15_done_cm4;

    /* Global verdict */
    volatile uint32_t cm4_done;
    volatile uint32_t cm4_all_passed;
} GpioSelfTestShared_t;

#define GPIO_TEST_SHARED  ((GpioSelfTestShared_t *)GPIO_TEST_SHARED_ADDR)

#ifdef __cplusplus
}
#endif

#endif /* GPIO_BENCH_PROTOCOL_H */
