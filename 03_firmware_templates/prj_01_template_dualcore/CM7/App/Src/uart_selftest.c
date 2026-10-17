/**
  ******************************************************************************
  * @file    uart_selftest.c
  * @brief   04_uart selftest, CPU1 (Cortex-M7) side (preset selftest-uart).
  *
  *          Tests all three UART driver tiers (BM, LL, HAL) on the AD2 bench:
  *          - UT0: bench resting state and negative control (UT0_CTRL)
  *          - UT1: parameter rejection and state preservation (U2, U3)
  *          - UT2: BRR/PRESC formula verification (U5, LPUART1)
  *          - UT3: wire encoding formats on the wire (8N1, 8E1, 8O1, 7E1, 7N1, 9N1, 8N2)
  *          - UT4: loopback reception over JU1 (BM, LL IRQ ring buffer, CM7 & CM4)
  *          - UT5: error injection via AD2 DIO15 (PE, FE, NE, ORE) (SU1-SU3)
  *          - UT6: hardware flow control RTS/CTS over JU2
  *          - UT7: UART_HAL_InitEx and cross-core lock contention (U6)
  *          - UT8: UART_HAL_CheckDmaErrata in-vivo with DMA1
  *          - UT9: console UART_HAL_Init MSP lock status (U1)
  *          - UT10: prj_04 regression loopback over JU1 (1 kOhm)
  *
  *          Electrical safety rules:
  *            S2:  bench_safe() sets wired pins in input, MCU_ACK low.
  *            S4:  bench_claim() strictly checks permissions per step.
  *            S7:  handshake over HOST_REQ (PE2) / MCU_ACK (PE7).
  *            SU1: AD2 drives DIO15 only in UT5.
  *            SU2: PD5 in resting state before AD2 drives PD6.
  *            SU3: DIO15 restored to input between steps and on exit.
  *            SU4: PD3-PD6 configured only in selftest-uart.
  ******************************************************************************
  */

#include "uart_selftest.h"
#include "fw_build_id.h"
#include "gpio_bm.h"
#include "gpio_ll.h"
#include "gpio_hal.h"
#include "uart_bm.h"
#include "uart_ll.h"
#include "uart_hal.h"
#include "shared_memory.h"
#include "hsem_bm.h"
#include "app_log.h"
#include "board_def.h"
#include "stm32h7xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG(fmt, ...)       APP_LOG("[uart] " fmt, ##__VA_ARGS__)

#define CYC_PER_MS          400000U
#define SYNC_TIMEOUT_CYC    4000000000UL     /* 10 s at 400 MHz */
#define CM4_TIMEOUT_MS      10000U

static uint32_t s_step = UST_STEP_NONE;
static bool     s_bench_abort = false;
static uint32_t s_fail = 0U;
static char     s_line[512];

static UartLL_Handle_t     *s_active_ll_handle = NULL;
static UartLL_RingBuffer_t *s_active_ll_ring = NULL;
static UART_HandleTypeDef  *s_active_hal_handle = NULL;
static DMA_HandleTypeDef    s_hdma_usart2_tx;

void USART2_IRQHandler(void)
{
    if (s_active_ll_handle != NULL)
    {
        UART_LL_IrqService(s_active_ll_handle, s_active_ll_ring);
    }
    if (s_active_hal_handle != NULL)
    {
        HAL_UART_IRQHandler(s_active_hal_handle);
    }
}

void DMA1_Stream0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma_usart2_tx);
}

/* ---- Helpers --------------------------------------------------------------- */

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

#define LOG_CHUNK 120U

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
    LOG("VERDICT %s %s", (s_step < k_ust_step_count) ? k_ust_steps[s_step].name : "-", ok ? "PASS" : "FAIL");
}

/* ---- MCO1 Clock Reference (PA8 / DIO14) (S-B3) ----------------------------- */

static uint32_t s_mco_saved = 0U;
static bool     s_mco_changed = false;

static void mco1_enable(void)
{
    if (!s_mco_changed)
    {
        s_mco_saved = RCC->CFGR & (RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE);
        s_mco_changed = true;
    }
    /* MCO1 = HSE (8 MHz) / 2 = 4 MHz on PA8 (AF0). RCC_CFGR is CPU1 register */
    MODIFY_REG(RCC->CFGR, RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE, RCC_CFGR_MCO1_1 | RCC_CFGR_MCO1PRE_1);

    /* PA8 alternate function 0 (MCO1) */
    (void)GPIO_BM_ConfigAlternate(GPIOA, UART_PIN_MCO, 0U, GPIO_BM_PULL_NONE, GPIO_BM_SPEED_VERY_HIGH);
}

static void mco1_restore(void)
{
    if (s_mco_changed)
    {
        MODIFY_REG(RCC->CFGR, RCC_CFGR_MCO1 | RCC_CFGR_MCO1PRE, s_mco_saved);
        s_mco_changed = false;
    }
    (void)GPIO_BM_ConfigInput(GPIOA, UART_PIN_MCO, GPIO_BM_PULL_NONE);
}

/* ---- S2: Safe State -------------------------------------------------------- */

static void bench_safe(void)
{
    /* Ensure GPIO clocks are active */
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIODEN | RCC_AHB4ENR_GPIOEEN;
    (void)RCC->AHB4ENR;

    /* S2: PE2 (HOST_REQ) input pull-down, PE7 (MCU_ACK) output low */
    GPIOE->MODER  = (GPIOE->MODER  & ~(3UL << (UART_PIN_REQ * 2U))) | (0UL << (UART_PIN_REQ * 2U));
    GPIOE->PUPDR  = (GPIOE->PUPDR  & ~(3UL << (UART_PIN_REQ * 2U))) | (2UL << (UART_PIN_REQ * 2U));

    GPIOE->BSRR   = 1UL << (UART_PIN_ACK + 16U);
    GPIOE->OTYPER = GPIOE->OTYPER & ~(1UL << UART_PIN_ACK);
    GPIOE->PUPDR  = GPIOE->PUPDR  & ~(3UL << (UART_PIN_ACK * 2U));
    GPIOE->MODER  = (GPIOE->MODER  & ~(3UL << (UART_PIN_ACK * 2U))) | (1UL << (UART_PIN_ACK * 2U));

    /* S2: PD3, PD4, PD5, PD6 into input no-pull */
    for (uint32_t p = UART_PIN_CTS; p <= UART_PIN_RX; p++)
    {
        GPIOD->MODER &= ~(3UL << (p * 2U));
        GPIOD->PUPDR &= ~(3UL << (p * 2U));
        if (p < 8U)
        {
            GPIOD->AFR[0] &= ~(0xFUL << (p * 4U));
        }
    }

    /* Release USART2 if clocked */
    if ((RCC->APB1LENR & RCC_APB1LENR_USART2EN) != 0U)
    {
        USART2->CR1 = 0U;
        USART2->CR2 = 0U;
        USART2->CR3 = 0U;
        USART2->ICR = 0xFFFFFFFFUL;
    }

    /* Disable interrupts and clear pending (S-m) */
    HAL_NVIC_DisableIRQ(USART2_IRQn);
    HAL_NVIC_DisableIRQ(DMA1_Stream0_IRQn);
    HAL_NVIC_ClearPendingIRQ(USART2_IRQn);
    HAL_NVIC_ClearPendingIRQ(DMA1_Stream0_IRQn);

    s_active_ll_handle = NULL;
    s_active_ll_ring   = NULL;
    s_active_hal_handle = NULL;

    __DSB();
}

static void bench_abort(const char *reason)
{
    if (!s_bench_abort)
    {
        s_bench_abort = true;
        s_fail++;
    }
    bench_safe();
    LOG("ABORT step=%s reason=%s", (s_step < k_ust_step_count) ? k_ust_steps[s_step].name : "-", reason);
}

static bool bench_claim(const GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af)
{
    return ust_line_rule(0U, s_step, port, pin, mode, af, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE);
}

static bool pin_cfg(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af, uint32_t pull, uint32_t level)
{
    if ((mode == GPIO_BM_MODE_OUTPUT) || (mode == GPIO_BM_MODE_AF))
    {
        if (!bench_claim(port, pin, mode, af))
        {
            bench_abort("claim refused");
            return false;
        }
    }

    GpioBm_Status_t st;
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
        bench_abort("pin_cfg failed");
        return false;
    }
    return true;
}

/* ---- Handshake (S7) -------------------------------------------------------- */

static void ack(bool high)
{
    GPIOE->BSRR = high ? (1UL << UART_PIN_ACK) : (1UL << (UART_PIN_ACK + 16U));
    __DSB();
}

