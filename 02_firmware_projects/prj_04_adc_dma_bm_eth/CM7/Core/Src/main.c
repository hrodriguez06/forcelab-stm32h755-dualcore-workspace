/**
  ******************************************************************************
  * @file    main.c
  * @brief   CPU1 (Cortex-M7) boot trampoline.
  *
  *          Boot order, and why it is this order
  *          ------------------------------------
  *            1. MPU_Config()        - before the caches, so that .eth_ram,
  *                                     .dma_buf, .d3_ram and .shared_ram are
  *                                     already marked non-cacheable when the
  *                                     D-Cache comes up. Enabling the cache
  *                                     first would leave those regions with
  *                                     the default cacheable attributes.
  *            2. SCB_EnableICache()
  *            3. SCB_EnableDCache()
  *            4. HAL_Init()          - also sets NVIC_PRIORITYGROUP_4.
  *            5. SystemClock_Config()
  *            6. app_main()          - tasks and scheduler.
  ******************************************************************************
  */

#include "main.h"
#include "mpu_config.h"
#include "app_main.h"
#include "led_bm.h"
#include "shared_memory.h"
#include "hsem_bm.h"
#if defined(HSEM_DRIVER_SELFTEST) && HSEM_DRIVER_SELFTEST
#include "hsem_selftest_shared.h"
#endif
#if defined(UART_DRIVER_SELFTEST) && UART_DRIVER_SELFTEST
#include "uart_bm.h"

static void EarlyBoot_UartBm(void);
#endif
static void SystemClock_Config(void);
static void PeripheralClock_Config(void);
static void ITCM_Init(void);

/* Placed by CM7/linker/stm32h755zi_cm7.ld around the .itcm output section. */
extern uint32_t _si_itcm;   /* load address in FLASH   */
extern uint32_t _s_itcm;    /* run address in ITCM RAM */
extern uint32_t _e_itcm;

int main(void)
{
#if CM4_BOOT_ENABLED
    int32_t timeout;
#endif

    /* Before anything else, so that every later stage has a way to report.
       Bare-metal on purpose: no HAL, no clock tree, no MPU yet. */
    LED_Init();
    LED_BootStage(LED_BOOT_LEDS_ALIVE);

    /* Immediately invalidate SHARED_MAGIC upon boot / warm reset.
       .shared_ram is NOLOAD and preserves SRAM4 data across warm resets.
       Clearing magic here guarantees CPU2 will never mistakenly assume
       shared memory or clocks are ready from a previous execution. */
    g_shared.magic = 0U;

#if CM4_BOOT_ENABLED
    /* If CPU2 is already booting, it immediately enters STOP mode waiting for
       the HSEM notification. Wait for D2CKRDY to drop if CPU2 is active, but do
       not fail fatally if CPU2 is gated (BCM4=0) and waiting for BOOT_C2. */
    HSEM_BM_Init();
    HSEM_BM_TimebaseStart();
    (void)HSEM_BM_BootWaitCpu2Parked(200000000UL); /* 500 ms @ 400 MHz */
#endif

    /* Memory attributes first, then the caches.
       If the board stops with only the green LED lit, MPU_Config() denied a
       region the CPU needs: the next instruction fetch or stack push faults,
       and the fault handler cannot run either. */
    MPU_Config();
    LED_BootStage(LED_BOOT_MPU_DONE);

    SCB_EnableICache();
    SCB_EnableDCache();
    LED_BootStage(LED_BOOT_CACHES_DONE);

    /* Copy .itcm_text from its FLASH load address into ITCM. Must happen
       before anything calls a function placed there. */
    ITCM_Init();

    HAL_Init();
    LED_BootStage(LED_BOOT_HAL_DONE);

    /* Rule B.1 of README.md section 5: all four NVIC bits are preemption
       priority. HAL_Init() already does this; it is repeated because the
       interrupt priorities the drivers choose depend on it. */
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

#if defined(UART_DRIVER_SELFTEST) && UART_DRIVER_SELFTEST
    /* Call EarlyBoot_UartBm before clock tree configuration (using reset HSI 64MHz) */
    EarlyBoot_UartBm();
#endif

    SystemClock_Config();
    PeripheralClock_Config();
    LED_BootStage(LED_BOOT_CLOCKS_DONE);

    /* Initialise shared RAM and publish SHARED_MAGIC before CPU2 can read it.
       Placed outside the #if: clearing the block is independent of whether
       CPU2 is enabled to boot; only the ordering relative to HSEM 0 matters. */
    Shared_Init();

#if CM4_BOOT_ENABLED
    /* Enable CM4 peripheral clocks from CM7 upfront to avoid cross-core
       read-modify-write races on RCC->AHB1ENR / AHB4ENR / APB1LENR (Rule D). */
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
#if defined(UART_DRIVER_SELFTEST) && UART_DRIVER_SELFTEST
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
#endif

    /* Release CPU2 (Cortex-M4) boot if it was gated in RCC_GCR (BCM4=0). */
    HAL_RCCEx_EnableBootCore(RCC_BOOT_C2);

#if defined(HSEM_DRIVER_SELFTEST) && HSEM_DRIVER_SELFTEST
    /* ---- Phase F2 Boot Tests (H5, H5b, H5c) ----------------------------- */
    /* H5: Verify D2CKRDY stays 0 for 500 ms after CPU2 parks */
    bool h5_ok = true;
    for (uint32_t ms = 0; ms < 500; ms++)
    {
        if ((RCC->CR & RCC_CR_D2CKRDY) != 0U)
        {
            h5_ok = false;
            break;
        }
        for (volatile uint32_t s = 0; s < 40000U; s++) { __NOP(); }
    }

    /* H5b: Release HSEM before magic is valid */
    g_shared.magic = 0U;
    __DMB();
    HSEM_BM_FastTake(HSEM_ID_BOOT_SYNC);
    HSEM_BM_Release(HSEM_ID_BOOT_SYNC, 0U);
    for (volatile uint32_t s = 0; s < 400000U; s++) { __NOP(); } /* ~10 ms */
    bool h5b_ok = ((RCC->CR & RCC_CR_D2CKRDY) == 0U);

    /* H5c: Spurious EXTI event to CPU2 via EXTI_SWIER1 */
    EXTI_D2->C2EMR1 |= (1UL << 23);
    EXTI->SWIER1 |= (1UL << 23);
    for (volatile uint32_t s = 0; s < 400000U; s++) { __NOP(); } /* ~10 ms */
    EXTI_D2->C2EMR1 &= ~(1UL << 23);
    bool h5c_ok = ((RCC->CR & RCC_CR_D2CKRDY) == 0U);

    /* Now set valid magic and release CPU2 */
    Shared_Init();
    if (HSEM_BM_BootReleaseCpu2(HSEM_ID_BOOT_SYNC, 400000000UL) != HSEM_BM_OK)
    {
        Error_Handler();
    }
    bool h5_release_ok = ((RCC->CR & RCC_CR_D2CKRDY) != 0U);

    HSEM_TEST_SHARED->h5_pass = (h5_ok && h5b_ok && h5c_ok && h5_release_ok) ? 1U : 0U;
#else
    /* The clock tree is up and shared memory is ready: release CPU2. */
    if (HSEM_BM_BootReleaseCpu2(HSEM_ID_BOOT_SYNC, 400000000UL) != HSEM_BM_OK)
    {
        Error_Handler();
    }
#endif

    app_main();

    /* app_main() starts the scheduler and does not return. */
    for (;;)
    {
    }
}

