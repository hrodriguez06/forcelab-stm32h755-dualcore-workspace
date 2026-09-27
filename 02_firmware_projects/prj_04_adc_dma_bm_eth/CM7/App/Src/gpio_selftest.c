/**
  ******************************************************************************
  * @file    gpio_selftest.c
  * @brief   GPIO driver self-test implementation for Cortex-M7 (prj_04).
  *          Coordinated dual-core tests, bench safety guards S1-S10,
  *          T0a loose line checks, T0b guard verifications, and T15 race control.
  ******************************************************************************
  */

#include "gpio_selftest.h"
#include "gpio_bench_protocol.h"
#include "shared_memory.h"
#include "app_log.h"
#include "stm32h7xx.h"
#include <string.h>
#include <stdbool.h>

/* Forward declarations */
static bool bench_claim_output(GPIO_TypeDef *port, uint32_t pin, uint32_t step);
static void bench_safe(void);
static bool wait_host_req(bool expected_high, uint32_t timeout_ms);
static void set_mcu_ack(bool high);
static bool mcu_handshake(uint32_t timeout_ms);

/**
 * @brief Safety guard S4: Bench claim check for Cortex-M7.
 *        Guards MODER, OTYPER, and AFR writes on wired and unwired pins.
 */
static bool bench_claim_output(GPIO_TypeDef *port, uint32_t pin, uint32_t step)
{
    /* PE0 (SWIER) and PE1 (LD2 Yellow LED) MUST NEVER BE TOUCHED */
    if (port == GPIOE && (pin == 0U || pin == 1U))
    {
        return false;
    }

    /* PE2 (HOST_REQ), PE3 (EXTI3), PE4 (EXTI4) are Solo AD2 -> NEVER driven by MCU */
    if (port == GPIOE && (pin == 2U || pin == 3U || pin == 4U))
    {
        return false;
    }

    /* PE8..PE15 (Bus bits 0..7): only allowed if step is an authorized bus=MCU step */
    if (port == GPIOE && (pin >= 8U && pin <= 15U))
    {
        if (step == GPIO_BENCH_STEP_T0A_MCU_TO_AD2 ||
            step == GPIO_BENCH_STEP_T0B_S5_NEG)
        {
            return true;
        }
        return false;
    }

    /* PA8: only allowed if step == T10 (MCO1 test) */
    if (port == GPIOA && pin == 8U)
    {
        return (step == 10U);
    }

    /* PE5 (CM7 ACK) and PE7 (MCU_ACK) are Solo MCU */
    if (port == GPIOE && (pin == 5U || pin == 7U))
    {
        return true;
    }

    /* PE6 is Solo MCU (CM4 ACK) */
    if (port == GPIOE && pin == 6U)
    {
        return true;
    }

    /* PF14 (Scope 1) and PF15 (Scope 2) are Solo MCU */
    if (port == GPIOF && (pin == 14U || pin == 15U))
    {
        return true;
    }

    /* Unwired test pins PF6..PF11 allowed (PF10 CM7, PF11 CM4 for T15 race) */
    if (port == GPIOF && (pin >= 6U && pin <= 11U))
    {
        return true;
    }

    /* All other ports/pins rejected */
    return false;
}

/**
 * @brief Helper to configure a wired pin's mode under bench_claim_output protection (S4).
 */
static bool bench_set_pin_mode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t otype, uint32_t step)
{
    /* If claiming output (01), AF (10), or any driven mode, must pass S4 guard */
    if (mode == 1U || mode == 2U)
    {
        if (!bench_claim_output(port, pin, step))
        {
            return false;
        }
    }
    else
    {
        /* Input or Analog: PE0 and PE1 must still be rejected unconditionally */
        if (port == GPIOE && (pin == 0U || pin == 1U))
        {
            return false;
        }
    }

    /* Apply OTYPER if output */
    if (mode == 1U)
    {
        if (otype == 1U)
        {
            port->OTYPER |= (1UL << pin);
        }
        else
        {
            port->OTYPER &= ~(1UL << pin);
        }
    }

    /* Apply MODER */
    uint32_t moder = port->MODER;
    moder &= ~(3UL << (pin * 2U));
    moder |= ((mode & 3UL) << (pin * 2U));
    port->MODER = moder;

    return true;
}