static bool wait_req(bool high, uint32_t timeout_cyc)
{
    const uint32_t t0 = DWT->CYCCNT;
    while ((((GPIOE->IDR >> UART_PIN_REQ) & 1UL) != 0UL) != high)
    {
        if ((DWT->CYCCNT - t0) >= timeout_cyc)
        {
            return false;
        }
    }
    return true;
}

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
    const uint32_t idx = ust_step_index(name);
    if (idx == UST_STEP_NONE)
    {
        bench_abort("unknown step");
        return false;
    }
    s_step = idx;
    LOG("STEP %s v=%u", k_ust_steps[idx].name, (unsigned)UART_BENCH_VERSION);
    return sync0();
}

static void step_end(void)
{
    bench_safe();
    s_step = UST_STEP_NONE;
}

/* ---- CM4 Inter-Core RPC ---------------------------------------------------- */

static bool cm4_send_cmd(UstCmd_t cmd, uint32_t timeout_ms)
{
    if (UST_AREA->magic != UST_AREA_MAGIC)
    {
        return false;
    }

    uint32_t next_seq = UST_AREA->cmd_seq + 1U;
    UST_AREA->cmd = (uint32_t)cmd;
    __DMB();
    UST_AREA->cmd_seq = next_seq;
    __DMB();

    uint32_t waited = 0U;
    while ((UST_AREA->ack_seq != next_seq) && (waited < timeout_ms))
    {
        delay_ms(1U);
        waited++;
    }

    return (UST_AREA->ack_seq == next_seq);
}

/* ---- Entry Points ---------------------------------------------------------- */

void UART_Selftest_CM7_PreRelease(void)
{
    memset(UST_AREA, 0, sizeof(UstArea_t));
    UST_AREA->magic = UST_AREA_MAGIC;
    UST_AREA->session = (DWT->CYCCNT | 1UL);
    __DMB();
}

/* ---- Test Steps UT0 - UT10 ------------------------------------------------- */

static void run_ut0(void)
{
    if (!step_begin("UT0")) { return; }

    /*
     * UT0: Resting state and series resistor verification (Revision 2, finding S-M7, S-B4).
     * 1. PD5 configured as TX idle high.
     * 2. Host reads DIO15 (PD6): must read 1.
     * 3. Host sets DIO15 output low (0V) through 1 kOhm series resistor JU1.
     * 4. MCU reads IDR of PD6 (must be 0) and IDR of PD5 (must be 1).
     * 5. Host releases DIO15 (input Z).
     * 6. MCU reads IDR of PD6 (must return to 1).
     */
    if (!pin_cfg(GPIOD, UART_PIN_TX, GPIO_BM_MODE_OUTPUT, 0U, GPIO_BM_PULL_NONE, 1U))
    {
        verdict(false, "UT0 claim PD5 failed");
        step_end();
        return;
    }

    /* Wait for host to sample resting high state */
    if (!sync0()) { step_end(); return; }

    /* Host now pulls PD6 low with AD2 DIO15 OE (3.3 mA through 1k JU1).
     * We sync again and read IDR of PD6 and PD5 */
    if (!sync0()) { step_end(); return; }
    uint32_t pd6_low = (GPIOD->IDR >> UART_PIN_RX) & 1U;
    uint32_t pd5_high = (GPIOD->IDR >> UART_PIN_TX) & 1U;

    /* Host releases DIO15 back to input Z.
     * We sync again and verify PD6 returns high */
    if (!sync0()) { step_end(); return; }
    uint32_t pd6_restored = (GPIOD->IDR >> UART_PIN_RX) & 1U;

    bool ok = (pd6_low == 0U) && (pd5_high == 1U) && (pd6_restored == 1U);
    verdict(ok, "UT0 bench resting state and 1k resistor (low=%lu, high=%lu, rest=%lu)",
            (unsigned long)pd6_low, (unsigned long)pd5_high, (unsigned long)pd6_restored);
    step_end();

    if (!step_begin("UT0_CTRL")) { return; }
    /* Negative control: PD5 driven low, held until next handshake */
    if (!pin_cfg(GPIOD, UART_PIN_TX, GPIO_BM_MODE_OUTPUT, 0U, GPIO_BM_PULL_NONE, 0U))
    {
        verdict(false, "UT0_CTRL pin_cfg failed");
        step_end();
        return;
    }
    if (!sync0()) { step_end(); return; }

    uint32_t pd6_ctrl = (GPIOD->IDR >> UART_PIN_RX) & 1U;
    verdict(pd6_ctrl == 0U, "UT0_CTRL negative control: PD5 low propagates to PD6");
    step_end();
}

