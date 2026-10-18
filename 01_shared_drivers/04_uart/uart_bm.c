/**
  ******************************************************************************
  * @file    uart_bm.c
  * @brief   Bare-metal UART/USART/LPUART driver for STM32H755.
  *
  *          Driver tier: *_bm (DRIVER_ARCHITECTURE_GUIDELINES.md).
  *          Pure register access, zero ST library dependency, deterministic.
  ******************************************************************************
  */

#include "uart_bm.h"
#include "gpio_bm.h"

#define UART_BM_MAX_SPIN_ACK          100000UL
#define UART_BM_MAX_BAUD_ERROR_PCT    2ULL

typedef enum
{
    UART_BM_BUS_APB1L,
    UART_BM_BUS_APB2,
    UART_BM_BUS_APB4
} UartBm_Bus_t;

typedef struct
{
    USART_TypeDef *instance;
    UartBm_Bus_t   bus;
    uint32_t       enr_mask;
    bool           is_lpuart;
} UartBm_InstanceDescriptor_t;

static const UartBm_InstanceDescriptor_t k_instances[] = {
    { USART1,  UART_BM_BUS_APB2,  RCC_APB2ENR_USART1EN,  false },
    { USART2,  UART_BM_BUS_APB1L, RCC_APB1LENR_USART2EN, false },
    { USART3,  UART_BM_BUS_APB1L, RCC_APB1LENR_USART3EN, false },
    { UART4,   UART_BM_BUS_APB1L, RCC_APB1LENR_UART4EN,  false },
    { UART5,   UART_BM_BUS_APB1L, RCC_APB1LENR_UART5EN,  false },
    { USART6,  UART_BM_BUS_APB2,  RCC_APB2ENR_USART6EN,  false },
    { UART7,   UART_BM_BUS_APB1L, RCC_APB1LENR_UART7EN,  false },
    { UART8,   UART_BM_BUS_APB1L, RCC_APB1LENR_UART8EN,  false },
    { LPUART1, UART_BM_BUS_APB4,  RCC_APB4ENR_LPUART1EN, true  }
};

#define UART_BM_INSTANCE_COUNT (sizeof(k_instances) / sizeof(k_instances[0]))

/* Prescaler divider values indexed by PRESC[3:0] register value (RM0399 §51.8.14) */
static const uint16_t k_presc_divs[] = {
    1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256
};
#define UART_BM_PRESC_COUNT (sizeof(k_presc_divs) / sizeof(k_presc_divs[0]))

static const UartBm_InstanceDescriptor_t *UART_BM_FindDescriptor(const USART_TypeDef *instance)
{
    if (instance == NULL)
    {
        return NULL;
    }

    for (size_t i = 0; i < UART_BM_INSTANCE_COUNT; i++)
    {
        if (k_instances[i].instance == instance)
        {
            return &k_instances[i];
        }
    }

    return NULL;
}

static void UART_BM_EnableBusClock(UartBm_Bus_t bus, uint32_t mask)
{
    volatile uint32_t dummy;

    switch (bus)
    {
        case UART_BM_BUS_APB1L:
            RCC->APB1LENR |= mask;
            dummy = RCC->APB1LENR;
            break;

        case UART_BM_BUS_APB2:
            RCC->APB2ENR |= mask;
            dummy = RCC->APB2ENR;
            break;

        case UART_BM_BUS_APB4:
            RCC->APB4ENR |= mask;
            dummy = RCC->APB4ENR;
            break;
    }

    (void)dummy;
}

