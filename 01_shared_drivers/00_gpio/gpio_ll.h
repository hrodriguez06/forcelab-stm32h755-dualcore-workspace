/**
  ******************************************************************************
  * @file    gpio_ll.h
  * @brief   GPIO pin driver, Low-Layer tier.
  *
  *          Driver tier: *_ll (DRIVER_ARCHITECTURE_GUIDELINES.md). The same
  *          contract as gpio_bm, written against the ST LL constants
  *          (LL_GPIO_MODE_*, LL_GPIO_OUTPUT_*, LL_GPIO_SPEED_FREQ_*,
  *          LL_GPIO_PULL_*, LL_GPIO_AF_*) and implemented with the LL inlines
  *          of stm32h7xx_ll_gpio.h.
  *
  *          What it takes from gpio_bm, so that there is one copy of it:
  *            - the validation (GPIO_BM_CheckConfig, GPIO_BM_IsValidPort), so a
  *              refused call returns exactly the status gpio_bm returns;
  *            - the port clock (GPIO_BM_EnablePortClock);
  *            - the section and the cross-core hook (GPIO_BM_SectionEnter/Exit),
  *              so a project that installed GPIO_BM_SetSharedLock() protects
  *              this tier too.
  *
  *          Self-contained: needs neither USE_FULL_LL_DRIVER (LL_GPIO_Init is
  *          not used) nor stm32h7xx_hal.h. No state of its own in RAM.
  *          Status codes are GpioBm_Status_t, as in hsem_ll.
  ******************************************************************************
  */

#ifndef GPIO_LL_H
#define GPIO_LL_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32h7xx.h"
#include "stm32h7xx_ll_gpio.h"
#include "gpio_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Pin configuration ---------------------------------------------------------- */
typedef struct
{
    GPIO_TypeDef *port;
    uint32_t pin_idx;          /*!< 0..15 (an index, not an LL_GPIO_PIN_x mask)            */
    uint32_t mode;             /*!< LL_GPIO_MODE_INPUT / _OUTPUT / _ALTERNATE / _ANALOG    */
    uint32_t output_type;      /*!< LL_GPIO_OUTPUT_PUSHPULL / _OPENDRAIN; OUTPUT/ALTERNATE */
    uint32_t speed;            /*!< LL_GPIO_SPEED_FREQ_*; OUTPUT/ALTERNATE only            */
    uint32_t pull;             /*!< LL_GPIO_PULL_NO / _UP / _DOWN; must be NO for ANALOG   */
    uint32_t alternate;        /*!< LL_GPIO_AF_0..LL_GPIO_AF_15; ALTERNATE only            */
    uint32_t initial_level;    /*!< 0/1, OUTPUT only: written with BSRR before MODER       */
    bool     allow_debug_pins; /*!< false: PA13/PA14 (SWD) refused                         */
} GpioLL_PinConfig_t;

/**
  * @brief  Configures one pin. Same steps and statuses as GPIO_BM_ConfigPin():
  *         validation before any access, port clock, then inside the section:
  *         LCKR check, BSRR (OUTPUT), OTYPER/OSPEEDR (OUTPUT/ALTERNATE), PUPDR,
  *         AFR (ALTERNATE), MODER last, and a read-back of the five fields.
  * @retval GPIO_BM_OK, _ERR_PARAM, _ERR_DEBUG_PIN, _ERR_LOCKED, _ERR_BUSY, _ERR_VERIFY.
  */
GpioBm_Status_t GPIO_LL_ConfigPin(const GpioLL_PinConfig_t *cfg);

/**
  * @brief  Configures `count` pins in order. Every entry is validated before the
  *         first peripheral access, so a bad entry anywhere writes nothing.
  *         Each pin then gets its own section (one pin per section, so the time
  *         with interrupts masked does not grow with `count`).
  * @param  done  Optional (NULL allowed): number of pins configured when the
  *               function returned. On a failure at entry k, pins 0..k-1 stay
  *               configured and *done = k.
  * @retval The first status other than GPIO_BM_OK, or GPIO_BM_OK.
  *         GPIO_BM_ERR_PARAM for cfgs == NULL or count == 0.
  */