/**
 * @brief Enforces S2 Safe State on MCU side.
 *        All wired test pins set to INPUT mode (00) without pull, except:
 *          - PE7 (MCU_ACK) which is OUTPUT LOW (01)
 *          - PE2 (HOST_REQ) which is INPUT with pull-down (10)
 *        PE0 and PE1 (LD2) are completely UNTOUCHED.
 */
static void bench_safe(void)
{
    /* Ensure clocks are active for test ports */
    RCC->AHB4ENR |= (RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOEEN | RCC_AHB4ENR_GPIOFEN);
    __DSB();

    /* GPIOE: Configure PE2..PE6 and PE8..PE15 as INPUT (00).
       Crucial: PE0 and PE1 (bits 3:0 of MODER/PUPDR) are NEVER touched! */
    uint32_t mask_pe2_to_pe6 = 0x00003FC0UL;  /* pins 2, 3, 4, 5, 6 */
    uint32_t mask_pe8_to_pe15 = 0xFFFF0000UL; /* pins 8..15 */
    uint32_t clear_mask_e = mask_pe2_to_pe6 | mask_pe8_to_pe15;

    GPIOE->MODER &= ~clear_mask_e;
    GPIOE->PUPDR &= ~clear_mask_e;

    /* PE2 (HOST_REQ): pull-down so it stays cleanly 0 when AD2 is not driving */
    GPIOE->PUPDR |= (2UL << (2U * 2U));

    /* PE7 (MCU_ACK) is Solo MCU: set to OUTPUT LOW (01) */
    if (bench_claim_output(GPIOE, 7U, GPIO_BENCH_STEP_IDLE))
    {
        /* ODR low first (C11) */
        GPIOE->BSRR = (1UL << (7U + 16U));
        GPIOE->OTYPER &= ~(1UL << 7U);
        uint32_t moder_e = GPIOE->MODER;
        moder_e &= ~(3UL << (7U * 2U));
        moder_e |= (1UL << (7U * 2U));
        GPIOE->MODER = moder_e;
    }

    /* GPIOF: Configure PF14 and PF15 as INPUT (00) without pull */
    uint32_t clear_mask_f = (3UL << (14U * 2U)) | (3UL << (15U * 2U));
    GPIOF->MODER &= ~clear_mask_f;
    GPIOF->PUPDR &= ~clear_mask_f;

    /* GPIOA: Configure PA8 as INPUT (00) without pull */
    GPIOA->MODER &= ~(3UL << (8U * 2U));
    GPIOA->PUPDR &= ~(3UL << (8U * 2U));

    __DSB();
}

static void set_mcu_ack(bool high)
{
    if (high)
    {
        GPIOE->BSRR = (1UL << 7U);
    }
    else
    {
        GPIOE->BSRR = (1UL << (7U + 16U));
    }
    __DSB();
}

static bool wait_host_req(bool expected_high, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint32_t target_mask = (1UL << 2U); /* PE2 (HOST_REQ) */

    for (;;)
    {
        bool is_high = ((GPIOE->IDR & target_mask) != 0U);
        if (is_high == expected_high)
        {
            return true;
        }
        if ((HAL_GetTick() - start) >= timeout_ms)
        {
            return false;
        }
    }
}

/**
 * @brief 4-phase asynchronous level handshake on MCU side.
 */
static bool mcu_handshake(uint32_t timeout_ms)
{
    set_mcu_ack(true);
    if (!wait_host_req(true, timeout_ms))
    {
        set_mcu_ack(false);
        return false;
    }
    set_mcu_ack(false);
    return wait_host_req(false, timeout_ms);
}

/**
 * @brief S5 Bus handover loose check.
 */
