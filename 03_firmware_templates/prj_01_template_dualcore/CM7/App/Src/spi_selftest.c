/**
  ******************************************************************************
  * @file    spi_selftest.c
  * @brief   06_spi selftest, CPU1 side (preset selftest-spi), phase F0: the
  *          Analog Discovery 2 bench and the J1 jumper. No driver code yet
  *          (F1-F3).
  *
  *          Prints Rev and build id of both cores, then runs the bench steps
  *          T0a, T0c and T0b(b)(c)(d)(e)(f) with the host
  *          (tools/spi_hw_check.py, which runs T0b(a) on a mock DLL), and
  *          prints the global verdict, CPU2's results included. Plan and
  *          meaning of every test: 01_shared_drivers/06_spi/IMPLEMENTATION_PLAN.md.
  *
  *          Electrical safety (no series resistors on the bench, and J1 ties
  *          two MCU pins together with a wire):
  *            S2  bench_safe(): every wired pin and the four J1 pins in input,
  *                MCU_ACK low, SPI4 released (IER = 0, SPE = 0, CFG2 = 0). At
  *                start, on any abort, on a timeout and at the end.
  *            S4  bench_claim(): the only way a wired pin or a J1 pin becomes an
  *                output or AF (pin_cfg() is the only caller of the GPIO driver
  *                for those pins). The permission comes from the CURRENT step of
  *                k_sst_steps[], which only step_begin() sets.
  *            S5  buschk(): the bus is shown released (both pulls followed) in
  *                the same step, before either side drives it.
  *            S7  every step is announced and waits for the host before it
  *                moves anything; 10 s without the host -> S2 and abort.
  *            S13 PE11..PE14 go to AF5 only in a "bus = MCU" step, and only
  *                after SPI4 reads back SPE = 0, AFCNTR = 0 and MASTER = 1 (or
  *                CFG2 = 0) with its clock on in this core's bank
  *                (spi4_s13_ok()); the S5 check then runs with the pins in AF5.
  *                step_end() fails a step that leaves SPI4 enabled or with
  *                AFCNTR = 1.
  *            S14 PA6 and PB4 are never GPIO outputs; PB5 only in T0C; the J1
  *                pins belong to the one core the step names (CPU1: AF8).
  ******************************************************************************
  */

#include "spi_selftest.h"
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "app_log.h"
#include "stm32h7xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG(fmt, ...)       APP_LOG("[spi] " fmt, ##__VA_ARGS__)

#define CYC_PER_MS          400000U
#define SYNC_TIMEOUT_CYC    4000000000UL          /* 10 s at 400 MHz (CYCCNT wraps at 10.7 s) */
#define CM4_TIMEOUT_MS      30000U
#define CM4_READY_MS        5000U

/* ---- Bench wiring (00_gpio README section 8; 06_spi plan "Banco de pruebas") ---------- */
#define PE_REQ              2U        /* HOST_REQ, AD2 -> MCU (DIO12) */
#define PE_DIO8             3U        /* DIO8,  AD2 only              */
#define PE_DIO10            4U        /* DIO10, AD2 only              */
#define PE_DIO9             5U        /* DIO9,  MCU only (unused here)*/
#define PE_DIO11            6U        /* DIO11, MCU only (unused here)*/
#define PE_ACK              7U        /* MCU_ACK, MCU -> AD2 (DIO13)  */
#define BUS_SHIFT           8U        /* PE8..PE15 = DIO0..DIO7       */
#define BUS_MASK            0xFF00UL
#define PE_NSS              11U       /* DIO3: SPI4_NSS  (AF5)        */
#define PE_SCK              12U       /* DIO4: SPI4_SCK  (AF5)        */
#define PE_MISO             13U       /* DIO5: SPI4_MISO (AF5), input */
#define PE_MOSI             14U       /* DIO6: SPI4_MOSI (AF5)        */
#define PE_SPI4_MASK        ((1UL << PE_NSS) | (1UL << PE_SCK) | (1UL << PE_MISO) | (1UL << PE_MOSI))
#define PA_MCO              8U        /* DIO14, MCU only: MCO1 (AF0)  */
#define PA_J1_SCK           5U        /* CN7-10, unwired              */
#define PA_J1_MISO          6U        /* CN7-12, J1-B                 */
#define PB_J1_MISO          4U        /* CN7-19, unwired (D2 control) */
#define PB_J1_MOSI          5U        /* CN7-14, J1-A                 */
#define PF_SCOPE1           14U
#define PF_SCOPE2           15U

