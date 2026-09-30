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

/* ==========================================================================
 * F1a additions. Everything above this line is the pre-F1a API and keeps its
 * signatures and constants (catalog criterion C2). Two notes above are
 * superseded by what follows:
 *   - GPIO_BM_ConfigAlternate() and GPIO_BM_ConfigAnalog() now also reject
 *     pull > 2 and speed > 3 and misaligned port pointers, run inside the
 *     section below, and return false when the pin is locked by LCKR or the
 *     read-back differs from what was written.
 *   - They no longer read-modify-write without a lock: every configuration
 *     write of this driver happens inside GPIO_BM_SectionEnter/Exit, which
 *     masks interrupts on the calling core and, when a project installed a
 *     cross-core hook with GPIO_BM_SetSharedLock(), also takes that lock.
 * ========================================================================== */

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

/**
  * @brief  Configures one pin. The only configuration path of the new API.
  *
  *         1. Validation before any peripheral access, in this order: NULL,
  *            port (GPIOA..GPIOK on the 0x400 stride), pin, fields, SWD pin.
  *            A refusal writes nothing, not even the port clock.
  *         2. Port clock on (GPIO_BM_EnablePortClock, banked per core).
  *         3. Inside the section: LCKR check, then BSRR (OUTPUT only), OTYPER
  *            and OSPEEDR (OUTPUT/AF only), PUPDR, AFR (AF only), MODER last,
  *            then a read-back of the five fields of the pin.
  *
  * @retval GPIO_BM_OK, GPIO_BM_ERR_PARAM, GPIO_BM_ERR_DEBUG_PIN,
  *         GPIO_BM_ERR_LOCKED (nothing written), GPIO_BM_ERR_BUSY (nothing
  *         written), GPIO_BM_ERR_VERIFY.
  */
GpioBm_Status_t GPIO_BM_ConfigPin(const GpioBm_PinConfig_t *cfg);
GpioBm_Status_t GPIO_BM_ConfigOutput(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t otype,
                                     uint32_t speed, uint32_t pull, uint32_t initial_level);
GpioBm_Status_t GPIO_BM_ConfigInput(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t pull);
GpioBm_Status_t GPIO_BM_ConfigAlternateEx(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t af,
                                          uint32_t otype, uint32_t pull, uint32_t speed);
GpioBm_Status_t GPIO_BM_DeinitPin(GPIO_TypeDef *port, uint32_t pin_idx);  /* analog, AF0, PP, LOW, no pull */

/**
  * @brief  Reads the five configuration fields and the ODR bit of one pin.
  *         No section: it only reads. The port clock must already be on.
  */
GpioBm_Status_t GPIO_BM_ReadConfig(const GPIO_TypeDef *port, uint32_t pin_idx, GpioBm_PinConfig_t *out);

/* ---- Pin operations: static inline, no validation --------------------------------
 * Set, Reset, Write and WritePort are one store to BSRR, which is atomic
 * (RM0399 12.3.5): no section, safe from both cores and from any ISR.
 * Toggle is one ODR read and one BSRR store: atomic per store, but two
 * concurrent togglers of the same pin can both read the same ODR.
 * Read and ReadPort are one IDR read. Masks above bit 15 are ignored.
 * ---------------------------------------------------------------------------------- */
static inline void GPIO_BM_Set(GPIO_TypeDef *p, uint32_t mask)
{
    p->BSRR = mask & 0xFFFFUL;
}

static inline void GPIO_BM_Reset(GPIO_TypeDef *p, uint32_t mask)
{
    p->BSRR = (mask & 0xFFFFUL) << 16U;
}

static inline void GPIO_BM_Toggle(GPIO_TypeDef *p, uint32_t mask)        /* ODR read + one BSRR write */
{
    uint32_t odr = p->ODR;
    p->BSRR = ((odr & mask & 0xFFFFUL) << 16U) | (~odr & mask & 0xFFFFUL);
}

static inline void GPIO_BM_Write(GPIO_TypeDef *p, uint32_t pin_idx, uint32_t level)
{
    p->BSRR = (1UL << (pin_idx & 0xFU)) << ((level != 0U) ? 0U : 16U);
}

static inline void GPIO_BM_WritePort(GPIO_TypeDef *p, uint32_t mask, uint32_t value)
{
    p->BSRR = ((mask & ~value & 0xFFFFUL) << 16U) | (mask & value & 0xFFFFUL);
}

static inline bool GPIO_BM_Read(const GPIO_TypeDef *p, uint32_t pin_idx)
{
    return ((p->IDR >> (pin_idx & 0xFU)) & 1UL) != 0UL;
}

static inline uint32_t GPIO_BM_ReadPort(const GPIO_TypeDef *p)
{
    return p->IDR & 0xFFFFUL;
}

/* ---- LCKR -------------------------------------------------------------------- */
/**
  * @brief  Locks the configuration of the pins in `mask` until the next reset
  *         (RM0399 12.3.6 and 12.4.8): LCKK=1|mask, mask, LCKK=1|mask, read,
  *         and a read of LCKK, all as 32-bit accesses inside the section.
  * @retval GPIO_BM_OK when LCKK reads 1 and every pin of `mask` reads locked;
  *         GPIO_BM_ERR_VERIFY when not; GPIO_BM_ERR_LOCKED when the port was
  *         already locked without every pin of `mask` (LCKR is frozen until
  *         reset); GPIO_BM_ERR_PARAM for a bad port or a mask of 0 or > 16 bits.
  */
GpioBm_Status_t GPIO_BM_LockPins(GPIO_TypeDef *port, uint32_t mask);  /* LCKK sequence, word accesses, LCKK read back */
bool            GPIO_BM_IsLocked(const GPIO_TypeDef *port, uint32_t pin_idx);

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