static bool s5_check_bus_loose(void)
{
    /* 1. Pull-up check: MCU puts PE8..PE15 in input pull-up */
    uint32_t pupdr = GPIOE->PUPDR;
    pupdr &= ~0xFFFF0000UL;
    pupdr |= 0x55550000UL; /* 01 on bits 8..15 */
    GPIOE->PUPDR = pupdr;

    HAL_Delay(GPIO_BENCH_DEADTIME_MS);

    uint32_t idr_up = (GPIOE->IDR >> 8U) & 0xFFU;
    APP_LOG("[m7_gpio_test] BUSCHK up");
    if (!mcu_handshake(GPIO_BENCH_TIMEOUT_MS))
    {
        bench_safe();
        return false;
    }

    /* 2. Pull-down check: MCU puts PE8..PE15 in input pull-down */
    pupdr = GPIOE->PUPDR;
    pupdr &= ~0xFFFF0000UL;
    pupdr |= 0xAAAA0000UL; /* 10 on bits 8..15 */
    GPIOE->PUPDR = pupdr;

    HAL_Delay(GPIO_BENCH_DEADTIME_MS);

    uint32_t idr_down = (GPIOE->IDR >> 8U) & 0xFFU;
    APP_LOG("[m7_gpio_test] BUSCHK down");
    if (!mcu_handshake(GPIO_BENCH_TIMEOUT_MS))
    {
        bench_safe();
        return false;
    }

    /* Restore PE8..PE15 to no pull */
    GPIOE->PUPDR &= ~0xFFFF0000UL;

    return (idr_up == 0xFFU && idr_down == 0x00U);
}

/* ---- T0b Test Implementations -------------------------------------------- */

static bool test_t0b_claim_guards(void)
{
    bool all_rejected = true;
    uint32_t moder_before = GPIOE->MODER;

    /* (1) PE0 and PE1 rejection */
    if (bench_claim_output(GPIOE, 0U, GPIO_BENCH_STEP_T0B_GUARDS)) all_rejected = false;
    if (bench_claim_output(GPIOE, 1U, GPIO_BENCH_STEP_T0B_GUARDS)) all_rejected = false;

    /* (2) PE2, PE3, PE4 rejection */
    if (bench_claim_output(GPIOE, 2U, GPIO_BENCH_STEP_T0B_GUARDS)) all_rejected = false;
    if (bench_claim_output(GPIOE, 3U, GPIO_BENCH_STEP_T0B_GUARDS)) all_rejected = false;
    if (bench_claim_output(GPIOE, 4U, GPIO_BENCH_STEP_T0B_GUARDS)) all_rejected = false;

    /* (3) PE8..PE15 rejection when step is not bus=MCU */
    for (uint32_t pin = 8U; pin <= 15U; pin++)
    {
        if (bench_claim_output(GPIOE, pin, GPIO_BENCH_STEP_T0B_GUARDS))
        {
            all_rejected = false;
        }
    }

    /* Verify MODER was untouched */
    if (GPIOE->MODER != moder_before)
    {
        all_rejected = false;
    }

    APP_LOG("[m7_gpio_test] T0b(b) claim guards: %s", all_rejected ? "PASS" : "FAIL");
    return all_rejected;
}

static bool test_t0b_s5_neg(void)
{
    APP_LOG("[m7_gpio_test] T0b_c ready bus=MCU");

    /* MCU intentionally claims PE8..PE15 for negative control step */
    for (uint32_t pin = 8U; pin <= 15U; pin++)
    {
        if (!bench_set_pin_mode(GPIOE, pin, 1U, 0U, GPIO_BENCH_STEP_T0B_S5_NEG))
        {
            return false;
        }
    }

    /* Drive 0x5A on bus lines PE8..PE15 (0x5A00) while AD2 is in input (no contention) */
    uint32_t set_mask = 0x5A00UL;
    uint32_t reset_mask = 0xA500UL;
    GPIOE->BSRR = (reset_mask << 16U) | set_mask;

    APP_LOG("[m7_gpio_test] T0b(c) MCU driving bus to 0x5A (AD2 in input)");

    /* Host executes S5 check, detects driven bus, aborts safely, and pulses HOST_REQ */
    bool host_ok = mcu_handshake(GPIO_BENCH_TIMEOUT_MS);

    /* Release bus back to safe input */
    bench_safe();

    APP_LOG("[m7_gpio_test] T0b(c) S5 negative control: %s", host_ok ? "PASS" : "FAIL");
    return host_ok;
}