static bool UART_BM_CalcBrrUsart(uint32_t ker_ck, uint32_t baud, bool over8,
                                uint32_t *out_brr, uint32_t *out_presc)
{
    if ((ker_ck == 0U) || (baud == 0U) || (out_brr == NULL) || (out_presc == NULL))
    {
        return false;
    }

    for (size_t p = 0; p < UART_BM_PRESC_COUNT; p++)
    {
        uint32_t ker_p = ker_ck / k_presc_divs[p];
        if (ker_p == 0U)
        {
            break;
        }

        uint32_t usartdiv;
        uint32_t brr;
        uint32_t actual;

        if (over8)
        {
            /* OVER8: usartdiv = 2 * ker_p / baud with rounding */
            usartdiv = (2U * ker_p + (baud / 2U)) / baud;
            if ((usartdiv < 16U) || (usartdiv > 0xFFFFU))
            {
                continue;
            }

            /* RM0399 51.5.5: BRR[2:0] = USARTDIV[3:0] >> 1, BRR[3] = 0, BRR[15:4] = USARTDIV[15:4] */
            brr = (usartdiv & 0xFFF0UL) | ((usartdiv & 0x000FUL) >> 1);

            /* Actual hardware division factor reconstructed from programmed BRR */
            uint32_t actual_div = (brr & 0xFFF0UL) | ((brr & 0x0007UL) << 1);
            if (actual_div == 0U)
            {
                continue;
            }
            actual = (2U * ker_p) / actual_div;
        }
        else
        {
            /* OVER16: usartdiv = ker_p / baud with rounding */
            usartdiv = (ker_p + (baud / 2U)) / baud;
            if ((usartdiv < 16U) || (usartdiv > 0xFFFFU))
            {
                continue;
            }

            brr    = usartdiv;
            actual = ker_p / brr;
        }

        /* Check error margin <= 2% */
        int32_t diff = (int32_t)actual - (int32_t)baud;
        if (diff < 0)
        {
            diff = -diff;
        }

        if (((uint64_t)diff * 100ULL) <= ((uint64_t)baud * UART_BM_MAX_BAUD_ERROR_PCT))
        {
            *out_brr   = brr;
            *out_presc = (uint32_t)p;
            return true;
        }
    }

    return false;
}

static bool UART_BM_CalcBrrLpuart(uint32_t ker_ck, uint32_t baud,
                                 uint32_t *out_brr, uint32_t *out_presc)
{
    if ((ker_ck == 0U) || (baud == 0U) || (out_brr == NULL) || (out_presc == NULL))
    {
        return false;
    }

    for (size_t p = 0; p < UART_BM_PRESC_COUNT; p++)
    {
        uint32_t ker_p = ker_ck / k_presc_divs[p];

        /* RM0399 52.4.4: 3 * baud <= ker_presc <= 4096 * baud (64-bit safe) */
        if (((uint64_t)ker_p < (3ULL * baud)) || ((uint64_t)ker_p > (4096ULL * baud)))
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

        /* LPUARTDIV = 256 * ker_p / baud with rounding */
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

        if (((uint64_t)diff * 100ULL) <= ((uint64_t)baud * UART_BM_MAX_BAUD_ERROR_PCT))
        {
            *out_brr   = brr;
            *out_presc = (uint32_t)p;
            return true;
        }
    }

    return false;
}

