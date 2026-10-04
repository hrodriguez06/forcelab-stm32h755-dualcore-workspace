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
   go of the pins, RM0399 53:3456-3464; a slave has no mode fault, 53:1474-1475)
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

static bool cm4_send(SstCmd_t cmd, uint32_t a0)
{
    SstArea_t *a = SST_AREA;
    a->cmd  = (uint32_t)cmd;
    a->arg0 = a0;
    a->arg1 = 0U;
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
    if (step_begin("DONE"))
    {
        step_end();
    }
    bench_safe();
    verdict(!s_bench_abort, "bench completed without abort");

    (void)cm4_send(SST_CMD_DONE, 0U);
    verdict((a->m4_timeout == 0U) && (a->m4_aborts == 0U), "CM4 finished: timeout %lu, aborts %lu",
            (unsigned long)a->m4_timeout, (unsigned long)a->m4_aborts);
    LOG("Status_Task stack high-water mark %lu words", (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    LOG("FW VERDICT=%s failures=%lu", (s_fail == 0U) ? "PASS" : "FAIL", (unsigned long)s_fail);

    (void)xTaskResumeAll();
}