/**
  * @brief  Copies the .itcm output section from FLASH into ITCM RAM.
  *
  *         The linker script declares `.itcm ... >ITCMRAM AT> FLASH`, so the
  *         contents are stored in FLASH at _si_itcm and expected at _s_itcm
  *         when they run. Nothing performs that copy on its own: the CMSIS
  *         startup file only copies _sidata -> _sdata and zeroes .bss.
  *
  *         While .itcm_text is empty this costs nothing and changes nothing.
  *         The moment someone writes
  *             __attribute__((section(".itcm_text"))) void fast_isr(void)
  *         without this, the core branches into uninitialised ITCM at
  *         0x00000000 and HardFaults on the first call.
  */
static void ITCM_Init(void)
{
    uint32_t *src = &_si_itcm;
    uint32_t *dst = &_s_itcm;

    /* The linker aligns both ends of .itcm to 8, so a word copy is exact. */
    while (dst < &_e_itcm)
    {
        *dst++ = *src++;
    }

    /* The copy went out through the D-Cache as data; the core is about to
       fetch it as instructions. Clean the data side, invalidate the
       instruction side, then synchronise before anything can branch there. */
    SCB_CleanDCache();
    SCB_InvalidateICache();
    __DSB();
    __ISB();
}

/**
  * @brief  400 MHz from the 8 MHz ST-LINK MCO, voltage scale 1.
  *
  *         ES0445 2.3.5: 480 MHz needs VOS0, which is unavailable on silicon
  *         revisions Y and W. 400 MHz with VOS1 is valid on every revision, so
  *         this project does not gate on REV_ID in DBGMCU_IDC. Read that field
  *         first if you ever raise this to 480 MHz.
  *
  *         Resulting tree: SYSCLK 400 MHz, HCLK 200 MHz (AHB, and therefore
  *         the Ethernet MAC clock), APB1..APB4 100 MHz.
  */
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    /* NUCLEO-H755ZI-Q hardware uses SMPS directly to supply Vcore */
    HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY);

    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    while (__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY) == 0U)
    {
    }

    /* HSE = 8 MHz in bypass: the NUCLEO-H755ZI-Q has no MCU crystal, the
       clock comes from the ST-LINK MCO. */
    osc.OscillatorType   = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState         = RCC_HSE_BYPASS;
    osc.HSIState         = RCC_HSI_OFF;
    osc.CSIState         = RCC_CSI_OFF;
    osc.PLL.PLLState     = RCC_PLL_ON;
    osc.PLL.PLLSource    = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM         = 4;      /* 8 MHz / 4   = 2 MHz  reference */
    osc.PLL.PLLN         = 400;    /* 2 MHz * 400 = 800 MHz VCO      */
    osc.PLL.PLLFRACN     = 0;
    osc.PLL.PLLP         = 2;      /* 800 / 2     = 400 MHz SYSCLK   */
    osc.PLL.PLLQ         = 4;
    osc.PLL.PLLR         = 2;
    osc.PLL.PLLVCOSEL    = RCC_PLL1VCOWIDE;
    osc.PLL.PLLRGE       = RCC_PLL1VCIRANGE_1;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        Error_Handler();
    }

    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK   | RCC_CLOCKTYPE_HCLK |
                         RCC_CLOCKTYPE_D1PCLK1  | RCC_CLOCKTYPE_PCLK1 |
                         RCC_CLOCKTYPE_PCLK2    | RCC_CLOCKTYPE_D3PCLK1;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.SYSCLKDivider  = RCC_SYSCLK_DIV1;
    clk.AHBCLKDivider  = RCC_HCLK_DIV2;
    clk.APB3CLKDivider = RCC_APB3_DIV2;
    clk.APB1CLKDivider = RCC_APB1_DIV2;
    clk.APB2CLKDivider = RCC_APB2_DIV2;
    clk.APB4CLKDivider = RCC_APB4_DIV2;

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK)
    {
        Error_Handler();
    }

    /* The I/O compensation cell trims the slew rate of the fast I/O banks. ST
       recommends it above 100 MHz HCLK for high-speed interfaces, and the
       50 MHz RMII pins are exactly that case. */
    __HAL_RCC_CSI_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    HAL_EnableCompensationCell();
}