GpioBm_Status_t GPIO_LL_ConfigPins(const GpioLL_PinConfig_t *cfgs, uint32_t count, uint32_t *done);

/** @brief  Analog, AF0, push-pull, low speed, no pull, ODR 0. MODER goes first. Same as GPIO_BM_DeinitPin(). */
GpioBm_Status_t GPIO_LL_DeinitPin(GPIO_TypeDef *port, uint32_t pin_idx);

/** @brief  Reads the five fields and the ODR bit of one pin (LL constants). No section. */
GpioBm_Status_t GPIO_LL_ReadConfig(const GPIO_TypeDef *port, uint32_t pin_idx, GpioLL_PinConfig_t *out);

/**
  * @brief  LCKK sequence with LL_GPIO_LockPin() inside the section, then a read of LCKK.
  *         Same behaviour and statuses as GPIO_BM_LockPins(): OK, ERR_VERIFY,
  *         ERR_PARAM, ERR_BUSY; on a locked port the locked pins are kept in the
  *         sequence and the read-back decides.
  */
GpioBm_Status_t GPIO_LL_LockPins(GPIO_TypeDef *port, uint32_t mask);

/** @retval true when LCKK is set and the pin's LCKy bit is set. false for a bad port or pin.
  * @note   As GPIO_BM_IsLocked(): only reads, so the port clock must be on; an
  *         unclocked port reads all ones (LCKR = 0xFFFFFFFF on Rev V), so
  *         every pin reads as locked. */
bool            GPIO_LL_IsLocked(const GPIO_TypeDef *port, uint32_t pin_idx);

/* ---- Pin operations: static inline, no validation --------------------------------
 * Set, Reset, Write and WritePort are one store to BSRR (atomic, RM0399 12.3.5):
 * safe from both cores and any ISR. Toggle is LL_GPIO_TogglePin: one ODR read and
 * one BSRR store. Masks above bit 15 are ignored, as in gpio_bm.
 * WritePort is NOT LL_GPIO_WriteOutputPort(): that one writes ODR, a whole-port
 * store that can undo a concurrent BSRR from the other core.
 * ---------------------------------------------------------------------------------- */
static inline void GPIO_LL_Set(GPIO_TypeDef *p, uint32_t mask)
{
    LL_GPIO_SetOutputPin(p, mask & 0xFFFFUL);
}

static inline void GPIO_LL_Reset(GPIO_TypeDef *p, uint32_t mask)
{
    LL_GPIO_ResetOutputPin(p, mask & 0xFFFFUL);
}

static inline void GPIO_LL_Toggle(GPIO_TypeDef *p, uint32_t mask)
{
    LL_GPIO_TogglePin(p, mask & 0xFFFFUL);
}

static inline void GPIO_LL_Write(GPIO_TypeDef *p, uint32_t pin_idx, uint32_t level)
{
    const uint32_t m = 1UL << (pin_idx & 0xFU);
    if (level != 0U)
    {
        LL_GPIO_SetOutputPin(p, m);
    }
    else
    {
        LL_GPIO_ResetOutputPin(p, m);
    }
}

static inline void GPIO_LL_WritePort(GPIO_TypeDef *p, uint32_t mask, uint32_t value)
{
    WRITE_REG(p->BSRR, ((mask & ~value & 0xFFFFUL) << 16U) | (mask & value & 0xFFFFUL));
}

static inline bool GPIO_LL_Read(const GPIO_TypeDef *p, uint32_t pin_idx)
{
    return LL_GPIO_IsInputPinSet(p, 1UL << (pin_idx & 0xFU)) != 0UL;
}

static inline uint32_t GPIO_LL_ReadPort(const GPIO_TypeDef *p)
{
    return LL_GPIO_ReadInputPort(p) & 0xFFFFUL;
}

#ifdef __cplusplus
}
#endif

#endif /* GPIO_LL_H */