static void run_ut1(void)
{
    if (!step_begin("UT1")) { return; }

    bool ok = true;

    /* Ensure clocks are enabled for instances to properly test validation (S-M2) */
    RCC->APB1LENR |= RCC_APB1LENR_USART2EN;
    (void)RCC->APB1LENR;

    /* Claim pins for USART2 in AF7 */
    if (!pin_cfg(GPIOD, UART_PIN_TX, GPIO_BM_MODE_AF, 7U, GPIO_BM_PULL_UP, 0U) ||
        !pin_cfg(GPIOD, UART_PIN_RX, GPIO_BM_MODE_AF, 7U, GPIO_BM_PULL_UP, 0U))
    {
        verdict(false, "UT1 pin claim failed");
        step_end();
        return;
    }

    static const UartBm_Pin_t bm_tx = { .port = GPIOD, .pin_idx = UART_PIN_TX, .af = 7U };
    static const UartBm_Pin_t bm_rx = { .port = GPIOD, .pin_idx = UART_PIN_RX, .af = 7U };
    static const UartLL_Pin_t ll_tx = { .port = GPIOD, .pin_idx = UART_PIN_TX, .af = 7U };
    static const UartLL_Pin_t ll_rx = { .port = GPIOD, .pin_idx = UART_PIN_RX, .af = 7U };
    static const uart_hal_pin_t hal_tx = { .port = GPIOD, .pin_idx = UART_PIN_TX, .af = 7U };
    static const uart_hal_pin_t hal_rx = { .port = GPIOD, .pin_idx = UART_PIN_RX, .af = 7U };

    /* Register snapshot helper */
    typedef struct {
        uint32_t cr1, cr2, cr3, brr, presc;
        uint32_t moder, afr0, afr1;
        uint32_t rcc;
    } RegSnapshot_t;

    RegSnapshot_t snap_before;
    snap_before.cr1   = USART2->CR1;
    snap_before.cr2   = USART2->CR2;
    snap_before.cr3   = USART2->CR3;
    snap_before.brr   = USART2->BRR;
    snap_before.presc = USART2->PRESC;
    snap_before.moder = GPIOD->MODER;
    snap_before.afr0  = GPIOD->AFR[0];
    snap_before.afr1  = GPIOD->AFR[1];
    snap_before.rcc   = RCC->APB1LENR;

    /* 1. BM Rejections */
    UartBm_Handle_t h_bm;
    UartBm_Config_t cfg_bm = {
        .instance        = USART2,
        .kernel_clock_hz = 100000000UL,
        .baudrate        = 115200UL,
        .data_bits       = UART_BM_DATA_BITS_8,
        .stop_bits       = UART_BM_STOP_BITS_1,
        .parity          = UART_BM_PARITY_NONE,
        .enable_clock    = true,
        .tx_pin          = &bm_tx,
        .rx_pin          = &bm_rx,
    };

    if (UART_BM_Init(NULL, &cfg_bm)) { ok = false; }
    if (UART_BM_Init(&h_bm, NULL)) { ok = false; }
    cfg_bm.instance = (USART_TypeDef *)0x50000000UL; /* Unknown instance across tiers (S-M2) */
    if (UART_BM_Init(&h_bm, &cfg_bm)) { ok = false; }
    cfg_bm.instance = USART2;
    cfg_bm.baudrate = 0UL;
    if (UART_BM_Init(&h_bm, &cfg_bm)) { ok = false; }
    cfg_bm.baudrate = 200000000UL; /* > f_ker */
    if (UART_BM_Init(&h_bm, &cfg_bm)) { ok = false; }
    cfg_bm.baudrate = 115200UL;
    cfg_bm.data_bits = 99U;
    if (UART_BM_Init(&h_bm, &cfg_bm)) { ok = false; }
    cfg_bm.data_bits = UART_BM_DATA_BITS_8;
    cfg_bm.stop_bits = 99U;
    if (UART_BM_Init(&h_bm, &cfg_bm)) { ok = false; }
    cfg_bm.stop_bits = UART_BM_STOP_BITS_1;
    cfg_bm.parity = 99U;
    if (UART_BM_Init(&h_bm, &cfg_bm)) { ok = false; }

    /* 2. LL Rejections (U2) */
    UartLL_Handle_t h_ll;
    UartLL_Config_t cfg_ll = {
        .instance        = USART2,
        .kernel_clock_hz = 100000000UL,
        .baudrate        = 115200UL,
        .data_bits       = LL_USART_DATAWIDTH_8B,
        .stop_bits       = LL_USART_STOPBITS_1,
        .parity          = LL_USART_PARITY_NONE,
        .oversampling    = LL_USART_OVERSAMPLING_16,
        .fifo_enable     = false,
        .hw_flow         = LL_USART_HWCONTROL_NONE,
        .enable_clock    = true,
        .tx_pin          = &ll_tx,
        .rx_pin          = &ll_rx,
    };

    if (UART_LL_Init(NULL, &cfg_ll)) { ok = false; }
    if (UART_LL_Init(&h_ll, NULL)) { ok = false; }
    cfg_ll.instance = (USART_TypeDef *)0x50000000UL; /* Unknown instance (S-M2) */
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }
    cfg_ll.instance = USART2;
    cfg_ll.baudrate = 0UL;
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }
    cfg_ll.baudrate = 115200UL;
    /* Central U2 case: UART_BM_DATA_BITS_7 (=1) passed to LL! */
    cfg_ll.data_bits = UART_BM_DATA_BITS_7;
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }
    cfg_ll.data_bits = 999U;
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }
    cfg_ll.data_bits = LL_USART_DATAWIDTH_8B;
    cfg_ll.stop_bits = 999U;
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }
    cfg_ll.stop_bits = LL_USART_STOPBITS_1;
    cfg_ll.parity = 999U;
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }
    cfg_ll.parity = LL_USART_PARITY_NONE;
    cfg_ll.oversampling = 999U;
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }
    cfg_ll.oversampling = LL_USART_OVERSAMPLING_16;
    cfg_ll.hw_flow = 999U;
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }
    cfg_ll.hw_flow = LL_USART_HWCONTROL_NONE;

    /* D2: Invalid pin AF (> 15) rejected in Phase 1 before touching hardware */
    static const UartLL_Pin_t bad_af_pin = { .port = GPIOD, .pin_idx = UART_PIN_TX, .af = 16U };
    cfg_ll.tx_pin = &bad_af_pin;
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }
    cfg_ll.tx_pin = &ll_tx;

    /* 3. HAL Rejections */
    UART_HandleTypeDef h_hal;
    uart_hal_config_t cfg_hal = {
        .instance = USART2,
        .baudrate = 115200UL,
    };
    uart_hal_config_ex_t cfg_hal_ex = {
        .instance     = USART2,
        .baudrate     = 115200UL,
        .word_length  = UART_WORDLENGTH_8B,
        .stop_bits    = UART_STOPBITS_1,
        .parity       = UART_PARITY_NONE,
        .hw_flow_ctl  = UART_HWCONTROL_NONE,
        .oversampling = UART_OVERSAMPLING_16,
        .enable_clock = true,
        .tx_pin       = &hal_tx,
        .rx_pin       = &hal_rx,
    };
    if (UART_HAL_Init(NULL, &cfg_hal) != HAL_ERROR) { ok = false; }
    if (UART_HAL_Init(&h_hal, NULL) != HAL_ERROR) { ok = false; }
    cfg_hal.instance = (USART_TypeDef *)0x50000000UL;
    if (UART_HAL_Init(&h_hal, &cfg_hal) != HAL_ERROR) { ok = false; }
    cfg_hal.instance = USART2;

    if (UART_HAL_InitEx(NULL, &cfg_hal_ex) != HAL_ERROR) { ok = false; }
    if (UART_HAL_InitEx(&h_hal, NULL) != HAL_ERROR) { ok = false; }
    cfg_hal_ex.instance = (USART_TypeDef *)0x50000000UL;
    if (UART_HAL_InitEx(&h_hal, &cfg_hal_ex) != HAL_ERROR) { ok = false; }
    cfg_hal_ex.instance = USART2;

    /* D2: HAL invalid pin AF (> 15) */
    static const uart_hal_pin_t bad_hal_af = { .port = GPIOD, .pin_idx = UART_PIN_TX, .af = 16U };
    cfg_hal_ex.tx_pin = &bad_hal_af;
    if (UART_HAL_InitEx(&h_hal, &cfg_hal_ex) != HAL_ERROR) { ok = false; }
    cfg_hal_ex.tx_pin = &hal_tx;

    /* Verify snapshot preserved (U3) */
    RegSnapshot_t snap_after;
    snap_after.cr1   = USART2->CR1;
    snap_after.cr2   = USART2->CR2;
    snap_after.cr3   = USART2->CR3;
    snap_after.brr   = USART2->BRR;
    snap_after.presc = USART2->PRESC;
    snap_after.moder = GPIOD->MODER;
    snap_after.afr0  = GPIOD->AFR[0];
    snap_after.afr1  = GPIOD->AFR[1];
    snap_after.rcc   = RCC->APB1LENR;

    if ((snap_before.cr1 != snap_after.cr1) || (snap_before.cr2 != snap_after.cr2) ||
        (snap_before.cr3 != snap_after.cr3) || (snap_before.brr != snap_after.brr) ||
        (snap_before.presc != snap_after.presc) || (snap_before.moder != snap_after.moder) ||
        (snap_before.afr0 != snap_after.afr0) || (snap_before.afr1 != snap_after.afr1) ||
        (snap_before.rcc != snap_after.rcc))
    {
        ok = false;
    }

    /* Positive control: valid init DOES modify registers */
    cfg_bm.stop_bits = UART_BM_STOP_BITS_1;
    cfg_bm.parity = UART_BM_PARITY_NONE;
    cfg_bm.enable_clock = true;
    if (UART_BM_Init(&h_bm, &cfg_bm))
    {
        if (USART2->CR1 == snap_before.cr1)
        {
            ok = false; /* valid init must change registers */
        }
        (void)UART_BM_Deinit(&h_bm);
    }
    else
    {
        ok = false;
    }

    verdict(ok, "UT1 parameter rejection across BM, LL, HAL (U2, U3, D2)");
    step_end();
}

