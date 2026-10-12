/**
  ******************************************************************************
  * @file    gpio_selftest.c
  * @brief   GPIO driver self-test implementation for Cortex-M7 (prj_04).
  *          Coordinated dual-core tests, bench safety guards S1-S10,
  *          T0a loose line checks, T0b guard verifications, T15 race
  *          (F0 control plus the gpio_bm phases of F1a), T1, T2, B1 and T16.
  *          Implements M1 (step-known guard), M2 (bench_safe on error, all-or-nothing),
  *          m1 (checked claims), B2 (calculated verdicts), and B1 (rebuilt T15).
  ******************************************************************************
  */

#include "gpio_selftest.h"
#include "gpio_bench_protocol.h"
#include "shared_memory.h"
#include "app_log.h"
#include "stm32h7xx.h"
#include "stm32h7xx_hal.h"
#include "gpio_bm.h"
#include "hsem_bm.h"
#include <string.h>
#include <stdbool.h>

/* ---- Bench Step Permission Table (M1, R4-m2) ------------------------------ */
/* Lines other than the bus. Whether the bus PE8..PE15 may be claimed is not
   listed here: it comes only from the bus owner of GPIO_BENCH_STEPS_TABLE
   (bench_step_allows), so the table is the single source and nothing has to
   be changed in two places. */
typedef struct {
    uint8_t     step_id;
    uint16_t    gpioa_allowed_mask; /* bit 8 = PA8 */
    uint16_t    gpioe_solo_mask;    /* bits 5, 6, 7 (Solo MCU); never bus bits */
    uint16_t    gpiof_allowed_mask; /* bits 6..11, 14, 15 */
} BenchStepPerm_t;

#define BENCH_GPIOE_BUS_MASK  0xFF00U

static const BenchStepPerm_t k_step_perms[] = {
    { GPIO_BENCH_STEP_T15_RACE,       0U,        0U,                                (1U << 6) | (1U << 10) },
    { GPIO_BENCH_STEP_T1_REJECT,      0U,        0U,                                (1U << 9) },
    { GPIO_BENCH_STEP_T2_PARITY,      0U,        0U,                                (0xFU << 6) },
    { GPIO_BENCH_STEP_B1_MEASURE,     0U,        0U,                                (1U << 10) },
    { GPIO_BENCH_STEP_IDLE,           0U,        (1U << 7),                         0U },
    { GPIO_BENCH_STEP_T0B_GUARDS,     0U,        (1U << 7),                         0U },
    { GPIO_BENCH_STEP_T0B_S5_NEG,     0U,        (1U << 7),                         0U },
    { GPIO_BENCH_STEP_T0B_TIMEOUT,    0U,        (1U << 7),                         0U },
    { GPIO_BENCH_STEP_T0A_LOOSE,      0U,        (1U << 7),                         0U },
    { GPIO_BENCH_STEP_T0A_HANDSHAKE,  0U,        (1U << 7),                         0U },
    { GPIO_BENCH_STEP_T0A_MCU_TO_AD2, (1U << 8), (1U << 5) | (1U << 6) | (1U << 7), 0U },
    { GPIO_BENCH_STEP_T0A_AD2_TO_MCU, 0U,        (1U << 7),                         0U },
    { GPIO_BENCH_STEP_T0A_SCOPE,      0U,        (1U << 7),                         (1U << 14) | (1U << 15) },
    { GPIO_BENCH_STEP_T16_LCKR,       0U,        (1U << 7),                         (0xFU << 6) },
    { GPIO_BENCH_STEP_T3_MCU_OPS,     0U,        (1U << 7),                         0U },  /* + bus: table says MCU */
    { GPIO_BENCH_STEP_T4_AD2_PATTERNS,0U,        (1U << 7),                         0U },
    { GPIO_BENCH_STEP_T7_ANALOG_IDR,  0U,        (1U << 7),                         0U },
    { GPIO_BENCH_STEP_T8_NO_GLITCH,   0U,        (1U << 5) | (1U << 7),             0U },
    { GPIO_BENCH_STEP_T5_PULLS,       0U,        (1U << 7),                         (1U << 14) | (1U << 15) },
    { GPIO_BENCH_STEP_T6_OPEN_DRAIN,  0U,        (1U << 7),                         (1U << 14) | (1U << 15) },
    { GPIO_BENCH_STEP_T9_RISE_TIME,   0U,        (1U << 7),                         (1U << 14) | (1U << 15) },
    { GPIO_BENCH_STEP_T10_MCO1,       (1U << 8), (1U << 7),                         0U },
    { GPIO_BENCH_STEP_DONE,           0U,        (1U << 7),                         0U },
};

#define EXPAND_BENCH_ITEM(s, t, p, n, l, b, r) { (uint8_t)(s), (uint8_t)(t), (uint8_t)(p), (n), (l), (uint8_t)(b), (uint16_t)(r) },

const BenchProtocolItem_t g_bench_protocol_items[] = {
    GPIO_BENCH_PROTOCOL_TABLE(EXPAND_BENCH_ITEM)
};
const size_t g_bench_protocol_item_count = sizeof(g_bench_protocol_items) / sizeof(g_bench_protocol_items[0]);

static uint8_t s_current_step_id = GPIO_BENCH_STEP_T15_RACE;
static uint32_t s_step_prim_idx = 0U;

static const BenchStepPerm_t *bench_get_perm_for_step(uint8_t step_id)
{
    for (size_t i = 0; i < sizeof(k_step_perms) / sizeof(k_step_perms[0]); i++)
    {
        if (k_step_perms[i].step_id == step_id)
        {
            return &k_step_perms[i];
        }
    }
    return NULL;
}

static void bench_enter_step(uint8_t step_id)
{
    s_current_step_id = step_id;
    s_step_prim_idx = 0U;
    const char *bus_str = bench_get_step_bus_name(step_id);
    uint8_t bus_owner = bench_get_step_bus_owner(step_id);
    APP_LOG("[m7_gpio_test] STEP id=%u bus=%s v=%u",
            (unsigned int)step_id, bus_str, (unsigned int)GPIO_BENCH_PROTOCOL_VERSION);
    if (GPIO_TEST_SHARED != NULL)
    {
        GPIO_TEST_SHARED->current_step = step_id;
        GPIO_TEST_SHARED->bus_owner = bus_owner;
    }
}

/**
 * @brief Pure permission check for a given step, port and pin (M1).
 */
static bool bench_step_allows(uint8_t step_id, GPIO_TypeDef *port, uint32_t pin)
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

    const BenchStepPerm_t *perm = bench_get_perm_for_step(step_id);
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
        if ((pin_mask & BENCH_GPIOE_BUS_MASK) != 0U)
        {
            /* R4-m2: the bus follows the table's owner and nothing else */
            return bench_get_step_bus_owner(step_id) == GPIO_BENCH_BUS_MCU;
        }
        return (perm->gpioe_solo_mask & pin_mask) != 0U;
    }
    else if (port == GPIOF)
    {
        return (perm->gpiof_allowed_mask & pin_mask) != 0U;
    }

    return false;
}

/**
 * @brief Safety guard S4: Bench claim check for Cortex-M7 (M1).
 *        No step parameter accepted; reads s_current_step_id.
 */
