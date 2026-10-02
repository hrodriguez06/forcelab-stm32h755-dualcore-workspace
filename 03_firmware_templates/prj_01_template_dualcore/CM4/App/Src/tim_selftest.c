/**
  ******************************************************************************
  * @file    tim_selftest.c
  * @brief   03_tim selftest, CPU2 side (preset selftest-tim), phase F0.
  *
  *          CPU2 has no console. It publishes its Rev, build id and step-table
  *          version in the SRAM4 block (Common/Inc/tim_selftest.h), serves the
  *          commands CPU1 writes there, and CPU1 prints and judges the results.
  *
  *          Electrical safety: the only wired pins CPU2 may ever configure are
  *          PE5/PE6 (TIM15_CH1/CH2, AF4, MCU-only lines). bench_claim() judges
  *          every output/AF against the step CPU1 published (S4), and
  *          bench_safe() puts both back in input (S2). pin_cfg() is the only
  *          caller of the GPIO driver for a wired pin.
  ******************************************************************************
  */

#include "tim_selftest.h"
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "stm32h7xx_hal.h"

/* CPU1 sends the next command within seconds; a minute of silence means it
   stopped (a fault, or a bench abort that skipped the DONE). CPU2 then goes to
   S2 and on to the application loop instead of waiting for ever. */
#define TST_CM4_IDLE_TIMEOUT_MS   60000U

#define PE_T15C1   5U
#define PE_T15C2   6U

/* ---- S2 --------------------------------------------------------------------------- */

/* PE5/PE6 in input without pull, AF fields 0. Raw writes inside the cross-core
   lock (taken if it can be; the writes happen regardless, interrupts masked). */
static void bench_safe(void)
{
    GpioBm_Section_t s;
    const bool locked = (GPIO_BM_SectionEnter(&s) == GPIO_BM_OK);
    const uint32_t pm = __get_PRIMASK();
    __disable_irq();
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOEEN;      /* CPU2's bank (RM0399 9.5.10) */
    (void)RCC->AHB4ENR;
    const uint32_t m2 = (3UL << (PE_T15C1 * 2U)) | (3UL << (PE_T15C2 * 2U));
    GPIOE->MODER   = GPIOE->MODER & ~m2;
    GPIOE->PUPDR   = GPIOE->PUPDR & ~m2;
    GPIOE->AFR[0] &= ~((0xFUL << (PE_T15C1 * 4U)) | (0xFUL << (PE_T15C2 * 4U)));
    __DSB();
    __set_PRIMASK(pm);
    if (locked)
    {
        GPIO_BM_SectionExit(&s);
    }
}

static void bench_abort(void)
{
    TST_AREA->m4_aborts++;
    bench_safe();
}

/* ---- S4 ----------------------------------------------------------------------------- */

/* All or nothing, against the step CPU1 published in the block. */
static bool bench_claim(const GPIO_TypeDef *port, uint32_t mask, uint32_t mode, uint32_t af)
{
    const uint32_t step = TST_AREA->step;
    if ((mask & 0xFFFFUL) == 0U)
    {
        return false;
    }
    for (uint32_t p = 0U; p < 16U; p++)
    {
        if (((mask & (1UL << p)) != 0U) && !tst_line_rule(1U, step, port, p, mode, af))
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
        st = GPIO_BM_ERR_PARAM;    /* CPU2 never makes a wired pin a GPIO output */
    }
    if (st != GPIO_BM_OK)
    {
        bench_abort();
        return false;
    }
    return true;
}

/* ---- T0b(b) on CPU2 --------------------------------------------------------------- */

typedef struct { GPIO_TypeDef *port; uint16_t mask; uint8_t mode; uint8_t af; } Claim_t;

#define O  GPIO_BM_MODE_OUTPUT
#define A  GPIO_BM_MODE_AF
/* Case k sets bit k of the result when accepted. Expected, by hand, in
   tim_selftest.h: only case 0 in step T0B, none between steps. */
static const Claim_t k_claims[TST_M4_CLAIM_CASES] = {
    { GPIOE, 1U << PE_T15C1, A, 4U },              /*  0 PE5 TIM15_CH1: the accepted control      */
    { GPIOE, 1U << PE_T15C2, A, 4U },              /*  1 PE6: not allowed by step T0B             */
    { GPIOE, 1U << PE_T15C1, A, 1U },              /*  2 PE5 in AF1: wrong AF                      */
    { GPIOE, 1U << PE_T15C1, O, 0U },              /*  3 PE5 as a GPIO output                      */
    { GPIOE, 1U << 4, A, 4U },                     /*  4 PE4 TIM15_CH1N: AD2-only line             */
    { GPIOE, 1U << 3, A, 4U },                     /*  5 PE3 TIM15_BKIN: AD2-only line             */
    { GPIOE, 1U << 2, O, 0U },                     /*  6 PE2 HOST_REQ                              */
    { GPIOE, 1U << 7, O, 0U },                     /*  7 PE7 MCU_ACK                               */
    { GPIOE, 1U << 0, O, 0U },                     /*  8 PE0: never touched (plan R4)              */
    { GPIOE, 1U << 1, O, 0U },                     /*  9 PE1 LD2                                   */
    { GPIOE, 1U << 9, A, 1U },                     /* 10 PE9: CPU1's bus                           */
    { GPIOA, 1U << 8, A, 0U },                     /* 11 PA8 MCO1: CPU1's line                     */
    { GPIOE, (1U << PE_T15C1) | (1U << 4), A, 4U },/* 12 mixed: PE5 with PE4                       */
};
#undef O
#undef A

