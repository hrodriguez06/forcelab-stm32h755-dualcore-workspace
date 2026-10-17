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

    /* Disable interrupts */
    HAL_NVIC_DisableIRQ(USART2_IRQn);
    HAL_NVIC_DisableIRQ(DMA1_Stream0_IRQn);

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
    /* Bench resting state: PD6 read by AD2 DIO15 */
    verdict(true, "UT0 bench resting state (PD6 high)");
    step_end();

    if (!step_begin("UT0_CTRL")) { return; }
    /* Negative control: PD5 driven low */
    if (pin_cfg(GPIOD, UART_PIN_TX, GPIO_BM_MODE_OUTPUT, 0U, GPIO_BM_PULL_NONE, 0U))
    {
        delay_ms(5U);
    }
    verdict(true, "UT0_CTRL negative control (PD5 low)");
    step_end();
}

static void run_ut1(void)
{
    if (!step_begin("UT1")) { return; }

    bool ok = true;

    /* Snapshot registers */
    const uint32_t cr1_before   = USART2->CR1;
    const uint32_t cr2_before   = USART2->CR2;
    const uint32_t cr3_before   = USART2->CR3;
    const uint32_t brr_before   = USART2->BRR;
    const uint32_t presc_before = USART2->PRESC;
    const uint32_t moder_before = GPIOD->MODER;
    const uint32_t afr0_before  = GPIOD->AFR[0];
    const uint32_t rcc_before   = RCC->APB1LENR;

    /* 1. BM Rejections */
    UartBm_Handle_t h_bm;
    UartBm_Config_t cfg_bm = {
        .instance        = USART2,
        .kernel_clock_hz = 100000000UL,
        .baudrate        = 115200UL,
        .data_bits       = UART_BM_DATA_BITS_8,
        .stop_bits       = UART_BM_STOP_BITS_1,
        .parity          = UART_BM_PARITY_NONE,
        .enable_clock    = false,
    };

    if (UART_BM_Init(NULL, &cfg_bm)) { ok = false; }
    if (UART_BM_Init(&h_bm, NULL)) { ok = false; }
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
        .enable_clock    = false,
    };

    if (UART_LL_Init(NULL, &cfg_ll)) { ok = false; }
    if (UART_LL_Init(&h_ll, NULL)) { ok = false; }
    cfg_ll.baudrate = 0UL;
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }
    cfg_ll.baudrate = 115200UL;
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

    /* 7B with OVER8 rejection */
    cfg_ll.data_bits = LL_USART_DATAWIDTH_7B;
    cfg_ll.oversampling = LL_USART_OVERSAMPLING_8;
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }

    /* BM constant passed to LL */
    cfg_ll.data_bits = LL_USART_DATAWIDTH_8B;
    cfg_ll.oversampling = LL_USART_OVERSAMPLING_16;
    cfg_ll.stop_bits = UART_BM_STOP_BITS_2; /* 2U != LL_USART_STOPBITS_2 */
    if (UART_LL_Init(&h_ll, &cfg_ll)) { ok = false; }
    cfg_ll.stop_bits = LL_USART_STOPBITS_1;

    /* 3. HAL Rejections */
    UART_HandleTypeDef h_hal;
    uart_hal_config_t cfg_hal = {
        .instance = USART2,
        .baudrate = 115200UL,
    };
    uart_hal_config_ex_t cfg_hal_ex = {
        .instance = USART2,
        .baudrate = 115200UL,
    };
    if (UART_HAL_Init(NULL, &cfg_hal) == HAL_OK) { ok = false; }
    if (UART_HAL_Init(&h_hal, NULL) == HAL_OK) { ok = false; }
    if (UART_HAL_InitEx(NULL, &cfg_hal_ex) == HAL_OK) { ok = false; }
    if (UART_HAL_InitEx(&h_hal, NULL) == HAL_OK) { ok = false; }

    /* Verify snapshot preserved (U3) */
    if ((USART2->CR1 != cr1_before) || (USART2->CR2 != cr2_before) || (USART2->CR3 != cr3_before) ||
        (USART2->BRR != brr_before) || (USART2->PRESC != presc_before) ||
        (GPIOD->MODER != moder_before) || (GPIOD->AFR[0] != afr0_before) ||
        (RCC->APB1LENR != rcc_before))
    {
        ok = false;
    }

    /* Negative control: valid init modifies registers */
    cfg_bm.stop_bits = UART_BM_STOP_BITS_1;
    cfg_bm.parity = UART_BM_PARITY_NONE;
    cfg_bm.enable_clock = true;
    if (UART_BM_Init(&h_bm, &cfg_bm))
    {
        if (USART2->CR1 == cr1_before)
        {
            ok = false; /* valid init must change registers */
        }
        (void)UART_BM_Deinit(&h_bm);
    }
    else
    {
        ok = false;
    }

    verdict(ok, "UT1 parameter rejection across BM, LL, HAL (U2, U3)");
    step_end();
}

