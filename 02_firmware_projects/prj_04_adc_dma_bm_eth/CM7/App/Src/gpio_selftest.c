/**
  ******************************************************************************
  * @file    gpio_selftest.c
  * @brief   GPIO driver self-test implementation for Cortex-M7 (prj_04).
  *          Coordinated dual-core tests, bench safety guards S1-S10,
  *          T0a loose line checks, T0b guard verifications, and T15 race control.
  *          Implements M1 (step-known guard), M2 (bench_safe on error, all-or-nothing),
  *          m1 (checked claims), B2 (calculated verdicts), and B1 (rebuilt T15).
  ******************************************************************************
  */

#include "gpio_selftest.h"
#include "gpio_bench_protocol.h"
#include "shared_memory.h"
#include "app_log.h"
#include "stm32h7xx.h"
#include <string.h>
#include <stdbool.h>

/* ---- Bench Step Permission Table (M1) ------------------------------------ */
typedef struct {
    uint8_t     step_id;
    uint8_t     bus_owner;
    const char *bus_name;
    uint16_t    gpioa_allowed_mask; /* bit 8 = PA8 */
    uint16_t    gpioe_allowed_mask; /* bits 5, 6, 7, 8..15 */
    uint16_t    gpiof_allowed_mask; /* bits 6..11, 14, 15 */
} BenchStepPerm_t;

static const BenchStepPerm_t k_step_perms[] = {
    { GPIO_BENCH_STEP_T15_RACE,       GPIO_BENCH_BUS_NONE, "NONE", 0U,         0U,                                                               (1U << 10) },
    { GPIO_BENCH_STEP_IDLE,           GPIO_BENCH_BUS_NONE, "NONE", 0U,         (1U << 7),                                                        0U },
    { GPIO_BENCH_STEP_T0B_GUARDS,     GPIO_BENCH_BUS_NONE, "NONE", 0U,         (1U << 7),                                                        0U },
    { GPIO_BENCH_STEP_T0B_S5_NEG,     GPIO_BENCH_BUS_MCU,  "MCU",  0U,         (1U << 7) | 0xFF00U,                                              0U },
    { GPIO_BENCH_STEP_T0B_TIMEOUT,    GPIO_BENCH_BUS_NONE, "NONE", 0U,         (1U << 7),                                                        0U },
    { GPIO_BENCH_STEP_T0A_LOOSE,      GPIO_BENCH_BUS_NONE, "NONE", 0U,         (1U << 7),                                                        0U },
    { GPIO_BENCH_STEP_T0A_HANDSHAKE,  GPIO_BENCH_BUS_NONE, "NONE", 0U,         (1U << 7),                                                        0U },
    { GPIO_BENCH_STEP_T0A_MCU_TO_AD2, GPIO_BENCH_BUS_MCU,  "MCU",  (1U << 8), (1U << 5) | (1U << 6) | (1U << 7) | 0xFF00U,                     0U },
    { GPIO_BENCH_STEP_T0A_AD2_TO_MCU, GPIO_BENCH_BUS_AD2,  "AD2",  0U,         (1U << 7),                                                        0U },
    { GPIO_BENCH_STEP_T0A_SCOPE,      GPIO_BENCH_BUS_NONE, "NONE", 0U,         (1U << 7),                                                        (1U << 14) | (1U << 15) },
    { GPIO_BENCH_STEP_DONE,           GPIO_BENCH_BUS_NONE, "NONE", 0U,         (1U << 7),                                                        0U },
};

static uint8_t s_current_step_id = GPIO_BENCH_STEP_T15_RACE;

static const BenchStepPerm_t *bench_get_current_perm(void)
{
    for (size_t i = 0; i < sizeof(k_step_perms) / sizeof(k_step_perms[0]); i++)
    {
        if (k_step_perms[i].step_id == s_current_step_id)
        {
            return &k_step_perms[i];
        }
    }
    return NULL;
}

static void bench_enter_step(uint8_t step_id)
{
    s_current_step_id = step_id;
    const BenchStepPerm_t *perm = bench_get_current_perm();
    const char *bus_str = (perm != NULL) ? perm->bus_name : "NONE";
    APP_LOG("[m7_gpio_test] STEP id=%u bus=%s v=%u",
            (unsigned int)step_id, bus_str, (unsigned int)GPIO_BENCH_PROTOCOL_VERSION);
    if (GPIO_TEST_SHARED != NULL)
    {
        GPIO_TEST_SHARED->current_step = step_id;
        GPIO_TEST_SHARED->bus_owner = (perm != NULL) ? perm->bus_owner : GPIO_BENCH_BUS_NONE;
    }
}