static bool bench_claim_output(GPIO_TypeDef *port, uint32_t pin)
{
    return bench_step_allows(s_current_step_id, port, pin);
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
static void t10_restore_mco(void);

static void bench_safe(void)
{
    /* T10 may have left MCO1 on HSI/4; put the saved fields back. */
    t10_restore_mco();

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
    /* Check protocol table at runtime (R2-M3) */
    uint8_t expected = bench_get_expected_primitive(s_current_step_id, s_step_prim_idx);
    if (expected != GPIO_BENCH_PRIM_HS_MCU)
    {
        bench_safe();
        APP_LOG("[m7_gpio_test] ABORT step=%u reason=protocol", (unsigned int)s_current_step_id);
        return false;
    }
    s_step_prim_idx++;

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
typedef void (*BenchAction_t)(void *ctx);
static bool hs_host_act(uint32_t timeout_ms, BenchAction_t act, void *ctx);

static void hs_sample_idr(void *ctx)
{
    *(uint32_t *)ctx = GPIOE->IDR;
}

/* R-B1 HS_HOST with an IDR sample between HOST_REQ and MCU_ACK (hs_host_act). */
static bool hs_host_sample(uint32_t timeout_ms, uint32_t *p_idr)
{
    if (p_idr != NULL)
    {
        return hs_host_act(timeout_ms, hs_sample_idr, p_idr);
    }
    return hs_host_act(timeout_ms, NULL, NULL);
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

/* ==========================================================================
 * Tests without the AD2 (F1a): T15, T1, T2, B1 and T16.
 * ========================================================================== */

#define CM7_CYCLES_PER_US       400U
#define XCORE_WAIT_CYCLES       2000000000UL    /* 5 s at 400 MHz */

/* ---- CM7 -> CM4 job channel ---------------------------------------------- */

static void cm4_post(uint32_t cmd)
{
    __DMB();
    GPIO_TEST_SHARED->cmd = cmd;
    __DMB();
}

static bool cm4_wait_ack(uint32_t cmd)
{
    return wait_var_cycles(&GPIO_TEST_SHARED->ack, cmd, XCORE_WAIT_CYCLES);
}

/* CM4 jobs that need CM7 quiet (T1 photographs every port) run with CM7
   parked here: PRIMASK set, so no CM7 task or ISR writes any GPIO while CM4
   works. Bounded by XCORE_WAIT_CYCLES. */
static bool cm4_run_parked(uint32_t cmd)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    cm4_post(cmd);
    bool ok = cm4_wait_ack(cmd);
    __set_PRIMASK(primask);
    return ok;
}

/* ---- T15: race on GPIOF_MODER (PF10 CM7, PF11 CM4, PF6 for the ISR) ------ */

#define T15_SOLO_ITERS          50000U
#define T15_RMW_ITERS           500000U
#define T15_DRV_ITERS           200000U
#define T15_ISR_RATE_HZ         20000U

/* Limit of the test hook's HSEM_BM_TakeTimeout. The other core holds the lock
   only for the read-modify-writes and the read-back of one ConfigPin call,
   which is a few microseconds at most even at CM4's 200 MHz (B1 measures the
   real hold). 100 us is more than 20x that: a TIMEOUT then means the other
   core is stuck, not busy. It is also the longest this core waits with
   PRIMASK set, which bounds the interrupt latency the test can add.
   100 us x 400 cycles/us = 40 000 cycles. */
#define T15_HOOK_LIMIT_CYCLES   40000U

#define PF10_SHIFT              (10U * 2U)
#define PF11_SHIFT              (11U * 2U)
#define PF6_SHIFT               (6U * 2U)

static const GpioBm_PinConfig_t k_pf10_in = {
    .port = GPIOF, .pin_idx = 10U, .mode = GPIO_BM_MODE_INPUT,
    .otype = GPIO_BM_OTYPE_PP, .speed = GPIO_BM_SPEED_LOW, .pull = GPIO_BM_PULL_NONE,
};
static const GpioBm_PinConfig_t k_pf10_out = {
    .port = GPIOF, .pin_idx = 10U, .mode = GPIO_BM_MODE_OUTPUT,
    .otype = GPIO_BM_OTYPE_PP, .speed = GPIO_BM_SPEED_LOW, .pull = GPIO_BM_PULL_NONE,
    .initial_level = 0U,
};
static const GpioBm_PinConfig_t k_pf6_cfg[2] = {
    { .port = GPIOF, .pin_idx = 6U, .mode = GPIO_BM_MODE_INPUT,
      .otype = GPIO_BM_OTYPE_PP, .speed = GPIO_BM_SPEED_LOW, .pull = GPIO_BM_PULL_NONE },
    { .port = GPIOF, .pin_idx = 6U, .mode = GPIO_BM_MODE_OUTPUT,
      .otype = GPIO_BM_OTYPE_PP, .speed = GPIO_BM_SPEED_LOW, .pull = GPIO_BM_PULL_NONE,
      .initial_level = 0U },
};

typedef struct
{
    uint32_t corr;      /* own field wrong after the call, or the other pin out of {0,1} */
    uint32_t overlap;   /* iterations that saw the other core (or the ISR) active */
    uint32_t ok;
    uint32_t busy;
    uint32_t verify;
    uint32_t other;     /* any other status */
} T15Stats_t;

/* Hand read-modify-write of PF10's MODER field, as in F0. `widen` adds the
   NOPs F0 used to make the window visible. */
static void t15_rmw_loop(uint32_t iters, bool widen, volatile const uint32_t *other_flag, T15Stats_t *st)
{
    for (uint32_t i = 0U; i < iters; i++)
    {
        if ((other_flag != NULL) && (*other_flag != 0U))
        {
            st->overlap++;
        }

        uint32_t val = (i & 1U);
        uint32_t m = GPIOF->MODER;
        m &= ~(3UL << PF10_SHIFT);
        m |= (val << PF10_SHIFT);
        if (widen)
        {
            __asm__ volatile("nop; nop; nop; nop;");
        }
        GPIOF->MODER = m;
        if (widen)
        {
            __asm__ volatile("nop; nop;");
        }

        if (((GPIOF->MODER >> PF10_SHIFT) & 3U) != val)
        {
            st->corr++;
        }
        if (((GPIOF->MODER >> PF11_SHIFT) & 3U) > 1U)
        {
            st->corr++;
        }
    }
}

/* The same work through the driver: PF10 alternates INPUT / OUTPUT. */
static void t15_drv_loop(uint32_t iters, volatile const uint32_t *other_flag, T15Stats_t *st)
{
    for (uint32_t i = 0U; i < iters; i++)
    {
        if ((other_flag != NULL) && (*other_flag != 0U))
        {
            st->overlap++;
        }

        const GpioBm_PinConfig_t *cfg = ((i & 1U) != 0U) ? &k_pf10_out : &k_pf10_in;
        GpioBm_Status_t r = GPIO_BM_ConfigPin(cfg);
        switch (r)
        {
            case GPIO_BM_OK:         st->ok++;     break;
            case GPIO_BM_ERR_BUSY:   st->busy++;   break;
            case GPIO_BM_ERR_VERIFY: st->verify++; break;
            default:                 st->other++;  break;
        }

        if (((GPIOF->MODER >> PF10_SHIFT) & 3U) != cfg->mode)
        {
            st->corr++;
        }
        if (((GPIOF->MODER >> PF11_SHIFT) & 3U) > 1U)
        {
            st->corr++;
        }
    }
}

/* ---- The T15 (e) test hook: HSEM 5, instrumented for B1 ------------------ */
/* Lives here and only here (not in gpio_bm, shared_memory.c or main.c), and
   is installed only for T15 (e) and B1. */
static volatile uint32_t s_hook_t_enter;
static volatile uint32_t s_hook_t_acq;
static uint32_t s_hook_hold_max;
static uint32_t s_hook_primask_max;
static uint32_t s_hook_fail;
static bool     s_hook_count_calls;

static bool t15_hook_lock(void *ctx)
{
    (void)ctx;
    uint32_t t0 = DWT->CYCCNT;
    if (HSEM_BM_TakeTimeout(BOARD_GPIO_HSEM_ID, 0U, T15_HOOK_LIMIT_CYCLES) != HSEM_BM_OK)
    {
        s_hook_fail++;
        return false;
    }
    s_hook_t_enter = t0;
    s_hook_t_acq   = DWT->CYCCNT;
    if (s_hook_count_calls)
    {
        /* Both cores increment this word only while holding the lock. */
        GPIO_TEST_SHARED->t15_locked_calls++;
    }
    return true;
}

static void t15_hook_unlock(void *ctx)
{
    (void)ctx;
    uint32_t t = DWT->CYCCNT;
    uint32_t hold = t - s_hook_t_acq;
    uint32_t pm   = t - s_hook_t_enter;
    if (hold > s_hook_hold_max)
    {
        s_hook_hold_max = hold;
    }
    if (pm > s_hook_primask_max)
    {
        s_hook_primask_max = pm;
    }
    __DMB();
    (void)HSEM_BM_Release(BOARD_GPIO_HSEM_ID, 0U);
}

static const GpioBm_SharedLock_t k_t15_hook = { t15_hook_lock, t15_hook_unlock, NULL };

/* ---- (f) and (g): TIM17 on CM7 reconfigures PF6 --------------------------- */

static volatile uint32_t s_isr_count;
static volatile uint32_t s_isr_in_loop;   /* ISR entries while the main loop ran */
static volatile uint32_t s_isr_corr;      /* PF6 not as the previous ISR left it */
static volatile uint32_t s_isr_err;       /* ConfigPin status != OK inside the ISR */
static volatile uint32_t s_isr_last;      /* 0 = INPUT, 1 = OUTPUT */
static volatile uint32_t s_isr_primed;
static volatile uint32_t s_main_in_loop;

void TIM17_IRQHandler(void)
{
    TIM17->SR = ~TIM_SR_UIF;
    __DSB();

    s_isr_count++;
    if (s_main_in_loop != 0U)
    {
        s_isr_in_loop++;
    }

    uint32_t field = (GPIOF->MODER >> PF6_SHIFT) & 3U;
    if ((s_isr_primed != 0U) && (field != s_isr_last))
    {
        s_isr_corr++;
    }

    uint32_t next = s_isr_last ^ 1U;
    if (GPIO_BM_ConfigPin(&k_pf6_cfg[next]) != GPIO_BM_OK)
    {
        s_isr_err++;
    }
    s_isr_last   = next;
    s_isr_primed = 1U;
}

static uint32_t tim17_clock_hz(void)
{
    uint32_t pclk2 = HAL_RCC_GetPCLK2Freq();
    /* RM0399 8.5.x: with TIMPRE = 0, the timer kernel clock is 2 x PCLK2
       whenever the APB2 prescaler divides. */
    return ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE2) != 0U) ? (2U * pclk2) : pclk2;
}

static uint32_t s_tim17_was_on;

static uint32_t tim17_start(void)
{
    uint32_t clk = tim17_clock_hz();
    uint32_t arr = (clk / T15_ISR_RATE_HZ) - 1U;

    s_tim17_was_on = RCC->APB2ENR & RCC_APB2ENR_TIM17EN;
    RCC->APB2ENR |= RCC_APB2ENR_TIM17EN;       /* banked per core (RM0399 9.7.1) */
    (void)RCC->APB2ENR;

    TIM17->CR1  = 0U;
    TIM17->PSC  = 0U;
    TIM17->ARR  = arr;
    TIM17->CNT  = 0U;
    TIM17->EGR  = TIM_EGR_UG;
    TIM17->SR   = 0U;
    TIM17->DIER = TIM_DIER_UIE;
    NVIC_ClearPendingIRQ(TIM17_IRQn);
    NVIC_SetPriority(TIM17_IRQn, 6U);
    NVIC_EnableIRQ(TIM17_IRQn);
    TIM17->CR1  = TIM_CR1_CEN;
    return clk / (arr + 1U);
}

static void tim17_stop(void)
{
    TIM17->CR1  = 0U;
    TIM17->DIER = 0U;
    NVIC_DisableIRQ(TIM17_IRQn);
    TIM17->SR   = 0U;
    NVIC_ClearPendingIRQ(TIM17_IRQn);
    __DSB();
    if (s_tim17_was_on == 0U)
    {
        RCC->APB2ENR &= ~RCC_APB2ENR_TIM17EN;
    }
}

static void isr_phase_reset(void)
{
    s_isr_count = 0U;
    s_isr_in_loop = 0U;
    s_isr_corr = 0U;
    s_isr_err = 0U;
    s_isr_last = 0U;
    s_isr_primed = 0U;
    (void)GPIO_BM_ConfigPin(&k_pf6_cfg[0]);
}

static uint32_t s_lckr_boot;
static uint32_t s_b1_hold_max_e;
static uint32_t s_b1_primask_max_e;