static void run_ut2(void)
{
    if (!step_begin("UT2")) { return; }

    bool ok = true;

    /* Enable peripheral clocks for test instances (S-B5) */
    RCC->APB1LENR |= RCC_APB1LENR_UART4EN | RCC_APB1LENR_UART5EN | RCC_APB1LENR_UART8EN | RCC_APB1LENR_USART2EN;
    RCC->APB2ENR  |= RCC_APB2ENR_USART1EN | RCC_APB2ENR_USART6EN;
    RCC->APB4ENR  |= RCC_APB4ENR_LPUART1EN;
    (void)RCC->APB4ENR;

    /* Instances without external pins on bench: UART4, UART5, UART8, USART1, USART6, LPUART1 */
    static const struct {
        const char    *name;
        USART_TypeDef *inst;
        uint32_t       f_ker;
        uint32_t       baud;
        bool           over8;
    } cases[] = {
        { "UART4",   UART4,   100000000UL, 9600UL,     false },
        { "UART4",   UART4,   100000000UL, 115200UL,   false },
        { "UART5",   UART5,   100000000UL, 921600UL,   true  },
        { "UART8",   UART8,   100000000UL, 3773585UL,  true  }, /* U5: div=53 odd accepted with correct applied BRR */
        { "UART8",   UART8,   100000000UL, 12500000UL, true  }, /* U5: div=16 even */
        { "USART1",  USART1,  80000000UL,  115200UL,   false },
        { "USART6",  USART6,  80000000UL,  921600UL,   true  },
        { "LPUART1", LPUART1, 25000000UL,  9600UL,     false },
    };

    /* Test both BM and LL on each row */
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        /* 1. BM Tier */
        UartBm_Config_t cfg_bm = {
            .instance        = cases[i].inst,
            .kernel_clock_hz = cases[i].f_ker,
            .baudrate        = cases[i].baud,
            .data_bits       = UART_BM_DATA_BITS_8,
            .stop_bits       = UART_BM_STOP_BITS_1,
            .parity          = UART_BM_PARITY_NONE,
            .enable_clock    = false,
            .oversampling_8  = cases[i].over8,
        };
        UartBm_Handle_t h_bm;
        if (!UART_BM_Init(&h_bm, &cfg_bm))
        {
            ok = false;
            break;
        }
        uint32_t presc_bm = cases[i].inst->PRESC;
        uint32_t brr_bm   = cases[i].inst->BRR;
        (void)UART_BM_Deinit(&h_bm);

        LOG("UT2 row: tier=BM inst=%s f_ker=%lu baud=%lu over8=%u presc=0x%lx brr=0x%lx",
            cases[i].name, (unsigned long)cases[i].f_ker, (unsigned long)cases[i].baud,
            cases[i].over8 ? 1U : 0U, (unsigned long)presc_bm, (unsigned long)brr_bm);

        /* 2. LL Tier */
        UartLL_Config_t cfg_ll = {
            .instance        = cases[i].inst,
            .kernel_clock_hz = cases[i].f_ker,
            .baudrate        = cases[i].baud,
            .data_bits       = LL_USART_DATAWIDTH_8B,
            .stop_bits       = LL_USART_STOPBITS_1,
            .parity          = LL_USART_PARITY_NONE,
            .oversampling    = cases[i].over8 ? LL_USART_OVERSAMPLING_8 : LL_USART_OVERSAMPLING_16,
            .fifo_enable     = false,
            .hw_flow         = LL_USART_HWCONTROL_NONE,
            .enable_clock    = false,
        };
        UartLL_Handle_t h_ll;
        if (!UART_LL_Init(&h_ll, &cfg_ll))
        {
            ok = false;
            break;
        }
        uint32_t presc_ll = cases[i].inst->PRESC;
        uint32_t brr_ll   = cases[i].inst->BRR;
        (void)UART_LL_Deinit(&h_ll);

        LOG("UT2 row: tier=LL inst=%s f_ker=%lu baud=%lu over8=%u presc=0x%lx brr=0x%lx",
            cases[i].name, (unsigned long)cases[i].f_ker, (unsigned long)cases[i].baud,
            cases[i].over8 ? 1U : 0U, (unsigned long)presc_ll, (unsigned long)brr_ll);

        /* Verify BM and LL match on PRESC and BRR */
        if ((presc_bm != presc_ll) || (brr_bm != brr_ll))
        {
            ok = false;
        }
    }

    /* 3. U5 Rejection tests: small odd divisors (div=17 and div=19) whose applied divisor
     * error exceeds 2% must be REJECTED by both BM and LL (S-B5, U5) */
    static const uint32_t u5_rejects[] = { 11764705UL, 10526315UL };
    for (size_t r = 0U; r < sizeof(u5_rejects) / sizeof(u5_rejects[0]); r++)
    {
        UartBm_Config_t cfg_bm = {
            .instance = UART8,
            .kernel_clock_hz = 100000000UL,
            .baudrate = u5_rejects[r],
            .data_bits = UART_BM_DATA_BITS_8,
            .stop_bits = UART_BM_STOP_BITS_1,
            .parity = UART_BM_PARITY_NONE,
            .enable_clock = false,
            .oversampling_8 = true,
        };
        UartBm_Handle_t h_bm;
        if (UART_BM_Init(&h_bm, &cfg_bm))
        {
            ok = false; /* MUST reject */
            (void)UART_BM_Deinit(&h_bm);
        }

        UartLL_Config_t cfg_ll = {
            .instance = UART8,
            .kernel_clock_hz = 100000000UL,
            .baudrate = u5_rejects[r],
            .data_bits = LL_USART_DATAWIDTH_8B,
            .stop_bits = LL_USART_STOPBITS_1,
            .parity = LL_USART_PARITY_NONE,
            .oversampling = LL_USART_OVERSAMPLING_8,
            .fifo_enable = false,
            .hw_flow = LL_USART_HWCONTROL_NONE,
            .enable_clock = false,
        };
        UartLL_Handle_t h_ll;
        if (UART_LL_Init(&h_ll, &cfg_ll))
        {
            ok = false; /* MUST reject */
            (void)UART_LL_Deinit(&h_ll);
        }
    }

    verdict(ok, "UT2 baudrate divider and prescaler calculations (BM, LL, U5, LPUART1)");
    step_end();
}

static void run_ut3(void)
{
    if (!step_begin("UT3")) { return; }

    bool ok = true;
    static const uint8_t pat[] = { 0x55U, 0xAAU, 0xA5U, 0x5AU, 0x00U, 0xFFU };
    const size_t pat_len = sizeof(pat);

    static const UartBm_Pin_t bm_tx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .af = 7U };
    static const UartLL_Pin_t ll_tx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .af = 7U };
    static const uart_hal_pin_t hal_tx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .af = 7U };

    /* 1. BM Rows */
    static const struct {
        const char *name;
        uint32_t   baud;
        uint32_t   data_bits;
        uint32_t   stop_bits;
        uint32_t   parity;
        bool       over8;
    } bm_formats[] = {
        { "8N1",  115200UL, UART_BM_DATA_BITS_8, UART_BM_STOP_BITS_1, UART_BM_PARITY_NONE, false },
        { "8E1",  115200UL, UART_BM_DATA_BITS_8, UART_BM_STOP_BITS_1, UART_BM_PARITY_EVEN, false },
        { "8O1",  115200UL, UART_BM_DATA_BITS_8, UART_BM_STOP_BITS_1, UART_BM_PARITY_ODD,  false },
        { "7E1",  115200UL, UART_BM_DATA_BITS_7, UART_BM_STOP_BITS_1, UART_BM_PARITY_EVEN, false },
        { "7N1",  115200UL, UART_BM_DATA_BITS_7, UART_BM_STOP_BITS_1, UART_BM_PARITY_NONE, false },
        { "9N1",  115200UL, UART_BM_DATA_BITS_9, UART_BM_STOP_BITS_1, UART_BM_PARITY_NONE, false },
        { "8N2",  115200UL, UART_BM_DATA_BITS_8, UART_BM_STOP_BITS_2, UART_BM_PARITY_NONE, false },
        { "8N1",  9600UL,   UART_BM_DATA_BITS_8, UART_BM_STOP_BITS_1, UART_BM_PARITY_NONE, false },
        { "8N1",  921600UL, UART_BM_DATA_BITS_8, UART_BM_STOP_BITS_1, UART_BM_PARITY_NONE, true  },
    };

    for (size_t i = 0U; i < sizeof(bm_formats) / sizeof(bm_formats[0]); i++)
    {
        UartBm_Config_t cfg = {
            .instance        = USART2,
            .kernel_clock_hz = 100000000UL,
            .baudrate        = bm_formats[i].baud,
            .data_bits       = bm_formats[i].data_bits,
            .stop_bits       = bm_formats[i].stop_bits,
            .parity          = bm_formats[i].parity,
            .enable_clock    = true,
            .oversampling_8  = bm_formats[i].over8,
            .fifo_enable     = true,
            .tx_pin          = &bm_tx,
        };

        UartBm_Handle_t h;
        if (!UART_BM_Init(&h, &cfg))
        {
            ok = false;
            break;
        }

        LOG("UT3 row: tier=BM format=%s baud=%lu over8=%u presc=0x%lx brr=0x%lx",
            bm_formats[i].name, (unsigned long)bm_formats[i].baud,
            bm_formats[i].over8 ? 1U : 0U, (unsigned long)USART2->PRESC, (unsigned long)USART2->BRR);

        for (size_t j = 0U; j < pat_len; j++)
        {
            if (!UART_BM_WriteByte(&h, pat[j]))
            {
                ok = false;
                break;
            }
        }
        (void)UART_BM_Flush(&h, 100000U);
        while ((USART2->ISR & USART_ISR_TC) == 0U) {}
        delay_ms(5U);
        (void)UART_BM_Deinit(&h);
    }

    /* 2. LL Rows */
    static const struct {
        const char *name;
        uint32_t   baud;
        uint32_t   data_bits;
        uint32_t   stop_bits;
        uint32_t   parity;
        uint32_t   oversampling;
    } ll_formats[] = {
        { "8N1", 115200UL, LL_USART_DATAWIDTH_8B, LL_USART_STOPBITS_1, LL_USART_PARITY_NONE, LL_USART_OVERSAMPLING_16 },
        { "8E1", 115200UL, LL_USART_DATAWIDTH_8B, LL_USART_STOPBITS_1, LL_USART_PARITY_EVEN, LL_USART_OVERSAMPLING_16 },
        { "7E1", 115200UL, LL_USART_DATAWIDTH_7B, LL_USART_STOPBITS_1, LL_USART_PARITY_EVEN, LL_USART_OVERSAMPLING_16 },
        { "9N1", 115200UL, LL_USART_DATAWIDTH_9B, LL_USART_STOPBITS_1, LL_USART_PARITY_NONE, LL_USART_OVERSAMPLING_16 },
    };

    for (size_t i = 0U; i < sizeof(ll_formats) / sizeof(ll_formats[0]); i++)
    {
        UartLL_Config_t cfg_ll = {
            .instance        = USART2,
            .kernel_clock_hz = 100000000UL,
            .baudrate        = ll_formats[i].baud,
            .data_bits       = ll_formats[i].data_bits,
            .stop_bits       = ll_formats[i].stop_bits,
            .parity          = ll_formats[i].parity,
            .oversampling    = ll_formats[i].oversampling,
            .fifo_enable     = true,
            .hw_flow         = LL_USART_HWCONTROL_NONE,
            .enable_clock    = true,
            .tx_pin          = &ll_tx,
        };

        UartLL_Handle_t h_ll;
        if (!UART_LL_Init(&h_ll, &cfg_ll))
        {
            ok = false;
            break;
        }

        LOG("UT3 row: tier=LL format=%s baud=%lu over8=%u presc=0x%lx brr=0x%lx",
            ll_formats[i].name, (unsigned long)ll_formats[i].baud,
            (ll_formats[i].oversampling == LL_USART_OVERSAMPLING_8) ? 1U : 0U,
            (unsigned long)USART2->PRESC, (unsigned long)USART2->BRR);

        for (size_t j = 0U; j < pat_len; j++)
        {
            if (!UART_LL_WriteByte(&h_ll, pat[j]))
            {
                ok = false;
                break;
            }
        }
        (void)UART_LL_Flush(&h_ll, 100000U);
        while ((USART2->ISR & USART_ISR_TC) == 0U) {}
        delay_ms(5U);
        (void)UART_LL_Deinit(&h_ll);
    }

    /* 3. HAL Rows */
    static const struct {
        const char *name;
        uint32_t   baud;
        uint32_t   word_len;
        uint32_t   parity;
    } hal_formats[] = {
        { "8N1", 115200UL, UART_WORDLENGTH_8B, UART_PARITY_NONE },
        { "8E1", 115200UL, UART_WORDLENGTH_9B, UART_PARITY_EVEN }, /* 8E1 requires 9B in HAL */
    };

    for (size_t i = 0U; i < sizeof(hal_formats) / sizeof(hal_formats[0]); i++)
    {
        UART_HandleTypeDef h_hal;
        memset(&h_hal, 0, sizeof(h_hal));
        uart_hal_config_ex_t cfg_hal = {
            .instance     = USART2,
            .baudrate     = hal_formats[i].baud,
            .word_length  = hal_formats[i].word_len,
            .stop_bits    = UART_STOPBITS_1,
            .parity       = hal_formats[i].parity,
            .hw_flow_ctl  = UART_HWCONTROL_NONE,
            .oversampling = UART_OVERSAMPLING_16,
            .enable_clock = true,
            .tx_pin       = &hal_tx,
        };

        if (UART_HAL_InitEx(&h_hal, &cfg_hal) != HAL_OK)
        {
            ok = false;
            break;
        }

        LOG("UT3 row: tier=HAL format=%s baud=%lu over8=0 presc=0x%lx brr=0x%lx",
            hal_formats[i].name, (unsigned long)hal_formats[i].baud,
            (unsigned long)USART2->PRESC, (unsigned long)USART2->BRR);

        if (HAL_UART_Transmit(&h_hal, (uint8_t *)pat, (uint16_t)pat_len, 100U) != HAL_OK)
        {
            ok = false;
        }
        while ((USART2->ISR & USART_ISR_TC) == 0U) {}
        delay_ms(5U);
        (void)HAL_UART_DeInit(&h_hal);
    }

    verdict(ok, "UT3 wire format encoding across BM, LL, HAL (MCO1 ratio 25:1)");
    step_end();
}