/**
 * @brief Safety guard S4: Bench claim check for Cortex-M7 (M1).
 *        No step parameter accepted; reads s_current_step_id.
 */
static bool bench_claim_output(GPIO_TypeDef *port, uint32_t pin)
{
    if (pin > 15U)
    {
        return false;
    }

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

    const BenchStepPerm_t *perm = bench_get_current_perm();
    if (perm == NULL)
    {
        return false;
    }

    uint16_t pin_mask = (uint16_t)(1U << pin);
    if (port == GPIOA)
    {
        return (perm->gpioa_allowed_mask & pin_mask) != 0U;
    }
    else if (port == GPIOE)
    {
        return (perm->gpioe_allowed_mask & pin_mask) != 0U;
    }
    else if (port == GPIOF)
    {
        return (perm->gpiof_allowed_mask & pin_mask) != 0U;
    }

    return false;
}

/**
 * @brief Helper to configure a mask of pins with all-or-nothing check (M2, m1).
 */
static bool bench_claim_pins(GPIO_TypeDef *port, uint32_t pin_mask, uint32_t mode, uint32_t otype)
{
    /* 1. All-or-nothing check: validate every pin before any register access */
    for (uint32_t pin = 0U; pin < 16U; pin++)
    {
        if ((pin_mask & (1U << pin)) != 0U)
        {
            if (mode == 1U || mode == 2U) /* Output or AF */
            {
                if (!bench_claim_output(port, pin))
                {
                    return false;
                }
            }
            else
            {
                if (port == GPIOE && (pin == 0U || pin == 1U))
                {
                    return false;
                }
            }
        }
    }

    /* 2. All pins valid: apply configuration */
    for (uint32_t pin = 0U; pin < 16U; pin++)
    {
        if ((pin_mask & (1U << pin)) != 0U)
        {
            uint32_t shift2 = pin * 2U;
            port->MODER = (port->MODER & ~(3UL << shift2)) | ((mode & 3UL) << shift2);
            if (mode == 1U || mode == 2U)
            {
                port->OTYPER = (port->OTYPER & ~(1UL << pin)) | ((otype & 1UL) << pin);
            }
        }
    }
    return true;
}