/**
  * @brief  Kernel clocks for the peripherals this firmware uses.
  * @note   The Ethernet MAC has no kernel clock selection: it runs on HCLK,
  *         and the RMII 50 MHz reference arrives from the LAN8742A on PA1.
  *         USART3 is the ST-LINK virtual COM port.
  */
static void PeripheralClock_Config(void)
{
    RCC_PeriphCLKInitTypeDef periph = {0};

    /* USART3 (ST-LINK VCP) on D2 APB1 */
    periph.PeriphClockSelection = RCC_PERIPHCLK_USART3 | RCC_PERIPHCLK_ADC;
    periph.Usart234578ClockSelection = RCC_USART234578CLKSOURCE_D2PCLK1;

    /* PLL2 configuration for ADC kernel clock (adc_ker_ck = 25 MHz)
     * HSE (8 MHz) / PLL2M (4) = 2 MHz (VCIRANGE_1: 2-4 MHz)
     * 2 MHz * PLL2N (100)     = 200 MHz VCO (VCOMEDIUM: 150-420 MHz)
     * 200 MHz / PLL2P (8)     = 25 MHz adc_ker_ck
     */
    periph.PLL2.PLL2M      = 4;
    periph.PLL2.PLL2N      = 100;
    periph.PLL2.PLL2P      = 8;
    periph.PLL2.PLL2Q      = 2;
    periph.PLL2.PLL2R      = 2;
    periph.PLL2.PLL2RGE    = RCC_PLL2VCIRANGE_1;
    periph.PLL2.PLL2VCOSEL = RCC_PLL2VCOMEDIUM;
    periph.PLL2.PLL2FRACN  = 0;
    periph.AdcClockSelection = RCC_ADCCLKSOURCE_PLL2;

    if (HAL_RCCEx_PeriphCLKConfig(&periph) != HAL_OK)
    {
        Error_Handler();
    }
}

void Error_Handler(void)
{
    __disable_irq();
    LED_PanicRed();
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
static void EarlyBoot_UartBm(void)
{
    /* HSI is 64 MHz at reset, USART234578SEL=000 -> 64 MHz kernel clock */
    UartBm_Pin_t tx = { .port = GPIOD, .pin_idx = 8, .af = 7 };
    UartBm_Pin_t rx = { .port = GPIOD, .pin_idx = 9, .af = 7 };
    UartBm_Config_t cfg = {
        .instance        = USART3,
        .kernel_clock_hz = 64000000UL,
        .baudrate        = 115200UL,
        .data_bits       = UART_BM_DATA_BITS_8,
        .stop_bits       = UART_BM_STOP_BITS_1,
        .parity          = UART_BM_PARITY_NONE,
        .oversampling_8  = false,
        .fifo_enable     = false,
        .hw_flow         = UART_BM_HW_FLOW_NONE,
        .enable_clock    = true,
        .tx_pin          = &tx,
        .rx_pin          = &rx
    };
    UartBm_Handle_t h_bm;
    if (UART_BM_Init(&h_bm, &cfg))
    {
        const char msg[] = "\r\n[boot_bm] CPU1 alive before clock tree (HSI 64MHz, USART3 115200)\r\n";
        (void)UART_BM_Write(&h_bm, (const uint8_t *)msg, sizeof(msg) - 1, 100000U);
        (void)UART_BM_Flush(&h_bm, 100000U);
    }
}
#endif

