/**
  ******************************************************************************
  * @file    uart_ll.c
  * @brief   Low-Layer UART/USART/LPUART driver for STM32H755.
  *
  *          Driver tier: *_ll (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Based strictly on ST LL static inline functions and CMSIS core.
  ******************************************************************************
  */

#include "uart_ll.h"
#include "gpio_bm.h"

#define UART_LL_MAX_SPIN_ACK          100000UL
#define UART_LL_MAX_BAUD_ERROR_PCT    2ULL

typedef enum
{
    UART_LL_BUS_APB1L,
    UART_LL_BUS_APB2,
    UART_LL_BUS_APB4
} UartLL_Bus_t;

typedef struct
{
    USART_TypeDef *instance;
    UartLL_Bus_t   bus;
    uint32_t       enr_mask;
    bool           is_lpuart;
} UartLL_InstanceDescriptor_t;

static const UartLL_InstanceDescriptor_t k_ll_instances[] = {
    { USART1,  UART_LL_BUS_APB2,  RCC_APB2ENR_USART1EN,  false },
    { USART2,  UART_LL_BUS_APB1L, RCC_APB1LENR_USART2EN, false },
    { USART3,  UART_LL_BUS_APB1L, RCC_APB1LENR_USART3EN, false },
    { UART4,   UART_LL_BUS_APB1L, RCC_APB1LENR_UART4EN,  false },
    { UART5,   UART_LL_BUS_APB1L, RCC_APB1LENR_UART5EN,  false },
    { USART6,  UART_LL_BUS_APB2,  RCC_APB2ENR_USART6EN,  false },
    { UART7,   UART_LL_BUS_APB1L, RCC_APB1LENR_UART7EN,  false },
    { UART8,   UART_LL_BUS_APB1L, RCC_APB1LENR_UART8EN,  false },
    { LPUART1, UART_LL_BUS_APB4,  RCC_APB4ENR_LPUART1EN, true  }
};

#define UART_LL_INSTANCE_COUNT (sizeof(k_ll_instances) / sizeof(k_ll_instances[0]))

static const uint16_t k_ll_presc_divs[] = {
    1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256
};
#define UART_LL_PRESC_COUNT (sizeof(k_ll_presc_divs) / sizeof(k_ll_presc_divs[0]))

static const UartLL_InstanceDescriptor_t *UART_LL_FindDescriptor(const USART_TypeDef *instance)
{
    if (instance == NULL)
    {
        return NULL;
    }

    for (size_t i = 0; i < UART_LL_INSTANCE_COUNT; i++)
    {
        if (k_ll_instances[i].instance == instance)
        {
            return &k_ll_instances[i];
        }
    }

    return NULL;
}

static void UART_LL_EnableBusClock(UartLL_Bus_t bus, uint32_t mask)
{
    volatile uint32_t dummy;

    switch (bus)
    {
        case UART_LL_BUS_APB1L:
            RCC->APB1LENR |= mask;
            dummy = RCC->APB1LENR;
            break;

        case UART_LL_BUS_APB2:
            RCC->APB2ENR |= mask;
            dummy = RCC->APB2ENR;
            break;

        case UART_LL_BUS_APB4:
            RCC->APB4ENR |= mask;
            dummy = RCC->APB4ENR;
            break;
    }

    (void)dummy;
}

