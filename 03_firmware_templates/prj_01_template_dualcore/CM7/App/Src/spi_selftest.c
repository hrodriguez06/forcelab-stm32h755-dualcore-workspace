/**
  ******************************************************************************
  * @file    spi_selftest.c
  * @brief   06_spi selftest, CPU1 side (preset selftest-spi), phases F0 (the
  *          Analog Discovery 2 bench and the J1 jumper) and F1 (spi_bm).
  *
  *          Prints Rev and build id of both cores, then runs the bench steps
  *          T0a, T0c and T0b(b)(c)(d)(e)(f) with the host
  *          (tools/spi_hw_check.py, which runs T0b(a) on a mock DLL), then the
  *          F1 steps: F1_NOPIN (T1-T3, T7(d), T10, T11(a), T13, T14 with no pin),
  *          J1_SPI6 and J1_SPI1 (T4, T6-T9, T11(b), T12(a), T14, B1 through the
  *          jumper, SPI1 on CPU2) and SPI4_AD2 (T5, T7(c), T9, T12(b), T14
  *          against the AD2, which only listens), then the F2 steps (spi_ll): F2_NOPIN
  *          (T1, T2 parity with spi_bm, T14), J1_SPI6_LL and J1_SPI1_LL (T4, T6, T7, T14,
  *          B1; T15 with spi_bm and spi_ll) and SPI4_AD2_LL (T5(a)), then the F3 steps
  *          (spi_hal, CPU1): F3_NOPIN (T1, T2 against spi_bm and the raw HAL, T14),
  *          J1_SPI6_HAL (T4, T6, T7(b), T14, B1) and SPI4_AD2_HAL (T5(a), T7(c)), and
  *          prints the global verdict, CPU2's results included. Plan and meaning of every test:
  *          01_shared_drivers/06_spi/IMPLEMENTATION_PLAN.md.
  *
  *          Electrical safety (no series resistors on the bench, and J1 ties
  *          two MCU pins together with a wire):
  *            S2  bench_safe(): every wired pin and the four J1 pins in input,
  *                MCU_ACK low, SPI4 and SPI6 released in the order of plan rule
  *                9 (spi_release()). At start, on any abort, on a timeout and
  *                at the end.
  *            S4  bench_claim(): the only way a wired pin or a J1 pin becomes an
  *                output or AF (pin_cfg() is the only caller of the GPIO driver
  *                for those pins). The permission comes from the CURRENT step of
  *                k_sst_steps[], which only step_begin() sets.
  *            S5  buschk(): the bus is shown released (both pulls followed) in
  *                the same step, before either side drives it.
  *            S7  every step is announced and waits for the host before it
  *                moves anything; 10 s without the host -> S2 and abort.
  *            S13 PE11..PE14 go to AF5 only in a "bus = MCU" step, and only
  *                after SPI4 reads back at rest (sst_spi_at_rest(): SPE = 0,
  *                AFCNTR = 0, IOSWP = 0, MASTER = 1 or CFG2 = 0) with its clock
  *                on in this core's bank (spi4_s13_ok()); the S5 check then
  *                runs with the pins in AF5. step_end() fails a step that
  *                leaves SPI4 or SPI6 enabled or with AFCNTR = 1.
  *            S14 PA6 and PB4 are never GPIO outputs; PB5 only in T0C; the J1
  *                pins belong to the one core the step names (CPU1: AF8), and
  *                go to AF only with SPI6 read back at rest, clocked in this
  *                core's bank (spi6_j1_ok(), plan R2-3).
  ******************************************************************************
  */

#include "spi_selftest.h"
#include "spi_bm.h"
#include "spi_hal.h"
#include "dma_bm.h"
#include "dma_hal.h"
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
static char      s_line[400];             /* verdict()/log_long() text; one task, no reentrancy */
static uint32_t  s_af_speed = GPIO_BM_SPEED_LOW;   /* speed of a claimed AF pin (T8: VERY_HIGH on J1) */
static uint32_t  s_mco_saved;             /* RCC_CFGR MCO1 fields before T5(g) (plan R4) */
static bool      s_mco_changed;

/* RCC_CFGR.MCO1/MCO1PRE back to what they were (plan R4: written only in the steps
   of T5, saved before and restored with read-back). */
static void mco_restore(void)
{
    if (s_mco_changed)
    {
        MODIFY_REG(RCC->CFGR, RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE, s_mco_saved);
        s_mco_changed = false;
    }
}

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

/* App_Log() cuts a line at APP_LOG_LINE_MAX (160, CM7/App/Src/app_log.c): in
   the first run (docs/spi_f0_bench_65e19f7.txt) two lines lost their end,
   one of them its "-> PASS". So s_line is printed in pieces of at most
   LOG_CHUNK characters, cut at a space, the next ones starting with "| ",
   and `suffix` goes on the last piece. */
#define LOG_CHUNK           120U

static void log_text(const char *suffix)
{
    const char *t = s_line;
    size_t n = strlen(t);
    bool first = true;
    while (n > LOG_CHUNK)
    {
        size_t cut = LOG_CHUNK;
        while ((cut > (LOG_CHUNK / 2U)) && (t[cut] != ' '))
        {
            cut--;
        }
        LOG("%s%.*s", first ? "" : "| ", (int)cut, t);
        t += cut;
        n -= cut;
        while ((n > 0U) && (*t == ' '))
        {
            t++;
            n--;
        }
        first = false;
    }
    LOG("%s%s%s", first ? "" : "| ", t, suffix);
}