static bool bench_set_pin_mode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t otype)
{
    return bench_claim_pins(port, (1U << pin), mode, otype);
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
    uint32_t mask_pe2_to_pe6 = 0x00003FF0UL;  /* pins 2, 3, 4, 5, 6 */
    uint32_t mask_pe8_to_pe15 = 0xFFFF0000UL; /* pins 8..15 */
    uint32_t clear_mask_e = mask_pe2_to_pe6 | mask_pe8_to_pe15;

    GPIOE->MODER &= ~clear_mask_e;
    GPIOE->PUPDR &= ~clear_mask_e;

    /* PE2 (HOST_REQ): pull-down so it stays cleanly 0 when AD2 is not driving */
    GPIOE->PUPDR |= (2UL << (2U * 2U));

    /* PE7 (MCU_ACK) is Solo MCU: set to OUTPUT LOW (01) */
    /* Temporarily allow PE7 configuration */
    GPIOE->BSRR = (1UL << (7U + 16U)); /* ODR low first (C11) */
    GPIOE->OTYPER &= ~(1UL << 7U);
    uint32_t moder_e = GPIOE->MODER;
    moder_e &= ~(3UL << (7U * 2U));
    moder_e |= (1UL << (7U * 2U));
    GPIOE->MODER = moder_e;

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
 * @brief R-B1 HS_MCU Handshake Primitive:
 *        MCU asserts MCU_ACK=1 -> waits HOST_REQ=1 -> drops MCU_ACK=0 -> waits HOST_REQ=0.
 *        10.0s timeout per phase. On timeout: bench_safe(), announces abort, returns false.
 */
static bool hs_mcu(void)
{
    /* Phase 1: MCU asserts MCU_ACK = 1 */
    set_mcu_ack(true);

    /* Phase 2: Wait for HOST_REQ = 1 */
    if (!wait_host_req(true, GPIO_BENCH_TIMEOUT_MS))
    {
        set_mcu_ack(false);
        bench_safe();
        APP_LOG("[m7_gpio_test] ABORT step=%u reason=timeout", (unsigned int)s_current_step_id);
        return false;
    }

    /* Phase 3: MCU drops MCU_ACK = 0 */
    set_mcu_ack(false);

    /* Phase 4: Wait for HOST_REQ = 0 */
    if (!wait_host_req(false, GPIO_BENCH_TIMEOUT_MS))
    {
        bench_safe();
        APP_LOG("[m7_gpio_test] ABORT step=%u reason=timeout", (unsigned int)s_current_step_id);
        return false;
    }

    return true;
}

/**
 * @brief R-B1 HS_HOST Handshake Primitive:
 *        Waits HOST_REQ=1 -> samples IDR (optional) -> asserts MCU_ACK=1 -> waits HOST_REQ=0 -> drops MCU_ACK=0.
 *        Timeout per phase. On timeout: bench_safe(), announces abort, returns false.
 */
static bool hs_host_sample(uint32_t timeout_ms, uint32_t *p_idr)
{
    /* Phase 1: Wait for HOST_REQ = 1 */
    if (!wait_host_req(true, timeout_ms))
    {
        bench_safe();
        APP_LOG("[m7_gpio_test] ABORT step=%u reason=timeout", (unsigned int)s_current_step_id);
        return false;
    }

    if (p_idr != NULL)
    {
        *p_idr = GPIOE->IDR;
    }

    /* Phase 2: MCU asserts MCU_ACK = 1 */
    set_mcu_ack(true);

    /* Phase 3: Wait for HOST_REQ = 0 */
    if (!wait_host_req(false, GPIO_BENCH_TIMEOUT_MS))
    {
        set_mcu_ack(false);
        bench_safe();
        APP_LOG("[m7_gpio_test] ABORT step=%u reason=timeout", (unsigned int)s_current_step_id);
        return false;
    }

    /* Phase 4: MCU drops MCU_ACK = 0 */
    set_mcu_ack(false);

    return true;
}

static bool hs_host(uint32_t timeout_ms)
{
    return hs_host_sample(timeout_ms, NULL);
}

static bool wait_var_cycles(volatile uint32_t *var, uint32_t expected, uint32_t timeout_cycles)
{
    uint32_t start = DWT->CYCCNT;
    while (*var != expected)
    {
        if ((DWT->CYCCNT - start) >= timeout_cycles)
        {
            return false;
        }
    }
    return true;
}

/* ---- T15 Race Control Implementation (B1) -------------------------------- */

static bool test_t15_race_control(void)
{
    bool pass = true;

    /* Ensure GPIOF clock enabled for PF10 and PF11 */
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOFEN;
    __DSB();

    /* B1: Explicitly set PF10 and PF11 to INPUT mode (00), clearing reset analog state (11) */
    GPIOF->MODER &= ~((3UL << (10U * 2U)) | (3UL << (11U * 2U)));
    GPIOF->PUPDR &= ~((3UL << (10U * 2U)) | (3UL << (11U * 2U)));

    bench_enter_step(GPIO_BENCH_STEP_T15_RACE);

    uint32_t session = GPIO_TEST_SHARED->session_id;

    /* Guard check for PF10 */
    if (!bench_claim_output(GPIOF, 10U))
    {
        APP_LOG("[m7_gpio_test] T15 FAIL: bench_claim_output rejected PF10");
        pass = false;
        goto exit;
    }

    /* Wait for CM4 ready with matching session_id (timeout: 5s = 2,000,000,000 cycles @ 400 MHz) */
    if (!wait_var_cycles(&GPIO_TEST_SHARED->cm4_ready, session, 2000000000UL))
    {
        APP_LOG("[m7_gpio_test] T15 FAIL: TIMEOUT waiting for CM4 ready");
        pass = false;
        goto exit;
    }

    /* ---- Phase (a): CM7 solo (50,000 iterations) -------------------------- */
    GPIO_TEST_SHARED->t15_phase = 1U;
    __DMB();

    uint32_t corruptions_a = 0U;
    for (uint32_t i = 0U; i < 50000U; i++)
    {
        uint32_t val = (i & 1U);
        uint32_t m = GPIOF->MODER;
        m &= ~(3UL << (10U * 2U));
        m |= (val << (10U * 2U));
        GPIOF->MODER = m;

        uint32_t readback = (GPIOF->MODER >> (10U * 2U)) & 3U;
        if (readback != val)
        {
            corruptions_a++;
        }
        uint32_t m4_bits = (GPIOF->MODER >> (11U * 2U)) & 3U;
        if (m4_bits > 1U)
        {
            corruptions_a++;
        }
    }
    GPIO_TEST_SHARED->t15_corruptions_phase_a = corruptions_a;

    /* ---- Phase (b): CM4 solo (50,000 iterations) -------------------------- */
    GPIO_TEST_SHARED->t15_phase = 2U;
    GPIO_TEST_SHARED->go_signal = session;
    __DMB();

    /* Wait for CM4 to complete phase (b) */
    if (!wait_var_cycles(&GPIO_TEST_SHARED->cm4_ready, session + 1U, 2000000000UL))
    {
        APP_LOG("[m7_gpio_test] T15 FAIL: TIMEOUT waiting for CM4 phase (b)");
        pass = false;
        goto exit;
    }
    uint32_t corruptions_b = GPIO_TEST_SHARED->t15_corruptions_phase_b;

    /* ---- Phase (c): Concurrent run (500,000 iterations) -------------------- */
    GPIO_TEST_SHARED->t15_phase = 3U;
    GPIO_TEST_SHARED->in_loop_cm7 = 1U;
    GPIO_TEST_SHARED->go_signal = session + 1U;
    __DMB();

    uint32_t corruptions_c = 0U;
    uint32_t overlap_cm7 = 0U;

    for (uint32_t i = 0U; i < 500000U; i++)
    {
        if (GPIO_TEST_SHARED->in_loop_cm4 != 0U)
        {
            overlap_cm7++;
        }

        uint32_t val = (i & 1U);
        uint32_t m = GPIOF->MODER;
        m &= ~(3UL << (10U * 2U));
        m |= (val << (10U * 2U));

        __asm__ volatile("nop; nop; nop; nop;");
        GPIOF->MODER = m;
        __asm__ volatile("nop; nop;");

        uint32_t readback = (GPIOF->MODER >> (10U * 2U)) & 3U;
        if (readback != val)
        {
            corruptions_c++;
        }

        uint32_t m4_bits = (GPIOF->MODER >> (11U * 2U)) & 3U;
        if (m4_bits > 1U)
        {
            corruptions_c++;
        }
    }

    GPIO_TEST_SHARED->in_loop_cm7 = 0U;
    GPIO_TEST_SHARED->t15_cm7_corruptions_c = corruptions_c;
    GPIO_TEST_SHARED->t15_cm7_overlap_cnt = overlap_cm7;
    GPIO_TEST_SHARED->t15_total_calls_cm7 = 500000U;
    __DMB();

    /* Wait for CM4 to complete phase (c) */
    wait_var_cycles(&GPIO_TEST_SHARED->in_loop_cm4, 0U, 2000000000UL);

    /* Restore PF10 and PF11 to input mode (00) */
    GPIOF->MODER &= ~((3UL << (10U * 2U)) | (3UL << (11U * 2U)));

    uint32_t overlap_cm4 = GPIO_TEST_SHARED->t15_cm4_overlap_cnt;
    uint32_t cm4_c = GPIO_TEST_SHARED->t15_cm4_corruptions_c;
    uint32_t total_c = corruptions_c + cm4_c;
    GPIO_TEST_SHARED->t15_corruptions_phase_c = total_c;

    APP_LOG("[m7_gpio_test] INFO T15 phase (a) CM7 solo: corruptions=%lu (expected 0)", (unsigned long)corruptions_a);
    APP_LOG("[m7_gpio_test] INFO T15 phase (b) CM4 solo: corruptions=%lu (expected 0)", (unsigned long)corruptions_b);
    APP_LOG("[m7_gpio_test] INFO T15 phase (c) concurrent: total_corruptions=%lu (CM7=%lu, CM4=%lu, overlap CM7=%lu, CM4=%lu)",
            (unsigned long)total_c, (unsigned long)corruptions_c, (unsigned long)cm4_c, (unsigned long)overlap_cm7, (unsigned long)overlap_cm4);

    if (corruptions_a != 0U || corruptions_b != 0U || total_c == 0U || overlap_cm7 == 0U || overlap_cm4 == 0U)
    {
        pass = false;
    }

    APP_LOG("[m7_gpio_test] T15 VERDICT: %s", pass ? "PASS" : "FAIL");

exit:
    if (!pass)
    {
        bench_safe();
    }
    return pass;
}

/* ---- T0b Test Implementations -------------------------------------------- */

static bool test_t0b_claim_guards(void)
{
    bool pass = true;
    bench_enter_step(GPIO_BENCH_STEP_T0B_GUARDS);

    uint32_t moder_before = GPIOE->MODER;

    /* (1) PE0 and PE1 rejection unconditionally */
    if (bench_claim_output(GPIOE, 0U)) pass = false;
    if (bench_claim_output(GPIOE, 1U)) pass = false;

    /* (2) PE2, PE3, PE4 rejection unconditionally */
    if (bench_claim_output(GPIOE, 2U)) pass = false;
    if (bench_claim_output(GPIOE, 3U)) pass = false;
    if (bench_claim_output(GPIOE, 4U)) pass = false;

    /* (3) Bus lines PE8..PE15 rejection when step is not bus=MCU */
    for (uint32_t pin = 8U; pin <= 15U; pin++)
    {
        if (bench_claim_output(GPIOE, pin)) pass = false;
    }

    /* (4) PA8 rejection in T0B_GUARDS step */
    if (bench_claim_output(GPIOA, 8U)) pass = false;

    /* (5) Bus rejection when step has bus_owner=AD2 */
    s_current_step_id = GPIO_BENCH_STEP_T0A_AD2_TO_MCU;
    if (bench_claim_output(GPIOE, 8U)) pass = false;
    s_current_step_id = GPIO_BENCH_STEP_T0B_GUARDS;

    /* (6) PA8 accepted in T0A_MCU_TO_AD2 step */
    s_current_step_id = GPIO_BENCH_STEP_T0A_MCU_TO_AD2;
    if (!bench_claim_output(GPIOA, 8U)) pass = false;
    s_current_step_id = GPIO_BENCH_STEP_T0B_GUARDS;

    /* (7) All-or-nothing check in bench_claim_pins (M2, m1) */
    uint32_t moder_snap = GPIOE->MODER;
    uint32_t otyper_snap = GPIOE->OTYPER;
    uint32_t afr0_snap = GPIOE->AFR[0];
    uint32_t afr1_snap = GPIOE->AFR[1];

    /* Mask with PE5 (allowed in step 8) and PE4 (forbidden): must fail and touch nothing */
    s_current_step_id = GPIO_BENCH_STEP_T0A_MCU_TO_AD2;
    uint32_t test_mask = (1U << 5U) | (1U << 4U);
    if (bench_claim_pins(GPIOE, test_mask, 1U, 0U))
    {
        pass = false; /* Must fail */
    }
    if (GPIOE->MODER != moder_snap || GPIOE->OTYPER != otyper_snap ||
        GPIOE->AFR[0] != afr0_snap || GPIOE->AFR[1] != afr1_snap)
    {
        pass = false; /* Registers were modified despite rejection! */
    }
    s_current_step_id = GPIO_BENCH_STEP_T0B_GUARDS;

    /* Verify GPIOE->MODER was untouched overall */
    if (GPIOE->MODER != moder_before)
    {
        pass = false;
    }

    APP_LOG("[m7_gpio_test] T0b_b guards verification: %s", pass ? "PASS" : "FAIL");

    if (!pass)
    {
        bench_safe();
        return false;
    }
    if (!hs_mcu())
    {
        return false;
    }
    return true;
}

static bool test_t0b_s5_neg(void)
{
    bool pass = true;
    bench_enter_step(GPIO_BENCH_STEP_T0B_S5_NEG);

    /* MCU intentionally claims PE8..PE15 for negative control step */
    if (!bench_claim_pins(GPIOE, 0xFF00UL, 1U, 0U))
    {
        bench_safe();
        return false;
    }

    /* Drive 0x5A on bus lines PE8..PE15 (0x5A00) while AD2 is in input (no contention) */
    uint32_t set_mask = 0x5A00UL;
    uint32_t reset_mask = 0xA500UL;
    GPIOE->BSRR = (reset_mask << 16U) | set_mask;

    APP_LOG("[m7_gpio_test] BUSCHK up drive=0x5A");

    /* Host executes s5_verify_released(), detects driven bus (0x5A != 0xFF), aborts safely, and synchronizes */
    if (!hs_mcu())
    {
        return false;
    }

    /* Release bus back to safe input */
    bench_safe();

    /* Verify all wired pins are input */
    bool all_inputs = (((GPIOE->MODER & 0xFFFF3FF0UL) == 0U) &&
                       ((GPIOF->MODER & ((3UL << 28U) | (3UL << 30U))) == 0U) &&
                       ((GPIOA->MODER & (3UL << 16U)) == 0U));

    if (!all_inputs)
    {
        pass = false;
    }

    APP_LOG("[m7_gpio_test] T0b_c safe state verified: all wired pins input");

    if (!hs_mcu())
    {
        return false;
    }

    return pass;
}

static bool test_t0b_timeout(void)
{
    bool pass = true;
    bench_enter_step(GPIO_BENCH_STEP_T0B_TIMEOUT);

    /* Deliberately wait for HOST_REQ which host will NOT send for 10s */
    bool received = wait_host_req(true, GPIO_BENCH_TIMEOUT_MS);

    /* Must time out (received == false) */
    if (received)
    {
        APP_LOG("[m7_gpio_test] T0b(d) FAIL: spurious HOST_REQ received!");
        bench_safe();
        return false;
    }

    /* S2 safe state must be enforced on timeout */
    bench_safe();

    /* Verify all wired test pins are inputs */
    bool all_inputs = (((GPIOE->MODER & 0xFFFF3FF0UL) == 0U) &&
                       ((GPIOF->MODER & ((3UL << 28U) | (3UL << 30U))) == 0U) &&
                       ((GPIOA->MODER & (3UL << 16U)) == 0U));

    if (!all_inputs)
    {
        pass = false;
    }

    APP_LOG("[m7_gpio_test] timeout expired -> bench_safe() invoked (%s)", pass ? "PASS" : "FAIL");

    return pass;
}

/* ---- T0a Test Implementations -------------------------------------------- */

static bool test_t0a_loose(void)
{
    bench_enter_step(GPIO_BENCH_STEP_T0A_LOOSE);

    /* 1. Pull-up check on 13 test lines (PE7 MCU_ACK stays OUTPUT, PE2 HOST_REQ stays INPUT) */
    uint32_t pupdr_e = GPIOE->PUPDR;
    pupdr_e &= ~0xFFFF3F00UL;
    pupdr_e |= (0x55550000UL | (1UL << (5U * 2U)) | (1UL << (6U * 2U)) | (1UL << (3U * 2U)) | (1UL << (4U * 2U)));
    GPIOE->PUPDR = pupdr_e;

    GPIOA->PUPDR &= ~(3UL << 16U);
    GPIOA->PUPDR |= (1UL << 16U);

    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    APP_LOG("[m7_gpio_test] BUSCHK up");
    if (!hs_mcu())
    {
        return false;
    }

    /* 2. Pull-down check */
    pupdr_e = GPIOE->PUPDR;
    pupdr_e &= ~0xFFFF3F00UL;
    pupdr_e |= (0xAAAA0000UL | (2UL << (5U * 2U)) | (2UL << (6U * 2U)) | (2UL << (3U * 2U)) | (2UL << (4U * 2U)));
    GPIOE->PUPDR = pupdr_e;

    GPIOA->PUPDR &= ~(3UL << 16U);
    GPIOA->PUPDR |= (2UL << 16U);

    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    APP_LOG("[m7_gpio_test] BUSCHK down");
    if (!hs_mcu())
    {
        return false;
    }

    bench_safe();
    APP_LOG("[m7_gpio_test] T0a_loose: DONE (host verdict)");
    return true;
}

static bool test_t0a_handshake(void)
{
    bench_enter_step(GPIO_BENCH_STEP_T0A_HANDSHAKE);

    /* 1. HS_HOST: MCU validates PE2 (HOST_REQ) HIGH then LOW */
    if (!hs_host(GPIO_BENCH_TIMEOUT_MS))
    {
        return false;
    }
    APP_LOG("[m7_gpio_test] Handshake PE2: OK");

    /* 2. HS_MCU: Host validates PE7 (MCU_ACK) HIGH then LOW */
    if (!hs_mcu())
    {
        return false;
    }

    APP_LOG("[m7_gpio_test] T0a_handshake: DONE (host verdict)");
    return true;
}

static bool test_t0a_mcu_to_ad2(void)
{
    bench_enter_step(GPIO_BENCH_STEP_T0A_MCU_TO_AD2);

    /* 1. Pull-up check */
    uint32_t pupdr_e = GPIOE->PUPDR;
    pupdr_e &= ~0xFFFF0000UL;
    pupdr_e |= 0x55550000UL;
    GPIOE->PUPDR = pupdr_e;
    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    APP_LOG("[m7_gpio_test] BUSCHK up");
    if (!hs_mcu()) { return false; }

    /* 2. Pull-down check */
    pupdr_e = GPIOE->PUPDR;
    pupdr_e &= ~0xFFFF0000UL;
    pupdr_e |= 0xAAAA0000UL;
    GPIOE->PUPDR = pupdr_e;
    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    APP_LOG("[m7_gpio_test] BUSCHK down");
    if (!hs_mcu()) { return false; }
    GPIOE->PUPDR &= ~0xFFFF0000UL;

    /* Configure Solo MCU lines as push-pull output: PE5, PE6, PE7, PA8 */
    if (!bench_set_pin_mode(GPIOE, 5U, 1U, 0U) ||
        !bench_set_pin_mode(GPIOE, 6U, 1U, 0U) ||
        !bench_set_pin_mode(GPIOE, 7U, 1U, 0U) ||
        !bench_set_pin_mode(GPIOA, 8U, 1U, 0U))
    {
        bench_safe();
        return false;
    }

    /* Configure bus PE8..PE15 as push-pull output */
    if (!bench_claim_pins(GPIOE, 0xFF00UL, 1U, 0U))
    {
        bench_safe();
        return false;
    }

    /* Ensure all outputs initially low */
    GPIOE->BSRR = (0xFFE0UL << 16U);
    GPIOA->BSRR = (1UL << (8U + 16U));

    /* Walking-1 across 11 test lines:
       PE5 (DIO9), PE6 (DIO11), PE8..PE15 (DIO0..DIO7), PA8 (DIO14)
       Order: [DIO9, DIO11, DIO0..DIO7, DIO14] */
    uint32_t test_pins[] = { 5U, 6U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U };
    for (uint32_t i = 0U; i < sizeof(test_pins) / sizeof(test_pins[0]); i++)
    {
        uint32_t pin = test_pins[i];
        GPIOE->BSRR = (1UL << pin);
        HAL_Delay(1);

        if (!hs_mcu())
        {
            return false;
        }

        GPIOE->BSRR = (1UL << (pin + 16U));
    }

    /* Test PA8 (DIO14) */
    GPIOA->BSRR = (1UL << 8U);
    HAL_Delay(1);
    if (!hs_mcu())
    {
        return false;
    }
    GPIOA->BSRR = (1UL << (8U + 16U));

    /* MCU->AD2 Negative Control: ODR set with pins in input with pull-down */
    GPIOE->MODER &= ~0xFFFF3FE0UL; /* PE5..PE15 input */
    GPIOE->PUPDR = (GPIOE->PUPDR & ~0xFFFF3FE0UL) | 0xAAAA2A80UL; /* pull-down */
    GPIOA->MODER &= ~(3UL << 16U);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3UL << 16U)) | (2UL << 16U);

    GPIOE->ODR |= 0xFF60UL;
    GPIOA->ODR |= (1UL << 8U);

    APP_LOG("[m7_gpio_test] T0a_mcu control ready");
    if (!hs_mcu())
    {
        return false;
    }

    bench_safe();
    APP_LOG("[m7_gpio_test] T0a_mcu: DONE (host verdict)");
    return true;
}

