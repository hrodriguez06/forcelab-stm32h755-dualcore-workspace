/**
  ******************************************************************************
  * @file    spi_selftest.c
  * @brief   06_spi selftest, CPU2 side (preset selftest-spi), phase F0.
  *
  *          CPU2 has no console. It publishes its Rev, build id and step-table
  *          version in the SRAM4 block (Common/Inc/spi_selftest.h), serves the
  *          commands CPU1 writes there, and CPU1 prints and judges the results.
  *
  *          Electrical safety: the only pins CPU2 may ever configure as AF are
  *          the four J1 pins (PA5, PA6, PB4, PB5) in AF5 (SPI1), and only in a
  *          step whose J1 owner is CPU2; none of them, ever, as a GPIO output
  *          from this core (S14: PA6 and PB4 never; PB5 only in T0C, whose
  *          owner is CPU1). No AD2 line is CPU2's. bench_claim() judges every
  *          output/AF against the step CPU1 published (S4), and bench_safe()
  *          puts the four J1 pins back in input without pull (S2, S14).
  *          pin_cfg() is the only caller of the GPIO driver for those pins.
  ******************************************************************************
  */

#include "spi_selftest.h"
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "stm32h7xx_hal.h"

/* CPU1 sends the next command within seconds; a minute of silence means it
   stopped (a fault, or a bench abort that skipped the DONE). CPU2 then goes to
   S2 and on to the application loop instead of waiting for ever. */
#define SST_CM4_IDLE_TIMEOUT_MS   60000U

#define PA_J1_SCK    5U
#define PA_J1_MISO   6U
#define PB_J1_MISO   4U
#define PB_J1_MOSI   5U

/* ---- S2 --------------------------------------------------------------------------- */

/* PA5, PA6, PB4, PB5 in input without pull, AF fields 0. Raw writes inside the
   cross-core lock (taken if it can be; the writes happen regardless,
   interrupts masked). */
static void bench_safe(void)
{
    GpioBm_Section_t s;
    const bool locked = (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK);
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN;   /* CPU2's bank (RM0399 9.5.10) */
    (void)RCC->AHB4ENR;
    const uint32_t a2 = (3UL << (PA_J1_SCK * 2U)) | (3UL << (PA_J1_MISO * 2U));
    GPIOA->MODER   = GPIOA->MODER & ~a2;
    GPIOA->PUPDR   = GPIOA->PUPDR & ~a2;
    GPIOA->AFR[0] &= ~((0xFUL << (PA_J1_SCK * 4U)) | (0xFUL << (PA_J1_MISO * 4U)));
    const uint32_t b2 = (3UL << (PB_J1_MISO * 2U)) | (3UL << (PB_J1_MOSI * 2U));
    GPIOB->MODER   = GPIOB->MODER & ~b2;
    GPIOB->PUPDR   = GPIOB->PUPDR & ~b2;
    GPIOB->AFR[0] &= ~((0xFUL << (PB_J1_MISO * 4U)) | (0xFUL << (PB_J1_MOSI * 4U)));
    __DSB();
    __set_PRIMASK(pm);
    if (locked)
    {
        GPIO_BM_SectionExit(&s);
    }
}

static void bench_abort(void)
{
    SST_AREA->m4_aborts++;
    bench_safe();
}

/* ---- S4, S14 ------------------------------------------------------------------------- */

/* All or nothing, against the step CPU1 published in the block. */
static bool bench_claim(const GPIO_TypeDef *port, uint32_t mask, uint32_t mode, uint32_t af)
{
    const uint32_t step = SST_AREA->step;
    if ((mask & 0xFFFFUL) == 0U)
    {
        return false;
    }
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if (((mask & (1UL << p)) != 0U) && !sst_line_rule(1U, step, port, p, mode, af))
        {
            return false;
        }
    }
    return true;
}

static bool pin_cfg(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af, uint32_t pull)
{
    GpioBm_Status_t st;
    if ((mode == GPIO_BM_MODE_OUTPUT) || (mode == GPIO_BM_MODE_AF))
    {
        if (!bench_claim(port, 1UL << pin, mode, af))
        {
            bench_abort();
            return false;
        }
    }
    if (mode == GPIO_BM_MODE_AF)
    {
        st = GPIO_BM_ConfigAlternateEx(port, pin, af, GPIO_BM_OTYPE_PP, pull, GPIO_BM_SPEED_LOW);
    }
    else if (mode == GPIO_BM_MODE_INPUT)
    {
        st = GPIO_BM_ConfigInput(port, pin, pull);
    }
    else
    {
        st = GPIO_BM_ERR_PARAM;    /* CPU2 never makes a pin of this bench a GPIO output */
    }
    if (st != GPIO_BM_OK)
    {
        bench_abort();
        return false;
    }
    return true;
}