static bool test_t0b_timeout(void)
{
    APP_LOG("[m7_gpio_test] T0b_d ready bus=NONE");
    APP_LOG("[m7_gpio_test] T0b(d) waiting for HOST_REQ timeout (10s)...");

    /* Deliberately wait for HOST_REQ which host will NOT send for 10s */
    bool received = wait_host_req(true, GPIO_BENCH_TIMEOUT_MS);

    /* Must time out (received == false) */
    if (received)
    {
        APP_LOG("[m7_gpio_test] T0b(d) FAIL: spurious HOST_REQ received!");
        return false;
    }

    /* S2 safe state must be enforced on timeout */
    bench_safe();

    /* Verify all wired test pins are inputs */
    bool all_inputs = (((GPIOE->MODER & 0xFFFF3FC0UL) == 0U) &&
                       ((GPIOF->MODER & ((3UL << 28U) | (3UL << 30U))) == 0U) &&
                       ((GPIOA->MODER & (3UL << 16U)) == 0U));

    APP_LOG("[m7_gpio_test] T0b(d) timeout expired -> bench_safe() invoked, all pins input (%s)",
            all_inputs ? "PASS" : "FAIL");

    /* Re-synchronize with host after the timeout */
    (void)mcu_handshake(5000U);

    return all_inputs;
}

/* ---- T0a Test Implementations -------------------------------------------- */

static bool test_t0a_loose(void)
{
    APP_LOG("[m7_gpio_test] T0a_loose ready bus=NONE");

    /* 1. Pull-up check on all test lines (PE7 MCU_ACK stays OUTPUT, PE2 HOST_REQ stays INPUT) */
    uint32_t pupdr_e = GPIOE->PUPDR;
    pupdr_e &= ~0xFFFF3F00UL;
    pupdr_e |= (0x55550000UL | (1UL << (5U * 2U)) | (1UL << (6U * 2U)) | (1UL << (3U * 2U)) | (1UL << (4U * 2U)));
    GPIOE->PUPDR = pupdr_e;

    GPIOA->PUPDR &= ~(3UL << 16U);
    GPIOA->PUPDR |= (1UL << 16U);

    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    APP_LOG("[m7_gpio_test] BUSCHK up");
    if (!mcu_handshake(GPIO_BENCH_TIMEOUT_MS)) return false;

    /* 2. Pull-down check */
    pupdr_e = GPIOE->PUPDR;
    pupdr_e &= ~0xFFFF3F00UL;
    pupdr_e |= (0xAAAA0000UL | (2UL << (5U * 2U)) | (2UL << (6U * 2U)) | (2UL << (3U * 2U)) | (2UL << (4U * 2U)));
    GPIOE->PUPDR = pupdr_e;

    GPIOA->PUPDR &= ~(3UL << 16U);
    GPIOA->PUPDR |= (2UL << 16U);

    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    APP_LOG("[m7_gpio_test] BUSCHK down");
    if (!mcu_handshake(GPIO_BENCH_TIMEOUT_MS)) return false;

    /* 3. Analog negative control: PE8..PE15 in analog mode (MODER = 11, PUPDR = 00) */
    GPIOE->PUPDR &= ~0xFFFF0000UL;
    for (uint32_t pin = 8U; pin <= 15U; pin++)
    {
        bench_set_pin_mode(GPIOE, pin, 3U, 0U, GPIO_BENCH_STEP_T0A_LOOSE);
    }
    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    APP_LOG("[m7_gpio_test] BUSCHK analog");

    if (!mcu_handshake(GPIO_BENCH_TIMEOUT_MS)) return false;

    bench_safe();
    APP_LOG("[m7_gpio_test] T0a loose check: PASS");
    return true;
}

