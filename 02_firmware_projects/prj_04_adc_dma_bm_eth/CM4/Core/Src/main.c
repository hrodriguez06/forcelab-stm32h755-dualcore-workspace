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
#include "hsem_bm.h"
#include "adc_acquisition.h"
#if defined(UART_DRIVER_SELFTEST) && UART_DRIVER_SELFTEST
#include "uart_bm.h"
#include "uart_ll.h"
#endif
#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST
#include "adc_bm.h"
#include "adc_ll.h"
#include "adc_selftest_hw.h"
#include <stdio.h>
#include <string.h>
#endif
#if defined(DMA_DRIVER_SELFTEST) && DMA_DRIVER_SELFTEST
#include "dma_selftest.h"
#endif
#if defined(HSEM_DRIVER_SELFTEST) && HSEM_DRIVER_SELFTEST
#include "hsem_selftest.h"
#endif
#if defined(GPIO_DRIVER_SELFTEST) && GPIO_DRIVER_SELFTEST
#include "gpio_selftest.h"
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
#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST
static void CM4_Test_Adc_Drivers(void);
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

    HSEM_BM_Init();
    HSEM_BM_TimebaseStart();

    /* Deliberately not calling Shared_Init(): the block belongs to CPU1 and is
       already initialised by the time this core is released. Zeroing it here
       would wipe whatever CPU1 has published. */

#if defined(UART_DRIVER_SELFTEST) && UART_DRIVER_SELFTEST
    /* Run shared UART driver verification on USART2 (PD5/PD6) */
    CM4_Test_Uart_Drivers();
#endif

#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST
    /* Run shared ADC driver verification (BM & LL register match, internal channels, oversampling, AWD) */
    CM4_Test_Adc_Drivers();

    /* Hardware tests T2, T3, T4, T6 (adc_selftest_hw.h). Must run before
       ADC_Acquisition_Init(): they borrow ADC1/2/3, DMA1_Stream0/1 and TIM3. */
    ADC_SelfTestHw_Run();
#endif

#if defined(DMA_DRIVER_SELFTEST) && DMA_DRIVER_SELFTEST
    /* Run shared DMA driver verification (T1-T7) on CM4 */
    DMA_SelfTest_CM4_Run();
#endif

#if defined(HSEM_DRIVER_SELFTEST) && HSEM_DRIVER_SELFTEST
    /* Run shared HSEM driver selftest on CM4 */
    HSEM_SelfTest_CM4_Run();
#endif

#if defined(GPIO_DRIVER_SELFTEST) && GPIO_DRIVER_SELFTEST
    /* Run shared GPIO driver selftest on CM4 */
    GPIO_SelfTest_CM4_Run();
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

#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST
        /* One result line per mailbox slot CPU1 has freed. */
        ADC_SelfTestHw_Poll();
#endif

#if defined(DMA_DRIVER_SELFTEST) && DMA_DRIVER_SELFTEST
        /* One result line per mailbox slot CPU1 has freed. */
        DMA_SelfTest_CM4_Poll();
#endif

#if defined(HSEM_DRIVER_SELFTEST) && HSEM_DRIVER_SELFTEST
        /* One result line per mailbox slot CPU1 has freed. */
        HSEM_SelfTest_CM4_Poll();
#endif

#if defined(GPIO_DRIVER_SELFTEST) && GPIO_DRIVER_SELFTEST
        /* One result line per mailbox slot CPU1 has freed. */
        GPIO_SelfTest_CM4_Poll();
#endif
    }
}

static bool Shared_MagicReady(void *ctx)
{
    (void)ctx;
    return (g_shared.magic == SHARED_MAGIC);
}

/**
  * @brief  Parks CPU2 in STOP until CPU1 releases HSEM 0 and shared memory is ready.
  */