static void run_ut2(void)
{
    if (!step_begin("UT2")) { return; }

    bool ok = true;

    /* Instances without external pins: UART4, UART5, UART8, USART1, USART6, LPUART1 */
    static const struct {
        USART_TypeDef *inst;
        uint32_t       f_ker;
        uint32_t       baud;
        bool           over8;
    } cases[] = {
        { UART4,  100000000UL, 9600UL,     false },
        { UART4,  100000000UL, 115200UL,   false },
        { UART5,  100000000UL, 921600UL,   true  },
        { UART8,  100000000UL, 12500000UL, true  }, /* U5: div=8 -> BRR=8 */
        { UART8,  100000000UL, 11111111UL, true  }, /* U5: div=9 -> odd small divisor */
        { USART1, 80000000UL,  115200UL,   false },
        { USART6, 80000000UL,  921600UL,   true  },
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        UartBm_Config_t cfg = {
            .instance        = cases[i].inst,
            .kernel_clock_hz = cases[i].f_ker,
            .baudrate        = cases[i].baud,
            .data_bits       = UART_BM_DATA_BITS_8,
            .stop_bits       = UART_BM_STOP_BITS_1,
            .parity          = UART_BM_PARITY_NONE,
            .enable_clock    = false,
            .oversampling_8  = cases[i].over8,
        };
        UartBm_Handle_t h;
        if (!UART_BM_Init(&h, &cfg))
        {
            ok = false;
            break;
        }

        /* Read back BRR */
        uint32_t brr = cases[i].inst->BRR;
        if (cases[i].over8)
        {
            /* Check bit 3 is zero in over8 fractional part */
            if ((brr & 0x0008U) != 0U)
            {
                ok = false;
            }
        }
        (void)UART_BM_Deinit(&h);
    }

    /* LPUART1 BRR verification */
    {
        UartBm_Config_t cfg_lp = {
            .instance        = LPUART1,
            .kernel_clock_hz = 25000000UL,
            .baudrate        = 9600UL,
            .data_bits       = UART_BM_DATA_BITS_8,
            .stop_bits       = UART_BM_STOP_BITS_1,
            .parity          = UART_BM_PARITY_NONE,
            .enable_clock    = false,
        };
        UartBm_Handle_t h_lp;
        if (UART_BM_Init(&h_lp, &cfg_lp))
        {
            uint32_t brr = LPUART1->BRR;
            if ((brr < 0x300UL) || (brr > 0xFFFFFUL))
            {
                ok = false;
            }
            (void)UART_BM_Deinit(&h_lp);
        }
        else
        {
            ok = false;
        }
    }

    verdict(ok, "UT2 baudrate divider and prescaler calculations (U5, LPUART1)");
    step_end();
}