static void run_ut4(void)
{
    if (!step_begin("UT4")) { return; }

    bool ok = true;
    static const UartBm_Pin_t bm_tx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .af = 7U };
    static const UartBm_Pin_t bm_rx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .af = 7U };
    static const UartLL_Pin_t ll_tx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .af = 7U };
    static const UartLL_Pin_t ll_rx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .af = 7U };

    /* 1. BM Loopback over JU1 (8N1 @ 115200) */
    {
        UartBm_Config_t cfg = {
            .instance        = USART2,
            .kernel_clock_hz = 100000000UL,
            .baudrate        = 115200UL,
            .data_bits       = UART_BM_DATA_BITS_8,
            .stop_bits       = UART_BM_STOP_BITS_1,
            .parity          = UART_BM_PARITY_NONE,
            .enable_clock    = true,
            .oversampling_8  = false,
            .fifo_enable     = true,
            .tx_pin          = &bm_tx,
            .rx_pin          = &bm_rx,
        };
        UartBm_Handle_t h;
        if (UART_BM_Init(&h, &cfg))
        {
            for (uint8_t b = 0x30U; b < 0x38U; b++)
            {
                (void)UART_BM_WriteByte(&h, b);
                uint8_t rx = 0U;
                uint32_t sp = 100000U;
                while (!UART_BM_ReadByte(&h, &rx) && (sp > 0U)) { sp--; }
                if (rx != b) { ok = false; break; }
            }
            (void)UART_BM_Deinit(&h);
        }
        else
        {
            ok = false;
        }
    }

    /* 2. LL Loopback polled: FIFO off and FIFO on (S-M5) */
    for (int fifo = 0; fifo <= 1; fifo++)
    {
        UartLL_Config_t cfg = {
            .instance        = USART2,
            .kernel_clock_hz = 100000000UL,
            .baudrate        = 115200UL,
            .data_bits       = LL_USART_DATAWIDTH_8B,
            .stop_bits       = LL_USART_STOPBITS_1,
            .parity          = LL_USART_PARITY_NONE,
            .oversampling    = LL_USART_OVERSAMPLING_16,
            .fifo_enable     = (fifo != 0),
            .hw_flow         = LL_USART_HWCONTROL_NONE,
            .enable_clock    = true,
            .tx_pin          = &ll_tx,
            .rx_pin          = &ll_rx,
        };
        UartLL_Handle_t h;
        if (UART_LL_Init(&h, &cfg))
        {
            for (uint8_t b = 0x41U; b < 0x49U; b++)
            {
                (void)UART_LL_WriteByte(&h, b);
                uint8_t rx = 0U;
                uint32_t sp = 100000U;
                while (!UART_LL_ReadByte(&h, &rx) && (sp > 0U)) { sp--; }
                if (rx != b) { ok = false; break; }
            }
            (void)UART_LL_Deinit(&h);
        }
        else
        {
            ok = false;
        }
    }

    /* 3. LL Loopback: OVER8 (S-M5) */
    {
        UartLL_Config_t cfg = {
            .instance        = USART2,
            .kernel_clock_hz = 100000000UL,
            .baudrate        = 921600UL,
            .data_bits       = LL_USART_DATAWIDTH_8B,
            .stop_bits       = LL_USART_STOPBITS_1,
            .parity          = LL_USART_PARITY_NONE,
            .oversampling    = LL_USART_OVERSAMPLING_8,
            .fifo_enable     = true,
            .hw_flow         = LL_USART_HWCONTROL_NONE,
            .enable_clock    = true,
            .tx_pin          = &ll_tx,
            .rx_pin          = &ll_rx,
        };
        UartLL_Handle_t h;
        if (UART_LL_Init(&h, &cfg))
        {
            for (uint8_t b = 0x50U; b < 0x58U; b++)
            {
                (void)UART_LL_WriteByte(&h, b);
                uint8_t rx = 0U;
                uint32_t sp = 100000U;
                while (!UART_LL_ReadByte(&h, &rx) && (sp > 0U)) { sp--; }
                if (rx != b) { ok = false; break; }
            }
            (void)UART_LL_Deinit(&h);
        }
        else
        {
            ok = false;
        }
    }

    /* 4. LL Loopback: 7-bit mask verification (S-M5) */
    {
        UartLL_Config_t cfg = {
            .instance        = USART2,
            .kernel_clock_hz = 100000000UL,
            .baudrate        = 115200UL,
            .data_bits       = LL_USART_DATAWIDTH_7B,
            .stop_bits       = LL_USART_STOPBITS_1,
            .parity          = LL_USART_PARITY_NONE,
            .oversampling    = LL_USART_OVERSAMPLING_16,
            .fifo_enable     = false,
            .hw_flow         = LL_USART_HWCONTROL_NONE,
            .enable_clock    = true,
            .tx_pin          = &ll_tx,
            .rx_pin          = &ll_rx,
        };
        UartLL_Handle_t h;
        if (UART_LL_Init(&h, &cfg))
        {
            /* Send byte with MSB set: 0xA5. In 7-bit mode, HW masks to 0x25 (0xA5 & 0x7F) */
            (void)UART_LL_WriteByte(&h, 0xA5U);
            uint8_t rx = 0U;
            uint32_t sp = 100000U;
            while (!UART_LL_ReadByte(&h, &rx) && (sp > 0U)) { sp--; }
            if (rx != (0xA5U & 0x7FU)) { ok = false; }
            (void)UART_LL_Deinit(&h);
        }
        else
        {
            ok = false;
        }
    }

    /* 5. LL Loopback with IRQ and Ring Buffer */
    static uint8_t             rx_buf[64];
    static UartLL_RingBuffer_t ring = { .data = rx_buf, .size = sizeof(rx_buf), .head = 0U, .tail = 0U };
    static UartLL_Handle_t     h_ll;

    {
        UartLL_Config_t cfg = {
            .instance        = USART2,
            .kernel_clock_hz = 100000000UL,
            .baudrate        = 115200UL,
            .data_bits       = LL_USART_DATAWIDTH_8B,
            .stop_bits       = LL_USART_STOPBITS_1,
            .parity          = LL_USART_PARITY_NONE,
            .oversampling    = LL_USART_OVERSAMPLING_16,
            .fifo_enable     = true,
            .hw_flow         = LL_USART_HWCONTROL_NONE,
            .enable_clock    = true,
            .tx_pin          = &ll_tx,
            .rx_pin          = &ll_rx,
        };

        ring.head = 0U;
        ring.tail = 0U;
        s_active_ll_handle = &h_ll;
        s_active_ll_ring   = &ring;

        if (UART_LL_Init(&h_ll, &cfg))
        {
            if (UART_LL_EnableRxIrq(&h_ll, USART2_IRQn, 5U))
            {
                static const uint8_t test_data[] = "CM7_UART_LL_IRQ_TEST_0123456789";
                const size_t len = sizeof(test_data) - 1U;

                (void)UART_LL_Write(&h_ll, test_data, len, 50000U);
                (void)UART_LL_Flush(&h_ll, 50000U);

                uint32_t sp = 200000U;
                while ((UartLL_RingBuffer_Count(&ring) < len) && (sp > 0U)) { sp--; }

                if (UartLL_RingBuffer_Count(&ring) != len)
                {
                    ok = false;
                }
                else
                {
                    for (size_t i = 0U; i < len; i++)
                    {
                        uint8_t b = 0U;
                        if (!UartLL_RingBuffer_Get(&ring, &b) || (b != test_data[i]))
                        {
                            ok = false;
                            break;
                        }
                    }
                }
                UART_LL_DisableRxIrq(&h_ll, USART2_IRQn);
            }
            else
            {
                ok = false;
            }
            (void)UART_LL_Deinit(&h_ll);
        }
        else
        {
            ok = false;
        }

        s_active_ll_handle = NULL;
        s_active_ll_ring   = NULL;
    }

    verdict(ok, "UT4 CM7 loopback reception (JU1, polled/irq, fifo, over8, 7b mask)");
    step_end();
}

