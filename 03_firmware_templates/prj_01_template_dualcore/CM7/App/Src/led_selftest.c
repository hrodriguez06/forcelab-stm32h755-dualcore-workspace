/**
  ******************************************************************************
  * @file    led_selftest.c
  * @brief   01_led driver selftest implementation (preset selftest-led, CM7).
  *
  *          Tests LT1-LT5 according to 01_shared_drivers/01_led/IMPLEMENTATION_PLAN.md
  *          and AUDIT.md requirements:
  *            LT1: L3 - validation, section, cross-core hook, read-back
  *            LT2: L5 - invalid port/pin rejection and 15-word register snapshots
  *            LT3: L4 - atomic LED_Toggle vs periodic TIM17 ISR race condition (DWT-measured)
  *            LT4: G14 - LED_PanicRed exception direct RMW, green pin unchanged
  *            LT5: Final state restoration, TIM17 stopped, pins restored
  ******************************************************************************
  */

#include "led_selftest.h"
#include "led_bm.h"
#include "gpio_bm.h"
#include "board_def.h"
#include "shared_memory.h"
#include "app_log.h"
#include "fw_build_id.h"
#include "stm32h7xx.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LT3_N               200000UL

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

static void print_snapshot_15(const char *label, const uint32_t s[15])
{
    APP_LOG("[led] %s_A: %08lX %08lX %08lX %08lX %08lX",
            label,
            (unsigned long)s[0], (unsigned long)s[1], (unsigned long)s[2], (unsigned long)s[3], (unsigned long)s[4]);
    APP_LOG("[led] %s_B: %08lX %08lX %08lX %08lX %08lX",
            label,
            (unsigned long)s[5], (unsigned long)s[6], (unsigned long)s[7], (unsigned long)s[8], (unsigned long)s[9]);
    APP_LOG("[led] %s_E: %08lX %08lX %08lX %08lX %08lX",
            label,
            (unsigned long)s[10], (unsigned long)s[11], (unsigned long)s[12], (unsigned long)s[13], (unsigned long)s[14]);
}

