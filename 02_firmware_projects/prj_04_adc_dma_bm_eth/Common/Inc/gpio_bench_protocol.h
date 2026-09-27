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

#include "shared_memory.h"

#define GPIO_BENCH_PROTOCOL_VERSION     1U

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

/* ---- Dual-Core SRAM4 Communication Structure (m2: defined in shared_memory.h) - */
_Static_assert(sizeof(SharedMemory_t) <= (GPIO_TEST_SHARED_ADDR - 0x38000000UL),
               "SharedMemory_t overlaps GPIO_TEST_SHARED_ADDR in SRAM4 (m2)");

typedef struct __attribute__((aligned(32)))
{
    volatile uint32_t session_id;          /*!< Unique session ID set by CM7 on cold boot */
    volatile uint32_t cm7_ready;           /*!< Set to session_id when CM7 is ready */
    volatile uint32_t cm4_ready;           /*!< Set to session_id when CM4 is ready */
    volatile uint32_t go_signal;           /*!< Set to session_id to release execution */
    volatile uint32_t current_step;        /*!< Step ID currently being executed */
    volatile uint32_t bus_owner;           /*!< GPIO_BENCH_BUS_NONE, MCU, or AD2 */

    /* T15 synchronization and loop markers */
    volatile uint32_t t15_phase;           /*!< 1: CM7 solo, 2: CM4 solo, 3: concurrent */
    volatile uint32_t in_loop_cm7;         /*!< 1 while CM7 is inside the loop */
    volatile uint32_t in_loop_cm4;         /*!< 1 while CM4 is inside the loop */
    volatile uint32_t t15_cm7_overlap_cnt; /*!< CM7 loop count seeing in_loop_cm4 == 1 */
    volatile uint32_t t15_cm4_overlap_cnt; /*!< CM4 loop count seeing in_loop_cm7 == 1 */

    /* T15 results */
    volatile uint32_t t15_corruptions_phase_a; /*!< CM7 solo: expected 0 */
    volatile uint32_t t15_corruptions_phase_b; /*!< CM4 solo: expected 0 */
    volatile uint32_t t15_cm7_corruptions_c;   /*!< CM7 concurrent corruptions */
    volatile uint32_t t15_cm4_corruptions_c;   /*!< CM4 concurrent corruptions */
    volatile uint32_t t15_corruptions_phase_c; /*!< Total concurrent: expected > 0 */
    volatile uint32_t t15_total_calls_cm7;
    volatile uint32_t t15_total_calls_cm4;

    /* T0a results */
    volatile uint32_t t0a_loose_pass;
    volatile uint32_t t0a_hs_pass;
    volatile uint32_t t0a_mcu_pass;
    volatile uint32_t t0a_ad2_pass;
    volatile uint32_t t0a_scope_pass;

    /* T0b results */
    volatile uint32_t t0b_claim_pass;     /*!< (b) bench_claim_output rejections */
    volatile uint32_t t0b_s5_neg_pass;    /*!< (c) S5 negative control */
    volatile uint32_t t0b_timeout_pass;   /*!< (d) 10s timeout to bench_safe */

    /* Global verdict */
    volatile uint32_t cm4_done;
    volatile uint32_t cm4_all_passed;
} GpioSelfTestShared_t;

/* Compatibility aliases for T15 legacy access during step transitions */
#define t15_cm7_count           t15_total_calls_cm7
#define t15_cm4_count           t15_total_calls_cm4
#define t15_corruptions_cm7     t15_corruptions_phase_a
#define t15_corruptions_cm4     t15_corruptions_phase_b
#define t15_done_cm7            in_loop_cm7
#define t15_done_cm4            in_loop_cm4
#define t15_total_corruptions   t15_corruptions_phase_c

#define GPIO_TEST_SHARED  ((GpioSelfTestShared_t *)GPIO_TEST_SHARED_ADDR)

#ifdef __cplusplus
}
#endif

#endif /* GPIO_BENCH_PROTOCOL_H */