static bool test_t0a_ad2_to_mcu(void)
{
    bool pass = true;
    bench_enter_step(GPIO_BENCH_STEP_T0A_AD2_TO_MCU);

    /* S5 check: exchange BUSCHK up and down */
    uint32_t pupdr_e = GPIOE->PUPDR;
    pupdr_e &= ~0xFFFF0000UL;
    pupdr_e |= 0x55550000UL;
    GPIOE->PUPDR = pupdr_e;
    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    APP_LOG("[m7_gpio_test] BUSCHK up");
    if (!hs_mcu()) { return false; }

    pupdr_e = GPIOE->PUPDR;
    pupdr_e &= ~0xFFFF0000UL;
    pupdr_e |= 0xAAAA0000UL;
    GPIOE->PUPDR = pupdr_e;
    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    APP_LOG("[m7_gpio_test] BUSCHK down");
    if (!hs_mcu()) { return false; }

    /* Configure input pull-down on PE3, PE4, PE8..PE15 */
    pupdr_e = GPIOE->PUPDR;
    pupdr_e &= ~0xFFFF03C0UL;
    pupdr_e |= (0xAAAA0000UL | (2UL << (3U * 2U)) | (2UL << (4U * 2U)));
    GPIOE->PUPDR = pupdr_e;

    /* Signal ready to host via hs_mcu */
    APP_LOG("[m7_gpio_test] T0a_ad2 ready");
    if (!hs_mcu()) { return false; }

    /* Host walks 10 bits: PE3 (DIO8), PE4 (DIO10), PE8..PE15 (DIO0..DIO7) */
    uint32_t dio_order[] = { 8U, 10U, 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U };
    uint32_t pin_order[] = { 3U, 4U,  8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U };
    uint32_t observed_mask = (1UL << 3U) | (1UL << 4U) | 0xFF00UL;

    for (size_t i = 0; i < sizeof(dio_order) / sizeof(dio_order[0]); i++)
    {
        uint32_t dio_b = dio_order[i];
        uint32_t pin = pin_order[i];

        uint32_t raw_idr = 0U;
        if (!hs_host_sample(GPIO_BENCH_TIMEOUT_MS, &raw_idr))
        {
            return false;
        }

        uint32_t idr = raw_idr & observed_mask;
        uint32_t exp = (1UL << pin);
        bool bit_ok = (idr == exp);
        if (!bit_ok)
        {
            pass = false;
        }

        APP_LOG("[m7_gpio_test] T0a_ad2 bit=%u idr=0x%04lX exp=0x%04lX %s",
                (unsigned int)dio_b, (unsigned long)idr, (unsigned long)exp, bit_ok ? "OK" : "FAIL");
    }

    /* AD2->MCU Negative Control: MCU bus in analog, AD2 drives 0xFF */
    GPIOE->PUPDR &= ~0xFFFF0000UL;
    GPIOE->MODER |= 0xFFFF0000UL; /* bits 8..15 analog (11) */

    APP_LOG("[m7_gpio_test] T0a_ad2 analog control ready");
    uint32_t raw_analog = 0U;
    if (!hs_host_sample(GPIO_BENCH_TIMEOUT_MS, &raw_analog))
    {
        return false;
    }

    uint32_t idr_analog = raw_analog & 0xFF00UL;
    bool analog_ok = (idr_analog == 0U);
    if (!analog_ok)
    {
        pass = false;
    }
    APP_LOG("[m7_gpio_test] T0a_ad2 analog control result: %s", analog_ok ? "OK" : "FAIL");

    bench_safe();
    APP_LOG("[m7_gpio_test] T0a_ad2: %s", pass ? "PASS" : "FAIL");
    return pass;
}