/* The configuration registers of GPIOA and GPIOE, read raw: a refused claim
   writes none of them (the same oracle as CPU1's port_snap). */
#define SNAP_REGS   12U
typedef struct { uint32_t r[SNAP_REGS]; } PortSnap_t;

static void port_snap(PortSnap_t *s)
{
    const GPIO_TypeDef *const ports[2] = { GPIOA, GPIOE };
    uint32_t i = 0U;
    for (uint32_t k = 0U; k < 2U; k++)
    {
        s->r[i++] = ports[k]->MODER;
        s->r[i++] = ports[k]->OTYPER;
        s->r[i++] = ports[k]->OSPEEDR;
        s->r[i++] = ports[k]->PUPDR;
        s->r[i++] = ports[k]->AFR[0];
        s->r[i++] = ports[k]->AFR[1];
    }
}

/* Bit k set = register k of the snapshot differs. */
static uint32_t port_snap_diff(const PortSnap_t *a, const PortSnap_t *b)
{
    uint32_t diff = 0U;
    for (uint32_t k = 0U; k < SNAP_REGS; k++)
    {
        if (a->r[k] != b->r[k])
        {
            diff |= 1UL << k;
        }
    }
    return diff;
}

static void run_claims(TstClaimResult_t *r)
{
    PortSnap_t before;
    PortSnap_t after;
    port_snap(&before);
    uint32_t acc = 0U;
    r->step_seen = TST_AREA->step;
    for (uint32_t k = 0U; k < TST_M4_CLAIM_CASES; k++)
    {
        if (bench_claim(k_claims[k].port, k_claims[k].mask, k_claims[k].mode, k_claims[k].af))
        {
            acc |= 1UL << k;
        }
    }
    r->accepted     = acc;
    r->cases        = TST_M4_CLAIM_CASES;
    port_snap(&after);
    r->regs_changed = port_snap_diff(&before, &after);
}

/* PE5 to AF4 through the claim (allowed in step T0B; PE5 is an MCU-only line,
   the AD2 never drives DIO9), then bench_safe() must bring it back to input. */
static void run_safe(TstSafeResult_t *r)
{
    r->claim_ok = pin_cfg(GPIOE, PE_T15C1, GPIO_BM_MODE_AF, TST_AF_TIM15, GPIO_BM_PULL_NONE) ? 1U : 0U;
    r->af_moder = (GPIOE->MODER >> (PE_T15C1 * 2U)) & 3UL;
    r->af_afr   = (GPIOE->AFR[0] >> (PE_T15C1 * 4U)) & 0xFUL;
    bench_safe();
    r->safe_moder = (GPIOE->MODER >> (PE_T15C1 * 2U)) & 3UL;
    r->safe_afr   = (GPIOE->AFR[0] >> (PE_T15C1 * 4U)) & 0xFUL;
    r->safe_pupdr = (GPIOE->PUPDR >> (PE_T15C1 * 2U)) & 3UL;
    r->pe6_moder  = (GPIOE->MODER >> (PE_T15C2 * 2U)) & 3UL;
}

static void copy_str(volatile char *dst, uint32_t n, const char *src)
{
    uint32_t i = 0U;
    for (; (i + 1U < n) && (src[i] != '\0'); i++)
    {
        dst[i] = src[i];
    }
    for (; i < n; i++)
    {
        dst[i] = '\0';
    }
}

void TIM_Selftest_CM4_Run(void)
{
    TstArea_t *a = TST_AREA;

    /* CPU1 opened the session before releasing this core. */
    if (a->magic != TST_AREA_MAGIC)
    {
        return;
    }
    const uint32_t session = a->session;
    bench_safe();
    copy_str(a->m4_rev, sizeof(a->m4_rev), FW_GIT_REV);
    copy_str(a->m4_build_id, sizeof(a->m4_build_id), FW_BUILD_ID);
    a->m4_version = TIM_BENCH_VERSION;
    uint32_t last_seq = a->cmd_seq;
    __DMB();
    a->m4_ready = session;

    for (;;)
    {
        const uint32_t t0 = HAL_GetTick();
        while (a->cmd_seq == last_seq)
        {
            if ((HAL_GetTick() - t0) > TST_CM4_IDLE_TIMEOUT_MS)
            {
                a->m4_timeout = 1U;
                bench_safe();
                return;
            }
        }
        __DMB();
        last_seq = a->cmd_seq;
        const uint32_t cmd = a->cmd;
        const uint32_t a0  = a->arg0;

        switch (cmd)
        {
        case TST_CMD_CLAIMS:
            if (a0 < 2U)
            {
                run_claims(&a->m4_claims[a0]);
            }
            break;
        case TST_CMD_SAFE:
            run_safe(&a->m4_safe);
            break;
        case TST_CMD_DONE:
            bench_safe();
            __DMB();
            a->ack_seq = last_seq;
            return;
        default:
            break;
        }
        __DMB();
        a->ack_seq = last_seq;
    }
}