static bool test_t15_race(void)
{
    bool pass = true;
    uint32_t session = GPIO_TEST_SHARED->session_id;

    bench_enter_step(GPIO_BENCH_STEP_T15_RACE);

    /* LCKR is cleared only by a reset; T16 locks PF7..PF9 at the end of the
       previous run. Say what this boot found before anything else. */
    s_lckr_boot = GPIOF->LCKR;
    APP_LOG("[m7_gpio_test] LCKR at boot: GPIOF=0x%08lX %s",
            (unsigned long)s_lckr_boot, (s_lckr_boot == 0U) ? "CLEAN" : "LOCKED");
    if (s_lckr_boot != 0U)
    {
        pass = false;
    }

    if (!bench_claim_output(GPIOF, 10U) || !bench_claim_output(GPIOF, 6U))
    {
        APP_LOG("[m7_gpio_test] T15 FAIL: bench_claim_output rejected PF10/PF6");
        bench_safe();
        APP_LOG("[m7_gpio_test] T15 VERDICT: FAIL");
        return false;
    }

    /* Known start: PF6, PF10 and PF11 in input, not in the reset analog state. */
    GPIOF->MODER &= ~((3UL << PF6_SHIFT) | (3UL << PF10_SHIFT) | (3UL << PF11_SHIFT));
    GPIOF->PUPDR &= ~((3UL << PF6_SHIFT) | (3UL << PF10_SHIFT) | (3UL << PF11_SHIFT));

    if (!wait_var_cycles(&GPIO_TEST_SHARED->cm4_ready, session, XCORE_WAIT_CYCLES))
    {
        APP_LOG("[m7_gpio_test] T15 FAIL: TIMEOUT waiting for CM4 ready");
        bench_safe();
        APP_LOG("[m7_gpio_test] T15 VERDICT: FAIL");
        return false;
    }

    T15Stats_t a = {0}, c = {0}, d = {0}, e = {0}, f = {0}, g = {0};
    bool xcore_ok = true;

    /* (a) CM7 alone, hand RMW: the control of the control. */
    t15_rmw_loop(T15_SOLO_ITERS, false, NULL, &a);

    /* (b) CM4 alone, hand RMW. */
    cm4_post(GPIO_ST_CMD_T15_B);
    xcore_ok = xcore_ok && cm4_wait_ack(GPIO_ST_CMD_T15_B);

    /* (c) both, hand RMW: the control. */
    GPIO_TEST_SHARED->in_loop_cm7 = 1U;
    cm4_post(GPIO_ST_CMD_T15_C);
    t15_rmw_loop(T15_RMW_ITERS, true, &GPIO_TEST_SHARED->in_loop_cm4, &c);
    GPIO_TEST_SHARED->in_loop_cm7 = 0U;
    xcore_ok = xcore_ok && cm4_wait_ack(GPIO_ST_CMD_T15_C);

    /* (d) both, gpio_bm without hook: PRIMASK does not protect across cores. */
    GPIO_BM_SetSharedLock(NULL);
    GPIO_TEST_SHARED->in_loop_cm7 = 1U;
    cm4_post(GPIO_ST_CMD_T15_D);
    t15_drv_loop(T15_DRV_ITERS, &GPIO_TEST_SHARED->in_loop_cm4, &d);
    GPIO_TEST_SHARED->in_loop_cm7 = 0U;
    xcore_ok = xcore_ok && cm4_wait_ack(GPIO_ST_CMD_T15_D);

    /* (e) both, gpio_bm with the test hook. */
    GPIO_TEST_SHARED->t15_locked_calls = 0U;
    s_hook_hold_max = 0U;
    s_hook_primask_max = 0U;
    s_hook_fail = 0U;
    s_hook_count_calls = true;
    GPIO_BM_SetSharedLock(&k_t15_hook);
    GPIO_TEST_SHARED->in_loop_cm7 = 1U;
    cm4_post(GPIO_ST_CMD_T15_E);
    t15_drv_loop(T15_DRV_ITERS, &GPIO_TEST_SHARED->in_loop_cm4, &e);
    GPIO_TEST_SHARED->in_loop_cm7 = 0U;
    GPIO_BM_SetSharedLock(NULL);
    s_hook_count_calls = false;
    xcore_ok = xcore_ok && cm4_wait_ack(GPIO_ST_CMD_T15_E);
    s_b1_hold_max_e    = s_hook_hold_max;
    s_b1_primask_max_e = s_hook_primask_max;
    uint32_t hook_fail_e = s_hook_fail;

    /* (f) and (g): same core. CM4 waits for its next job (parked, no GPIO).
       A TIM17 ISR on CM7 reconfigures PF6 while the loop works on PF10. */
    isr_phase_reset();
    uint32_t isr_rate = tim17_start();
    s_main_in_loop = 1U;
    t15_rmw_loop(T15_RMW_ITERS, true, &s_main_in_loop, &f);   /* (f): hand RMW, no PRIMASK */
    s_main_in_loop = 0U;
    tim17_stop();
    uint32_t f_isr_count = s_isr_count, f_isr_in_loop = s_isr_in_loop;
    uint32_t f_isr_corr = s_isr_corr, f_isr_err = s_isr_err;

    isr_phase_reset();
    (void)tim17_start();
    s_main_in_loop = 1U;
    GPIO_BM_SetSharedLock(NULL);
    t15_drv_loop(T15_DRV_ITERS, NULL, &g);                    /* (g): gpio_bm, no hook */
    s_main_in_loop = 0U;
    tim17_stop();
    uint32_t g_isr_count = s_isr_count, g_isr_in_loop = s_isr_in_loop;
    uint32_t g_isr_corr = s_isr_corr, g_isr_err = s_isr_err;

    /* Back to input, as found. */
    GPIOF->MODER &= ~((3UL << PF6_SHIFT) | (3UL << PF10_SHIFT) | (3UL << PF11_SHIFT));

    volatile GpioSelfTestShared_t *sh = GPIO_TEST_SHARED;
    uint32_t corr_a = a.corr;
    uint32_t corr_b = sh->t15_cm4_corr[0];
    uint32_t corr_c = c.corr + sh->t15_cm4_corr[1];
    uint32_t corr_d = d.corr + d.verify + d.other + sh->t15_cm4_corr[2] + sh->t15_cm4_verify[2];
    uint32_t corr_e = e.corr + e.verify + e.other + sh->t15_cm4_corr[3] + sh->t15_cm4_verify[3];
    uint32_t busy_e = e.busy + sh->t15_cm4_busy[3];
    uint32_t corr_f = f.corr + f_isr_corr + f_isr_err;
    uint32_t corr_g = g.corr + g.verify + g.other + g_isr_corr + g_isr_err;
    uint32_t ok_sum_e = e.ok + sh->t15_cm4_ok[3];

    APP_LOG("[m7_gpio_test] INFO T15 (a) CM7 solo, hand RMW: corruptions=%lu (expected 0)", (unsigned long)corr_a);
    APP_LOG("[m7_gpio_test] INFO T15 (b) CM4 solo, hand RMW: corruptions=%lu (expected 0)", (unsigned long)corr_b);
    APP_LOG("[m7_gpio_test] INFO T15 (c) both, hand RMW: corr=%lu (CM7=%lu CM4=%lu) ovl CM7=%lu CM4=%lu (expect >0)",
            (unsigned long)corr_c, (unsigned long)c.corr, (unsigned long)sh->t15_cm4_corr[1],
            (unsigned long)c.overlap, (unsigned long)sh->t15_cm4_overlap[1]);
    APP_LOG("[m7_gpio_test] INFO T15 (d) both, gpio_bm no hook: corr=%lu ovl CM7=%lu CM4=%lu (expect >0)",
            (unsigned long)corr_d, (unsigned long)d.overlap, (unsigned long)sh->t15_cm4_overlap[2]);
    APP_LOG("[m7_gpio_test] INFO T15 (d) detail: CM7 field=%lu verify=%lu other=%lu, CM4 field=%lu verify=%lu",
            (unsigned long)d.corr, (unsigned long)d.verify, (unsigned long)d.other,
            (unsigned long)sh->t15_cm4_corr[2], (unsigned long)sh->t15_cm4_verify[2]);
    APP_LOG("[m7_gpio_test] INFO T15 (e) both, gpio_bm + hook: corr=%lu busy=%lu hook_fail CM7=%lu CM4=%lu ovl CM7=%lu CM4=%lu (expect 0)",
            (unsigned long)corr_e, (unsigned long)busy_e, (unsigned long)hook_fail_e,
            (unsigned long)sh->t15_cm4_hook_fail, (unsigned long)e.overlap, (unsigned long)sh->t15_cm4_overlap[3]);
    APP_LOG("[m7_gpio_test] INFO T15 (e) calls: locked=%lu ok CM7=%lu + CM4=%lu = %lu (iters CM7=%lu CM4=%lu)",
            (unsigned long)sh->t15_locked_calls, (unsigned long)e.ok, (unsigned long)sh->t15_cm4_ok[3],
            (unsigned long)ok_sum_e, (unsigned long)T15_DRV_ITERS, (unsigned long)sh->t15_cm4_calls[3]);
    APP_LOG("[m7_gpio_test] INFO T15 (f) CM7 hand RMW vs TIM17 ISR at %lu Hz: corr=%lu ISR in loop=%lu of %lu (expect >0)",
            (unsigned long)isr_rate, (unsigned long)corr_f, (unsigned long)f_isr_in_loop, (unsigned long)f_isr_count);
    APP_LOG("[m7_gpio_test] INFO T15 (f) detail: main=%lu ISR=%lu ISR err=%lu",
            (unsigned long)f.corr, (unsigned long)f_isr_corr, (unsigned long)f_isr_err);
    APP_LOG("[m7_gpio_test] INFO T15 (g) CM7 gpio_bm vs TIM17 ISR: corr=%lu ISR in loop=%lu of %lu (expect 0)",
            (unsigned long)corr_g, (unsigned long)g_isr_in_loop, (unsigned long)g_isr_count);
    APP_LOG("[m7_gpio_test] INFO T15 (g) detail: main=%lu verify=%lu other=%lu ISR=%lu ISR err=%lu",
            (unsigned long)g.corr, (unsigned long)g.verify, (unsigned long)g.other,
            (unsigned long)g_isr_corr, (unsigned long)g_isr_err);

    if (!xcore_ok)
    {
        APP_LOG("[m7_gpio_test] INFO T15 FAIL: a CM4 phase did not acknowledge in time");
        pass = false;
    }
    if ((corr_a != 0U) || (corr_b != 0U))                                   { pass = false; }
    if ((corr_c == 0U) || (c.overlap == 0U) || (sh->t15_cm4_overlap[1] == 0U)) { pass = false; }
    if ((corr_d == 0U) || (d.overlap == 0U) || (sh->t15_cm4_overlap[2] == 0U)) { pass = false; }
    if ((corr_e != 0U) || (busy_e != 0U) || (hook_fail_e != 0U) || (sh->t15_cm4_hook_fail != 0U) ||
        (e.overlap == 0U) || (sh->t15_cm4_overlap[3] == 0U) ||
        (sh->t15_locked_calls != ok_sum_e) ||
        (e.ok != T15_DRV_ITERS) || (sh->t15_cm4_ok[3] != sh->t15_cm4_calls[3]))  { pass = false; }
    if ((corr_f == 0U) || (f_isr_in_loop == 0U))                              { pass = false; }
    if ((corr_g != 0U) || (g_isr_in_loop == 0U))                              { pass = false; }

    APP_LOG("[m7_gpio_test] T15 VERDICT: %s", pass ? "PASS" : "FAIL");
    if (!pass)
    {
        bench_safe();
    }
    return pass;
}

/* ---- T1: refusals touch nothing -------------------------------------------- */

#define PHOTO_PORTS     11U
#define PHOTO_REGS      8U

typedef struct
{
    uint32_t enr_c1;
    uint32_t enr_c2;
    uint32_t regs[PHOTO_PORTS][PHOTO_REGS];
} GpioPhoto_t;

/* Every configuration register of every clocked port, LCKR, and both banks of
   RCC_AHB4ENR. A port clocked by neither core is not read: its registers can
   only change after a clock enable, which the RCC words show. */
static void photo_take(GpioPhoto_t *ph)
{
    ph->enr_c1 = RCC_C1->AHB4ENR;
    ph->enr_c2 = RCC_C2->AHB4ENR;
    uint32_t clocked = ph->enr_c1 | ph->enr_c2;

    for (uint32_t p = 0U; p < PHOTO_PORTS; p++)
    {
        if ((clocked & (1UL << p)) == 0U)
        {
            for (uint32_t r = 0U; r < PHOTO_REGS; r++)
            {
                ph->regs[p][r] = 0U;
            }
            continue;
        }
        const GPIO_TypeDef *g = (const GPIO_TypeDef *)(GPIOA_BASE + (p * 0x400UL));
        ph->regs[p][0] = g->MODER;
        ph->regs[p][1] = g->OTYPER;
        ph->regs[p][2] = g->OSPEEDR;
        ph->regs[p][3] = g->PUPDR;
        ph->regs[p][4] = g->AFR[0];
        ph->regs[p][5] = g->AFR[1];
        ph->regs[p][6] = g->ODR;
        ph->regs[p][7] = g->LCKR;
    }
}

static bool photo_equal(const GpioPhoto_t *x, const GpioPhoto_t *y)
{
    return memcmp(x, y, sizeof(*x)) == 0;
}

/* A port clocked by neither core: a refusal that enabled its clock first
   would show in RCC_AHB4ENR. Searched from GPIOK down. */
static GPIO_TypeDef *t1_unclocked_port(char *letter)
{
    uint32_t clocked = RCC_C1->AHB4ENR | RCC_C2->AHB4ENR;
    for (int32_t p = (int32_t)PHOTO_PORTS - 1; p >= 0; p--)
    {
        if ((clocked & (1UL << (uint32_t)p)) == 0U)
        {
            *letter = (char)('A' + p);
            return (GPIO_TypeDef *)(GPIOA_BASE + ((uint32_t)p * 0x400UL));
        }
    }
    *letter = '?';
    return NULL;
}

typedef struct
{
    uint32_t cases;
    uint32_t failed;
} T1Stats_t;

static GpioPhoto_t s_ph_before;
static GpioPhoto_t s_ph_after;

static void t1_check(const char *name, uint32_t got, uint32_t exp, T1Stats_t *st)
{
    bool same = photo_equal(&s_ph_before, &s_ph_after);
    bool ok = (got == exp) && same;
    st->cases++;
    if (!ok)
    {
        st->failed++;
    }
    APP_LOG("[m7_gpio_test] INFO T1 %-22s got=%lu exp=%lu photo=%s %s",
            name, (unsigned long)got, (unsigned long)exp, same ? "same" : "CHANGED", ok ? "OK" : "FAIL");
}

