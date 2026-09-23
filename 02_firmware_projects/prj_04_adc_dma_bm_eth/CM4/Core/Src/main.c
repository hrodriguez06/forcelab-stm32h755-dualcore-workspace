/**
  ******************************************************************************
  * @file    main.c
  * @brief   CPU2 (Cortex-M4) entry point - bare metal, no RTOS.
  *
  *          Boot contract with CPU1, from
  *          00_docs_stm32h7/docs/00_core_and_system/dual_core_boot_ipc.md:
  *
  *            1. Both cores leave reset together (BCM7 and BCM4 both set).
  *            2. CPU2 does NOT touch RCC, the PLLs or the power supply. CPU1
  *               owns the clock tree. CPU2 running before that is configured
  *               would execute at the wrong frequency and, worse, would race
  *               CPU1's writes to RCC.
  *            3. So CPU2 immediately enters STOP on the D2 domain and waits
  *               for the HSEM 0 notification. Entering STOP is what drops
  *               D2CKRDY, which is how CPU1 detects that CPU2 has parked.
  *            4. CPU1 configures everything, then takes and releases HSEM 0.
  *               That wakes CPU2, which from then on only initialises its own
  *               peripherals.
  *
  *          Deviating from this is the usual cause of a dual-core board that
  *          works "sometimes": the failure depends on which core wins a race
  *          during the first microseconds.
  ******************************************************************************
  */

#include "main.h"
#include "shared_memory.h"
#include "adc_acquisition.h"
#if defined(UART_DRIVER_SELFTEST) && UART_DRIVER_SELFTEST
#include "uart_bm.h"
#include "uart_ll.h"
#endif

#define HEARTBEAT_DELAY_LOOPS   2000000UL

/* Liveness markers in CPU2's OWN SRAM2, deliberately not in the shared block.
   If g_shared.m4_heartbeat never moves, these say why: cpu2_woke tells you
   whether the HSEM handshake completed at all, and cpu2_loops whether the
   super-loop is running. A debugger can read them without halting anything -
   nothing else on the board writes SRAM2. */
volatile uint32_t cpu2_woke  = 0UL;
volatile uint32_t cpu2_loops = 0UL;

static void CPU2_WaitForCPU1(void);
#if defined(UART_DRIVER_SELFTEST) && UART_DRIVER_SELFTEST
static void CM4_Test_Uart_Drivers(void);
#endif

int main(void)
{
    uint32_t last_heartbeat_tick = 0U;

    CPU2_WaitForCPU1();

    /* Silicon errata ES0445 2.2.2: Disable FPU lazy stacking to avoid
       potential register corruption during interrupt entry. */
    FPU->FPCCR &= ~FPU_FPCCR_LSPEN_Msk;

    cpu2_woke = 0xC0FFEEUL;

    /* The clock tree is up and owned by CPU1. From here CPU2 configures only
       what belongs to it. HAL_Init() sets up the SysTick of THIS core and the
       NVIC priority grouping; it does not touch the clock tree. */
    HAL_Init();
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    /* Deliberately not calling Shared_Init(): the block belongs to CPU1 and is
       already initialised by the time this core is released. Zeroing it here
       would wipe whatever CPU1 has published. */

#if defined(UART_DRIVER_SELFTEST) && UART_DRIVER_SELFTEST
    /* Run shared UART driver verification on USART2 (PD5/PD6) */
    CM4_Test_Uart_Drivers();
#endif

    /* Initialize ADC1 + DMA1 with TIM3 hardware trigger */
    if (!ADC_Acquisition_Init())
    {
        Error_Handler();
    }

    for (;;)
    {
        uint32_t now;

        cpu2_loops++;

        /* Update CM4 heartbeat every 100 ms */
        now = HAL_GetTick();
        if ((now - last_heartbeat_tick) >= 100U)
        {
            last_heartbeat_tick = now;
            g_shared.m4_heartbeat++;
        }

        /* Process completed ADC DMA half-buffer if ready */
        ADC_Acquisition_Process();
    }
}

/**
  * @brief  Parks CPU2 in STOP until CPU1 releases HSEM 0.
  *
  * @note   HSEM 0 is the channel ST reserves for this handshake, and the
  *         notification is what wakes the core out of STOP - not a poll loop.
  *         The order matters: activate the notification BEFORE entering STOP,
  *         or CPU1 can release the semaphore in the window between the two and
  *         CPU2 sleeps for ever.
  */