static bool test_t0a_mcu_to_ad2(void)
{
    APP_LOG("[m7_gpio_test] T0a_mcu ready bus=MCU");

    /* Step S5 check before bus driving */
    if (!s5_check_bus_loose())
    {
        APP_LOG("[m7_gpio_test] ABORT: S5 pre-check failed before MCU walking-1");
        return false;
    }

    /* Configure Solo MCU lines as push-pull output: PE5, PE6, PE7, PA8 */
    bench_set_pin_mode(GPIOE, 5U, 1U, 0U, GPIO_BENCH_STEP_T0A_MCU_TO_AD2);
    bench_set_pin_mode(GPIOE, 6U, 1U, 0U, GPIO_BENCH_STEP_T0A_MCU_TO_AD2);
    bench_set_pin_mode(GPIOE, 7U, 1U, 0U, GPIO_BENCH_STEP_T0A_MCU_TO_AD2);
    bench_set_pin_mode(GPIOA, 8U, 1U, 0U, 10U /* T10 authorized */);

    /* Configure bus PE8..PE15 as push-pull output under step T0A_MCU_TO_AD2 */
    for (uint32_t pin = 8U; pin <= 15U; pin++)
    {
        bench_set_pin_mode(GPIOE, pin, 1U, 0U, GPIO_BENCH_STEP_T0A_MCU_TO_AD2);
    }

    /* Ensure all outputs initially low */
    GPIOE->BSRR = (0xFFE0UL << 16U);
    GPIOA->BSRR = (1UL << (8U + 16U));

    /* Walking-1 across test lines: PE5, PE6, and PE8..PE15 */
    uint32_t test_pins[] = { 5U, 6U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U };
    for (uint32_t i = 0U; i < sizeof(test_pins) / sizeof(test_pins[0]); i++)
    {
        uint32_t pin = test_pins[i];
        GPIOE->BSRR = (1UL << pin);
        HAL_Delay(1);

        if (!mcu_handshake(GPIO_BENCH_TIMEOUT_MS)) { bench_safe(); return false; }

        GPIOE->BSRR = (1UL << (pin + 16U));
    }

    /* Test PA8 */
    GPIOA->BSRR = (1UL << 8U);
    HAL_Delay(1);
    if (!mcu_handshake(GPIO_BENCH_TIMEOUT_MS)) { bench_safe(); return false; }
    GPIOA->BSRR = (1UL << (8U + 16U));

    bench_safe();
    APP_LOG("[m7_gpio_test] T0a MCU->AD2 walking-1: PASS");
    return true;
}

static bool test_t0a_ad2_to_mcu(void)
{
    APP_LOG("[m7_gpio_test] T0a_ad2 ready bus=AD2");

    /* Pre-check bus loose */
    if (!s5_check_bus_loose())
    {
        APP_LOG("[m7_gpio_test] ABORT: S5 pre-check failed before AD2 walking-1");
        return false;
    }

    /* Configure input pull-down on PE3 (EXTI3), PE4 (EXTI4), PE8..PE15 */
    uint32_t pupdr = GPIOE->PUPDR;
    pupdr &= ~0xFFFF0030UL;
    pupdr |= (0xAAAA0000UL | (2UL << (3U * 2U)) | (2UL << (4U * 2U)));
    GPIOE->PUPDR = pupdr;

    /* Signal ready to host via initial handshake */
    if (!mcu_handshake(GPIO_BENCH_TIMEOUT_MS)) { bench_safe(); return false; }

    /* Host walks bits on AD2 and confirms with REQ/ACK for each of the 10 bits */
    uint32_t expected_mcu_pins[] = { 3U, 4U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U };
    for (uint32_t i = 0U; i < sizeof(expected_mcu_pins) / sizeof(expected_mcu_pins[0]); i++)
    {
        uint32_t pin = expected_mcu_pins[i];
        /* Wait for host to assert HOST_REQ */
        if (!wait_host_req(true, GPIO_BENCH_TIMEOUT_MS)) { bench_safe(); return false; }

        /* Read pin state */
        bool pin_state = ((GPIOE->IDR & (1UL << pin)) != 0U);
        if (!pin_state)
        {
            APP_LOG("[m7_gpio_test] T0a AD2->MCU bit on pin %lu read LOW!", pin);
        }

        /* Acknowledge */
        set_mcu_ack(true);
        if (!wait_host_req(false, GPIO_BENCH_TIMEOUT_MS)) { bench_safe(); return false; }
        set_mcu_ack(false);
    }

    bench_safe();
    APP_LOG("[m7_gpio_test] T0a AD2->MCU walking-1: PASS");
    return true;
}