void LED_Selftest_Run(void)
{
    App_Log_Init();

    APP_LOG(" ");
    APP_LOG("=== STM32H755ZI 01_led driver selftest - CPU1 (Cortex-M7) ===");
    APP_LOG("[sys] build rev=%s id=%s", FW_GIT_REV, FW_BUILD_ID);

    bool fw_pass = true;

    /* ---- LT1 (L3): Section, lock hook and read-back ------------------------ */
    GpioBm_Status_t st_deinit_g = GPIO_BM_DeinitPin(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT);
    GpioBm_Status_t st_deinit_y = GPIO_BM_DeinitPin(BOARD_LED_YELLOW_PORT, BOARD_LED_YELLOW_BIT);
    GpioBm_Status_t st_deinit_r = GPIO_BM_DeinitPin(BOARD_LED_RED_PORT, BOARD_LED_RED_BIT);

    GpioBm_PinConfig_t pre_g, pre_y, pre_r;
    GpioBm_Status_t st_read_pre_g = GPIO_BM_ReadConfig(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT, &pre_g);
    GpioBm_Status_t st_read_pre_y = GPIO_BM_ReadConfig(BOARD_LED_YELLOW_PORT, BOARD_LED_YELLOW_BIT, &pre_y);
    GpioBm_Status_t st_read_pre_r = GPIO_BM_ReadConfig(BOARD_LED_RED_PORT, BOARD_LED_RED_BIT, &pre_r);

    APP_LOG("[led] LT1_PRE: deinit_rc=%d,%d,%d read_rc=%d,%d,%d modes=%lu,%lu,%lu",
            (int)st_deinit_g, (int)st_deinit_y, (int)st_deinit_r,
            (int)st_read_pre_g, (int)st_read_pre_y, (int)st_read_pre_r,
            (unsigned long)pre_g.mode, (unsigned long)pre_y.mode, (unsigned long)pre_r.mode);

    if ((st_deinit_g != GPIO_BM_OK) || (st_deinit_y != GPIO_BM_OK) || (st_deinit_r != GPIO_BM_OK) ||
        (st_read_pre_g != GPIO_BM_OK) || (st_read_pre_y != GPIO_BM_OK) || (st_read_pre_r != GPIO_BM_OK) ||
        (pre_g.mode != GPIO_BM_MODE_ANALOG) || (pre_y.mode != GPIO_BM_MODE_ANALOG) || (pre_r.mode != GPIO_BM_MODE_ANALOG))
    {
        fw_pass = false;
    }

    const led_driver_config_t board_cfg = {
        .leds = {
            [LED_GREEN]  = { .port = BOARD_LED_GREEN_PORT,  .pin_num = BOARD_LED_GREEN_BIT },
            [LED_YELLOW] = { .port = BOARD_LED_YELLOW_PORT, .pin_num = BOARD_LED_YELLOW_BIT },
            [LED_RED]    = { .port = BOARD_LED_RED_PORT,    .pin_num = BOARD_LED_RED_BIT }
        }
    };

    const GpioBm_SharedLock_t hook_true = {
        .lock = lt1_lock_true,
        .unlock = lt1_unlock,
        .ctx = NULL
    };

    s_lt1_lock_count = 0U;
    s_lt1_unlock_count = 0U;
    GPIO_BM_SetSharedLock(&hook_true);
    uint32_t mask_lt1 = LED_Init_ConfigEx(&board_cfg);
    Shared_GpioLockInstall(); /* Restore production cross-core hook immediately (A3) */

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

    /* LT1 Control: lock() returns false, registers must remain untouched */
    GpioBm_Status_t st_ctrl_deinit_g = GPIO_BM_DeinitPin(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT);
    GpioBm_Status_t st_ctrl_deinit_y = GPIO_BM_DeinitPin(BOARD_LED_YELLOW_PORT, BOARD_LED_YELLOW_BIT);
    GpioBm_Status_t st_ctrl_deinit_r = GPIO_BM_DeinitPin(BOARD_LED_RED_PORT, BOARD_LED_RED_BIT);

    GpioBm_PinConfig_t ctrl_pre_g, ctrl_pre_y, ctrl_pre_r;
    (void)GPIO_BM_ReadConfig(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT, &ctrl_pre_g);
    (void)GPIO_BM_ReadConfig(BOARD_LED_YELLOW_PORT, BOARD_LED_YELLOW_BIT, &ctrl_pre_y);
    (void)GPIO_BM_ReadConfig(BOARD_LED_RED_PORT, BOARD_LED_RED_BIT, &ctrl_pre_r);

    uint32_t snap_lt1_ctrl_pre[15];
    take_gpio_snapshot_15(snap_lt1_ctrl_pre);

    const GpioBm_SharedLock_t hook_false = {
        .lock = lt1_lock_false,
        .unlock = lt1_unlock,
        .ctx = NULL
    };

    s_lt1_lock_count = 0U;
    s_lt1_unlock_count = 0U;
    GPIO_BM_SetSharedLock(&hook_false);
    uint32_t mask_lt1_ctrl = LED_Init_ConfigEx(&board_cfg);
    Shared_GpioLockInstall(); /* Restore production cross-core hook immediately (A3) */

    LED_Set(LED_GREEN, 1U);
    LED_Set(LED_YELLOW, 1U);
    LED_Set(LED_RED, 1U);

    uint32_t snap_lt1_ctrl_post[15];
    take_gpio_snapshot_15(snap_lt1_ctrl_post);

    uint32_t ctrl_odr_g = (BOARD_LED_GREEN_PORT->ODR >> BOARD_LED_GREEN_BIT) & 1U;
    uint32_t ctrl_odr_y = (BOARD_LED_YELLOW_PORT->ODR >> BOARD_LED_YELLOW_BIT) & 1U;
    uint32_t ctrl_odr_r = (BOARD_LED_RED_PORT->ODR >> BOARD_LED_RED_BIT) & 1U;

    APP_LOG("[led] LT1_CTRL: mask=0x%02lX lock=%lu unlock=%lu deinit_rc=%d,%d,%d modes=%lu,%lu,%lu odr_g=%lu odr_y=%lu odr_r=%lu",
            (unsigned long)mask_lt1_ctrl, (unsigned long)s_lt1_lock_count, (unsigned long)s_lt1_unlock_count,
            (int)st_ctrl_deinit_g, (int)st_ctrl_deinit_y, (int)st_ctrl_deinit_r,
            (unsigned long)ctrl_pre_g.mode, (unsigned long)ctrl_pre_y.mode, (unsigned long)ctrl_pre_r.mode,
            (unsigned long)ctrl_odr_g, (unsigned long)ctrl_odr_y, (unsigned long)ctrl_odr_r);

    print_snapshot_15("LT1_CTRL_PRE", snap_lt1_ctrl_pre);
    print_snapshot_15("LT1_CTRL_POST", snap_lt1_ctrl_post);

    if ((mask_lt1_ctrl != 0U) || (memcmp(snap_lt1_ctrl_pre, snap_lt1_ctrl_post, sizeof(snap_lt1_ctrl_pre)) != 0) ||
        (ctrl_odr_g != 0U) || (ctrl_odr_y != 0U) || (ctrl_odr_r != 0U))
    {
        fw_pass = false;
    }

    LED_Init();

    /* ---- LT2 (L5): Bad table rejection and snapshot verification ----------- */
    uint32_t snap_lt2_pre[15];
    take_gpio_snapshot_15(snap_lt2_pre);

    /* Invalid table:
     * - green: pin_num 16 (> 15)
     * - yellow: port address 0x58024000 (beyond GPIOK 0x58022800, reserved boundary RM0399 §2.2 / Tab 8)
     * - red: PA13 (SWD TMS/SWDIO pin) */
    const led_driver_config_t bad_cfg = {
        .leds = {
            [LED_GREEN]  = { .port = BOARD_LED_GREEN_PORT,         .pin_num = 16U },
            [LED_YELLOW] = { .port = (GPIO_TypeDef *)0x58024000UL, .pin_num = BOARD_LED_YELLOW_BIT },
            [LED_RED]    = { .port = GPIOA,                        .pin_num = 13U }
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

    APP_LOG("[led] LT2: mask=0x%02lX", (unsigned long)mask_lt2);
    print_snapshot_15("LT2_PRE", snap_lt2_pre);
    print_snapshot_15("LT2_POST_INIT", snap_lt2_post_init);
    print_snapshot_15("LT2_POST_OPS", snap_lt2_post_ops);

    if ((mask_lt2 != 0U) ||
        (memcmp(snap_lt2_pre, snap_lt2_post_init, sizeof(snap_lt2_pre)) != 0) ||
        (memcmp(snap_lt2_pre, snap_lt2_post_ops, sizeof(snap_lt2_pre)) != 0))
    {
        fw_pass = false;
    }

    /* LT2 Control: table with only green valid, PB0 put in analog beforehand */
    (void)GPIO_BM_DeinitPin(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT);

    uint32_t snap_lt2_ctrl_pre[15];
    take_gpio_snapshot_15(snap_lt2_ctrl_pre);

    const led_driver_config_t semi_bad_cfg = {
        .leds = {
            [LED_GREEN]  = { .port = BOARD_LED_GREEN_PORT,         .pin_num = BOARD_LED_GREEN_BIT },
            [LED_YELLOW] = { .port = (GPIO_TypeDef *)0x58024000UL, .pin_num = BOARD_LED_YELLOW_BIT },
            [LED_RED]    = { .port = GPIOA,                        .pin_num = 13U }
        }
    };

    uint32_t mask_lt2_ctrl = LED_Init_ConfigEx(&semi_bad_cfg);

    uint32_t snap_lt2_ctrl_post[15];
    take_gpio_snapshot_15(snap_lt2_ctrl_post);

    APP_LOG("[led] LT2_CTRL: mask=0x%02lX", (unsigned long)mask_lt2_ctrl);
    print_snapshot_15("LT2_CTRL_PRE", snap_lt2_ctrl_pre);
    print_snapshot_15("LT2_CTRL_POST", snap_lt2_ctrl_post);

    if ((mask_lt2_ctrl != 0x01U) ||
        (memcmp(snap_lt2_ctrl_pre, snap_lt2_ctrl_post, sizeof(snap_lt2_ctrl_pre)) == 0))
    {
        fw_pass = false;
    }

    LED_Init();

    /* ---- LT3 (L4): Atomic LED_Toggle vs Periodic TIM17 ISR ----------------- */
    /* Enable DWT cycle counter for raw frequency measurement (A4) */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* Save state to restore upon LT3 completion (A11) */
    uint32_t prev_basepri = __get_BASEPRI();
    uint32_t prev_apb2enr = RCC->APB2ENR;
    uint32_t prev_tim17_prio = NVIC_GetPriority(TIM17_IRQn);

    RCC->APB2ENR |= RCC_APB2ENR_TIM17EN;
    (void)RCC->APB2ENR;
    TIM17->CR1  = 0U;
    TIM17->PSC  = 0U;
    TIM17->ARR  = 999U; /* 200 kHz from 200 MHz APB2 timer clock */
    TIM17->EGR  = TIM_EGR_UG;
    TIM17->SR   = 0U;
    TIM17->DIER = TIM_DIER_UIE;

    __set_BASEPRI(0);
    NVIC_SetPriority(TIM17_IRQn, 6U);
    NVIC_ClearPendingIRQ(TIM17_IRQn);
    NVIC_EnableIRQ(TIM17_IRQn);

    /* 1. Real run: LED_Toggle with PRIMASK masking */
    s_use_old_toggle = false;
    s_isr_count = 0U;
    TIM17->CR1 = TIM_CR1_CEN;

    /* Measure ISR frequency against raw DWT cycles (A4) */
    uint32_t dwt_start = DWT->CYCCNT;
    uint32_t isr_start = s_isr_count;
    while ((s_isr_count - isr_start) < 1000U)
    {
        /* Sample 1000 ISRs (~5 ms at 200 kHz) */
    }
    uint32_t dwt_end = DWT->CYCCNT;
    uint32_t isr_end = s_isr_count;
    uint32_t dwt_cycles = dwt_end - dwt_start;
    uint32_t dwt_isrs = isr_end - isr_start;

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

    /* Clean up TIM17: stopped, DIER=0, pending cleared, priority and clock restored (A11, A12) */
    TIM17->CR1  = 0U;
    TIM17->DIER = 0U;
    NVIC_DisableIRQ(TIM17_IRQn);
    TIM17->SR   = 0U;
    NVIC_ClearPendingIRQ(TIM17_IRQn);
    NVIC_SetPriority(TIM17_IRQn, prev_tim17_prio);

    if ((prev_apb2enr & RCC_APB2ENR_TIM17EN) == 0U)
    {
        RCC->APB2ENR &= ~RCC_APB2ENR_TIM17EN;
        (void)RCC->APB2ENR;
    }
    __set_BASEPRI(prev_basepri);

    APP_LOG("[led] LT3: N=%lu dwt_cycles=%lu dwt_isrs=%lu cpu_hz=%lu window_isrs=%lu lost=%lu",
            (unsigned long)LT3_N, (unsigned long)dwt_cycles, (unsigned long)dwt_isrs,
            (unsigned long)SystemCoreClock, (unsigned long)real_window_isrs, (unsigned long)real_lost);
    APP_LOG("[led] LT3_CTRL: N=%lu window_isrs=%lu lost=%lu",
            (unsigned long)LT3_N, (unsigned long)ctrl_window_isrs, (unsigned long)ctrl_lost);

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

    APP_LOG("[led] LT4: red_mode=%lu red_odr=%lu",
            (unsigned long)red_after.mode, (unsigned long)red_odr_after);
    APP_LOG("[led] LT4_GREEN_PRE: mode=%lu otype=%lu speed=%lu pull=%lu odr=%lu",
            (unsigned long)green_before.mode, (unsigned long)green_before.otype,
            (unsigned long)green_before.speed, (unsigned long)green_before.pull,
            (unsigned long)green_odr_before);
    APP_LOG("[led] LT4_GREEN_POST: mode=%lu otype=%lu speed=%lu pull=%lu odr=%lu",
            (unsigned long)green_after.mode, (unsigned long)green_after.otype,
            (unsigned long)green_after.speed, (unsigned long)green_after.pull,
            (unsigned long)green_odr_after);

    bool green_unchanged = (green_before.mode == green_after.mode) &&
                           (green_before.otype == green_after.otype) &&
                           (green_before.speed == green_after.speed) &&
                           (green_before.pull == green_after.pull) &&
                           (green_odr_before == green_odr_after);

    if ((red_after.mode != GPIO_BM_MODE_OUTPUT) || (red_odr_after != 1U) || !green_unchanged)
    {
        fw_pass = false;
    }

    LED_Init();

    /* ---- LT5: Final restoration -------------------------------------------- */
    /* Single LED_Init call (A12) and restore production cross-core hook (A3) */
    Shared_GpioLockInstall();

    uint32_t tim17_cr1 = TIM17->CR1;
    uint32_t tim17_dier = TIM17->DIER;
    uint32_t tim17_nvic = NVIC_GetPendingIRQ(TIM17_IRQn);
    uint32_t tim17_apb2 = RCC->APB2ENR & RCC_APB2ENR_TIM17EN;

    APP_LOG("[led] LT5_TIM17: cr1=0x%04lX dier=0x%04lX nvic_pending=%lu apb2enr=0x%08lX",
            (unsigned long)tim17_cr1, (unsigned long)tim17_dier, (unsigned long)tim17_nvic, (unsigned long)tim17_apb2);

    bool tim17_stopped = ((tim17_cr1 & TIM_CR1_CEN) == 0U) &&
                         ((tim17_dier & TIM_DIER_UIE) == 0U) &&
                         (tim17_nvic == 0U) &&
                         (tim17_apb2 == 0U);

    GpioBm_PinConfig_t fin_g, fin_y, fin_r;
    (void)GPIO_BM_ReadConfig(BOARD_LED_GREEN_PORT, BOARD_LED_GREEN_BIT, &fin_g);
    (void)GPIO_BM_ReadConfig(BOARD_LED_YELLOW_PORT, BOARD_LED_YELLOW_BIT, &fin_y);
    (void)GPIO_BM_ReadConfig(BOARD_LED_RED_PORT, BOARD_LED_RED_BIT, &fin_r);

    uint32_t fin_odr_g = (BOARD_LED_GREEN_PORT->ODR >> BOARD_LED_GREEN_BIT) & 1U;
    uint32_t fin_odr_y = (BOARD_LED_YELLOW_PORT->ODR >> BOARD_LED_YELLOW_BIT) & 1U;
    uint32_t fin_odr_r = (BOARD_LED_RED_PORT->ODR >> BOARD_LED_RED_BIT) & 1U;

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

    /* Final guarantee: production cross-core hook installed */
    Shared_GpioLockInstall();

    APP_LOG("[led] FW VERDICT=%s", fw_pass ? "PASS" : "FAIL");
}