static void CPU2_WaitForCPU1(void)
{
    __HAL_RCC_HSEM_CLK_ENABLE();

    /* Subscribe this core to HSEM 0 before sleeping. */
    HAL_HSEM_ActivateNotification(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_BOOT_SYNC));

    /* SEVONPEND: wake on an interrupt that is merely PENDING, without enabling
       it in the NVIC and without vectoring to a handler.
     *
     * This line is why CPU2 wakes at all, and leaving it out fails silently.
     * HAL_PWREx_EnterSTOPMode() issues a single __WFE(), and WFE resumes only
     * on an *event*. Releasing HSEM 0 sets HSEM_C2ISR and HSEM_C2MISR, but a
     * pending interrupt is not an event unless it is NVIC-enabled or
     * SEVONPEND is set - and enabling HSEM2_IRQn instead would vector into the
     * weak Default_Handler, which is an infinite loop.
     *
     * ST's reference CM4 code (Projects/NUCLEO-H745ZI-Q/Applications/FreeRTOS/
     * FreeRTOS_AMP_Dual_RTOS/CM4/Src/main.c) omits it, because there CPU1
     * leaves the D2 domain idle and the domain exits DSTOP through the
     * peripheral wake-up path instead. Here CPU1 holds D2 awake - the console
     * USART3 sits on APB1, in D2 - so CPU2 is only in CSleep and the core's
     * own event logic is the only way back.
     *
     * Symptom when it is missing: HSEM_C2IER = 1 and HSEM_C2MISR = 1 (the
     * handshake worked), CPU1 runs normally, and CPU2 never leaves this
     * function. Nothing faults. */
    SET_BIT(SCB->SCR, SCB_SCR_SEVONPEND_Msk);

    /* Enter STOP on D2 until CPU1 finishes initialization and publishes SHARED_MAGIC */
    while (g_shared.magic != SHARED_MAGIC)
    {
        HAL_PWREx_ClearPendingEvent();
        HAL_PWREx_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFE, PWR_D2_DOMAIN);
    }

    /* Woken by the HSEM notification: unsubscribe, clear it, and put the core
       back to its normal wake-up behaviour. */
    __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_BOOT_SYNC));
    HAL_HSEM_DeactivateNotification(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_BOOT_SYNC));
    CLEAR_BIT(SCB->SCR, SCB_SCR_SEVONPEND_Msk);
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;)
    {
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    Error_Handler();
}
#endif

#if defined(UART_DRIVER_SELFTEST) && UART_DRIVER_SELFTEST

static uint8_t s_ll_ring_buf[64];
static UartLL_RingBuffer_t s_ll_ring = {
    .data = s_ll_ring_buf,
    .size = sizeof(s_ll_ring_buf),
    .head = 0,
    .tail = 0
};
static UartLL_Handle_t s_ll_handle;

void USART2_IRQHandler(void)
{
    UART_LL_IrqService(&s_ll_handle, &s_ll_ring);
}