bool UART_BM_Init(UartBm_Handle_t *h, const UartBm_Config_t *cfg)
{
    if ((h == NULL) || (cfg == NULL) || (cfg->instance == NULL))
    {
        return false;
    }

    const UartBm_InstanceDescriptor_t *desc = UART_BM_FindDescriptor(cfg->instance);
    if (desc == NULL)
    {
        return false;
    }

    /* =========================================================================
     * Phase 1: Pure parameter validation & register pre-computation.
     * ZERO hardware registers (GPIO, RCC, or USART) are touched before this completes.
     * ========================================================================= */

    /* 1.1 Stop bits validation (LPUART does not support 0.5 or 1.5 stop bits) */
    if (desc->is_lpuart &&
        ((cfg->stop_bits == UART_BM_STOP_BITS_0_5) || (cfg->stop_bits == UART_BM_STOP_BITS_1_5)))
    {
        return false;
    }

    uint32_t cr2 = 0;
    switch (cfg->stop_bits)
    {
        case UART_BM_STOP_BITS_1:
            cr2 = 0;
            break;
        case UART_BM_STOP_BITS_0_5:
            cr2 = USART_CR2_STOP_0;
            break;
        case UART_BM_STOP_BITS_2:
            cr2 = USART_CR2_STOP_1;
            break;
        case UART_BM_STOP_BITS_1_5:
            cr2 = (USART_CR2_STOP_0 | USART_CR2_STOP_1);
            break;
        default:
            return false;
    }

    /* 1.2 Hardware flow control validation */
    uint32_t cr3 = 0;
    switch (cfg->hw_flow)
    {
        case UART_BM_HW_FLOW_NONE:
            break;
        case UART_BM_HW_FLOW_RTS:
            cr3 |= USART_CR3_RTSE;
            break;
        case UART_BM_HW_FLOW_CTS:
            cr3 |= USART_CR3_CTSE;
            break;
        case UART_BM_HW_FLOW_RTS_CTS:
            cr3 |= (USART_CR3_RTSE | USART_CR3_CTSE);
            break;
        default:
            return false;
    }

    /* 1.3 Parity validation */
    uint32_t cr1 = 0;
    bool has_parity = false;
    if (cfg->parity == UART_BM_PARITY_NONE)
    {
        has_parity = false;
    }
    else if (cfg->parity == UART_BM_PARITY_EVEN)
    {
        has_parity = true;
        cr1 |= USART_CR1_PCE;
    }
    else if (cfg->parity == UART_BM_PARITY_ODD)
    {
        has_parity = true;
        cr1 |= (USART_CR1_PCE | USART_CR1_PS);
    }
    else
    {
        return false;
    }

    /* 1.4 Word length validation (M[1:0] reflects TOTAL frame bits = data_bits + parity) */
    if (!has_parity)
    {
        switch (cfg->data_bits)
        {
            case UART_BM_DATA_BITS_7:
                cr1 |= USART_CR1_M1; /* 7 bits total */
                break;
            case UART_BM_DATA_BITS_8:
                /* 8 bits total: M1=0, M0=0 */
                break;
            case UART_BM_DATA_BITS_9:
                cr1 |= USART_CR1_M0; /* 9 bits total */
                break;
            default:
                return false;
        }
    }
    else
    {
        switch (cfg->data_bits)
        {
            case UART_BM_DATA_BITS_7:
                /* 7 data + 1 parity = 8 bits total: M1=0, M0=0 */
                break;
            case UART_BM_DATA_BITS_8:
                /* 8 data + 1 parity = 9 bits total: M1=0, M0=1 */
                cr1 |= USART_CR1_M0;
                break;
            case UART_BM_DATA_BITS_9:
                /* 9 data + 1 parity = 10 bits: not supported by hardware */
                return false;
            default:
                return false;
        }
    }

    /* 1.5 Baud rate / prescaler calculation */
    uint32_t brr   = 0;
    uint32_t presc = 0;
    bool     brr_ok;

    if (desc->is_lpuart)
    {
        brr_ok = UART_BM_CalcBrrLpuart(cfg->kernel_clock_hz, cfg->baudrate, &brr, &presc);
    }
    else
    {
        brr_ok = UART_BM_CalcBrrUsart(cfg->kernel_clock_hz, cfg->baudrate,
                                      cfg->oversampling_8, &brr, &presc);
    }

    if (!brr_ok)
    {
        return false;
    }

    /* 1.6 Additional CR1 flags */
    if (!desc->is_lpuart && cfg->oversampling_8)
    {
        cr1 |= USART_CR1_OVER8;
    }

    if (cfg->fifo_enable)
    {
        cr1 |= USART_CR1_FIFOEN;
    }

    cr1 |= (USART_CR1_TE | USART_CR1_RE);

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
        UART_BM_EnableBusClock(desc->bus, desc->enr_mask);
    }

    /* 2.3 Program peripheral registers */
    USART_TypeDef *u = cfg->instance;

    u->CR1 &= ~USART_CR1_UE;
    u->PRESC = presc & 0xFUL;
    u->BRR   = brr;
    u->CR2   = cr2;
    u->CR3   = cr3;
    u->CR1   = cr1;

    /* 2.4 Enable USART (UE) */
    u->CR1 |= USART_CR1_UE;

    /* 2.5 Wait for TEACK and REACK flags */
    uint32_t spin = UART_BM_MAX_SPIN_ACK;
    while (((u->ISR & (USART_ISR_TEACK | USART_ISR_REACK)) != (USART_ISR_TEACK | USART_ISR_REACK)) &&
           (spin > 0U))
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
    h->data_bits    = cfg->data_bits;
    h->error_flags  = 0;

    return true;
}

