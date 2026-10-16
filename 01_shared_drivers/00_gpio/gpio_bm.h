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

#if defined(DUAL_CORE) && !defined(CORE_CM4) && !defined(CORE_CM7)
#error "DUAL_CORE requires either CORE_CM4 or CORE_CM7 to be defined"
#endif

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
  *         (MODER, OTYPER, OSPEEDR, PUPDR, AFR): every configuration call of
  *         this driver writes them inside GPIO_BM_SectionEnter/Exit, which
  *         takes the cross-core lock when a project installed one with
  *         GPIO_BM_SetSharedLock(). See 00_gpio/README.md sections 3 and 4.
  */
bool GPIO_BM_EnablePortClock(GPIO_TypeDef *port);

/**
  * @brief  true when the port's GPIOxEN bit is set in either core's bank
  *         (RCC_C1->AHB4ENR or RCC_C2->AHB4ENR), i.e. its registers can be
  *         read; false when both are clear, and for an invalid port. Only
  *         reads RCC; either core can read both banks. Unclocked, the port
  *         reads all ones (LCKR = 0xFFFFFFFF, measured on Rev V).
  */
bool GPIO_BM_IsPortClocked(const GPIO_TypeDef *port);

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
  * @retval true when written and read back (GPIO_BM_ConfigPin() == GPIO_BM_OK).
  * @retval false on an invalid parameter (NULL or misaligned port, pin or
  *         AF > 15, pull > 2, speed > 3; nothing written), a pin locked by
  *         LCKR, a cross-core lock that timed out, or a read-back mismatch.
  *         PA13/PA14 are accepted, as before F1a.
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
  * @retval true when written and read back (GPIO_BM_ConfigPin() == GPIO_BM_OK).
  * @retval false on an invalid parameter (NULL or misaligned port, pin > 15;
  *         nothing written), a pin locked by LCKR, a cross-core lock that
  *         timed out, or a read-back mismatch. PA13/PA14 are accepted.
  */
bool GPIO_BM_ConfigAnalog(GPIO_TypeDef *port, uint32_t pin_idx);

/* ==========================================================================
 * F1a additions. Everything above this line is the pre-F1a API and keeps its
 * signatures and constants (catalog criterion C2); since F1a the two legacy
 * configuration calls are wrappers of GPIO_BM_ConfigPin(), so they share its
 * validation, its section and its read-back.
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
    , GPIO_BM_ERR_NO_CLOCK   /*!< port unclocked in both RCC banks: its registers cannot be read */
} GpioBm_Status_t;

/* ---- Canonical pin -------------------------------------------------------------- */
typedef struct { GPIO_TypeDef *port; uint8_t pin_idx; } GpioBm_Pin_t;
#define GPIO_BM_PIN_MASK(idx)    (1UL << (idx))
/* Single-bit masks of bits 0..15. A mask of 0 (or with no bit below 16) gives
   16, an index every function of the driver refuses with GPIO_BM_ERR_PARAM,
   instead of the undefined __builtin_ctz(0). */
#define GPIO_BM_PIN_INDEX(mask)  ((uint32_t)__builtin_ctz(((uint32_t)(mask) & 0xFFFFUL) | 0x10000UL))

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
  *         No section: it only reads. The port clock must already be on:
  *         GPIO_BM_ERR_NO_CLOCK, with *out untouched, when it is off in both
  *         banks (an unclocked port reads all ones, not its configuration).
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
  *         GPIO_BM_ERR_VERIFY when not; GPIO_BM_ERR_PARAM for a bad port or a
  *         mask of 0 or > 16 bits; GPIO_BM_ERR_BUSY from the cross-core hook.
  * @note   On a port that is already locked, the pins already locked are kept
  *         in the sequence and the result is judged by the read-back only.
  *         RM0399 12.4.8 says LCKy can only be written while LCKK = 0, but on
  *         Rev V silicon a second sequence adds pins (00_gpio/README.md, T16).
  *         When every pin of `mask` is already locked, nothing is written.
  */
