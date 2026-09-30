/**
  ******************************************************************************
  * @file    gpio_hal.h
  * @brief   GPIO pin driver, HAL tier.
  *
  *          Driver tier: *_hal (DRIVER_ARCHITECTURE_GUIDELINES.md). Wraps
  *          HAL_GPIO_Init() and HAL_GPIO_LockPin() and takes the HAL constants
  *          (GPIO_MODE_*, GPIO_NOPULL/PULLUP/PULLDOWN, GPIO_SPEED_FREQ_*,
  *          GPIO_AFx_*). What it adds to the HAL, and why it exists:
  *            - validation that survives USE_FULL_ASSERT being off (the HAL
  *              only has assert_param), with the rules of gpio_bm;
  *            - the port clock (the HAL leaves it to the caller);
  *            - every HAL_GPIO_Init() of a pin runs inside the gpio_bm section,
  *              so the cross-core hook of GPIO_BM_SetSharedLock() protects it
  *              (HAL_GPIO_Init read-modify-writes MODER/OTYPER/OSPEEDR/PUPDR/AFR,
  *              which both cores share);
  *            - the initial output level written with BSRR before the pin
  *              becomes an output (HAL_GPIO_Init never writes ODR);
  *            - ERR on a pin frozen by LCKR, which HAL_GPIO_Init writes silently,
  *              and a read-back of the five fields afterwards;
  *            - analog with a pull refused (HAL_GPIO_Init skips PUPDR for
  *              analog + PULLUP and writes it for analog + PULLDOWN);
  *            - a lock that does not report HAL_OK for pins it did not lock.
  *
  *          Out of scope: EXTI. The GPIO_MODE_IT_* and GPIO_MODE_EVT_* modes are
  *          refused, and GPIO_HAL_DeinitPin() never calls HAL_GPIO_DeInit(),
  *          which writes the shared EXTI registers (see 00_gpio/README.md).
  *
  *          Status mapping (GPIO_HAL_Status): GPIO_BM_OK -> HAL_OK,
  *          GPIO_BM_ERR_BUSY -> HAL_BUSY, every other status -> HAL_ERROR.
  *          Compiles to nothing without HAL_GPIO_MODULE_ENABLED. No state of
  *          its own in RAM; never defines HAL_GPIO_EXTI_Callback or a handler.
  ******************************************************************************
  */

#ifndef GPIO_HAL_H
#define GPIO_HAL_H

#include "stm32h7xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

#if defined(HAL_GPIO_MODULE_ENABLED)

#include "gpio_bm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    GPIO_TypeDef *port;
    uint32_t      pin_idx;          /*!< 0..15 (an index; GPIO_BM_PIN_INDEX(GPIO_PIN_x) converts) */
    uint32_t      mode;             /*!< GPIO_MODE_INPUT, _OUTPUT_PP, _OUTPUT_OD, _AF_PP, _AF_OD,
                                         _ANALOG. EXTI modes are refused                         */
    uint32_t      pull;             /*!< GPIO_NOPULL / GPIO_PULLUP / GPIO_PULLDOWN; NOPULL if ANALOG */
    uint32_t      speed;            /*!< GPIO_SPEED_FREQ_*; output and AF modes only              */
    uint32_t      alternate;        /*!< GPIO_AFx_*; AF modes only                                */
    GPIO_PinState initial_level;    /*!< output modes only: BSRR before HAL_GPIO_Init             */
    bool          allow_debug_pins; /*!< false: PA13/PA14 (SWD) refused                           */
} GpioHal_PinConfig_t;

/** @brief  The fixed mapping of a gpio_bm status onto HAL_StatusTypeDef. */
static inline HAL_StatusTypeDef GPIO_HAL_Status(GpioBm_Status_t st)
{
    return (st == GPIO_BM_OK) ? HAL_OK : ((st == GPIO_BM_ERR_BUSY) ? HAL_BUSY : HAL_ERROR);
}

/**
  * @brief  Configures one pin. Validation before any access, port clock, then
  *         inside the section: LCKR check, BSRR (output modes), HAL_GPIO_Init()
  *         for that single pin, and a read-back of the five fields.
  * @retval HAL_OK; HAL_BUSY when the cross-core hook did not get the lock
  *         (nothing written); HAL_ERROR for a refused parameter, a SWD pin,
  *         a locked pin (nothing written) or a read-back mismatch.
  */
HAL_StatusTypeDef GPIO_HAL_ConfigPin(const GpioHal_PinConfig_t *cfg);

/**
  * @brief  Drop-in replacement for HAL_GPIO_Init(port, init), for MSP code.
  *         Same fields, same meaning, and the same result in the registers;
  *         in addition: the whole request is validated before the first access,
  *         the port clock is enabled, a pin mask with a locked pin writes
  *         nothing, and each pin of `init->Pin` is written in its own section
  *         with a read-back. Like HAL_GPIO_Init it never writes ODR: set the
  *         level of an output first (HAL_GPIO_WritePin) or use GPIO_HAL_ConfigPin.
  *         PA13/PA14 in the mask are refused; use GPIO_HAL_ConfigPin with
  *         allow_debug_pins to reconfigure SWD on purpose.
  * @retval HAL_OK, HAL_BUSY or HAL_ERROR as GPIO_HAL_ConfigPin. On a failure
  *         at pin k of the mask, the lower pins stay configured.
  */
HAL_StatusTypeDef GPIO_HAL_Init(GPIO_TypeDef *port, const GPIO_InitTypeDef *init);

/**
  * @brief  Analog, AF0, push-pull, low speed, no pull, ODR 0, MODER first:
  *         GPIO_BM_DeinitPin() with the HAL status. Not HAL_GPIO_DeInit(): that
  *         one clears the calling core's EXTI masks and the shared RTSR1/FTSR1/
  *         EXTICR of any line routed to this port, including every GPIOA line
  *         while EXTICR still holds its reset value.
  */
HAL_StatusTypeDef GPIO_HAL_DeinitPin(GPIO_TypeDef *port, uint32_t pin_idx);

/**
  * @brief  HAL_GPIO_LockPin() inside the section. HAL_OK only when LCKK reads 1
  *         and every pin of `mask` reads locked; on a port that was already
  *         locked, HAL_OK only when those pins are among the locked ones
  *         (HAL_GPIO_LockPin alone returns HAL_OK there regardless).
  */
HAL_StatusTypeDef GPIO_HAL_LockPins(GPIO_TypeDef *port, uint32_t mask);

/* Pin levels: HAL_GPIO_WritePin, HAL_GPIO_TogglePin and HAL_GPIO_ReadPin are one
   BSRR store (one ODR read before it, for Toggle) or one IDR read, so they are
   safe from both cores and need no wrapper. */

#ifdef __cplusplus
}
#endif

#endif /* HAL_GPIO_MODULE_ENABLED */

#endif /* GPIO_HAL_H */
