/**
  ******************************************************************************
  * @file    gpio_bm.h
  * @brief   GPIO port helpers, bare-metal tier.
  *
  *          Driver tier: *_bm. Direct register access only, no ST HAL and no
  *          LL headers, so this file is usable from the earliest boot code -
  *          before the clock tree, before the MPU and before HAL_Init().
  *          01_led/led_bm.c depends on exactly that property.
  ******************************************************************************
  */

#ifndef GPIO_BM_H
#define GPIO_BM_H

#include <stdbool.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Enables the AHB4 clock of any GPIO port, GPIOA..GPIOK.
  *
  *         Derives the RCC_AHB4ENR bit from the port address rather than
  *         taking one __HAL_RCC_GPIOx_CLK_ENABLE() per port: the ports are
  *         0x400 apart and GPIOAEN..GPIOKEN are bits 0..10 in that order, so a
  *         driver handed a port at run time can bring it up without a switch
  *         over eleven macros it would have to keep in sync.
  *
  * @param  port  Port to clock. NULL and any address outside GPIOA..GPIOK are
  *               rejected rather than turned into an out-of-range bit.
  * @retval true  when the clock is on and readable.
  * @retval false when `port` is not a GPIO port; nothing was written.
  *
  * @note   Includes the read-back that RCC needs before the peripheral answers,
  *         the same one ST's __HAL_RCC_GPIOx_CLK_ENABLE() performs.
  * @note   Dual-core: safe from either CPU at any time. RCC_AHB4ENR (offset
  *         0xE0) is banked per core (RM0399 9.7.1), so each CPU writes its own
  *         copy and a port stays clocked while either core has its bit set.
  *         What the two cores DO share are the port's configuration registers
  *         (MODER, OTYPER, OSPEEDR, PUPDR, AFR): GPIO_BM_ConfigAlternate() and
  *         GPIO_BM_ConfigAnalog() read-modify-write them without a lock, so two
  *         cores must not configure pins of the same port at the same time.
  *         See 00_gpio/README.md section 4.
  */
bool GPIO_BM_EnablePortClock(GPIO_TypeDef *port);

/* ---- GPIO Pull & Speed Constants (Bare-Metal) --------------------------- */
#define GPIO_BM_PULL_NONE         0x00000000UL
#define GPIO_BM_PULL_UP           0x00000001UL
#define GPIO_BM_PULL_DOWN         0x00000002UL

#define GPIO_BM_SPEED_LOW         0x00000000UL
#define GPIO_BM_SPEED_MEDIUM      0x00000001UL
#define GPIO_BM_SPEED_HIGH        0x00000002UL
#define GPIO_BM_SPEED_VERY_HIGH   0x00000003UL

/**
  * @brief  Configures a single GPIO pin for alternate function (AF) mode in Push-Pull.
  *
  *         Enables the port clock automatically via GPIO_BM_EnablePortClock(port),
  *         programs AFR[0] or AFR[1], sets MODER to alternate function (0b10),
  *         OTYPER to push-pull (0), and applies the requested pull and speed.
  *
  * @param  port     GPIO port pointer (GPIOA..GPIOK).
  * @param  pin_idx  Pin index (0..15).
  * @param  af       Alternate function index (0..15, e.g. 7 for USART1..3).
  * @param  pull     Pull configuration (GPIO_BM_PULL_*).
  * @param  speed    Speed configuration (GPIO_BM_SPEED_*).
  * @retval true on success, false on invalid parameters (NULL port or pin/AF > 15).
  */
bool GPIO_BM_ConfigAlternate(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t af, uint32_t pull, uint32_t speed);

/**
  * @brief  Configures a single GPIO pin for analog mode (no pull-up/down).
  *
  *         Enables the port clock automatically via GPIO_BM_EnablePortClock(port),
  *         sets MODER to analog (0b11) and PUPDR to no-pull (0b00).
  *
  * @param  port     GPIO port pointer (GPIOA..GPIOK).
  * @param  pin_idx  Pin index (0..15).
  * @retval true on success, false on invalid parameters (NULL port or pin > 15).
  */