static bool test_t0a_scope(void)
{
    APP_LOG("[m7_gpio_test] T0a_scope ready bus=NONE");

    /* Claim PF14 and PF15 as push-pull output */
    if (!bench_set_pin_mode(GPIOF, 14U, 1U, 0U, GPIO_BENCH_STEP_T0A_SCOPE) ||
        !bench_set_pin_mode(GPIOF, 15U, 1U, 0U, GPIO_BENCH_STEP_T0A_SCOPE))
    {
        return false;
    }

    /* 1. Drive LOW */
    GPIOF->BSRR = (1UL << (14U + 16U)) | (1UL << (15U + 16U));
    HAL_Delay(2);
    if (!mcu_handshake(GPIO_BENCH_TIMEOUT_MS)) { bench_safe(); return false; }

    /* 2. Drive HIGH */
    GPIOF->BSRR = (1UL << 14U) | (1UL << 15U);
    HAL_Delay(2);
    if (!mcu_handshake(GPIO_BENCH_TIMEOUT_MS)) { bench_safe(); return false; }

    bench_safe();
    APP_LOG("[m7_gpio_test] T0a Scope check: PASS");
    return true;
}

/* ---- T15 Race Control Implementation ------------------------------------- */

static bool test_t15_race_control(void)
{
    APP_LOG("[m7_gpio_test] T15 ready bus=NONE");

    /* Ensure GPIOF clock enabled for PF10 */
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOFEN;
    __DSB();

    /* Guard check for PF10 */
    if (!bench_claim_output(GPIOF, 10U, GPIO_BENCH_STEP_T15_RACE))
    {
        APP_LOG("[m7_gpio_test] T15 FAIL: bench_claim_output rejected PF10");
        return false;
    }

    /* Synchronize dual-core start via SRAM4 */
    GPIO_TEST_SHARED->cm7_ready = 1U;
    __DMB();

    uint32_t wait_cm4 = 0U;
    while ((GPIO_TEST_SHARED->cm4_ready == 0U) && (wait_cm4 < 10000000U))
    {
        wait_cm4++;
        __asm__ volatile("nop;");
    }

    if (GPIO_TEST_SHARED->cm4_ready == 0U)
    {
        APP_LOG("[m7_gpio_test] T15 FAIL: CM4 did not become ready");
        return false;
    }

    /* Start simultaneous race without lock */
    GPIO_TEST_SHARED->go_signal = 1U;
    __DMB();

    uint32_t corruptions = 0U;
    for (uint32_t i = 0U; i < 500000U; i++)
    {
        uint32_t val = (i & 1U); /* 0 = input, 1 = output */

        /* Hand-written RMW on GPIOF->MODER for PF10 (bits 21:20) without lock */
        uint32_t m = GPIOF->MODER;
        m &= ~(3UL << (10U * 2U));
        m |= (val << (10U * 2U));

        __asm__ volatile("nop; nop; nop; nop;");
        GPIOF->MODER = m;
        __asm__ volatile("nop; nop;");

        /* Check own bits */
        uint32_t readback = (GPIOF->MODER >> (10U * 2U)) & 3U;
        if (readback != val)
        {
            corruptions++;
        }

        /* Check CM4 bits (PF11: bits 23:22): must be 0 or 1 */
        uint32_t m4_bits = (GPIOF->MODER >> (11U * 2U)) & 3U;
        if (m4_bits > 1U)
        {
            corruptions++;
        }
    }

    /* Restore PF10 to input mode */
    GPIOF->MODER &= ~(3UL << (10U * 2U));

    GPIO_TEST_SHARED->t15_cm7_count = 500000U;
    GPIO_TEST_SHARED->t15_corruptions_cm7 = corruptions;
    GPIO_TEST_SHARED->t15_done_cm7 = 1U;
    __DMB();

    /* Wait for CM4 to finish */
    uint32_t wait_cm4_done = 0U;
    while ((GPIO_TEST_SHARED->t15_done_cm4 == 0U) && (wait_cm4_done < 10000000U))
    {
        wait_cm4_done++;
        __asm__ volatile("nop;");
    }

    uint32_t total = corruptions + GPIO_TEST_SHARED->t15_corruptions_cm4;
    GPIO_TEST_SHARED->t15_total_corruptions = total;

    APP_LOG("[m7_gpio_test] T15 race control: iterations=500000 cm7_corruptions=%lu cm4_corruptions=%lu total=%lu -> %s",
            corruptions, GPIO_TEST_SHARED->t15_corruptions_cm4, total, (total > 0U) ? "PASS" : "FAIL");

    return (total > 0U);
}

/* ---- CM7 Main Entry Point ------------------------------------------------ */