static bool UART_LL_SelectPrescaler(uint32_t ker_ck, uint32_t baud, bool is_lpuart,
                                    uint32_t oversampling, uint32_t *out_presc)
{
    for (size_t p = 0; p < UART_LL_PRESC_COUNT; p++)
    {
        uint32_t ker_p = ker_ck / k_ll_presc_divs[p];
        if (ker_p == 0U)
        {
            break;
        }

        if (is_lpuart)
        {
            /* RM0399 52.4.4: 3 * baud <= ker_presc <= 4096 * baud (64-bit safe) */
            if (((uint64_t)ker_p < 3ULL * baud) || ((uint64_t)ker_p > 4096ULL * baud))
            {
                continue;
            }

            /* ES0445 2.21.2: Avoid non-integer ker_p / baud in [3, 4] */
            uint32_t ratio_int = ker_p / baud;
            uint32_t ratio_rem = ker_p % baud;
            if ((ratio_int >= 3U) && (ratio_int <= 4U) && (ratio_rem != 0U))
            {
                continue;
            }

            uint64_t num = (256ULL * (uint64_t)ker_p) + ((uint64_t)baud / 2ULL);
            uint32_t brr = (uint32_t)(num / baud);
            if ((brr < 0x300UL) || (brr > 0xFFFFFUL))
            {
                continue;
            }

            uint32_t actual = (uint32_t)((256ULL * (uint64_t)ker_p) / brr);
            int32_t  diff   = (int32_t)actual - (int32_t)baud;
            if (diff < 0)
            {
                diff = -diff;
            }

            if (((uint64_t)diff * 100ULL) <= ((uint64_t)baud * UART_LL_MAX_BAUD_ERROR_PCT))
            {
                *out_presc = (uint32_t)p;
                return true;
            }
        }
        else
        {
            uint32_t usartdiv;
            uint32_t actual;

            if (oversampling == LL_USART_OVERSAMPLING_8)
            {
                usartdiv = (2U * ker_p + (baud / 2U)) / baud;
                if ((usartdiv < 16U) || (usartdiv > 0xFFFFU))
                {
                    continue;
                }

                /* RM0399 51.5.5: BRR[2:0] = USARTDIV[3:0] >> 1, BRR[3] = 0, BRR[15:4] = USARTDIV[15:4] */
                uint32_t brr = (usartdiv & 0xFFF0UL) | ((usartdiv & 0x000FUL) >> 1);

                /* Actual hardware division factor reconstructed from programmed BRR (U5) */
                uint32_t actual_div = (brr & 0xFFF0UL) | ((brr & 0x0007UL) << 1);
                if (actual_div == 0U)
                {
                    continue;
                }
                actual = (2U * ker_p) / actual_div;
            }
            else
            {
                usartdiv = (ker_p + (baud / 2U)) / baud;
                if ((usartdiv < 16U) || (usartdiv > 0xFFFFU))
                {
                    continue;
                }
                actual = ker_p / usartdiv;
            }

            int32_t diff = (int32_t)actual - (int32_t)baud;
            if (diff < 0)
            {
                diff = -diff;
            }

            if (((uint64_t)diff * 100ULL) <= ((uint64_t)baud * UART_LL_MAX_BAUD_ERROR_PCT))
            {
                *out_presc = (uint32_t)p;
                return true;
            }
        }
    }

    return false;
}