static bool test_t0a_scope(void)
{
    bench_enter_step(GPIO_BENCH_STEP_T0A_SCOPE);

    /* Claim PF14 and PF15 as push-pull output */
    if (!bench_set_pin_mode(GPIOF, 14U, 1U, 0U) ||
        !bench_set_pin_mode(GPIOF, 15U, 1U, 0U))
    {
        bench_safe();
        return false;
    }

    /* 1. Drive LOW */
    GPIOF->BSRR = (1UL << (14U + 16U)) | (1UL << (15U + 16U));
    HAL_Delay(2);
    APP_LOG("[m7_gpio_test] Scope LOW ready");
    if (!hs_mcu()) { return false; }

    /* 2. Drive HIGH */
    GPIOF->BSRR = (1UL << 14U) | (1UL << 15U);
    HAL_Delay(2);
    APP_LOG("[m7_gpio_test] Scope HIGH ready");
    if (!hs_mcu()) { return false; }

    bench_safe();
    APP_LOG("[m7_gpio_test] T0a_scope: DONE (host verdict)");
    return true;
}

/* ---- CM7 Main Entry Point ------------------------------------------------ */

void GPIO_SelfTest_CM7_Run(void)
{
    /* 1. S8: Print Banner Rev <hash> */
    APP_LOG("[m7_gpio_test] Rev %s", FW_GIT_REV);

    /* Enforce S2 safe state initially */
    bench_safe();

    /* 2. B1: Run T15 dual-core race control FIRST (before host synchronization) */
    bool t15_pass = test_t15_race_control();
    if (!t15_pass)
    {
        goto test_failed;
    }

    /* 3. S8: Announce idle state and wait for host session */
    bench_enter_step(GPIO_BENCH_STEP_IDLE);
    APP_LOG("[m7_gpio_test] GPIO_BENCH v%u idle", (unsigned int)GPIO_BENCH_PROTOCOL_VERSION);
    APP_LOG("[m7_gpio_test] INFO Waiting for HOST_REQ to start session...");

    if (!hs_host(60000U))
    {
        goto test_failed;
    }

    /* 4. Execute test suite sequentially */
    if (!test_t0b_claim_guards()) goto test_failed;
    if (!test_t0b_s5_neg())        goto test_failed;
    if (!test_t0b_timeout())       goto test_failed;
    if (!test_t0a_loose())         goto test_failed;
    if (!test_t0a_handshake())     goto test_failed;
    if (!test_t0a_mcu_to_ad2())    goto test_failed;
    if (!test_t0a_ad2_to_mcu())    goto test_failed;
    if (!test_t0a_scope())         goto test_failed;

    bench_enter_step(GPIO_BENCH_STEP_DONE);

    /* Wait for CM4 logs and verdict */
    uint32_t cm4_wait = 0U;
    while ((GPIO_TEST_SHARED->cm4_done == 0U) && (cm4_wait < 5000U))
    {
        HAL_Delay(1);
        cm4_wait++;
    }

    bool cm4_passed = (GPIO_TEST_SHARED->cm4_all_passed != 0U);

    if (cm4_passed)
    {
        APP_LOG("[m7_gpio_test] VERDICT: ALL GPIO SELF-TESTS PASSED");
    }
    else
    {
        APP_LOG("[m7_gpio_test] VERDICT: GPIO SELF-TESTS FAILED");
    }

    bench_safe();
    return;

test_failed:
    bench_safe();
    APP_LOG("[m7_gpio_test] VERDICT: GPIO SELF-TESTS FAILED");
}