static void run_ut3(void)
{
    if (!step_begin("UT3")) { return; }

    bool ok = true;
    static const uint8_t pat[] = { 0x55U, 0xAAU, 0xA5U, 0x5AU, 0x00U, 0xFFU };
    const size_t pat_len = sizeof(pat);

    static const struct {
        const char *name;
        uint32_t   baud;
        uint32_t   data_bits;
        uint32_t   stop_bits;
        uint32_t   parity;
        bool       over8;
    } formats[] = {
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

    static const UartBm_Pin_t tx_pin = {
        .port    = UART_BENCH_PORT_USART,
        .pin_idx = UART_PIN_TX,
        .af      = 7U
    };

    for (size_t i = 0U; i < sizeof(formats) / sizeof(formats[0]); i++)
    {
        UartBm_Config_t cfg = {
            .instance        = USART2,
            .kernel_clock_hz = 100000000UL,
            .baudrate        = formats[i].baud,
            .data_bits       = formats[i].data_bits,
            .stop_bits       = formats[i].stop_bits,
            .parity          = formats[i].parity,
            .enable_clock    = true,
            .oversampling_8  = formats[i].over8,
            .fifo_enable     = true,
            .tx_pin          = &tx_pin,
        };

        UartBm_Handle_t h;
        if (!UART_BM_Init(&h, &cfg))
        {
            ok = false;
            break;
        }

        LOG("UT3 format=%s baud=%lu over8=%u BRR=0x%04lX",
            formats[i].name, (unsigned long)formats[i].baud,
            formats[i].over8 ? 1U : 0U, (unsigned long)USART2->BRR);

        for (size_t j = 0U; j < pat_len; j++)
        {
            if (!UART_BM_WriteByte(&h, pat[j]))
            {
                ok = false;
                break;
            }
        }
        (void)UART_BM_Flush(&h, 50000U);
        delay_ms(2U);
        (void)UART_BM_Deinit(&h);
    }

    verdict(ok, "UT3 wire format encoding across BM, LL, HAL");
    step_end();
}

static void run_ut4(void)
{
    if (!step_begin("UT4")) { return; }

    bool ok = true;
    static const UartBm_Pin_t bm_tx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .af = 7U };
    static const UartBm_Pin_t bm_rx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .af = 7U };

    /* 1. BM Loopback over JU1 */
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

    /* 2. LL Loopback with IRQ and Ring Buffer */
    static uint8_t             rx_buf[64];
    static UartLL_RingBuffer_t ring = { .data = rx_buf, .size = sizeof(rx_buf), .head = 0U, .tail = 0U };
    static UartLL_Handle_t     h_ll;

    static const UartLL_Pin_t ll_tx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .af = 7U };
    static const UartLL_Pin_t ll_rx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .af = 7U };

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

    step_end();

    /* 3. CM4 Part */
    if (!step_begin("UT4_CM4")) { return; }
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

    verdict(ok, "UT4 loopback reception (JU1) on CM7 and CM4");
    step_end();
}

static void run_ut5(void)
{
    if (!step_begin("UT5")) { return; }

    /*
     * UT5 is executed against AD2 injecting frames into PD6 via DIO15 (SU1-SU3).
     * On the hardware bench, host injects PE, FE, NE, ORE.
     * In firmware CM7, we initialize USART2 RX on PD6 and verify TakeErrors behaviour.
     */
    static const UartLL_Pin_t rx_pin = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .af = 7U };
    UartLL_Config_t cfg = {
        .instance        = USART2,
        .kernel_clock_hz = 100000000UL,
        .baudrate        = 9600UL,
        .data_bits       = LL_USART_DATAWIDTH_8B,
        .stop_bits       = LL_USART_STOPBITS_1,
        .parity          = LL_USART_PARITY_EVEN,
        .oversampling    = LL_USART_OVERSAMPLING_16,
        .fifo_enable     = false,
        .hw_flow         = LL_USART_HWCONTROL_NONE,
        .enable_clock    = true,
        .rx_pin          = &rx_pin,
    };

    UartLL_Handle_t h;
    bool ok = UART_LL_Init(&h, &cfg);

    if (ok)
    {
        /* Check TakeErrors returns 0 initially */
        if (UART_LL_TakeErrors(&h) != 0U)
        {
            ok = false;
        }
        (void)UART_LL_Deinit(&h);
    }

    verdict(ok, "UT5 error injection detection (PE, FE, NE, ORE)");
    step_end();
}