bool GPIO_BM_ConfigAnalog(GPIO_TypeDef *port, uint32_t pin_idx);

/* ---- Status ------------------------------------------------------------------ */
typedef enum
{
    GPIO_BM_OK = 0,
    GPIO_BM_ERR_PARAM,       /*!< refused before any peripheral access                 */
    GPIO_BM_ERR_DEBUG_PIN,   /*!< PA13/PA14 without allow_debug_pins                   */
    GPIO_BM_ERR_LOCKED,      /*!< LCKR freezes this pin: the write would be ignored    */
    GPIO_BM_ERR_BUSY,        /*!< the cross-core hook did not get the lock in its limit */
    GPIO_BM_ERR_VERIFY,      /*!< read-back differs from what was written              */
    GPIO_BM_ERR_CONFLICT,    /*!< EXTI line already routed to another port (F2)        */
    GPIO_BM_ERR_NOT_READY    /*!< compensation cell: CSI off or READY not set (F2)      */
} GpioBm_Status_t;

/* ---- Canonical pin -------------------------------------------------------------- */
typedef struct { GPIO_TypeDef *port; uint8_t pin_idx; } GpioBm_Pin_t;
#define GPIO_BM_PIN_MASK(idx)    (1UL << (idx))
#define GPIO_BM_PIN_INDEX(mask)  ((uint32_t)__builtin_ctz((uint32_t)(mask)))   /* single-bit masks only */

#define GPIO_BM_MODE_INPUT   0U   /* MODER field values, RM0399 12.4.1 */
#define GPIO_BM_MODE_OUTPUT  1U
#define GPIO_BM_MODE_AF      2U
#define GPIO_BM_MODE_ANALOG  3U
#define GPIO_BM_OTYPE_PP     0U
#define GPIO_BM_OTYPE_OD     1U

typedef struct
{
    GPIO_TypeDef *port;
    uint32_t pin_idx;          /*!< 0..15                                             */
    uint32_t mode;             /*!< GPIO_BM_MODE_*                                    */
    uint32_t otype;            /*!< GPIO_BM_OTYPE_*; ignored for INPUT/ANALOG          */
    uint32_t speed;            /*!< GPIO_BM_SPEED_*; ignored for INPUT/ANALOG          */
    uint32_t pull;             /*!< GPIO_BM_PULL_*; must be NONE for ANALOG            */
    uint32_t af;               /*!< 0..15, MODE_AF only                                */
    uint32_t initial_level;    /*!< 0/1, MODE_OUTPUT only: BSRR before MODER           */
    bool     allow_debug_pins; /*!< false: PA13/PA14 refused                          */
} GpioBm_PinConfig_t;

/* ---- Section and cross-core hook ------------------------------------------------ */
/* The section covers one pin. PRIMASK is saved and interrupts are masked on
 * the calling core first; only then, at nesting level 1 and when a hook is
 * installed, lock() is called. So no ISR of this core can run while the
 * cross-core lock is held. GPIO_BM_SectionExit() restores the saved PRIMASK
 * and never enables interrupts that were masked on entry, so the driver can
 * be used from a fault handler. The driver itself never waits: the hook is
 * bounded by the project. */
typedef struct
{
    bool (*lock)(void *ctx);    /*!< true = taken. Bounded by the project (TakeTimeout) */
    void (*unlock)(void *ctx);
    void  *ctx;
} GpioBm_SharedLock_t;

typedef struct { uint32_t primask; uint8_t took_shared; } GpioBm_Section_t;

void            GPIO_BM_SetSharedLock(const GpioBm_SharedLock_t *lock);   /* per core; NULL = PRIMASK only */
GpioBm_Status_t GPIO_BM_SectionEnter(GpioBm_Section_t *s);   /* for consumers wrapping HAL_GPIO_Init */
void            GPIO_BM_SectionExit(const GpioBm_Section_t *s);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_BM_H */