GpioBm_Status_t GPIO_BM_LockPins(GPIO_TypeDef *port, uint32_t mask);  /* LCKK sequence, word accesses, LCKK read back */

/**
  * @brief  true when LCKK is set and the pin's LCKy bit is set; false for a bad
  *         port or pin. Only reads, and only meaningful while
  *         GPIO_BM_IsPortClocked(port) is true: an unclocked port reads all
  *         ones (LCKR = 0xFFFFFFFF, measured on Rev V), so every pin reads as
  *         locked. The configuration calls enable the clock before they look
  *         at LCKR.
  */
bool            GPIO_BM_IsLocked(const GPIO_TypeDef *port, uint32_t pin_idx);

/* ---- Section and cross-core hook ------------------------------------------------ */
/* The section covers one pin. PRIMASK is saved and interrupts are masked on
 * the calling core first; only then, at nesting level 1 and when a hook is
 * installed, lock() is called. So no ISR of this core can run while the
 * cross-core lock is held. GPIO_BM_SectionExit() restores the saved PRIMASK
 * and never enables interrupts that were masked on entry, so the driver can
 * be used from a fault handler. The driver itself never waits: the hook is
 * bounded by the project, and that bound is also the longest time this core
 * can spend with interrupts masked inside lock() (00_gpio/README.md, 10).
 *
 * Fault handlers: a section opened by a handler that preempted this core while
 * it was inside lock() is nested, so it does not take the cross-core lock and
 * writes unprotected (00_gpio/README.md, 10). Once lock() has returned, the
 * lock belongs to this core and a nested section is protected by it. */
typedef struct
{
    bool (*lock)(void *ctx);    /*!< true = taken. Bounded by the project (TakeTimeout) */
    void (*unlock)(void *ctx);
    void  *ctx;
} GpioBm_SharedLock_t;

typedef struct
{
    uint32_t primask;                  /*!< PRIMASK on entry, restored by Exit           */
    uint8_t  took_shared;              /*!< 1 when this section called lock()            */
    const GpioBm_SharedLock_t *taken;  /*!< the hook lock() was called on; Exit unlocks
                                            that one, even if the hook changed meanwhile */
} GpioBm_Section_t;

/* Per core; NULL = PRIMASK only. Safe at any time: a section already open
   releases the hook it took, and the new hook is used from the next section. */
void            GPIO_BM_SetSharedLock(const GpioBm_SharedLock_t *lock);
GpioBm_Status_t GPIO_BM_SectionEnter(GpioBm_Section_t *s);   /* for consumers wrapping HAL_GPIO_Init */
void            GPIO_BM_SectionExit(const GpioBm_Section_t *s);

/* ---- Validation without peripheral access (used by gpio_ll and gpio_hal) --------
 * Pure functions: no GPIO, RCC or LCKR access, so a tier built on top can refuse
 * a request with exactly the status gpio_bm would return, before it touches
 * anything. They are the single copy of the rules of GPIO_BM_ConfigPin step 1. */

/** @retval true when `port` is exactly one of GPIOA..GPIOK (base address on the 0x400 stride). */
bool            GPIO_BM_IsValidPort(const GPIO_TypeDef *port);

/** @retval The status GPIO_BM_ConfigPin() would return from its validation step:
  *         GPIO_BM_OK, GPIO_BM_ERR_PARAM or GPIO_BM_ERR_DEBUG_PIN. */
GpioBm_Status_t GPIO_BM_CheckConfig(const GpioBm_PinConfig_t *cfg);

/* ==========================================================================
 * F2 additions: EXTI 0-15 and I/O compensation cell.
 * ========================================================================== */

#define GPIO_BM_EXTI_RISING       1U
#define GPIO_BM_EXTI_FALLING      2U
#define GPIO_BM_EXTI_BOTH         3U
#define GPIO_BM_EXTI_IRQN_INVALID ((IRQn_Type)(-128))

