/**
  ******************************************************************************
  * @file    uart_selftest.c
  * @brief   04_uart selftest, CPU2 (Cortex-M4) side.
  *
  *          Coordinated by CM7 via UST_AREA in SRAM4 (0x3800E000):
  *          - UST_CMD_UT4: LL interrupt-driven reception over loopback (JU1)
  *          - UST_CMD_LOCK_ACQUIRE: take cross-core GPIO lock (BOARD_GPIO_HSEM_ID)
  *          - UST_CMD_LOCK_RELEASE: release cross-core GPIO lock
  *          - UST_CMD_UT10: prj_04 regression loopback tests over JU1
  *          - UST_CMD_DONE: exit selftest to application loop
  ******************************************************************************
  */

#include "uart_selftest.h"
#include "shared_memory.h"
#include "hsem_bm.h"
#include "uart_bm.h"
#include "uart_ll.h"
#include "fw_build_id.h"
#include "gpio_bm.h"

#include <string.h>

static uint8_t             s_cm4_rx_buf[64];
static UartLL_RingBuffer_t s_cm4_ring = {
    .data = s_cm4_rx_buf,
    .size = sizeof(s_cm4_rx_buf),
    .head = 0U,
    .tail = 0U
};
static UartLL_Handle_t     s_cm4_uart;

void USART2_IRQHandler(void)
{
    UART_LL_IrqService(&s_cm4_uart, &s_cm4_ring);
}

static void copy_field(volatile char *dst, uint32_t max_len, const char *src)
{
    uint32_t i = 0U;
    while ((i + 1U < max_len) && (src[i] != '\0'))
    {
        dst[i] = src[i];
        i++;
    }
    while (i < max_len)
    {
        dst[i++] = '\0';
    }
}

static bool run_cm4_ut4(void)
{
    static const UartLL_Pin_t tx_pin = {
        .port    = UART_BENCH_PORT_USART,
        .pin_idx = UART_PIN_TX,
        .af      = 7U
    };
    static const UartLL_Pin_t rx_pin = {
        .port    = UART_BENCH_PORT_USART,
        .pin_idx = UART_PIN_RX,
        .af      = 7U
    };

    const UartLL_Config_t cfg = {
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
        .tx_pin          = &tx_pin,
        .rx_pin          = &rx_pin,
    };

    s_cm4_ring.head = 0U;
    s_cm4_ring.tail = 0U;

    if (!UART_LL_Init(&s_cm4_uart, &cfg))
    {
        return false;
    }

    if (!UART_LL_EnableRxIrq(&s_cm4_uart, USART2_IRQn, 5U))
    {
        (void)UART_LL_Deinit(&s_cm4_uart);
        return false;
    }

    static const uint8_t payload[] = "M4_UART_UT4_TEST";
    const size_t len = sizeof(payload) - 1U;

    size_t written = UART_LL_Write(&s_cm4_uart, payload, len, 50000U);
    (void)UART_LL_Flush(&s_cm4_uart, 50000U);

    if (written != len)
    {
        UART_LL_DisableRxIrq(&s_cm4_uart, USART2_IRQn);
        (void)UART_LL_Deinit(&s_cm4_uart);
        return false;
    }

    /* Wait up to 100 ms for reception */
    uint32_t spin = 200000U;
    while ((UartLL_RingBuffer_Count(&s_cm4_ring) < len) && (spin > 0U))
    {
        spin--;
    }

    bool match = true;
    if (UartLL_RingBuffer_Count(&s_cm4_ring) != len)
    {
        match = false;
    }
    else
    {
        for (size_t i = 0U; i < len; i++)
        {
            uint8_t b = 0U;
            if (!UartLL_RingBuffer_Get(&s_cm4_ring, &b) || (b != payload[i]))
            {
                match = false;
                break;
            }
        }
    }

    UART_LL_DisableRxIrq(&s_cm4_uart, USART2_IRQn);
    (void)UART_LL_Deinit(&s_cm4_uart);

    /* Restore pins to input */
    (void)GPIO_BM_ConfigPin(&(GpioBm_PinConfig_t){
        .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .mode = GPIO_BM_MODE_INPUT, .allow_debug_pins = false
    });
    (void)GPIO_BM_ConfigPin(&(GpioBm_PinConfig_t){
        .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .mode = GPIO_BM_MODE_INPUT, .allow_debug_pins = false
    });

    return match;
}