/* Photo, call, photo, all with PRIMASK set: no CM7 task or ISR writes a GPIO
   in between, and CM4 is waiting for its next job. */
#define T1_CASE(name, call_expr, expected)                           \
    do {                                                             \
        uint32_t pm_ = __get_PRIMASK();                              \
        __disable_irq();                                             \
        photo_take(&s_ph_before);                                    \
        uint32_t got_ = (uint32_t)(call_expr);                       \
        photo_take(&s_ph_after);                                     \
        __set_PRIMASK(pm_);                                          \
        t1_check((name), got_, (uint32_t)(expected), &st);           \
    } while (0)

static bool test_t1_reject(void)
{
    T1Stats_t st = {0};
    char letter;
    bool pass = true;

    bench_enter_step(GPIO_BENCH_STEP_T1_REJECT);

    GPIO_TypeDef *P = t1_unclocked_port(&letter);
    if (P == NULL)
    {
        APP_LOG("[m7_gpio_test] INFO T1 no port is unclocked in both cores: the clock of a refusal cannot be checked");
        pass = false;
    }
    else
    {
        APP_LOG("[m7_gpio_test] INFO T1 unclocked port used for field cases: GPIO%c", letter);

        GPIO_TypeDef *const misaligned = (GPIO_TypeDef *)(GPIOA_BASE + 4U);
        GPIO_TypeDef *const past_k     = (GPIO_TypeDef *)(GPIOK_BASE + 0x400U);
        GpioBm_PinConfig_t cfg;

#define CFG(p_, pin_, mode_, ot_, sp_, pu_, af_, lvl_)                                   \
        (cfg = (GpioBm_PinConfig_t){ .port = (p_), .pin_idx = (pin_), .mode = (mode_),   \
                                     .otype = (ot_), .speed = (sp_), .pull = (pu_),       \
                                     .af = (af_), .initial_level = (lvl_),                \
                                     .allow_debug_pins = false }, &cfg)

        T1_CASE("ConfigPin(NULL)",      GPIO_BM_ConfigPin(NULL),                                 GPIO_BM_ERR_PARAM);
        T1_CASE("port NULL",            GPIO_BM_ConfigPin(CFG(NULL, 0U, 0U, 0U, 0U, 0U, 0U, 0U)),       GPIO_BM_ERR_PARAM);
        T1_CASE("GPIOA+4",              GPIO_BM_ConfigPin(CFG(misaligned, 0U, 0U, 0U, 0U, 0U, 0U, 0U)), GPIO_BM_ERR_PARAM);
        T1_CASE("past GPIOK",           GPIO_BM_ConfigPin(CFG(past_k, 0U, 0U, 0U, 0U, 0U, 0U, 0U)),     GPIO_BM_ERR_PARAM);
        T1_CASE("pin 16",               GPIO_BM_ConfigPin(CFG(P, 16U, 0U, 0U, 0U, 0U, 0U, 0U)),         GPIO_BM_ERR_PARAM);
        T1_CASE("AF 16",                GPIO_BM_ConfigPin(CFG(P, 0U, 2U, 0U, 0U, 0U, 16U, 0U)),         GPIO_BM_ERR_PARAM);
        T1_CASE("pull 3",               GPIO_BM_ConfigPin(CFG(P, 0U, 0U, 0U, 0U, 3U, 0U, 0U)),          GPIO_BM_ERR_PARAM);
        T1_CASE("speed 4",              GPIO_BM_ConfigPin(CFG(P, 0U, 1U, 0U, 4U, 0U, 0U, 0U)),          GPIO_BM_ERR_PARAM);
        T1_CASE("mode 4",               GPIO_BM_ConfigPin(CFG(P, 0U, 4U, 0U, 0U, 0U, 0U, 0U)),          GPIO_BM_ERR_PARAM);
        T1_CASE("level 2",              GPIO_BM_ConfigPin(CFG(P, 0U, 1U, 0U, 0U, 0U, 0U, 2U)),          GPIO_BM_ERR_PARAM);
        T1_CASE("analog + pull",        GPIO_BM_ConfigPin(CFG(P, 0U, 3U, 0U, 0U, 1U, 0U, 0U)),          GPIO_BM_ERR_PARAM);
        T1_CASE("otype 2",              GPIO_BM_ConfigPin(CFG(P, 0U, 1U, 2U, 0U, 0U, 0U, 0U)),          GPIO_BM_ERR_PARAM);
        T1_CASE("PA13",                 GPIO_BM_ConfigPin(CFG(GPIOA, 13U, 0U, 0U, 0U, 0U, 0U, 0U)),     GPIO_BM_ERR_DEBUG_PIN);
        T1_CASE("PA14",                 GPIO_BM_ConfigPin(CFG(GPIOA, 14U, 0U, 0U, 0U, 0U, 0U, 0U)),     GPIO_BM_ERR_DEBUG_PIN);
        T1_CASE("ConfigOutput speed 4", GPIO_BM_ConfigOutput(P, 0U, 0U, 4U, 0U, 0U),             GPIO_BM_ERR_PARAM);
        T1_CASE("ConfigInput pull 3",   GPIO_BM_ConfigInput(P, 0U, 3U),                          GPIO_BM_ERR_PARAM);
        T1_CASE("AlternateEx af 16",    GPIO_BM_ConfigAlternateEx(P, 0U, 16U, 0U, 0U, 0U),       GPIO_BM_ERR_PARAM);
        T1_CASE("DeinitPin GPIOA+4",    GPIO_BM_DeinitPin(misaligned, 0U),                       GPIO_BM_ERR_PARAM);
        T1_CASE("DeinitPin pin 16",     GPIO_BM_DeinitPin(P, 16U),                               GPIO_BM_ERR_PARAM);
        T1_CASE("DeinitPin PA13",       GPIO_BM_DeinitPin(GPIOA, 13U),                           GPIO_BM_ERR_DEBUG_PIN);
        T1_CASE("LockPins mask 0",      GPIO_BM_LockPins(P, 0U),                                 GPIO_BM_ERR_PARAM);
        T1_CASE("LockPins mask 1<<16",  GPIO_BM_LockPins(P, 0x10000U),                           GPIO_BM_ERR_PARAM);
        T1_CASE("LockPins past GPIOK",  GPIO_BM_LockPins(past_k, 1U),                            GPIO_BM_ERR_PARAM);
        T1_CASE("ReadConfig out NULL",  GPIO_BM_ReadConfig(P, 0U, NULL),                         GPIO_BM_ERR_PARAM);
        /* Legacy API (G3, G4): false and nothing written. */
        T1_CASE("legacy AF pull 3",     GPIO_BM_ConfigAlternate(P, 0U, 7U, 3U, 0U),              0U);
        T1_CASE("legacy AF speed 4",    GPIO_BM_ConfigAlternate(P, 0U, 7U, 0U, 4U),              0U);
        T1_CASE("legacy AF af 16",      GPIO_BM_ConfigAlternate(P, 0U, 16U, 0U, 0U),             0U);
        T1_CASE("legacy analog +4",     GPIO_BM_ConfigAnalog(misaligned, 0U),                    0U);
        T1_CASE("EnableClock GPIOA+4",  GPIO_BM_EnablePortClock(misaligned),                     0U);
        T1_CASE("EnableClock past K",   GPIO_BM_EnablePortClock(past_k),                         0U);
#undef CFG
    }

    /* Control: a valid call does change the photo, so the comparator sees a change. */
    bool control_changed = false;
    if (bench_claim_output(GPIOF, 9U))
    {
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        photo_take(&s_ph_before);
        GpioBm_Status_t r = GPIO_BM_ConfigOutput(GPIOF, 9U, GPIO_BM_OTYPE_OD, GPIO_BM_SPEED_HIGH,
                                                 GPIO_BM_PULL_UP, 1U);
        photo_take(&s_ph_after);
        __set_PRIMASK(pm);
        control_changed = (r == GPIO_BM_OK) && !photo_equal(&s_ph_before, &s_ph_after);
        (void)GPIO_BM_DeinitPin(GPIOF, 9U);
    }
    APP_LOG("[m7_gpio_test] INFO T1 control: valid ConfigOutput(PF9) changed the photo: %s",
            control_changed ? "yes" : "NO");

    bool cm7_pass = pass && (st.failed == 0U) && control_changed;
    APP_LOG("[m7_gpio_test] INFO T1 CM7: cases=%lu failed=%lu control=%s -> %s",
            (unsigned long)st.cases, (unsigned long)st.failed, control_changed ? "changed" : "unchanged",
            cm7_pass ? "PASS" : "FAIL");

    /* CM4 runs its own T1 with CM7 parked. */
    bool cm4_ok = cm4_run_parked(GPIO_ST_CMD_T1);
    volatile GpioSelfTestShared_t *sh = GPIO_TEST_SHARED;
    APP_LOG("[m7_gpio_test] INFO T1 CM4: cases=%lu failed=%lu control=%s -> %s%s",
            (unsigned long)sh->t1_cm4_cases, (unsigned long)sh->t1_cm4_failed_cases,
            (sh->t1_cm4_control_changed != 0U) ? "changed" : "unchanged",
            (sh->t1_cm4_pass != 0U) ? "PASS" : "FAIL", cm4_ok ? "" : " (no ack)");

    pass = cm7_pass && cm4_ok && (sh->t1_cm4_pass != 0U);
    APP_LOG("[m7_gpio_test] T1 VERDICT: %s", pass ? "PASS" : "FAIL");
    return pass;
}

/* ---- T2: same bits as HAL_GPIO_Init, and legacy as in ca569b6 --------------- */

typedef struct
{
    uint32_t moder, otyper, ospeedr, pupdr, afr;
} PinFields_t;

static PinFields_t pin_fields(const GPIO_TypeDef *g, uint32_t pin)
{
    PinFields_t f;
    f.moder   = (g->MODER   >> (pin * 2U)) & 3U;
    f.otyper  = (g->OTYPER  >> pin) & 1U;
    f.ospeedr = (g->OSPEEDR >> (pin * 2U)) & 3U;
    f.pupdr   = (g->PUPDR   >> (pin * 2U)) & 3U;
    f.afr     = (g->AFR[pin >> 3U] >> ((pin & 7U) * 4U)) & 0xFU;
    return f;
}

static bool fields_equal(const PinFields_t *x, const PinFields_t *y)
{
    return (x->moder == y->moder) && (x->otyper == y->otyper) && (x->ospeedr == y->ospeedr) &&
           (x->pupdr == y->pupdr) && (x->afr == y->afr);
}

/* Test-side writer for start states: plain RMW on unwired PF6..PF9, claimed. */
static void pin_force(GPIO_TypeDef *g, uint32_t pin, const PinFields_t *f)
{
    g->OTYPER  = (g->OTYPER  & ~(1UL << pin))            | (f->otyper << pin);
    g->OSPEEDR = (g->OSPEEDR & ~(3UL << (pin * 2U)))     | (f->ospeedr << (pin * 2U));
    g->PUPDR   = (g->PUPDR   & ~(3UL << (pin * 2U)))     | (f->pupdr << (pin * 2U));
    g->AFR[pin >> 3U] = (g->AFR[pin >> 3U] & ~(0xFUL << ((pin & 7U) * 4U))) | (f->afr << ((pin & 7U) * 4U));
    g->MODER   = (g->MODER   & ~(3UL << (pin * 2U)))     | (f->moder << (pin * 2U));
}

