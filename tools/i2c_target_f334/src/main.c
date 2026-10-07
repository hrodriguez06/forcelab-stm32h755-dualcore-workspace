/**
  ******************************************************************************
  * @file    main.c
  * @brief   NUCLEO-F334R8 bench target of 05_i2c: clocks, the 1 MHz time base,
  *          the independent watchdog, the fault handlers and the main loop
  *          (console commands, target_poll()).
  *
  *          Clock: HSI 8 MHz / 2 x 16 = 64 MHz on the PLL (RM0364 9.2.3), flash
  *          with 2 wait states (48 < HCLK <= 72 MHz), APB1 /2 = 32 MHz (its
  *          maximum is 36 MHz), APB2 /1; I2C1 kernel = SYSCLK. If the PLL does
  *          not lock, everything stays on the HSI (8 MHz) and the banner says
  *          so (sysclk=8000000). The HSI is +-1 % (DS9994), enough for the
  *          console; nothing on the bench times itself against this clock.
  *
  *          Watchdog: the IWDG (LSI 30..50 kHz, DS9994; /32, reload 449: 450
  *          counts, 288..480 ms, 360 ms at 40 kHz) is refreshed by the main loop
  *          only. If the firmware stops, the reset puts PB8/PB9 back in input: a
  *          hold can never outlive the firmware by more than 480 ms, inside the
  *          500 ms of S21 even when TIM2 cannot run (audit F0, m1). The holds
  *          themselves end in the TIM2 interrupt, not here.
  *
  *          Interrupt priorities (audit F0, m3): TIM2 0 (the end of a hold),
  *          I2C1 1, EXTI9_5 2 (the SCL edges of a hold), USART2 3. A fast,
  *          endless SCL during an SDA hold cannot delay the release by time.
  *          Every vector this firmware does not use ends in bench_fault_release()
  *          (src/vectors.c, audit F0, m2).
  ******************************************************************************
  */

#include "bench.h"

uint32_t g_sysclk_hz = 8000000UL;

/* ---- Time base: TIM2, 32 bits, 1 MHz ------------------------------------------------- */

uint32_t us_now(void)
{
    return TIM2->CNT;
}

bool wait_us(uint32_t us)
{
    const uint32_t t0 = us_now();
    uint32_t last = t0;
    uint32_t same = 0U;
    for (;;)
    {
        const uint32_t now = us_now();
        if ((now - t0) >= us)
        {
            return true;
        }
        if (now == last)
        {
            if (++same > WAIT_STALL_READS)
            {
                return false;          /* the counter stopped: never wait for ever */
            }
        }
        else
        {
            same = 0U;
            last = now;
        }
    }
}

static void timebase_init(uint32_t tim_clk_hz)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    (void)RCC->APB1ENR;
    TIM2->CR1  = 0U;
    TIM2->PSC  = (tim_clk_hz / 1000000UL) - 1U;
    TIM2->ARR  = 0xFFFFFFFFUL;
    TIM2->CCMR1 = 0U;                  /* CC1: output compare, frozen: only its flag is used */
    TIM2->DIER = 0U;
    TIM2->EGR  = TIM_EGR_UG;           /* load PSC */
    TIM2->SR   = 0U;
    TIM2->CR1  = TIM_CR1_CEN;
    NVIC_SetPriority(TIM2_IRQn, 0U);       /* above everything: the end of a hold (m3) */
    NVIC_EnableIRQ(TIM2_IRQn);
    NVIC_SetPriority(EXTI9_5_IRQn, 2U);
    NVIC_EnableIRQ(EXTI9_5_IRQn);
}

/* ---- Clocks --------------------------------------------------------------------------- */

#define CLOCK_SPIN_MAX   200000UL     /* bounded waits before any timer runs */

/* APB1 is HCLK / 2 on every path, so PCLK1 = SYSCLK / 2 and the TIM2 clock
   (x2 when the APB1 prescaler is not 1, RM0364 9.2) = SYSCLK. */
static uint32_t clock_init(void)
{
    RCC->CFGR = RCC_CFGR_PLLSRC_HSI_DIV2 | RCC_CFGR_PLLMUL16 | RCC_CFGR_PPRE1_DIV2;   /* SW = HSI */
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_1;
    if ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_1)
    {
        return 8000000UL;
    }
    RCC->CR  |= RCC_CR_PLLON;
    uint32_t n = 0U;
    while (((RCC->CR & RCC_CR_PLLRDY) == 0U) && (n < CLOCK_SPIN_MAX))
    {
        n++;
    }
    if ((RCC->CR & RCC_CR_PLLRDY) == 0U)
    {
        return 8000000UL;              /* HSI, APB1 = 4 MHz */
    }
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    n = 0U;
    while (((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) && (n < CLOCK_SPIN_MAX))
    {
        n++;
    }
    return ((RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_PLL) ? 64000000UL : 8000000UL;
}

/* ---- Watchdog -------------------------------------------------------------------------- */

static void iwdg_init(void)
{
    IWDG->KR  = 0xCCCCU;               /* start (and the LSI with it) */
    IWDG->KR  = 0x5555U;               /* unlock PR and RLR */
    IWDG->PR  = 3U;                    /* /32 */
    IWDG->RLR = 449U;                  /* 450 counts: 288..480 ms over 30..50 kHz (m1) */
    for (uint32_t n = 0U; (IWDG->SR != 0U) && (n < CLOCK_SPIN_MAX); n++)
    {
    }
    IWDG->KR  = 0xAAAAU;
}

/* ---- Faults: the bus pins are released before anything else --------------------------- */

void bench_fault_release(void)
{
    __disable_irq();
    target_pins_safe();
    for (;;)
    {
        /* the IWDG resets the chip */
    }
}

void NMI_Handler(void)        { bench_fault_release(); }
void HardFault_Handler(void)  { bench_fault_release(); }
void MemManage_Handler(void)  { bench_fault_release(); }
void BusFault_Handler(void)   { bench_fault_release(); }
void UsageFault_Handler(void) { bench_fault_release(); }

/* ---- Main ------------------------------------------------------------------------------- */

int main(void)
{
    const uint32_t reset_flags = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;

    target_pins_safe();                /* first: PB8/PB9 input, open-drain type, no pull */
    g_sysclk_hz = clock_init();
    SystemCoreClock = g_sysclk_hz;
    timebase_init(g_sysclk_hz);        /* TIM2 clock = SYSCLK (clock_init) */
    iwdg_init();
    serial_init(g_sysclk_hz / 2U);
    target_init();
    target_banner(reset_flags);

    char line[64];
    for (;;)
    {
        IWDG->KR = 0xAAAAU;
        if (serial_getline(line, sizeof(line)))
        {
            target_command(line);
        }
        target_poll();
    }
}