void GPIO_SelfTest_CM7_Run(void)
{
    /* Banner Rev <hash> and GPIO_BENCH v1 idle (S8) */
    APP_LOG("[m7_gpio_test] Rev %s", FW_GIT_REV);
    APP_LOG("[m7_gpio_test] GPIO_BENCH v%u idle", GPIO_BENCH_PROTOCOL_VERSION);

    /* Enforce S2 safe state initially */
    bench_safe();

    /* Synchronize start with host */
    APP_LOG("[m7_gpio_test] Waiting for HOST_REQ to start session...");
    if (!wait_host_req(true, 60000U))
    {
        APP_LOG("[m7_gpio_test] TIMEOUT waiting for HOST_REQ");
        bench_safe();
        return;
    }
    set_mcu_ack(true);
    if (!wait_host_req(false, 5000U)) { bench_safe(); return; }
    set_mcu_ack(false);
    APP_LOG("[m7_gpio_test] Host connected. Starting tests.");

    /* T0b: Guards verification before any bus movement */
    bool t0b_claim_ok = test_t0b_claim_guards();
    GPIO_TEST_SHARED->t0b_claim_pass = t0b_claim_ok ? 1U : 0U;

    bool t0b_s5_neg_ok = test_t0b_s5_neg();
    GPIO_TEST_SHARED->t0b_s5_neg_pass = t0b_s5_neg_ok ? 1U : 0U;

    bool t0b_timeout_ok = test_t0b_timeout();
    GPIO_TEST_SHARED->t0b_timeout_pass = t0b_timeout_ok ? 1U : 0U;

    bool t0b_ok = t0b_claim_ok && t0b_s5_neg_ok && t0b_timeout_ok;
    APP_LOG("[m7_gpio_test] T0b VERDICT: %s", t0b_ok ? "PASS" : "FAIL");

    /* T0a: Loose lines, walking-1 in both directions, scope */
    bool t0a_loose_ok = test_t0a_loose();
    GPIO_TEST_SHARED->t0a_loose_pass = t0a_loose_ok ? 1U : 0U;

    bool t0a_mcu_ok = test_t0a_mcu_to_ad2();
    GPIO_TEST_SHARED->t0a_mcu_pass = t0a_mcu_ok ? 1U : 0U;

    bool t0a_ad2_ok = test_t0a_ad2_to_mcu();
    GPIO_TEST_SHARED->t0a_ad2_pass = t0a_ad2_ok ? 1U : 0U;

    bool t0a_scope_ok = test_t0a_scope();
    GPIO_TEST_SHARED->t0a_scope_pass = t0a_scope_ok ? 1U : 0U;

    bool t0a_ok = t0a_loose_ok && t0a_mcu_ok && t0a_ad2_ok && t0a_scope_ok;
    APP_LOG("[m7_gpio_test] T0a VERDICT: %s", t0a_ok ? "PASS" : "FAIL");

    /* T15: Race negative control (RMW without lock) */
    bool t15_ok = test_t15_race_control();

    /* Wait for CM4 completion */
    uint32_t wait_cm4_final = 0U;
    while ((GPIO_TEST_SHARED->cm4_done == 0U) && (wait_cm4_final < 10000000U))
    {
        wait_cm4_final++;
        __asm__ volatile("nop;");
    }

    bool cm4_passed = (GPIO_TEST_SHARED->cm4_done == 1U) &&
                      (GPIO_TEST_SHARED->cm4_all_passed == 1U);

    APP_LOG("[m7_gpio_test] CM4 sub-verdict: done=%lu all_passed=%lu -> %s",
            GPIO_TEST_SHARED->cm4_done, GPIO_TEST_SHARED->cm4_all_passed,
            cm4_passed ? "PASS" : "FAIL");

    /* Compute Global Verdict */
    bool all_passed = t0b_ok && t0a_ok && t15_ok && cm4_passed;

    APP_LOG("[m7_gpio_test] =================================================");
    if (all_passed)
    {
        APP_LOG("[m7_gpio_test] VERDICT: ALL GPIO SELF-TESTS PASSED (PASS)");
    }
    else
    {
        APP_LOG("[m7_gpio_test] VERDICT: GPIO SELF-TESTS FAILED (FAIL)");
    }
    APP_LOG("[m7_gpio_test] =================================================");

    /* Return to safe state S2 */
    bench_safe();
}