/* The legacy bodies as they were in ca569b6, verbatim except for the clock
   enable (GPIOF is already clocked). The oracle for "the legacy path leaves
   the same bits as in ca569b6". */
static void ref_ca569b6_config_alternate(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t af, uint32_t pull, uint32_t speed)
{
    uint32_t afr_idx   = pin_idx >> 3U;
    uint32_t afr_shift = (pin_idx & 7U) * 4U;
    port->AFR[afr_idx] = (port->AFR[afr_idx] & ~(0xFU << afr_shift)) | ((af & 0xFU) << afr_shift);
    port->OTYPER &= ~(1UL << pin_idx);
    uint32_t pos2 = pin_idx * 2U;
    port->OSPEEDR = (port->OSPEEDR & ~(0x3UL << pos2)) | ((speed & 0x3UL) << pos2);
    port->PUPDR = (port->PUPDR & ~(0x3UL << pos2)) | ((pull & 0x3UL) << pos2);
    port->MODER = (port->MODER & ~(0x3UL << pos2)) | (0x2UL << pos2);
}

static void ref_ca569b6_config_analog(GPIO_TypeDef *port, uint32_t pin_idx)
{
    uint32_t pos2 = pin_idx * 2U;
    port->MODER |= (0x3UL << pos2);
    port->PUPDR &= ~(0x3UL << pos2);
}

static const PinFields_t k_t2_start[2] = {
    { .moder = 3U, .otyper = 1U, .ospeedr = 2U, .pupdr = 1U, .afr = 5U },
    { .moder = 1U, .otyper = 0U, .ospeedr = 1U, .pupdr = 2U, .afr = 10U },
};

static uint32_t hal_mode(uint32_t mode, uint32_t otype)
{
    switch (mode)
    {
        case GPIO_BM_MODE_INPUT:  return GPIO_MODE_INPUT;
        case GPIO_BM_MODE_OUTPUT: return (otype != 0U) ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT_PP;
        case GPIO_BM_MODE_AF:     return (otype != 0U) ? GPIO_MODE_AF_OD : GPIO_MODE_AF_PP;
        default:                  return GPIO_MODE_ANALOG;
    }
}

static const uint32_t k_hal_pull[3] = { GPIO_NOPULL, GPIO_PULLUP, GPIO_PULLDOWN };
static const uint32_t k_t2_afs[3]   = { 0U, 7U, 15U };

typedef struct
{
    uint32_t combos, compared, refused, mismatches, odr_bad, legacy_bad;
} T2Stats_t;

static void t2_report_mismatch(const char *what, uint32_t pin, const PinFields_t *a, const PinFields_t *b, T2Stats_t *st)
{
    if (st->mismatches + st->legacy_bad + st->odr_bad <= 8U)
    {
        APP_LOG("[m7_gpio_test] INFO T2 MISMATCH %s PF%lu drv M%lu T%lu S%lu P%lu A%lu ref M%lu T%lu S%lu P%lu A%lu",
                what, (unsigned long)pin,
                (unsigned long)a->moder, (unsigned long)a->otyper, (unsigned long)a->ospeedr, (unsigned long)a->pupdr, (unsigned long)a->afr,
                (unsigned long)b->moder, (unsigned long)b->otyper, (unsigned long)b->ospeedr, (unsigned long)b->pupdr, (unsigned long)b->afr);
    }
}

static bool test_t2_parity(void)
{
    T2Stats_t st = {0};
    bool claims = true;
    PinFields_t last_drv = {0}, last_ref = {0};
    bool have_last = false;

    bench_enter_step(GPIO_BENCH_STEP_T2_PARITY);

    for (uint32_t pin = 6U; pin <= 9U; pin++)
    {
        claims = claims && bench_claim_output(GPIOF, pin);
    }
    if (!claims)
    {
        APP_LOG("[m7_gpio_test] INFO T2 bench_claim_output rejected PF6..PF9");
        APP_LOG("[m7_gpio_test] T2 VERDICT: FAIL");
        bench_safe();
        return false;
    }

    /* Pairs: the driver writes the first pin, HAL_GPIO_Init the second. PF6/PF7
       exercise AFR[0], PF8/PF9 AFR[1]. */
    static const uint32_t pairs[2][2] = { { 6U, 7U }, { 8U, 9U } };

    for (uint32_t mode = 0U; mode < 4U; mode++)
    for (uint32_t ot = 0U; ot < 2U; ot++)
    for (uint32_t sp = 0U; sp < 4U; sp++)
    for (uint32_t pu = 0U; pu < 3U; pu++)
    for (uint32_t ai = 0U; ai < 3U; ai++)
    {
        uint32_t level = st.combos & 1U;
        st.combos++;

        for (uint32_t pr = 0U; pr < 2U; pr++)
        for (uint32_t sv = 0U; sv < 2U; sv++)
        {
            uint32_t pd = pairs[pr][0];
            uint32_t ph = pairs[pr][1];
            GpioBm_PinConfig_t cfg = {
                .port = GPIOF, .pin_idx = pd, .mode = mode, .otype = ot, .speed = sp,
                .pull = pu, .af = k_t2_afs[ai], .initial_level = level, .allow_debug_pins = false,
            };

            pin_force(GPIOF, pd, &k_t2_start[sv]);
            pin_force(GPIOF, ph, &k_t2_start[sv]);

            GpioBm_Status_t r = GPIO_BM_ConfigPin(&cfg);
            if ((mode == GPIO_BM_MODE_ANALOG) && (pu != GPIO_BM_PULL_NONE))
            {
                /* C6 refuses analog with pull; HAL has no such rule (G7). */
                st.refused++;
                if (r != GPIO_BM_ERR_PARAM)
                {
                    st.mismatches++;
                }
                continue;
            }

            GPIO_InitTypeDef init = {
                .Pin = (1UL << ph), .Mode = hal_mode(mode, ot), .Pull = k_hal_pull[pu],
                .Speed = sp, .Alternate = k_t2_afs[ai],
            };
            HAL_GPIO_Init(GPIOF, &init);

            PinFields_t fd = pin_fields(GPIOF, pd);
            PinFields_t fh = pin_fields(GPIOF, ph);
            st.compared++;
            if ((r != GPIO_BM_OK) || !fields_equal(&fd, &fh))
            {
                t2_report_mismatch("cfg", pd, &fd, &fh, &st);
                st.mismatches++;
            }
            else
            {
                last_drv = fd;
                last_ref = fh;
                have_last = true;
            }
            /* ODR: HAL does not write it; RM0399 12.4.6 does: BSRR set/reset. */
            if ((mode == GPIO_BM_MODE_OUTPUT) && (((GPIOF->ODR >> pd) & 1U) != level))
            {
                st.odr_bad++;
            }
        }
    }

    /* Legacy calls with the consumers' arguments, against the ca569b6 bodies. */
    for (uint32_t pr = 0U; pr < 2U; pr++)
    for (uint32_t sv = 0U; sv < 2U; sv++)
    {
        uint32_t pd = pairs[pr][0];
        uint32_t ph = pairs[pr][1];

        pin_force(GPIOF, pd, &k_t2_start[sv]);
        pin_force(GPIOF, ph, &k_t2_start[sv]);
        bool ok_alt = GPIO_BM_ConfigAlternate(GPIOF, pd, 7U, GPIO_BM_PULL_UP, GPIO_BM_SPEED_VERY_HIGH);
        ref_ca569b6_config_alternate(GPIOF, ph, 7U, GPIO_BM_PULL_UP, GPIO_BM_SPEED_VERY_HIGH);
        PinFields_t fd = pin_fields(GPIOF, pd), fh = pin_fields(GPIOF, ph);
        if (!ok_alt || !fields_equal(&fd, &fh))
        {
            t2_report_mismatch("legacy uart AF7", pd, &fd, &fh, &st);
            st.legacy_bad++;
        }

        pin_force(GPIOF, pd, &k_t2_start[sv]);
        pin_force(GPIOF, ph, &k_t2_start[sv]);
        bool ok_an = GPIO_BM_ConfigAnalog(GPIOF, pd);
        ref_ca569b6_config_analog(GPIOF, ph);
        fd = pin_fields(GPIOF, pd);
        fh = pin_fields(GPIOF, ph);
        if (!ok_an || !fields_equal(&fd, &fh))
        {
            t2_report_mismatch("legacy adc analog", pd, &fd, &fh, &st);
            st.legacy_bad++;
        }
    }

    /* Control: an image altered on purpose must be seen as different. */
    bool control_detected = false;
    if (have_last)
    {
        PinFields_t altered = last_ref;
        altered.ospeedr ^= 1U;
        control_detected = !fields_equal(&last_drv, &altered);
    }

    /* Back to the reset state (analog) for the unwired pins. */
    for (uint32_t pin = 6U; pin <= 9U; pin++)
    {
        (void)GPIO_BM_DeinitPin(GPIOF, pin);
    }

    bool cm7_pass = (st.mismatches == 0U) && (st.odr_bad == 0U) && (st.legacy_bad == 0U) &&
                    control_detected && (st.compared > 0U);
    APP_LOG("[m7_gpio_test] INFO T2 CM7 vs HAL_GPIO_Init: combos=%lu compared=%lu refused(analog+pull)=%lu",
            (unsigned long)st.combos, (unsigned long)st.compared, (unsigned long)st.refused);
    APP_LOG("[m7_gpio_test] INFO T2 CM7: mismatches=%lu odr_bad=%lu legacy_bad=%lu control=%s -> %s",
            (unsigned long)st.mismatches, (unsigned long)st.odr_bad, (unsigned long)st.legacy_bad,
            control_detected ? "detected" : "MISSED", cm7_pass ? "PASS" : "FAIL");

    bool cm4_ok = cm4_run_parked(GPIO_ST_CMD_T2);
    volatile GpioSelfTestShared_t *sh = GPIO_TEST_SHARED;
    APP_LOG("[m7_gpio_test] INFO T2 CM4 vs RM table: combos=%lu mismatches=%lu control=%s -> %s%s",
            (unsigned long)sh->t2_cm4_combos, (unsigned long)sh->t2_cm4_mismatches,
            (sh->t2_cm4_control_detected != 0U) ? "detected" : "MISSED",
            (sh->t2_cm4_pass != 0U) ? "PASS" : "FAIL", cm4_ok ? "" : " (no ack)");

    bool pass = cm7_pass && cm4_ok && (sh->t2_cm4_pass != 0U);
    APP_LOG("[m7_gpio_test] T2 VERDICT: %s", pass ? "PASS" : "FAIL");
    return pass;
}

/* ---- B1: measurements, recorded only ------------------------------------------ */

#define B1_SAMPLES      1000U

static void b1_measure(bool with_hook, uint32_t *mn, uint32_t *mx)
{
    *mn = 0xFFFFFFFFU;
    *mx = 0U;
    GPIO_BM_SetSharedLock(with_hook ? &k_t15_hook : NULL);
    for (uint32_t i = 0U; i < B1_SAMPLES; i++)
    {
        const GpioBm_PinConfig_t *cfg = ((i & 1U) != 0U) ? &k_pf10_out : &k_pf10_in;
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        uint32_t t0 = DWT->CYCCNT;
        (void)GPIO_BM_ConfigPin(cfg);
        uint32_t dt = DWT->CYCCNT - t0;
        __set_PRIMASK(pm);
        if (dt < *mn) { *mn = dt; }
        if (dt > *mx) { *mx = dt; }
    }
    GPIO_BM_SetSharedLock(NULL);
}

