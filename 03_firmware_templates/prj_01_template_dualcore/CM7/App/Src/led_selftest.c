/**
  ******************************************************************************
  * @file    led_selftest.c
  * @brief   01_led driver selftest implementation (preset selftest-led, CM7).
  *
  *          Tests LT1-LT5 according to 01_shared_drivers/01_led/IMPLEMENTATION_PLAN.md:
  *            LT1: L3 - validation, section, cross-core hook, read-back
  *            LT2: L5 - invalid port/pin handling and rejection
  *            LT3: L4 - atomic LED_Toggle vs periodic TIM17 ISR race condition
  *            LT4: G14 - LED_PanicRed exception direct RMW
  *            LT5: Final state restoration
  ******************************************************************************
  */

#include "led_selftest.h"
#include "led_bm.h"
#include "gpio_bm.h"
#include "board_def.h"
#include "app_log.h"
#include "fw_build_id.h"
#include "stm32h7xx.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LT3_N               200000UL
#define LT3_FREQ_HZ         200000UL     /* 200 kHz from 200 MHz APB2 timer clock (ARR=999) */

/* Forward declaration for LT3 local control function */
static void old_LED_Toggle(led_t led);

static volatile uint32_t s_isr_count = 0U;
static volatile bool     s_use_old_toggle = false;

void TIM17_IRQHandler(void)
{
    TIM17->SR = ~TIM_SR_UIF;
    if (s_use_old_toggle)
    {
        old_LED_Toggle(LED_GREEN);
    }
    else
    {
        LED_Toggle(LED_GREEN);
    }
    s_isr_count++;
    __DSB();
}

/* Local copy of g_led_config for the control function old_LED_Toggle */
static led_driver_config_t g_selftest_led_config = {
    .leds = {
        [LED_GREEN]  = { .port = BOARD_LED_GREEN_PORT,  .pin_num = BOARD_LED_GREEN_BIT },
        [LED_YELLOW] = { .port = BOARD_LED_YELLOW_PORT, .pin_num = BOARD_LED_YELLOW_BIT },
        [LED_RED]    = { .port = BOARD_LED_RED_PORT,    .pin_num = BOARD_LED_RED_BIT }
    }
};

/* Local copy of LED_Toggle from commit 0927c75 (without PRIMASK masking) */
static void old_LED_Toggle(led_t led)
{
    if ((uint32_t)led >= (uint32_t)LED_COUNT)
    {
        return;
    }

    GPIO_TypeDef *port = g_selftest_led_config.leds[led].port;
    uint32_t bit = g_selftest_led_config.leds[led].pin_num;

    if (port == NULL)
    {
        return;
    }

    if (port->ODR & (1UL << bit))
    {
        port->BSRR = (1UL << (bit + 16U));
    }
    else
    {
        port->BSRR = (1UL << bit);
    }
}

/* Shared lock hooks for LT1 */
static volatile uint32_t s_lt1_lock_count = 0U;
static volatile uint32_t s_lt1_unlock_count = 0U;

static bool lt1_lock_true(void *ctx)
{
    (void)ctx;
    s_lt1_lock_count++;
    return true;
}

static void lt1_unlock(void *ctx)
{
    (void)ctx;
    s_lt1_unlock_count++;
}

static bool lt1_lock_false(void *ctx)
{
    (void)ctx;
    s_lt1_lock_count++;
    return false;
}

static void take_gpio_snapshot_15(uint32_t out[15])
{
    out[0]  = GPIOA->MODER;
    out[1]  = GPIOA->OTYPER;
    out[2]  = GPIOA->OSPEEDR;
    out[3]  = GPIOA->PUPDR;
    out[4]  = GPIOA->ODR;

    out[5]  = GPIOB->MODER;
    out[6]  = GPIOB->OTYPER;
    out[7]  = GPIOB->OSPEEDR;
    out[8]  = GPIOB->PUPDR;
    out[9]  = GPIOB->ODR;

    out[10] = GPIOE->MODER;
    out[11] = GPIOE->OTYPER;
    out[12] = GPIOE->OSPEEDR;
    out[13] = GPIOE->PUPDR;
    out[14] = GPIOE->ODR;
}