/**
 * @brief  Configures an EXTI line (0..15) mapped to a GPIO port.
 *
 *         Configures only the calling core's interrupt and event masks (IMR1/EMR1
 *         for CPU1, C2IMR1/C2EMR1 for CPU2).
 *         Enables the calling core's RCC APB4ENR.SYSCFGEN bit before taking the lock
 *         (banked per core), so SYSCFGEN remains enabled even if an error is returned.
 *         Note: RTSR1 and FTSR1 are shared between cores. A core configuring an
 *         already-routed shared line may modify the edge trigger (RTSR1/FTSR1) even
 *         if the other core currently has the line unmasked.
 *
 *         Order of operations inside section:
 *           1. SYSCFG_EXTICR routing (RM0399 §13.3.2, §13.6.2)
 *           2. EXTI_RTSR1 / EXTI_FTSR1 triggers (RM0399 §21.6.1, §21.6.2)
 *           3. Clears this core's pending bit in PR1 (C2PR1 for CM4) (RM0399 §21.6.21)
 *           4. Sets requested interrupt/event mask bit in IMR1/EMR1 (C2IMR1/C2EMR1)
 *              and clears unrequested bits (RM0399 §21.6.19, §21.6.20).
 *           5. Read-back verification of written registers inside the section (C12).
 *
 * @param  port       GPIO port base address (GPIOA..GPIOK).
 * @param  pin_idx    Pin/line index (0..15).
 * @param  trigger    Trigger selection: GPIO_BM_EXTI_RISING (1),
 *                    GPIO_BM_EXTI_FALLING (2), or GPIO_BM_EXTI_BOTH (3).
 * @param  interrupt  Enable interrupt generation for the calling core.
 * @param  event      Enable event generation for the calling core.
 * @retval GPIO_BM_OK           Configuration applied and verified successfully.
 * @retval GPIO_BM_ERR_PARAM    Invalid port, pin_idx > 15, trigger not 1..3,
 *                              or both interrupt and event are false.
 * @retval GPIO_BM_ERR_CONFLICT Line is already routed to another port and has
 *                              any mask bit active in either core.
 * @retval GPIO_BM_ERR_BUSY     Cross-core lock not acquired within timeout.
 * @retval GPIO_BM_ERR_VERIFY   Register write failed read-back verification.
 */
GpioBm_Status_t GPIO_BM_ExtiConfig(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t trigger,
                                   bool interrupt, bool event);

/**
 * @brief  Disables an EXTI line (0..15) for the calling core.
 *
 *         Clears this core's interrupt/event masks and pending bit. If the
 *         other core's masks for this line are also zero, clears the triggers
 *         (RTSR1/FTSR1) and resets the SYSCFG_EXTICR routing for this line.
 *         Enables the calling core's RCC APB4ENR.SYSCFGEN bit before taking the lock.
 *         Note: Once a line is completely disabled (both cores' masks cleared),
 *         its EXTICR routing is reset to 0 (GPIOA). A subsequent call to
 *         GPIO_BM_ExtiDisable with the original non-GPIOA port will return
 *         GPIO_BM_ERR_CONFLICT because the line is no longer routed to that port.
 *
 * @param  port     GPIO port base address (GPIOA..GPIOK).
 * @param  pin_idx  Pin/line index (0..15).
 * @retval GPIO_BM_OK           Line disabled for this core and verified.
 * @retval GPIO_BM_ERR_PARAM    Invalid port or pin_idx > 15.
 * @retval GPIO_BM_ERR_CONFLICT Line is currently routed to another port in EXTICR.
 * @retval GPIO_BM_ERR_BUSY     Cross-core lock not acquired within timeout.
 * @retval GPIO_BM_ERR_VERIFY   Register write failed read-back verification.
 */
GpioBm_Status_t GPIO_BM_ExtiDisable(GPIO_TypeDef *port, uint32_t pin_idx);