static void CPU2_WaitForCPU1(void)
{
    const HsemBm_BootWait_t cfg = {
        .sem_id     = HSEM_ID_BOOT_SYNC,
        .ready      = Shared_MagicReady,
        .ctx        = NULL,
        .wake_count = &g_shared.cpu2_wake_count
    };
    (void)HSEM_BM_BootWaitForCpu1(&cfg);
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

#if defined(ADC_DRIVER_SELFTEST) && ADC_DRIVER_SELFTEST

static void CM4_Test_Adc_Drivers(void)
{
    /* Enable ADC3 peripheral clock in AHB4 */
    RCC->AHB4ENR |= RCC_AHB4ENR_ADC3EN;
    volatile uint32_t dummy = RCC->AHB4ENR;
    (void)dummy;

    bool uncal_check_ok = false;
    bool cal_injected_ok = false;
    bool bm_init_ok = false;
    bool ll_init_ok = false;
    bool reg_compare_ok = true;
    bool bm_read_vref_ok = false;
    bool bm_read_temp_ok = false;
    bool ll_read_vref_ok = false;
    bool ll_read_temp_ok = false;
    bool res_sweep_ok = true;
    bool ovs_trunc_rejected = false;
    bool ovs_valid_accepted = false;
    bool ovs_hw_verified = false;
    bool awd_trip_ok = false;
    bool awd_notrip_ok = false;
    uint32_t vdda_bm = 0;
    int32_t temp_bm = 0;
    uint32_t vdda_ll = 0;
    int32_t temp_ll = 0;
    uint16_t ovs_raw = 0;

    /* 0. Verify uncalibrated failure behavior on CM4 (S2 requirement: never invent constants) */
    if ((ADC_BM_CalculateVDDA(24200U, 16U) == 0U) &&
        (ADC_BM_CalculateTemperature(12000U, 3300U, 16U) == ADC_BM_TEMPERATURE_ERROR) &&
        (ADC_LL_CalculateVDDA(24200U, 16U) == 0U) &&
        (ADC_LL_CalculateTemperature(12000U, 3300U, 16U) == ADC_LL_TEMPERATURE_ERROR))
    {
        uncal_check_ok = true;
    }

    /* Wait for M7 to publish valid calibration words */
    uint32_t wait_spins = 1000000U;
    while ((g_shared.cal_valid == 0U) && (wait_spins > 0U))
    {
        wait_spins--;
    }

    if (g_shared.cal_valid != 0U)
    {
        AdcBm_CalibrationData_t bm_cal = {
            .vrefint_cal = g_shared.vrefint_cal,
            .ts_cal1     = g_shared.ts_cal1,
            .ts_cal2     = g_shared.ts_cal2
        };
        AdcLl_CalibrationData_t ll_cal = {
            .vrefint_cal = g_shared.vrefint_cal,
            .ts_cal1     = g_shared.ts_cal1,
            .ts_cal2     = g_shared.ts_cal2
        };
        if (ADC_BM_InjectCalibration(&bm_cal) && ADC_LL_InjectCalibration(&ll_cal))
        {
            cal_injected_ok = true;
        }
    }

    /* 1. Register Parity Test between BM and LL on ADC3 */
    AdcBm_ChannelConfig_t bm_chs[3] = {
        { .channel = ADC_BM_CHANNEL_VREFINT,    .sampling_time = ADC_BM_SAMPLETIME_810CYCLES_5 },
        { .channel = ADC_BM_CHANNEL_TEMPSENSOR, .sampling_time = ADC_BM_SAMPLETIME_810CYCLES_5 },
        { .channel = ADC_BM_CHANNEL_VBAT,       .sampling_time = ADC_BM_SAMPLETIME_810CYCLES_5 }
    };

    AdcBm_ConfigEx_t bm_ex = {
        .adc_instance        = ADC3,
        .channels            = bm_chs,
        .num_channels        = 3U,
        .resolution          = ADC_BM_RESOLUTION_16B,
        .trigger_source      = 0U,
        .trigger_edge        = ADC_BM_TRIGGER_DISABLED,
        .boost_mode          = ADC_BM_BOOST_AUTO,
        .dma_mode            = ADC_BM_DMA_DISABLED,
        .dma_circular        = false,
        .buffer              = NULL,
        .buffer_length       = 0U,
        .kernel_clock_hz     = 25000000UL,
        .clock_mode          = ADC_BM_CLOCK_ASYNC,
        .prescaler           = ADC_BM_PRESC_DIV1,
        .analog_pin_port     = NULL,
        .analog_pin_idx      = 0U,
        .oversampling_enable = false,
        .oversampling_ratio  = 0U,
        .oversampling_shift  = 0U,
        .awd_enable          = false,
        .awd_all_channels    = false,
        .awd_channel         = 0U,
        .awd_high_threshold  = 0U,
        .awd_low_threshold   = 0U
    };

    static AdcBm_Handle_t h_bm;
    memset(&h_bm, 0, sizeof(h_bm));
    bm_init_ok = ADC_BM_InitEx(&h_bm, &bm_ex);

    /* Snapshot registers left by BM */
    uint32_t bm_cr    = ADC3->CR;
    uint32_t bm_cfgr  = ADC3->CFGR;
    uint32_t bm_cfgr2 = ADC3->CFGR2;
    uint32_t bm_pcsel = ADC3->PCSEL;
    uint32_t bm_sqr1  = ADC3->SQR1;
    uint32_t bm_smpr1 = ADC3->SMPR1;
    uint32_t bm_smpr2 = ADC3->SMPR2;

    ADC_BM_Deinit(&h_bm);

    /* Now init LL with the exact same configuration */
    AdcLl_ChannelConfig_t ll_chs[3] = {
        { .channel = ADC_LL_CHANNEL_VREFINT,    .sampling_time = ADC_LL_SAMPLETIME_810CYCLES_5 },
        { .channel = ADC_LL_CHANNEL_TEMPSENSOR, .sampling_time = ADC_LL_SAMPLETIME_810CYCLES_5 },
        { .channel = ADC_LL_CHANNEL_VBAT,       .sampling_time = ADC_LL_SAMPLETIME_810CYCLES_5 }
    };

    AdcLl_ConfigEx_t ll_ex = {
        .adc_instance        = ADC3,
        .channels            = ll_chs,
        .num_channels        = 3U,
        .resolution          = ADC_LL_RESOLUTION_16B,
        .trigger_source      = 0U,
        .trigger_edge        = ADC_LL_TRIGGER_DISABLED,
        .boost_mode          = ADC_LL_BOOST_AUTO,
        .dma_mode            = ADC_LL_DMA_DISABLED,
        .dma_circular        = false,
        .buffer              = NULL,
        .buffer_length       = 0U,
        .kernel_clock_hz     = 25000000UL,
        .clock_mode          = ADC_LL_CLOCK_ASYNC,
        .prescaler           = ADC_LL_PRESC_DIV1,
        .analog_pin_port     = NULL,
        .analog_pin_idx      = 0U,
        .oversampling_enable = false,
        .oversampling_ratio  = 0U,
        .oversampling_shift  = 0U,
        .awd_enable          = false,
        .awd_all_channels    = false,
        .awd_channel         = 0U,
        .awd_high_threshold  = 0U,
        .awd_low_threshold   = 0U
    };

    static AdcLl_Handle_t h_ll;
    memset(&h_ll, 0, sizeof(h_ll));
    ll_init_ok = ADC_LL_InitEx(&h_ll, &ll_ex);

    uint32_t ll_cr    = ADC3->CR;
    uint32_t ll_cfgr  = ADC3->CFGR;
    uint32_t ll_cfgr2 = ADC3->CFGR2;
    uint32_t ll_pcsel = ADC3->PCSEL;
    uint32_t ll_sqr1  = ADC3->SQR1;
    uint32_t ll_smpr1 = ADC3->SMPR1;
    uint32_t ll_smpr2 = ADC3->SMPR2;

    if ((bm_cr != ll_cr) ||
        (bm_cfgr != ll_cfgr) ||
        (bm_cfgr2 != ll_cfgr2) ||
        (bm_pcsel != ll_pcsel) ||
        (bm_sqr1 != ll_sqr1) ||
        (bm_smpr1 != ll_smpr1) ||
        (bm_smpr2 != ll_smpr2))
    {
        reg_compare_ok = false;
    }
    ADC_LL_Deinit(&h_ll);

    /* 2. ReadSingle on Internal Channels with BM */
    memset(&h_bm, 0, sizeof(h_bm));
    ADC_BM_InitEx(&h_bm, &bm_ex);
    (void)ADC_BM_Calibrate(ADC3);
    (void)ADC_BM_Enable(ADC3);

    uint16_t vref_raw_bm = 0;
    if (ADC_BM_ReadSingle(ADC3, ADC_BM_CHANNEL_VREFINT, ADC_BM_SAMPLETIME_810CYCLES_5, &vref_raw_bm, 100000U))
    {
        vdda_bm = ADC_BM_CalculateVDDA(vref_raw_bm, 16U);
        if ((vdda_bm >= 3100U) && (vdda_bm <= 3500U))
        {
            bm_read_vref_ok = true;
        }
    }

    uint16_t temp_raw_bm = 0;
    if (ADC_BM_ReadSingle(ADC3, ADC_BM_CHANNEL_TEMPSENSOR, ADC_BM_SAMPLETIME_810CYCLES_5, &temp_raw_bm, 100000U))
    {
        temp_bm = ADC_BM_CalculateTemperature(temp_raw_bm, vdda_bm, 16U);
        if ((temp_bm >= 10) && (temp_bm <= 90))
        {
            bm_read_temp_ok = true;
        }
    }
    ADC_BM_Deinit(&h_bm);

    /* 3. ReadSingle on Internal Channels with LL */
    memset(&h_ll, 0, sizeof(h_ll));
    ADC_LL_InitEx(&h_ll, &ll_ex);
    (void)ADC_LL_Calibrate(ADC3);
    (void)ADC_LL_Enable(ADC3);

    uint16_t vref_raw_ll = 0;
    if (ADC_LL_ReadSingle(ADC3, ADC_LL_CHANNEL_VREFINT, ADC_LL_SAMPLETIME_810CYCLES_5, &vref_raw_ll, 100000U))
    {
        vdda_ll = ADC_LL_CalculateVDDA(vref_raw_ll, 16U);
        if ((vdda_ll >= 3100U) && (vdda_ll <= 3500U))
        {
            ll_read_vref_ok = true;
        }
    }

    uint16_t temp_raw_ll = 0;
    if (ADC_LL_ReadSingle(ADC3, ADC_LL_CHANNEL_TEMPSENSOR, ADC_LL_SAMPLETIME_810CYCLES_5, &temp_raw_ll, 100000U))
    {
        temp_ll = ADC_LL_CalculateTemperature(temp_raw_ll, vdda_ll, 16U);
        if ((temp_ll >= 10) && (temp_ll <= 90))
        {
            ll_read_temp_ok = true;
        }
    }
    ADC_LL_Deinit(&h_ll);

    /* 4. T2 Resolution Sweep (16, 14, 12, 10, 8 bits) checking 2^(16-N) scaling */
    if (vref_raw_bm > 0U)
    {
        uint32_t v_16 = ADC_BM_CalculateVDDA(vref_raw_bm, 16U);
        uint32_t v_14 = ADC_BM_CalculateVDDA((uint16_t)(vref_raw_bm >> 2U), 14U);
        uint32_t v_12 = ADC_BM_CalculateVDDA((uint16_t)(vref_raw_bm >> 4U), 12U);
        uint32_t v_10 = ADC_BM_CalculateVDDA((uint16_t)(vref_raw_bm >> 6U), 10U);
        uint32_t v_8  = ADC_BM_CalculateVDDA((uint16_t)(vref_raw_bm >> 8U), 8U);

        int32_t d14 = (int32_t)v_16 - (int32_t)v_14; if (d14 < 0) d14 = -d14;
        int32_t d12 = (int32_t)v_16 - (int32_t)v_12; if (d12 < 0) d12 = -d12;
        int32_t d10 = (int32_t)v_16 - (int32_t)v_10; if (d10 < 0) d10 = -d10;
        int32_t d8  = (int32_t)v_16 - (int32_t)v_8;  if (d8 < 0)  d8 = -d8;

        if ((d14 > 10) || (d12 > 10) || (d10 > 15) || (d8 > 35))
        {
            res_sweep_ok = false;
        }
    }
    else
    {
        res_sweep_ok = false;
    }

    /* 5. Oversampling Truncation Validation */
    /* 16-bit + 256x (8 add bits) with shift 0 = 24 bits > 16 bits: must be rejected */
    memset(&h_bm, 0, sizeof(h_bm));
    AdcBm_ConfigEx_t ovs_bad = bm_ex;
    ovs_bad.oversampling_enable = true;
    ovs_bad.oversampling_ratio  = ADC_BM_OVERSAMPLING_256X;
    ovs_bad.oversampling_shift  = ADC_BM_OVERSAMPLING_SHIFT_NONE;
    if (!ADC_BM_InitEx(&h_bm, &ovs_bad))
    {
        ovs_trunc_rejected = true;
    }

    /* With shift 8 bits (24 - 8 = 16 bits): must be accepted */
    memset(&h_bm, 0, sizeof(h_bm));
    AdcBm_ConfigEx_t ovs_good = bm_ex;
    ovs_good.oversampling_enable = true;
    ovs_good.oversampling_ratio  = ADC_BM_OVERSAMPLING_256X;
    ovs_good.oversampling_shift  = ADC_BM_OVERSAMPLING_SHIFT_8B;
    if (ADC_BM_InitEx(&h_bm, &ovs_good))
    {
        ovs_valid_accepted = true;
        ADC_BM_Deinit(&h_bm);
    }

    /* 6. T7 Oversampling x16 shift 4: CFGR2 Register Readback & Hardware Conversion */
    memset(&h_bm, 0, sizeof(h_bm));
    AdcBm_ConfigEx_t ovs_t7 = bm_ex;
    ovs_t7.oversampling_enable = true;
    ovs_t7.oversampling_ratio  = ADC_BM_OVERSAMPLING_16X;   /* ratio = 16 -> OVSR = 15 */
    ovs_t7.oversampling_shift  = ADC_BM_OVERSAMPLING_SHIFT_4B; /* shift = 4 -> OVSS = 4 */
    if (ADC_BM_InitEx(&h_bm, &ovs_t7))
    {
        uint32_t cfgr2_val = ADC3->CFGR2;
        bool rovse_ok = ((cfgr2_val & ADC_CFGR2_ROVSE) != 0U);
        bool ovsr_ok  = (((cfgr2_val & ADC_CFGR2_OVSR_Msk) >> ADC_CFGR2_OVSR_Pos) == 15U);
        bool ovss_ok  = (((cfgr2_val & ADC_CFGR2_OVSS_Msk) >> ADC_CFGR2_OVSS_Pos) == 4U);

        if (rovse_ok && ovsr_ok && ovss_ok)
        {
            (void)ADC_BM_Calibrate(ADC3);
            (void)ADC_BM_Enable(ADC3);
            if (ADC_BM_ReadSingle(ADC3, ADC_BM_CHANNEL_VREFINT, ADC_BM_SAMPLETIME_810CYCLES_5, &ovs_raw, 100000U))
            {
                if ((ovs_raw >= 20000U) && (ovs_raw <= 30000U))
                {
                    ovs_hw_verified = true;
                }
            }
        }
        ADC_BM_Deinit(&h_bm);
    }

    /* 7. T5 Analog Watchdog (AWD1): Trip test and No-trip test */
    /* Trip test: window [0, 100] -> VREFINT ~24000 trips AWD1 */
    memset(&h_bm, 0, sizeof(h_bm));
    AdcBm_ConfigEx_t awd_cfg = bm_ex;
    awd_cfg.awd_enable          = true;
    awd_cfg.awd_all_channels    = true;
    awd_cfg.awd_high_threshold  = 100U;
    awd_cfg.awd_low_threshold   = 0U;
    if (ADC_BM_InitEx(&h_bm, &awd_cfg))
    {
        (void)ADC_BM_Enable(ADC3);
        uint16_t dummy_raw = 0;
        (void)ADC_BM_ReadSingle(ADC3, ADC_BM_CHANNEL_VREFINT, ADC_BM_SAMPLETIME_810CYCLES_5, &dummy_raw, 100000U);
        if ((ADC3->ISR & ADC_ISR_AWD1) != 0U)
        {
            ADC_BM_IRQHandler_ADC(&h_bm);
            if (ADC_BM_TakeAwdEvents(&h_bm) > 0U)
            {
                awd_trip_ok = true;
            }
        }
        ADC_BM_Deinit(&h_bm);
    }

    /* No-trip test: window [0, 65535] -> VREFINT does NOT trip AWD1 */
    memset(&h_bm, 0, sizeof(h_bm));
    AdcBm_ConfigEx_t awd_notrip = bm_ex;
    awd_notrip.awd_enable          = true;
    awd_notrip.awd_all_channels    = true;
    awd_notrip.awd_high_threshold  = 65535U;
    awd_notrip.awd_low_threshold   = 0U;
    if (ADC_BM_InitEx(&h_bm, &awd_notrip))
    {
        (void)ADC_BM_Enable(ADC3);
        uint16_t dummy_raw = 0;
        (void)ADC_BM_ReadSingle(ADC3, ADC_BM_CHANNEL_VREFINT, ADC_BM_SAMPLETIME_810CYCLES_5, &dummy_raw, 100000U);
        if ((ADC3->ISR & ADC_ISR_AWD1) == 0U)
        {
            awd_notrip_ok = true;
        }
        ADC_BM_Deinit(&h_bm);
    }

    /* Format report into g_shared.m4_to_m7 */
    SHARED_LOCK(HSEM_ID_M4_TO_M7_MBOX);
    char *p = (char *)g_shared.m4_to_m7;
    int pos = 0;

    const char *prefix = "ADC:B=";
    while (*prefix != '\0') { p[pos++] = *prefix++; }
    p[pos++] = bm_init_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'L'; p[pos++] = '=';
    p[pos++] = ll_init_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'R'; p[pos++] = 'g'; p[pos++] = '=';
    p[pos++] = reg_compare_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'U'; p[pos++] = '=';
    p[pos++] = uncal_check_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'C'; p[pos++] = '=';
    p[pos++] = cal_injected_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'V'; p[pos++] = '=';
    p[pos++] = (bm_read_vref_ok && ll_read_vref_ok) ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'T'; p[pos++] = '=';
    p[pos++] = (bm_read_temp_ok && ll_read_temp_ok) ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'R'; p[pos++] = 's'; p[pos++] = '=';
    p[pos++] = res_sweep_ok ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'O'; p[pos++] = '=';
    p[pos++] = (ovs_trunc_rejected && ovs_valid_accepted && ovs_hw_verified) ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'A'; p[pos++] = '=';
    p[pos++] = (awd_trip_ok && awd_notrip_ok) ? '1' : '0';

    p[pos++] = ' '; p[pos++] = 'V'; p[pos++] = 'd'; p[pos++] = '=';
    char tmp[10]; int ti = 0; uint32_t v = vdda_bm;
    if (v == 0) { p[pos++] = '0'; }
    else {
        while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) { p[pos++] = tmp[--ti]; }
    }

    p[pos] = '\0';

    __DMB();
    g_shared.m4_to_m7_len = (uint32_t)pos;
    SHARED_UNLOCK(HSEM_ID_M4_TO_M7_MBOX);
}

#endif /* ADC_DRIVER_SELFTEST */