bool UART_LL_Init(UartLL_Handle_t *h, const UartLL_Config_t *cfg)
{
    if ((h == NULL) || (cfg == NULL) || (cfg->instance == NULL))
    {
        return false;
    }

    const UartLL_InstanceDescriptor_t *desc = UART_LL_FindDescriptor(cfg->instance);
    if (desc == NULL)
    {
        return false;
    }

    /* =========================================================================
     * Phase 1: Pure parameter validation & calculation (U2, U3).
     * ZERO hardware registers (GPIO, RCC, or USART) are touched before this completes.
     * ========================================================================= */

    /* 1.1 Data bits validation (LL_USART_DATAWIDTH_*) */
    if ((cfg->data_bits != LL_USART_DATAWIDTH_7B) &&
        (cfg->data_bits != LL_USART_DATAWIDTH_8B) &&
        (cfg->data_bits != LL_USART_DATAWIDTH_9B))
    {
        return false;
    }

    /* 1.2 Stop bits validation (LL_USART_STOPBITS_*) */
    if ((cfg->stop_bits != LL_USART_STOPBITS_0_5) &&
        (cfg->stop_bits != LL_USART_STOPBITS_1) &&
        (cfg->stop_bits != LL_USART_STOPBITS_1_5) &&
        (cfg->stop_bits != LL_USART_STOPBITS_2))
    {
        return false;
    }

    /* RM0399 §52.4.4: LPUART does not support 0.5 or 1.5 stop bits */
    if (desc->is_lpuart &&
        ((cfg->stop_bits == LL_USART_STOPBITS_0_5) || (cfg->stop_bits == LL_USART_STOPBITS_1_5)))
    {
        return false;
    }

    /* 1.3 Parity validation (LL_USART_PARITY_*) */
    if ((cfg->parity != LL_USART_PARITY_NONE) &&
        (cfg->parity != LL_USART_PARITY_EVEN) &&
        (cfg->parity != LL_USART_PARITY_ODD))
    {
        return false;
    }

    /* 1.4 Hardware flow control validation (LL_USART_HWCONTROL_*) */
    if ((cfg->hw_flow != LL_USART_HWCONTROL_NONE) &&
        (cfg->hw_flow != LL_USART_HWCONTROL_RTS) &&
        (cfg->hw_flow != LL_USART_HWCONTROL_CTS) &&
        (cfg->hw_flow != LL_USART_HWCONTROL_RTS_CTS))
    {
        return false;
    }

    /* 1.5 Oversampling validation (LL_USART_OVERSAMPLING_*) */
    if ((cfg->oversampling != LL_USART_OVERSAMPLING_16) &&
        (cfg->oversampling != LL_USART_OVERSAMPLING_8))
    {
        return false;
    }

    /* RM0399 §52.4.4: LPUART does not support OVER8 */
    if (desc->is_lpuart && (cfg->oversampling != LL_USART_OVERSAMPLING_16))
    {
        return false;
    }

    /* 1.6 Optional pin parameter validation without touching hardware */
    if (cfg->tx_pin != NULL)
    {
        if ((cfg->tx_pin->port == NULL) || (cfg->tx_pin->pin_idx > 15U) ||
            !GPIO_BM_IsValidPort(cfg->tx_pin->port))
        {
            return false;
        }
    }

    if (cfg->rx_pin != NULL)
    {
        if ((cfg->rx_pin->port == NULL) || (cfg->rx_pin->pin_idx > 15U) ||
            !GPIO_BM_IsValidPort(cfg->rx_pin->port))
        {
            return false;
        }
    }

    /* 1.7 Determine Prescaler and validate Baud Rate */
    uint32_t presc = 0;
    if (!UART_LL_SelectPrescaler(cfg->kernel_clock_hz, cfg->baudrate, desc->is_lpuart,
                                 cfg->oversampling, &presc))
    {
        return false;
    }

    /* =========================================================================
     * Phase 2: Hardware configuration.
     * All inputs verified; begin applying changes to hardware.
     * ========================================================================= */

    /* 2.1 Optional GPIO configuration */
    if (cfg->tx_pin != NULL)
    {
        if (!GPIO_BM_ConfigAlternate(cfg->tx_pin->port, cfg->tx_pin->pin_idx,
                                     cfg->tx_pin->af, GPIO_BM_PULL_UP,
                                     GPIO_BM_SPEED_VERY_HIGH))
        {
            return false;
        }
    }

    if (cfg->rx_pin != NULL)
    {
        if (!GPIO_BM_ConfigAlternate(cfg->rx_pin->port, cfg->rx_pin->pin_idx,
                                     cfg->rx_pin->af, GPIO_BM_PULL_UP,
                                     GPIO_BM_SPEED_VERY_HIGH))
        {
            return false;
        }
    }

    /* 2.2 Clock gating in RCC */
    if (cfg->enable_clock)
    {
        UART_LL_EnableBusClock(desc->bus, desc->enr_mask);
    }

    USART_TypeDef *u = cfg->instance;

    /* 2.3 Disable peripheral and reset control registers to known state (U4, RM0399 §51.5.2 / §52.4.2) */
    LL_USART_Disable(u);
    u->CR1 = 0U;
    u->CR2 = 0U;
    u->CR3 = 0U;

    /* 5. Set Prescaler and Baudrate via LL static inlines */
    if (desc->is_lpuart)
    {
        LL_LPUART_SetPrescaler(u, presc);
        LL_LPUART_SetBaudRate(u, cfg->kernel_clock_hz, presc, cfg->baudrate);
    }
    else
    {
        LL_USART_SetPrescaler(u, presc);
        LL_USART_SetBaudRate(u, cfg->kernel_clock_hz, presc, cfg->oversampling, cfg->baudrate);
    }

    /* 6. Framing, parity, stop bits, flow control */
    LL_USART_SetDataWidth(u, cfg->data_bits);
    LL_USART_SetStopBitsLength(u, cfg->stop_bits);
    LL_USART_SetParity(u, cfg->parity);
    LL_USART_SetHWFlowCtrl(u, cfg->hw_flow);

    if (!desc->is_lpuart)
    {
        LL_USART_SetOverSampling(u, cfg->oversampling);
    }

    if (cfg->fifo_enable)
    {
        LL_USART_EnableFIFO(u);
    }

    LL_USART_EnableDirectionTx(u);
    LL_USART_EnableDirectionRx(u);

    /* 7. Enable peripheral */
    LL_USART_Enable(u);

    /* 8. Spin until TEACK & REACK are set */
    uint32_t spin = UART_LL_MAX_SPIN_ACK;
    while ((!LL_USART_IsActiveFlag_TEACK(u) || !LL_USART_IsActiveFlag_REACK(u)) && (spin > 0U))
    {
        spin--;
    }

    if (spin == 0U)
    {
        return false;
    }

    h->instance     = u;
    h->is_lpuart    = desc->is_lpuart;
    h->fifo_enabled = cfg->fifo_enable;
    h->error_flags  = 0;

    return true;
}

bool UART_LL_Deinit(UartLL_Handle_t *h)
{
    if ((h == NULL) || (h->instance == NULL))
    {
        return false;
    }

    USART_TypeDef *u = h->instance;
    LL_USART_Disable(u);
    u->CR1 = 0;
    u->CR2 = 0;
    u->CR3 = 0;

    h->instance     = NULL;
    h->error_flags  = 0;
    h->fifo_enabled = false;

    return true;
}

bool UART_LL_WriteByte(UartLL_Handle_t *h, uint8_t b)
{
    if ((h == NULL) || (h->instance == NULL))
    {
        return false;
    }

    if (!LL_USART_IsActiveFlag_TXE_TXFNF(h->instance))
    {
        return false;
    }

    LL_USART_TransmitData8(h->instance, b);
    return true;
}