/* Cycles -> hundredths of a microsecond, for printing without floats. */
static uint32_t cyc_to_cus(uint32_t cycles, uint32_t cycles_per_us)
{
    return (uint32_t)(((uint64_t)cycles * 100U) / cycles_per_us);
}

static void b1_print(const char *what, uint32_t cycles, uint32_t cycles_per_us)
{
    uint32_t cus = cyc_to_cus(cycles, cycles_per_us);
    APP_LOG("[m7_gpio_test] INFO B1 %s: %lu cycles = %lu.%02lu us",
            what, (unsigned long)cycles, (unsigned long)(cus / 100U), (unsigned long)(cus % 100U));
}

static bool test_b1_measure(void)
{
    bench_enter_step(GPIO_BENCH_STEP_B1_MEASURE);

    if (!bench_claim_output(GPIOF, 10U))
    {
        APP_LOG("[m7_gpio_test] B1 DONE: INCOMPLETE");
        return false;
    }

    uint32_t nh_min, nh_max, h_min, h_max;
    b1_measure(false, &nh_min, &nh_max);
    b1_measure(true, &h_min, &h_max);
    GPIOF->MODER &= ~(3UL << PF10_SHIFT);

    b1_print("CM7 ConfigPin, no hook, max", nh_max, CM7_CYCLES_PER_US);
    b1_print("CM7 ConfigPin, no hook, min", nh_min, CM7_CYCLES_PER_US);
    b1_print("CM7 ConfigPin, hook, max", h_max, CM7_CYCLES_PER_US);
    b1_print("CM7 ConfigPin, hook, min", h_min, CM7_CYCLES_PER_US);
    b1_print("CM7 T15(e) lock hold max", s_b1_hold_max_e, CM7_CYCLES_PER_US);
    b1_print("CM7 T15(e) lock() entry to unlock max", s_b1_primask_max_e, CM7_CYCLES_PER_US);

    bool cm4_ok = cm4_run_parked(GPIO_ST_CMD_B1);
    volatile GpioSelfTestShared_t *sh = GPIO_TEST_SHARED;
    const uint32_t c4 = 200U;
    b1_print("CM4 ConfigPin, no hook, max", sh->b1_cm4_cfg_nohook_max, c4);
    b1_print("CM4 ConfigPin, no hook, min", sh->b1_cm4_cfg_nohook_min, c4);
    b1_print("CM4 ConfigPin, hook, max", sh->b1_cm4_cfg_hook_max, c4);
    b1_print("CM4 ConfigPin, hook, min", sh->b1_cm4_cfg_hook_min, c4);
    b1_print("CM4 T15(e) lock hold max", sh->b1_cm4_hold_max, c4);
    b1_print("CM4 T15(e) lock() entry to unlock max", sh->b1_cm4_primask_max, c4);

    bool complete = cm4_ok && (sh->b1_cm4_done != 0U) && (nh_max != 0U) && (h_max != 0U) &&
                    (s_b1_hold_max_e != 0U) && (sh->b1_cm4_hold_max != 0U);
    APP_LOG("[m7_gpio_test] B1 DONE: %s", complete ? "COMPLETE" : "INCOMPLETE");
    return complete;
}

/* ---- T16: LCKR, the last test of the image -------------------------------------- */

static bool test_t16_lckr(void)
{
    bool pass = true;
    GpioPhoto_t *b = &s_ph_before;
    GpioPhoto_t *a = &s_ph_after;

    bench_enter_step(GPIO_BENCH_STEP_T16_LCKR);

    for (uint32_t pin = 6U; pin <= 9U; pin++)
    {
        pass = pass && bench_claim_output(GPIOF, pin);
    }
    if (!pass)
    {
        APP_LOG("[m7_gpio_test] INFO T16 bench_claim_output rejected PF6..PF9");
        APP_LOG("[m7_gpio_test] T16 VERDICT: FAIL");
        return false;
    }

    if (s_lckr_boot != 0U)
    {
        APP_LOG("[m7_gpio_test] INFO T16 GPIOF was already locked at boot (0x%08lX): not a valid run",
                (unsigned long)s_lckr_boot);
        pass = false;
    }

    /* Known state before locking: input, pull-down, so an ignored OUTPUT
       write would be visible in MODER, PUPDR and ODR. */
    for (uint32_t pin = 6U; pin <= 9U; pin++)
    {
        (void)GPIO_BM_ConfigInput(GPIOF, pin, GPIO_BM_PULL_DOWN);
    }

    const uint32_t mask = (1UL << 7U) | (1UL << 8U) | (1UL << 9U);
    GpioBm_Status_t lk = GPIO_BM_LockPins(GPIOF, mask);
    uint32_t lckr = GPIOF->LCKR;
    APP_LOG("[m7_gpio_test] INFO T16 LockPins(PF7..PF9) -> %u, LCKR=0x%08lX", (unsigned int)lk, (unsigned long)lckr);
    if ((lk != GPIO_BM_OK) || ((lckr & GPIO_LCKR_LCKK) == 0U) || ((lckr & mask) != mask))
    {
        pass = false;
    }

    for (uint32_t pin = 7U; pin <= 9U; pin++)
    {
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        photo_take(b);
        GpioBm_Status_t s1 = GPIO_BM_ConfigOutput(GPIOF, pin, GPIO_BM_OTYPE_OD, GPIO_BM_SPEED_HIGH, GPIO_BM_PULL_UP, 1U);
        GpioBm_Status_t s2 = GPIO_BM_DeinitPin(GPIOF, pin);
        bool            s3 = GPIO_BM_ConfigAnalog(GPIOF, pin);
        bool            s4 = GPIO_BM_IsLocked(GPIOF, pin);
        photo_take(a);
        __set_PRIMASK(pm);
        bool same = photo_equal(b, a);
        bool ok = (s1 == GPIO_BM_ERR_LOCKED) && (s2 == GPIO_BM_ERR_LOCKED) && !s3 && s4 && same;
        APP_LOG("[m7_gpio_test] INFO T16 PF%lu: ConfigOutput=%u DeinitPin=%u ConfigAnalog=%u IsLocked=%u photo=%s %s",
                (unsigned long)pin, (unsigned int)s1, (unsigned int)s2, (unsigned int)s3, (unsigned int)s4,
                same ? "same" : "CHANGED", ok ? "OK" : "FAIL");
        pass = pass && ok;
    }

    /* Control: PF6 is not locked and does change. */
    {
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        photo_take(b);
        GpioBm_Status_t s = GPIO_BM_ConfigOutput(GPIOF, 6U, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 0U);
        bool locked6 = GPIO_BM_IsLocked(GPIOF, 6U);
        photo_take(a);
        __set_PRIMASK(pm);
        bool changed = !photo_equal(b, a);
        bool ok = (s == GPIO_BM_OK) && changed && !locked6;
        APP_LOG("[m7_gpio_test] INFO T16 control PF6 (not locked): ConfigOutput=%u IsLocked=%u photo=%s %s",
                (unsigned int)s, (unsigned int)locked6, changed ? "changed" : "SAME", ok ? "OK" : "FAIL");
        pass = pass && ok;
        (void)GPIO_BM_DeinitPin(GPIOF, 6U);
    }

    lckr = GPIOF->LCKR;
    APP_LOG("[m7_gpio_test] INFO T16 LCKK=%lu (stays set until the next reset)",
            (unsigned long)((lckr & GPIO_LCKR_LCKK) != 0U));
    APP_LOG("[m7_gpio_test] T16 VERDICT: %s", pass ? "PASS" : "FAIL");
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

    /* (5) Bus rejection when step has bus_owner=AD2 (queried via pure bench_step_allows) */
    if (bench_step_allows(GPIO_BENCH_STEP_T0A_AD2_TO_MCU, GPIOE, 8U)) pass = false;

    /* (6) PA8 accepted in T0A_MCU_TO_AD2 step (queried via pure bench_step_allows) */
    if (!bench_step_allows(GPIO_BENCH_STEP_T0A_MCU_TO_AD2, GPIOA, 8U)) pass = false;

    /* (7) All-or-nothing check in bench_claim_pins (M2, m1) */
    uint32_t moder_snap = GPIOE->MODER;
    uint32_t otyper_snap = GPIOE->OTYPER;
    uint32_t afr0_snap = GPIOE->AFR[0];
    uint32_t afr1_snap = GPIOE->AFR[1];

    /* Mask with PE7 (allowed in current step T0B_GUARDS) and PE4 (forbidden): must fail and touch nothing */
    uint32_t test_mask = (1U << 7U) | (1U << 4U);
    if (bench_claim_pins(GPIOE, test_mask, 1U, 0U))
    {
        pass = false; /* Must fail */
    }
    if (GPIOE->MODER != moder_snap || GPIOE->OTYPER != otyper_snap ||
        GPIOE->AFR[0] != afr0_snap || GPIOE->AFR[1] != afr1_snap)
    {
        pass = false; /* Registers were modified despite rejection! */
    }

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
        APP_LOG("[m7_gpio_test] T0b_c safe state verification FAILED: pins not input");
    }
    else
    {
        APP_LOG("[m7_gpio_test] T0b_c safe state verified: all wired pins input");
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
    pupdr_e &= ~0xFFFF3FC0UL;
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
    pupdr_e &= ~0xFFFF3FC0UL;
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

    /* MCU->AD2 Negative Control: ODR set with pins in input with pull-down (R-m3) */
    GPIOE->MODER &= ~0xFFFF3FC0UL; /* PE3..PE6, PE8..PE15 input (preserves PE2 and PE7) */
    GPIOE->PUPDR = (GPIOE->PUPDR & ~0xFFFF3FC0UL) | 0xAAAA2A80UL; /* pull-down (preserves PE2 pull-down) */
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

/* ==========================================================================
 * Tests with the AD2 (F1a): T3, T4, T7, T8, T5, T6, T9 and T10.
 * Every step follows its row of GPIO_BENCH_PROTOCOL_TABLE; every output or
 * AF configuration of a wired pin is preceded by bench_claim_output() for the
 * current step (S4); every bus handover runs the S5 procedure first.
 * ========================================================================== */

/* Busy wait on this core's DWT cycle counter. */
static void delay_cycles(uint32_t cycles)
{
    uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < cycles)
    {
    }
}

/* S5 point 1, shared by every step that hands the bus over: pull-up, the host
   reads 0xFF; pull-down, the host reads 0x00. The MCU checks its IDR too. */
static bool bench_buschk(void)
{
    uint32_t pupdr_e = GPIOE->PUPDR;
    pupdr_e &= ~0xFFFF0000UL;
    pupdr_e |= 0x55550000UL;
    GPIOE->PUPDR = pupdr_e;
    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    uint32_t idr_up = (GPIOE->IDR >> 8U) & 0xFFU;
    APP_LOG("[m7_gpio_test] BUSCHK up");
    if (!hs_mcu()) { return false; }

    pupdr_e = GPIOE->PUPDR;
    pupdr_e &= ~0xFFFF0000UL;
    pupdr_e |= 0xAAAA0000UL;
    GPIOE->PUPDR = pupdr_e;
    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    uint32_t idr_down = (GPIOE->IDR >> 8U) & 0xFFU;
    APP_LOG("[m7_gpio_test] BUSCHK down");
    if (!hs_mcu()) { return false; }

    if ((idr_up != 0xFFU) || (idr_down != 0x00U))
    {
        bench_safe();
        APP_LOG("[m7_gpio_test] ABORT step=%u reason=buschk idr up=0x%02lX down=0x%02lX",
                (unsigned int)s_current_step_id, (unsigned long)idr_up, (unsigned long)idr_down);
        return false;
    }
    return true;
}