static void log_long(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_long(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(s_line, sizeof(s_line), fmt, ap);
    va_end(ap);
    log_text("");
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
    log_text(ok ? " -> PASS" : " -> FAIL");
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

/* SPI6's clock in CPU1's bank: SPI6 is CPU1's J1 instance (AF8, plan R2-3). */
static bool spi6_clocked(void)
{
    return (RCC_C1->APB4ENR & RCC_APB4ENR_SPI6EN) != 0U;
}

/* Releases a clocked SPI in the order of plan rule 9 (R2-2): IER = 0 (before
   SPE = 0, ES0445 2.22.3) -> SPE = 0 keeping SSI -> CFG2 = 0 (AFCNTR = 0 lets
   go of the pins, RM0399 53:3456-3464; a slave has no mode fault, 53:1475-1476)
   -> CR1 = 0 -> every IFCR flag in one write. */
static void spi_release(SPI_TypeDef *spi)
{
    spi->IER  = 0U;
    spi->CR1  = spi->CR1 & SPI_CR1_SSI;
    spi->CFG2 = 0U;
    spi->CR1  = 0U;
    spi->IFCR = SST_SPI_IFCR_ALL;
    __DSB();
}

/* ---- S2: safe state ------------------------------------------------------------------ */

/* Every wired pin and the four J1 pins in input without pull (PE2 with
   pull-down, so HOST_REQ reads 0 while the AD2 does not drive it), their AF
   fields at 0, MCU_ACK driven low. PE0 (plan R4) and PE1 (LD2, board_def.h)
   are never touched. Pins first: a pin in input is driven by nobody, whatever
   its peripheral does. Then SPI4 (S13) and SPI6 (S14, plan R2-3), each if
   clocked in this bank, are released by spi_release() (rule 9). Raw writes:
   the safe state must not depend on code under test. The cross-core lock is
   asked for, and the writes happen even if it is refused, with this core's
   interrupts masked: safety first. */
static void bench_safe(void)
{
    GpioBm_Section_t s;
    const bool locked = (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK);
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();

    mco_restore();
    s_af_speed = GPIO_BM_SPEED_LOW;
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
        spi_release(SPI4);
    }
    if (spi6_clocked())
    {
        spi_release(SPI6);
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
   before any SPI4 read), then sst_spi_at_rest(): SPE = 0, AFCNTR = 0, IOSWP = 0,
   master or CFG2 = 0. Reads only. */
static bool spi4_s13_ok(void)
{
    return spi4_clocked() && sst_spi_at_rest(SPI4->CR1, SPI4->CFG2);
}

/* S14 with plan R2-3: SPI6, CPU1's J1 instance, read back the same way before a
   J1 pin may go to AF (AF8). Reads only. */
static bool spi6_j1_ok(void)
{
    return spi6_clocked() && sst_spi_at_rest(SPI6->CR1, SPI6->CFG2);
}

#define J1_MASK_A   ((1UL << PA_J1_SCK) | (1UL << PA_J1_MISO))
#define J1_MASK_B   ((1UL << PB_J1_MISO) | (1UL << PB_J1_MOSI))

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
    if ((mode == GPIO_BM_MODE_AF) &&
        (((port == GPIOA) && ((mask & J1_MASK_A) != 0U)) || ((port == GPIOB) && ((mask & J1_MASK_B) != 0U))))
    {
        return spi6_j1_ok();
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
        st = GPIO_BM_ConfigAlternateEx(port, pin, af, GPIO_BM_OTYPE_PP, pull, s_af_speed);
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
static bool spi_left_driving(const char *name, SPI_TypeDef *spi, bool clocked)
{
    if (!clocked || (s_step >= k_sst_step_count))
    {
        return false;
    }
    const uint32_t cr1  = spi->CR1;
    const uint32_t cfg2 = spi->CFG2;
    if (((cr1 & SPI_CR1_SPE) == 0U) && ((cfg2 & SPI_CFG2_AFCNTR) == 0U))
    {
        return false;
    }
    verdict(false, "step %s ended with %s CR1=0x%08lX CFG2=0x%08lX (SPE or AFCNTR set)",
            k_sst_steps[s_step].name, name, (unsigned long)cr1, (unsigned long)cfg2);
    return true;
}

static void step_end(void)
{
    const bool bad4 = spi_left_driving("SPI4", SPI4, spi4_clocked());
    const bool bad6 = spi_left_driving("SPI6", SPI6, spi6_clocked());
    if (bad4 || bad6)
    {
        bench_abort("SPI4 or SPI6 not released at the end of the step");
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

/* ==========================================================================================
 * CPU2 commands
 * ========================================================================================== */

static bool cm4_send4(SstCmd_t cmd, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    SstArea_t *a = SST_AREA;
    a->cmd  = (uint32_t)cmd;
    a->arg0 = a0;
    a->arg1 = a1;
    a->arg2 = a2;
    a->arg3 = a3;
    __DMB();
    const uint32_t seq = a->cmd_seq + 1U;
    a->cmd_seq = seq;
    __DMB();
    const uint32_t t0 = HAL_GetTick();
    while (a->ack_seq != seq)
    {
        if ((HAL_GetTick() - t0) > CM4_TIMEOUT_MS)
        {
            LOG("CM4 did not answer command %lu", (unsigned long)cmd);
            s_fail++;
            return false;
        }
    }
    __DMB();
    return true;
}

static bool cm4_send(SstCmd_t cmd, uint32_t a0)
{
    return cm4_send4(cmd, a0, 0U, 0U, 0U);
}

/* CPU2's claim cases in one slot, against the bit pattern written by hand. */
static void cm4_claims(uint32_t slot, uint32_t expect, uint32_t step_expected, const char *what)
{
    if (cm4_send(SST_CMD_CLAIMS, slot))
    {
        const SstClaimResult_t *r = &SST_AREA->m4_claims[slot];
        verdict((r->cases == SST_M4_CLAIM_CASES) && (r->accepted == expect) && (r->regs_changed == 0U) &&
                (r->step_seen == step_expected),
                "%s CM4 claim %s: %lu cases, accepted 0x%05lX (0x%05lX), regs changed 0x%05lX",
                (slot == SST_M4_SLOT_S14) ? "T0B(f)" : "T0B(b)(f)", what, (unsigned long)r->cases,
                (unsigned long)r->accepted, (unsigned long)expect, (unsigned long)r->regs_changed);
    }
}

/* ==========================================================================================
 * Claim tables and the "nothing written" oracle
 * ========================================================================================== */

/* Configuration registers of the ports a claim can name: a refused claim writes none. */
#define SNAP_PORTS   4U
typedef struct { uint32_t r[SNAP_PORTS * 6U]; } PortSnap_t;

static void port_snap(PortSnap_t *s)
{
    const GPIO_TypeDef *const ports[SNAP_PORTS] = { GPIOA, GPIOB, GPIOE, GPIOF };
    uint32_t i = 0U;
    for (uint32_t k = 0U; k < SNAP_PORTS; k++)
    {
        s->r[i++] = ports[k]->MODER;
        s->r[i++] = ports[k]->OTYPER;
        s->r[i++] = ports[k]->OSPEEDR;
        s->r[i++] = ports[k]->PUPDR;
        s->r[i++] = ports[k]->AFR[0];
        s->r[i++] = ports[k]->AFR[1];
    }
}

static bool port_snap_eq(const PortSnap_t *a, const PortSnap_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

typedef struct { GPIO_TypeDef *port; uint16_t mask; uint8_t mode; uint8_t af; } Claim_t;

/* Bit i set = case i was accepted. */
static uint32_t run_claims(const Claim_t *c, uint32_t n)
{
    uint32_t acc = 0U;
    for (uint32_t i = 0U; i < n; i++)
    {
        if (bench_claim(c[i].port, c[i].mask, c[i].mode, c[i].af))
        {
            acc |= 1UL << i;
        }
    }
    return acc;
}

#define NCASES(t)   ((uint32_t)(sizeof(t) / sizeof((t)[0])))
#define O  GPIO_BM_MODE_OUTPUT
#define A  GPIO_BM_MODE_AF
#define J1_ALL_A    ((1U << PA_J1_SCK) | (1U << PA_J1_MISO))
#define J1_ALL_B    ((1U << PB_J1_MISO) | (1U << PB_J1_MOSI))

/* T0b(b), CPU1, step T0B (bus = NONE, J1 = CPU2): every case must be refused. */
static const Claim_t k_t0b_refuse[] = {
    { GPIOE, 1U << 0, O, 0U },          { GPIOE, 1U << 1, O, 0U },          /* PE0 (R4); LD2         */
    { GPIOE, 1U << PE_REQ, O, 0U },     { GPIOE, 1U << PE_REQ, A, 5U },     /* HOST_REQ; SPI4_SCK    */
    { GPIOE, 1U << PE_DIO8, O, 0U },    { GPIOE, 1U << PE_DIO10, A, 5U },   /* AD2 only; SPI4_NSS    */
    { GPIOE, 1U << PE_DIO9, A, 5U },    { GPIOE, 1U << PE_DIO11, A, 5U },   /* SPI4_MISO; SPI4_MOSI  */
    { GPIOE, 1U << PE_ACK, O, 0U },                                         /* MCU_ACK               */
    { GPIOE, 1U << 8, A, 5U },          { GPIOE, 1U << 10, A, 5U },         /* never AF (S13)        */
    { GPIOE, 1U << 15, A, 5U },         { GPIOE, 1U << 9, O, 0U },          /* never AF; bus = NONE  */
    { GPIOE, 1U << PE_SCK, A, 5U },                                         /* SPI4 in bus = NONE    */
    { GPIOA, 1U << PA_MCO, A, 1U },     { GPIOA, 1U << PA_MCO, O, 0U },     /* PA8 AF1; output       */
    { GPIOF, 1U << PF_SCOPE1, O, 0U },  { GPIOC, 1U << 13, O, 0U },         /* scope; PC13 (R4)      */
    { GPIOA, (1U << PA_MCO) | (1U << 13), A, 0U },                          /* mixed with SWD        */
    { GPIOA, 1U << 7, A, 5U },          { GPIOA, 1U << 4, A, 5U },          /* RMII_CRS_DV; PA4 (R4) */
    { GPIOG, 1U << 13, A, 5U },         { GPIOB, 1U << 0, O, 0U },          /* RMII_TXD0; LD1        */
};

/* T0b(f), CPU1 in a step whose J1 owner is CPU2 (T0B): every case refused. */
static const Claim_t k_t0b_j1_refuse[] = {
    { GPIOA, 1U << PA_J1_MISO, A, 8U }, { GPIOB, 1U << PB_J1_MOSI, A, 8U }, /* AF8, but J1 is CPU2's */
    { GPIOA, 1U << PA_J1_SCK, A, 5U },  { GPIOB, 1U << PB_J1_MISO, A, 5U }, /* CPU2's AF too         */
    { GPIOB, 1U << PB_J1_MOSI, O, 0U },                                     /* PB5 out outside T0C   */
};

/* T0b(f), CPU1 in T0C (J1 = CPU1, PB5 output allowed): only case 0 accepted. */
static const Claim_t k_t0c_claims[] = {
    { GPIOB, 1U << PB_J1_MOSI, O, 0U },                                     /* 0 the T0C driver      */
    { GPIOA, 1U << PA_J1_MISO, O, 0U }, { GPIOB, 1U << PB_J1_MISO, O, 0U }, /* never outputs (S14)   */
    { GPIOA, 1U << PA_J1_SCK, O, 0U },                                      /* only PB5 is           */
    { GPIOB, 1U << PB_J1_MOSI, A, 8U },                                     /* no AF in T0C          */
    { GPIOB, J1_ALL_B, O, 0U },                                             /* PB5 with PB4          */
};
#define T0C_CLAIMS_EXPECT   0x00000001UL

/* T0b(f), CPU1 in T0B_S14 (J1 = CPU1, AF8): cases 0..4 accepted, the rest refused. */
static const Claim_t k_s14_claims[] = {
    { GPIOA, 1U << PA_J1_SCK, A, 8U },  { GPIOA, 1U << PA_J1_MISO, A, 8U }, /* 0, 1 SPI6 SCK, MISO   */
    { GPIOB, 1U << PB_J1_MISO, A, 8U }, { GPIOB, 1U << PB_J1_MOSI, A, 8U }, /* 2, 3 MISO alt, MOSI   */
    { GPIOA, J1_ALL_A, A, 8U },                                             /* 4 PA5|PA6 together    */
    { GPIOA, 1U << PA_J1_MISO, O, 0U }, { GPIOB, 1U << PB_J1_MISO, O, 0U }, /* never outputs         */
    { GPIOB, 1U << PB_J1_MOSI, O, 0U }, { GPIOA, 1U << PA_J1_SCK, O, 0U },  /* PB5 only in T0C; PA5  */
    { GPIOA, 1U << PA_J1_MISO, A, 5U }, { GPIOB, 1U << PB_J1_MOSI, A, 5U }, /* AF5 is CPU2's         */
    { GPIOB, 1U << PB_J1_MISO, A, 7U }, { GPIOA, 1U << PA_J1_SCK, A, 0U },  /* SPI2_NSS; AF0         */
    { GPIOB, 1U << PB_J1_MISO, A, 0U },                                     /* NJTRST                */
    { GPIOA, 1U << 7, A, 8U },          { GPIOA, (1U << PA_J1_MISO) | (1U << 7), A, 8U },  /* PA7    */
};
#define S14_CLAIMS_EXPECT   0x0000001FUL

/* T0b(e), CPU1 in T0B_S13 with SPI4 at a correct state: pins and AFs S13 refuses. */
static const Claim_t k_s13_pin_refuse[] = {
    { GPIOE, 1U << PE_SCK, A, 6U },     { GPIOE, 1U << PE_SCK, A, 1U },     /* wrong AF              */
    { GPIOE, 1U << 8, A, 5U },          { GPIOE, 1U << 10, A, 5U },         /* never AF              */
    { GPIOE, 1U << 15, A, 5U },         { GPIOE, 1U << 9, A, 5U },          /* never AF              */
    { GPIOE, 1U << PE_SCK, O, 0U },                                         /* no output in T0B_S13  */
    { GPIOE, 1U << PE_REQ, A, 5U },     { GPIOE, 1U << PE_DIO11, A, 5U },   /* second SPI4 set       */
    { GPIOE, (uint16_t)(PE_SPI4_MASK | (1UL << 10)), A, 5U },               /* SPI4 with PE10        */
    { GPIOA, 1U << PA_J1_SCK, A, 8U },                                      /* J1 = NONE here        */
};
#undef O
#undef A

/* ==========================================================================================
 * Bench steps
 * ========================================================================================== */

/* The 13 lines T0a pulls, in DIO order: DIO0..DIO7 (PE8..PE15), DIO8 (PE3),
   DIO9 (PE5), DIO10 (PE4), DIO11 (PE6), DIO14 (PA8). HOST_REQ (DIO12) and
   MCU_ACK (DIO13) are exercised by every handshake. */
typedef struct { GPIO_TypeDef *port; uint8_t pin; uint8_t dio; } Line_t;
static const Line_t k_lines[] = {
    { GPIOE, 8U, 0U }, { GPIOE, 9U, 1U }, { GPIOE, 10U, 2U }, { GPIOE, 11U, 3U },
    { GPIOE, 12U, 4U }, { GPIOE, 13U, 5U }, { GPIOE, 14U, 6U }, { GPIOE, 15U, 7U },
    { GPIOE, PE_DIO8, 8U }, { GPIOE, PE_DIO9, 9U }, { GPIOE, PE_DIO10, 10U }, { GPIOE, PE_DIO11, 11U },
    { GPIOA, PA_MCO, 14U },
};
#define N_LINES      (sizeof(k_lines) / sizeof(k_lines[0]))
#define T0A_CTRL_K   9U     /* the control pulls DIO9 (PE5) the wrong way */

/* Bit k of the result = line k reads 1 on the MCU. */
static uint32_t lines_idr(void)
{
    uint32_t v = 0U;
    for (uint32_t k = 0U; k < N_LINES; k++)
    {
        if (((k_lines[k].port->IDR >> k_lines[k].pin) & 1UL) != 0U)
        {
            v |= 1UL << k;
        }
    }
    return v;
}

/* Pull-up on the lines of `up_mask` (bit k = line k), pull-down on the rest. */
static bool lines_pull(uint32_t up_mask)
{
    for (uint32_t k = 0U; k < N_LINES; k++)
    {
        const uint32_t pull = ((up_mask & (1UL << k)) != 0U) ? GPIO_BM_PULL_UP : GPIO_BM_PULL_DOWN;
        if (!pin_cfg(k_lines[k].port, k_lines[k].pin, GPIO_BM_MODE_INPUT, 0U, pull, 0U))
        {
            return false;
        }
    }
    return true;
}

/* T0a (as in 03_tim): every line that is not the handshake follows both MCU
   pulls, then a walking 1 (one line up, the others down: two lines shorted
   together, or a line on the wrong DIO, show up), then the control: all up
   except DIO9 pulled down on purpose, which the same check must flag. */
static void step_t0a(void)
{
    const uint32_t all = (1UL << N_LINES) - 1U;
    if (!step_begin("T0A"))
    {
        return;
    }
    uint32_t up = 0U;
    uint32_t dn = 0U;
    uint32_t walk_bad = 0U;
    for (uint32_t phase = 0U; phase < (2U + N_LINES); phase++)
    {
        const uint32_t pattern = (phase == 0U) ? all : ((phase == 1U) ? 0U : (1UL << (phase - 2U)));
        if (!lines_pull(pattern))
        {
            return;
        }
        delay_ms(1U);
        const uint32_t v = lines_idr();
        if (phase == 0U)
        {
            up = v;
        }
        else if (phase == 1U)
        {
            dn = v;
        }
        else if (v != pattern)
        {
            walk_bad |= 1UL << (phase - 2U);
        }
        if (!sync0())
        {
            return;
        }
    }
    if (!lines_pull(all & ~(1UL << T0A_CTRL_K)))
    {
        return;
    }
    delay_ms(1U);
    const uint32_t ctrl = lines_idr();
    if (!sync0())
    {
        return;
    }
    for (uint32_t k = 0U; k < N_LINES; k++)
    {
        (void)pin_cfg(k_lines[k].port, k_lines[k].pin, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U);
    }
    verdict((up == all) && (dn == 0U), "T0A MCU IDR of the 13 lines, pull-up 0x%04lX (0x%04lX), pull-down 0x%04lX (0x0000)",
            (unsigned long)up, (unsigned long)all, (unsigned long)dn);
    verdict(walk_bad == 0U, "T0A walking 1 on 13 lines, MCU IDR: lines that differ 0x%04lX", (unsigned long)walk_bad);
    verdict((ctrl ^ all) == (1UL << T0A_CTRL_K),
            "T0A control: DIO9 (PE5) pulled down on purpose, MCU check flags 0x%04lX (0x%04lX expected)",
            (unsigned long)(ctrl ^ all), (unsigned long)(1UL << T0A_CTRL_K));
    step_end();
}

/* J1 net levels: bit 0 PA6, bit 1 PB4, bit 2 PA5, bit 3 PB5. */
static uint32_t j1_idr(void)
{
    return ((GPIOA->IDR >> PA_J1_MISO) & 1UL) | (((GPIOB->IDR >> PB_J1_MISO) & 1UL) << 1) |
           (((GPIOA->IDR >> PA_J1_SCK) & 1UL) << 2) | (((GPIOB->IDR >> PB_J1_MOSI) & 1UL) << 3);
}

/* The four J1 pins in input with these pulls (no claim: inputs drive nothing). */
static bool j1_inputs(uint32_t pa6, uint32_t pb4, uint32_t pa5, uint32_t pb5)
{
    return pin_cfg(GPIOA, PA_J1_MISO, GPIO_BM_MODE_INPUT, 0U, pa6, 0U) &&
           pin_cfg(GPIOB, PB_J1_MISO, GPIO_BM_MODE_INPUT, 0U, pb4, 0U) &&
           pin_cfg(GPIOA, PA_J1_SCK, GPIO_BM_MODE_INPUT, 0U, pa5, 0U) &&
           pin_cfg(GPIOB, PB_J1_MOSI, GPIO_BM_MODE_INPUT, 0U, pb5, 0U);
}

#define T0C_PATTERN    0x5A3CU   /* 16 levels of PB5, bit 0 first: 8 ones, 8 zeros, both edges */

/* T0c: the jumper J1 (CN7-14 PB5 <-> CN7-12 PA6), at the start of every
   session. Owner CPU1 (k_sst_steps). Three parts:
   1. Claims (T0b(f) in this step): PB5 as an output accepted; PA6, PB4, PA5
      as outputs and PB5 in AF refused; nothing written.
   2. The plan's control, run FIRST, before anything drives the net: PB5 in
      input without pull, PA6 pulled up and then down. PA6 must follow its own
      pull, and PB5 (through J1) with it: then nothing else drives the J1 net
      and the wire is there, and only then may PB5 become an output.
   3. The test: PB5 a push-pull output (low speed) writes the 16 levels of
      T0C_PATTERN; PA6 in input without pull follows it; PB4 and PA5, unwired,
      in input with the pull opposite to PB5, follow their pull (no short to
      the J1 net).
   Any failure stops the session (bench_abort): the plan says to stop and
   report when T0c fails, and nothing may move a J1 pin after that. */
static void step_t0c(void)
{
    if (!step_begin("T0C"))
    {
        return;
    }
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    const uint32_t acc = run_claims(k_t0c_claims, NCASES(k_t0c_claims));
    port_snap(&after);
    verdict((acc == T0C_CLAIMS_EXPECT) && port_snap_eq(&before, &after),
            "T0C(f) claims in T0C: %lu cases, accepted 0x%02lX (0x%02lX: PB5 output only), regs untouched %u",
            (unsigned long)NCASES(k_t0c_claims), (unsigned long)acc, (unsigned long)T0C_CLAIMS_EXPECT,
            (unsigned)port_snap_eq(&before, &after));

    /* 2. control first: nobody drives the net, PA6's pull moves PA6 and PB5 */
    uint32_t ctl_up = 0U;
    uint32_t ctl_dn = 0U;
    bool ok = j1_inputs(GPIO_BM_PULL_UP, GPIO_BM_PULL_DOWN, GPIO_BM_PULL_DOWN, GPIO_BM_PULL_NONE);
    if (ok)
    {
        delay_ms(1U);
        ctl_up = j1_idr();
        ok = j1_inputs(GPIO_BM_PULL_DOWN, GPIO_BM_PULL_UP, GPIO_BM_PULL_UP, GPIO_BM_PULL_NONE);
    }
    if (ok)
    {
        delay_ms(1U);
        ctl_dn = j1_idr();
    }
    /* up: PA6 1, PB4 0, PA5 0, PB5 1 = 0x9; down: PA6 0, PB4 1, PA5 1, PB5 0 = 0x6 */
    const bool ctl_ok = ok && (ctl_up == 0x9U) && (ctl_dn == 0x6U);
    verdict(ctl_ok, "T0C control: PB5 input no pull, PA6 pull-up -> PA6|PB4|PA5|PB5 = 0x%lX (0x9), "
            "pull-down -> 0x%lX (0x6): PA6 follows its pull and PB5 follows PA6 through J1",
            (unsigned long)ctl_up, (unsigned long)ctl_dn);
    if (!ctl_ok)
    {
        if (!s_bench_abort)
        {
            bench_abort("T0C: J1 net not shown free, PB5 not driven");
        }
        return;
    }

    /* 3. PB5 drives the net */
    uint32_t pa6_bad = 0U;
    uint32_t pull_bad = 0U;
    uint32_t pa6_seen = 0U;
    ok = j1_inputs(GPIO_BM_PULL_NONE, GPIO_BM_PULL_UP, GPIO_BM_PULL_UP, GPIO_BM_PULL_NONE) &&
         pin_cfg(GPIOB, PB_J1_MOSI, GPIO_BM_MODE_OUTPUT, 0U, GPIO_BM_PULL_NONE, 0U);
    for (uint32_t i = 0U; ok && (i < 16U); i++)
    {
        const uint32_t b = (T0C_PATTERN >> i) & 1UL;
        const uint32_t opp = (b != 0U) ? GPIO_BM_PULL_DOWN : GPIO_BM_PULL_UP;
        ok = pin_cfg(GPIOB, PB_J1_MISO, GPIO_BM_MODE_INPUT, 0U, opp, 0U) &&
             pin_cfg(GPIOA, PA_J1_SCK, GPIO_BM_MODE_INPUT, 0U, opp, 0U);
        GPIOB->BSRR = (b != 0U) ? (1UL << PB_J1_MOSI) : (1UL << (PB_J1_MOSI + 16U));
        __DSB();
        delay_ms(1U);
        const uint32_t v = j1_idr();
        const uint32_t pa6 = v & 1UL;
        pa6_seen |= pa6 << i;
        if (pa6 != b)
        {
            pa6_bad |= 1UL << i;
        }
        if ((((v >> 1) & 1UL) == b) || (((v >> 2) & 1UL) == b))
        {
            pull_bad |= 1UL << i;
        }
    }
    const uint32_t moder_pb5 = (GPIOB->MODER >> (PB_J1_MOSI * 2U)) & 3UL;
    const bool released = j1_inputs(GPIO_BM_PULL_NONE, GPIO_BM_PULL_NONE, GPIO_BM_PULL_NONE, GPIO_BM_PULL_NONE);
    const uint32_t moder_after = (GPIOB->MODER >> (PB_J1_MOSI * 2U)) & 3UL;
    verdict(ok && (pa6_bad == 0U), "T0C PB5 wrote 0x%04lX over 16 steps, PA6 read 0x%04lX: steps that differ 0x%04lX",
            (unsigned long)T0C_PATTERN, (unsigned long)pa6_seen, (unsigned long)pa6_bad);
    verdict(ok && (pull_bad == 0U), "T0C PB4 and PA5 (pull opposite to PB5) followed their pull: steps that differ 0x%04lX",
            (unsigned long)pull_bad);
    verdict(released && (moder_pb5 == 1U) && (moder_after == 0U),
            "T0C PB5 MODER %lu (output) during the test, %lu (input) after; J1 pins input without pull",
            (unsigned long)moder_pb5, (unsigned long)moder_after);
    const bool pass = ok && released && (pa6_bad == 0U) && (pull_bad == 0U);
    LOG("T0C summary %s", pass ? "PASS" : "FAIL");
    if (!pass)
    {
        if (!s_bench_abort)
        {
            bench_abort("T0C failed: J1 absent, loose or elsewhere");
        }
        return;
    }
    if (!sync0())          /* the host reads the summary before the next step */
    {
        return;
    }
    step_end();
}

/* SPI raw state for the S13 and S14 cases (the selftest, not a driver: F0 has none).
   Written only with the four SPI4 pins in input, except where a test says.
   Order: SPE = 0 with SSI = 1 (inactive: SSIOP is 0 in every configuration
   of this selftest; CFG2 is protected while SPE = 1), then CFG2, then the
   requested SSI, then every flag cleared, then SPE if asked. So no write
   order makes a master with SSM = 1 see SSI active: that is a mode fault,
   which clears MASTER and, while MODF stays set, keeps SPE from being set
   (RM0399 53:1464-1476, 53:3280). In the first run
   (docs/spi_f0_bench_65e19f7.txt) CR1 = 0 was written before CFG2 = 0, and
   the "SPE=1" case of T0b(e) then read SPE back as 0. */
static void spi_set(SPI_TypeDef *spi, uint32_t cr1, uint32_t cfg2)
{
    spi->IER  = 0U;
    spi->CR1  = SPI_CR1_SSI;
    spi->CFG2 = cfg2;
    spi->CR1  = cr1 & ~SPI_CR1_SPE;
    spi->IFCR = SST_SPI_IFCR_ALL;
    spi->CR1  = cr1;
    __DSB();
}


/* T0b(b)(e)(f) on CPU1 and CPU2 in step T0B (bus = NONE, J1 = CPU2). */
static void step_t0b(void)
{
    SstArea_t *a = SST_AREA;
    if (!step_begin("T0B"))
    {
        return;
    }
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    const uint32_t bad_b = run_claims(k_t0b_refuse, NCASES(k_t0b_refuse));
    const uint32_t bad_f = run_claims(k_t0b_j1_refuse, NCASES(k_t0b_j1_refuse));
    const bool accepted = bench_claim(GPIOA, 1UL << PA_MCO, GPIO_BM_MODE_AF, SST_AF_MCO1);   /* the control */

    /* (e) outside a "bus = MCU" step: SPI4 in a state S13 accepts, the claim
       of PE11..PE14 in AF5 must still be refused here (the step decides). */
    const bool clk_was_on = spi4_clocked();
    RCC->APB2ENR |= RCC_APB2ENR_SPI4EN;      /* CPU1's bank (RM0399 9.5.10) */
    (void)RCC->APB2ENR;
    spi_set(SPI4, SST_SPI_SSI_INACTIVE, SST_SPI_MASTER_SOFT);
    const bool s13_state_ok = spi4_s13_ok();
    const bool e_accepted = bench_claim(GPIOE, PE_SPI4_MASK, GPIO_BM_MODE_AF, SST_AF_SPI4);
    spi_set(SPI4, 0U, 0U);
    if (!clk_was_on)
    {
        RCC->APB2ENR &= ~RCC_APB2ENR_SPI4EN;
        (void)RCC->APB2ENR;
    }
    port_snap(&after);
    const bool untouched = port_snap_eq(&before, &after);
    verdict((bad_b == 0U) && accepted && untouched,
            "T0B(b) CM7 claim: %lu refusals, accepted by mistake 0x%08lX; PA8 AF0 accepted (control) %u; regs untouched %u",
            (unsigned long)NCASES(k_t0b_refuse), (unsigned long)bad_b, (unsigned)accepted, (unsigned)untouched);
    verdict((bad_f == 0U) && untouched,
            "T0B(f) CM7 claim of J1 in CPU2's step: %lu refusals (AF8, AF5, PB5 output), accepted by mistake 0x%02lX",
            (unsigned long)NCASES(k_t0b_j1_refuse), (unsigned long)bad_f);
    verdict(s13_state_ok && !e_accepted,
            "T0B(e) PE11..PE14 AF5 in bus = NONE with SPI4 at an S13 state (master, SPE = 0, AFCNTR = 0: "
            "S13 read-back %u): refused %u", (unsigned)s13_state_ok, (unsigned)!e_accepted);

    cm4_claims(SST_M4_SLOT_T0B, SST_M4_CLAIMS_EXPECT_T0B, s_step, "in T0B (J1 = CM4)");
    if (cm4_send(SST_CMD_SAFE, 0U))
    {
        const SstSafeResult_t *r = &a->m4_safe;
        verdict((r->claim_ok == 1U) && (r->af_moder == 2U) && (r->af_afr == SST_AF_SPI1) && (r->pull_pupdr == 0x6U) &&
                (r->safe_moder == 0U) && (r->safe_pupdr == 0U) && (r->safe_afr == 0U),
                "T0B(f) CM4 bench_safe: PA5 AF5 MODER=%lu AFR=%lu, PA6|PB4 PUPDR=0x%lX; after safe J1 MODER=0x%02lX "
                "PUPDR=0x%02lX AFR=0x%04lX (all 0)", (unsigned long)r->af_moder, (unsigned long)r->af_afr,
                (unsigned long)r->pull_pupdr, (unsigned long)r->safe_moder, (unsigned long)r->safe_pupdr,
                (unsigned long)r->safe_afr);
        verdict((r->spi1_cr1 == SST_SPI_SSI_INACTIVE) && (r->spi1_cfg2 == SST_SPI_MASTER_SOFT) && (r->spi1_cr1_safe == 0U) &&
                (r->spi1_cfg2_safe == 0U) && ((r->spi1_sr_safe & SPI_SR_MODF) == 0U) && (r->spi1_clk_same == 1U),
                "T0B(f) R2-3 CM4 bench_safe releases SPI1: CR1 0x%08lX -> 0x%08lX, CFG2 0x%08lX -> 0x%08lX, SR 0x%08lX "
                "(MODF 0); clock as before %lu", (unsigned long)r->spi1_cr1, (unsigned long)r->spi1_cr1_safe,
                (unsigned long)r->spi1_cfg2, (unsigned long)r->spi1_cfg2_safe, (unsigned long)r->spi1_sr_safe,
                (unsigned long)r->spi1_clk_same);
    }
    if (cm4_send(SST_CMD_SPI1, 0U))
    {
        const SstSpiCaseResult_t *r = &a->m4_spi1;
        verdict((r->cases == SST_SPI_CASES) && (r->accepted == SST_SPI_CASES_EXPECT) && (r->spe_seen == 1U) &&
                (r->regs_changed == 0U) && (r->cr1_after == 0U) && (r->cfg2_after == 0U) && (r->clk_same == 1U),
                "T0B(f) R2-3 CM4, PA6 AF5 against SPI1: SPE=1 (read back %lu), AFCNTR=1, IOSWP=1, slave CFG2!=0, clock "
                "off refused; master at rest, CFG2=0 accepted: accepted 0x%02lX (0x%02lX), GPIO regs changed 0x%05lX, "
                "SPI1 back to CR1=0x%08lX CFG2=0x%08lX, clock as before %lu", (unsigned long)r->spe_seen,
                (unsigned long)r->accepted, (unsigned long)SST_SPI_CASES_EXPECT, (unsigned long)r->regs_changed,
                (unsigned long)r->cr1_after, (unsigned long)r->cfg2_after, (unsigned long)r->clk_same);
    }
    if (!sync0())
    {
        return;
    }
    step_end();
    cm4_claims(SST_M4_SLOT_NONE, SST_M4_CLAIMS_EXPECT_NONE, SST_STEP_NONE, "between steps");
}

/* T0b(c): the MCU drives 0x5A on purpose while the AD2 only listens. The host
   runs its normal released-bus check, which must fail here (as in 03_tim). */
static void step_t0b_s5neg(void)
{
    if (!step_begin("T0B_S5NEG") || !buschk())   /* S5: released first, then driven */
    {
        return;
    }
    delay_ms(1U);
    for (uint32_t p = BUS_SHIFT; p < 16U; p++)
    {
        if (!pin_cfg(GPIOE, p, GPIO_BM_MODE_OUTPUT, 0U, GPIO_BM_PULL_NONE, (0x5AUL >> (p - BUS_SHIFT)) & 1UL))
        {
            return;
        }
    }
    const uint32_t driven = bus_idr();
    (void)sync0();       /* host: released-bus check, pull-up phase   */
    (void)sync0();       /* host: released-bus check, pull-down phase */
    (void)pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
    const uint32_t moder_bus = GPIOE->MODER >> (BUS_SHIFT * 2U);
    (void)buschk();      /* and now it really is released */
    verdict((driven == 0x5AU) && (moder_bus == 0U) && !s_bench_abort,
            "T0B(c) bus driven to 0x%02lX during the host check, then released (MODER bus 0x%04lX)",
            (unsigned long)driven, (unsigned long)moder_bus);
    step_end();
}

/* After the T0b(d) timeout: MCU_ACK stays low (bench_safe() drove it low) and
   the MCU goes on only once the host has raised and dropped HOST_REQ. The host
   reads ACK low while the MCU waits here, so its check does not depend on the
   order in which console lines and DIO reads arrive (audit B1: in
   docs/spi_f0_audit_bench_153d5b3.txt the next step had raised ACK again
   before the host read it). */
static bool sync_ack_low(void)
{
    if (s_bench_abort)
    {
        return false;
    }
    bool ok = wait_req(true, SYNC_TIMEOUT_CYC);
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

/* T0b(d): the host does not answer; the MCU must time out into S2, and wait
   with MCU_ACK low until the host has seen it (sync_ack_low()). */
static void step_t0b_timeout(void)
{
    if (!step_begin("T0B_TIMEOUT"))
    {
        return;
    }
    ack(true);
    const bool answered = wait_req(true, SYNC_TIMEOUT_CYC);
    bench_safe();
    const uint32_t e_moder = GPIOE->MODER & 0xFFFFFFF0UL;         /* PE2..PE15 */
    const uint32_t expect  = 1UL << (PE_ACK * 2U);                /* only MCU_ACK is an output */
    const uint32_t j1 = ((GPIOA->MODER >> (PA_J1_SCK * 2U)) & 0xFUL) | (((GPIOB->MODER >> (PB_J1_MISO * 2U)) & 0xFUL) << 4);
    const bool safe = (e_moder == expect) && (j1 == 0U) &&
                      (((GPIOF->MODER >> (PF_SCOPE1 * 2U)) & 0xFUL) == 0U) &&
                      (((GPIOA->MODER >> (PA_MCO * 2U)) & 3UL) == 0U) &&
                      (((GPIOE->ODR >> PE_ACK) & 1UL) == 0U) && (GPIOE->AFR[1] == 0U);
    verdict(!answered && safe, "T0B(d) no HOST_REQ in 10 s -> bench_safe: GPIOE MODER[31:4]=0x%08lX (0x%08lX), AFRH=0x%08lX, "
            "J1 MODER PA5|PA6|PB4|PB5=0x%02lX (0)", (unsigned long)e_moder, (unsigned long)expect,
            (unsigned long)GPIOE->AFR[1], (unsigned long)j1);
    LOG("T0B timeout handled");
    if (!sync_ack_low())   /* host: MCU_ACK low, then a HOST_REQ pulse */
    {
        return;
    }
    step_end();
}

typedef struct { uint32_t cr1, cfg2, clk; const char *what; } S13Case_t;

/* T0b(e): SPI4 states S13 must refuse (the pins in input meanwhile, so none of
   them reaches a pin; AFCNTR = 1 only with MASTER = 1, RM0399 53:3462). */
static const S13Case_t k_s13_cases[] = {
    { SST_SPI_SSI_INACTIVE | SPI_CR1_SPE, SST_SPI_MASTER_SOFT,                   1U, "SPE=1" },
    { SST_SPI_SSI_INACTIVE,               SST_SPI_MASTER_SOFT | SPI_CFG2_AFCNTR, 1U, "AFCNTR=1" },
    { SST_SPI_SSI_INACTIVE,               SPI_CFG2_SSM | SPI_CFG2_CPOL,       1U, "MASTER=0, CFG2!=0" },
    { SST_SPI_SSI_INACTIVE,               SST_SPI_MASTER_SOFT,                   0U, "SPI4 clock off in CPU1's bank" },
};

/* The S5 condition read by the MCU, without aborting (the T0b(e) control). */
static bool s5_holds(uint32_t idr, bool high)
{
    return idr == (high ? 0xFFU : 0x00U);
}

/* T0b(e): S13. PE11..PE14 go to AF5 only in a "bus = MCU" step with SPI4 read
   back at SPE = 0, AFCNTR = 0, master or reset. The S5 check then runs with the
   pins in AF5 and must pass. Control: with AFCNTR = 1 (SPE = 0) the SPI drives
   SCK at CPOL, so SCK no longer follows the pulls and the same S5 condition
   fails; no contention, the AD2 is in input. AFCNTR is set only around each
   reading, after the claim of the new pull has been accepted with AFCNTR = 0;
   the step ends with SPI4 back at its reset values (the Deinit of a keep_pins
   step, checked by step_end()). */
static void step_t0b_s13(void)
{
    if (!step_begin("T0B_S13") || !buschk())
    {
        return;
    }
    const bool clk_was_on = spi4_clocked();
    RCC->APB2ENR |= RCC_APB2ENR_SPI4EN;      /* CPU1's bank (RM0399 9.5.10) */
    (void)RCC->APB2ENR;
    log_long("T0B(e) SPI4 at the start of the step: CR1=0x%08lX CFG2=0x%08lX SR=0x%08lX (MODF %lu)",
        (unsigned long)SPI4->CR1, (unsigned long)SPI4->CFG2, (unsigned long)SPI4->SR,
        (unsigned long)((SPI4->SR & SPI_SR_MODF) != 0U));

    PortSnap_t g0;
    PortSnap_t g1;
    port_snap(&g0);
    uint32_t bad_state = 0U;
    uint32_t spe_seen = 0U;
    uint32_t spe_sr = 0U;
    for (uint32_t i = 0U; i < NCASES(k_s13_cases); i++)
    {
        const S13Case_t *c = &k_s13_cases[i];
        RCC->APB2ENR |= RCC_APB2ENR_SPI4EN;
        (void)RCC->APB2ENR;
        spi_set(SPI4, c->cr1, c->cfg2);
        if ((c->cr1 & SPI_CR1_SPE) != 0U)
        {
            spe_seen = SPI4->CR1 & SPI_CR1_SPE;   /* the case is only a case if SPE really is 1 */
            spe_sr   = SPI4->SR;
        }
        if (c->clk == 0U)
        {
            RCC->APB2ENR &= ~RCC_APB2ENR_SPI4EN;
            (void)RCC->APB2ENR;
        }
        if (bench_claim(GPIOE, PE_SPI4_MASK, GPIO_BM_MODE_AF, SST_AF_SPI4))
        {
            bad_state |= 1UL << i;
            LOG("T0B(e) case %lu (%s) accepted", (unsigned long)i, c->what);
        }
        RCC->APB2ENR |= RCC_APB2ENR_SPI4EN;
        (void)RCC->APB2ENR;
        spi_set(SPI4, SST_SPI_SSI_INACTIVE, SST_SPI_MASTER_SOFT);
    }
    const uint32_t bad_pins = run_claims(k_s13_pin_refuse, NCASES(k_s13_pin_refuse));
    /* The controls: the states S13 accepts. */
    const bool ok_master = bench_claim(GPIOE, PE_SPI4_MASK, GPIO_BM_MODE_AF, SST_AF_SPI4);
    spi_set(SPI4, 0U, 0U);
    const bool ok_reset = bench_claim(GPIOE, PE_SPI4_MASK, GPIO_BM_MODE_AF, SST_AF_SPI4);
    port_snap(&g1);
    const bool untouched = port_snap_eq(&g0, &g1);
    log_long("T0B(e) SPI4 cases: SPE=1 (read back %lu), AFCNTR=1, MASTER=0 with CPOL, clock off; pin cases: AF6, AF1, PE8, "
        "PE10, PE15, PE9, PE12 output, PE2, PE6, SPI4 with PE10, PA5 AF8; SR in the SPE case 0x%08lX",
        (unsigned long)spe_seen, (unsigned long)spe_sr);
    verdict((bad_state == 0U) && (bad_pins == 0U) && (spe_seen != 0U) && untouched,
            "T0B(e) S13 claim: %lu SPI4 states and %lu pin cases refused, accepted by mistake 0x%lX / 0x%04lX, "
            "GPIO regs untouched %u", (unsigned long)NCASES(k_s13_cases), (unsigned long)NCASES(k_s13_pin_refuse),
            (unsigned long)bad_state, (unsigned long)bad_pins, (unsigned)untouched);
    verdict(ok_master && ok_reset, "T0B(e) controls: CFG2=MASTER|SSM accepted %u, CFG2=0 (reset) accepted %u",
            (unsigned)ok_master, (unsigned)ok_reset);

    /* The pins in AF5 with SPI4 a master at rest: S5 must pass (host and MCU). */
    uint32_t up = 0U;
    uint32_t dn = 0U;
    bool s5_af = false;
    spi_set(SPI4, SST_SPI_SSI_INACTIVE, SST_SPI_MASTER_SOFT);
    if (!s_bench_abort)
    {
        s5_af = buschk_af(PE_SPI4_MASK, &up, &dn);     /* host: S5 check with PE11..PE14 in AF5 */
    }
    const uint32_t moder_af = (GPIOE->MODER >> (PE_NSS * 2U)) & 0xFFUL;
    const uint32_t afr_af   = (GPIOE->AFR[1] >> ((PE_NSS - 8U) * 4U)) & 0xFFFFUL;
    verdict(s5_af && (moder_af == 0xAAU) && (afr_af == 0x5555U) && (up == 0xFFU) && (dn == 0x00U),
            "T0B(e) S5 with PE11..PE14 in AF5 (MODER 0x%02lX, AFR 0x%04lX), SPI4 CFG2=0x%08lX: MCU IDR bus up 0x%02lX down 0x%02lX",
            (unsigned long)moder_af, (unsigned long)afr_af, (unsigned long)SPI4->CFG2, (unsigned long)up, (unsigned long)dn);

    /* Control: AFCNTR = 1 around each reading, CPOL 0 then 1, pull-up then down. */
    uint32_t v[4] = { 0U, 0U, 0U, 0U };
    bool held[4] = { false, false, false, false };
    bool run_ok = s5_af;
    for (uint32_t k = 0U; run_ok && (k < 4U); k++)
    {
        const uint32_t cpol = (k >= 2U) ? SPI_CFG2_CPOL : 0U;
        const bool high = (k & 1U) == 0U;
        spi_set(SPI4, SST_SPI_SSI_INACTIVE, SST_SPI_MASTER_SOFT | cpol);           /* AFCNTR = 0, the final polarity */
        run_ok = bus_pull(high ? GPIO_BM_PULL_UP : GPIO_BM_PULL_DOWN, PE_SPI4_MASK);
        if (!run_ok)
        {
            break;
        }
        SPI4->CFG2 = SST_SPI_MASTER_SOFT | cpol | SPI_CFG2_AFCNTR;         /* SPE = 0: CFG2 writable */
        __DSB();
        delay_ms(1U);
        v[k] = bus_idr();
        held[k] = !s5_holds(v[k], high);
        run_ok = sync0();                                              /* host: s5_released() */
        SPI4->CFG2 = SST_SPI_MASTER_SOFT | cpol;                           /* AFCNTR = 0 again */
        __DSB();
    }
    /* Deinit (plan rule 9, by hand): CFG2 = 0 lets the pins go; S5 passes again in AF5. */
    spi_set(SPI4, 0U, 0U);
    uint32_t up2 = 0U;
    uint32_t dn2 = 0U;
    const bool s5_after = run_ok && buschk_af(PE_SPI4_MASK, &up2, &dn2);
    const uint32_t sck = 1UL << (PE_SCK - BUS_SHIFT);
    const uint32_t mosi = 1UL << (PE_MOSI - BUS_SHIFT);
    const uint32_t others = 0xFFUL & ~(sck | mosi);
    bool ctrl_ok = run_ok;
    for (uint32_t k = 0U; k < 4U; k++)
    {
        const uint32_t cpol_level = (k >= 2U) ? sck : 0U;
        const uint32_t pulled = ((k & 1U) == 0U) ? others : 0U;
        ctrl_ok = ctrl_ok && ((v[k] & sck) == cpol_level) && ((v[k] & others) == pulled);
    }
    ctrl_ok = ctrl_ok && held[0] && held[3];
    log_long("T0B(e) control AFCNTR=1 MCU IDR bus: CPOL=0 up 0x%02lX down 0x%02lX, CPOL=1 up 0x%02lX down 0x%02lX; "
        "MOSI (DIO6) %lu %lu %lu %lu", (unsigned long)v[0], (unsigned long)v[1], (unsigned long)v[2],
        (unsigned long)v[3], (unsigned long)((v[0] >> 6) & 1U), (unsigned long)((v[1] >> 6) & 1U),
        (unsigned long)((v[2] >> 6) & 1U), (unsigned long)((v[3] >> 6) & 1U));
    verdict(ctrl_ok, "T0B(e) control AFCNTR=1, SPE=0: SCK (PE12) at CPOL in all 4 readings, the S5 condition fails "
            "(CPOL=0 pull-up %u, CPOL=1 pull-down %u); NSS, MISO and the 4 other bus lines follow the pull",
            (unsigned)held[0], (unsigned)held[3]);
    verdict(s5_after && (up2 == 0xFFU) && (dn2 == 0x00U),
            "T0B(e) after the Deinit (CFG2=0), PE11..PE14 still in AF5: S5 passes again, MCU IDR bus up 0x%02lX down 0x%02lX",
            (unsigned long)up2, (unsigned long)dn2);

    (void)pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
    spi_set(SPI4, 0U, 0U);
    const uint32_t cr1 = SPI4->CR1;
    const uint32_t cr2 = SPI4->CR2;
    const uint32_t cfg1 = SPI4->CFG1;
    const uint32_t cfg2 = SPI4->CFG2;
    const uint32_t ier = SPI4->IER;
    verdict((cr1 == 0U) && (cr2 == 0U) && (cfg1 == 0x00070007UL) && (cfg2 == 0U) && (ier == 0U),
            "T0B(e) SPI4 at its reset values: CR1=0x%08lX CR2=0x%08lX CFG1=0x%08lX (0x00070007) CFG2=0x%08lX IER=0x%08lX",
            (unsigned long)cr1, (unsigned long)cr2, (unsigned long)cfg1, (unsigned long)cfg2, (unsigned long)ier);
    (void)buschk();
    step_end();
    if (!clk_was_on)
    {
        RCC->APB2ENR &= ~RCC_APB2ENR_SPI4EN;
        (void)RCC->APB2ENR;
    }
    verdict(spi4_clocked() == clk_was_on, "T0B(e) SPI4 clock in CPU1's bank as before the step (%u)", (unsigned)clk_was_on);
}

/* T0b(f), plan R2-3: the SPI states of SST_SPI_CASES, one per condition of
   sst_spi_at_rest() plus the clock, against a claim of PA6 in AF8; the J1 pins
   stay in input, so no state reaches a pin (AFCNTR = 1 only with MASTER = 1,
   RM0399 53:3462). Cases 5 and 6 are the accepted controls. */
typedef struct { uint32_t cr1, cfg2, clk; } SpiCase_t;
static const SpiCase_t k_spi6_cases[SST_SPI_CASES] = {
    { SST_SPI_SSI_INACTIVE | SPI_CR1_SPE, SST_SPI_MASTER_SOFT,                   1U },   /* 0 SPE = 1          */
    { SST_SPI_SSI_INACTIVE,               SST_SPI_MASTER_SOFT | SPI_CFG2_AFCNTR, 1U },   /* 1 AFCNTR = 1       */
    { SST_SPI_SSI_INACTIVE,               SST_SPI_MASTER_SOFT | SPI_CFG2_IOSWP,  1U },   /* 2 IOSWP = 1        */
    { SST_SPI_SSI_INACTIVE,               SPI_CFG2_SSM | SPI_CFG2_CPOL,       1U },   /* 3 slave, CFG2 != 0 */
    { SST_SPI_SSI_INACTIVE,               SST_SPI_MASTER_SOFT,                   0U },   /* 4 clock off        */
    { SST_SPI_SSI_INACTIVE,               SST_SPI_MASTER_SOFT,                   1U },   /* 5 control: master  */
    { 0U,                              0U,                                 1U },   /* 6 control: reset   */
};

static void spi6_cases(SstSpiCaseResult_t *r)
{
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    uint32_t acc = 0U;
    r->spe_seen = 0U;
    for (uint32_t k = 0U; k < SST_SPI_CASES; k++)
    {
        RCC->APB4ENR |= RCC_APB4ENR_SPI6EN;
        (void)RCC->APB4ENR;
        spi_set(SPI6, k_spi6_cases[k].cr1, k_spi6_cases[k].cfg2);
        if ((k_spi6_cases[k].cr1 & SPI_CR1_SPE) != 0U)
        {
            r->spe_seen = ((SPI6->CR1 & SPI_CR1_SPE) != 0U) ? 1U : 0U;
        }
        if (k_spi6_cases[k].clk == 0U)
        {
            RCC->APB4ENR &= ~RCC_APB4ENR_SPI6EN;
            (void)RCC->APB4ENR;
        }
        if (bench_claim(GPIOA, 1UL << PA_J1_MISO, GPIO_BM_MODE_AF, SST_AF_SPI6))
        {
            acc |= 1UL << k;
        }
        RCC->APB4ENR |= RCC_APB4ENR_SPI6EN;
        (void)RCC->APB4ENR;
        spi_set(SPI6, 0U, 0U);
    }
    port_snap(&after);
    r->cases        = SST_SPI_CASES;
    r->accepted     = acc;
    r->regs_changed = port_snap_eq(&before, &after) ? 0U : 1U;
    r->cr1_after    = SPI6->CR1;
    r->cfg2_after   = SPI6->CFG2;
}

/* T0b(f): S14 in a step whose J1 owner is CPU1 (T0B_S14), SPI6 clocked in
   CPU1's bank at its reset values. Claims only: AF8 on the four pins is
   accepted, outputs, CPU2's AF5 and other AFs are refused, nothing is
   written; then the SPI6 states (R2-3); then bench_safe() must release a
   configured SPI6 (rule 9 order: CFG2 = 0 and CR1 = 0, no MODF). CPU2 is
   refused everything in this step. */
static void step_t0b_s14(void)
{
    if (!step_begin("T0B_S14"))
    {
        return;
    }
    const bool clk_was_on = spi6_clocked();
    RCC->APB4ENR |= RCC_APB4ENR_SPI6EN;      /* CPU1's bank (RM0399 9.5.10) */
    (void)RCC->APB4ENR;
    spi_set(SPI6, 0U, 0U);
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    const uint32_t acc = run_claims(k_s14_claims, NCASES(k_s14_claims));
    port_snap(&after);
    verdict((acc == S14_CLAIMS_EXPECT) && port_snap_eq(&before, &after),
            "T0B(f) CM7 claim in T0B_S14 (J1 = CM7, SPI6 at reset): %lu cases, accepted 0x%05lX (0x%05lX: AF8 on PA5, "
            "PA6, PB4, PB5), regs untouched %u", (unsigned long)NCASES(k_s14_claims), (unsigned long)acc,
            (unsigned long)S14_CLAIMS_EXPECT, (unsigned)port_snap_eq(&before, &after));

    SstSpiCaseResult_t r6;
    spi6_cases(&r6);
    verdict((r6.accepted == SST_SPI_CASES_EXPECT) && (r6.spe_seen == 1U) && (r6.regs_changed == 0U) &&
            (r6.cr1_after == 0U) && (r6.cfg2_after == 0U),
            "T0B(f) R2-3 CM7, PA6 AF8 against SPI6: SPE=1 (read back %lu), AFCNTR=1, IOSWP=1, slave CFG2!=0, clock off "
            "refused; master at rest, CFG2=0 accepted: accepted 0x%02lX (0x%02lX), GPIO regs untouched %u",
            (unsigned long)r6.spe_seen, (unsigned long)r6.accepted, (unsigned long)SST_SPI_CASES_EXPECT,
            (unsigned)(r6.regs_changed == 0U));

    spi_set(SPI6, SST_SPI_SSI_INACTIVE, SST_SPI_MASTER_SOFT);
    const uint32_t cr1_before = SPI6->CR1;
    const uint32_t cfg2_before = SPI6->CFG2;
    bench_safe();
    const uint32_t cr1_safe = SPI6->CR1;
    const uint32_t cfg2_safe = SPI6->CFG2;
    const uint32_t sr_safe = SPI6->SR;
    if (!clk_was_on)
    {
        RCC->APB4ENR &= ~RCC_APB4ENR_SPI6EN;
        (void)RCC->APB4ENR;
    }
    verdict((cr1_before == SST_SPI_SSI_INACTIVE) && (cfg2_before == SST_SPI_MASTER_SOFT) && (cr1_safe == 0U) &&
            (cfg2_safe == 0U) && ((sr_safe & SPI_SR_MODF) == 0U) && (spi6_clocked() == clk_was_on),
            "T0B(f) R2-3 CM7 bench_safe releases SPI6: CR1 0x%08lX -> 0x%08lX, CFG2 0x%08lX -> 0x%08lX, SR 0x%08lX (MODF 0); "
            "clock as before (%u)", (unsigned long)cr1_before, (unsigned long)cr1_safe, (unsigned long)cfg2_before,
            (unsigned long)cfg2_safe, (unsigned long)sr_safe, (unsigned)clk_was_on);
    cm4_claims(SST_M4_SLOT_S14, SST_M4_CLAIMS_EXPECT_S14, s_step, "in T0B_S14 (J1 = CM7)");
    if (!sync0())
    {
        return;
    }
    step_end();
}

/* ==========================================================================================
 * Phase F1: spi_bm (plan "Plan de pruebas", T1-T14 and B1). CPU1 owns SPI2-SPI6 and
 * runs SPI1's tests on CPU2 through the SRAM4 block (plan "Selftest", C10).
 * ========================================================================================== */

/* ---- The project's clocks (prj_01 writes no SPIxSEL: T10) -------------------------------- */
static uint32_t s_cpu_hz;          /* CPU1, from SystemCoreClock                              */
static uint32_t s_k123;            /* pll1_q_ck: SPI1-3 (SPI123SEL = 000, RM0399 09:4653)      */
static uint32_t s_k45;             /* rcc_pclk2: SPI4/5 (SPI45SEL = 000, 09:4643)              */
static uint32_t s_k6;              /* rcc_pclk4: SPI6 (SPI6SEL = 000, 09:4802)                 */
static uint32_t s_seed;            /* xorshift32 state of the run's seeds                     */

static void f1_clocks(void)
{
    PLL1_ClocksTypeDef p1;
    HAL_RCCEx_GetPLL1ClockFreq(&p1);
    s_cpu_hz = SystemCoreClock;
    s_k123   = p1.PLL1_Q_Frequency;
    s_k45    = HAL_RCC_GetPCLK2Freq();
    s_k6     = HAL_RCCEx_GetD3PCLK1Freq();
    s_seed   = DWT->CYCCNT | 1U;
    log_long("F1 clocks (HAL, from HSE_VALUE): CPU1 %lu Hz, SPI1-3 kernel %lu Hz, SPI4/5 %lu Hz, SPI6 %lu Hz; "
             "run seed 0x%08lX", (unsigned long)s_cpu_hz, (unsigned long)s_k123, (unsigned long)s_k45,
             (unsigned long)s_k6, (unsigned long)s_seed);
}

/* ---- F2: the tier under test. T4, T5(a), T6, T7, T15 and B1 run the same code through
   spi_bm or spi_ll; the LL configuration is the test's own (sst_ll_of()). ---------------- */
#define TIER_BM       0U
#define TIER_LL       1U
#define TIER_HAL      2U          /* F3: spi_hal, CPU1 only */
static uint32_t s_tier = TIER_BM;

/* F3: one HAL handle per CPU1 instance; s_hal_busy while a HAL IT or DMA transfer is
   in flight, so the test's SPI handlers call the HAL's routine only then. */
static SPI_HandleTypeDef s_h2;
static SPI_HandleTypeDef s_h3;
static SPI_HandleTypeDef s_h4;
static SPI_HandleTypeDef s_h5;
static SPI_HandleTypeDef s_h6;
static volatile bool     s_hal_busy;

static SPI_HandleTypeDef *hal_h(const SPI_TypeDef *spi)
{
    if (spi == SPI2)
    {
        return &s_h2;
    }
    if (spi == SPI3)
    {
        return &s_h3;
    }
    if (spi == SPI4)
    {
        return &s_h4;
    }
    return (spi == SPI5) ? &s_h5 : &s_h6;
}

/* A bound in CPU cycles as HAL milliseconds (at least 1). */
static uint32_t ms_of(uint32_t cycles)
{
    return (cycles / (SystemCoreClock / 1000U)) + 1U;
}

static SpiBm_Status_t x_init(const SpiBm_Config_t *c)
{
    if (s_tier == TIER_HAL)
    {
        SPI_HandleTypeDef *h = hal_h(c->spi);
        h->Instance = c->spi;
        return SPI_HAL_Init(h, c);
    }
    if (s_tier == TIER_LL)
    {
        SpiLl_Config_t l;
        sst_ll_of(c, &l);
        return SPI_LL_Init(&l);
    }
    return SPI_BM_Init(c);
}

static SpiBm_Status_t x_transfer(SPI_TypeDef *spi, const void *tx, void *rx, uint32_t n, uint32_t g, uint32_t m)
{
    if (s_tier == TIER_HAL)
    {
        return SPI_HAL_Transfer(hal_h(spi), tx, rx, n, ms_of(m));    /* no guard in the HAL (decision 5) */
    }
    return (s_tier == TIER_LL) ? SPI_LL_Transfer(spi, tx, rx, n, g, m) : SPI_BM_Transfer(spi, tx, rx, n, g, m);
}

static SpiBm_Status_t x_start_it(SPI_TypeDef *spi, SpiBm_Xfer_t *x)
{
    if (s_tier == TIER_HAL)
    {
        x->status = SPI_BM_PENDING;
        s_hal_busy = true;
        const SpiBm_Status_t st = SPI_HAL_StartTransferIt(hal_h(spi), x->tx, x->rx, x->frames);
        if (st != SPI_BM_OK)
        {
            s_hal_busy = false;
        }
        return st;
    }
    return (s_tier == TIER_LL) ? SPI_LL_TransferStartIt(spi, x) : SPI_BM_TransferStartIt(spi, x);
}

static bool x_irq(SPI_TypeDef *spi, SpiBm_Xfer_t *x)
{
    if (s_tier == TIER_HAL)
    {
        SPI_HandleTypeDef *h = hal_h(spi);
        HAL_SPI_IRQHandler(h);
        if (h->State != HAL_SPI_STATE_READY)
        {
            return false;
        }
        s_hal_busy = false;
        x->status = (h->ErrorCode == HAL_SPI_ERROR_NONE) ? SPI_BM_OK : SPI_BM_ERR_HAL;
        return true;
    }
    return (s_tier == TIER_LL) ? SPI_LL_TransferIrq(spi, x) : SPI_BM_TransferIrq(spi, x);
}

static SpiBm_Status_t x_dma_prepare(SPI_TypeDef *spi, uint32_t n, bool rx)
{
    return (s_tier == TIER_LL) ? SPI_LL_DmaPrepare(spi, n, rx) : SPI_BM_DmaPrepare(spi, n, rx);
}

static SpiBm_Status_t x_dma_start(SPI_TypeDef *spi)
{
    return (s_tier == TIER_LL) ? SPI_LL_DmaStart(spi) : SPI_BM_DmaStart(spi);
}

static SpiBm_Status_t x_wait_end(SPI_TypeDef *spi, uint32_t m)
{
    return (s_tier == TIER_LL) ? SPI_LL_WaitEnd(spi, m) : SPI_BM_WaitEnd(spi, m);
}

static SpiBm_Status_t x_dma_end(SPI_TypeDef *spi, uint32_t g)
{
    return (s_tier == TIER_LL) ? SPI_LL_DmaEnd(spi, g) : SPI_BM_DmaEnd(spi, g);
}

static uint32_t next_seed(void)
{
    uint32_t x = s_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_seed = x;
    return x;
}

static uint32_t kernel_of(const SPI_TypeDef *spi)
{
    if ((spi == SPI1) || (spi == SPI2) || (spi == SPI3))
    {
        return s_k123;
    }
    return (spi == SPI6) ? s_k6 : s_k45;
}

/* One SCK period in CPU cycles (ES0445 2.22.5), through the driver's pure function. */
static uint32_t guard_of(const SPI_TypeDef *spi, uint32_t mbr)
{
    uint32_t g = 0U;
    if ((SPI_BM_CalcGuardCycles(s_cpu_hz, kernel_of(spi), mbr, &g) != SPI_BM_OK) || (g == 0U))
    {
        g = 1U;
    }
    return g;
}

/* A transfer's bound: four times its bus time plus 10 ms. */
static uint32_t budget_of(const SPI_TypeDef *spi, uint32_t mbr, uint32_t bits, uint32_t frames)
{
    const uint64_t sck = ((uint64_t)s_cpu_hz << (mbr + 1U)) / kernel_of(spi);
    const uint64_t c = ((uint64_t)frames * (bits + 2U) * sck * 4U) + ((uint64_t)CYC_PER_MS * 10U);
    return (c > 0xF0000000ULL) ? 0xF0000000UL : (uint32_t)c;
}

static SpiBm_Config_t mkcfg(SPI_TypeDef *spi, uint32_t mbr, uint32_t mode, uint32_t bits, bool lsb,
                            uint32_t dir, uint32_t thr)
{
    const SpiBm_Config_t c = { spi, mbr, mode, bits, lsb, dir, thr, SPI_BM_NSS_SOFT, false, 0U, 0U, false };
    return c;
}

/* Frames a FIFO of the instance holds, and the largest packet (half of it, 53:3394). */
static uint32_t bpf_of(uint32_t bits)
{
    return (bits <= 8U) ? 1U : ((bits <= 16U) ? 2U : ((bits <= 24U) ? 3U : 4U));
}

static uint32_t fifo_frames(const SPI_TypeDef *spi, uint32_t bits)
{
    SpiBm_Caps_t caps = { 0U, 0U, 0U, 0U };
    (void)SPI_BM_GetCaps(spi, &caps);
    return (uint32_t)caps.fifo_bytes / bpf_of(bits);
}

static uint32_t thr_max(const SPI_TypeDef *spi, uint32_t bits)
{
    return fifo_frames(spi, bits) / 2U;
}

/* Buffers. The polling ones in AXI SRAM (CPU only, cacheable: no DMA touches them);
   DMA1/DMA2 ones in SRAM1 (.dma_buf, non-cacheable, MPU R5); BDMA ones in SRAM4
   (.d3_ram, CPU1 only: plan SP3). Every DMA buffer aligned to 32 bytes and a
   multiple of 32 bytes long (instructions 3.5). */
#define F1_MAXF       4096U
#define F1_BIGF       65535U
static uint32_t s_tx[F1_MAXF] __attribute__((section(".axi_ram"), aligned(32)));
static uint32_t s_rx[F1_MAXF] __attribute__((section(".axi_ram"), aligned(32)));
static uint8_t  s_big_tx[F1_BIGF + 1U] __attribute__((section(".axi_ram"), aligned(32)));
static uint8_t  s_big_rx[F1_BIGF + 1U] __attribute__((section(".axi_ram"), aligned(32)));
static uint8_t  s_dma1_buf[256] __attribute__((section(".dma_buf"), aligned(32)));
#define BD_MAXF       8192U
static uint16_t s_bd_tx[BD_MAXF] __attribute__((section(".d3_ram"), aligned(32)));
static uint16_t s_bd_rx[BD_MAXF] __attribute__((section(".d3_ram"), aligned(32)));

/* The registers of an SPI the oracles compare (RM0399 53.11), raw. */
#define NREG          8U
static void spi_regs(const SPI_TypeDef *s, uint32_t *r)
{
    r[0] = s->CR1;
    r[1] = s->CR2;
    r[2] = s->CFG1;
    r[3] = s->CFG2;
    r[4] = s->IER;
    r[5] = s->CRCPOLY;
    r[6] = s->UDRDR;
    r[7] = s->I2SCFGR;
}

/* Reset values typed from RM0399 53.11 (CR1 53:3188, CR2 53:3285, CFG1 53:3309, CFG2
   53:3442, IER 53:3544, CRCPOLY 53:3815, UDRDR 53:3902, I2SCFGR 53:3923). */
static const uint32_t k_reset_regs[NREG] = { 0U, 0U, 0x00070007UL, 0U, 0U, 0x00000107UL, 0U, 0U };

/* Bit k set = register k differs from its reset value (T14's oracle). */
static uint32_t reset_diff(const SPI_TypeDef *s)
{
    uint32_t r[NREG];
    spi_regs(s, r);
    uint32_t d = 0U;
    for (uint32_t k = 0U; k < NREG; k++)
    {
        if (r[k] != k_reset_regs[k])
        {
            d |= 1UL << k;
        }
    }
    return d;
}

/* T14 for one instance: Deinit, then the reset image. The control (`ctrl`) runs the
   same comparison on the instance still initialised, which must differ. */
static void t14_check(const char *tag, SPI_TypeDef *spi, const SpiBm_Config_t *ctrl)
{
    uint32_t dctrl = 0U;
    if (ctrl != NULL)
    {
        (void)SPI_BM_Init(ctrl);
        dctrl = reset_diff(spi);
    }
    const SpiBm_Status_t st = SPI_BM_Deinit(spi);
    const uint32_t d = reset_diff(spi);
    verdict((st == SPI_BM_OK) && (d == 0U) && ((ctrl == NULL) || (dctrl != 0U)),
            "T14 %s Deinit %u, registers that differ from the RM0399 reset image 0x%02lX (0); control, still "
            "initialised: 0x%02lX%s", tag, (unsigned)st, (unsigned long)d, (unsigned long)dctrl,
            (ctrl == NULL) ? " (no control)" : " (!= 0)");
}

/* ---- T10: the kernel clock field, against RCC read directly ---------------------------- */
static void f1_t10(void)
{
    SPI_TypeDef *const inst[6] = { SPI1, SPI2, SPI3, SPI4, SPI5, SPI6 };
    const uint32_t d2 = RCC->D2CCIP1R;
    const uint32_t d3 = RCC->D3CCIPR;
    /* Field positions typed from RM0399 9.7.19 (SPI123SEL 14:12, SPI45SEL 18:16) and
       9.7.21 (SPI6SEL 30:28). */
    const uint32_t raw[6] = { (d2 >> 12) & 7U, (d2 >> 12) & 7U, (d2 >> 12) & 7U, (d2 >> 16) & 7U,
                              (d2 >> 16) & 7U, (d3 >> 28) & 7U };
    uint32_t bad = 0U;
    uint32_t got[6] = { 9U, 9U, 9U, 9U, 9U, 9U };
    for (uint32_t i = 0U; i < 6U; i++)
    {
        if ((SPI_BM_GetKernelSource(inst[i], &got[i]) != SPI_BM_OK) || (got[i] != raw[i]) || (got[i] != 0U))
        {
            bad |= 1UL << i;
        }
    }
    verdict(bad == 0U, "T10 GetKernelSource SPI1..6 = %lu %lu %lu %lu %lu %lu; RCC_D2CCIP1R 0x%08lX RCC_D3CCIPR 0x%08lX "
            "read directly give the same (0 in prj_01): differ 0x%02lX", (unsigned long)got[0], (unsigned long)got[1],
            (unsigned long)got[2], (unsigned long)got[3], (unsigned long)got[4], (unsigned long)got[5],
            (unsigned long)d2, (unsigned long)d3, (unsigned long)bad);
}

/* ---- T3: the pure functions, printed for the host to recompute (C15) ------------------- */
static uint32_t s_b1_calc_max;     /* B1: worst CalcPrescaler, cycles */

static void t3_p(uint32_t *i, uint32_t k, uint32_t x)
{
    uint32_t mbr = 0xFFU;
    uint32_t sck = 0U;
    const uint32_t t0 = DWT->CYCCNT;
    const SpiBm_Status_t st = SPI_BM_CalcPrescaler(k, x, &mbr, &sck);
    const uint32_t dt = DWT->CYCCNT - t0;
    if (dt > s_b1_calc_max)
    {
        s_b1_calc_max = dt;
    }
    LOG("T3P %lu k=%lu x=%lu st=%u mbr=%lu sck=%lu", (unsigned long)*i, (unsigned long)k, (unsigned long)x,
        (unsigned)st, (unsigned long)((st == SPI_BM_OK) ? mbr : 0U), (unsigned long)((st == SPI_BM_OK) ? sck : 0U));
    (*i)++;
}

static void t3_g(uint32_t *i, uint32_t c, uint32_t k, uint32_t m)
{
    uint32_t cyc = 0U;
    const SpiBm_Status_t st = SPI_BM_CalcGuardCycles(c, k, m, &cyc);
    LOG("T3G %lu c=%lu k=%lu m=%lu st=%u cyc=%lu", (unsigned long)*i, (unsigned long)c, (unsigned long)k,
        (unsigned long)m, (unsigned)st, (unsigned long)((st == SPI_BM_OK) ? cyc : 0U));
    (*i)++;
}

static void f1_t3(void)
{
    static const uint32_t ks[] = { 1U, 2U, 3U, 255U, 256U, 257U, 511U, 512U, 513U, 1000000U, 99999989U,
                                   100000000U, 101250000U, 200000000U, 202600000U, 268435456U, 2147483647U,
                                   2147483648U, 4294967295U };
    uint32_t n = 0U;
    for (uint32_t a = 0U; a < (sizeof(ks) / sizeof(ks[0])); a++)
    {
        const uint32_t k = ks[a];
        /* the limits of the rule: max = k/2, k/256 (exact, and one below), their
           neighbours, 1, k itself, k + 1, the bench speeds and a small prime */
        const uint32_t xs[11] = { k / 2U, (k / 2U) - 1U, k / 256U, (k / 256U) - 1U, (k / 256U) + 1U, 1U, k,
                                  (k == 0xFFFFFFFFUL) ? k : (k + 1U), 25000000U, 12500000U, 7U };
        for (uint32_t b = 0U; b < 11U; b++)
        {
            if (xs[b] != 0U)
            {
                t3_p(&n, k, xs[b]);
            }
        }
    }
    t3_p(&n, 0U, 1U);
    t3_p(&n, 1U, 0U);
    t3_p(&n, 0U, 0U);
    const uint32_t np = n;
    static const uint32_t cs[] = { 1U, 64000000U, 200000000U, 400000000U, 405200000U, 4294967295U };
    static const uint32_t kk[] = { 1U, 3U, 100000000U, 200000000U, 4294967295U };
    uint32_t g = 0U;
    for (uint32_t a = 0U; a < (sizeof(cs) / sizeof(cs[0])); a++)
    {
        for (uint32_t b = 0U; b < (sizeof(kk) / sizeof(kk[0])); b++)
        {
            t3_g(&g, cs[a], kk[b], 0U);
            t3_g(&g, cs[a], kk[b], 3U);
            t3_g(&g, cs[a], kk[b], 7U);
        }
    }
    for (uint32_t m = 0U; m <= 8U; m++)
    {
        t3_g(&g, 400000000U, 100000000U, m);       /* the bench's SPI4-6; m = 8 refused */
    }
    t3_g(&g, 0U, 1U, 0U);
    t3_g(&g, 1U, 0U, 0U);
    t3_g(&g, 4294967295U, 1U, 7U);                 /* 2^40 cycles: ERR_RANGE */
    LOG("T3 done: %lu CalcPrescaler vectors, %lu CalcGuardCycles vectors (the host recomputes them)",
        (unsigned long)np, (unsigned long)g);
}

/* ---- T1: refusals, with the registers of the instances and both RCC banks read
   before and after (C6, C12). The expected status of each case is written by hand. ---- */
#define T1_MAX        64U
static uint8_t  s_t1_got[T1_MAX];
static uint8_t  s_t1_exp[T1_MAX];
static uint32_t s_t1_n;

static void t1(SpiBm_Status_t got, SpiBm_Status_t exp)
{
    if (s_t1_n < T1_MAX)
    {
        s_t1_got[s_t1_n] = (uint8_t)got;
        s_t1_exp[s_t1_n] = (uint8_t)exp;
    }
    s_t1_n++;
}

/* T1's photo of an instance: every register but the data registers (reading RXDR
   pops the RxFIFO): the eight of spi_regs() plus SR, TXCRC and RXCRC. */
#define T1_NREG       (NREG + 3U)
#define T1_SNAP       ((4U * T1_NREG) + 6U)
static void t1_snap(uint32_t *r)
{
    SPI_TypeDef *const inst[4] = { SPI2, SPI4, SPI5, SPI6 };
    for (uint32_t i = 0U; i < 4U; i++)
    {
        uint32_t *q = &r[i * T1_NREG];
        spi_regs(inst[i], q);
        q[NREG]      = inst[i]->SR;
        q[NREG + 1U] = inst[i]->TXCRC;
        q[NREG + 2U] = inst[i]->RXCRC;
    }
    uint32_t *b = &r[4U * T1_NREG];
    b[0] = RCC_C1->APB1LENR;
    b[1] = RCC_C1->APB2ENR;
    b[2] = RCC_C1->APB4ENR;
    b[3] = RCC_C2->APB1LENR;
    b[4] = RCC_C2->APB2ENR;
    b[5] = RCC_C2->APB4ENR;
}

static void f1_t1(void)
{
    SPI_TypeDef *const inst[4] = { SPI2, SPI4, SPI5, SPI6 };
    for (uint32_t i = 0U; i < 4U; i++)
    {
        (void)SPI_BM_EnableClock(inst[i]);
        (void)SPI_BM_Deinit(inst[i]);
    }
    uint32_t a[T1_SNAP];
    uint32_t b[T1_SNAP];
    uint32_t same = 0U;
    const SpiBm_Config_t c = mkcfg(SPI2, 7U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    SpiBm_Config_t k;
    uint32_t v = 0U;
    uint32_t w = 0U;
    IRQn_Type irq = SPI2_IRQn;
    SpiBm_Caps_t caps;
    s_t1_n = 0U;

    /* 1. Nothing initialised: parameters. */
    t1_snap(a);
    t1(SPI_BM_CheckConfig(NULL), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Init(NULL), SPI_BM_ERR_PARAM);
    k = c; k.spi = (SPI_TypeDef *)USART3;                         t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.spi = (SPI_TypeDef *)((uint32_t)SPI1 + 4U);          t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.data_bits = 3U;                                       t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.data_bits = 33U;                                      t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.spi = SPI4; k.data_bits = 17U;                        t1(SPI_BM_Init(&k), SPI_BM_ERR_UNSUPPORTED);
    k = c; k.spi = SPI5; k.data_bits = 17U;                        t1(SPI_BM_Init(&k), SPI_BM_ERR_UNSUPPORTED);
    k = c; k.spi = SPI6; k.data_bits = 17U;                        t1(SPI_BM_Init(&k), SPI_BM_ERR_UNSUPPORTED);
    k = c; k.mode = 4U;                                            t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.mbr = 8U;                                             t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.direction = 2U;                                       t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.nss = 3U;                                             t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.nss = SPI_BM_NSS_HW; k.mssi = 16U;                    t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.midi = 16U;                                           t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.mssi = 1U;                                            t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.fifo_threshold = 0U;                                  t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.fifo_threshold = 17U;                                 t1(SPI_BM_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.spi = SPI4; k.data_bits = 16U; k.fifo_threshold = 3U; t1(SPI_BM_Init(&k), SPI_BM_ERR_RANGE);
    k = c; k.data_bits = 32U; k.fifo_threshold = 3U;               t1(SPI_BM_Init(&k), SPI_BM_ERR_RANGE);
    t1(SPI_BM_TransferStartIt(SPI2, NULL), SPI_BM_ERR_PARAM);
    t1(SPI_BM_CalcPrescaler(100U, 1U, NULL, &v), SPI_BM_ERR_PARAM);
    t1(SPI_BM_CalcGuardCycles(1U, 1U, 8U, &v), SPI_BM_ERR_PARAM);
    t1(SPI_BM_GetCaps(NULL, &caps), SPI_BM_ERR_PARAM);
    t1(SPI_BM_GetCaps(SPI2, NULL), SPI_BM_ERR_PARAM);
    t1(SPI_BM_GetIRQn((SPI_TypeDef *)USART3, &irq), SPI_BM_ERR_PARAM);
    t1(SPI_BM_GetDmaRequest(SPI2, true, NULL), SPI_BM_ERR_PARAM);
    t1(SPI_BM_EnableClock((SPI_TypeDef *)USART3), SPI_BM_ERR_PARAM);
    t1(SPI_BM_DisableClock(NULL), SPI_BM_ERR_PARAM);
    t1(SPI_BM_GetKernelSource(SPI2, NULL), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Deinit((SPI_TypeDef *)USART3), SPI_BM_ERR_PARAM);
    t1(SPI_BM_DmaStart(NULL), SPI_BM_ERR_PARAM);
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 1U : 0U;

    /* 2. SPI2 transmit only, SPI5 16-bit: the transfer's own parameters. */
    k = c; k.direction = SPI_BM_DIR_TX_ONLY;
    const SpiBm_Status_t i2 = SPI_BM_Init(&k);
    k = c; k.spi = SPI5; k.data_bits = 16U;
    const SpiBm_Status_t i5 = SPI_BM_Init(&k);
    SpiBm_Xfer_t x = { s_tx, NULL, 1U, 0U, 0U, 0U, SPI_BM_OK };
    t1_snap(a);
    t1(SPI_BM_Transfer(SPI2, s_tx, NULL, 0U, 1U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Transfer(SPI2, s_tx, NULL, 65536U, 1U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Transfer(SPI2, NULL, NULL, 1U, 1U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Transfer(SPI2, s_tx, NULL, 1U, 0U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Transfer(SPI2, s_tx, NULL, 1U, 1U, 0U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_Transfer(SPI2, s_tx, s_rx, 1U, 1U, 1000U), SPI_BM_ERR_PARAM);          /* rx in TX_ONLY */
    t1(SPI_BM_Transfer(SPI5, (const uint8_t *)s_tx + 1U, NULL, 1U, 1U, 1000U), SPI_BM_ERR_PARAM);   /* odd */
    t1(SPI_BM_TransferStartIt(SPI2, &x), SPI_BM_ERR_PARAM);                             /* guard 0 */
    t1(SPI_BM_DmaPrepare(SPI2, 1U, true), SPI_BM_ERR_PARAM);                            /* rx in TX_ONLY */
    t1(SPI_BM_DmaPrepare(SPI2, 0U, false), SPI_BM_ERR_PARAM);
    t1(SPI_BM_DmaPrepare(SPI2, 65536U, false), SPI_BM_ERR_PARAM);
    t1(SPI_BM_DmaEnd(SPI2, 0U), SPI_BM_ERR_PARAM);
    t1(SPI_BM_WaitEnd(SPI2, 0U), SPI_BM_ERR_PARAM);
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 2U : 0U;
    (void)SPI_BM_Deinit(SPI5);

    /* 3. SPE = 1 (set by hand; SPI2 has no pin): what the hardware would ignore. */
    k = c;
    const SpiBm_Status_t i3 = SPI_BM_Init(&k);
    SPI2->CR1 |= SPI_CR1_SPE;
    const uint32_t spe = SPI2->CR1 & SPI_CR1_SPE;
    t1_snap(a);
    t1(SPI_BM_Init(&k), SPI_BM_ERR_BUSY);
    t1(SPI_BM_CheckConfig(&k), SPI_BM_ERR_BUSY);
    t1(SPI_BM_DmaPrepare(SPI2, 1U, false), SPI_BM_ERR_BUSY);
    t1(SPI_BM_DmaStart(SPI2), SPI_BM_ERR_BUSY);
    t1(SPI_BM_Transfer(SPI2, s_tx, NULL, 1U, 1U, 1000U), SPI_BM_ERR_BUSY);
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 4U : 0U;
    SPI2->CR1 &= ~SPI_CR1_SPE;

    /* 4. AFCNTR = 1 in force (keep_pins; SPI2 has no pin): another CPOL or SSIOP. */
    k = c; k.keep_pins = true;
    const SpiBm_Status_t i4 = SPI_BM_Init(&k);
    t1_snap(a);
    k.mode = 2U;                                     t1(SPI_BM_Init(&k), SPI_BM_ERR_BUSY);
    k.mode = 0U; k.nss_active_high = true;           t1(SPI_BM_Init(&k), SPI_BM_ERR_BUSY);
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 8U : 0U;
    (void)SPI_BM_Deinit(SPI2);

    /* The control: a valid call changes the snapshot. */
    t1_snap(a);
    k = c;
    w = SPI_BM_Init(&k);
    t1_snap(b);
    const bool ctrl = memcmp(a, b, sizeof(a)) != 0;
    (void)SPI_BM_Deinit(SPI2);

    uint32_t bad = 0U;
    for (uint32_t i = 0U; (i < s_t1_n) && (i < T1_MAX); i++)
    {
        if (s_t1_got[i] != s_t1_exp[i])
        {
            bad++;
            LOG("T1 CM7 case %lu: status %u, expected %u", (unsigned long)i, (unsigned)s_t1_got[i],
                (unsigned)s_t1_exp[i]);
        }
    }
    verdict((bad == 0U) && (s_t1_n <= T1_MAX) && (same == 0xFU) && (spe != 0U) &&
            (i2 == SPI_BM_OK) && (i5 == SPI_BM_OK) && (i3 == SPI_BM_OK) && (i4 == SPI_BM_OK),
            "T1 CM7 refusals on SPI2, SPI4, SPI5, SPI6: %lu cases, %lu with another status; registers of the four "
            "and RCC_C1/C2 APB1LENR/APB2ENR/APB4ENR unchanged in the 4 groups: 0x%lX (0xF); SPE set by hand %u",
            (unsigned long)s_t1_n, (unsigned long)bad, (unsigned long)same, (unsigned)(spe != 0U));
    verdict(ctrl && (w == SPI_BM_OK), "T1 CM7 control: a valid SPI_BM_Init(SPI2) changes the snapshot (%u)",
            (unsigned)ctrl);
}

/* ---- T2: register images against the RM0399 field table typed in sst_t2_expect() ---- */
typedef struct { uint32_t n, bad, bad_deinit, bad_reset, first_i, first_got2, first_exp2; } T2Count_t;

static void t2_one(const SpiBm_Config_t *c, T2Count_t *t)
{
    uint32_t e1 = 0U;
    uint32_t e2 = 0U;
    uint32_t ecr1 = 0U;
    sst_t2_expect(c, &e1, &e2, &ecr1);
    const SpiBm_Status_t st = SPI_BM_Init(c);
    SPI_TypeDef *spi = c->spi;
    const uint32_t g1 = spi->CFG1;
    const uint32_t g2 = spi->CFG2;
    const uint32_t gcr = spi->CR1;
    const bool rest = (spi->I2SCFGR == 0U) && (spi->CRCPOLY == 0x00000107UL) && (spi->UDRDR == 0U);
    if ((st != SPI_BM_OK) || (g1 != e1) || (g2 != e2) || (gcr != ecr1))
    {
        if (t->bad == 0U)
        {
            t->first_i = t->n;
            t->first_got2 = g2;
            t->first_exp2 = e2;
            LOG("T2 first difference: config %lu st %u CFG1 0x%08lX/0x%08lX CFG2 0x%08lX/0x%08lX CR1 0x%lX/0x%lX",
                (unsigned long)t->n, (unsigned)st, (unsigned long)g1, (unsigned long)e1, (unsigned long)g2,
                (unsigned long)e2, (unsigned long)gcr, (unsigned long)ecr1);
        }
        t->bad++;
    }
    if (!rest)
    {
        t->bad_reset++;
    }
    if (SPI_BM_Deinit(spi) != SPI_BM_OK)
    {
        t->bad_deinit++;
    }
    t->n++;
}

static void t2_inst(SPI_TypeDef *spi, const char *name)
{
    SpiBm_Caps_t caps = { 0U, 0U, 0U, 0U };
    (void)SPI_BM_GetCaps(spi, &caps);
    static const uint32_t bits_list[4] = { 4U, 8U, 16U, 32U };
    const uint32_t nb = (caps.max_data_bits == 32U) ? 4U : 3U;
    T2Count_t t = { 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    (void)SPI_BM_EnableClock(spi);
    (void)SPI_BM_Deinit(spi);
    for (uint32_t mbr = 0U; mbr <= 7U; mbr++)
    {
        for (uint32_t mode = 0U; mode <= 3U; mode++)
        {
            for (uint32_t ib = 0U; ib < nb; ib++)
            {
                const uint32_t bits = bits_list[ib];
                const uint32_t thr[2] = { 1U, thr_max(spi, bits) };
                for (uint32_t it = 0U; it < 2U; it++)
                {
                    for (uint32_t lsb = 0U; lsb < 2U; lsb++)
                    {
                        for (uint32_t dir = 0U; dir < 2U; dir++)
                        {
                            for (uint32_t nss = 0U; nss < 3U; nss++)
                            {
                                for (uint32_t op = 0U; op < 2U; op++)
                                {
                                    for (uint32_t mi = 0U; mi < 4U; mi++)
                                    {
                                        const uint32_t mssi = ((mi & 1U) != 0U) ? 15U : 0U;
                                        const uint32_t midi = ((mi & 2U) != 0U) ? 15U : 0U;
                                        if ((nss == SPI_BM_NSS_SOFT) && (mssi != 0U))
                                        {
                                            continue;
                                        }
                                        for (uint32_t keep = 0U; keep < 2U; keep++)
                                        {
                                            const SpiBm_Config_t c = { spi, mbr, mode, bits, lsb != 0U, dir, thr[it],
                                                                       nss, op != 0U, mssi, midi, keep != 0U };
                                            t2_one(&c, &t);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    /* The control: the first configuration's expected CFG2 with MASTER flipped. */
    uint32_t e1 = 0U;
    uint32_t e2 = 0U;
    uint32_t ecr1 = 0U;
    const SpiBm_Config_t c0 = mkcfg(spi, 0U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    sst_t2_expect(&c0, &e1, &e2, &ecr1);
    (void)SPI_BM_Init(&c0);
    const bool caught = spi->CFG2 != (e2 ^ (1UL << 22));
    (void)SPI_BM_Deinit(spi);
    verdict((t.bad == 0U) && (t.bad_deinit == 0U) && (t.bad_reset == 0U) && caught,
            "T2 %s %lu configurations against the RM0399 field table: CFG1/CFG2/CR1 differ in %lu, Deinit failed %lu, "
            "I2SCFGR/CRCPOLY/UDRDR off reset %lu; control (MASTER flipped in the expected CFG2) caught %u", name,
            (unsigned long)t.n, (unsigned long)t.bad, (unsigned long)t.bad_deinit, (unsigned long)t.bad_reset,
            (unsigned)caught);
}

/* GetCaps against table 435 typed here (53:56 FIFO, 53:68 frame size), and the kernel
   field and DMAMUX of each row (RM0399 09:4643-4653, 09:4802; tables 126, 130). */
static void f1_t2_caps(void)
{
    SPI_TypeDef *const inst[6] = { SPI1, SPI2, SPI3, SPI4, SPI5, SPI6 };
    static const uint8_t hand[6][4] = { { 32U, 16U, 0U, 1U }, { 32U, 16U, 0U, 1U }, { 32U, 16U, 0U, 1U },
                                        { 16U, 8U, 1U, 1U },  { 16U, 8U, 1U, 1U },  { 16U, 8U, 2U, 2U } };
    uint32_t bad = 0U;
    for (uint32_t i = 0U; i < 6U; i++)
    {
        SpiBm_Caps_t c = { 0U, 0U, 0U, 0U };
        if ((SPI_BM_GetCaps(inst[i], &c) != SPI_BM_OK) || (c.max_data_bits != hand[i][0]) ||
            (c.fifo_bytes != hand[i][1]) || (c.kernel_select != hand[i][2]) || (c.dmamux != hand[i][3]))
        {
            bad |= 1UL << i;
        }
    }
    verdict(bad == 0U, "T2 GetCaps of SPI1..6 against table 435 typed by hand (32/16/16 bits, FIFO 16/8 B, "
            "kernel field, DMAMUX): differ 0x%02lX", (unsigned long)bad);
}

/* ---- T7(d): DMAMUX requests against the HAL's own constants; IRQn against CMSIS ----- */
static void f1_t7d(void)
{
    SPI_TypeDef *const inst[6] = { SPI1, SPI2, SPI3, SPI4, SPI5, SPI6 };
    static const uint32_t rx[6] = { DMA_REQUEST_SPI1_RX, DMA_REQUEST_SPI2_RX, DMA_REQUEST_SPI3_RX,
                                    DMA_REQUEST_SPI4_RX, DMA_REQUEST_SPI5_RX, BDMA_REQUEST_SPI6_RX };
    static const uint32_t tx[6] = { DMA_REQUEST_SPI1_TX, DMA_REQUEST_SPI2_TX, DMA_REQUEST_SPI3_TX,
                                    DMA_REQUEST_SPI4_TX, DMA_REQUEST_SPI5_TX, BDMA_REQUEST_SPI6_TX };
    static const IRQn_Type irqs[6] = { SPI1_IRQn, SPI2_IRQn, SPI3_IRQn, SPI4_IRQn, SPI5_IRQn, SPI6_IRQn };
    uint32_t bad = 0U;
    for (uint32_t i = 0U; i < 6U; i++)
    {
        uint32_t r = 0U;
        uint32_t t = 0U;
        IRQn_Type q = NonMaskableInt_IRQn;
        if ((SPI_BM_GetDmaRequest(inst[i], false, &r) != SPI_BM_OK) || (r != rx[i]) ||
            (SPI_BM_GetDmaRequest(inst[i], true, &t) != SPI_BM_OK) || (t != tx[i]) ||
            (SPI_BM_GetIRQn(inst[i], &q) != SPI_BM_OK) || (q != irqs[i]))
        {
            bad |= 1UL << i;
        }
    }
    verdict(bad == 0U, "T7(d) GetDmaRequest RX/TX of SPI1..6 = DMA_REQUEST_SPIx_RX/TX (BDMA_REQUEST_SPI6_RX/TX for "
            "SPI6) of stm32h7xx_hal_dma.h, GetIRQn = SPIx_IRQn: differ 0x%02lX", (unsigned long)bad);
}

/* ---- T9: the engine, without pins: t(2000) - t(1000) in CPU cycles ------------------ */
static void t9_one(SPI_TypeDef *spi, const char *name, uint32_t mbr, uint32_t bits, uint32_t *bad)
{
    const SpiBm_Config_t c = mkcfg(spi, mbr, 0U, bits, false, SPI_BM_DIR_TX_ONLY, 1U);
    uint32_t t[2] = { 0U, 0U };
    SpiBm_Status_t st[2] = { SPI_BM_ERR_PARAM, SPI_BM_ERR_PARAM };
    sst_fill(s_tx, sst_elem_bytes(bits), 2000U, bits, next_seed());
    for (uint32_t k = 0U; k < 2U; k++)
    {
        const uint32_t n = (k + 1U) * 1000U;
        (void)SPI_BM_Init(&c);
        const uint32_t t0 = DWT->CYCCNT;
        st[k] = SPI_BM_Transfer(spi, s_tx, NULL, n, guard_of(spi, mbr), budget_of(spi, mbr, bits, n));
        t[k] = DWT->CYCCNT - t0;
        (void)SPI_BM_Deinit(spi);
    }
    /* Oracle: 1000 frames x DSIZE x 2^(MBR+1) kernel ticks, in CPU cycles. */
    const uint64_t num = 1000ULL * bits * (1ULL << (mbr + 1U)) * s_cpu_hz;
    const double exp_c = (double)num / (double)kernel_of(spi);
    const double ctl_c = exp_c * 2.0;            /* the same count with MBR + 1 */
    const double d = (double)(t[1] - t[0]);
    const double err = (d - exp_c) / exp_c;
    const double cerr = (d - ctl_c) / ctl_c;
    const bool ok = (st[0] == SPI_BM_OK) && (st[1] == SPI_BM_OK) && (err <= 0.01) && (err >= -0.01) &&
                    ((cerr > 0.01) || (cerr < -0.01));
    if (!ok)
    {
        (*bad)++;
    }
    verdict(ok, "T9 %s TX only MBR /%lu %lu bits: t(2000) - t(1000) = %lu cycles, expected %lu (%+ld ppm, +-1 %%); "
            "with MBR + 1 %lu (%+ld ppm, must be out); st %u %u", name, (unsigned long)(2UL << mbr),
            (unsigned long)bits, (unsigned long)(t[1] - t[0]), (unsigned long)exp_c, (long)(err * 1e6),
            (unsigned long)ctl_c, (long)(cerr * 1e6), (unsigned)st[0], (unsigned)st[1]);
}

/* On this board an SPI master whose SCK is not routed to a pin in its AF never
   reaches EOT and receives nothing (F1 development run,
   docs/spi_dev_f1_bench_25426f8-dirty.txt: SPI2, SPI3, SPI5 and the pinless SPI1
   stopped at CTSIZE = 1, and SPI2 sent 31 frames into an unread RxFIFO without OVR;
   the same transfer on SPI4 with its pins in AF5 ended). So every test that needs a
   transfer to end runs on an instance the bench routes: SPI6 and SPI1 on J1, SPI4 on
   the AD2 bus (owner's decision, 2026-10-03). */
static void f1_t9(SPI_TypeDef *spi, const char *name)
{
    uint32_t bad = 0U;
    for (uint32_t m = 0U; m < 2U; m++)
    {
        for (uint32_t b = 0U; b < 2U; b++)
        {
            t9_one(spi, name, (m == 0U) ? 3U : 5U, (b == 0U) ? 8U : 16U, &bad);
        }
    }
    (void)bad;
}

/* ---- T11(a): mode fault on SPI2 (no pin, plan R2-4). No transfer is needed: the
   control and the recovery are read with SPI_BM_CheckTransfer() (reads only), and the
   fault with SPI_BM_Transfer(), which returns before it starts anything. ---------- */
static void f1_t11a(void)
{
    SPI_TypeDef *spi = SPI2;
    const SpiBm_Config_t c = mkcfg(spi, 7U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U);
    const uint32_t g = guard_of(spi, 7U);
    const uint32_t bud = budget_of(spi, 7U, 8U, 4U);
    sst_fill(s_tx, 1U, 4U, 8U, next_seed());
    (void)SPI_BM_EnableClock(spi);
    /* control first: SSI inactive, no fault */
    (void)SPI_BM_Init(&c);
    const SpiBm_Status_t s_ctrl = SPI_BM_CheckTransfer(spi, s_tx, NULL, 4U);
    const uint32_t modf_ctrl = spi->SR & SPI_SR_MODF;
    /* SSI made active by hand (SSIOP = 0, SSM = 1): mode fault, with SPE = 0 already
       (plan revision 2, P1) */
    spi->CR1 &= ~SPI_CR1_SSI;
    __DSB();
    const uint32_t sr_f = spi->SR;
    const SpiBm_Status_t c_f = SPI_BM_CheckTransfer(spi, s_tx, NULL, 4U);
    const uint32_t sr_c = spi->SR;                    /* CheckTransfer writes nothing: MODF stays */
    const SpiBm_Status_t s_f = SPI_BM_Transfer(spi, s_tx, NULL, 4U, g, bud);
    const uint32_t cr1_f = spi->CR1;
    const uint32_t cfg2_f = spi->CFG2;
    const uint32_t sr_after = spi->SR;
    const SpiBm_Status_t d1 = SPI_BM_Deinit(spi);
    const SpiBm_Status_t i1 = SPI_BM_Init(&c);
    const uint32_t cfg2_rec = spi->CFG2;
    const SpiBm_Status_t s_rec = SPI_BM_CheckTransfer(spi, s_tx, NULL, 4U);
    (void)SPI_BM_Deinit(spi);
    verdict((s_ctrl == SPI_BM_OK) && (modf_ctrl == 0U), "T11(a) control: SSI inactive, CheckTransfer %u, MODF %lu (0)",
            (unsigned)s_ctrl, (unsigned long)(modf_ctrl != 0U));
    verdict(((sr_f & SPI_SR_MODF) != 0U) && (c_f == SPI_BM_ERR_MODF) && ((sr_c & SPI_SR_MODF) != 0U) &&
            (s_f == SPI_BM_ERR_MODF) && ((cr1_f & SPI_CR1_SPE) == 0U) && ((cfg2_f & SPI_CFG2_MASTER) == 0U) &&
            ((sr_after & SPI_SR_MODF) == 0U),
            "T11(a) SSI active by hand: SR 0x%08lX (MODF); CheckTransfer %u (ERR_MODF %u), MODF still set %u; "
            "Transfer %u, then SPE %lu MASTER %lu, MODF cleared %u", (unsigned long)sr_f, (unsigned)c_f,
            (unsigned)SPI_BM_ERR_MODF, (unsigned)((sr_c & SPI_SR_MODF) != 0U), (unsigned)s_f,
            (unsigned long)((cr1_f & SPI_CR1_SPE) != 0U), (unsigned long)((cfg2_f & SPI_CFG2_MASTER) != 0U),
            (unsigned)((sr_after & SPI_SR_MODF) == 0U));
    verdict((d1 == SPI_BM_OK) && (i1 == SPI_BM_OK) && ((cfg2_rec & SPI_CFG2_MASTER) != 0U) && (s_rec == SPI_BM_OK),
            "T11(a) Deinit %u + Init %u recover: MASTER %lu, CheckTransfer %u", (unsigned)d1, (unsigned)i1,
            (unsigned long)((cfg2_rec & SPI_CFG2_MASTER) != 0U), (unsigned)s_rec);
    t14_check("SPI2", spi, &c);
}

/* ---- T11(b): OVR by hand on a routed instance (full duplex, 4 FIFOs of frames
   pushed, the RxFIFO never read), and TakeErrors. The caller has the pins in AF. ---- */
static void f1_t11b(SPI_TypeDef *spi, const char *name)
{
    const uint32_t n = 4U * fifo_frames(spi, 8U);
    const SpiBm_Config_t f = mkcfg(spi, 2U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    (void)SPI_BM_Init(&f);
    spi->CR2 = n;
    spi->CR1 |= SPI_CR1_SPE;
    uint32_t pushed = 0U;
    const uint32_t t0 = DWT->CYCCNT;
    spi->CR1 |= SPI_CR1_CSTART;
    while (((spi->SR & SPI_SR_EOT) == 0U) && ((DWT->CYCCNT - t0) < (10U * CYC_PER_MS)))
    {
        if ((pushed < n) && ((spi->SR & SPI_SR_TXP) != 0U))
        {
            *(volatile uint8_t *)((uintptr_t)spi + 0x20U) = (uint8_t)pushed;
            pushed++;
        }
    }
    const uint32_t sr_b = spi->SR;
    const uint32_t taken = SPI_BM_TakeErrors(spi);
    const uint32_t sr_a = spi->SR;
    spi->CR1 &= ~SPI_CR1_SPE;
    (void)SPI_BM_Deinit(spi);
    verdict(((sr_b & SPI_SR_EOT) != 0U) && ((sr_b & SPI_SR_OVR) != 0U) &&
            (taken == (sr_b & (SPI_SR_OVR | SPI_SR_MODF | SPI_SR_TIFRE))) &&
            ((sr_a & (SPI_SR_OVR | SPI_SR_MODF | SPI_SR_TIFRE)) == 0U),
            "T11(b) %s OVR by hand (%lu frames, RxFIFO unread): SR 0x%08lX (EOT, OVR), TakeErrors 0x%03lX, then SR "
            "0x%08lX: taken = the error flags set, all cleared", name, (unsigned long)n, (unsigned long)sr_b,
            (unsigned long)taken, (unsigned long)sr_a);
}

/* ---- T13: RCC banks. SPI2 transmits from DMA1 stream 1 (circular, 64 B) for 65535
   frames at /256 while CPU2 disables its own bank, then CPU1 disables its bank. ---- */
#define T13_STREAM    DMA1_Stream1

static uint32_t ctsize(const SPI_TypeDef *spi)
{
    return (spi->SR >> SPI_SR_CTSIZE_Pos) & 0xFFFFU;
}

static void f1_t13(void)
{
    SPI_TypeDef *spi = SPI2;
    SstArea_t *a = SST_AREA;
    const SpiBm_Config_t c = mkcfg(spi, 7U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U);
    uint32_t req = 0U;
    (void)SPI_BM_GetDmaRequest(spi, true, &req);
    (void)SPI_BM_EnableClock(spi);
    (void)SPI_BM_Init(&c);
    sst_fill(s_dma1_buf, 1U, 64U, 8U, next_seed());
    const SpiBm_Status_t p = SPI_BM_DmaPrepare(spi, 65535U, false);
    const DmaBm_ConfigEx_t d = {
        .base = { .stream = T13_STREAM, .request_id = req, .direction = DMA_BM_DIR_MEM_TO_PERIPH,
                  .periph_data_size = DMA_BM_DATA_SIZE_BYTE, .mem_data_size = DMA_BM_DATA_SIZE_BYTE,
                  .periph_inc = false, .mem_inc = true, .circular = true, .priority = DMA_BM_PRIORITY_HIGH,
                  .enable_it_ht = false, .enable_it_tc = false, .enable_it_te = false, .enable_it_dme = false,
                  .enable_it_fe = false, .enable_trbuff = false, .enable_fifo = false,
                  .fifo_threshold = DMA_BM_FIFO_TH_1_2, .periph_addr = (uint32_t)&spi->TXDR,
                  .mem_addr = (uint32_t)s_dma1_buf, .buffer_length = 64U },
        .mem1_addr = 0U, .mem_burst = DMA_BM_BURST_SINGLE, .periph_burst = DMA_BM_BURST_SINGLE,
        .periph_flow_ctrl = false, .fail_if_busy = true, .stop_spin_limit = 0U };
    const bool dmaok = DMA_BM_InitStreamEx(&d) && DMA_BM_StartEx(T13_STREAM);
    const SpiBm_Status_t s = SPI_BM_DmaStart(spi);
    delay_ms(20U);
    const uint32_t c0 = ctsize(spi);
    uint32_t c1 = 0U;
    uint32_t c2 = 0U;
    uint32_t r1 = 0U;
    uint32_t r2 = 0U;
    bool m1 = false;
    bool m2 = false;
    if (cm4_send(SST_CMD_T13, 1U))
    {
        r1 = a->m4_res[2];
        m1 = (a->m4_res[0] == (uint32_t)SPI_BM_OK);
    }
    delay_ms(30U);
    c1 = ctsize(spi);
    if (cm4_send(SST_CMD_T13, 2U))
    {
        r2 = a->m4_res[2];
        m2 = (a->m4_res[0] == (uint32_t)SPI_BM_OK) && (a->m4_res[1] == (uint32_t)SPI_BM_OK);
    }
    delay_ms(30U);
    c2 = ctsize(spi);
    const uint32_t c1bank_on = RCC_C1->APB1LENR & RCC_APB1LENR_SPI2EN;
    /* CPU1 disables its bank: nobody enables SPI2 now; 30 ms later its count must not
       have moved (read again once the clock is back: an unclocked SPI reads nothing). */
    const SpiBm_Status_t dc = SPI_BM_DisableClock(spi);
    const uint32_t both_off = (RCC_C1->APB1LENR | RCC_C2->APB1LENR) & RCC_APB1LENR_SPI2EN;
    delay_ms(30U);
    (void)SPI_BM_EnableClock(spi);
    const uint32_t c3 = ctsize(spi);
    (void)DMA_BM_StopEx(T13_STREAM, 0U);
    (void)SPI_BM_DmaEnd(spi, guard_of(spi, 7U));
    (void)DMA_BM_DeinitStream(T13_STREAM);
    (void)SPI_BM_Deinit(spi);
    const bool run1 = (c0 > c1) && ((c0 - c1) > 1000U);
    const bool run2 = (c1 > c2) && ((c1 - c2) > 1000U);
    const bool stop = (c2 >= c3) && ((c2 - c3) < 200U);
    verdict((p == SPI_BM_OK) && dmaok && (s == SPI_BM_OK) && m1 && run1 && ((r1 & RCC_APB1LENR_SPI2EN) == 0U),
            "T13 SPI2 sending by DMA1 S1: CTSIZE %lu; CPU2 DisableClock(SPI2) (RCC_C2 APB1LENR SPI2EN %lu): 30 ms later "
            "%lu, still running", (unsigned long)c0, (unsigned long)((r1 & RCC_APB1LENR_SPI2EN) != 0U),
            (unsigned long)c1);
    verdict(m2 && run2 && ((r2 & RCC_APB1LENR_SPI2EN) == 0U) && (c1bank_on != 0U),
            "T13 CPU2 EnableClock + DisableClock(SPI2): RCC_C2 SPI2EN %lu, RCC_C1 SPI2EN %lu; 30 ms later CTSIZE %lu, "
            "still running", (unsigned long)((r2 & RCC_APB1LENR_SPI2EN) != 0U), (unsigned long)(c1bank_on != 0U),
            (unsigned long)c2);
    verdict((dc == SPI_BM_OK) && (both_off == 0U) && stop,
            "T13 control: CPU1 DisableClock(SPI2) %u, both banks off; 30 ms later (clock back) CTSIZE %lu, %lu before: "
            "stopped", (unsigned)dc, (unsigned long)c3, (unsigned long)c2);
}

/* ---- CPU2's no-pin tests (T1, T2, T9 on SPI1), printed by CPU1 ----------------------- */
static void f1_m4_nopin(void)
{
    SstArea_t *a = SST_AREA;
    if (cm4_send(SST_CMD_T1, 0U))
    {
        const volatile uint32_t *r = a->m4_res;
        verdict((r[1] == 0U) && (r[2] == 0xFU) && (r[3] == 1U),
                "T1 CM4 refusals on SPI1: %lu cases, %lu with another status (first %lu: %lu, expected %lu); registers "
                "and RCC banks unchanged in the groups 0x%lX (0xF); control changes them %lu", (unsigned long)r[0],
                (unsigned long)r[1], (unsigned long)r[4], (unsigned long)r[5], (unsigned long)r[6],
                (unsigned long)r[2], (unsigned long)r[3]);
    }
    if (cm4_send(SST_CMD_T2, 0U))
    {
        const volatile uint32_t *r = a->m4_res;
        verdict((r[1] == 0U) && (r[2] == 0U) && (r[3] == 0U) && (r[4] == 1U),
                "T2 CM4 SPI1 %lu configurations against the RM0399 field table: differ %lu, Deinit failed %lu, "
                "I2SCFGR/CRCPOLY/UDRDR off reset %lu; control caught %lu (first difference: config %lu CFG2 0x%08lX/0x%08lX)",
                (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[2], (unsigned long)r[3], (unsigned long)r[4],
                (unsigned long)r[5], (unsigned long)r[6], (unsigned long)r[7]);
    }
}

/* T9 on SPI1 (CPU2), J1 pins in AF5 (see the note above f1_t9). */
static void f1_m4_t9(void)
{
    SstArea_t *a = SST_AREA;
    for (uint32_t m = 0U; m < 2U; m++)
    {
        for (uint32_t b = 0U; b < 2U; b++)
        {
            const uint32_t mbr = (m == 0U) ? 3U : 5U;
            const uint32_t bits = (b == 0U) ? 8U : 16U;
            if (cm4_send4(SST_CMD_T9, mbr, bits, 0U, 0U))
            {
                const uint32_t d = a->m4_res[1] - a->m4_res[0];
                /* CPU2 at 200 MHz, SPI1's kernel at 200 MHz: x1 (plan T9) */
                const double exp_c = 1000.0 * (double)bits * (double)(2UL << mbr) * ((double)a->m4_res[2] / (double)s_k123);
                const double err = ((double)d - exp_c) / exp_c;
                const double cerr = ((double)d - (2.0 * exp_c)) / (2.0 * exp_c);
                verdict((a->m4_res[3] == 0U) && (err <= 0.01) && (err >= -0.01) && ((cerr > 0.01) || (cerr < -0.01)),
                        "T9 SPI1 (CM4 at %lu Hz) TX only MBR /%lu %lu bits: t(2000) - t(1000) = %lu cycles, expected %lu "
                        "(%+ld ppm, +-1 %%); with MBR + 1 %+ld ppm (must be out); status 0x%lX", (unsigned long)a->m4_res[2],
                        (unsigned long)(2UL << mbr), (unsigned long)bits, (unsigned long)d, (unsigned long)exp_c,
                        (long)(err * 1e6), (long)(cerr * 1e6), (unsigned long)a->m4_res[3]);
            }
        }
    }
}

/* ---- B1 (CPU1): cycles of a cold Init and of a 1-frame Transfer; the caller has the
   pins of `spi` in AF (see the note above f1_t9) ---------------------------------- */
static uint32_t s_b1_irq_min = 0xFFFFFFFFUL;
static uint32_t s_b1_irq_max;

static void f1_b1(SPI_TypeDef *spi, const char *name)
{
    const SpiBm_Config_t c = mkcfg(spi, 0U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U);
    (void)SPI_BM_EnableClock(spi);
    (void)SPI_BM_Deinit(spi);
    s_tx[0] = 0x5AU;
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    uint32_t t0 = DWT->CYCCNT;
    const SpiBm_Status_t si = x_init(&c);
    const uint32_t c_init = DWT->CYCCNT - t0;
    t0 = DWT->CYCCNT;
    const SpiBm_Status_t st = x_transfer(spi, s_tx, NULL, 1U, guard_of(spi, 0U), budget_of(spi, 0U, 8U, 1U));
    const uint32_t c_xfer = DWT->CYCCNT - t0;
    __set_PRIMASK(pm);
    (void)SPI_BM_Deinit(spi);
    LOG("B1 CM7 %s Init %lu cycles (st %u), Transfer of 1 frame at /2 %lu cycles (st %u), "
        "CalcPrescaler worst %lu", name, (unsigned long)c_init, (unsigned)si, (unsigned long)c_xfer, (unsigned)st,
        (unsigned long)s_b1_calc_max);
}

/* The F1_NOPIN step: no pin moves, the host waits for the closing handshake. */
static void step_f1_nopin(void)
{
    if (!step_begin("F1_NOPIN"))
    {
        return;
    }
    f1_t10();
    f1_t3();
    f1_t1();
    f1_t2_caps();
    t2_inst(SPI2, "SPI2");
    t2_inst(SPI3, "SPI3");
    t2_inst(SPI4, "SPI4");
    t2_inst(SPI5, "SPI5");
    t2_inst(SPI6, "SPI6");
    f1_t7d();
    f1_t11a();
    f1_t13();
    f1_m4_nopin();
    SPI_TypeDef *const inst[5] = { SPI2, SPI3, SPI4, SPI5, SPI6 };
    const char *const names[5] = { "SPI2", "SPI3", "SPI4", "SPI5", "SPI6" };
    for (uint32_t i = 0U; i < 5U; i++)
    {
        t14_check(names[i], inst[i], NULL);
    }
    if (!sync0())
    {
        return;
    }
    step_end();
}

/* ==========================================================================================
 * F1 on J1: SPI6 (CPU1, AF8) and SPI1 (CPU2, AF5), the wire PB5 -> PA6 as the oracle
 * ========================================================================================== */

static const char k_ord[2] = { 'M', 'L' };

/* The four J1 pins for SPI6 (S14: SPI6 read back at rest before each AF). SCK pulled
   to CPOL (plan "Archivos": pull of SCK = CPOL), MOSI and MISO without pull; `miso`
   0 = PA6 (the loop), 1/2 = PB4 with pull-up/down and PA6 in input (plan D2). The
   one MISO pin in AF is always switched off before the other is switched on. */
static bool spi6_pins(uint32_t cpol, uint32_t miso, uint32_t speed)
{
    s_af_speed = speed;
    bool ok = pin_cfg(GPIOA, PA_J1_SCK, GPIO_BM_MODE_AF, SST_AF_SPI6, (cpol != 0U) ? GPIO_BM_PULL_UP : GPIO_BM_PULL_DOWN, 0U) &&
              pin_cfg(GPIOB, PB_J1_MOSI, GPIO_BM_MODE_AF, SST_AF_SPI6, GPIO_BM_PULL_NONE, 0U);
    if (ok && (miso == 0U))
    {
        ok = pin_cfg(GPIOB, PB_J1_MISO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U) &&
             pin_cfg(GPIOA, PA_J1_MISO, GPIO_BM_MODE_AF, SST_AF_SPI6, GPIO_BM_PULL_NONE, 0U);
    }
    else if (ok)
    {
        ok = pin_cfg(GPIOA, PA_J1_MISO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U) &&
             pin_cfg(GPIOB, PB_J1_MISO, GPIO_BM_MODE_AF, SST_AF_SPI6, (miso == 1U) ? GPIO_BM_PULL_UP : GPIO_BM_PULL_DOWN, 0U);
    }
    s_af_speed = GPIO_BM_SPEED_LOW;
    return ok;
}

static bool j1_release(void)
{
    return j1_inputs(GPIO_BM_PULL_NONE, GPIO_BM_PULL_NONE, GPIO_BM_PULL_NONE, GPIO_BM_PULL_NONE);
}

/* One polling transfer on a CPU1 instance: pattern, Init, Transfer, Deinit (T14 per
   transfer), and what the oracles need. The RX buffer starts with a sentinel. */
static void xfer_local(const SpiBm_Config_t *c, uint32_t n, uint32_t seed, void *tx, void *rx, SstXferRes_t *r)
{
    SPI_TypeDef *spi = c->spi;
    const uint32_t bits = c->data_bits;
    const uint32_t es = sst_elem_bytes(bits);
    sst_fill(tx, es, n, bits, seed);
    sst_fill_const(rx, es, n, 0xA5A5A5A5UL);
    r->init_st = x_init(c);
    const uint32_t t0 = DWT->CYCCNT;
    r->status = x_transfer(spi, tx, rx, n, guard_of(spi, c->mbr), budget_of(spi, c->mbr, bits, n));
    r->cycles = DWT->CYCCNT - t0;
    r->mism = sst_mismatch(tx, rx, es, n, bits);
    r->crc = sst_crc32_frames(rx, es, n, bits);
    r->cov = sst_coverage(tx, es, n, bits);
    r->irqs = 0U;
    r->sentinel_left = 0U;
    r->deinit_st = SPI_BM_Deinit(spi);
}

/* One transfer on SPI1, run by CPU2 (SST_CMD_XFER). */
static bool xfer_m4(uint32_t xcfg, uint32_t n, uint32_t seed, uint32_t xm, SstXferRes_t *r)
{
    if (!cm4_send4(SST_CMD_XFER, xcfg | ((s_tier == TIER_LL) ? SST_XCFG_LL : 0U), seed, n, xm))
    {
        return false;
    }
    const volatile uint32_t *s = SST_AREA->m4_res;
    r->status = s[0];
    r->mism = s[1];
    r->crc = s[2];
    r->cov = s[3];
    r->cycles = s[4];
    r->irqs = s[5];
    r->init_st = s[6];
    r->deinit_st = s[7];
    r->sentinel_left = s[8];
    return true;
}

static bool xfer_ok(const SstXferRes_t *r)
{
    return (r->status == (uint32_t)SPI_BM_OK) && (r->mism == 0U) && (r->init_st == (uint32_t)SPI_BM_OK) &&
           (r->deinit_st == (uint32_t)SPI_BM_OK);
}

static void t4_line(const char *name, uint32_t mode, uint32_t bits, uint32_t lsb, uint32_t thr, uint32_t n,
                    uint32_t seed, const SstXferRes_t *r)
{
    LOG("T4 %s m%lu b%lu %c t%lu n%lu s=%08lX st=%lu mis=%lu crc=%08lX cov=%08lX cyc=%lu", name, (unsigned long)mode,
        (unsigned long)bits, k_ord[lsb], (unsigned long)thr, (unsigned long)n, (unsigned long)seed,
        (unsigned long)r->status, (unsigned long)r->mism, (unsigned long)r->crc, (unsigned long)r->cov,
        (unsigned long)r->cycles);
}

/* T4: the matrix of the plan on one instance. `m4` = SPI1 through CPU2. */
static void t4_matrix(SPI_TypeDef *spi, const char *name, bool m4, const uint32_t *bl, uint32_t nb)
{
    uint32_t mbr = 0U;
    uint32_t sck = 0U;
    (void)SPI_BM_CalcPrescaler(kernel_of(spi), 12500000U, &mbr, &sck);
    uint32_t count = 0U;
    uint32_t bad = 0U;
    uint32_t worst = 0U;
    for (uint32_t mode = 0U; mode <= 3U; mode++)
    {
        if (m4)
        {
            if (!cm4_send4(SST_CMD_J1PINS, SST_J1P_LOOP, mode >> 1, GPIO_BM_SPEED_HIGH, 0U) || (SST_AREA->m4_res[0] != 1U))
            {
                bench_abort("CPU2 J1 pins");
                return;
            }
        }
        else if (!spi6_pins(mode >> 1, 0U, GPIO_BM_SPEED_HIGH))
        {
            return;
        }
        for (uint32_t ib = 0U; ib < nb; ib++)
        {
            const uint32_t bits = bl[ib];
            const uint32_t f = fifo_frames(spi, bits);
            const uint32_t frames[8] = { 1U, 2U, 3U, f - 1U, f, f + 1U, 255U, 4096U };
            const uint32_t thr[2] = { 1U, thr_max(spi, bits) };
            for (uint32_t lsb = 0U; lsb < 2U; lsb++)
            {
                for (uint32_t it = 0U; it < 2U; it++)
                {
                    for (uint32_t k = 0U; k < 8U; k++)
                    {
                        const uint32_t seed = next_seed();
                        SstXferRes_t r;
                        if (m4)
                        {
                            if (!xfer_m4(SST_XCFG(mode, bits, lsb, thr[it], mbr, 0U), frames[k], seed, SST_XM_POLL, &r))
                            {
                                bench_abort("CPU2 transfer");
                                return;
                            }
                        }
                        else
                        {
                            const SpiBm_Config_t c = mkcfg(spi, mbr, mode, bits, lsb != 0U, SPI_BM_DIR_FULL_DUPLEX, thr[it]);
                            xfer_local(&c, frames[k], seed, s_tx, s_rx, &r);
                        }
                        t4_line(name, mode, bits, lsb, thr[it], frames[k], seed, &r);
                        count++;
                        if (!xfer_ok(&r))
                        {
                            bad++;
                        }
                        if (r.cycles > worst)
                        {
                            worst = r.cycles;
                        }
                    }
                }
            }
        }
    }
    verdict((bad == 0U) && (count > 0U), "T4 %s loop through J1 at /%lu (%lu Hz): %lu transfers, %lu with a status, a "
            "mismatch or a failed Init/Deinit; longest %lu cycles (every one within its max_cycles)", name,
            (unsigned long)(2UL << mbr), (unsigned long)sck, (unsigned long)count, (unsigned long)bad,
            (unsigned long)worst);
}

/* T4's control (plan D2): MISO moved to PB4, pulled up then down: RX all ones / all
   zeros. The host computes the mismatches from the seed. */
static void t4_control(SPI_TypeDef *spi, const char *name, bool m4, const uint32_t *bl, uint32_t nb)
{
    uint32_t mbr = 0U;
    uint32_t sck = 0U;
    (void)SPI_BM_CalcPrescaler(kernel_of(spi), 12500000U, &mbr, &sck);
    for (uint32_t p = 0U; p < 2U; p++)
    {
        if (m4)
        {
            if (!cm4_send4(SST_CMD_J1PINS, (p == 0U) ? SST_J1P_PB4_UP : SST_J1P_PB4_DOWN, 0U, GPIO_BM_SPEED_HIGH, 0U) ||
                (SST_AREA->m4_res[0] != 1U))
            {
                bench_abort("CPU2 J1 pins");
                return;
            }
        }
        else if (!spi6_pins(0U, p + 1U, GPIO_BM_SPEED_HIGH))
        {
            return;
        }
        for (uint32_t ib = 0U; ib < nb; ib++)
        {
            const uint32_t bits = bl[ib];
            const uint32_t seed = next_seed();
            SstXferRes_t r;
            if (m4)
            {
                (void)xfer_m4(SST_XCFG(0U, bits, 0U, 1U, mbr, 0U), 255U, seed, SST_XM_POLL, &r);
            }
            else
            {
                const SpiBm_Config_t c = mkcfg(spi, mbr, 0U, bits, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
                xfer_local(&c, 255U, seed, s_tx, s_rx, &r);
            }
            LOG("T4C %s b%lu %s n255 s=%08lX st=%lu mis=%lu crc=%08lX", name, (unsigned long)bits,
                (p == 0U) ? "up" : "down", (unsigned long)seed, (unsigned long)r.status, (unsigned long)r.mism,
                (unsigned long)r.crc);
        }
    }
}

/* ---- T6: transfers by interrupt (SPI6 here; SPI1's ISR is CPU2's) ------------------- */
static SpiBm_Xfer_t      s_it;
static volatile bool     s_it_active;
static volatile uint32_t s_it_irqs;
static volatile uint32_t s_it_spurious;

/* The project's vector for the test (plan C14: not the driver's), priority 6. A call
   with no transfer running is the T6(c) control's spurious IRQ: counted, IER cleared. */
void SPI6_IRQHandler(void)
{
    s_it_irqs++;
    if (s_it_active || s_hal_busy)
    {
        const uint32_t t0 = DWT->CYCCNT;
        const bool done = x_irq(SPI6, &s_it);
        const uint32_t dt = DWT->CYCCNT - t0;
        if (dt < s_b1_irq_min)
        {
            s_b1_irq_min = dt;
        }
        if (dt > s_b1_irq_max)
        {
            s_b1_irq_max = dt;
        }
        if (done)
        {
            s_it_active = false;
        }
    }
    else
    {
        s_it_spurious++;
        SPI6->IER = 0U;
    }
}

static void t6_spi6(const char *name)
{
    IRQn_Type irq = SPI6_IRQn;
    (void)SPI_BM_GetIRQn(SPI6, &irq);
    HAL_NVIC_SetPriority(irq, 6U, 0U);
    NVIC_ClearPendingIRQ(irq);
    HAL_NVIC_EnableIRQ(irq);
    const uint32_t mbr = 4U;                      /* /32: 3.125 MHz */
    uint32_t bad = 0U;
    uint32_t after_total = 0U;
    uint32_t post_bad = 0U;
    for (uint32_t b = 0U; b < 2U; b++)
    {
        const uint32_t bits = (b == 0U) ? 8U : 16U;
        const uint32_t es = sst_elem_bytes(bits);
        const uint32_t frames[3] = { 1U, fifo_frames(SPI6, bits) + 1U, 1024U };
        for (uint32_t k = 0U; k < 3U; k++)
        {
            const uint32_t n = frames[k];
            const uint32_t seed = next_seed();
            const SpiBm_Config_t c = mkcfg(SPI6, mbr, 0U, bits, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
            sst_fill(s_tx, es, n, bits, seed);
            sst_fill_const(s_rx, es, n, 0xA5A5A5A5UL);
            const SpiBm_Status_t si = x_init(&c);
            s_it.tx = s_tx;
            s_it.rx = s_rx;
            s_it.frames = n;
            s_it.guard_cycles = guard_of(SPI6, mbr);
            s_it_irqs = 0U;
            s_it_spurious = 0U;
            s_it_active = true;
            const SpiBm_Status_t ss = x_start_it(SPI6, &s_it);
            if (ss != SPI_BM_OK)
            {
                s_it_active = false;
            }
            const uint32_t t0 = DWT->CYCCNT;
            const uint32_t bud = budget_of(SPI6, mbr, bits, n);
            while (s_it_active && ((DWT->CYCCNT - t0) < bud))
            {
            }
            const bool done = !s_it_active;
            if ((s_tier == TIER_HAL) && (ss == SPI_BM_OK))
            {
                s_it.status = SPI_HAL_Wait(&s_h6, 100U);     /* already READY: the handle's verdict */
                LOG("T6H %s b%lu n%lu ier=%08lX cr1=%08lX spurious=%lu herr=%08lX", name, (unsigned long)bits,
                    (unsigned long)n, (unsigned long)SPI6->IER, (unsigned long)SPI6->CR1,
                    (unsigned long)s_it_spurious, (unsigned long)s_h6.ErrorCode);
            }
            s_it_active = false;
            const uint32_t irqs = s_it_irqs;
            /* (c) ES0445 2.22.3: 10 ms after the end, SPE = 0, no IRQ of SPI6 */
            delay_ms(10U);
            const uint32_t after = s_it_irqs - irqs;
            const uint32_t spe = SPI6->CR1 & SPI_CR1_SPE;
            after_total += after;
            /* Every IRQ after the close lands in the handler's spurious branch. The HAL's
               IT start fills the FIFO before it sets DXPIE (stm32h7xx_hal_spi.c l. 1926-1963):
               when every frame fits, TXTF came first and DXPIE stays set (RM0399 53.11.5),
               and SPI_CloseTransfer clears SPE before IER (l. 3945-3948, ES0445 2.22.3): one
               IRQ after the close, a HAL limitation (owner's decision, 2026-10-04). */
            const uint32_t post = s_it_spurious;
            const uint32_t post_exp = ((s_tier == TIER_HAL) && (n <= fifo_frames(SPI6, bits))) ? 1U : 0U;
            if (post != post_exp)
            {
                post_bad++;
            }
            const uint32_t mism = sst_mismatch(s_tx, s_rx, es, n, bits);
            const uint32_t crc = sst_crc32_frames(s_rx, es, n, bits);
            const SpiBm_Status_t sd = SPI_BM_Deinit(SPI6);
            LOG("T6 %s b%lu n%lu s=%08lX st=%lu mis=%lu crc=%08lX irq=%lu after=%lu post=%lu exp=%lu", name,
                (unsigned long)bits, (unsigned long)n, (unsigned long)seed, (unsigned long)s_it.status,
                (unsigned long)mism, (unsigned long)crc, (unsigned long)irqs, (unsigned long)after,
                (unsigned long)post, (unsigned long)post_exp);
            if ((si != SPI_BM_OK) || (ss != SPI_BM_OK) || !done || (s_it.status != SPI_BM_OK) || (mism != 0U) ||
                (after != 0U) || (spe != 0U) || (sd != SPI_BM_OK))
            {
                bad++;
            }
        }
    }
    /* The control: TXPIE = 1 by hand with SPE = 0 (TXP is 1 at reset, 53:3597): the IRQ
       fires although no transfer runs. */
    const SpiBm_Config_t c = mkcfg(SPI6, mbr, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    (void)SPI_BM_Init(&c);
    s_it_spurious = 0U;
    SPI6->IER = SPI_IER_TXPIE;
    delay_ms(1U);
    const uint32_t spurious = s_it_spurious;
    (void)SPI_BM_Deinit(SPI6);
    HAL_NVIC_DisableIRQ(irq);
    NVIC_ClearPendingIRQ(irq);
    verdict((bad == 0U) && (post_bad == 0U), "T6 %s by interrupt (8/16 bits, 1, FIFO + 1, 1024 frames): %lu failed; "
            "(c) IRQs in the 10 ms after each end, SPE = 0: %lu in total (0); IRQs after the close other than "
            "expected (1 with the HAL when every frame fits the FIFO, ES0445 2.22.3 in the HAL; else 0): %lu",
            name, (unsigned long)bad, (unsigned long)after_total, (unsigned long)post_bad);
    verdict(spurious >= 1U, "T6(c) %s control: TXPIE = 1 by hand with SPE = 0: %lu spurious IRQ (>= 1)", name,
            (unsigned long)spurious);
}

/* ---- T7(b): SPI6 with BDMA (dma_hal, plan D7), buffers in .d3_ram ----------------- */
static DMA_HandleTypeDef s_hbd_rx;
static DMA_HandleTypeDef s_hbd_tx;

static bool bd_init(DMA_HandleTypeDef *h, BDMA_Channel_TypeDef *ch, uint32_t req, uint32_t dir, uint32_t es)
{
    const DmaHal_Config_t c = {
        .instance = ch, .request = req, .direction = dir, .periph_inc = DMA_PINC_DISABLE, .mem_inc = DMA_MINC_ENABLE,
        .periph_align = (es == 1U) ? DMA_PDATAALIGN_BYTE : DMA_PDATAALIGN_HALFWORD,
        .mem_align = (es == 1U) ? DMA_MDATAALIGN_BYTE : DMA_MDATAALIGN_HALFWORD, .mode = DMA_NORMAL,
        .priority = DMA_PRIORITY_HIGH, .fifo_mode = DMA_FIFOMODE_DISABLE, .fifo_threshold = DMA_FIFO_THRESHOLD_FULL,
        .mem_burst = DMA_MBURST_SINGLE, .periph_burst = DMA_PBURST_SINGLE, .enable_clock = true, .irq_priority = -1 };
    return DMA_HAL_Init(h, &c) == HAL_OK;
}

/* One SPI6 transfer by BDMA, in the order of plan rule 8 and 53:1262-1274. Returns
   WaitEnd's status; *sentinel = RX words still holding the sentinel. */
static SpiBm_Status_t t7b_one(uint32_t bits, uint32_t n, uint32_t seed, bool rxdma, uint32_t *mism, uint32_t *crc,
                              uint32_t *sentinel, bool *steps_ok)
{
    const uint32_t es = sst_elem_bytes(bits);
    const uint32_t mbr = 2U;                      /* /8: 12.5 MHz */
    const SpiBm_Config_t c = mkcfg(SPI6, mbr, 0U, bits, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    sst_fill(s_bd_tx, es, n, bits, seed);
    sst_fill_const(s_bd_rx, es, n, 0xA5A5A5A5UL);
    const SpiBm_Status_t si = x_init(&c);
    const SpiBm_Status_t sp = x_dma_prepare(SPI6, n, rxdma);
    const bool d_ok = bd_init(&s_hbd_rx, BDMA_Channel0, BDMA_REQUEST_SPI6_RX, DMA_PERIPH_TO_MEMORY, es) &&
                      bd_init(&s_hbd_tx, BDMA_Channel1, BDMA_REQUEST_SPI6_TX, DMA_MEMORY_TO_PERIPH, es);
    bool s_ok = d_ok;
    if (rxdma && d_ok)
    {
        s_ok = HAL_DMA_Start(&s_hbd_rx, (uint32_t)&SPI6->RXDR, (uint32_t)s_bd_rx, n) == HAL_OK;
    }
    if (s_ok)
    {
        s_ok = HAL_DMA_Start(&s_hbd_tx, (uint32_t)s_bd_tx, (uint32_t)&SPI6->TXDR, n) == HAL_OK;
    }
    const SpiBm_Status_t ss = x_dma_start(SPI6);
    const SpiBm_Status_t sw = x_wait_end(SPI6, budget_of(SPI6, mbr, bits, n));
    if (rxdma)
    {
        (void)HAL_DMA_PollForTransfer(&s_hbd_rx, HAL_DMA_FULL_TRANSFER, 10U);
    }
    (void)HAL_DMA_Abort(&s_hbd_tx);                   /* 1. the caller's streams */
    (void)HAL_DMA_Abort(&s_hbd_rx);
    const SpiBm_Status_t se = x_dma_end(SPI6, guard_of(SPI6, mbr));   /* 2. SPI, 3. DMA bits */
    (void)DMA_HAL_DeInit(&s_hbd_tx);
    (void)DMA_HAL_DeInit(&s_hbd_rx);
    const SpiBm_Status_t sd = SPI_BM_Deinit(SPI6);
    *mism = sst_mismatch(s_bd_tx, s_bd_rx, es, n, bits);
    *crc = sst_crc32_frames(s_bd_rx, es, n, bits);
    uint32_t left = 0U;
    for (uint32_t i = 0U; i < n; i++)
    {
        if (sst_get(s_bd_rx, es, i) == (0xA5A5A5A5UL & sst_mask(es * 8U)))
        {
            left++;
        }
    }
    *sentinel = left;
    *steps_ok = (si == SPI_BM_OK) && (sp == SPI_BM_OK) && s_ok && (ss == SPI_BM_OK) && (se == SPI_BM_OK) &&
                (sd == SPI_BM_OK);
    return sw;
}

static void t7b_spi6(const char *name)
{
    uint32_t bad = 0U;
    for (uint32_t b = 0U; b < 2U; b++)
    {
        const uint32_t bits = (b == 0U) ? 8U : 16U;
        const uint32_t frames[4] = { 1U, 9U, 255U, BD_MAXF };
        for (uint32_t k = 0U; k < 4U; k++)
        {
            const uint32_t seed = next_seed();
            uint32_t mism = 0U;
            uint32_t crc = 0U;
            uint32_t left = 0U;
            bool steps = false;
            const SpiBm_Status_t st = t7b_one(bits, frames[k], seed, true, &mism, &crc, &left, &steps);
            LOG("T7B %s b%lu n%lu s=%08lX st=%u mis=%lu crc=%08lX", name, (unsigned long)bits, (unsigned long)frames[k],
                (unsigned long)seed, (unsigned)st, (unsigned long)mism, (unsigned long)crc);
            if ((st != SPI_BM_OK) || (mism != 0U) || !steps)
            {
                bad++;
            }
        }
    }
    uint32_t mism = 0U;
    uint32_t crc = 0U;
    uint32_t left = 0U;
    bool steps = false;
    const SpiBm_Status_t st = t7b_one(8U, 255U, next_seed(), false, &mism, &crc, &left, &steps);
    verdict(bad == 0U, "T7(b) %s by BDMA (channels 0/1, DMAMUX2 11/12, buffers in .d3_ram), 8/16 bits, 1..8192 "
            "frames: %lu failed", name, (unsigned long)bad);
    verdict((st == SPI_BM_ERR_OVERRUN) && (left == 255U) && steps,
            "T7(b) %s control RXDMAEN = 0, 255 frames: WaitEnd %u (ERR_OVERRUN %u), RX words with the sentinel %lu (255)",
            name, (unsigned)st, (unsigned)SPI_BM_ERR_OVERRUN, (unsigned long)left);
}

/* ---- T8: speed sweep on J1 (pins at VERY_HIGH) ---------------------------------------- */
static void t8_sweep(SPI_TypeDef *spi, const char *name, bool m4, uint32_t wide)
{
    uint32_t pass_mask = 0U;     /* bit mbr = both widths passed */
    uint32_t bad_req = 0U;
    if (m4)
    {
        (void)cm4_send4(SST_CMD_J1PINS, SST_J1P_LOOP, 0U, GPIO_BM_SPEED_VERY_HIGH, 0U);
    }
    else
    {
        (void)spi6_pins(0U, 0U, GPIO_BM_SPEED_VERY_HIGH);
    }
    for (uint32_t i = 0U; i < 8U; i++)
    {
        const uint32_t mbr = 7U - i;
        const uint32_t f = kernel_of(spi) >> (mbr + 1U);
        bool both = true;
        for (uint32_t b = 0U; b < 2U; b++)
        {
            const uint32_t bits = (b == 0U) ? 8U : wide;
            const uint32_t seed = next_seed();
            SstXferRes_t r;
            if (m4)
            {
                (void)xfer_m4(SST_XCFG(0U, bits, 0U, 1U, mbr, 0U), 1024U, seed, SST_XM_POLL, &r);
            }
            else
            {
                const SpiBm_Config_t c = mkcfg(spi, mbr, 0U, bits, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
                xfer_local(&c, 1024U, seed, s_tx, s_rx, &r);
            }
            const bool ok = xfer_ok(&r);
            both = both && ok;
            LOG("T8 %s /%lu %lu Hz b%lu s=%08lX st=%lu mis=%lu crc=%08lX %s", name, (unsigned long)(2UL << mbr),
                (unsigned long)f, (unsigned long)bits, (unsigned long)seed, (unsigned long)r.status,
                (unsigned long)r.mism, (unsigned long)r.crc, (f <= 25000000U) ? "required" : "recorded");
        }
        if (both)
        {
            pass_mask |= 1UL << mbr;
        }
        else if (f <= 25000000U)
        {
            bad_req |= 1UL << mbr;
        }
    }
    uint32_t top = 8U;
    for (uint32_t mbr = 0U; mbr < 8U; mbr++)
    {
        if ((pass_mask & (1UL << mbr)) != 0U)
        {
            top = mbr;
            break;
        }
    }
    /* The control at the fastest passing speed: MISO on PB4 (pull-up): must fail. */
    SstXferRes_t r = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    if (top < 8U)
    {
        if (m4)
        {
            (void)cm4_send4(SST_CMD_J1PINS, SST_J1P_PB4_UP, 0U, GPIO_BM_SPEED_VERY_HIGH, 0U);
            (void)xfer_m4(SST_XCFG(0U, 8U, 0U, 1U, top, 0U), 1024U, next_seed(), SST_XM_POLL, &r);
            (void)cm4_send4(SST_CMD_J1PINS, SST_J1P_LOOP, 0U, GPIO_BM_SPEED_HIGH, 0U);
        }
        else
        {
            (void)spi6_pins(0U, 1U, GPIO_BM_SPEED_VERY_HIGH);
            const SpiBm_Config_t c = mkcfg(spi, top, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
            xfer_local(&c, 1024U, next_seed(), s_tx, s_rx, &r);
            (void)spi6_pins(0U, 0U, GPIO_BM_SPEED_HIGH);
        }
    }
    verdict(bad_req == 0U, "T8 %s 1024 frames of 8 and %lu bits, MBR /256../2: passed at /%lu and slower (mask 0x%02lX); "
            "failed where required (<= 25 MHz) 0x%02lX", name, (unsigned long)wide,
            (unsigned long)((top < 8U) ? (2UL << top) : 0UL), (unsigned long)pass_mask, (unsigned long)bad_req);
    verdict((top < 8U) && (r.mism > 0U), "T8 %s control at /%lu, MISO on PB4 (pull-up): %lu mismatches (> 0)", name,
            (unsigned long)((top < 8U) ? (2UL << top) : 0UL), (unsigned long)r.mism);
}

/* ---- T12(a): ES0445 2.22.1 on SPI6 ------------------------------------------------------ */
static void t12a_spi6(void)
{
    const uint32_t mbr = 7U;                      /* /256 */
    const SpiBm_Config_t c = mkcfg(SPI6, mbr, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    const uint32_t g = guard_of(SPI6, mbr);
    const uint32_t bud = budget_of(SPI6, mbr, 8U, 1U);
    uint32_t timeouts = 0U;
    uint32_t data_bad = 0U;
    (void)SPI_BM_Init(&c);
    uint8_t *tx = (uint8_t *)s_tx;
    uint8_t *rx = (uint8_t *)s_rx;
    for (uint32_t i = 0U; i < 10000U; i++)
    {
        tx[0] = (uint8_t)(i * 37U + 11U);
        rx[0] = (uint8_t)~tx[0];
        const SpiBm_Status_t st = SPI_BM_Transfer(SPI6, tx, rx, 1U, g, bud);
        if (st == SPI_BM_ERR_TIMEOUT)
        {
            timeouts++;
        }
        if ((st != SPI_BM_OK) || (rx[0] != tx[0]))
        {
            data_bad++;
        }
    }
    (void)SPI_BM_Deinit(SPI6);
    /* The control (decision 6), by hand: CSTART again with SPE = 1 right after each EOT. */
    (void)SPI_BM_Init(&c);
    SPI6->CR2 = 1U;
    SPI6->CR1 |= SPI_CR1_SPE;
    /* A stall is waited for 20 frame times, then the SPI is restarted (SPE 0 -> 1);
       after 20 stalls the control stops: it has shown what it records. */
    uint32_t stalls = 0U;
    uint32_t runs = 0U;
    for (uint32_t i = 0U; (i < 10000U) && (stalls < 20U); i++)
    {
        *(volatile uint8_t *)((uintptr_t)SPI6 + 0x20U) = (uint8_t)i;
        SPI6->CR1 |= SPI_CR1_CSTART;
        const uint32_t t0 = DWT->CYCCNT;
        while (((SPI6->SR & SPI_SR_EOT) == 0U) && ((DWT->CYCCNT - t0) < (20U * g * 10U)))
        {
        }
        if ((SPI6->SR & SPI_SR_EOT) == 0U)
        {
            stalls++;
            SPI6->CR1 &= ~SPI_CR1_SPE;
            SPI6->IFCR = SST_SPI_IFCR_ALL;
            SPI6->CR2 = 1U;
            SPI6->CR1 |= SPI_CR1_SPE;
        }
        else
        {
            (void)*(volatile uint8_t *)((uintptr_t)SPI6 + 0x30U);
            SPI6->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC;
        }
        runs++;
    }
    SPI6->IER = 0U;
    SPI6->CR1 &= ~SPI_CR1_SPE;
    SPI6->IFCR = SST_SPI_IFCR_ALL;
    (void)SPI_BM_Deinit(SPI6);
    verdict((timeouts == 0U) && (data_bad == 0U), "T12(a) 10000 transfers of 1 frame on SPI6 at /256 without pause: "
            "%lu ERR_TIMEOUT, %lu wrong (0, 0)", (unsigned long)timeouts, (unsigned long)data_bad);
    log_long("T12(a) control by hand (CSTART again with SPE = 1 right after EOT): %lu transfers, %lu stalls; %s "
             "(decision 6: recorded only)", (unsigned long)runs, (unsigned long)stalls,
             (stalls > 0U) ? "reproduced" : "not reproduced");
}

/* The J1_SPI6 step: CPU1 owns J1, SPI6 in AF8. */
static void step_j1_spi6(void)
{
    if (!step_begin("J1_SPI6"))
    {
        return;
    }
    static const uint32_t bits6[7] = { 4U, 5U, 7U, 8U, 9U, 15U, 16U };
    static const uint32_t ctl6[3] = { 4U, 8U, 16U };
    (void)SPI_BM_EnableClock(SPI6);
    (void)SPI_BM_Deinit(SPI6);
    t4_matrix(SPI6, "SPI6", false, bits6, 7U);
    /* once: 65535 frames of 8 bits */
    if (!s_bench_abort && spi6_pins(0U, 0U, GPIO_BM_SPEED_HIGH))
    {
        uint32_t mbr = 0U;
        uint32_t sck = 0U;
        (void)SPI_BM_CalcPrescaler(s_k6, 12500000U, &mbr, &sck);
        const SpiBm_Config_t c = mkcfg(SPI6, mbr, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
        const uint32_t seed = next_seed();
        SstXferRes_t r;
        xfer_local(&c, F1_BIGF, seed, s_big_tx, s_big_rx, &r);
        t4_line("SPI6", 0U, 8U, 0U, 1U, F1_BIGF, seed, &r);
        verdict(xfer_ok(&r), "T4 SPI6 65535 frames of 8 bits: status %lu, %lu mismatches",
                (unsigned long)r.status, (unsigned long)r.mism);
    }
    if (!s_bench_abort)
    {
        t4_control(SPI6, "SPI6", false, ctl6, 3U);
        (void)spi6_pins(0U, 0U, GPIO_BM_SPEED_HIGH);
    }
    if (!s_bench_abort)
    {
        t6_spi6("SPI6");
        t7b_spi6("SPI6");
        t8_sweep(SPI6, "SPI6", false, 16U);
        t12a_spi6();
    }
    if (!s_bench_abort && spi6_pins(0U, 0U, GPIO_BM_SPEED_HIGH))
    {
        f1_t9(SPI6, "SPI6");
        f1_t11b(SPI6, "SPI6");
        f1_b1(SPI6, "SPI6");
    }
    const SpiBm_Config_t ctrl = mkcfg(SPI6, 2U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    t14_check("SPI6", SPI6, &ctrl);
    const bool rel = j1_release();
    const uint32_t j1 = ((GPIOA->MODER >> (PA_J1_SCK * 2U)) & 0xFUL) | (((GPIOB->MODER >> (PB_J1_MISO * 2U)) & 0xFUL) << 4);
    verdict(rel && (j1 == 0U), "T14 J1 pins back in input: MODER PA5|PA6|PB4|PB5 0x%02lX (0)", (unsigned long)j1);
    if (!sync0())
    {
        return;
    }
    step_end();
}

/* The J1_SPI1 step: CPU2 owns J1 (SPI1, AF5); CPU1 orders and prints. */
static void step_j1_spi1(void)
{
    SstArea_t *a = SST_AREA;
    if (!step_begin("J1_SPI1"))
    {
        return;
    }
    static const uint32_t bits1[11] = { 4U, 5U, 7U, 8U, 9U, 15U, 16U, 17U, 24U, 31U, 32U };
    static const uint32_t ctl1[4] = { 4U, 8U, 16U, 32U };
    t4_matrix(SPI1, "SPI1", true, bits1, 11U);
    if (!s_bench_abort)
    {
        t4_control(SPI1, "SPI1", true, ctl1, 4U);
        (void)cm4_send4(SST_CMD_J1PINS, SST_J1P_LOOP, 0U, GPIO_BM_SPEED_HIGH, 0U);
    }
    /* T6 on SPI1 (CPU2's SPI1_IRQHandler) */
    uint32_t bad = 0U;
    for (uint32_t b = 0U; (b < 3U) && !s_bench_abort; b++)
    {
        const uint32_t bits = (b == 0U) ? 8U : ((b == 1U) ? 16U : 32U);
        const uint32_t frames[3] = { 1U, fifo_frames(SPI1, bits) + 1U, 1024U };
        for (uint32_t k = 0U; k < 3U; k++)
        {
            const uint32_t seed = next_seed();
            SstXferRes_t r;
            (void)xfer_m4(SST_XCFG(0U, bits, 0U, 1U, 5U, 0U), frames[k], seed, SST_XM_IT, &r);
            const uint32_t after = a->m4_res[9];
            LOG("T6 SPI1 b%lu n%lu s=%08lX st=%lu mis=%lu crc=%08lX irq=%lu after=%lu", (unsigned long)bits,
                (unsigned long)frames[k], (unsigned long)seed, (unsigned long)r.status, (unsigned long)r.mism,
                (unsigned long)r.crc, (unsigned long)r.irqs, (unsigned long)after);
            if (!xfer_ok(&r) || (after != 0U))
            {
                bad++;
            }
        }
    }
    uint32_t spurious = 0U;
    if (cm4_send(SST_CMD_T6C, 1U))
    {
        spurious = a->m4_res[0];
    }
    verdict(bad == 0U, "T6 SPI1 by interrupt on CPU2 (8/16/32 bits, 1, FIFO + 1, 1024 frames): %lu failed, (c) IRQs "
            "in the 10 ms after each end 0", (unsigned long)bad);
    verdict(spurious >= 1U, "T6(c) control on CPU2: TXPIE = 1 by hand with SPE = 0: %lu spurious IRQ (>= 1)",
            (unsigned long)spurious);
    /* T7(a): SPI1 by DMA2 streams 0 (RX) and 1 (TX), CPU2's buffers in SRAM2 */
    bad = 0U;
    for (uint32_t b = 0U; (b < 3U) && !s_bench_abort; b++)
    {
        const uint32_t bits = (b == 0U) ? 8U : ((b == 1U) ? 16U : 32U);
        const uint32_t frames[5] = { 1U, 2U, 17U, 255U, 4096U };
        for (uint32_t k = 0U; k < 5U; k++)
        {
            const uint32_t seed = next_seed();
            SstXferRes_t r;
            (void)xfer_m4(SST_XCFG(0U, bits, 0U, 1U, 3U, 0U), frames[k], seed, SST_XM_DMA, &r);
            LOG("T7A SPI1 b%lu n%lu s=%08lX st=%lu mis=%lu crc=%08lX", (unsigned long)bits, (unsigned long)frames[k],
                (unsigned long)seed, (unsigned long)r.status, (unsigned long)r.mism, (unsigned long)r.crc);
            if (!xfer_ok(&r))
            {
                bad++;
            }
        }
    }
    SstXferRes_t rc = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    (void)xfer_m4(SST_XCFG(0U, 8U, 0U, 1U, 3U, 0U), 255U, next_seed(), SST_XM_DMA_NORX, &rc);
    verdict(bad == 0U, "T7(a) SPI1 by DMA2 (CPU2, 8/16/32 bits, 1..4096 frames): %lu failed", (unsigned long)bad);
    verdict((rc.status == (uint32_t)SPI_BM_ERR_OVERRUN) && (rc.sentinel_left == 255U),
            "T7(a) control RXDMAEN = 0, 255 frames: WaitEnd %lu (ERR_OVERRUN %u), RX words with the sentinel %lu (255)",
            (unsigned long)rc.status, (unsigned)SPI_BM_ERR_OVERRUN, (unsigned long)rc.sentinel_left);
    if (!s_bench_abort)
    {
        t8_sweep(SPI1, "SPI1", true, 32U);
    }
    if (!s_bench_abort && cm4_send4(SST_CMD_J1PINS, SST_J1P_LOOP, 0U, GPIO_BM_SPEED_HIGH, 0U))
    {
        f1_m4_t9();
        if (cm4_send(SST_CMD_B1, 0U))
        {
            LOG("B1 CM4 SPI1 SPI_BM_Init %lu cycles, SPI_BM_Transfer of 1 frame at /2 %lu cycles, TransferIrq per "
                "call min %lu max %lu", (unsigned long)a->m4_res[0], (unsigned long)a->m4_res[1],
                (unsigned long)a->m4_res[2], (unsigned long)a->m4_res[3]);
        }
    }
    if (cm4_send(SST_CMD_T14, 0U))
    {
        verdict((a->m4_res[0] == (uint32_t)SPI_BM_OK) && (a->m4_res[1] == 0U),
                "T14 SPI1 (CPU2) Deinit %lu, registers off the RM0399 reset image 0x%02lX (0)",
                (unsigned long)a->m4_res[0], (unsigned long)a->m4_res[1]);
    }
    if (cm4_send4(SST_CMD_J1PINS, SST_J1P_RELEASE, 0U, GPIO_BM_SPEED_LOW, 0U))
    {
        verdict(a->m4_res[0] == 1U, "T14 CPU2's J1 pins back in input: MODER PA5|PA6|PB4|PB5 0x%02lX (0)",
                (unsigned long)a->m4_res[1]);
    }
    LOG("B1 CM7 TransferIrq per call (T6, SPI6) min %lu max %lu cycles", (unsigned long)s_b1_irq_min,
        (unsigned long)s_b1_irq_max);
    if (!sync0())
    {
        return;
    }
    step_end();
}

/* ==========================================================================================
 * F1 on the AD2 bus: SPI4 (CPU1, AF5 on PE11..PE14, S13), the AD2 only listening
 * ========================================================================================== */

/* PE11 NSS pulled to its inactive level, PE12 SCK to CPOL, PE13 MISO (input) to
   `miso_pull`, PE14 MOSI down; each claim reads SPI4 back at rest (S13). */
static bool spi4_pins(uint32_t cpol, bool ssiop, uint32_t miso_pull, uint32_t sck_pull)
{
    s_af_speed = GPIO_BM_SPEED_MEDIUM;
    const bool ok =
        pin_cfg(GPIOE, PE_NSS, GPIO_BM_MODE_AF, SST_AF_SPI4, ssiop ? GPIO_BM_PULL_DOWN : GPIO_BM_PULL_UP, 0U) &&
        pin_cfg(GPIOE, PE_SCK, GPIO_BM_MODE_AF, SST_AF_SPI4,
                (sck_pull != 0xFFU) ? sck_pull : ((cpol != 0U) ? GPIO_BM_PULL_UP : GPIO_BM_PULL_DOWN), 0U) &&
        pin_cfg(GPIOE, PE_MISO, GPIO_BM_MODE_AF, SST_AF_SPI4, miso_pull, 0U) &&
        pin_cfg(GPIOE, PE_MOSI, GPIO_BM_MODE_AF, SST_AF_SPI4, GPIO_BM_PULL_DOWN, 0U);
    s_af_speed = GPIO_BM_SPEED_LOW;
    return ok;
}

/* A handshake whose action runs between the host's HOST_REQ rise (its LA armed) and
   MCU_ACK falling. */
typedef SpiBm_Status_t (*CapFn_t)(void);
static bool sync_run(CapFn_t fn, SpiBm_Status_t *st)
{
    if (s_bench_abort)
    {
        return false;
    }
    ack(true);
    bool ok = wait_req(true, SYNC_TIMEOUT_CYC);
    if (ok)
    {
        *st = fn();
    }
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

static SpiBm_Config_t s_cap_cfg;
static uint32_t       s_cap_n;
static uint32_t       s_cap_k;
static uint32_t       s_cap_bad;

static SpiBm_Status_t cap_poll(void)
{
    return x_transfer(SPI4, s_tx, NULL, s_cap_n, guard_of(SPI4, s_cap_cfg.mbr),
                           budget_of(SPI4, s_cap_cfg.mbr, s_cap_cfg.data_bits, s_cap_n));
}

static DMA_HandleTypeDef s_hd1;      /* F3: DMA1 stream 0 for SPI4 TX through dma_hal */

static SpiBm_Status_t cap_dma(void)
{
    if (s_tier == TIER_HAL)
    {
        s_hal_busy = true;
        SpiBm_Status_t st = SPI_HAL_StartTransferDma(&s_h4, s_dma1_buf, NULL, s_cap_n);
        if (st == SPI_BM_OK)
        {
            st = SPI_HAL_Wait(&s_h4, ms_of(budget_of(SPI4, s_cap_cfg.mbr, s_cap_cfg.data_bits, s_cap_n)));
        }
        s_hal_busy = false;
        return st;
    }
    const SpiBm_Status_t s = x_dma_start(SPI4);
    const SpiBm_Status_t w = x_wait_end(SPI4, budget_of(SPI4, s_cap_cfg.mbr, s_cap_cfg.data_bits, s_cap_n));
    return (s != SPI_BM_OK) ? s : w;
}

/* T12(b)'s control (decision 6), by hand: SPE = 0 right after EOT, no guard. */
static SpiBm_Status_t cap_noguard(void)
{
    SPI4->CR2 = 1U;
    SPI4->CR1 |= SPI_CR1_SPE;
    *(volatile uint8_t *)((uintptr_t)SPI4 + 0x20U) = (uint8_t)s_tx[0];
    SPI4->CR1 |= SPI_CR1_CSTART;
    const uint32_t t0 = DWT->CYCCNT;
    while (((SPI4->SR & SPI_SR_EOT) == 0U) && ((DWT->CYCCNT - t0) < (20U * CYC_PER_MS)))
    {
    }
    const bool eot = (SPI4->SR & SPI_SR_EOT) != 0U;
    SPI4->CR1 &= ~SPI_CR1_SPE;
    SPI4->IFCR = SST_SPI_IFCR_ALL;
    return eot ? SPI_BM_OK : SPI_BM_ERR_TIMEOUT;
}

/* One capture: configuration announced ("CAP"), the transfer inside the handshake,
   its status afterwards ("CAPR"). The host decodes the LA capture against the seed. */
static void cap_one(char kind, const SpiBm_Config_t *c, uint32_t n, CapFn_t fn)
{
    if (s_bench_abort)
    {
        return;
    }
    s_cap_cfg = *c;
    s_cap_n = n;
    const uint32_t seed = next_seed();
    sst_fill(s_tx, sst_elem_bytes(c->data_bits), n, c->data_bits, seed);
    const SpiBm_Status_t si = x_init(c);
    if (!spi4_pins(c->mode >> 1, c->nss_active_high, GPIO_BM_PULL_DOWN, 0xFFU))
    {
        return;
    }
    HAL_StatusTypeDef dinit = HAL_OK;
    if ((fn == cap_dma) && (s_tier == TIER_HAL))
    {
        sst_fill(s_dma1_buf, sst_elem_bytes(c->data_bits), n, c->data_bits, seed);
        const uint32_t al = (c->data_bits <= 8U) ? 0U : 1U;
        const DmaHal_Config_t dc = {
            .instance = DMA1_Stream0, .request = DMA_REQUEST_SPI4_TX, .direction = DMA_MEMORY_TO_PERIPH,
            .periph_inc = DMA_PINC_DISABLE, .mem_inc = DMA_MINC_ENABLE,
            .periph_align = (al == 0U) ? DMA_PDATAALIGN_BYTE : DMA_PDATAALIGN_HALFWORD,
            .mem_align = (al == 0U) ? DMA_MDATAALIGN_BYTE : DMA_MDATAALIGN_HALFWORD, .mode = DMA_NORMAL,
            .priority = DMA_PRIORITY_HIGH, .fifo_mode = DMA_FIFOMODE_ENABLE, .fifo_threshold = DMA_FIFO_THRESHOLD_FULL,
            .mem_burst = DMA_MBURST_SINGLE, .periph_burst = DMA_PBURST_SINGLE, .enable_clock = true,
            .irq_priority = 6 };
        /* FIFO mode: DMA_HAL_Init (02_dma, dma_hal.c:183-186) compares the PSIZE and MSIZE
           encodings of a direct-mode stream and refuses every size above a byte. */
        dinit = DMA_HAL_Init(&s_hd1, &dc);
        s_h4.hdmatx = NULL;
        if (dinit != HAL_OK)
        {
            s_cap_bad++;
        }
        else
        {
            __HAL_LINKDMA(&s_h4, hdmatx, s_hd1);
        }
        s_h4.hdmarx = NULL;
        HAL_NVIC_SetPriority(SPI4_IRQn, 6U, 0U);
        NVIC_ClearPendingIRQ(SPI4_IRQn);
        HAL_NVIC_EnableIRQ(SPI4_IRQn);
    }
    else if (fn == cap_dma)
    {
        uint32_t req = 0U;
        (void)SPI_BM_GetDmaRequest(SPI4, true, &req);
        const DmaBm_ConfigEx_t d = {
            .base = { .stream = DMA1_Stream0, .request_id = req, .direction = DMA_BM_DIR_MEM_TO_PERIPH,
                      .periph_data_size = (c->data_bits <= 8U) ? DMA_BM_DATA_SIZE_BYTE : DMA_BM_DATA_SIZE_HALFWORD,
                      .mem_data_size = (c->data_bits <= 8U) ? DMA_BM_DATA_SIZE_BYTE : DMA_BM_DATA_SIZE_HALFWORD,
                      .periph_inc = false, .mem_inc = true, .circular = false, .priority = DMA_BM_PRIORITY_HIGH,
                      .enable_it_ht = false, .enable_it_tc = false, .enable_it_te = false, .enable_it_dme = false,
                      .enable_it_fe = false, .enable_trbuff = false, .enable_fifo = false,
                      .fifo_threshold = DMA_BM_FIFO_TH_1_2, .periph_addr = (uint32_t)&SPI4->TXDR,
                      .mem_addr = (uint32_t)s_dma1_buf, .buffer_length = n },
            .mem1_addr = 0U, .mem_burst = DMA_BM_BURST_SINGLE, .periph_burst = DMA_BM_BURST_SINGLE,
            .periph_flow_ctrl = false, .fail_if_busy = true, .stop_spin_limit = 0U };
        sst_fill(s_dma1_buf, sst_elem_bytes(c->data_bits), n, c->data_bits, seed);
        if ((x_dma_prepare(SPI4, n, false) != SPI_BM_OK) || !DMA_BM_InitStreamEx(&d) || !DMA_BM_StartEx(DMA1_Stream0))
        {
            s_cap_bad++;
        }
    }
    log_long("CAP %c k%lu m%lu b%lu l%u n%lu nss%lu op%u mssi%lu midi%lu mbr%lu s=%08lX", kind, (unsigned long)s_cap_k,
             (unsigned long)c->mode, (unsigned long)c->data_bits, (unsigned)c->lsb_first, (unsigned long)n,
             (unsigned long)c->nss, (unsigned)c->nss_active_high, (unsigned long)c->mssi, (unsigned long)c->midi,
             (unsigned long)c->mbr, (unsigned long)seed);
    SpiBm_Status_t st = SPI_BM_ERR_PARAM;
    if (!sync_run(fn, &st))
    {
        return;
    }
    if ((fn == cap_dma) && (s_tier == TIER_HAL))
    {
        LOG("CAPH k%lu si=%u dinit=%u herr=%08lX hstate=%u derr=%08lX dstate=%u dcr=%08lX ndtr=%lu buf=%08lX",
            (unsigned long)s_cap_k, (unsigned)si, (unsigned)dinit, (unsigned long)s_h4.ErrorCode,
            (unsigned)s_h4.State, (unsigned long)s_hd1.ErrorCode, (unsigned)s_hd1.State,
            (unsigned long)DMA1_Stream0->CR, (unsigned long)DMA1_Stream0->NDTR, (unsigned long)(uintptr_t)s_dma1_buf);
        HAL_NVIC_DisableIRQ(SPI4_IRQn);
        NVIC_ClearPendingIRQ(SPI4_IRQn);
        (void)DMA_HAL_DeInit(&s_hd1);
        s_h4.hdmatx = NULL;
    }
    else if (fn == cap_dma)
    {
        (void)DMA_BM_StopEx(DMA1_Stream0, 0U);
        if (x_dma_end(SPI4, guard_of(SPI4, c->mbr)) != SPI_BM_OK)
        {
            s_cap_bad++;
        }
        (void)DMA_BM_DeinitStream(DMA1_Stream0);
    }
    const SpiBm_Status_t sd = SPI_BM_Deinit(SPI4);
    LOG("CAPR k%lu st=%u", (unsigned long)s_cap_k, (unsigned)st);
    if ((si != SPI_BM_OK) || (st != SPI_BM_OK) || (sd != SPI_BM_OK))
    {
        s_cap_bad++;
    }
    s_cap_k++;
}

/* Frames that fit in the AD2's 4096 samples at 100 MS/s (one sample per kernel tick at
   100 MHz): about 3000 samples of bus time after a 300-sample prefill. */
static uint32_t cap_frames(uint32_t bits, uint32_t mbr, uint32_t mssi, uint32_t midi, uint32_t cap)
{
    const uint32_t per = 2UL << mbr;
    const uint32_t frame = (bits + midi) * per;
    const uint32_t room = 3000U - ((mssi + 2U) * per);
    uint32_t n = room / frame;
    if (n > cap)
    {
        n = cap;
    }
    return (n < 1U) ? 1U : n;
}

static void step_spi4_ad2(void)
{
    if (!step_begin("SPI4_AD2") || !buschk())
    {
        return;
    }
    const bool clk_was_on = spi4_clocked();
    (void)SPI_BM_EnableClock(SPI4);
    (void)SPI_BM_Deinit(SPI4);
    /* MCO1 = HSE/8 on PA8 (AF0) for T5(g); RCC_CFGR saved, restored with read-back (R4). */
    const uint32_t cfgr_before = RCC->CFGR;
    s_mco_saved = cfgr_before & (RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE);
    s_mco_changed = true;
    MODIFY_REG(RCC->CFGR, RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE, RCC_CFGR_MCO1_1 | RCC_CFGR_MCO1PRE_3);
    if (!pin_cfg(GPIOA, PA_MCO, GPIO_BM_MODE_AF, SST_AF_MCO1, GPIO_BM_PULL_NONE, 0U))
    {
        return;
    }
    /* S5 with PE11..PE14 in AF5 and SPI4 at its reset values (S13). */
    if (!spi4_pins(0U, false, GPIO_BM_PULL_DOWN, 0xFFU) || !buschk_af(PE_SPI4_MASK, NULL, NULL))
    {
        return;
    }
    s_cap_k = 0U;
    s_cap_bad = 0U;
    /* (a) modes x {4, 8, 13, 16} x MSB/LSB, hardware SS, /16 */
    static const uint32_t ab[4] = { 4U, 8U, 13U, 16U };
    for (uint32_t mode = 0U; mode <= 3U; mode++)
    {
        for (uint32_t ib = 0U; ib < 4U; ib++)
        {
            for (uint32_t lsb = 0U; lsb < 2U; lsb++)
            {
                const SpiBm_Config_t c = { SPI4, 3U, mode, ab[ib], lsb != 0U, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW,
                                           false, 0U, 0U, false };
                cap_one('a', &c, cap_frames(ab[ib], 3U, 0U, 0U, 32U), cap_poll);
            }
        }
    }
    /* (b) SSIOP = 1 (SSIOP = 0 is (a)) */
    {
        const SpiBm_Config_t c = { SPI4, 3U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW, true, 0U, 0U, false };
        cap_one('b', &c, cap_frames(8U, 3U, 0U, 0U, 16U), cap_poll);
    }
    /* (c) SS pulsed between frames: NSS_HW_PULSE with MIDI = 2 */
    {
        const SpiBm_Config_t c = { SPI4, 3U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW_PULSE, false, 0U, 2U,
                                   false };
        cap_one('c', &c, cap_frames(8U, 3U, 0U, 2U, 12U), cap_poll);
    }
    /* (d) MSSI and MIDI 0, 1, 4, 15 */
    static const uint32_t dk[4] = { 0U, 1U, 4U, 15U };
    for (uint32_t i = 0U; i < 4U; i++)
    {
        const SpiBm_Config_t c = { SPI4, 3U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW, false, dk[i], 0U,
                                   false };
        cap_one('d', &c, cap_frames(8U, 3U, dk[i], 0U, 6U), cap_poll);
    }
    for (uint32_t i = 0U; i < 4U; i++)
    {
        const SpiBm_Config_t c = { SPI4, 3U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW, false, 0U, dk[i],
                                   false };
        cap_one('d', &c, cap_frames(8U, 3U, 0U, dk[i], 6U), cap_poll);
    }
    /* (g) SCK frequency against MCO1, MBR /4 ... /256; long enough to fill the capture */
    for (uint32_t mbr = 1U; mbr <= 7U; mbr++)
    {
        const SpiBm_Config_t c = { SPI4, mbr, 0U, 16U, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_SOFT, false, 0U, 0U,
                                   false };
        cap_one('g', &c, (4000U / (16U * (2U << mbr))) + 2U, cap_poll);
    }
    /* T7(c): SPI4 transmit only by DMA1 stream 0 */
    for (uint32_t b = 0U; b < 2U; b++)
    {
        const uint32_t bits = (b == 0U) ? 8U : 16U;
        const SpiBm_Config_t c = { SPI4, 3U, 0U, bits, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW, false, 0U, 0U,
                                   false };
        cap_one('m', &c, cap_frames(bits, 3U, 0U, 0U, 16U), cap_dma);
    }
    /* T12(b): CPHA = 0, /256, one frame: the driver's guard, then the control without it */
    {
        const SpiBm_Config_t c = { SPI4, 7U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_SOFT, false, 0U, 0U,
                                   false };
        cap_one('G', &c, 1U, cap_poll);
        cap_one('N', &c, 1U, cap_noguard);
    }
    /* (e) keep_pins 1 and 0, SCK pulled up against CPOL = 0, the level read between two
       transfers; a keep_pins step ends with Deinit (S13) */
    for (uint32_t keep = 0U; (keep < 2U) && !s_bench_abort; keep++)
    {
        const bool kp = (keep == 0U);            /* 1 first, then the control 0 */
        SpiBm_Config_t c = { SPI4, 3U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW, false, 0U, 0U, false };
        (void)SPI_BM_Init(&c);
        if (!spi4_pins(0U, false, GPIO_BM_PULL_DOWN, GPIO_BM_PULL_UP))
        {
            return;
        }
        c.keep_pins = kp;
        const SpiBm_Status_t si = SPI_BM_Init(&c);
        sst_fill(s_tx, 1U, 2U, 8U, next_seed());
        const SpiBm_Status_t s1 = SPI_BM_Transfer(SPI4, s_tx, NULL, 2U, guard_of(SPI4, 3U), budget_of(SPI4, 3U, 8U, 2U));
        LOG("CAP e k%lu keep%u", (unsigned long)s_cap_k, (unsigned)kp);
        if (!sync0())                              /* host: DIO4 level between the transfers */
        {
            return;
        }
        const SpiBm_Status_t s2 = SPI_BM_Transfer(SPI4, s_tx, NULL, 2U, guard_of(SPI4, 3U), budget_of(SPI4, 3U, 8U, 2U));
        const SpiBm_Status_t sd = SPI_BM_Deinit(SPI4);
        if ((si != SPI_BM_OK) || (s1 != SPI_BM_OK) || (s2 != SPI_BM_OK) || (sd != SPI_BM_OK))
        {
            s_cap_bad++;
        }
        s_cap_k++;
    }
    LOG("CAP done k%lu", (unsigned long)s_cap_k);
    verdict(s_cap_bad == 0U, "T5/T7(c)/T12(b) SPI4: %lu captures and level reads run, %lu with a failed Init, transfer, "
            "DMA step or Deinit", (unsigned long)s_cap_k, (unsigned long)s_cap_bad);
    /* (f) RX of SPI4 = the pull of PE13: up, then down */
    for (uint32_t p = 0U; (p < 2U) && !s_bench_abort; p++)
    {
        const SpiBm_Config_t c = { SPI4, 3U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U, SPI_BM_NSS_HW, false, 0U, 0U,
                                   false };
        (void)SPI_BM_Init(&c);
        if (!spi4_pins(0U, false, (p == 0U) ? GPIO_BM_PULL_UP : GPIO_BM_PULL_DOWN, 0xFFU))
        {
            return;
        }
        sst_fill(s_tx, 1U, 16U, 8U, next_seed());
        sst_fill_const(s_rx, 1U, 16U, 0x5AU);
        const SpiBm_Status_t st = SPI_BM_Transfer(SPI4, s_tx, s_rx, 16U, guard_of(SPI4, 3U), budget_of(SPI4, 3U, 8U, 16U));
        uint32_t want = (p == 0U) ? 0xFFU : 0x00U;
        uint32_t bad = 0U;
        for (uint32_t i = 0U; i < 16U; i++)
        {
            if (((const uint8_t *)s_rx)[i] != want)
            {
                bad++;
            }
        }
        (void)SPI_BM_Deinit(SPI4);
        verdict((st == SPI_BM_OK) && (bad == 0U), "T5(f) SPI4 full duplex, PE13 (MISO, input) pulled %s: RX 16 frames, "
                "%lu not 0x%02lX; status %u", (p == 0U) ? "up" : "down", (unsigned long)bad, (unsigned long)want,
                (unsigned)st);
    }
    /* T9 on SPI4 (see the note above f1_t9): transmit only, NSS_SOFT (PE11 not driven) */
    if (!s_bench_abort && spi4_pins(0U, false, GPIO_BM_PULL_DOWN, 0xFFU))
    {
        f1_t9(SPI4, "SPI4");
    }
    /* T14: Deinit, S5 still in AF5, then in input */
    const SpiBm_Config_t ctrl = { SPI4, 3U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW, false, 0U, 0U, false };
    t14_check("SPI4", SPI4, &ctrl);
    if (!spi4_pins(0U, false, GPIO_BM_PULL_DOWN, 0xFFU) || !buschk_af(PE_SPI4_MASK, NULL, NULL))
    {
        return;
    }
    (void)pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
    (void)pin_cfg(GPIOA, PA_MCO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U);
    const uint32_t mco_on = RCC->CFGR & (RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE);
    mco_restore();
    const uint32_t cfgr_after = RCC->CFGR;
    verdict((mco_on == (RCC_CFGR_MCO1_1 | RCC_CFGR_MCO1PRE_3)) && (cfgr_after == cfgr_before),
            "T5(g) MCO1 = HSE/8 during the step (0x%08lX); RCC_CFGR restored: before 0x%08lX after 0x%08lX",
            (unsigned long)mco_on, (unsigned long)cfgr_before, (unsigned long)cfgr_after);
    if (!buschk())
    {
        return;
    }
    if (!clk_was_on)
    {
        (void)SPI_BM_DisableClock(SPI4);
    }
    step_end();
}

/* ==========================================================================================
 * Phase F2: spi_ll (plan revision 3: T1, T2, T4, T5(a), T6, T7(a)(b), T14 with LL on both
 * cores; T15 with BM and LL; B1). The tests of F1 run again with s_tier = TIER_LL; the LL
 * configuration of each is the test's own (sst_ll_of(), written by hand from the LL
 * constants).
 * ========================================================================================== */

/* ---- T1 with spi_ll (CPU1: SPI2, SPI4, SPI5, SPI6) ------------------------------------- */
static void f2_t1(void)
{
    SPI_TypeDef *const inst[4] = { SPI2, SPI4, SPI5, SPI6 };
    for (uint32_t i = 0U; i < 4U; i++)
    {
        (void)SPI_BM_EnableClock(inst[i]);
        (void)SPI_BM_Deinit(inst[i]);
    }
    uint32_t a[T1_SNAP];
    uint32_t b[T1_SNAP];
    uint32_t same = 0U;
    const SpiBm_Config_t cb = mkcfg(SPI2, 7U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    SpiLl_Config_t c;
    sst_ll_of(&cb, &c);
    SpiLl_Config_t k;
    s_t1_n = 0U;

    /* 1. Nothing initialised: every LL field with a constant LL does not know, the
          capability, the FIFO range, and the parameters of the other calls. */
    t1_snap(a);
    t1(SPI_LL_CheckConfig(NULL), SPI_BM_ERR_PARAM);
    t1(SPI_LL_Init(NULL), SPI_BM_ERR_PARAM);
    k = c; k.spi = (SPI_TypeDef *)USART3;                          t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.spi = (SPI_TypeDef *)((uint32_t)SPI1 + 4U);           t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.data_width = SPI_CFG1_DSIZE_1;                         t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);  /* 3 bits */
    k = c; k.data_width = LL_SPI_DATAWIDTH_8BIT | (1UL << 5);       t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);  /* stray */
    k = c; k.spi = SPI4; k.data_width = LL_SPI_DATAWIDTH_17BIT;     t1(SPI_LL_Init(&k), SPI_BM_ERR_UNSUPPORTED);
    k = c; k.spi = SPI5; k.data_width = LL_SPI_DATAWIDTH_17BIT;     t1(SPI_LL_Init(&k), SPI_BM_ERR_UNSUPPORTED);
    k = c; k.spi = SPI6; k.data_width = LL_SPI_DATAWIDTH_17BIT;     t1(SPI_LL_Init(&k), SPI_BM_ERR_UNSUPPORTED);
    k = c; k.clock_polarity = SPI_CFG2_CPHA;                        t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.clock_phase = SPI_CFG2_CPOL;                           t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.bit_order = 1U;                                        t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.nss_polarity = SPI_CFG2_SSOE;                          t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.baudrate = 1U;                                         t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.transfer_direction = LL_SPI_SIMPLEX_RX;                t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.transfer_direction = LL_SPI_HALF_DUPLEX_TX;            t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.nss = LL_SPI_NSS_HARD_INPUT;                           t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.nss_pulse = true;                                      t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);  /* with SOFT */
    k = c; k.ss_idleness = LL_SPI_SS_IDLENESS_05CYCLE;              t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);  /* with SOFT */
    k = c; k.nss = LL_SPI_NSS_HARD_OUTPUT; k.ss_idleness = SPI_CFG2_MIDI_0;
                                                                    t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.interdata_idleness = 1U;                               t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.fifo_threshold = 1U;                                   t1(SPI_LL_Init(&k), SPI_BM_ERR_PARAM);
    k = c; k.spi = SPI4; k.data_width = LL_SPI_DATAWIDTH_16BIT; k.fifo_threshold = LL_SPI_FIFO_TH_03DATA;
                                                                    t1(SPI_LL_Init(&k), SPI_BM_ERR_RANGE);
    k = c; k.data_width = LL_SPI_DATAWIDTH_32BIT; k.fifo_threshold = LL_SPI_FIFO_TH_03DATA;
                                                                    t1(SPI_LL_Init(&k), SPI_BM_ERR_RANGE);
    t1(SPI_LL_TransferStartIt(SPI2, NULL), SPI_BM_ERR_PARAM);
    t1(SPI_LL_DmaStart(NULL), SPI_BM_ERR_PARAM);
    t1(SPI_LL_WaitEnd(NULL, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_LL_DmaEnd((SPI_TypeDef *)USART3, 1U), SPI_BM_ERR_PARAM);
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 1U : 0U;

    /* 2. SPI2 transmit only, SPI5 16-bit, both through spi_ll: the transfer's parameters. */
    k = c; k.transfer_direction = LL_SPI_SIMPLEX_TX;
    const SpiBm_Status_t i2 = SPI_LL_Init(&k);
    k = c; k.spi = SPI5; k.data_width = LL_SPI_DATAWIDTH_16BIT;
    const SpiBm_Status_t i5 = SPI_LL_Init(&k);
    SpiBm_Xfer_t x = { s_tx, NULL, 1U, 0U, 0U, 0U, SPI_BM_OK };
    t1_snap(a);
    t1(SPI_LL_Transfer(SPI2, s_tx, NULL, 0U, 1U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_LL_Transfer(SPI2, s_tx, NULL, 65536U, 1U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_LL_Transfer(SPI2, NULL, NULL, 1U, 1U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_LL_Transfer(SPI2, s_tx, NULL, 1U, 0U, 1000U), SPI_BM_ERR_PARAM);
    t1(SPI_LL_Transfer(SPI2, s_tx, NULL, 1U, 1U, 0U), SPI_BM_ERR_PARAM);
    t1(SPI_LL_Transfer(SPI2, s_tx, s_rx, 1U, 1U, 1000U), SPI_BM_ERR_PARAM);          /* rx in TX_ONLY */
    t1(SPI_LL_Transfer(SPI5, (const uint8_t *)s_tx + 1U, NULL, 1U, 1U, 1000U), SPI_BM_ERR_PARAM);   /* odd */
    t1(SPI_LL_TransferStartIt(SPI2, &x), SPI_BM_ERR_PARAM);                             /* guard 0 */
    t1(SPI_LL_DmaPrepare(SPI2, 1U, true), SPI_BM_ERR_PARAM);                            /* rx in TX_ONLY */
    t1(SPI_LL_DmaPrepare(SPI2, 0U, false), SPI_BM_ERR_PARAM);
    t1(SPI_LL_DmaPrepare(SPI2, 65536U, false), SPI_BM_ERR_PARAM);
    t1(SPI_LL_DmaEnd(SPI2, 0U), SPI_BM_ERR_PARAM);
    t1(SPI_LL_WaitEnd(SPI2, 0U), SPI_BM_ERR_PARAM);
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 2U : 0U;
    (void)SPI_BM_Deinit(SPI5);

    /* 3. SPE = 1 (by hand; SPI2 has no pin). */
    k = c;
    const SpiBm_Status_t i3 = SPI_LL_Init(&k);
    SPI2->CR1 |= SPI_CR1_SPE;
    const uint32_t spe = SPI2->CR1 & SPI_CR1_SPE;
    t1_snap(a);
    t1(SPI_LL_Init(&k), SPI_BM_ERR_BUSY);
    t1(SPI_LL_CheckConfig(&k), SPI_BM_ERR_BUSY);
    t1(SPI_LL_DmaPrepare(SPI2, 1U, false), SPI_BM_ERR_BUSY);
    t1(SPI_LL_DmaStart(SPI2), SPI_BM_ERR_BUSY);
    t1(SPI_LL_Transfer(SPI2, s_tx, NULL, 1U, 1U, 1000U), SPI_BM_ERR_BUSY);
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 4U : 0U;
    SPI2->CR1 &= ~SPI_CR1_SPE;

    /* 4. AFCNTR = 1 in force (keep_pins; SPI2 has no pin): another CPOL or SSIOP. */
    k = c; k.keep_pins = true;
    const SpiBm_Status_t i4 = SPI_LL_Init(&k);
    t1_snap(a);
    k.clock_polarity = LL_SPI_POLARITY_HIGH;                    t1(SPI_LL_Init(&k), SPI_BM_ERR_BUSY);
    k.clock_polarity = LL_SPI_POLARITY_LOW;
    k.nss_polarity = LL_SPI_NSS_POLARITY_HIGH;                  t1(SPI_LL_Init(&k), SPI_BM_ERR_BUSY);
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 8U : 0U;
    (void)SPI_BM_Deinit(SPI2);

    /* The control: a valid SPI_LL_Init changes the snapshot. */
    t1_snap(a);
    k = c;
    const SpiBm_Status_t w = SPI_LL_Init(&k);
    t1_snap(b);
    const bool ctrl = memcmp(a, b, sizeof(a)) != 0;
    (void)SPI_BM_Deinit(SPI2);

    uint32_t bad = 0U;
    for (uint32_t i = 0U; (i < s_t1_n) && (i < T1_MAX); i++)
    {
        if (s_t1_got[i] != s_t1_exp[i])
        {
            bad++;
            LOG("T1 LL CM7 case %lu: status %u, expected %u", (unsigned long)i, (unsigned)s_t1_got[i],
                (unsigned)s_t1_exp[i]);
        }
    }
    verdict((bad == 0U) && (s_t1_n <= T1_MAX) && (same == 0xFU) && (spe != 0U) &&
            (i2 == SPI_BM_OK) && (i5 == SPI_BM_OK) && (i3 == SPI_BM_OK) && (i4 == SPI_BM_OK),
            "T1 LL CM7 refusals through spi_ll on SPI2, SPI4, SPI5, SPI6: %lu cases, %lu with another status; "
            "registers of the four and RCC_C1/C2 APB1LENR/APB2ENR/APB4ENR unchanged in the 4 groups: 0x%lX (0xF); "
            "SPE set by hand %u", (unsigned long)s_t1_n, (unsigned long)bad, (unsigned long)same,
            (unsigned)(spe != 0U));
    verdict(ctrl && (w == SPI_BM_OK), "T1 LL CM7 control: a valid SPI_LL_Init(SPI2) changes the snapshot (%u)",
            (unsigned)ctrl);
}

/* ---- T2 with spi_ll: the image of each configuration against the RM table and against
   the one spi_bm leaves, bit for bit (A24). The photo is spi_regs() (8 registers). ---- */
typedef struct { uint32_t n, bad_rm, bad_bm, bad_init, bad_deinit, first_i, first_reg, got, exp; } T2LlCount_t;

static void t2ll_one(const SpiBm_Config_t *c, T2LlCount_t *t)
{
    SPI_TypeDef *spi = c->spi;
    uint32_t pb[NREG];
    uint32_t pl[NREG];
    const SpiBm_Status_t sb = SPI_BM_Init(c);
    spi_regs(spi, pb);
    const SpiBm_Status_t db = SPI_BM_Deinit(spi);
    SpiLl_Config_t l;
    sst_ll_of(c, &l);
    const SpiBm_Status_t sl = SPI_LL_Init(&l);
    spi_regs(spi, pl);
    const SpiBm_Status_t dl = SPI_BM_Deinit(spi);
    uint32_t e1 = 0U;
    uint32_t e2 = 0U;
    uint32_t ecr = 0U;
    sst_t2_expect(c, &e1, &e2, &ecr);
    if ((sb != SPI_BM_OK) || (sl != SPI_BM_OK))
    {
        t->bad_init++;
    }
    if ((db != SPI_BM_OK) || (dl != SPI_BM_OK))
    {
        t->bad_deinit++;
    }
    if ((pl[2] != e1) || (pl[3] != e2) || (pl[0] != ecr))
    {
        t->bad_rm++;
    }
    for (uint32_t r = 0U; r < NREG; r++)
    {
        if (pl[r] != pb[r])
        {
            if (t->bad_bm == 0U)
            {
                t->first_i = t->n;
                t->first_reg = r;
                t->got = pl[r];
                t->exp = pb[r];
            }
            t->bad_bm++;
            break;
        }
    }
    t->n++;
}

static void t2ll_inst(SPI_TypeDef *spi, const char *name)
{
    SpiBm_Caps_t caps = { 0U, 0U, 0U, 0U };
    (void)SPI_BM_GetCaps(spi, &caps);
    static const uint32_t bits_list[4] = { 4U, 8U, 16U, 32U };
    const uint32_t nb = (caps.max_data_bits == 32U) ? 4U : 3U;
    T2LlCount_t t = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    (void)SPI_BM_EnableClock(spi);
    (void)SPI_BM_Deinit(spi);
    for (uint32_t mbr = 0U; mbr <= 7U; mbr++)
    {
        for (uint32_t mode = 0U; mode <= 3U; mode++)
        {
            for (uint32_t ib = 0U; ib < nb; ib++)
            {
                const uint32_t bits = bits_list[ib];
                const uint32_t thr[2] = { 1U, thr_max(spi, bits) };
                for (uint32_t it = 0U; it < 2U; it++)
                {
                    for (uint32_t lsb = 0U; lsb < 2U; lsb++)
                    {
                        for (uint32_t dir = 0U; dir < 2U; dir++)
                        {
                            for (uint32_t nss = 0U; nss < 3U; nss++)
                            {
                                for (uint32_t op = 0U; op < 2U; op++)
                                {
                                    for (uint32_t mi = 0U; mi < 4U; mi++)
                                    {
                                        const uint32_t mssi = ((mi & 1U) != 0U) ? 15U : 0U;
                                        const uint32_t midi = ((mi & 2U) != 0U) ? 15U : 0U;
                                        if ((nss == SPI_BM_NSS_SOFT) && (mssi != 0U))
                                        {
                                            continue;
                                        }
                                        for (uint32_t keep = 0U; keep < 2U; keep++)
                                        {
                                            const SpiBm_Config_t c = { spi, mbr, mode, bits, lsb != 0U, dir, thr[it],
                                                                       nss, op != 0U, mssi, midi, keep != 0U };
                                            t2ll_one(&c, &t);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    /* The control: the same comparison, with one bit of the LL photo flipped (MASTER in
       CFG2), must report a difference. */
    const SpiBm_Config_t c0 = mkcfg(spi, 0U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    uint32_t pb[NREG];
    uint32_t pl[NREG];
    (void)SPI_BM_Init(&c0);
    spi_regs(spi, pb);
    (void)SPI_BM_Deinit(spi);
    SpiLl_Config_t l0;
    sst_ll_of(&c0, &l0);
    (void)SPI_LL_Init(&l0);
    spi_regs(spi, pl);
    (void)SPI_BM_Deinit(spi);
    pl[3] ^= SPI_CFG2_MASTER;
    const bool caught = memcmp(pb, pl, sizeof(pb)) != 0;
    if (t.bad_bm != 0U)
    {
        LOG("T2 LL %s first difference from spi_bm: config %lu register %lu LL 0x%08lX BM 0x%08lX", name,
            (unsigned long)t.first_i, (unsigned long)t.first_reg, (unsigned long)t.got, (unsigned long)t.exp);
    }
    verdict((t.bad_bm == 0U) && (t.bad_rm == 0U) && (t.bad_init == 0U) && (t.bad_deinit == 0U) && caught,
            "T2 LL %s %lu configurations: spi_ll image (CR1, CR2, CFG1, CFG2, IER, CRCPOLY, UDRDR, I2SCFGR) differs "
            "from spi_bm's in %lu, CFG1/CFG2/CR1 from the RM0399 table in %lu; Init failed %lu, Deinit failed %lu; "
            "control (MASTER flipped in the LL photo) caught %u", name, (unsigned long)t.n, (unsigned long)t.bad_bm,
            (unsigned long)t.bad_rm, (unsigned long)t.bad_init, (unsigned long)t.bad_deinit, (unsigned)caught);
}

/* ---- T15 (R3-3, audit M2): SPI6 through J1 with PA5 (SCK) in input --------------------- */

/* PA5 in input without pull, PB5 and PA6 in AF8 (S14: SPI6 at rest, read back). */
static bool spi6_pins_nosck(void)
{
    return pin_cfg(GPIOA, PA_J1_SCK, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U) &&
           pin_cfg(GPIOB, PB_J1_MISO, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_NONE, 0U) &&
           pin_cfg(GPIOB, PB_J1_MOSI, GPIO_BM_MODE_AF, SST_AF_SPI6, GPIO_BM_PULL_NONE, 0U) &&
           pin_cfg(GPIOA, PA_J1_MISO, GPIO_BM_MODE_AF, SST_AF_SPI6, GPIO_BM_PULL_NONE, 0U);
}

static void t15_one(uint32_t tier, bool sck_routed)
{
    s_tier = tier;
    const char *tn = (tier == TIER_LL) ? "LL" : "BM";
    const uint32_t mbr = 4U;                                     /* /32: 3.125 MHz */
    const uint32_t max = 400000U;                                /* 1 ms at 400 MHz */
    const SpiBm_Config_t c = mkcfg(SPI6, mbr, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    (void)SPI_BM_Deinit(SPI6);
    const bool pins = sck_routed ? spi6_pins(0U, 0U, GPIO_BM_SPEED_HIGH) : spi6_pins_nosck();
    if (!pins)
    {
        return;
    }
    uint8_t *tx = (uint8_t *)s_tx;
    uint8_t *rx = (uint8_t *)s_rx;
    tx[0] = 0xA5U;
    rx[0] = 0x00U;
    const SpiBm_Status_t si = x_init(&c);
    const uint32_t t0 = DWT->CYCCNT;
    const SpiBm_Status_t st = x_transfer(SPI6, tx, rx, 1U, guard_of(SPI6, mbr), max);
    const uint32_t dt = DWT->CYCCNT - t0;
    const uint32_t spe = SPI6->CR1 & SPI_CR1_SPE;
    const uint32_t ier = SPI6->IER;
    const uint32_t sr = SPI6->SR;
    const SpiBm_Status_t sd = SPI_BM_Deinit(SPI6);
    const uint32_t d = reset_diff(SPI6);
    s_tier = TIER_BM;
    if (!sck_routed)
    {
        verdict((si == SPI_BM_OK) && (st == SPI_BM_ERR_TIMEOUT) && (dt >= max) && (dt <= (max + (max / 10U))) &&
                (spe == 0U) && (ier == 0U) && (sd == SPI_BM_OK) && (d == 0U),
                "T15 %s SPI6, PA5 (SCK) in input, 1 frame: Transfer %u (ERR_TIMEOUT %u) after %lu cycles (max_cycles "
                "%lu .. +10 %%); then SPE %lu IER 0x%lX SR 0x%08lX; Deinit %u, registers off the reset image 0x%02lX (0)",
                tn, (unsigned)st, (unsigned)SPI_BM_ERR_TIMEOUT, (unsigned long)dt, (unsigned long)max,
                (unsigned long)(spe != 0U), (unsigned long)ier, (unsigned long)sr, (unsigned)sd, (unsigned long)d);
    }
    else
    {
        verdict((si == SPI_BM_OK) && (st == SPI_BM_OK) && (rx[0] == tx[0]) && (sd == SPI_BM_OK) && (d == 0U),
                "T15 %s control, PA5 in AF8: Transfer %u after %lu cycles, received 0x%02X through J1 (0x%02X); "
                "Deinit %u, off reset 0x%02lX", tn, (unsigned)st, (unsigned long)dt, (unsigned)rx[0], (unsigned)tx[0],
                (unsigned)sd, (unsigned long)d);
    }
}

/* ---- The F2 steps ------------------------------------------------------------------------ */

static void step_f2_nopin(void)
{
    SstArea_t *a = SST_AREA;
    if (!step_begin("F2_NOPIN"))
    {
        return;
    }
    f2_t1();
    t2ll_inst(SPI2, "SPI2");
    t2ll_inst(SPI3, "SPI3");
    t2ll_inst(SPI4, "SPI4");
    t2ll_inst(SPI5, "SPI5");
    t2ll_inst(SPI6, "SPI6");
    if (cm4_send(SST_CMD_T1, TIER_LL))
    {
        const volatile uint32_t *r = a->m4_res;
        verdict((r[1] == 0U) && (r[2] == 0xFU) && (r[3] == 1U),
                "T1 LL CM4 refusals through spi_ll on SPI1: %lu cases, %lu with another status (first %lu: %lu, "
                "expected %lu); registers and RCC banks unchanged in the groups 0x%lX (0xF); control changes them %lu",
                (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[4], (unsigned long)r[5],
                (unsigned long)r[6], (unsigned long)r[2], (unsigned long)r[3]);
    }
    if (cm4_send(SST_CMD_T2, TIER_LL))
    {
        const volatile uint32_t *r = a->m4_res;
        verdict((r[1] == 0U) && (r[2] == 0U) && (r[3] == 0U) && (r[4] == 1U) && (r[8] == 0U),
                "T2 LL CM4 SPI1 %lu configurations: spi_ll image differs from the RM0399 table in %lu, from spi_bm's "
                "in %lu, Init/Deinit failed %lu; control caught %lu (first difference: config %lu register %lu "
                "0x%08lX/0x%08lX)", (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[8],
                (unsigned long)(r[2] + r[3]), (unsigned long)r[4], (unsigned long)r[5], (unsigned long)r[9],
                (unsigned long)r[6], (unsigned long)r[7]);
    }
    SPI_TypeDef *const inst[5] = { SPI2, SPI3, SPI4, SPI5, SPI6 };
    const char *const names[5] = { "SPI2", "SPI3", "SPI4", "SPI5", "SPI6" };
    for (uint32_t i = 0U; i < 5U; i++)
    {
        SpiBm_Config_t cb = mkcfg(inst[i], 3U, 1U, 8U, true, SPI_BM_DIR_FULL_DUPLEX, 1U);
        SpiLl_Config_t l;
        sst_ll_of(&cb, &l);
        (void)SPI_LL_Init(&l);
        t14_check(names[i], inst[i], NULL);        /* after an LL Init: Deinit gives the reset image */
    }
    if (!sync0())
    {
        return;
    }
    step_end();
}

static void step_j1_spi6_ll(void)
{
    if (!step_begin("J1_SPI6_LL"))
    {
        return;
    }
    static const uint32_t bits6[7] = { 4U, 5U, 7U, 8U, 9U, 15U, 16U };
    static const uint32_t ctl6[3] = { 4U, 8U, 16U };
    (void)SPI_BM_EnableClock(SPI6);
    (void)SPI_BM_Deinit(SPI6);
    s_tier = TIER_LL;
    t4_matrix(SPI6, "SPI6/LL", false, bits6, 7U);
    if (!s_bench_abort && spi6_pins(0U, 0U, GPIO_BM_SPEED_HIGH))
    {
        uint32_t mbr = 0U;
        uint32_t sck = 0U;
        (void)SPI_BM_CalcPrescaler(s_k6, 12500000U, &mbr, &sck);
        const SpiBm_Config_t c = mkcfg(SPI6, mbr, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
        const uint32_t seed = next_seed();
        SstXferRes_t r;
        xfer_local(&c, F1_BIGF, seed, s_big_tx, s_big_rx, &r);
        t4_line("SPI6/LL", 0U, 8U, 0U, 1U, F1_BIGF, seed, &r);
        verdict(xfer_ok(&r), "T4 SPI6/LL 65535 frames of 8 bits: status %lu, %lu mismatches",
                (unsigned long)r.status, (unsigned long)r.mism);
    }
    if (!s_bench_abort)
    {
        t4_control(SPI6, "SPI6/LL", false, ctl6, 3U);
        (void)spi6_pins(0U, 0U, GPIO_BM_SPEED_HIGH);
    }
    if (!s_bench_abort)
    {
        s_b1_irq_min = 0xFFFFFFFFUL;
        s_b1_irq_max = 0U;
        t6_spi6("SPI6/LL");
        t7b_spi6("SPI6/LL");
    }
    if (!s_bench_abort && spi6_pins(0U, 0U, GPIO_BM_SPEED_HIGH))
    {
        f1_b1(SPI6, "SPI6/LL");
        LOG("B1 CM7 SPI6/LL TransferIrq per call (T6) min %lu max %lu cycles", (unsigned long)s_b1_irq_min,
            (unsigned long)s_b1_irq_max);
    }
    s_tier = TIER_BM;
    /* T15 with both tiers, the control after each (plan R3-3, audit M2) */
    if (!s_bench_abort)
    {
        t15_one(TIER_BM, false);
        t15_one(TIER_BM, true);
        t15_one(TIER_LL, false);
        t15_one(TIER_LL, true);
    }
    const SpiBm_Config_t cb = mkcfg(SPI6, 2U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    SpiLl_Config_t l;
    sst_ll_of(&cb, &l);
    (void)SPI_LL_Init(&l);
    const uint32_t dctrl = reset_diff(SPI6);
    t14_check("SPI6/LL", SPI6, NULL);
    verdict(dctrl != 0U, "T14 SPI6/LL control: after SPI_LL_Init, before Deinit, registers off reset 0x%02lX (!= 0)",
            (unsigned long)dctrl);
    const bool rel = j1_release();
    const uint32_t j1 = ((GPIOA->MODER >> (PA_J1_SCK * 2U)) & 0xFUL) | (((GPIOB->MODER >> (PB_J1_MISO * 2U)) & 0xFUL) << 4);
    verdict(rel && (j1 == 0U), "T14 J1 pins back in input: MODER PA5|PA6|PB4|PB5 0x%02lX (0)", (unsigned long)j1);
    if (!sync0())
    {
        return;
    }
    step_end();
}

static void step_j1_spi1_ll(void)
{
    SstArea_t *a = SST_AREA;
    if (!step_begin("J1_SPI1_LL"))
    {
        return;
    }
    static const uint32_t bits1[11] = { 4U, 5U, 7U, 8U, 9U, 15U, 16U, 17U, 24U, 31U, 32U };
    static const uint32_t ctl1[4] = { 4U, 8U, 16U, 32U };
    s_tier = TIER_LL;
    t4_matrix(SPI1, "SPI1/LL", true, bits1, 11U);
    if (!s_bench_abort)
    {
        t4_control(SPI1, "SPI1/LL", true, ctl1, 4U);
        (void)cm4_send4(SST_CMD_J1PINS, SST_J1P_LOOP, 0U, GPIO_BM_SPEED_HIGH, 0U);
    }
    uint32_t bad = 0U;
    for (uint32_t b = 0U; (b < 3U) && !s_bench_abort; b++)
    {
        const uint32_t bits = (b == 0U) ? 8U : ((b == 1U) ? 16U : 32U);
        const uint32_t frames[3] = { 1U, fifo_frames(SPI1, bits) + 1U, 1024U };
        for (uint32_t k = 0U; k < 3U; k++)
        {
            const uint32_t seed = next_seed();
            SstXferRes_t r;
            (void)xfer_m4(SST_XCFG(0U, bits, 0U, 1U, 5U, 0U), frames[k], seed, SST_XM_IT, &r);
            const uint32_t after = a->m4_res[9];
            LOG("T6 SPI1/LL b%lu n%lu s=%08lX st=%lu mis=%lu crc=%08lX irq=%lu after=%lu", (unsigned long)bits,
                (unsigned long)frames[k], (unsigned long)seed, (unsigned long)r.status, (unsigned long)r.mism,
                (unsigned long)r.crc, (unsigned long)r.irqs, (unsigned long)after);
            if (!xfer_ok(&r) || (after != 0U))
            {
                bad++;
            }
        }
    }
    verdict(bad == 0U, "T6 SPI1/LL by interrupt on CPU2 (8/16/32 bits, 1, FIFO + 1, 1024 frames): %lu failed, (c) "
            "IRQs in the 10 ms after each end 0", (unsigned long)bad);
    bad = 0U;
    for (uint32_t b = 0U; (b < 3U) && !s_bench_abort; b++)
    {
        const uint32_t bits = (b == 0U) ? 8U : ((b == 1U) ? 16U : 32U);
        const uint32_t frames[5] = { 1U, 2U, 17U, 255U, 4096U };
        for (uint32_t k = 0U; k < 5U; k++)
        {
            const uint32_t seed = next_seed();
            SstXferRes_t r;
            (void)xfer_m4(SST_XCFG(0U, bits, 0U, 1U, 3U, 0U), frames[k], seed, SST_XM_DMA, &r);
            LOG("T7A SPI1/LL b%lu n%lu s=%08lX st=%lu mis=%lu crc=%08lX", (unsigned long)bits,
                (unsigned long)frames[k], (unsigned long)seed, (unsigned long)r.status, (unsigned long)r.mism,
                (unsigned long)r.crc);
            if (!xfer_ok(&r))
            {
                bad++;
            }
        }
    }
    SstXferRes_t rc = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    (void)xfer_m4(SST_XCFG(0U, 8U, 0U, 1U, 3U, 0U), 255U, next_seed(), SST_XM_DMA_NORX, &rc);
    verdict(bad == 0U, "T7(a) SPI1/LL by DMA2 (CPU2, 8/16/32 bits, 1..4096 frames): %lu failed", (unsigned long)bad);
    verdict((rc.status == (uint32_t)SPI_BM_ERR_OVERRUN) && (rc.sentinel_left == 255U),
            "T7(a) SPI1/LL control RXDMAEN = 0, 255 frames: WaitEnd %lu (ERR_OVERRUN %u), RX words with the sentinel "
            "%lu (255)", (unsigned long)rc.status, (unsigned)SPI_BM_ERR_OVERRUN, (unsigned long)rc.sentinel_left);
    if (!s_bench_abort && cm4_send(SST_CMD_B1, TIER_LL))
    {
        LOG("B1 CM4 SPI1/LL SPI_LL_Init %lu cycles, SPI_LL_Transfer of 1 frame at /2 %lu cycles, TransferIrq per "
            "call min %lu max %lu", (unsigned long)a->m4_res[0], (unsigned long)a->m4_res[1],
            (unsigned long)a->m4_res[2], (unsigned long)a->m4_res[3]);
    }
    s_tier = TIER_BM;
    if (cm4_send(SST_CMD_T14, 0U))
    {
        verdict((a->m4_res[0] == (uint32_t)SPI_BM_OK) && (a->m4_res[1] == 0U),
                "T14 SPI1/LL (CPU2) Deinit %lu, registers off the RM0399 reset image 0x%02lX (0)",
                (unsigned long)a->m4_res[0], (unsigned long)a->m4_res[1]);
    }
    if (cm4_send4(SST_CMD_J1PINS, SST_J1P_RELEASE, 0U, GPIO_BM_SPEED_LOW, 0U))
    {
        verdict(a->m4_res[0] == 1U, "T14 CPU2's J1 pins back in input: MODER PA5|PA6|PB4|PB5 0x%02lX (0)",
                (unsigned long)a->m4_res[1]);
    }
    if (!sync0())
    {
        return;
    }
    step_end();
}

/* T5(a) through spi_ll: the same 32 captures, kind 'A' for the host. */
static void step_spi4_ad2_ll(void)
{
    if (!step_begin("SPI4_AD2_LL") || !buschk())
    {
        return;
    }
    const bool clk_was_on = spi4_clocked();
    (void)SPI_BM_EnableClock(SPI4);
    (void)SPI_BM_Deinit(SPI4);
    if (!spi4_pins(0U, false, GPIO_BM_PULL_DOWN, 0xFFU) || !buschk_af(PE_SPI4_MASK, NULL, NULL))
    {
        return;
    }
    s_cap_k = 0U;
    s_cap_bad = 0U;
    s_tier = TIER_LL;
    static const uint32_t ab[4] = { 4U, 8U, 13U, 16U };
    for (uint32_t mode = 0U; mode <= 3U; mode++)
    {
        for (uint32_t ib = 0U; ib < 4U; ib++)
        {
            for (uint32_t lsb = 0U; lsb < 2U; lsb++)
            {
                const SpiBm_Config_t c = { SPI4, 3U, mode, ab[ib], lsb != 0U, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW,
                                           false, 0U, 0U, false };
                cap_one('A', &c, cap_frames(ab[ib], 3U, 0U, 0U, 32U), cap_poll);
            }
        }
    }
    s_tier = TIER_BM;
    LOG("CAP done k%lu", (unsigned long)s_cap_k);
    verdict(s_cap_bad == 0U, "T5(a) SPI4/LL: %lu captures run, %lu with a failed Init, transfer or Deinit",
            (unsigned long)s_cap_k, (unsigned long)s_cap_bad);
    const SpiBm_Config_t ctrl = { SPI4, 3U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW, false, 0U, 0U, false };
    t14_check("SPI4/LL", SPI4, &ctrl);
    if (!spi4_pins(0U, false, GPIO_BM_PULL_DOWN, 0xFFU) || !buschk_af(PE_SPI4_MASK, NULL, NULL))
    {
        return;
    }
    (void)pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
    if (!buschk())
    {
        return;
    }
    if (!clk_was_on)
    {
        (void)SPI_BM_DisableClock(SPI4);
    }
    step_end();
}

/* ---- F3 handlers: the HAL's routines, only while a HAL transfer is in flight -------- */
void SPI4_IRQHandler(void)
{
    if (s_hal_busy)
    {
        HAL_SPI_IRQHandler(&s_h4);
        if (s_h4.State == HAL_SPI_STATE_READY)
        {
            s_hal_busy = false;
        }
    }
    else
    {
        SPI4->IER = 0U;
    }
}

void DMA1_Stream0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hd1);
}

void BDMA_Channel0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hbd_rx);
}

void BDMA_Channel1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hbd_tx);
}

/* A BDMA channel for the HAL's DMA path: as bd_init(), with its NVIC line enabled
   (dma_hal: irq_priority >= 0). */
static bool bd_init_irq(DMA_HandleTypeDef *h, BDMA_Channel_TypeDef *ch, uint32_t req, uint32_t dir, uint32_t es)
{
    const DmaHal_Config_t c = {
        .instance = ch, .request = req, .direction = dir, .periph_inc = DMA_PINC_DISABLE, .mem_inc = DMA_MINC_ENABLE,
        .periph_align = (es == 1U) ? DMA_PDATAALIGN_BYTE : DMA_PDATAALIGN_HALFWORD,
        .mem_align = (es == 1U) ? DMA_MDATAALIGN_BYTE : DMA_MDATAALIGN_HALFWORD, .mode = DMA_NORMAL,
        .priority = DMA_PRIORITY_HIGH, .fifo_mode = DMA_FIFOMODE_DISABLE, .fifo_threshold = DMA_FIFO_THRESHOLD_FULL,
        .mem_burst = DMA_MBURST_SINGLE, .periph_burst = DMA_PBURST_SINGLE, .enable_clock = true, .irq_priority = 6 };
    return DMA_HAL_Init(h, &c) == HAL_OK;
}

static void nvic_spi6(bool on)
{
    if (on)
    {
        HAL_NVIC_SetPriority(SPI6_IRQn, 6U, 0U);
        NVIC_ClearPendingIRQ(SPI6_IRQn);
        HAL_NVIC_EnableIRQ(SPI6_IRQn);
    }
    else
    {
        HAL_NVIC_DisableIRQ(SPI6_IRQn);
        NVIC_ClearPendingIRQ(SPI6_IRQn);
    }
}

/* ==========================================================================================
 * Phase F3: spi_hal on CPU1 (plan revision 3: T1, T2 (with the raw HAL image, R3-4), T4
 * (SPI6), T5(a) (SPI4), T6, T7(b) BDMA and (c) DMA1, T14, B1). The tests of F1 run again
 * with s_tier = TIER_HAL; the HAL configuration of the raw-HAL image of T2 is the test's
 * own (sst_hal_of(), HAL constants written by hand). HAL_GetTick() is printed before,
 * during and after each HAL step (TICK lines).
 * ========================================================================================== */

static void tick_line(const char *where)
{
    LOG("TICK %s %lu", where, (unsigned long)HAL_GetTick());
}

/* ---- The test's own SpiBm_Config_t -> SPI_InitTypeDef, for the raw-HAL image ----------- */
static const uint32_t k_hal_size[29] = {
    SPI_DATASIZE_4BIT,  SPI_DATASIZE_5BIT,  SPI_DATASIZE_6BIT,  SPI_DATASIZE_7BIT,  SPI_DATASIZE_8BIT,
    SPI_DATASIZE_9BIT,  SPI_DATASIZE_10BIT, SPI_DATASIZE_11BIT, SPI_DATASIZE_12BIT, SPI_DATASIZE_13BIT,
    SPI_DATASIZE_14BIT, SPI_DATASIZE_15BIT, SPI_DATASIZE_16BIT, SPI_DATASIZE_17BIT, SPI_DATASIZE_18BIT,
    SPI_DATASIZE_19BIT, SPI_DATASIZE_20BIT, SPI_DATASIZE_21BIT, SPI_DATASIZE_22BIT, SPI_DATASIZE_23BIT,
    SPI_DATASIZE_24BIT, SPI_DATASIZE_25BIT, SPI_DATASIZE_26BIT, SPI_DATASIZE_27BIT, SPI_DATASIZE_28BIT,
    SPI_DATASIZE_29BIT, SPI_DATASIZE_30BIT, SPI_DATASIZE_31BIT, SPI_DATASIZE_32BIT };
static const uint32_t k_hal_fifo[16] = {
    SPI_FIFO_THRESHOLD_01DATA, SPI_FIFO_THRESHOLD_02DATA, SPI_FIFO_THRESHOLD_03DATA, SPI_FIFO_THRESHOLD_04DATA,
    SPI_FIFO_THRESHOLD_05DATA, SPI_FIFO_THRESHOLD_06DATA, SPI_FIFO_THRESHOLD_07DATA, SPI_FIFO_THRESHOLD_08DATA,
    SPI_FIFO_THRESHOLD_09DATA, SPI_FIFO_THRESHOLD_10DATA, SPI_FIFO_THRESHOLD_11DATA, SPI_FIFO_THRESHOLD_12DATA,
    SPI_FIFO_THRESHOLD_13DATA, SPI_FIFO_THRESHOLD_14DATA, SPI_FIFO_THRESHOLD_15DATA, SPI_FIFO_THRESHOLD_16DATA };
static const uint32_t k_hal_brp[8] = {
    SPI_BAUDRATEPRESCALER_2, SPI_BAUDRATEPRESCALER_4, SPI_BAUDRATEPRESCALER_8, SPI_BAUDRATEPRESCALER_16,
    SPI_BAUDRATEPRESCALER_32, SPI_BAUDRATEPRESCALER_64, SPI_BAUDRATEPRESCALER_128, SPI_BAUDRATEPRESCALER_256 };
static const uint32_t k_hal_mssi[16] = {
    SPI_MASTER_SS_IDLENESS_00CYCLE, SPI_MASTER_SS_IDLENESS_01CYCLE, SPI_MASTER_SS_IDLENESS_02CYCLE,
    SPI_MASTER_SS_IDLENESS_03CYCLE, SPI_MASTER_SS_IDLENESS_04CYCLE, SPI_MASTER_SS_IDLENESS_05CYCLE,
    SPI_MASTER_SS_IDLENESS_06CYCLE, SPI_MASTER_SS_IDLENESS_07CYCLE, SPI_MASTER_SS_IDLENESS_08CYCLE,
    SPI_MASTER_SS_IDLENESS_09CYCLE, SPI_MASTER_SS_IDLENESS_10CYCLE, SPI_MASTER_SS_IDLENESS_11CYCLE,
    SPI_MASTER_SS_IDLENESS_12CYCLE, SPI_MASTER_SS_IDLENESS_13CYCLE, SPI_MASTER_SS_IDLENESS_14CYCLE,
    SPI_MASTER_SS_IDLENESS_15CYCLE };
static const uint32_t k_hal_midi[16] = {
    SPI_MASTER_INTERDATA_IDLENESS_00CYCLE, SPI_MASTER_INTERDATA_IDLENESS_01CYCLE, SPI_MASTER_INTERDATA_IDLENESS_02CYCLE,
    SPI_MASTER_INTERDATA_IDLENESS_03CYCLE, SPI_MASTER_INTERDATA_IDLENESS_04CYCLE, SPI_MASTER_INTERDATA_IDLENESS_05CYCLE,
    SPI_MASTER_INTERDATA_IDLENESS_06CYCLE, SPI_MASTER_INTERDATA_IDLENESS_07CYCLE, SPI_MASTER_INTERDATA_IDLENESS_08CYCLE,
    SPI_MASTER_INTERDATA_IDLENESS_09CYCLE, SPI_MASTER_INTERDATA_IDLENESS_10CYCLE, SPI_MASTER_INTERDATA_IDLENESS_11CYCLE,
    SPI_MASTER_INTERDATA_IDLENESS_12CYCLE, SPI_MASTER_INTERDATA_IDLENESS_13CYCLE, SPI_MASTER_INTERDATA_IDLENESS_14CYCLE,
    SPI_MASTER_INTERDATA_IDLENESS_15CYCLE };

static void sst_hal_of(const SpiBm_Config_t *b, SPI_InitTypeDef *in)
{
    in->Mode                       = SPI_MODE_MASTER;
    in->Direction                  = (b->direction == SPI_BM_DIR_TX_ONLY) ? SPI_DIRECTION_2LINES_TXONLY : SPI_DIRECTION_2LINES;
    in->DataSize                   = k_hal_size[(b->data_bits - 4U) % 29U];
    in->CLKPolarity                = ((b->mode & 2U) != 0U) ? SPI_POLARITY_HIGH : SPI_POLARITY_LOW;
    in->CLKPhase                   = ((b->mode & 1U) != 0U) ? SPI_PHASE_2EDGE : SPI_PHASE_1EDGE;
    in->NSS                        = (b->nss == SPI_BM_NSS_SOFT) ? SPI_NSS_SOFT : SPI_NSS_HARD_OUTPUT;
    in->BaudRatePrescaler          = k_hal_brp[b->mbr & 7U];
    in->FirstBit                   = b->lsb_first ? SPI_FIRSTBIT_LSB : SPI_FIRSTBIT_MSB;
    in->TIMode                     = SPI_TIMODE_DISABLE;
    in->CRCCalculation             = SPI_CRCCALCULATION_DISABLE;
    in->CRCPolynomial              = 0x7U;
    in->CRCLength                  = SPI_CRC_LENGTH_DATASIZE;
    in->NSSPMode                   = (b->nss == SPI_BM_NSS_HW_PULSE) ? SPI_NSS_PULSE_ENABLE : SPI_NSS_PULSE_DISABLE;
    in->NSSPolarity                = b->nss_active_high ? SPI_NSS_POLARITY_HIGH : SPI_NSS_POLARITY_LOW;
    in->FifoThreshold              = k_hal_fifo[(b->fifo_threshold - 1U) & 15U];
    in->TxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
    in->RxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
    in->MasterSSIdleness           = k_hal_mssi[b->mssi & 15U];
    in->MasterInterDataIdleness    = k_hal_midi[b->midi & 15U];
    in->MasterReceiverAutoSusp     = SPI_MASTER_RX_AUTOSUSP_DISABLE;
    in->MasterKeepIOState          = b->keep_pins ? SPI_MASTER_KEEP_IO_STATE_ENABLE : SPI_MASTER_KEEP_IO_STATE_DISABLE;
    in->IOSwap                     = SPI_IO_SWAP_DISABLE;
}

/* ---- T1 with spi_hal (CPU1: SPI2, SPI4, SPI6) ------------------------------------------ */
static DMA_HandleTypeDef s_hd_dummy;     /* a linked, never started DMA handle (T1 only) */

static void f3_t1(void)
{
    SPI_TypeDef *const inst[4] = { SPI2, SPI4, SPI5, SPI6 };
    for (uint32_t i = 0U; i < 4U; i++)
    {
        (void)SPI_BM_EnableClock(inst[i]);
        (void)SPI_BM_Deinit(inst[i]);
    }
    uint32_t a[T1_SNAP];
    uint32_t b[T1_SNAP];
    uint32_t same = 0U;
    const SpiBm_Config_t c = mkcfg(SPI2, 7U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    SpiBm_Config_t k;
    SPI_HandleTypeDef *h = &s_h2;
    s_t1_n = 0U;

    /* 1. Nothing initialised: the handle, the instance, the configuration, the calls. */
    t1_snap(a);
    h->Instance = SPI2;
    t1(SPI_HAL_Init(NULL, &c), SPI_BM_ERR_PARAM);
    t1(SPI_HAL_Init(h, NULL), SPI_BM_ERR_PARAM);
    h->Instance = (SPI_TypeDef *)USART3;                      t1(SPI_HAL_Init(h, &c), SPI_BM_ERR_PARAM);
    h->Instance = SPI4;                                       t1(SPI_HAL_Init(h, &c), SPI_BM_ERR_PARAM);  /* != cfg->spi */
    h->Instance = SPI2;
    k = c; k.data_bits = 3U;                                  t1(SPI_HAL_Init(h, &k), SPI_BM_ERR_PARAM);
    k = c; k.mode = 4U;                                       t1(SPI_HAL_Init(h, &k), SPI_BM_ERR_PARAM);
    k = c; k.mssi = 1U;                                       t1(SPI_HAL_Init(h, &k), SPI_BM_ERR_PARAM);
    k = c; k.data_bits = 32U; k.fifo_threshold = 3U;          t1(SPI_HAL_Init(h, &k), SPI_BM_ERR_RANGE);
    s_h6.Instance = SPI6;
    k = c; k.spi = SPI6; k.data_bits = 17U;                   t1(SPI_HAL_Init(&s_h6, &k), SPI_BM_ERR_UNSUPPORTED);
    s_h4.Instance = SPI4;
    k = c; k.spi = SPI4; k.data_bits = 16U; k.fifo_threshold = 3U;
                                                              t1(SPI_HAL_Init(&s_h4, &k), SPI_BM_ERR_RANGE);
    t1(SPI_HAL_Deinit(NULL), SPI_BM_ERR_PARAM);
    h->Instance = (SPI_TypeDef *)USART3;                      t1(SPI_HAL_Deinit(h), SPI_BM_ERR_PARAM);
    h->Instance = SPI2;
    t1(SPI_HAL_Wait(NULL, 10U), SPI_BM_ERR_PARAM);
    t1(SPI_HAL_Wait(h, 0U), SPI_BM_ERR_PARAM);
    t1(SPI_HAL_Transfer(NULL, s_tx, s_rx, 1U, 10U), SPI_BM_ERR_PARAM);
    t1(SPI_HAL_StartTransferIt(NULL, s_tx, s_rx, 1U), SPI_BM_ERR_PARAM);
    h->hdmatx = NULL;
    h->hdmarx = NULL;
    t1(SPI_HAL_StartTransferDma(h, s_tx, NULL, 1U), SPI_BM_ERR_PARAM);                     /* no hdmatx */
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 1U : 0U;

    /* 2. SPI2 transmit only through spi_hal, SPI5 16-bit: the transfer's parameters. */
    k = c; k.direction = SPI_BM_DIR_TX_ONLY;
    const SpiBm_Status_t i2 = SPI_HAL_Init(h, &k);
    s_h5.Instance = SPI5;
    k = c; k.spi = SPI5; k.data_bits = 16U;
    const SpiBm_Status_t i5 = SPI_HAL_Init(&s_h5, &k);
    t1_snap(a);
    t1(SPI_HAL_Transfer(h, s_tx, NULL, 1U, 0U), SPI_BM_ERR_PARAM);                         /* timeout 0 */
    t1(SPI_HAL_Transfer(h, s_tx, NULL, 0U, 10U), SPI_BM_ERR_PARAM);
    t1(SPI_HAL_Transfer(h, s_tx, NULL, 65536U, 10U), SPI_BM_ERR_PARAM);
    t1(SPI_HAL_Transfer(h, NULL, NULL, 1U, 10U), SPI_BM_ERR_PARAM);
    t1(SPI_HAL_Transfer(h, s_tx, s_rx, 1U, 10U), SPI_BM_ERR_PARAM);                        /* rx in TX_ONLY */
    t1(SPI_HAL_Transfer(&s_h5, (const uint8_t *)s_tx + 1U, NULL, 1U, 10U), SPI_BM_ERR_PARAM);   /* odd */
    t1(SPI_HAL_StartTransferIt(h, s_tx, NULL, 0U), SPI_BM_ERR_PARAM);
    h->hdmatx = &s_hd_dummy;
    t1(SPI_HAL_StartTransferDma(h, s_tx, s_rx, 1U), SPI_BM_ERR_PARAM);                     /* rx, no hdmarx */
    s_hd_dummy.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    s_h5.hdmatx = &s_hd_dummy;
    s_h5.hdmarx = NULL;
    t1(SPI_HAL_StartTransferDma(&s_h5, s_tx, NULL, 1U), SPI_BM_ERR_PARAM);  /* byte memory size, 16-bit frames */
    const bool h5_ready = s_h5.State == HAL_SPI_STATE_READY;                /* the HAL's refusal would leave it busy */
    s_h5.hdmatx = NULL;
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 2U : 0U;
    (void)SPI_HAL_Deinit(&s_h5);

    /* 3. SPE = 1 by hand, and the handle busy: refused before any HAL call. */
    k = c;
    const SpiBm_Status_t i3 = SPI_HAL_Init(h, &k);
    SPI2->CR1 |= SPI_CR1_SPE;
    const uint32_t spe = SPI2->CR1 & SPI_CR1_SPE;
    t1_snap(a);
    t1(SPI_HAL_Init(h, &k), SPI_BM_ERR_BUSY);
    t1(SPI_HAL_Transfer(h, s_tx, s_rx, 1U, 10U), SPI_BM_ERR_BUSY);
    t1(SPI_HAL_StartTransferIt(h, s_tx, s_rx, 1U), SPI_BM_ERR_BUSY);
    h->hdmarx = &s_hd_dummy;
    t1(SPI_HAL_StartTransferDma(h, s_tx, s_rx, 1U), SPI_BM_ERR_BUSY);
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 4U : 0U;
    SPI2->CR1 &= ~SPI_CR1_SPE;
    h->State = HAL_SPI_STATE_BUSY_TX_RX;                      /* the handle says busy, the SPI is at rest */
    t1(SPI_HAL_Transfer(h, s_tx, s_rx, 1U, 10U), SPI_BM_ERR_BUSY);
    h->State = HAL_SPI_STATE_READY;
    h->hdmatx = NULL;
    h->hdmarx = NULL;

    /* 4. AFCNTR = 1 in force: another CPOL or SSIOP. */
    k = c; k.keep_pins = true;
    const SpiBm_Status_t i4 = SPI_HAL_Init(h, &k);
    t1_snap(a);
    k.mode = 2U;                                     t1(SPI_HAL_Init(h, &k), SPI_BM_ERR_BUSY);
    k.mode = 0U; k.nss_active_high = true;           t1(SPI_HAL_Init(h, &k), SPI_BM_ERR_BUSY);
    t1_snap(b);
    same |= (memcmp(a, b, sizeof(a)) == 0) ? 8U : 0U;
    const SpiBm_Status_t d4 = SPI_HAL_Deinit(h);
    const bool reset_state = (h->State == HAL_SPI_STATE_RESET) && (reset_diff(SPI2) == 0U);

    /* The control: a valid SPI_HAL_Init changes the snapshot. */
    t1_snap(a);
    k = c;
    const SpiBm_Status_t w = SPI_HAL_Init(h, &k);
    t1_snap(b);
    const bool ctrl = memcmp(a, b, sizeof(a)) != 0;
    (void)SPI_HAL_Deinit(h);

    uint32_t bad = 0U;
    for (uint32_t i = 0U; (i < s_t1_n) && (i < T1_MAX); i++)
    {
        if (s_t1_got[i] != s_t1_exp[i])
        {
            bad++;
            LOG("T1 HAL CM7 case %lu: status %u, expected %u", (unsigned long)i, (unsigned)s_t1_got[i],
                (unsigned)s_t1_exp[i]);
        }
    }
    verdict((bad == 0U) && (s_t1_n <= T1_MAX) && (same == 0xFU) && (spe != 0U) && (i2 == SPI_BM_OK) &&
            (i5 == SPI_BM_OK) && (i3 == SPI_BM_OK) && (i4 == SPI_BM_OK) && h5_ready,
            "T1 HAL CM7 refusals through spi_hal on SPI2, SPI4, SPI5, SPI6: %lu cases, %lu with another status; "
            "registers of the four and RCC_C1/C2 APB1LENR/APB2ENR/APB4ENR unchanged in the 4 groups: 0x%lX (0xF); "
            "SPE set by hand %u; SPI5 handle READY after the DMA size refusal %u", (unsigned long)s_t1_n,
            (unsigned long)bad, (unsigned long)same, (unsigned)(spe != 0U), (unsigned)h5_ready);
    verdict(ctrl && (w == SPI_BM_OK), "T1 HAL CM7 control: a valid SPI_HAL_Init(SPI2) changes the snapshot (%u)",
            (unsigned)ctrl);
    verdict((d4 == SPI_BM_OK) && reset_state, "T14 HAL SPI_HAL_Deinit(SPI2) after a keep_pins Init: status %u, "
            "handle in RESET and registers at the reset image %u", (unsigned)d4, (unsigned)reset_state);
}

/* ---- T2 with spi_hal: the image of SPI_HAL_Init against the raw HAL_SPI_Init (bit for
   bit), against spi_bm (bit for bit but for the differences by design) and against the
   RM table. The one difference by design found in the HAL source: CR1.SSI with hardware
   SS (stm32h7xx_hal_spi.c l. 375-381 set SSI only for a soft SS active low). ---------- */
typedef struct { uint32_t n, bad_raw, bad_bm, design, bad_rm, bad_st, first_i, first_reg, got, exp; } T2HalCount_t;

static void t2hal_one(const SpiBm_Config_t *c, T2HalCount_t *t)
{
    SPI_TypeDef *spi = c->spi;
    SPI_HandleTypeDef *h = hal_h(spi);
    uint32_t pb[NREG];
    uint32_t ph[NREG];
    uint32_t pr[NREG];
    const SpiBm_Status_t sb = SPI_BM_Init(c);
    spi_regs(spi, pb);
    (void)SPI_BM_Deinit(spi);
    h->Instance = spi;
    const SpiBm_Status_t sh = SPI_HAL_Init(h, c);
    spi_regs(spi, ph);
    const SpiBm_Status_t dh = SPI_HAL_Deinit(h);
    /* the raw HAL: the reset image, then HAL_SPI_Init with no wrapper */
    (void)SPI_BM_Deinit(spi);
    h->State = HAL_SPI_STATE_RESET;
    sst_hal_of(c, &h->Init);
    const HAL_StatusTypeDef hr = HAL_SPI_Init(h);
    spi_regs(spi, pr);
    (void)SPI_HAL_Deinit(h);
    if ((sb != SPI_BM_OK) || (sh != SPI_BM_OK) || (dh != SPI_BM_OK) || (hr != HAL_OK))
    {
        t->bad_st++;
    }
    if (memcmp(ph, pr, sizeof(ph)) != 0)
    {
        t->bad_raw++;
    }
    /* spi_bm writes CR1.SSI inactive with any SS; the HAL only with a soft SS */
    const uint32_t design = ((c->nss != SPI_BM_NSS_SOFT) && !c->nss_active_high) ? SPI_CR1_SSI : 0U;
    if (((ph[0] ^ pb[0]) == design) && (design != 0U))
    {
        t->design++;
    }
    for (uint32_t r = 0U; r < NREG; r++)
    {
        const uint32_t allowed = (r == 0U) ? design : 0U;
        if ((ph[r] ^ pb[r]) != allowed)
        {
            if (t->bad_bm == 0U)
            {
                t->first_i = t->n;
                t->first_reg = r;
                t->got = ph[r];
                t->exp = pb[r];
            }
            t->bad_bm++;
            break;
        }
    }
    uint32_t e1 = 0U;
    uint32_t e2 = 0U;
    uint32_t ecr = 0U;
    sst_t2_expect(c, &e1, &e2, &ecr);
    if ((ph[2] != e1) || (ph[3] != e2) || (ph[0] != (ecr ^ design)))
    {
        t->bad_rm++;
    }
    t->n++;
}

static void t2hal_inst(SPI_TypeDef *spi, const char *name)
{
    SpiBm_Caps_t caps = { 0U, 0U, 0U, 0U };
    (void)SPI_BM_GetCaps(spi, &caps);
    static const uint32_t bits_list[4] = { 4U, 8U, 16U, 32U };
    const uint32_t nb = (caps.max_data_bits == 32U) ? 4U : 3U;
    T2HalCount_t t = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    (void)SPI_BM_EnableClock(spi);
    (void)SPI_BM_Deinit(spi);
    for (uint32_t mbr = 0U; mbr <= 7U; mbr++)
    {
        for (uint32_t mode = 0U; mode <= 3U; mode++)
        {
            for (uint32_t ib = 0U; ib < nb; ib++)
            {
                const uint32_t bits = bits_list[ib];
                const uint32_t thr[2] = { 1U, thr_max(spi, bits) };
                for (uint32_t it = 0U; it < 2U; it++)
                {
                    for (uint32_t lsb = 0U; lsb < 2U; lsb++)
                    {
                        for (uint32_t dir = 0U; dir < 2U; dir++)
                        {
                            for (uint32_t nss = 0U; nss < 3U; nss++)
                            {
                                for (uint32_t op = 0U; op < 2U; op++)
                                {
                                    for (uint32_t mi = 0U; mi < 4U; mi++)
                                    {
                                        const uint32_t mssi = ((mi & 1U) != 0U) ? 15U : 0U;
                                        const uint32_t midi = ((mi & 2U) != 0U) ? 15U : 0U;
                                        if ((nss == SPI_BM_NSS_SOFT) && (mssi != 0U))
                                        {
                                            continue;
                                        }
                                        for (uint32_t keep = 0U; keep < 2U; keep++)
                                        {
                                            const SpiBm_Config_t c = { spi, mbr, mode, bits, lsb != 0U, dir, thr[it],
                                                                       nss, op != 0U, mssi, midi, keep != 0U };
                                            t2hal_one(&c, &t);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    /* The control: one bit flipped in the spi_hal photo (MASTER in CFG2) is caught by the
       same comparison with the raw HAL image. */
    const SpiBm_Config_t c0 = mkcfg(spi, 0U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    SPI_HandleTypeDef *h = hal_h(spi);
    uint32_t ph[NREG];
    uint32_t pr[NREG];
    h->Instance = spi;
    (void)SPI_HAL_Init(h, &c0);
    spi_regs(spi, ph);
    (void)SPI_HAL_Deinit(h);
    h->State = HAL_SPI_STATE_RESET;
    sst_hal_of(&c0, &h->Init);
    (void)HAL_SPI_Init(h);
    spi_regs(spi, pr);
    (void)SPI_HAL_Deinit(h);
    ph[3] ^= SPI_CFG2_MASTER;
    const bool caught = memcmp(ph, pr, sizeof(ph)) != 0;
    if (t.bad_bm != 0U)
    {
        LOG("T2 HAL %s first difference from spi_bm outside the design: config %lu register %lu HAL 0x%08lX BM 0x%08lX",
            name, (unsigned long)t.first_i, (unsigned long)t.first_reg, (unsigned long)t.got, (unsigned long)t.exp);
    }
    verdict((t.bad_raw == 0U) && (t.bad_bm == 0U) && (t.bad_rm == 0U) && (t.bad_st == 0U) && caught,
            "T2 HAL %s %lu configurations: SPI_HAL_Init image differs from the raw HAL_SPI_Init image in %lu, from "
            "spi_bm's outside the differences by design in %lu (CR1.SSI with hardware SS, active low: %lu "
            "configurations), CFG1/CFG2/CR1 from the RM0399 table in %lu; a status other than OK %lu; control (MASTER "
            "flipped in the spi_hal photo) caught %u", name, (unsigned long)t.n, (unsigned long)t.bad_raw,
            (unsigned long)t.bad_bm, (unsigned long)t.design, (unsigned long)t.bad_rm, (unsigned long)t.bad_st,
            (unsigned)caught);
}

/* ---- T7(b) with spi_hal: SPI6 by BDMA (dma_hal), the HAL's DMA path ------------------- */
static SpiBm_Status_t t7b_one_hal(uint32_t bits, uint32_t n, uint32_t seed, bool rx, uint32_t *mism,
                                  uint32_t *left)
{
    const uint32_t es = sst_elem_bytes(bits);
    const uint32_t mbr = 2U;
    const SpiBm_Config_t c = mkcfg(SPI6, mbr, 0U, bits, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    sst_fill(s_bd_tx, es, n, bits, seed);
    sst_fill_const(s_bd_rx, es, n, 0xA5A5A5A5UL);
    s_h6.Instance = SPI6;
    const SpiBm_Status_t si = SPI_HAL_Init(&s_h6, &c);
    const bool d_ok = bd_init_irq(&s_hbd_rx, BDMA_Channel0, BDMA_REQUEST_SPI6_RX, DMA_PERIPH_TO_MEMORY, es) &&
                      bd_init_irq(&s_hbd_tx, BDMA_Channel1, BDMA_REQUEST_SPI6_TX, DMA_MEMORY_TO_PERIPH, es);
    __HAL_LINKDMA(&s_h6, hdmatx, s_hbd_tx);
    __HAL_LINKDMA(&s_h6, hdmarx, s_hbd_rx);
    nvic_spi6(true);
    s_hal_busy = true;
    SpiBm_Status_t st = SPI_HAL_StartTransferDma(&s_h6, s_bd_tx, rx ? s_bd_rx : NULL, n);
    if (st != SPI_BM_OK)
    {
        s_hal_busy = false;
    }
    else
    {
        st = SPI_HAL_Wait(&s_h6, ms_of(budget_of(SPI6, mbr, bits, n)));
    }
    s_hal_busy = false;
    nvic_spi6(false);
    (void)DMA_HAL_DeInit(&s_hbd_tx);
    (void)DMA_HAL_DeInit(&s_hbd_rx);
    s_h6.hdmatx = NULL;
    s_h6.hdmarx = NULL;
    const SpiBm_Status_t sd = SPI_HAL_Deinit(&s_h6);
    *mism = sst_mismatch(s_bd_tx, s_bd_rx, es, n, bits);
    uint32_t l = 0U;
    for (uint32_t i = 0U; i < n; i++)
    {
        if (sst_get(s_bd_rx, es, i) == (0xA5A5A5A5UL & sst_mask(es * 8U)))
        {
            l++;
        }
    }
    *left = l;
    if ((si != SPI_BM_OK) || !d_ok || (sd != SPI_BM_OK))
    {
        return SPI_BM_ERR_HAL;
    }
    return st;
}

static void t7b_hal(void)
{
    uint32_t bad = 0U;
    for (uint32_t b = 0U; b < 2U; b++)
    {
        const uint32_t bits = (b == 0U) ? 8U : 16U;
        const uint32_t frames[4] = { 1U, 9U, 255U, BD_MAXF };
        for (uint32_t k = 0U; k < 4U; k++)
        {
            const uint32_t seed = next_seed();
            uint32_t mism = 0U;
            uint32_t left = 0U;
            const SpiBm_Status_t st = t7b_one_hal(bits, frames[k], seed, true, &mism, &left);
            const uint32_t crc = sst_crc32_frames(s_bd_rx, sst_elem_bytes(bits), frames[k], bits);
            LOG("T7B SPI6/HAL b%lu n%lu s=%08lX st=%u mis=%lu crc=%08lX", (unsigned long)bits,
                (unsigned long)frames[k], (unsigned long)seed, (unsigned)st, (unsigned long)mism, (unsigned long)crc);
            if ((st != SPI_BM_OK) || (mism != 0U))
            {
                bad++;
            }
        }
    }
    uint32_t mism = 0U;
    uint32_t left = 0U;
    const SpiBm_Status_t st = t7b_one_hal(8U, 255U, next_seed(), false, &mism, &left);
    verdict(bad == 0U, "T7(b) SPI6/HAL by BDMA (HAL_SPI_TransmitReceive_DMA, channels 0/1, buffers in .d3_ram), "
            "8/16 bits, 1..8192 frames: %lu failed", (unsigned long)bad);
    verdict((st == SPI_BM_OK) && (left == 255U), "T7(b) SPI6/HAL control, no RX stream (HAL_SPI_Transmit_DMA, "
            "COMM = transmit only), 255 frames: status %u, RX words with the sentinel %lu (255)", (unsigned)st,
            (unsigned long)left);
}

/* ---- The F3 steps ------------------------------------------------------------------------ */

static void step_f3_nopin(void)
{
    if (!step_begin("F3_NOPIN"))
    {
        return;
    }
    tick_line("F3_NOPIN start");
    f3_t1();
    t2hal_inst(SPI2, "SPI2");
    tick_line("F3_NOPIN T2 SPI2");
    t2hal_inst(SPI3, "SPI3");
    t2hal_inst(SPI4, "SPI4");
    t2hal_inst(SPI5, "SPI5");
    t2hal_inst(SPI6, "SPI6");
    SPI_TypeDef *const inst[5] = { SPI2, SPI3, SPI4, SPI5, SPI6 };
    const char *const names[5] = { "SPI2", "SPI3", "SPI4", "SPI5", "SPI6" };
    for (uint32_t i = 0U; i < 5U; i++)
    {
        const SpiBm_Config_t cb = mkcfg(inst[i], 3U, 1U, 8U, true, SPI_BM_DIR_FULL_DUPLEX, 1U);
        SPI_HandleTypeDef *h = hal_h(inst[i]);
        h->Instance = inst[i];
        (void)SPI_HAL_Init(h, &cb);
        const SpiBm_Status_t sd = SPI_HAL_Deinit(h);
        const uint32_t d = reset_diff(inst[i]);
        verdict((sd == SPI_BM_OK) && (d == 0U) && (h->State == HAL_SPI_STATE_RESET),
                "T14 HAL %s SPI_HAL_Deinit after SPI_HAL_Init: %u, registers off the reset image 0x%02lX (0), handle "
                "RESET %u", names[i], (unsigned)sd, (unsigned long)d, (unsigned)(h->State == HAL_SPI_STATE_RESET));
    }
    tick_line("F3_NOPIN end");
    if (!sync0())
    {
        return;
    }
    step_end();
}

static void step_j1_spi6_hal(void)
{
    if (!step_begin("J1_SPI6_HAL"))
    {
        return;
    }
    tick_line("J1_SPI6_HAL start");
    static const uint32_t bits6[7] = { 4U, 5U, 7U, 8U, 9U, 15U, 16U };
    static const uint32_t ctl6[3] = { 4U, 8U, 16U };
    (void)SPI_BM_EnableClock(SPI6);
    (void)SPI_BM_Deinit(SPI6);
    s_tier = TIER_HAL;
    t4_matrix(SPI6, "SPI6/HAL", false, bits6, 7U);
    tick_line("J1_SPI6_HAL T4");
    if (!s_bench_abort && spi6_pins(0U, 0U, GPIO_BM_SPEED_HIGH))
    {
        uint32_t mbr = 0U;
        uint32_t sck = 0U;
        (void)SPI_BM_CalcPrescaler(s_k6, 12500000U, &mbr, &sck);
        const SpiBm_Config_t c = mkcfg(SPI6, mbr, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
        const uint32_t seed = next_seed();
        SstXferRes_t r;
        xfer_local(&c, F1_BIGF, seed, s_big_tx, s_big_rx, &r);
        t4_line("SPI6/HAL", 0U, 8U, 0U, 1U, F1_BIGF, seed, &r);
        verdict(xfer_ok(&r), "T4 SPI6/HAL 65535 frames of 8 bits: status %lu, %lu mismatches",
                (unsigned long)r.status, (unsigned long)r.mism);
    }
    if (!s_bench_abort)
    {
        t4_control(SPI6, "SPI6/HAL", false, ctl6, 3U);
        (void)spi6_pins(0U, 0U, GPIO_BM_SPEED_HIGH);
    }
    if (!s_bench_abort)
    {
        s_b1_irq_min = 0xFFFFFFFFUL;
        s_b1_irq_max = 0U;
        t6_spi6("SPI6/HAL");
        tick_line("J1_SPI6_HAL T6");
        t7b_hal();
        tick_line("J1_SPI6_HAL T7(b)");
    }
    if (!s_bench_abort && spi6_pins(0U, 0U, GPIO_BM_SPEED_HIGH))
    {
        f1_b1(SPI6, "SPI6/HAL");
        LOG("B1 CM7 SPI6/HAL HAL_SPI_IRQHandler per call (T6) min %lu max %lu cycles", (unsigned long)s_b1_irq_min,
            (unsigned long)s_b1_irq_max);
    }
    s_tier = TIER_BM;
    s_h6.Instance = SPI6;
    const SpiBm_Config_t cb = mkcfg(SPI6, 2U, 0U, 8U, false, SPI_BM_DIR_FULL_DUPLEX, 1U);
    (void)SPI_HAL_Init(&s_h6, &cb);
    const uint32_t dctrl = reset_diff(SPI6);
    const SpiBm_Status_t sd = SPI_HAL_Deinit(&s_h6);
    const uint32_t d = reset_diff(SPI6);
    verdict((sd == SPI_BM_OK) && (d == 0U) && (dctrl != 0U),
            "T14 SPI6/HAL SPI_HAL_Deinit %u, registers off the reset image 0x%02lX (0); control, after SPI_HAL_Init "
            "and before the Deinit: 0x%02lX (!= 0)", (unsigned)sd, (unsigned long)d, (unsigned long)dctrl);
    const bool rel = j1_release();
    const uint32_t j1 = ((GPIOA->MODER >> (PA_J1_SCK * 2U)) & 0xFUL) | (((GPIOB->MODER >> (PB_J1_MISO * 2U)) & 0xFUL) << 4);
    verdict(rel && (j1 == 0U), "T14 J1 pins back in input: MODER PA5|PA6|PB4|PB5 0x%02lX (0)", (unsigned long)j1);
    tick_line("J1_SPI6_HAL end");
    if (!sync0())
    {
        return;
    }
    step_end();
}

/* T5(a) through spi_hal (kind 'H') and T7(c) through spi_hal (kind 'Q'). */
static void step_spi4_ad2_hal(void)
{
    if (!step_begin("SPI4_AD2_HAL") || !buschk())
    {
        return;
    }
    tick_line("SPI4_AD2_HAL start");
    const bool clk_was_on = spi4_clocked();
    (void)SPI_BM_EnableClock(SPI4);
    (void)SPI_BM_Deinit(SPI4);
    if (!spi4_pins(0U, false, GPIO_BM_PULL_DOWN, 0xFFU) || !buschk_af(PE_SPI4_MASK, NULL, NULL))
    {
        return;
    }
    s_cap_k = 0U;
    s_cap_bad = 0U;
    s_tier = TIER_HAL;
    static const uint32_t ab[4] = { 4U, 8U, 13U, 16U };
    for (uint32_t mode = 0U; mode <= 3U; mode++)
    {
        for (uint32_t ib = 0U; ib < 4U; ib++)
        {
            for (uint32_t lsb = 0U; lsb < 2U; lsb++)
            {
                const SpiBm_Config_t c = { SPI4, 3U, mode, ab[ib], lsb != 0U, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW,
                                           false, 0U, 0U, false };
                cap_one('H', &c, cap_frames(ab[ib], 3U, 0U, 0U, 32U), cap_poll);
            }
        }
    }
    tick_line("SPI4_AD2_HAL T5(a)");
    for (uint32_t b = 0U; b < 2U; b++)
    {
        const uint32_t bits = (b == 0U) ? 8U : 16U;
        const SpiBm_Config_t c = { SPI4, 3U, 0U, bits, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW, false, 0U, 0U,
                                   false };
        cap_one('Q', &c, cap_frames(bits, 3U, 0U, 0U, 16U), cap_dma);
    }
    s_tier = TIER_BM;
    LOG("CAP done k%lu", (unsigned long)s_cap_k);
    verdict(s_cap_bad == 0U, "T5(a)/T7(c) SPI4/HAL: %lu captures run, %lu with a failed Init, transfer, DMA step or "
            "Deinit", (unsigned long)s_cap_k, (unsigned long)s_cap_bad);
    s_h4.Instance = SPI4;
    const SpiBm_Config_t ctrl = { SPI4, 3U, 0U, 8U, false, SPI_BM_DIR_TX_ONLY, 1U, SPI_BM_NSS_HW, false, 0U, 0U, false };
    (void)SPI_HAL_Init(&s_h4, &ctrl);
    const uint32_t dctrl = reset_diff(SPI4);
    const SpiBm_Status_t sd = SPI_HAL_Deinit(&s_h4);
    const uint32_t d = reset_diff(SPI4);
    verdict((sd == SPI_BM_OK) && (d == 0U) && (dctrl != 0U),
            "T14 SPI4/HAL SPI_HAL_Deinit %u, registers off the reset image 0x%02lX (0); control before the Deinit "
            "0x%02lX (!= 0)", (unsigned)sd, (unsigned long)d, (unsigned long)dctrl);
    if (!spi4_pins(0U, false, GPIO_BM_PULL_DOWN, 0xFFU) || !buschk_af(PE_SPI4_MASK, NULL, NULL))
    {
        return;
    }
    (void)pins_input(GPIOE, BUS_MASK, GPIO_BM_PULL_NONE);
    tick_line("SPI4_AD2_HAL end");
    if (!buschk())
    {
        return;
    }
    if (!clk_was_on)
    {
        (void)SPI_BM_DisableClock(SPI4);
    }
    step_end();
}

/* ==========================================================================================
 * Entry points
 * ========================================================================================== */

void SPI_Selftest_CM7_PreRelease(void)
{
    SstArea_t *a = SST_AREA;
    memset((void *)a, 0, sizeof(*a));
    a->session = DWT->CYCCNT ^ 0x5B100000UL;
    a->version = SPI_BENCH_VERSION;
    a->step    = SST_STEP_NONE;
    __DMB();
    a->magic = SST_AREA_MAGIC;
    __DSB();
}

static void copy_area_str(char *dst, const volatile char *src, uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++)
    {
        dst[i] = src[i];
    }
    dst[n - 1U] = '\0';
}

void SPI_Selftest_CM7_Run(void)
{
    SstArea_t *a = SST_AREA;
    char rev4[sizeof(a->m4_rev)];
    char id4[sizeof(a->m4_build_id)];

    vTaskSuspendAll();   /* no task switch for the whole test; the HAL tick (TIM6) keeps running */
    publish_step(SST_STEP_NONE);
    bench_safe();

    LOG("CM7 Rev %s", FW_GIT_REV);
    LOG("CM7 Build id %s", FW_BUILD_ID);
    const uint32_t t0 = HAL_GetTick();
    while ((a->m4_ready != a->session) && ((HAL_GetTick() - t0) < CM4_READY_MS))
    {
    }
    const bool m4_ok = (a->m4_ready == a->session);
    copy_area_str(rev4, a->m4_rev, sizeof(rev4));
    copy_area_str(id4, a->m4_build_id, sizeof(id4));
    LOG("CM4 Rev %s", m4_ok ? rev4 : "(CPU2 not ready)");
    LOG("CM4 Build id %s", m4_ok ? id4 : "(CPU2 not ready)");
    if (!m4_ok || (a->m4_version != SPI_BENCH_VERSION))
    {
        s_fail++;
    }
    LOG("SPI_BENCH v%u idle", (unsigned)SPI_BENCH_VERSION);
    /* DBGMCU is only read (plan R4: never written). */
    LOG("silicon REV_ID=0x%04lX, test block at 0x%08lX, CM4 table v%lu, steps %lu",
        (unsigned long)((DBGMCU->IDCODE & DBGMCU_IDCODE_REV_ID_Msk) >> DBGMCU_IDCODE_REV_ID_Pos),
        (unsigned long)SST_AREA_ADDR, (unsigned long)a->m4_version, (unsigned long)k_sst_step_count);

    step_t0a();
    step_t0c();
    step_t0b();
    step_t0b_s5neg();
    step_t0b_timeout();
    step_t0b_s13();
    step_t0b_s14();
    f1_clocks();
    step_f1_nopin();
    step_j1_spi6();
    step_j1_spi1();
    step_spi4_ad2();
    step_f2_nopin();
    step_j1_spi6_ll();
    step_j1_spi1_ll();
    step_spi4_ad2_ll();
    step_f3_nopin();
    step_j1_spi6_hal();
    step_spi4_ad2_hal();
    if (step_begin("DONE"))
    {
        step_end();
    }
    bench_safe();
    verdict(!s_bench_abort, "bench completed without abort");

    (void)cm4_send(SST_CMD_DONE, 0U);
    verdict((a->m4_timeout == 0U) && (a->m4_aborts == 0U), "CM4 finished: timeout %lu, aborts %lu",
            (unsigned long)a->m4_timeout, (unsigned long)a->m4_aborts);
    tick_line("after DONE");
    LOG("Status_Task stack high-water mark %lu words", (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    LOG("FW VERDICT=%s failures=%lu", (s_fail == 0U) ? "PASS" : "FAIL", (unsigned long)s_fail);

    (void)xTaskResumeAll();
}