size_t UART_LL_Write(UartLL_Handle_t *h, const uint8_t *d, size_t n, uint32_t spin_limit)
{
    if ((h == NULL) || (h->instance == NULL) || (d == NULL) || (n == 0U))
    {
        return 0;
    }

    USART_TypeDef *u = h->instance;

    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = spin_limit;
        while (!LL_USART_IsActiveFlag_TXE_TXFNF(u) && (spin > 0U))
        {
            spin--;
        }

        if (spin == 0U)
        {
            return i;
        }

        LL_USART_TransmitData8(u, d[i]);
    }

    return n;
}

bool UART_LL_ReadByte(UartLL_Handle_t *h, uint8_t *b)
{
    if ((h == NULL) || (h->instance == NULL) || (b == NULL))
    {
        return false;
    }

    USART_TypeDef *u   = h->instance;
    uint32_t       err = 0;

    if (LL_USART_IsActiveFlag_ORE(u))
    {
        err |= UART_LL_ERR_ORE;
        LL_USART_ClearFlag_ORE(u);
    }
    if (LL_USART_IsActiveFlag_FE(u))
    {
        err |= UART_LL_ERR_FE;
        LL_USART_ClearFlag_FE(u);
    }
    if (LL_USART_IsActiveFlag_NE(u))
    {
        err |= UART_LL_ERR_NE;
        LL_USART_ClearFlag_NE(u);
    }
    if (LL_USART_IsActiveFlag_PE(u))
    {
        err |= UART_LL_ERR_PE;
        LL_USART_ClearFlag_PE(u);
    }

    h->error_flags |= err;

    if (!LL_USART_IsActiveFlag_RXNE_RXFNE(u))
    {
        return false;
    }

    *b = LL_USART_ReceiveData8(u);
    return true;
}

bool UART_LL_Flush(UartLL_Handle_t *h, uint32_t spin_limit)
{
    if ((h == NULL) || (h->instance == NULL))
    {
        return false;
    }

    USART_TypeDef *u    = h->instance;
    uint32_t       spin = spin_limit;

    while (!LL_USART_IsActiveFlag_TC(u) && (spin > 0U))
    {
        spin--;
    }

    return (spin > 0U);
}

uint32_t UART_LL_TakeErrors(UartLL_Handle_t *h)
{
    if (h == NULL)
    {
        return 0;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t flags = h->error_flags;
    h->error_flags = 0;
    __set_PRIMASK(primask);

    return flags;
}

bool UART_LL_EnableRxIrq(UartLL_Handle_t *h, IRQn_Type irqn, uint32_t priority)
{
    if ((h == NULL) || (h->instance == NULL))
    {
        return false;
    }

    USART_TypeDef *u = h->instance;

    LL_USART_EnableIT_RXNE_RXFNE(u);
    LL_USART_EnableIT_ERROR(u);

    NVIC_SetPriority(irqn, priority);
    NVIC_EnableIRQ(irqn);

    return true;
}

void UART_LL_DisableRxIrq(UartLL_Handle_t *h, IRQn_Type irqn)
{
    if ((h == NULL) || (h->instance == NULL))
    {
        return;
    }

    USART_TypeDef *u = h->instance;

    LL_USART_DisableIT_RXNE_RXFNE(u);
    LL_USART_DisableIT_ERROR(u);

    NVIC_DisableIRQ(irqn);
}

void UART_LL_IrqService(UartLL_Handle_t *h, UartLL_RingBuffer_t *ring)
{
    if ((h == NULL) || (h->instance == NULL))
    {
        return;
    }

    USART_TypeDef *u = h->instance;

    if (LL_USART_IsActiveFlag_ORE(u))
    {
        h->error_flags |= UART_LL_ERR_ORE;
        LL_USART_ClearFlag_ORE(u);
    }
    if (LL_USART_IsActiveFlag_FE(u))
    {
        h->error_flags |= UART_LL_ERR_FE;
        LL_USART_ClearFlag_FE(u);
    }
    if (LL_USART_IsActiveFlag_NE(u))
    {
        h->error_flags |= UART_LL_ERR_NE;
        LL_USART_ClearFlag_NE(u);
    }
    if (LL_USART_IsActiveFlag_PE(u))
    {
        h->error_flags |= UART_LL_ERR_PE;
        LL_USART_ClearFlag_PE(u);
    }

    while (LL_USART_IsActiveFlag_RXNE_RXFNE(u))
    {
        uint8_t byte = LL_USART_ReceiveData8(u);
        if (ring != NULL)
        {
            (void)UartLL_RingBuffer_Put(ring, byte);
        }
    }
}
