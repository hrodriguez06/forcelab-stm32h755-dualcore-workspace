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

static bool bench_claim(const GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af)
{
    /* Core 1U is Cortex-M4 */
    return ust_line_rule(1U, ust_step_index("UT4_CM4"), port, pin, mode, af, GPIO_BM_OTYPE_PP, GPIO_BM_PULL_NONE);
}

static void cm4_pins_safe(void)
{
    (void)GPIO_BM_ConfigPin(&(GpioBm_PinConfig_t){
        .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_TX, .mode = GPIO_BM_MODE_INPUT, .allow_debug_pins = false
    });
    (void)GPIO_BM_ConfigPin(&(GpioBm_PinConfig_t){
        .port = UART_BENCH_PORT_USART, .pin_idx = UART_PIN_RX, .mode = GPIO_BM_MODE_INPUT, .allow_debug_pins = false
    });
}

static bool run_cm4_ut4(void)
{
    if (!bench_claim(UART_BENCH_PORT_USART, UART_PIN_TX, GPIO_BM_MODE_AF, 7U) ||
        !bench_claim(UART_BENCH_PORT_USART, UART_PIN_RX, GPIO_BM_MODE_AF, 7U))
    {
        return false;
    }

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
        cm4_pins_safe();
        return false;
    }

    if (!UART_LL_EnableRxIrq(&s_cm4_uart, USART2_IRQn, 5U))
    {
        (void)UART_LL_Deinit(&s_cm4_uart);
        cm4_pins_safe();
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
        cm4_pins_safe();
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
    cm4_pins_safe();

    return match;
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
                if (HSEM_BM_Release(BOARD_GPIO_HSEM_ID, 0U) == HSEM_BM_OK)
                {
                    UST_AREA->lock_held = 0U;
                }
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
