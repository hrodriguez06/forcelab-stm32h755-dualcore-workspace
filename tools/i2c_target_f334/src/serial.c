/**
  ******************************************************************************
  * @file    serial.c
  * @brief   USART2 console of the F334 bench target (PA2 TX, PA3 RX, AF7: the
  *          ST-LINK/V2-1 virtual COM port), 115200 8N1. Reception in the
  *          interrupt into a ring; transmission blocking, from the main loop
  *          only, with every wait bounded (TIM2, stall exit).
  ******************************************************************************
  */

#include "bench.h"

#define BAUD          115200U
#define RX_RING       128U
#define LINE_MAX      64U
#define TX_WAIT_US    2000U     /* one character takes 87 us */

static volatile uint8_t  s_rx[RX_RING];
static volatile uint32_t s_rx_head;
static volatile uint32_t s_rx_tail;
static volatile uint32_t s_rx_overruns;

static char     s_line[LINE_MAX];
static uint32_t s_line_n;
static bool     s_line_long;

void serial_init(uint32_t pclk1_hz)
{
    RCC->AHBENR  |= RCC_AHBENR_GPIOAEN;
    (void)RCC->AHBENR;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;

    /* PA2, PA3: AF7 (DS9994 table 14); RX with pull-up so a floating line
       reads idle. Not bus pins. */
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~((0xFUL << 8) | (0xFUL << 12))) | (7UL << 8) | (7UL << 12);
    GPIOA->PUPDR  = (GPIOA->PUPDR & ~((3UL << 4) | (3UL << 6))) | (1UL << 6);
    GPIOA->MODER  = (GPIOA->MODER & ~((3UL << 4) | (3UL << 6))) | (2UL << 4) | (2UL << 6);

    USART2->CR1 = 0U;
    USART2->BRR = (pclk1_hz + (BAUD / 2U)) / BAUD;    /* OVER8 = 0 */
    USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;
    NVIC_SetPriority(USART2_IRQn, 3U);
    NVIC_EnableIRQ(USART2_IRQn);
}

void USART2_IRQHandler(void)
{
    const uint32_t isr = USART2->ISR;
    if ((isr & USART_ISR_RXNE) != 0U)
    {
        const uint8_t c = (uint8_t)USART2->RDR;
        const uint32_t next = (s_rx_head + 1U) % RX_RING;
        if (next != s_rx_tail)
        {
            s_rx[s_rx_head] = c;
            s_rx_head = next;
        }
        else
        {
            s_rx_overruns++;
        }
    }
    if ((isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) != 0U)
    {
        if ((isr & USART_ISR_ORE) != 0U)
        {
            s_rx_overruns++;
        }
        USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF | USART_ICR_PECF;
    }
}

uint32_t serial_overruns(void)
{
    return s_rx_overruns;
}

bool serial_getline(char *buf, uint32_t size)
{
    while (s_rx_tail != s_rx_head)
    {
        const char c = (char)s_rx[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1U) % RX_RING;
        if ((c == '\r') || (c == '\n'))
        {
            const bool had = (s_line_n > 0U) && !s_line_long;
            if (had)
            {
                uint32_t i = 0U;
                for (; (i < s_line_n) && (i + 1U < size); i++)
                {
                    buf[i] = s_line[i];
                }
                buf[i] = '\0';
            }
            else if (s_line_long)
            {
                serial_puts("ERR line too long");
            }
            s_line_n = 0U;
            s_line_long = false;
            if (had)
            {
                return true;
            }
        }
        else if (s_line_n < (LINE_MAX - 1U))
        {
            s_line[s_line_n++] = c;
        }
        else
        {
            s_line_long = true;
        }
    }
    return false;
}

/* Blocking, bounded: a character that cannot go in TX_WAIT_US is dropped. */
static void put_char(char c)
{
    const uint32_t t0 = us_now();
    uint32_t last = t0;
    uint32_t same = 0U;
    while ((USART2->ISR & USART_ISR_TXE) == 0U)
    {
        const uint32_t now = us_now();
        if (now == last)
        {
            if (++same > WAIT_STALL_READS)
            {
                return;
            }
        }
        else
        {
            same = 0U;
            last = now;
        }
        if ((now - t0) > TX_WAIT_US)
        {
            return;
        }
    }
    USART2->TDR = (uint8_t)c;
}

void serial_puts(const char *s)
{
    while (*s != '\0')
    {
        put_char(*s++);
    }
    put_char('\r');
    put_char('\n');
}

/* ---- Line builder: no printf, so no heap and no newlib stdio ------------------ */

void ln_start(Line_t *l, const char *s)
{
    l->n = 0U;
    l->text[0] = '\0';
    ln_str(l, s);
}

void ln_str(Line_t *l, const char *s)
{
    while ((*s != '\0') && (l->n + 1U < sizeof(l->text)))
    {
        l->text[l->n++] = *s++;
    }
    l->text[l->n] = '\0';
}

void ln_u32(Line_t *l, uint32_t v)
{
    char tmp[11];
    uint32_t i = 0U;
    do
    {
        tmp[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while ((v != 0U) && (i < sizeof(tmp)));
    while (i > 0U)
    {
        const char d[2] = { tmp[--i], '\0' };
        ln_str(l, d);
    }
}

void ln_i32(Line_t *l, int32_t v)
{
    if (v < 0)
    {
        ln_str(l, "-");
        ln_u32(l, (uint32_t)(-(v + 1)) + 1U);
    }
    else
    {
        ln_u32(l, (uint32_t)v);
    }
}

void ln_hex(Line_t *l, uint32_t v, uint32_t digits)
{
    static const char k_hex[] = "0123456789ABCDEF";
    char tmp[9];
    if (digits > 8U)
    {
        digits = 8U;
    }
    for (uint32_t i = 0U; i < digits; i++)
    {
        tmp[i] = k_hex[(v >> (4U * (digits - 1U - i))) & 0xFU];
    }
    tmp[digits] = '\0';
    ln_str(l, tmp);
}

void ln_send(Line_t *l)
{
    serial_puts(l->text);
}