bool UART_BM_Deinit(UartBm_Handle_t *h)
{
    if ((h == NULL) || (h->instance == NULL))
    {
        return false;
    }

    USART_TypeDef *u = h->instance;
    u->CR1 &= ~USART_CR1_UE;
    u->CR1 = 0;
    u->CR2 = 0;
    u->CR3 = 0;

    h->instance     = NULL;
    h->error_flags  = 0;
    h->fifo_enabled = false;

    return true;
}

bool UART_BM_WriteByte(UartBm_Handle_t *h, uint8_t b)
{
    if ((h == NULL) || (h->instance == NULL))
    {
        return false;
    }

    if ((h->instance->ISR & USART_ISR_TXE_TXFNF) == 0U)
    {
        return false;
    }

    h->instance->TDR = b;
    return true;
}

size_t UART_BM_Write(UartBm_Handle_t *h, const uint8_t *d, size_t n, uint32_t spin_limit)
{
    if ((h == NULL) || (h->instance == NULL) || (d == NULL) || (n == 0U))
    {
        return 0;
    }

    USART_TypeDef *u = h->instance;

    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = spin_limit;
        while (((u->ISR & USART_ISR_TXE_TXFNF) == 0U) && (spin > 0U))
        {
            spin--;
        }

        if (spin == 0U)
        {
            return i;
        }

        u->TDR = d[i];
    }

    return n;
}

bool UART_BM_ReadByte(UartBm_Handle_t *h, uint8_t *b)
{
    if ((h == NULL) || (h->instance == NULL) || (b == NULL))
    {
        return false;
    }

    USART_TypeDef *u   = h->instance;
    uint32_t       isr = u->ISR;
    uint32_t       err = 0;

    if (isr & USART_ISR_ORE)
    {
        err |= UART_BM_ERR_ORE;
        u->ICR = USART_ICR_ORECF;
    }
    if (isr & USART_ISR_FE)
    {
        err |= UART_BM_ERR_FE;
        u->ICR = USART_ICR_FECF;
    }
    if (isr & USART_ISR_NE)
    {
        err |= UART_BM_ERR_NE;
        u->ICR = USART_ICR_NECF;
    }
    if (isr & USART_ISR_PE)
    {
        err |= UART_BM_ERR_PE;
        u->ICR = USART_ICR_PECF;
    }

    h->error_flags |= err;

    if ((u->ISR & USART_ISR_RXNE_RXFNE) == 0U)
    {
        return false;
    }

    uint32_t rdr = u->RDR;
    if (h->data_bits == UART_BM_DATA_BITS_7)
    {
        *b = (uint8_t)(rdr & 0x7FU);
    }
    else
    {
        *b = (uint8_t)(rdr & 0xFFU);
    }

    return true;
}

bool UART_BM_Flush(UartBm_Handle_t *h, uint32_t spin_limit)
{
    if ((h == NULL) || (h->instance == NULL))
    {
        return false;
    }

    USART_TypeDef *u    = h->instance;
    uint32_t       spin = spin_limit;

    while (((u->ISR & USART_ISR_TC) == 0U) && (spin > 0U))
    {
        spin--;
    }

    return (spin > 0U);
}

uint32_t UART_BM_TakeErrors(UartBm_Handle_t *h)
{
    if ((h == NULL) || (h->instance == NULL))
    {
        return 0;
    }

    USART_TypeDef *u   = h->instance;
    uint32_t       isr = u->ISR;
    uint32_t       err = 0;

    if (isr & USART_ISR_ORE)
    {
        err |= UART_BM_ERR_ORE;
        u->ICR = USART_ICR_ORECF;
    }
    if (isr & USART_ISR_FE)
    {
        err |= UART_BM_ERR_FE;
        u->ICR = USART_ICR_FECF;
    }
    if (isr & USART_ISR_NE)
    {
        err |= UART_BM_ERR_NE;
        u->ICR = USART_ICR_NECF;
    }
    if (isr & USART_ISR_PE)
    {
        err |= UART_BM_ERR_PE;
        u->ICR = USART_ICR_PECF;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t flags = h->error_flags | err;
    h->error_flags = 0;
    __set_PRIMASK(primask);

    return flags;
}