static void CM4_Test_Uart_Drivers(void)
{
    /* Enable clocks in CM4 domain for GPIOD and USART2 */
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();

    /* Test USART2 on PD5 (TX) / PD6 (RX) with APB1 clock = 100 MHz (pclk1) */
    UartBm_Pin_t tx_bm = { .port = GPIOD, .pin_idx = 5, .af = 7 };
    UartBm_Pin_t rx_bm = { .port = GPIOD, .pin_idx = 6, .af = 7 };
    UartLL_Pin_t tx_ll = { .port = GPIOD, .pin_idx = 5, .af = 7 };
    UartLL_Pin_t rx_ll = { .port = GPIOD, .pin_idx = 6, .af = 7 };

    /* 1. Probing for physical PD5->PD6 jumper wire */
    /* Configure PD5 as Output PP Low, PD6 as Input Pull-Up */
    GPIOD->MODER = (GPIOD->MODER & ~(3UL << (5 * 2))) | (1UL << (5 * 2));
    GPIOD->ODR &= ~(1UL << 5);
    GPIOD->MODER &= ~(3UL << (6 * 2));
    GPIOD->PUPDR = (GPIOD->PUPDR & ~(3UL << (6 * 2))) | (1UL << (6 * 2)); /* Pull-up */
    for (volatile int d = 0; d < 2000; d++) {}
    bool low_ok = ((GPIOD->IDR & (1UL << 6)) == 0);

    GPIOD->ODR |= (1UL << 5);
    GPIOD->PUPDR = (GPIOD->PUPDR & ~(3UL << (6 * 2))) | (2UL << (6 * 2)); /* Pull-down */
    for (volatile int d = 0; d < 2000; d++) {}
    bool high_ok = ((GPIOD->IDR & (1UL << 6)) != 0);

    bool jumper_present = (low_ok && high_ok);

    /* 2. Run BM test vectors at 9600, 115200, 921600 */
    uint32_t bauds[] = { 9600UL, 115200UL, 921600UL };
    bool bm_init_ok[3] = { false, false, false };
    bool bm_loop_ok[3] = { false, false, false };

    for (int i = 0; i < 3; i++)
    {
        UartBm_Config_t cfg = {
            .instance        = USART2,
            .kernel_clock_hz = 100000000UL,
            .baudrate        = bauds[i],
            .data_bits       = UART_BM_DATA_BITS_8,
            .stop_bits       = UART_BM_STOP_BITS_1,
            .parity          = UART_BM_PARITY_NONE,
            .oversampling_8  = (bauds[i] > 115200UL),
            .fifo_enable     = false,
            .hw_flow         = UART_BM_HW_FLOW_NONE,
            .enable_clock    = true,
            .tx_pin          = &tx_bm,
            .rx_pin          = &rx_bm
        };
        UartBm_Handle_t h_bm;
        if (UART_BM_Init(&h_bm, &cfg))
        {
            bm_init_ok[i] = true;
            if (jumper_present)
            {
                /* Calculate generous spin timeout based on baudrate:
                   At 200 MHz core, 1 iteration ~ 4 cycles.
                   At least 20 character times or 2 ms minimum. */
                uint32_t spin_char = (200000000UL * 20UL) / (4UL * bauds[i]);
                if (spin_char < 100000UL)
                {
                    spin_char = 100000UL;
                }

                /* Loopback test: send test pattern and verify */
                bool byte_match = true;
                for (uint8_t b = 0x30; b <= 0x38; b++)
                {
                    UART_BM_WriteByte(&h_bm, b);
                    if (!UART_BM_Flush(&h_bm, spin_char))
                    {
                        byte_match = false;
                        break;
                    }
                    uint8_t rx_val = 0;
                    uint32_t rx_spin = spin_char;
                    while (!UART_BM_ReadByte(&h_bm, &rx_val) && (rx_spin > 0U))
                    {
                        rx_spin--;
                    }
                    if ((rx_spin == 0U) || (rx_val != b))
                    {
                        byte_match = false;
                        break;
                    }
                }
                bm_loop_ok[i] = byte_match;
            }
            UART_BM_Deinit(&h_bm);
        }
    }

    /* 3. Run LL test at 921600 */
    UartLL_Config_t ll_cfg = {
        .instance        = USART2,
        .kernel_clock_hz = 100000000UL,
        .baudrate        = 921600UL,
        .data_bits       = LL_USART_DATAWIDTH_8B,
        .stop_bits       = LL_USART_STOPBITS_1,
        .parity          = LL_USART_PARITY_NONE,
        .oversampling    = LL_USART_OVERSAMPLING_8,
        .fifo_enable     = false,
        .hw_flow         = LL_USART_HWCONTROL_NONE,
        .enable_clock    = true,
        .tx_pin          = &tx_ll,
        .rx_pin          = &rx_ll
    };
    bool ll_init_ok = UART_LL_Init(&s_ll_handle, &ll_cfg);
    bool ll_loop_ok = false;
    bool ll_ore_ok  = false;

    if (ll_init_ok)
    {
        if (jumper_present)
        {
            UART_LL_EnableRxIrq(&s_ll_handle, USART2_IRQn, 5);

            /* Test IRQ loopback */
            UART_LL_WriteByte(&s_ll_handle, 0x42);
            (void)UART_LL_Flush(&s_ll_handle, 100000U);
            for (volatile int wait = 0; wait < 20000; wait++) {}

            uint8_t rx_ll_val = 0;
            if (UartLL_RingBuffer_Get(&s_ll_ring, &rx_ll_val) && (rx_ll_val == 0x42))
            {
                ll_loop_ok = true;
            }

            NVIC_DisableIRQ(USART2_IRQn);

            /* Test REAL hardware Overrun (ORE):
               Disable RX IRQ so ISR does not read incoming data.
               Send 2 bytes back-to-back:
               Byte 1 fills RDR and sets RXNE.
               Byte 2 cannot be moved from RSR to RDR while RXNE=1, triggering hardware ORE in ISR! */
            UART_LL_WriteByte(&s_ll_handle, 0xAA);
            (void)UART_LL_Flush(&s_ll_handle, 100000U);
            UART_LL_WriteByte(&s_ll_handle, 0x55);
            (void)UART_LL_Flush(&s_ll_handle, 100000U);
            for (volatile int wait = 0; wait < 10000; wait++) {}

            /* Verify hardware raised ORE in USART2->ISR */
            if ((USART2->ISR & USART_ISR_ORE) != 0U)
            {
                /* Calling ReadByte clears hardware ORE flag and latches UART_LL_ERR_ORE into handle */
                uint8_t dummy = 0;
                (void)UART_LL_ReadByte(&s_ll_handle, &dummy);

                /* UART_LL_TakeErrors() atomically retrieves error_flags and clears them */
                uint32_t errs = UART_LL_TakeErrors(&s_ll_handle);
                if (((errs & UART_LL_ERR_ORE) != 0U) &&
                    (s_ll_handle.error_flags == 0U) &&
                    ((USART2->ISR & USART_ISR_ORE) == 0U))
                {
                    ll_ore_ok = true;
                }
            }
        }
        UART_LL_Deinit(&s_ll_handle);
    }

    /* Format report into g_shared.m4_to_m7 without dynamic memory */
    SHARED_LOCK(HSEM_ID_M4_TO_M7_MBOX);
    char *p = (char *)g_shared.m4_to_m7;
    int pos = 0;

    /* jumper=0 or 1 */
    p[pos++] = 'j'; p[pos++] = 'u'; p[pos++] = 'm'; p[pos++] = 'p'; p[pos++] = 'e'; p[pos++] = 'r'; p[pos++] = '=';
    p[pos++] = jumper_present ? '1' : '0';

    /* bm_init=1,1,1 */
    p[pos++] = ' '; p[pos++] = 'b'; p[pos++] = 'm'; p[pos++] = '_'; p[pos++] = 'i'; p[pos++] = 'n'; p[pos++] = 'i'; p[pos++] = 't'; p[pos++] = '=';
    p[pos++] = bm_init_ok[0] ? '1' : '0'; p[pos++] = ',';
    p[pos++] = bm_init_ok[1] ? '1' : '0'; p[pos++] = ',';
    p[pos++] = bm_init_ok[2] ? '1' : '0';

    /* bm_loop=1,1,1 or skip */
    p[pos++] = ' '; p[pos++] = 'b'; p[pos++] = 'm'; p[pos++] = '_'; p[pos++] = 'l'; p[pos++] = 'o'; p[pos++] = 'o'; p[pos++] = 'p'; p[pos++] = '=';
    if (jumper_present)
    {
        p[pos++] = bm_loop_ok[0] ? '1' : '0'; p[pos++] = ',';
        p[pos++] = bm_loop_ok[1] ? '1' : '0'; p[pos++] = ',';
        p[pos++] = bm_loop_ok[2] ? '1' : '0';
    }
    else
    {
        p[pos++] = 's'; p[pos++] = 'k'; p[pos++] = 'i'; p[pos++] = 'p';
    }

    /* ll=1,1 or 1,skip or 0,0 */
    p[pos++] = ' '; p[pos++] = 'l'; p[pos++] = 'l'; p[pos++] = '=';
    p[pos++] = ll_init_ok ? '1' : '0'; p[pos++] = ',';
    if (jumper_present)
    {
        p[pos++] = ll_loop_ok ? '1' : '0';
    }
    else
    {
        p[pos++] = 's'; p[pos++] = 'k'; p[pos++] = 'i'; p[pos++] = 'p';
    }

    /* ore=1 or skip */
    p[pos++] = ' '; p[pos++] = 'o'; p[pos++] = 'r'; p[pos++] = 'e'; p[pos++] = '=';
    if (jumper_present)
    {
        p[pos++] = ll_ore_ok ? '1' : '0';
    }
    else
    {
        p[pos++] = 's'; p[pos++] = 'k'; p[pos++] = 'i'; p[pos++] = 'p';
    }

    p[pos] = '\0';
    __DMB();
    g_shared.m4_to_m7_len = (uint32_t)pos;
    SHARED_UNLOCK(HSEM_ID_M4_TO_M7_MBOX);
}

#endif /* UART_DRIVER_SELFTEST */