/* A HS_HOST whose action runs between HOST_REQ rising and MCU_ACK rising. */

static bool hs_host_act(uint32_t timeout_ms, BenchAction_t act, void *ctx)
{
    uint8_t expected = bench_get_expected_primitive(s_current_step_id, s_step_prim_idx);
    if (expected != GPIO_BENCH_PRIM_HS_HOST)
    {
        bench_safe();
        APP_LOG("[m7_gpio_test] ABORT step=%u reason=protocol", (unsigned int)s_current_step_id);
        return false;
    }
    s_step_prim_idx++;

    if (!wait_host_req(true, timeout_ms))
    {
        bench_safe();
        APP_LOG("[m7_gpio_test] ABORT step=%u reason=timeout", (unsigned int)s_current_step_id);
        return false;
    }
    if (act != NULL)
    {
        act(ctx);
    }
    set_mcu_ack(true);
    if (!wait_host_req(false, GPIO_BENCH_TIMEOUT_MS))
    {
        set_mcu_ack(false);
        bench_safe();
        APP_LOG("[m7_gpio_test] ABORT step=%u reason=timeout", (unsigned int)s_current_step_id);
        return false;
    }
    set_mcu_ack(false);
    return true;
}

/* Output configuration of a wired pin: the S4 claim, then the driver. */
static bool bench_cfg_output(GPIO_TypeDef *port, uint32_t pin, uint32_t otype, uint32_t speed,
                             uint32_t pull, uint32_t level)
{
    if (!bench_claim_output(port, pin))
    {
        return false;
    }
    return GPIO_BM_ConfigOutput(port, pin, otype, speed, pull, level) == GPIO_BM_OK;
}

/* ---- T3: Set / Reset / Write / WritePort / Toggle on the bus (bus = MCU) ---- */

#define T3_HOLD_CYCLES      (2U * CM7_CYCLES_PER_US)   /* each value held 2 us */

typedef struct
{
    bool corrupt;   /* the control: one value wrong on purpose */
} T3Ctx_t;

static void t3_hold(void)
{
    delay_cycles(T3_HOLD_CYCLES);
}

/* The sequence the host expects is written out in gpio_hw_check.py
   (T3_EXPECTED); the two are kept apart on purpose, so a mistake on one side
   shows up as a mismatch. */
static void t3_burst(void *ctx)
{
    const T3Ctx_t *c = (const T3Ctx_t *)ctx;
    uint32_t pm = __get_PRIMASK();
    __disable_irq();

    /* Walking 1 with Set / Reset */
    for (uint32_t b = 0U; b < 8U; b++)
    {
        GPIO_BM_Set(GPIOE, 1UL << (8U + b));   t3_hold();
        GPIO_BM_Reset(GPIOE, 1UL << (8U + b)); t3_hold();
    }
    /* Walking 0 with Write */
    GPIO_BM_WritePort(GPIOE, 0xFF00U, 0xFF00U); t3_hold();
    for (uint32_t b = 0U; b < 8U; b++)
    {
        GPIO_BM_Write(GPIOE, 8U + b, 0U); t3_hold();
        GPIO_BM_Write(GPIOE, 8U + b, 1U); t3_hold();
    }
    /* WritePort, full and partial masks */
    GPIO_BM_WritePort(GPIOE, 0xFF00U, c->corrupt ? 0x5400U : 0x5500U); t3_hold();
    GPIO_BM_WritePort(GPIOE, 0xFF00U, 0xAA00U); t3_hold();
    GPIO_BM_WritePort(GPIOE, 0xFF00U, 0x0F00U); t3_hold();
    GPIO_BM_WritePort(GPIOE, 0xFF00U, 0xF000U); t3_hold();
    GPIO_BM_WritePort(GPIOE, 0xFF00U, 0x0000U); t3_hold();
    GPIO_BM_WritePort(GPIOE, 0x0F00U, 0x0500U); t3_hold();
    GPIO_BM_WritePort(GPIOE, 0xF000U, 0xA000U); t3_hold();
    GPIO_BM_WritePort(GPIOE, 0xFF00U, 0x0000U); t3_hold();
    /* Toggle */
    GPIO_BM_Toggle(GPIOE, 0xFF00U); t3_hold();
    GPIO_BM_Toggle(GPIOE, 0x0F00U); t3_hold();
    GPIO_BM_Toggle(GPIOE, 0xF000U); t3_hold();
    /* Set and reset of the same pin in one BSRR write: set wins (RM0399 12.4.7) */
    GPIOE->BSRR = (1UL << 8U) | (1UL << (8U + 16U));   t3_hold();
    GPIOE->BSRR = (1UL << 15U) | (1UL << (15U + 16U)); t3_hold();
    GPIO_BM_Reset(GPIOE, 0xFF00U); t3_hold();

    __set_PRIMASK(pm);
}

static bool test_t3_mcu_ops(void)
{
    bench_enter_step(GPIO_BENCH_STEP_T3_MCU_OPS);
    if (!bench_buschk()) { return false; }

    /* S5 point 3: bus proven loose in this step; 1 ms, then drive. */
    HAL_Delay(GPIO_BENCH_DEADTIME_MS);
    GPIOE->BSRR = 0xFF00UL << 16U;               /* level low before MODER (C11) */
    if (!bench_claim_pins(GPIOE, 0xFF00UL, 1U, 0U))
    {
        bench_safe();
        return false;
    }
    GPIOE->PUPDR &= ~0xFFFF0000UL;

    T3Ctx_t ctx = { .corrupt = false };
    APP_LOG("[m7_gpio_test] T3 ready");
    if (!hs_host_act(GPIO_BENCH_TIMEOUT_MS, t3_burst, &ctx)) { return false; }

    ctx.corrupt = true;
    APP_LOG("[m7_gpio_test] T3 control ready");
    if (!hs_host_act(GPIO_BENCH_TIMEOUT_MS, t3_burst, &ctx)) { return false; }

    bench_safe();
    APP_LOG("[m7_gpio_test] T3: DONE (host verdict)");
    return true;
}

/* ---- T4: 256 patterns from the AD2, ReadPort and Read (bus = AD2) ----------- */

static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (uint32_t k = 0U; k < 8U; k++)
    {
        crc = ((crc & 1U) != 0U) ? ((crc >> 1U) ^ 0xEDB88320UL) : (crc >> 1U);
    }
    return crc;
}

typedef struct
{
    uint32_t count;
    uint32_t crc_port;
    uint32_t crc_bits;
} T4Ctx_t;

static void t4_sample(void *ctx)
{
    T4Ctx_t *c = (T4Ctx_t *)ctx;
    uint8_t by_port = (uint8_t)((GPIO_BM_ReadPort(GPIOE) >> 8U) & 0xFFU);
    uint8_t by_bits = 0U;
    for (uint32_t b = 0U; b < 8U; b++)
    {
        if (GPIO_BM_Read(GPIOE, 8U + b))
        {
            by_bits |= (uint8_t)(1U << b);
        }
    }
    c->crc_port = crc32_update(c->crc_port, by_port);
    c->crc_bits = crc32_update(c->crc_bits, by_bits);
    c->count++;
}

static bool t4_pass(uint32_t pass)
{
    T4Ctx_t c = { 0U, 0xFFFFFFFFUL, 0xFFFFFFFFUL };
    for (uint32_t i = 0U; i < 256U; i++)
    {
        if (!hs_host_act(GPIO_BENCH_TIMEOUT_MS, t4_sample, &c)) { return false; }
    }
    APP_LOG("[m7_gpio_test] T4 pass=%lu count=%lu crc_port=0x%08lX crc_bits=0x%08lX",
            (unsigned long)pass, (unsigned long)c.count,
            (unsigned long)(c.crc_port ^ 0xFFFFFFFFUL), (unsigned long)(c.crc_bits ^ 0xFFFFFFFFUL));
    return true;
}

/* S5 point 2: the bus stays in input (pull-down from BUSCHK); MODER read
   back before MCU_ACK tells the host it may drive. */
static bool bench_bus_released_for_ad2(uint32_t mode_field)
{
    uint32_t moder = GPIOE->MODER & 0xFFFF0000UL;
    uint32_t want  = (mode_field == 0U) ? 0U : 0xFFFF0000UL;
    if (moder != want)
    {
        bench_safe();
        APP_LOG("[m7_gpio_test] ABORT step=%u reason=moder 0x%08lX", (unsigned int)s_current_step_id,
                (unsigned long)moder);
        return false;
    }
    return true;
}

static bool test_t4_ad2_patterns(void)
{
    bench_enter_step(GPIO_BENCH_STEP_T4_AD2_PATTERNS);
    if (!bench_buschk()) { return false; }
    if (!bench_bus_released_for_ad2(0U)) { return false; }

    APP_LOG("[m7_gpio_test] T4 ready");
    if (!hs_mcu()) { return false; }
    if (!t4_pass(1U)) { return false; }
    if (!t4_pass(2U)) { return false; }
    /* The host has set OE = 0 (read back) before this last strobe. */
    if (!hs_host_act(GPIO_BENCH_TIMEOUT_MS, NULL, NULL)) { return false; }
    bench_safe();
    APP_LOG("[m7_gpio_test] T4: DONE (host verdict)");
    return true;
}

/* ---- T7: IDR in analog, then in input, while the AD2 drives 0xFF (bus = AD2) ---- */

static void t7_sample(void *ctx)
{
    *(uint32_t *)ctx = (GPIOE->IDR >> 8U) & 0xFFU;
}

static bool test_t7_analog_idr(void)
{
    uint32_t idr = 0U;
    bool cfg_ok = true;

    bench_enter_step(GPIO_BENCH_STEP_T7_ANALOG_IDR);
    if (!bench_buschk()) { return false; }

    /* Analog through the driver: not an output, no claim needed (S4). */
    for (uint32_t pin = 8U; pin <= 15U; pin++)
    {
        cfg_ok = cfg_ok && GPIO_BM_ConfigAnalog(GPIOE, pin);
    }
    if (!cfg_ok || !bench_bus_released_for_ad2(3U)) { bench_safe(); return false; }

    APP_LOG("[m7_gpio_test] T7 analog ready");
    if (!hs_mcu()) { return false; }
    if (!hs_host_act(GPIO_BENCH_TIMEOUT_MS, t7_sample, &idr)) { return false; }
    APP_LOG("[m7_gpio_test] T7 analog idr=0x%02lX", (unsigned long)idr);

    for (uint32_t pin = 8U; pin <= 15U; pin++)
    {
        cfg_ok = cfg_ok && (GPIO_BM_ConfigInput(GPIOE, pin, GPIO_BM_PULL_NONE) == GPIO_BM_OK);
    }
    if (!cfg_ok) { bench_safe(); return false; }
    APP_LOG("[m7_gpio_test] T7 input ready");
    if (!hs_host_act(GPIO_BENCH_TIMEOUT_MS, t7_sample, &idr)) { return false; }
    APP_LOG("[m7_gpio_test] T7 input idr=0x%02lX", (unsigned long)idr);

    if (!hs_host_act(GPIO_BENCH_TIMEOUT_MS, NULL, NULL)) { return false; }
    bench_safe();
    APP_LOG("[m7_gpio_test] T7: DONE (host verdict)");
    return true;
}

/* ---- T8: input -> output on PE5 without glitch (bus = NONE) ------------------- */

typedef struct
{
    char     name;
    uint32_t pull;          /* input pull before the switch */
    uint32_t stale_odr;     /* ODR left opposite on purpose where it matters */
    uint32_t target;        /* output level */
    bool     naive;         /* control: MODER first, 1 us, then BSRR */
} T8Case_t;