static const char *const k_bus_name[] = { "NONE", "MCU" };
static const char *const k_j1_name[]  = { "NONE", "CM7", "CM4" };

static uint32_t  s_step = SST_STEP_NONE;  /* NONE between steps: nothing wired may be driven */
static bool      s_bench_abort;
static uint32_t  s_fail;                  /* firmware-side verdicts that failed */
static char      s_line[160];             /* verdict() text; one task, no reentrancy */

/* ---- Small helpers ------------------------------------------------------------------ */

static void delay_cycles(uint32_t n)
{
    const uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < n)
    {
    }
}

static void delay_ms(uint32_t ms)
{
    for (uint32_t i = 0U; i < ms; i++)
    {
        delay_cycles(CYC_PER_MS);
    }
}

static void verdict(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void verdict(bool ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(s_line, sizeof(s_line), fmt, ap);
    va_end(ap);
    if (!ok)
    {
        s_fail++;
    }
    LOG("%s -> %s", s_line, ok ? "PASS" : "FAIL");
}

static void publish_step(uint32_t idx)
{
    s_step = idx;
    SST_AREA->step = idx;
    __DMB();
}

/* SPI4's clock in CPU1's RCC bank (RM0399 9.5.10, 9.7.1): the bank this core
   writes; RCC_C1 reads it whichever core runs the code. */
static bool spi4_clocked(void)
{
    return (RCC_C1->APB2ENR & RCC_APB2ENR_SPI4EN) != 0U;
}

/* ---- S2: safe state ------------------------------------------------------------------ */

/* Every wired pin and the four J1 pins in input without pull (PE2 with
   pull-down, so HOST_REQ reads 0 while the AD2 does not drive it), their AF
   fields at 0, MCU_ACK driven low. PE0 (plan R4) and PE1 (LD2, board_def.h)
   are never touched. Pins first: a pin in input is driven by nobody, whatever
   its peripheral does. Then SPI4, if clocked in this bank, is released the way
   rule 9 of the plan orders it (IER = 0 before SPE = 0, ES0445 2.22.3, then
   CFG2 = 0: AFCNTR = 0 lets go of the pins, RM0399 53:3456-3464). Raw writes:
   the safe state must not depend on code under test. The cross-core lock is
   asked for, and the writes happen even if it is refused, with this core's
   interrupts masked: safety first. */
static void bench_safe(void)
{
    GpioBm_Section_t s;
    const bool locked = (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK);
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();

    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOEEN | RCC_AHB4ENR_GPIOFEN;
    (void)RCC->AHB4ENR;

    const uint32_t e_pins = 0xFFFCUL & ~(1UL << PE_ACK);             /* PE2..PE6, PE8..PE15 */
    uint32_t mask2 = 0U;
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if ((e_pins & (1UL << p)) != 0U)
        {
            mask2 |= 3UL << (p * 2U);
        }
    }
    GPIOE->MODER   = GPIOE->MODER & ~mask2;
    GPIOE->PUPDR   = (GPIOE->PUPDR & ~mask2) | (2UL << (PE_REQ * 2U));
    GPIOE->AFR[0] &= 0x000000FFUL;                                    /* PE2..PE7 AF0 */
    GPIOE->AFR[1]  = 0U;                                              /* PE8..PE15 AF0 */

    GPIOE->BSRR   = 1UL << (PE_ACK + 16U);                            /* level first */
    GPIOE->OTYPER = GPIOE->OTYPER & ~(1UL << PE_ACK);
    GPIOE->PUPDR  = GPIOE->PUPDR & ~(3UL << (PE_ACK * 2U));
    GPIOE->MODER  = (GPIOE->MODER & ~(3UL << (PE_ACK * 2U))) | (1UL << (PE_ACK * 2U));

    const uint32_t a2 = (3UL << (PA_MCO * 2U)) | (3UL << (PA_J1_SCK * 2U)) | (3UL << (PA_J1_MISO * 2U));
    GPIOA->MODER   = GPIOA->MODER & ~a2;
    GPIOA->PUPDR   = GPIOA->PUPDR & ~a2;
    GPIOA->AFR[0] &= ~((0xFUL << (PA_J1_SCK * 4U)) | (0xFUL << (PA_J1_MISO * 4U)));
    GPIOA->AFR[1] &= ~(0xFUL << ((PA_MCO - 8U) * 4U));

    const uint32_t b2 = (3UL << (PB_J1_MISO * 2U)) | (3UL << (PB_J1_MOSI * 2U));
    GPIOB->MODER   = GPIOB->MODER & ~b2;
    GPIOB->PUPDR   = GPIOB->PUPDR & ~b2;
    GPIOB->AFR[0] &= ~((0xFUL << (PB_J1_MISO * 4U)) | (0xFUL << (PB_J1_MOSI * 4U)));

    const uint32_t f2 = (3UL << (PF_SCOPE1 * 2U)) | (3UL << (PF_SCOPE2 * 2U));
    GPIOF->MODER = GPIOF->MODER & ~f2;
    GPIOF->PUPDR = GPIOF->PUPDR & ~f2;
    __DSB();

    if (spi4_clocked())
    {
        SPI4->IER  = 0U;
        SPI4->CR1  = 0U;
        SPI4->CFG2 = 0U;
        __DSB();
    }

    __set_PRIMASK(pm);
    if (locked)
    {
        GPIO_BM_SectionExit(&s);
    }
}