static bool run_cm4_ut10(void)
{
    /* prj_04 regression: BM loop (9600, 115200, 921600), LL loop (115200, 921600), ORE */
    static const uint32_t bauds_bm[] = { 9600UL, 115200UL, 921600UL };
    static const uint32_t bauds_ll[] = { 115200UL, 921600UL };

    static const UartBm_Pin_t bm_tx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .af = 7U };
    static const UartBm_Pin_t bm_rx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .af = 7U };

    /* 1. BM loops */
    for (size_t i = 0; i < 3; i++)
    {
        UartBm_Config_t cfg = {
            .instance        = USART2,
            .kernel_clock_hz = 100000000UL,
            .baudrate        = bauds_bm[i],
            .data_bits       = UART_BM_DATA_BITS_8,
            .stop_bits       = UART_BM_STOP_BITS_1,
            .parity          = UART_BM_PARITY_NONE,
            .enable_clock    = true,
            .oversampling_8  = (bauds_bm[i] == 921600UL),
            .fifo_enable     = true,
            .tx_pin          = &bm_tx,
            .rx_pin          = &bm_rx,
        };
        UartBm_Handle_t h;
        if (!UART_BM_Init(&h, &cfg)) { return false; }

        for (uint8_t b = 0x10U; b < 0x18U; b++)
        {
            if (!UART_BM_WriteByte(&h, b)) { (void)UART_BM_Deinit(&h); return false; }
            uint8_t rx = 0U;
            uint32_t sp = 100000U;
            while (!UART_BM_ReadByte(&h, &rx) && (sp > 0U)) { sp--; }
            if (rx != b) { (void)UART_BM_Deinit(&h); return false; }
        }
        (void)UART_BM_Deinit(&h);
    }

    /* 2. LL loops */
    static const UartLL_Pin_t ll_tx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .af = 7U };
    static const UartLL_Pin_t ll_rx = { .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .af = 7U };

    for (size_t i = 0; i < 2; i++)
    {
        UartLL_Config_t cfg = {
            .instance        = USART2,
            .kernel_clock_hz = 100000000UL,
            .baudrate        = bauds_ll[i],
            .data_bits       = LL_USART_DATAWIDTH_8B,
            .stop_bits       = LL_USART_STOPBITS_1,
            .parity          = LL_USART_PARITY_NONE,
            .oversampling    = (bauds_ll[i] == 921600UL) ? LL_USART_OVERSAMPLING_8 : LL_USART_OVERSAMPLING_16,
            .fifo_enable     = true,
            .hw_flow         = LL_USART_HWCONTROL_NONE,
            .enable_clock    = true,
            .tx_pin          = &ll_tx,
            .rx_pin          = &ll_rx,
        };
        UartLL_Handle_t h;
        if (!UART_LL_Init(&h, &cfg)) { return false; }

        for (uint8_t b = 0x20U; b < 0x28U; b++)
        {
            if (!UART_LL_WriteByte(&h, b)) { (void)UART_LL_Deinit(&h); return false; }
            uint8_t rx = 0U;
            uint32_t sp = 100000U;
            while (!UART_LL_ReadByte(&h, &rx) && (sp > 0U)) { sp--; }
            if (rx != b) { (void)UART_LL_Deinit(&h); return false; }
        }
        (void)UART_LL_Deinit(&h);
    }

    /* 3. ORE test: without FIFO, send 3 bytes, read 1, assert ORE */
    {
        UartLL_Config_t cfg = {
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
        UartLL_Handle_t h;
        if (!UART_LL_Init(&h, &cfg)) { return false; }

        /* Write 4 bytes with small delay to overrun single RDR */
        for (uint8_t b = 0; b < 4; b++)
        {
            (void)UART_LL_WriteByte(&h, 0x55U + b);
            for (volatile uint32_t d = 0; d < 5000U; d++) {}
        }
        (void)UART_LL_Flush(&h, 100000U);

        uint8_t dummy = 0U;
        (void)UART_LL_ReadByte(&h, &dummy);
        uint32_t errs = UART_LL_TakeErrors(&h);
        (void)UART_LL_Deinit(&h);

        if ((errs & UART_LL_ERR_ORE) == 0U)
        {
            return false;
        }
    }

    /* Clean pins */
    (void)GPIO_BM_ConfigPin(&(GpioBm_PinConfig_t){
        .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .mode = GPIO_BM_MODE_INPUT, .allow_debug_pins = false
    });
    (void)GPIO_BM_ConfigPin(&(GpioBm_PinConfig_t){
        .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .mode = GPIO_BM_MODE_INPUT, .allow_debug_pins = false
    });

    return true;
}

void UART_Selftest_CM4_Run(void)
{
    if (UST_AREA->magic != UST_AREA_MAGIC)
    {
        return;
    }

    copy_field(UST_AREA->m4_rev, sizeof(UST_AREA->m4_rev), FW_GIT_REV);
    copy_field(UST_AREA->m4_build_id, sizeof(UST_AREA->m4_build_id), FW_BUILD_ID);
    __DMB();
    UST_AREA->m4_ready = UST_AREA->session;

    uint32_t last_seq = 0U;

    for (;;)
    {
        while (UST_AREA->cmd_seq == last_seq)
        {
            /* Spin waiting for command from CM7 */
        }

        uint32_t cmd = UST_AREA->cmd;
        last_seq     = UST_AREA->cmd_seq;

        switch (cmd)
        {
            case UST_CMD_UT4:
                UST_AREA->m4_ut4_ok = run_cm4_ut4() ? 1U : 0U;
                break;

            case UST_CMD_LOCK_ACQUIRE:
                if (HSEM_BM_TakeTimeout(BOARD_GPIO_HSEM_ID, 0U, 1000000UL) == HSEM_BM_OK)
                {
                    UST_AREA->lock_held = 1U;
                }
                else
                {
                    UST_AREA->lock_held = 0U;
                }
                break;

            case UST_CMD_LOCK_RELEASE:
                (void)HSEM_BM_Release(BOARD_GPIO_HSEM_ID, 0U);
                UST_AREA->lock_held = 0U;
                break;

            case UST_CMD_UT10:
                UST_AREA->m4_ut10_ok = run_cm4_ut10() ? 1U : 0U;
                break;

            case UST_CMD_DONE:
            default:
                UST_AREA->ack_seq = last_seq;
                __DMB();
                return;
        }

        __DMB();
        UST_AREA->ack_seq = last_seq;
        __DMB();
    }
}