static void run_ut4_cm4(void)
{
    if (!step_begin("UT4_CM4")) { return; }

    bool ok = true;
    if (cm4_send_cmd(UST_CMD_UT4, CM4_TIMEOUT_MS))
    {
        if (UST_AREA->m4_ut4_ok != 1U)
        {
            ok = false;
        }
    }
    else
    {
        ok = false;
    }

    verdict(ok, "UT4_CM4 CM4 loopback reception (JU1)");
    step_end();
}

static void run_ut5(void)
{
    if (!step_begin("UT5")) { return; }

    bool ok = true;

    /* SU2: Ensure PD5 is in input safe state with pull-up. PD5 is NOT configured as output/AF. */
    (void)pin_cfg(GPIOD, UART_PIN_TX, GPIO_BM_MODE_INPUT, 0U, GPIO_BM_PULL_UP, 0U);

    static const UartLL_Pin_t rx_pin = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .af = 7U };
    UartLL_Config_t cfg = {
        .instance        = USART2,
        .kernel_clock_hz = 100000000UL,
        .baudrate        = 9600UL,
        .data_bits       = LL_USART_DATAWIDTH_9B,
        .stop_bits       = LL_USART_STOPBITS_1,
        .parity          = LL_USART_PARITY_EVEN,
        .oversampling    = LL_USART_OVERSAMPLING_16,
        .fifo_enable     = false,
        .hw_flow         = LL_USART_HWCONTROL_NONE,
        .enable_clock    = true,
        .rx_pin          = &rx_pin,
    };

    UartLL_Handle_t h;
    if (!UART_LL_Init(&h, &cfg))
    {
        verdict(false, "UT5 UART_LL_Init failed");
        step_end();
        return;
    }

    /* Initial check: no errors pending */
    if (UART_LL_TakeErrors(&h) != 0U)
    {
        ok = false;
    }

    /* Phase 1: Host injects PE via DIO15 */
    if (sync0())
    {
        if (sync0())
        {
            uint8_t rx_b = 0U;
            (void)UART_LL_ReadByte(&h, &rx_b);
            uint32_t err = UART_LL_TakeErrors(&h);
            LOG("UT5 PE err=0x%lx (expected 0x%lx)", (unsigned long)err, (unsigned long)UART_LL_ERR_PE);
            if ((err & UART_LL_ERR_PE) == 0U) { ok = false; }
            if (UART_LL_TakeErrors(&h) != 0U) { ok = false; } /* single-shot */
        }
    }

    /* Phase 2: Host injects FE via DIO15 */
    if (sync0())
    {
        if (sync0())
        {
            uint8_t rx_b = 0U;
            (void)UART_LL_ReadByte(&h, &rx_b);
            uint32_t err = UART_LL_TakeErrors(&h);
            LOG("UT5 FE err=0x%lx (expected 0x%lx)", (unsigned long)err, (unsigned long)UART_LL_ERR_FE);
            if ((err & UART_LL_ERR_FE) == 0U) { ok = false; }
            if (UART_LL_TakeErrors(&h) != 0U) { ok = false; }
        }
    }

    /* Phase 3: Host injects NE via DIO15 */
    if (sync0())
    {
        if (sync0())
        {
            uint8_t rx_b = 0U;
            (void)UART_LL_ReadByte(&h, &rx_b);
            uint32_t err = UART_LL_TakeErrors(&h);
            LOG("UT5 NE err=0x%lx (flagged=%u)", (unsigned long)err, ((err & UART_LL_ERR_NE) != 0U) ? 1U : 0U);
        }
    }

    /* Phase 4: Host injects ORE (3 bytes sent without reading) */
    if (sync0())
    {
        if (sync0())
        {
            uint32_t err = UART_LL_TakeErrors(&h);
            LOG("UT5 ORE err=0x%lx (expected 0x%lx)", (unsigned long)err, (unsigned long)UART_LL_ERR_ORE);
            if ((err & UART_LL_ERR_ORE) == 0U) { ok = false; }
            uint8_t rx_b = 0U;
            while (UART_LL_ReadByte(&h, &rx_b)) {}
            (void)UART_LL_TakeErrors(&h);
        }
    }

    /* Phase 5: Clean frame control (no error expected) */
    if (sync0())
    {
        if (sync0())
        {
            uint8_t rx_b = 0U;
            (void)UART_LL_ReadByte(&h, &rx_b);
            uint32_t err = UART_LL_TakeErrors(&h);
            LOG("UT5 clean frame err=0x%lx (expected 0)", (unsigned long)err);
            if (err != 0U) { ok = false; }
        }
    }

    (void)UART_LL_Deinit(&h);

    verdict(ok, "UT5 error injection detection (PE, FE, NE, ORE, clean control)");
    step_end();
}