static void bench_abort(const char *reason)
{
    if (!s_bench_abort)
    {
        s_bench_abort = true;
        s_fail++;
    }
    bench_safe();
    LOG("ABORT step=%s reason=%s", (s_step < k_sst_step_count) ? k_sst_steps[s_step].name : "-", reason);
}

/* ---- S4 + S13 + S14: the claim --------------------------------------------------------- */

/* S13: SPI4 read back before PE11..PE14 may go to AF5. Its clock is on in this
   core's bank (an unclocked SPI4 cannot be read back at all, so it is refused
   before any SPI4 read), SPE = 0 and AFCNTR = 0 (with both at 0 the SPI takes
   no control of its pins, RM0399 53:3456-3464), and it is a master (MASTER =
   1) or at its reset value (CFG2 = 0). Reads only. */
static bool spi4_s13_ok(void)
{
    if (!spi4_clocked())
    {
        return false;
    }
    const uint32_t cr1  = SPI4->CR1;
    const uint32_t cfg2 = SPI4->CFG2;
    if (((cr1 & SPI_CR1_SPE) != 0U) || ((cfg2 & SPI_CFG2_AFCNTR) != 0U))
    {
        return false;
    }
    return ((cfg2 & SPI_CFG2_MASTER) != 0U) || (cfg2 == 0U);
}

/* All or nothing: every pin of the mask is judged before anything is written. */
static bool bench_claim(const GPIO_TypeDef *port, uint32_t mask, uint32_t mode, uint32_t af)
{
    if ((mask & 0xFFFFUL) == 0U)
    {
        return false;
    }
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if (((mask & (1UL << p)) != 0U) && !sst_line_rule(0U, s_step, port, p, mode, af))
        {
            return false;
        }
    }
    if ((port == GPIOE) && (mode == GPIO_BM_MODE_AF) && ((mask & PE_SPI4_MASK) != 0U))
    {
        return spi4_s13_ok();
    }
    return true;
}

/* The only path to the GPIO driver for a pin of this selftest. Inputs need no
   claim (nothing is driven); an output or an AF is claimed first. */
static bool pin_cfg(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af, uint32_t pull, uint32_t level)
{
    GpioBm_Status_t st;
    if ((mode == GPIO_BM_MODE_OUTPUT) || (mode == GPIO_BM_MODE_AF))
    {
        if (!bench_claim(port, 1UL << pin, mode, af))
        {
            bench_abort("claim refused");
            return false;
        }
    }
    if (mode == GPIO_BM_MODE_OUTPUT)
    {
        st = GPIO_BM_ConfigOutput(port, pin, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, pull, level);
    }
    else if (mode == GPIO_BM_MODE_AF)
    {
        st = GPIO_BM_ConfigAlternateEx(port, pin, af, GPIO_BM_OTYPE_PP, pull, GPIO_BM_SPEED_LOW);
    }
    else
    {
        st = GPIO_BM_ConfigInput(port, pin, pull);
    }
    if (st != GPIO_BM_OK)
    {
        LOG("gpio_bm config of pin %lu failed, status %lu", (unsigned long)pin, (unsigned long)st);
        bench_abort("config");
        return false;
    }
    return true;
}

static bool pins_input(GPIO_TypeDef *port, uint32_t mask, uint32_t pull)
{
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if (((mask & (1UL << p)) != 0U) && !pin_cfg(port, p, GPIO_BM_MODE_INPUT, 0U, pull, 0U))
        {
            return false;
        }
    }
    return true;
}

/* ---- Handshake (S7) --------------------------------------------------------------------- */

static void ack(bool high)
{
    GPIOE->BSRR = high ? (1UL << PE_ACK) : (1UL << (PE_ACK + 16U));
    __DSB();
}