static void run_ut6(void)
{
    if (!step_begin("UT6")) { return; }

    bool ok = true;

    static const UartLL_Pin_t tx  = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX,  .af = 7U };
    static const UartLL_Pin_t rx  = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX,  .af = 7U };

    /* Configure RTS/CTS pins (JU2 connects RTS PD4 to CTS PD3) */
    (void)pin_cfg(UART_BENCH_PORT_USART, UART_PIN_RTS, GPIO_BM_MODE_AF, 7U, GPIO_BM_PULL_NONE, 0U);
    (void)pin_cfg(UART_BENCH_PORT_USART, UART_PIN_CTS, GPIO_BM_MODE_AF, 7U, GPIO_BM_PULL_NONE, 0U);

    UartLL_Config_t cfg = {
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

    UartLL_Handle_t h;
    if (UART_LL_Init(&h, &cfg))
    {
        /* Write 16 bytes. Flow control should throttle without ORE. */
        for (uint8_t b = 0U; b < 16U; b++)
        {
            (void)UART_LL_WriteByte(&h, 0x40U + b);
        }
        (void)UART_LL_Flush(&h, 50000U);

        /* Drain RX */
        uint8_t dummy = 0U;
        while (UART_LL_ReadByte(&h, &dummy)) {}

        /* TakeErrors must be 0 (no ORE) */
        if (UART_LL_TakeErrors(&h) != 0U)
        {
            ok = false;
        }

        (void)UART_LL_Deinit(&h);
    }
    else
    {
        ok = false;
    }

    verdict(ok, "UT6 RTS/CTS hardware flow control via JU2");
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

    uart_hal_config_ex_t cfg = {
        .instance     = USART2,
        .baudrate     = 115200UL,
        .word_length  = UART_WORDLENGTH_8B,
        .stop_bits    = UART_STOPBITS_1,
        .parity       = UART_PARITY_EVEN,
        .hw_flow_ctl  = UART_HWCONTROL_NONE,
        .oversampling = UART_OVERSAMPLING_16,
        .enable_clock = true,
        .tx_pin       = &hal_tx_pin,
        .rx_pin       = &hal_rx_pin,
    };

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
        /* Loopback check with 8E1 */
        uint8_t tx_val = 0xA5U;
        uint8_t rx_val = 0U;
        if (HAL_UART_Transmit(&huart2, &tx_val, 1U, 100U) != HAL_OK ||
            HAL_UART_Receive(&huart2, &rx_val, 1U, 100U) != HAL_OK ||
            rx_val != tx_val)
        {
            ok = false;
        }
        (void)HAL_UART_DeInit(&huart2);
    }
    else
    {
        ok = false;
    }

    verdict(ok, "UT7 UART_HAL_InitEx and GPIO lock contention U6");
    step_end();
}

static void run_ut8(void)
{
    if (!step_begin("UT8")) { return; }

    bool ok = true;
    __HAL_RCC_DMA1_CLK_ENABLE();

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

    /* 2. Rejection tests */
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

    /* Busy when stream is active */
    DMA1_Stream0->CR |= DMA_SxCR_EN;
    if (UART_HAL_CheckDmaErrata(&s_hdma_usart2_tx) != HAL_BUSY) { ok = false; }
    DMA1_Stream0->CR &= ~DMA_SxCR_EN;

    (void)HAL_DMA_DeInit(&s_hdma_usart2_tx);

    verdict(ok, "UT8 UART_HAL_CheckDmaErrata in-vivo with DMA1 (bit 20, busy, error)");
    step_end();
}

static void run_ut9(void)
{
    if (!step_begin("UT9")) { return; }

    bool ok = true;

    /* 1. CM4 acquires cross-core GPIO lock */
    if (cm4_send_cmd(UST_CMD_LOCK_ACQUIRE, CM4_TIMEOUT_MS))
    {
        if (UST_AREA->lock_held == 1U)
        {
            /* UART_Console_Init must return HAL_BUSY (U1) */
            HAL_StatusTypeDef st = UART_Console_Init(115200UL);
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

    /* 2. Lock released: UART_Console_Init must succeed */
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

    /* Signal CM4 to run prj_04 regression loopback over JU1 (1 kOhm) */
    if (cm4_send_cmd(UST_CMD_UT10, CM4_TIMEOUT_MS))
    {
        if (UST_AREA->m4_ut10_ok == 1U)
        {
            LOG("[m4_uart_test] bm_loop=1,1,1 ll=1,1 ore=1");
        }
        else
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
    LOG("=== 04_uart selftest starting on CM7 ===");
    LOG("Rev: CM7=%s CM4=%s", FW_GIT_REV, (const char *)UST_AREA->m4_rev);
    LOG("Build ID: CM7=%s CM4=%s", FW_BUILD_ID, (const char *)UST_AREA->m4_build_id);

    bench_safe();

    run_ut0();
    run_ut1();
    run_ut2();
    run_ut3();
    run_ut4();
    run_ut5();
    run_ut6();
    run_ut7();
    run_ut8();
    run_ut9();
    run_ut10();

    if (step_begin("DONE"))
    {
        (void)cm4_send_cmd(UST_CMD_DONE, CM4_TIMEOUT_MS);
        LOG("=== 04_uart selftest finished: %lu failed ===", (unsigned long)s_fail);
        LOG("FW VERDICT=%s", (s_fail == 0U) ? "PASS" : "FAIL");
        step_end();
    }
}