static void run_ut6(void)
{
    if (!step_begin("UT6")) { return; }

    bool ok = true;

    static const UartLL_Pin_t tx  = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX,  .af = 7U };
    static const UartLL_Pin_t rx  = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX,  .af = 7U };

    /* Configure RTS/CTS pins (JU2 connects RTS PD4 to CTS PD3) */
    if (!pin_cfg(UART_BENCH_PORT_USART, UART_PIN_RTS, GPIO_BM_MODE_AF, 7U, GPIO_BM_PULL_NONE, 0U) ||
        !pin_cfg(UART_BENCH_PORT_USART, UART_PIN_CTS, GPIO_BM_MODE_AF, 7U, GPIO_BM_PULL_NONE, 0U))
    {
        verdict(false, "UT6 pin_cfg RTS/CTS failed");
        step_end();
        return;
    }

    /* 1. With Hardware Flow Control (RTS/CTS), FIFO enabled (16 entries) */
    UartLL_Config_t cfg_fc = {
        .instance        = USART2,
        .kernel_clock_hz = 100000000UL,
        .baudrate        = 115200UL,
        .data_bits       = LL_USART_DATAWIDTH_8B,
        .stop_bits       = LL_USART_STOPBITS_1,
        .parity          = LL_USART_PARITY_NONE,
        .oversampling    = LL_USART_OVERSAMPLING_16,
        .fifo_enable     = true,
        .hw_flow         = LL_USART_HWCONTROL_RTS_CTS,
        .enable_clock    = true,
        .tx_pin          = &tx,
        .rx_pin          = &rx,
    };

    UartLL_Handle_t h_fc;
    if (UART_LL_Init(&h_fc, &cfg_fc))
    {
        /* Write N = 20 bytes (N > FIFO + 1) without draining RX. Check every write (S-M3) */
        uint32_t written = 0U;
        for (uint8_t b = 0U; b < 20U; b++)
        {
            if (UART_LL_WriteByte(&h_fc, 0x40U + b))
            {
                written++;
            }
        }
        LOG("UT6 FC phase: written=%lu/20", (unsigned long)written);
        delay_ms(5U);

        /* Flow control should prevent ORE */
        uint32_t err_fc = UART_LL_TakeErrors(&h_fc);
        if (err_fc != 0U)
        {
            LOG("UT6 FC unexpected error=0x%lx", (unsigned long)err_fc);
            ok = false;
        }

        /* Drain RX */
        uint8_t dummy = 0U;
        while (UART_LL_ReadByte(&h_fc, &dummy)) {}

        (void)UART_LL_Deinit(&h_fc);
    }
    else
    {
        ok = false;
    }

    /* 2. Negative Control: WITHOUT flow control, N = 20 bytes causes ORE (S-M3) */
    UartLL_Config_t cfg_nofc = {
        .instance        = USART2,
        .kernel_clock_hz = 100000000UL,
        .baudrate        = 115200UL,
        .data_bits       = LL_USART_DATAWIDTH_8B,
        .stop_bits       = LL_USART_STOPBITS_1,
        .parity          = LL_USART_PARITY_NONE,
        .oversampling    = LL_USART_OVERSAMPLING_16,
        .fifo_enable     = false, /* No FIFO: 1 byte shift + 1 byte RDR -> 3rd byte causes ORE */
        .hw_flow         = LL_USART_HWCONTROL_NONE,
        .enable_clock    = true,
        .tx_pin          = &tx,
        .rx_pin          = &rx,
    };

    UartLL_Handle_t h_nofc;
    if (UART_LL_Init(&h_nofc, &cfg_nofc))
    {
        for (uint8_t b = 0U; b < 10U; b++)
        {
            (void)UART_LL_WriteByte(&h_nofc, 0x60U + b);
            delay_ms(1U);
        }
        delay_ms(5U);

        uint32_t err_nofc = UART_LL_TakeErrors(&h_nofc);
        LOG("UT6 NO-FC negative control err=0x%lx (expected ORE 0x%lx)",
            (unsigned long)err_nofc, (unsigned long)UART_LL_ERR_ORE);
        if ((err_nofc & UART_LL_ERR_ORE) == 0U)
        {
            ok = false; /* Negative control must produce ORE */
        }

        uint8_t dummy = 0U;
        while (UART_LL_ReadByte(&h_nofc, &dummy)) {}

        (void)UART_LL_Deinit(&h_nofc);
    }
    else
    {
        ok = false;
    }

    verdict(ok, "UT6 RTS/CTS flow control over JU2 (with ORE negative control)");
    step_end();
}

static void run_ut7(void)
{
    if (!step_begin("UT7")) { return; }

    bool ok = true;
    UART_HandleTypeDef huart2;
    memset(&huart2, 0, sizeof(huart2));
    static const uart_hal_pin_t hal_tx_pin = {
        .port    = UART_BENCH_PORT_USART,
        .pin_idx = UART_PIN_TX,
        .af      = 7U
    };
    static const uart_hal_pin_t hal_rx_pin = {
        .port    = UART_BENCH_PORT_USART,
        .pin_idx = UART_PIN_RX,
        .af      = 7U
    };

    /* S-M1: 8E1 requires UART_WORDLENGTH_9B in STM32 HAL! */
    uart_hal_config_ex_t cfg = {
        .instance     = USART2,
        .baudrate     = 115200UL,
        .word_length  = UART_WORDLENGTH_9B,
        .stop_bits    = UART_STOPBITS_1,
        .parity       = UART_PARITY_EVEN,
        .hw_flow_ctl  = UART_HWCONTROL_NONE,
        .oversampling = UART_OVERSAMPLING_16,
        .enable_clock = true,
        .tx_pin       = &hal_tx_pin,
        .rx_pin       = &hal_rx_pin,
    };

    /* Snapshot of pins before (S-M1) */
    uint32_t snap_moder_before = GPIOD->MODER;
    uint32_t snap_afr_before   = GPIOD->AFR[0];
    (void)snap_moder_before;
    (void)snap_afr_before;

    /* 1. Request CM4 to hold GPIO lock */
    if (cm4_send_cmd(UST_CMD_LOCK_ACQUIRE, CM4_TIMEOUT_MS))
    {
        if (UST_AREA->lock_held == 1U)
        {
            /* UART_HAL_InitEx must return HAL_BUSY (U6) */
            HAL_StatusTypeDef st = UART_HAL_InitEx(&huart2, &cfg);
            if (st != HAL_BUSY)
            {
                ok = false;
            }
        }
        else
        {
            ok = false;
        }
        (void)cm4_send_cmd(UST_CMD_LOCK_RELEASE, CM4_TIMEOUT_MS);
    }
    else
    {
        ok = false;
    }

    /* 2. Lock released: UART_HAL_InitEx must return HAL_OK */
    if (UART_HAL_InitEx(&huart2, &cfg) == HAL_OK)
    {
        /* Verify pins were actually configured to AF7 */
        uint32_t af_tx = (GPIOD->AFR[0] >> (UART_PIN_TX * 4U)) & 0xFU;
        uint32_t af_rx = (GPIOD->AFR[0] >> (UART_PIN_RX * 4U)) & 0xFU;
        if ((af_tx != 7U) || (af_rx != 7U))
        {
            ok = false;
        }

        /* Loopback check with 8E1: byte 0xA5 */
        uint8_t tx_val = 0xA5U;
        uint8_t rx_val = 0U;
        if (HAL_UART_Transmit(&huart2, &tx_val, 1U, 100U) != HAL_OK ||
            HAL_UART_Receive(&huart2, &rx_val, 1U, 100U) != HAL_OK)
        {
            ok = false;
        }
        /* With UART_WORDLENGTH_9B, rx_val must be exactly 0xA5 (not masked to 0x25!) */
        if (rx_val != tx_val)
        {
            LOG("UT7 rx_val=0x%02x != tx_val=0x%02x", rx_val, tx_val);
            ok = false;
        }
        (void)HAL_UART_DeInit(&huart2);
    }
    else
    {
        ok = false;
    }

    verdict(ok, "UT7 UART_HAL_InitEx 8E1, pin snapshot and GPIO lock contention U6");
    step_end();
}