void LED_Selftest_Run(void)
{
    App_Log_Init();

    APP_LOG(" ");
    APP_LOG("=== STM32H755ZI 01_led driver selftest - CPU1 (Cortex-M7) ===");
    APP_LOG("[sys] build rev=%s id=%s", FW_GIT_REV, FW_BUILD_ID);

    bool fw_pass = true;

    /* ---- LT1 (L3): Section, lock hook and read-back ------------------------ */
    (void)GPIO_BM_DeinitPin(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT);
    (void)GPIO_BM_DeinitPin(BOARD_LED_YELLOW_PORT, BOARD_LED_YELLOW_BIT);
    (void)GPIO_BM_DeinitPin(BOARD_LED_RED_PORT, BOARD_LED_RED_BIT);

    const GpioBm_SharedLock_t hook_true = {
        .lock = lt1_lock_true,
        .unlock = lt1_unlock,
        .ctx = NULL
    };
    GPIO_BM_SetSharedLock(&hook_true);

    s_lt1_lock_count = 0U;
    s_lt1_unlock_count = 0U;

    const led_driver_config_t board_cfg = {
        .leds = {
            [LED_GREEN]  = { .port = BOARD_LED_GREEN_PORT,  .pin_num = BOARD_LED_GREEN_BIT },
            [LED_YELLOW] = { .port = BOARD_LED_YELLOW_PORT, .pin_num = BOARD_LED_YELLOW_BIT },
            [LED_RED]    = { .port = BOARD_LED_RED_PORT,    .pin_num = BOARD_LED_RED_BIT }
        }
    };

    uint32_t mask_lt1 = LED_Init_ConfigEx(&board_cfg);

    GpioBm_PinConfig_t cfg_g, cfg_y, cfg_r;
    (void)GPIO_BM_ReadConfig(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT, &cfg_g);
    (void)GPIO_BM_ReadConfig(BOARD_LED_YELLOW_PORT, BOARD_LED_YELLOW_BIT, &cfg_y);
    (void)GPIO_BM_ReadConfig(BOARD_LED_RED_PORT, BOARD_LED_RED_BIT, &cfg_r);

    uint32_t odr_g = (BOARD_LED_GREEN_PORT->ODR >> BOARD_LED_GREEN_BIT) & 1U;
    uint32_t odr_y = (BOARD_LED_YELLOW_PORT->ODR >> BOARD_LED_YELLOW_BIT) & 1U;
    uint32_t odr_r = (BOARD_LED_RED_PORT->ODR >> BOARD_LED_RED_BIT) & 1U;

    APP_LOG("[led] LT1: mask=0x%02lX lock=%lu unlock=%lu",
            (unsigned long)mask_lt1, (unsigned long)s_lt1_lock_count, (unsigned long)s_lt1_unlock_count);
    APP_LOG("[led] LT1_PINS: G(mode=%lu,otype=%lu,speed=%lu,pull=%lu,odr=%lu) "
            "Y(mode=%lu,otype=%lu,speed=%lu,pull=%lu,odr=%lu) "
            "R(mode=%lu,otype=%lu,speed=%lu,pull=%lu,odr=%lu)",
            (unsigned long)cfg_g.mode, (unsigned long)cfg_g.otype, (unsigned long)cfg_g.speed, (unsigned long)cfg_g.pull, (unsigned long)odr_g,
            (unsigned long)cfg_y.mode, (unsigned long)cfg_y.otype, (unsigned long)cfg_y.speed, (unsigned long)cfg_y.pull, (unsigned long)odr_y,
            (unsigned long)cfg_r.mode, (unsigned long)cfg_r.otype, (unsigned long)cfg_r.speed, (unsigned long)cfg_r.pull, (unsigned long)odr_r);

    if ((mask_lt1 != 0x07U) || (s_lt1_lock_count < 3U) || (s_lt1_lock_count != s_lt1_unlock_count) ||
        (cfg_g.mode != GPIO_BM_MODE_OUTPUT) || (cfg_y.mode != GPIO_BM_MODE_OUTPUT) || (cfg_r.mode != GPIO_BM_MODE_OUTPUT) ||
        (cfg_g.otype != GPIO_BM_OTYPE_PP) || (cfg_y.otype != GPIO_BM_OTYPE_PP) || (cfg_r.otype != GPIO_BM_OTYPE_PP) ||
        (cfg_g.speed != GPIO_BM_SPEED_LOW) || (cfg_y.speed != GPIO_BM_SPEED_LOW) || (cfg_r.speed != GPIO_BM_SPEED_LOW) ||
        (cfg_g.pull != GPIO_BM_PULL_NONE) || (cfg_y.pull != GPIO_BM_PULL_NONE) || (cfg_r.pull != GPIO_BM_PULL_NONE) ||
        (odr_g != 0U) || (odr_y != 0U) || (odr_r != 0U))
    {
        fw_pass = false;
    }

    /* LT1 Control: lock() returns false */
    (void)GPIO_BM_DeinitPin(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT);
    (void)GPIO_BM_DeinitPin(BOARD_LED_YELLOW_PORT, BOARD_LED_YELLOW_BIT);
    (void)GPIO_BM_DeinitPin(BOARD_LED_RED_PORT, BOARD_LED_RED_BIT);

    uint32_t snap_lt1_ctrl_pre[15];
    take_gpio_snapshot_15(snap_lt1_ctrl_pre);

    const GpioBm_SharedLock_t hook_false = {
        .lock = lt1_lock_false,
        .unlock = lt1_unlock,
        .ctx = NULL
    };
    GPIO_BM_SetSharedLock(&hook_false);

    uint32_t mask_lt1_ctrl = LED_Init_ConfigEx(&board_cfg);

    LED_Set(LED_GREEN, 1U);
    LED_Set(LED_YELLOW, 1U);
    LED_Set(LED_RED, 1U);

    uint32_t snap_lt1_ctrl_post[15];
    take_gpio_snapshot_15(snap_lt1_ctrl_post);

    bool ctrl_lt1_regs_equal = (memcmp(snap_lt1_ctrl_pre, snap_lt1_ctrl_post, sizeof(snap_lt1_ctrl_pre)) == 0);
    uint32_t ctrl_odr_g = (BOARD_LED_GREEN_PORT->ODR >> BOARD_LED_GREEN_BIT) & 1U;
    uint32_t ctrl_odr_y = (BOARD_LED_YELLOW_PORT->ODR >> BOARD_LED_YELLOW_BIT) & 1U;
    uint32_t ctrl_odr_r = (BOARD_LED_RED_PORT->ODR >> BOARD_LED_RED_BIT) & 1U;

    APP_LOG("[led] LT1_CTRL: mask=0x%02lX regs_equal=%u odr_g=%lu odr_y=%lu odr_r=%lu",
            (unsigned long)mask_lt1_ctrl, ctrl_lt1_regs_equal ? 1U : 0U,
            (unsigned long)ctrl_odr_g, (unsigned long)ctrl_odr_y, (unsigned long)ctrl_odr_r);

    if ((mask_lt1_ctrl != 0U) || !ctrl_lt1_regs_equal || (ctrl_odr_g != 0U) || (ctrl_odr_y != 0U) || (ctrl_odr_r != 0U))
    {
        fw_pass = false;
    }

    /* Restore normal lock and init for next step */
    GPIO_BM_SetSharedLock(NULL);
    LED_Init();

    /* ---- LT2 (L5): Bad table rejection and snapshot verification ----------- */
    uint32_t snap_lt2_pre[15];
    take_gpio_snapshot_15(snap_lt2_pre);

    const led_driver_config_t bad_cfg = {
        .leds = {
            [LED_GREEN]  = { .port = BOARD_LED_GREEN_PORT,    .pin_num = 16U },                        /* pin_num > 15 */
            [LED_YELLOW] = { .port = (GPIO_TypeDef *)0x58024000UL, .pin_num = BOARD_LED_YELLOW_BIT }, /* invalid port address */
            [LED_RED]    = { .port = GPIOA,                   .pin_num = 13U }                         /* PA13 = SWD pin */
        }
    };

    uint32_t mask_lt2 = LED_Init_ConfigEx(&bad_cfg);

    uint32_t snap_lt2_post_init[15];
    take_gpio_snapshot_15(snap_lt2_post_init);

    LED_Set(LED_GREEN, 1U);
    LED_Toggle(LED_YELLOW);
    LED_BootStage(LED_BOOT_SCHEDULER);

    uint32_t snap_lt2_post_ops[15];
    take_gpio_snapshot_15(snap_lt2_post_ops);

    bool init_equal = (memcmp(snap_lt2_pre, snap_lt2_post_init, sizeof(snap_lt2_pre)) == 0);
    bool ops_equal  = (memcmp(snap_lt2_pre, snap_lt2_post_ops, sizeof(snap_lt2_pre)) == 0);

    APP_LOG("[led] LT2: mask=0x%02lX init_equal=%u ops_equal=%u",
            (unsigned long)mask_lt2, init_equal ? 1U : 0U, ops_equal ? 1U : 0U);

    if ((mask_lt2 != 0U) || !init_equal || !ops_equal)
    {
        fw_pass = false;
    }

    /* LT2 Control: table with only green valid, PB0 put in analog beforehand */
    (void)GPIO_BM_DeinitPin(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT);

    uint32_t snap_lt2_ctrl_pre[15];
    take_gpio_snapshot_15(snap_lt2_ctrl_pre);

    const led_driver_config_t semi_bad_cfg = {
        .leds = {
            [LED_GREEN]  = { .port = BOARD_LED_GREEN_PORT,    .pin_num = BOARD_LED_GREEN_BIT },
            [LED_YELLOW] = { .port = (GPIO_TypeDef *)0x58024000UL, .pin_num = BOARD_LED_YELLOW_BIT },
            [LED_RED]    = { .port = GPIOA,                   .pin_num = 13U }
        }
    };

    uint32_t mask_lt2_ctrl = LED_Init_ConfigEx(&semi_bad_cfg);

    uint32_t snap_lt2_ctrl_post[15];
    take_gpio_snapshot_15(snap_lt2_ctrl_post);

    uint32_t pb0_mode_before = (snap_lt2_ctrl_pre[5] >> (BOARD_LED_GREEN_BIT * 2U)) & 3U;
    uint32_t pb0_mode_after  = (snap_lt2_ctrl_post[5] >> (BOARD_LED_GREEN_BIT * 2U)) & 3U;
    bool gpiob_changed = (pb0_mode_before != pb0_mode_after) && (pb0_mode_after == GPIO_BM_MODE_OUTPUT);

    APP_LOG("[led] LT2_CTRL: mask=0x%02lX pb0_mode_before=%lu pb0_mode_after=%lu gpiob_changed=%u",
            (unsigned long)mask_lt2_ctrl, (unsigned long)pb0_mode_before, (unsigned long)pb0_mode_after,
            gpiob_changed ? 1U : 0U);

    if ((mask_lt2_ctrl != 0x01U) || !gpiob_changed)
    {
        fw_pass = false;
    }

    LED_Init();

    /* ---- LT3 (L4): Atomic LED_Toggle vs Periodic TIM17 ISR ----------------- */
    RCC->APB2ENR |= RCC_APB2ENR_TIM17EN;
    (void)RCC->APB2ENR;
    TIM17->CR1  = 0U;
    TIM17->PSC  = 0U;
    TIM17->ARR  = 999U; /* 200 kHz from 200 MHz APB2 timer clock */
    TIM17->EGR  = TIM_EGR_UG;
    TIM17->SR   = 0U;
    TIM17->DIER = TIM_DIER_UIE;
    /* Ensure BASEPRI is 0 so interrupts at priority 6 are not masked */
    __set_BASEPRI(0);
    NVIC_SetPriority(TIM17_IRQn, 6U);
    NVIC_ClearPendingIRQ(TIM17_IRQn);
    NVIC_EnableIRQ(TIM17_IRQn);

    /* 1. Real run: LED_Toggle with PRIMASK masking */
    s_use_old_toggle = false;
    s_isr_count = 0U;
    TIM17->CR1 = TIM_CR1_CEN;


    uint32_t real_lost = 0U;
    uint32_t real_window_isrs = 0U;

    for (uint32_t i = 0U; i < LT3_N; i++)
    {
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        uint32_t isr_before = s_isr_count;
        uint32_t odr_before = (BOARD_LED_GREEN_PORT->ODR >> BOARD_LED_GREEN_BIT) & 1U;
        __set_PRIMASK(pm);

        LED_Toggle(LED_GREEN);

        pm = __get_PRIMASK();
        __disable_irq();
        uint32_t isr_after = s_isr_count;
        uint32_t odr_after = (BOARD_LED_GREEN_PORT->ODR >> BOARD_LED_GREEN_BIT) & 1U;
        __set_PRIMASK(pm);

        uint32_t delta = isr_after - isr_before;
        if (delta > 0U)
        {
            real_window_isrs += delta;
        }
        uint32_t expected = odr_before ^ 1U ^ (delta & 1U);
        if (odr_after != expected)
        {
            real_lost++;
        }
    }

    TIM17->CR1 = 0U;

    /* 2. Control run: old_LED_Toggle without PRIMASK masking */
    s_use_old_toggle = true;
    s_isr_count = 0U;
    TIM17->CR1 = TIM_CR1_CEN;

    uint32_t ctrl_lost = 0U;
    uint32_t ctrl_window_isrs = 0U;

    for (uint32_t i = 0U; i < LT3_N; i++)
    {
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        uint32_t isr_before = s_isr_count;
        uint32_t odr_before = (BOARD_LED_GREEN_PORT->ODR >> BOARD_LED_GREEN_BIT) & 1U;
        __set_PRIMASK(pm);

        old_LED_Toggle(LED_GREEN);

        pm = __get_PRIMASK();
        __disable_irq();
        uint32_t isr_after = s_isr_count;
        uint32_t odr_after = (BOARD_LED_GREEN_PORT->ODR >> BOARD_LED_GREEN_BIT) & 1U;
        __set_PRIMASK(pm);

        uint32_t delta = isr_after - isr_before;
        if (delta > 0U)
        {
            ctrl_window_isrs += delta;
        }
        uint32_t expected = odr_before ^ 1U ^ (delta & 1U);
        if (odr_after != expected)
        {
            ctrl_lost++;
        }
    }

    /* Clean up TIM17: stopped, DIER=0, pending cleared, clock restored */
    TIM17->CR1  = 0U;
    TIM17->DIER = 0U;
    NVIC_DisableIRQ(TIM17_IRQn);
    TIM17->SR   = 0U;
    NVIC_ClearPendingIRQ(TIM17_IRQn);
    RCC->APB2ENR &= ~RCC_APB2ENR_TIM17EN;
    (void)RCC->APB2ENR;

    APP_LOG("[led] LT3: N=%lu freq_hz=%lu window_isrs=%lu lost=%lu",
            (unsigned long)LT3_N, (unsigned long)LT3_FREQ_HZ,
            (unsigned long)real_window_isrs, (unsigned long)real_lost);
    APP_LOG("[led] LT3_CTRL: N=%lu freq_hz=%lu window_isrs=%lu lost=%lu",
            (unsigned long)LT3_N, (unsigned long)LT3_FREQ_HZ,
            (unsigned long)ctrl_window_isrs, (unsigned long)ctrl_lost);

    if ((real_lost != 0U) || (ctrl_lost == 0U))
    {
        fw_pass = false;
    }

    LED_Init();

    /* ---- LT4 (G14): LED_PanicRed exception path ---------------------------- */
    (void)GPIO_BM_DeinitPin(BOARD_LED_RED_PORT, BOARD_LED_RED_BIT);

    GpioBm_PinConfig_t green_before;
    (void)GPIO_BM_ReadConfig(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT, &green_before);
    uint32_t green_odr_before = (BOARD_LED_GREEN_PORT->ODR >> BOARD_LED_GREEN_BIT) & 1U;

    LED_PanicRed();

    GpioBm_PinConfig_t red_after;
    (void)GPIO_BM_ReadConfig(BOARD_LED_RED_PORT, BOARD_LED_RED_BIT, &red_after);
    uint32_t red_odr_after = (BOARD_LED_RED_PORT->ODR >> BOARD_LED_RED_BIT) & 1U;

    GpioBm_PinConfig_t green_after;
    (void)GPIO_BM_ReadConfig(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT, &green_after);
    uint32_t green_odr_after = (BOARD_LED_GREEN_PORT->ODR >> BOARD_LED_GREEN_BIT) & 1U;

    bool green_unchanged = (green_before.mode == green_after.mode) &&
                           (green_before.pull == green_after.pull) &&
                           (green_odr_before == green_odr_after);

    APP_LOG("[led] LT4: red_mode=%lu red_odr=%lu",
            (unsigned long)red_after.mode, (unsigned long)red_odr_after);
    APP_LOG("[led] LT4_CTRL: green_unchanged=%u green_mode=%lu green_odr=%lu",
            green_unchanged ? 1U : 0U, (unsigned long)green_after.mode, (unsigned long)green_odr_after);

    if ((red_after.mode != GPIO_BM_MODE_OUTPUT) || (red_odr_after != 1U) || !green_unchanged)
    {
        fw_pass = false;
    }

    LED_Init();

    /* ---- LT5: Final restoration -------------------------------------------- */
    LED_Init();
    GPIO_BM_SetSharedLock(NULL);

    bool tim17_stopped = ((TIM17->CR1 & TIM_CR1_CEN) == 0U) &&
                         ((TIM17->DIER & TIM_DIER_UIE) == 0U) &&
                         (NVIC_GetPendingIRQ(TIM17_IRQn) == 0U) &&
                         ((RCC->APB2ENR & RCC_APB2ENR_TIM17EN) == 0U);

    GpioBm_PinConfig_t fin_g, fin_y, fin_r;
    (void)GPIO_BM_ReadConfig(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT, &fin_g);
    (void)GPIO_BM_ReadConfig(BOARD_LED_YELLOW_PORT, BOARD_LED_YELLOW_BIT, &fin_y);
    (void)GPIO_BM_ReadConfig(BOARD_LED_RED_PORT, BOARD_LED_RED_BIT, &fin_r);

    uint32_t fin_odr_g = (BOARD_LED_GREEN_PORT->ODR >> BOARD_LED_GREEN_BIT) & 1U;
    uint32_t fin_odr_y = (BOARD_LED_YELLOW_PORT->ODR >> BOARD_LED_YELLOW_BIT) & 1U;
    uint32_t fin_odr_r = (BOARD_LED_RED_PORT->ODR >> BOARD_LED_RED_BIT) & 1U;

    APP_LOG("[led] LT5: tim17_stopped=%u", tim17_stopped ? 1U : 0U);
    APP_LOG("[led] LT5_PINS: G(mode=%lu,otype=%lu,speed=%lu,pull=%lu,odr=%lu) "
            "Y(mode=%lu,otype=%lu,speed=%lu,pull=%lu,odr=%lu) "
            "R(mode=%lu,otype=%lu,speed=%lu,pull=%lu,odr=%lu)",
            (unsigned long)fin_g.mode, (unsigned long)fin_g.otype, (unsigned long)fin_g.speed, (unsigned long)fin_g.pull, (unsigned long)fin_odr_g,
            (unsigned long)fin_y.mode, (unsigned long)fin_y.otype, (unsigned long)fin_y.speed, (unsigned long)fin_y.pull, (unsigned long)fin_odr_y,
            (unsigned long)fin_r.mode, (unsigned long)fin_r.otype, (unsigned long)fin_r.speed, (unsigned long)fin_r.pull, (unsigned long)fin_odr_r);

    if (!tim17_stopped ||
        (fin_g.mode != GPIO_BM_MODE_OUTPUT) || (fin_y.mode != GPIO_BM_MODE_OUTPUT) || (fin_r.mode != GPIO_BM_MODE_OUTPUT) ||
        (fin_g.otype != GPIO_BM_OTYPE_PP) || (fin_y.otype != GPIO_BM_OTYPE_PP) || (fin_r.otype != GPIO_BM_OTYPE_PP) ||
        (fin_g.speed != GPIO_BM_SPEED_LOW) || (fin_y.speed != GPIO_BM_SPEED_LOW) || (fin_r.speed != GPIO_BM_SPEED_LOW) ||
        (fin_g.pull != GPIO_BM_PULL_NONE) || (fin_y.pull != GPIO_BM_PULL_NONE) || (fin_r.pull != GPIO_BM_PULL_NONE) ||
        (fin_odr_g != 0U) || (fin_odr_y != 0U) || (fin_odr_r != 0U))
    {
        fw_pass = false;
    }

    APP_LOG("[led] FW VERDICT=%s", fw_pass ? "PASS" : "FAIL");
}