/* A and B change level: exactly one edge. C and D keep the level with a stale
   ODR: no edge at all, which is what shows the driver writes BSRR first.
   E is C done in the naive order: the stale ODR must show as a pulse. */
static const T8Case_t k_t8_cases[5] = {
    { 'A', GPIO_BM_PULL_DOWN, 0U, 1U, false },
    { 'B', GPIO_BM_PULL_UP,   1U, 0U, false },
    { 'C', GPIO_BM_PULL_UP,   0U, 1U, false },
    { 'D', GPIO_BM_PULL_DOWN, 1U, 0U, false },
    { 'E', GPIO_BM_PULL_UP,   0U, 1U, true  },
};

static void t8_apply(void *ctx)
{
    const T8Case_t *tc = (const T8Case_t *)ctx;
    if (tc->naive)
    {
        (void)bench_set_pin_mode(GPIOE, 5U, 1U, 0U);           /* MODER first */
        delay_cycles(1U * CM7_CYCLES_PER_US);
        GPIOE->BSRR = (tc->target != 0U) ? (1UL << 5U) : (1UL << (5U + 16U));
    }
    else
    {
        (void)bench_cfg_output(GPIOE, 5U, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW,
                               GPIO_BM_PULL_NONE, tc->target);
    }
}

static bool test_t8_no_glitch(void)
{
    bench_enter_step(GPIO_BENCH_STEP_T8_NO_GLITCH);

    for (uint32_t i = 0U; i < 5U; i++)
    {
        const T8Case_t *tc = &k_t8_cases[i];
        if (!bench_claim_output(GPIOE, 5U)) { bench_safe(); return false; }
        (void)GPIO_BM_ConfigInput(GPIOE, 5U, tc->pull);
        GPIOE->BSRR = (tc->stale_odr != 0U) ? (1UL << 5U) : (1UL << (5U + 16U));
        HAL_Delay(2U);                                          /* let the pull settle */

        APP_LOG("[m7_gpio_test] T8 case=%c ready", tc->name);
        if (!hs_host_act(GPIO_BENCH_TIMEOUT_MS, t8_apply, (void *)tc)) { return false; }
        (void)GPIO_BM_ConfigInput(GPIOE, 5U, GPIO_BM_PULL_NONE);
    }

    bench_safe();
    APP_LOG("[m7_gpio_test] T8: DONE (host verdict)");
    return true;
}

/* ---- T5: pull resistances on PF14 / PF15 (bus = NONE) --------------------------- */

static bool test_t5_pulls(void)
{
    bench_enter_step(GPIO_BENCH_STEP_T5_PULLS);

    /* V_DD: PF15 push-pull high (its 22k goes to 3V3, so no current). */
    if (!bench_cfg_output(GPIOF, 15U, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 1U) ||
        (GPIO_BM_ConfigInput(GPIOF, 14U, GPIO_BM_PULL_NONE) != GPIO_BM_OK))
    {
        bench_safe();
        return false;
    }
    HAL_Delay(2U);
    APP_LOG("[m7_gpio_test] T5 vdd ready");
    if (!hs_mcu()) { return false; }

    /* No pull: PF14 sits on its 22k to GND, PF15 on its 22k to 3V3 against the scope load. */
    if ((GPIO_BM_ConfigInput(GPIOF, 14U, GPIO_BM_PULL_NONE) != GPIO_BM_OK) ||
        (GPIO_BM_ConfigInput(GPIOF, 15U, GPIO_BM_PULL_NONE) != GPIO_BM_OK))
    {
        bench_safe();
        return false;
    }
    HAL_Delay(2U);
    APP_LOG("[m7_gpio_test] T5 nopull ready");
    if (!hs_mcu()) { return false; }

    if ((GPIO_BM_ConfigInput(GPIOF, 14U, GPIO_BM_PULL_UP) != GPIO_BM_OK) ||
        (GPIO_BM_ConfigInput(GPIOF, 15U, GPIO_BM_PULL_DOWN) != GPIO_BM_OK))
    {
        bench_safe();
        return false;
    }
    HAL_Delay(2U);
    APP_LOG("[m7_gpio_test] T5 pulls ready");
    if (!hs_mcu()) { return false; }

    bench_safe();
    APP_LOG("[m7_gpio_test] T5: DONE (host verdict)");
    return true;
}

/* ---- T6: open-drain (bus = NONE) ------------------------------------------------- */

static bool test_t6_open_drain(void)
{
    bench_enter_step(GPIO_BENCH_STEP_T6_OPEN_DRAIN);

    /* (a) PF15 like an I2C line: OD low; (b) PF14 on its 22k to GND: OD released. */
    if (!bench_cfg_output(GPIOF, 15U, GPIO_BM_OTYPE_OD, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 0U) ||
        !bench_cfg_output(GPIOF, 14U, GPIO_BM_OTYPE_OD, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 1U))
    {
        bench_safe();
        return false;
    }
    HAL_Delay(2U);
    APP_LOG("[m7_gpio_test] T6 od low ready");
    if (!hs_mcu()) { return false; }

    GPIO_BM_Set(GPIOF, 1UL << 15U);                  /* (a) OD released */
    HAL_Delay(2U);
    APP_LOG("[m7_gpio_test] T6 od released ready");
    if (!hs_mcu()) { return false; }

    /* Control: push-pull high on PF14 does reach the rail. */
    if (!bench_cfg_output(GPIOF, 14U, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 1U))
    {
        bench_safe();
        return false;
    }
    HAL_Delay(2U);
    APP_LOG("[m7_gpio_test] T6 control ready");
    if (!hs_mcu()) { return false; }

    bench_safe();
    APP_LOG("[m7_gpio_test] T6: DONE (host verdict)");
    return true;
}

/* ---- T9: rise time per OSPEEDR (bus = NONE) ---------------------------------------- */

static void t9_rise(void *ctx)
{
    (void)ctx;
    GPIOF->BSRR = (1UL << 14U) | (1UL << 15U);
}

static bool test_t9_rise_time(void)
{
    bench_enter_step(GPIO_BENCH_STEP_T9_RISE_TIME);

    for (uint32_t sp = 0U; sp < 4U; sp++)
    {
        if (!bench_cfg_output(GPIOF, 14U, GPIO_BM_OTYPE_PP, sp, GPIO_BM_PULL_NONE, 0U) ||
            !bench_cfg_output(GPIOF, 15U, GPIO_BM_OTYPE_PP, sp, GPIO_BM_PULL_NONE, 0U))
        {
            bench_safe();
            return false;
        }
        HAL_Delay(2U);
        APP_LOG("[m7_gpio_test] T9 speed=%lu ready", (unsigned long)sp);
        for (uint32_t e = 0U; e < 10U; e++)
        {
            if (!hs_host_act(GPIO_BENCH_TIMEOUT_MS, t9_rise, NULL)) { return false; }
            GPIOF->BSRR = ((1UL << 14U) | (1UL << 15U)) << 16U;
            HAL_Delay(1U);
        }
    }

    /* Control: pins in input; the same BSRR write makes no edge. */
    (void)GPIO_BM_ConfigInput(GPIOF, 14U, GPIO_BM_PULL_NONE);
    (void)GPIO_BM_ConfigInput(GPIOF, 15U, GPIO_BM_PULL_NONE);
    GPIOF->BSRR = ((1UL << 14U) | (1UL << 15U)) << 16U;
    HAL_Delay(2U);
    APP_LOG("[m7_gpio_test] T9 control ready");
    if (!hs_host_act(GPIO_BENCH_TIMEOUT_MS, t9_rise, NULL)) { return false; }

    bench_safe();
    APP_LOG("[m7_gpio_test] T9: DONE (host verdict)");
    return true;
}

/* ---- T10: MCO1 = HSI / 4 on PA8 (bus = NONE) ---------------------------------------- */

static uint32_t s_t10_cfgr_saved;
static bool     s_t10_cfgr_dirty;

/* Only the MCO1 fields are touched and restored; RCC_CFGR belongs to CPU1. */
static void t10_restore_mco(void)
{
    if (s_t10_cfgr_dirty)
    {
        const uint32_t mco = RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE;
        RCC->CFGR = (RCC->CFGR & ~mco) | (s_t10_cfgr_saved & mco);
        s_t10_cfgr_dirty = false;
    }
}

static bool test_t10_mco1(void)
{
    bench_enter_step(GPIO_BENCH_STEP_T10_MCO1);

    uint32_t hsi = ((RCC->CR & RCC_CR_HSIRDY) != 0U) ? 1U : 0U;
    s_t10_cfgr_saved = RCC->CFGR;
    s_t10_cfgr_dirty = true;
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE)) |
                (0U << RCC_CFGR_MCO1_Pos) |            /* HSI */
                (4U << RCC_CFGR_MCO1PRE_Pos);           /* / 4 -> 16 MHz */

    if (!bench_claim_output(GPIOA, 8U) ||
        (GPIO_BM_ConfigAlternateEx(GPIOA, 8U, 0U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE,
                                   GPIO_BM_SPEED_VERY_HIGH) != GPIO_BM_OK))
    {
        bench_safe();
        return false;
    }
    HAL_Delay(2U);
    APP_LOG("[m7_gpio_test] T10 mco ready hsi=%lu", (unsigned long)hsi);
    if (!hs_mcu()) { return false; }

    /* Control: AF1 (TIM1_CH1, timer not running) carries no clock. */
    if (!bench_claim_output(GPIOA, 8U) ||
        (GPIO_BM_ConfigAlternateEx(GPIOA, 8U, 1U, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE,
                                   GPIO_BM_SPEED_VERY_HIGH) != GPIO_BM_OK))
    {
        bench_safe();
        return false;
    }
    HAL_Delay(2U);
    APP_LOG("[m7_gpio_test] T10 control ready");
    if (!hs_mcu()) { return false; }

    bench_safe();                                       /* PA8 input, MCO1 restored */
    APP_LOG("[m7_gpio_test] T10: DONE (host verdict)");
    return true;
}

/* ---- CM7 Main Entry Point ------------------------------------------------ */

void GPIO_SelfTest_CM7_Run(void)
{
    /* 1. S8: Print Banner Rev <hash> */
    APP_LOG("[m7_gpio_test] Rev %s", FW_GIT_REV);

    /* Enforce S2 safe state initially */
    bench_safe();

    /* 2. Tests without the AD2, before host synchronization. CM4 takes part
          in each through the job channel and finishes after B1. */
    if (!test_t15_race())  goto test_failed;
    if (!test_t1_reject()) goto test_failed;
    if (!test_t2_parity()) goto test_failed;
    if (!test_b1_measure()) goto test_failed;

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

    /* 5. F1a tests with the AD2, in the order of GPIO_BENCH_STEPS_TABLE. */
    if (!test_t3_mcu_ops())        goto test_failed;
    if (!test_t4_ad2_patterns())   goto test_failed;
    if (!test_t7_analog_idr())     goto test_failed;
    if (!test_t8_no_glitch())      goto test_failed;
    if (!test_t5_pulls())          goto test_failed;
    if (!test_t6_open_drain())     goto test_failed;
    if (!test_t9_rise_time())      goto test_failed;
    if (!test_t10_mco1())          goto test_failed;

    /* 6. T16 last: LCKR stays set until the next reset. */
    if (!test_t16_lckr())          goto test_failed;

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
    Shared_GpioLockInstall();
    return;

test_failed:
    bench_safe();
    Shared_GpioLockInstall();
    APP_LOG("[m7_gpio_test] VERDICT: GPIO SELF-TESTS FAILED");
}