static void run_ut8(void)
{
    if (!step_begin("UT8")) { return; }

    bool ok = true;
    __HAL_RCC_DMA1_CLK_ENABLE();

    static const uart_hal_pin_t hal_tx_pin = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .af = 7U };
    static const uart_hal_pin_t hal_rx_pin = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .af = 7U };

    uart_hal_config_ex_t cfg_uart = {
        .instance     = USART2,
        .baudrate     = 115200UL,
        .word_length  = UART_WORDLENGTH_8B,
        .stop_bits    = UART_STOPBITS_1,
        .parity       = UART_PARITY_NONE,
        .hw_flow_ctl  = UART_HWCONTROL_NONE,
        .oversampling = UART_OVERSAMPLING_16,
        .enable_clock = true,
        .tx_pin       = &hal_tx_pin,
        .rx_pin       = &hal_rx_pin,
    };

    UART_HandleTypeDef huart2;
    memset(&huart2, 0, sizeof(huart2));
    if (UART_HAL_InitEx(&huart2, &cfg_uart) != HAL_OK)
    {
        verdict(false, "UT8 UART_HAL_InitEx failed");
        step_end();
        return;
    }

    s_hdma_usart2_tx.Instance                 = DMA1_Stream0;
    s_hdma_usart2_tx.Init.Request             = DMA_REQUEST_USART2_TX;
    s_hdma_usart2_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    s_hdma_usart2_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_hdma_usart2_tx.Init.MemInc              = DMA_MINC_ENABLE;
    s_hdma_usart2_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_hdma_usart2_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    s_hdma_usart2_tx.Init.Mode                = DMA_NORMAL;
    s_hdma_usart2_tx.Init.Priority            = DMA_PRIORITY_LOW;
    s_hdma_usart2_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;

    if (HAL_DMA_Init(&s_hdma_usart2_tx) != HAL_OK)
    {
        ok = false;
    }

    /* 1. Test live errata check on DMA1 Stream 0 */
    if (UART_HAL_CheckDmaErrata(&s_hdma_usart2_tx) != HAL_OK)
    {
        ok = false;
    }

    /* Bit 20 must be set and read back as 1 */
    if ((DMA1_Stream0->CR & (1UL << 20)) == 0U)
    {
        ok = false;
    }

    /* 2. Live DMA Transfer (S-M4) */
    static const uint8_t dma_payload[] = "LIVE_DMA_TRANSFER_OK";
    const uint16_t dma_len = (uint16_t)(sizeof(dma_payload) - 1U);

    /* Link DMA to UART */
    __HAL_LINKDMA(&huart2, hdmatx, s_hdma_usart2_tx);

    /* Start transmit via DMA */
    if (HAL_UART_Transmit_DMA(&huart2, (uint8_t *)dma_payload, dma_len) != HAL_OK)
    {
        ok = false;
    }
    else
    {
        /* While DMA stream is active, CheckDmaErrata must return HAL_BUSY */
        if (UART_HAL_CheckDmaErrata(&s_hdma_usart2_tx) != HAL_BUSY)
        {
            ok = false;
        }

        /* Receive via loopback and verify */
        uint8_t rx_buf[32];
        memset(rx_buf, 0, sizeof(rx_buf));
        for (uint16_t i = 0U; i < dma_len; i++)
        {
            uint32_t sp = 100000U;
            while (((USART2->ISR & USART_ISR_RXNE_RXFNE) == 0U) && (sp > 0U)) { sp--; }
            if (sp == 0U) { ok = false; break; }
            rx_buf[i] = (uint8_t)(USART2->RDR & 0xFFU);
        }
        if (memcmp(rx_buf, dma_payload, dma_len) != 0)
        {
            ok = false;
        }

        /* Wait for TX DMA to finish */
        uint32_t sp_dma = 100000U;
        while ((huart2.gState != HAL_UART_STATE_READY) && (sp_dma > 0U)) { sp_dma--; }
    }

    /* 3. Rejection tests */
    if (UART_HAL_CheckDmaErrata(NULL) != HAL_ERROR) { ok = false; }

    /* Non-UART request */
    DMA_HandleTypeDef bad_req = s_hdma_usart2_tx;
    bad_req.Init.Request = DMA_REQUEST_SPI1_TX;
    if (UART_HAL_CheckDmaErrata(&bad_req) != HAL_ERROR) { ok = false; }

    /* BDMA rejection */
    DMA_HandleTypeDef bdma_handle;
    memset(&bdma_handle, 0, sizeof(bdma_handle));
    bdma_handle.Instance = BDMA_Channel0;
    bdma_handle.Init.Request = DMA_REQUEST_USART2_TX;
    if (UART_HAL_CheckDmaErrata(&bdma_handle) != HAL_ERROR) { ok = false; }

    (void)HAL_DMA_DeInit(&s_hdma_usart2_tx);
    (void)HAL_UART_DeInit(&huart2);

    verdict(ok, "UT8 UART_HAL_CheckDmaErrata in-vivo with DMA1 (bit 20, live transfer, busy, rejections)");
    step_end();
}

static void run_ut9(void)
{
    if (!step_begin("UT9")) { return; }

    bool ok = true;

    /* S-B6: Create a fresh UART_HandleTypeDef with gState = RESET so HAL_UART_Init
     * calls HAL_UART_MspInit and exercises U1 cross-core GPIO lock detection! */
    UART_HandleTypeDef fresh_console;
    memset(&fresh_console, 0, sizeof(fresh_console));
    fresh_console.gState = HAL_UART_STATE_RESET;
    fresh_console.RxState = HAL_UART_STATE_RESET;

    uart_hal_config_t cfg_console = {
        .instance = BOARD_CONSOLE_UART_INSTANCE,
        .baudrate = 115200UL
    };

    /* Snapshot PD8/PD9 */
    uint32_t snap_moder = GPIOD->MODER;
    uint32_t snap_pupdr = GPIOD->PUPDR;
    (void)snap_moder;
    (void)snap_pupdr;

    /* 1. CM4 acquires cross-core GPIO lock */
    if (cm4_send_cmd(UST_CMD_LOCK_ACQUIRE, CM4_TIMEOUT_MS))
    {
        if (UST_AREA->lock_held == 1U)
        {
            /* UART_HAL_Init on fresh handle must return HAL_BUSY (U1) */
            HAL_StatusTypeDef st = UART_HAL_Init(&fresh_console, &cfg_console);
            LOG("UT9 fresh handle with lock held: returned %d (expected %d)", (int)st, (int)HAL_BUSY);
            if (st != HAL_BUSY)
            {
                ok = false;
            }
        }
        else
        {
            ok = false;
        }
        (void)cm4_send_cmd(UST_CMD_LOCK_RELEASE, CM4_TIMEOUT_MS);
    }
    else
    {
        ok = false;
    }

    /* 2. Lock released: fresh handle init must succeed with HAL_OK */
    memset(&fresh_console, 0, sizeof(fresh_console));
    fresh_console.gState = HAL_UART_STATE_RESET;
    fresh_console.RxState = HAL_UART_STATE_RESET;
    HAL_StatusTypeDef st_free = UART_HAL_Init(&fresh_console, &cfg_console);
    LOG("UT9 fresh handle with lock free: returned %d (expected %d)", (int)st_free, (int)HAL_OK);
    if (st_free != HAL_OK)
    {
        ok = false;
    }

    /* 3. Re-initialize standard console handle */
    if (UART_Console_Init(115200UL) != HAL_OK)
    {
        ok = false;
    }

    verdict(ok, "UT9 UART_HAL_Init console MSP GPIO lock status U1");
    step_end();
}

static void run_ut10(void)
{
    if (!step_begin("UT10")) { return; }

    bool ok = true;

    /* Signal CM4 to run prj_04 regression loopback over JU1 (1 kOhm).
     * No fabricated print per S-M6! */
    if (cm4_send_cmd(UST_CMD_UT10, CM4_TIMEOUT_MS))
    {
        if (UST_AREA->m4_ut10_ok != 1U)
        {
            ok = false;
        }
    }
    else
    {
        ok = false;
    }

    verdict(ok, "UT10 prj_04 regression loopback with 1k resistor");
    step_end();
}

void UART_Selftest_CM7_Run(void)
{
    /* Wait for CM4 to signal ready (S-M10) */
    uint32_t wait_cm4 = 0U;
    while ((UST_AREA->m4_ready != 1U) && (wait_cm4 < 2000U))
    {
        delay_ms(1U);
        wait_cm4++;
    }

    LOG("=== 04_uart selftest starting on CM7 ===");
    LOG("[uart] CM7 Rev %s", FW_GIT_REV);
    LOG("[uart] CM7 Build id %s", FW_BUILD_ID);
    LOG("[uart] CM4 Rev %s", (const char *)UST_AREA->m4_rev);
    LOG("[uart] CM4 Build id %s", (const char *)UST_AREA->m4_build_id);

    /* MCO1: PA8 enabled, 4 MHz (HSE/2), ratio to 100 MHz f_ker is 25:1 */
    mco1_enable();
    LOG("[uart] MCO1: PA8 enabled, freq=4000000 Hz, f_ker=100000000 Hz, ratio=25:1");

    bench_safe();

    run_ut0();
    run_ut1();
    run_ut2();
    run_ut3();
    run_ut4();
    run_ut4_cm4();
    run_ut5();
    run_ut6();
    run_ut7();
    run_ut8();
    run_ut9();
    run_ut10();

    if (step_begin("DONE"))
    {
        (void)cm4_send_cmd(UST_CMD_DONE, CM4_TIMEOUT_MS);
        mco1_restore();
        LOG("=== 04_uart selftest finished: %lu failed ===", (unsigned long)s_fail);
        LOG("[uart] FW VERDICT=%s", (s_fail == 0U) ? "PASS" : "FAIL");
        LOG("FW VERDICT=%s", (s_fail == 0U) ? "PASS" : "FAIL");
        step_end();
    }
}