static bool wait_req(bool high, uint32_t timeout_cyc)
{
    const uint32_t t0 = DWT->CYCCNT;
    while ((((GPIOE->IDR >> PE_REQ) & 1UL) != 0UL) != high)
    {
        if ((DWT->CYCCNT - t0) >= timeout_cyc)
        {
            return false;
        }
    }
    return true;
}

/* One four-phase handshake: the MCU state is ready when ACK rises; the host
   acts or measures, then raises REQ; ACK drops and the MCU waits for REQ to
   drop. */
static bool sync0(void)
{
    if (s_bench_abort)
    {
        return false;
    }
    ack(true);
    bool ok = wait_req(true, SYNC_TIMEOUT_CYC);
    ack(false);
    if (ok)
    {
        ok = wait_req(false, SYNC_TIMEOUT_CYC);
    }
    if (!ok)
    {
        bench_abort("host timeout");
    }
    return ok;
}

static bool step_begin(const char *name)
{
    if (s_bench_abort)
    {
        return false;
    }
    const uint32_t idx = sst_step_index(name);
    if (idx == SST_STEP_NONE)
    {
        bench_abort("unknown step");
        return false;
    }
    publish_step(idx);
    LOG("STEP %s bus=%s j1=%s v=%u", k_sst_steps[idx].name, k_bus_name[k_sst_steps[idx].bus],
        k_j1_name[k_sst_steps[idx].j1], (unsigned)SPI_BENCH_VERSION);
    return sync0();   /* nothing moves until the host has checked the announcement */
}

/* S13: a step hands the bus back with SPI4 disabled and AFCNTR = 0 (a step that
   used keep_pins must have run Deinit). Checked here, then forced anyway. */
static void step_end(void)
{
    if ((s_step < k_sst_step_count) && spi4_clocked())
    {
        const uint32_t cr1  = SPI4->CR1;
        const uint32_t cfg2 = SPI4->CFG2;
        if (((cr1 & SPI_CR1_SPE) != 0U) || ((cfg2 & SPI_CFG2_AFCNTR) != 0U))
        {
            verdict(false, "step %s ended with SPI4 CR1=0x%08lX CFG2=0x%08lX (SPE or AFCNTR set)",
                    k_sst_steps[s_step].name, (unsigned long)cr1, (unsigned long)cfg2);
            bench_abort("SPI4 not released at the end of the step");
        }
    }
    publish_step(SST_STEP_NONE);
}

/* ---- S5: the bus is released, shown electrically --------------------------------------- */

static uint32_t bus_idr(void)
{
    return (GPIOE->IDR >> BUS_SHIFT) & 0xFFUL;
}

/* Pull of every bus pin; the pins in `af_mask` stay in AF5 (claimed again,
   so SPI4 is read back each time). */
static bool bus_pull(uint32_t pull, uint32_t af_mask)
{
    for (uint32_t p = BUS_SHIFT; p < 16U; p++)
    {
        const bool af = (af_mask & (1UL << p)) != 0U;
        if (!pin_cfg(GPIOE, p, af ? GPIO_BM_MODE_AF : GPIO_BM_MODE_INPUT, af ? SST_AF_SPI4 : 0U, pull, 0U))
        {
            return false;
        }
    }
    return true;
}

/* Both MCU pulls, each followed by a host check of the same lines; `af_mask`
   keeps those pins in AF5 (S13). Aborts if the MCU itself does not read them. */
static bool buschk_af(uint32_t af_mask, uint32_t *up_out, uint32_t *dn_out)
{
    if (!bus_pull(GPIO_BM_PULL_UP, af_mask))
    {
        return false;
    }
    delay_ms(1U);
    const uint32_t up = bus_idr();
    if (!sync0())
    {
        return false;
    }
    if (!bus_pull(GPIO_BM_PULL_DOWN, af_mask))
    {
        return false;
    }
    delay_ms(1U);
    const uint32_t dn = bus_idr();
    if (!sync0())
    {
        return false;
    }
    if (up_out != NULL)
    {
        *up_out = up;
    }
    if (dn_out != NULL)
    {
        *dn_out = dn;
    }
    if ((up != 0xFFU) || (dn != 0x00U))
    {
        LOG("BUSCHK idr up=0x%02lX down=0x%02lX", (unsigned long)up, (unsigned long)dn);
        bench_abort("bus not released");
        return false;
    }
    return true;
}

static bool buschk(void)
{
    if (!buschk_af(0U, NULL, NULL))
    {
        return false;
    }
    return pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
}