/**
 * @brief  Reads and clears the pending status of specified EXTI lines (0..15).
 *
 *         Reads the calling core's PR1 (or C2PR1), masks with (line_mask & 0xFFFF),
 *         clears the pending bits via write-1-to-clear without RMW (RM0399 §21.6.21),
 *         executes __DSB(), and returns the captured pending bits.
 *         Can be called from ISR context (does NOT enter a critical section or take locks).
 *
 * @param  line_mask  Bitmask of lines to check and acknowledge (bits 0..15).
 * @retval Bitmask of lines that were pending.
 */
uint32_t        GPIO_BM_ExtiTakePending(uint32_t line_mask);

/**
 * @brief  Triggers a software interrupt/event on an EXTI line (0..15).
 *
 *         Writes (1 << pin_idx) to EXTI_SWIER1 inside a section (RM0399 §21.6.3).
 *         RM0399 §21.5.5: Hardware only asserts the interrupt if the line's
 *         trigger (RTSR1 or FTSR1) and calling core mask (IMR1/C2IMR1) allow it;
 *         this function does not verify those prerequisites.
 *
 * @param  pin_idx  Line index (0..15).
 * @retval GPIO_BM_OK           Software trigger request written to SWIER1.
 * @retval GPIO_BM_ERR_PARAM    pin_idx > 15.
 * @retval GPIO_BM_ERR_BUSY     Cross-core lock not acquired within timeout.
 */
GpioBm_Status_t GPIO_BM_ExtiSoftwareTrigger(uint32_t pin_idx);

/**
 * @brief  Maps a GPIO pin/line index (0..15) to its CMSIS IRQn_Type vector.
 *
 * @param  pin_idx  Line index (0..15).
 * @retval CMSIS IRQn_Type (EXTI0_IRQn..EXTI4_IRQn, EXTI9_5_IRQn, EXTI15_10_IRQn),
 *         or GPIO_BM_EXTI_IRQN_INVALID if pin_idx > 15.
 */
IRQn_Type       GPIO_BM_ExtiGetIRQn(uint32_t pin_idx);

/**
 * @brief  Enables the I/O compensation cell and waits for it to become ready.
 *
 *         Requires CSI clock ready (RCC_CR.CSIRDY == 1). Sets SYSCFG_CCCSR.EN
 *         inside the section (RM0399 §12.3.11, §13.3.7, §13.6.5), verifies read-back,
 *         exits the section immediately to keep cross-core hold time < 60 us (Plan Rev 2),
 *         and waits outside the section up to max_cycles of DWT->CYCCNT for
 *         SYSCFG_CCCSR.READY == 1.
 *         Enables calling core's RCC APB4ENR.SYSCFGEN bit before taking the lock.
 *         Never writes RCC_CR, SYSCFG_CCCSR.HSLV, SYSCFG_CCCSR.CS, nor SYSCFG_CCCR.
 *         If DWT->CYCCNT does not advance or timeout expires before READY is asserted,
 *         returns GPIO_BM_ERR_NOT_READY and leaves CCCSR.EN = 1.
 *
 * @param  max_cycles  Maximum DWT->CYCCNT cycles to wait for READY. Must be > 0.
 * @retval GPIO_BM_OK           Compensation cell enabled and ready.
 * @retval GPIO_BM_ERR_PARAM    max_cycles == 0.
 * @retval GPIO_BM_ERR_NOT_READY CSI clock not ready (RCC_CR.CSIRDY == 0),
 *                              DWT->CYCCNT not advancing, or timeout before READY.
 * @retval GPIO_BM_ERR_BUSY     Cross-core lock not acquired within timeout.
 * @retval GPIO_BM_ERR_VERIFY   CCCSR.EN write failed read-back verification.
 */
GpioBm_Status_t GPIO_BM_EnableCompensationCell(uint32_t max_cycles);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_BM_H */
