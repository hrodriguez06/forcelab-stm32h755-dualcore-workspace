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
#if defined(GPIO_DRIVER_SELFTEST)
#include "gpio_selftest.h"
#endif
#if defined(TIM_DRIVER_SELFTEST)
#include "tim_selftest.h"
#endif
#if defined(SPI_DRIVER_SELFTEST)
#include "spi_selftest.h"
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
    HSEM_BM_SetClearKey(BOARD_HSEM_CLEAR_KEY);
    /* m5: 500 ms @ 64 MHz HSI = 32,000,000 cycles */
    if (HSEM_BM_BootWaitCpu2Parked(32000000UL) != HSEM_BM_OK)
    {
        Error_Handler();
    }
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

    SystemClock_Config();
    PeripheralClock_Config();
    LED_BootStage(LED_BOOT_CLOCKS_DONE);

    /* Initialise shared RAM and publish SHARED_MAGIC before CPU2 can read it.
       Placed outside the #if: clearing the block is independent of whether
       CPU2 is enabled to boot; only the ordering relative to HSEM 0 matters. */
    Shared_Init();

#if CM4_BOOT_ENABLED
    /* From the release on, both cores can configure pins, and the GPIO
       configuration registers have one copy for both: every write of
       gpio_bm/_ll/_hal on this core takes the cross-core lock from here on.
       HSEM_BM_Init() and the DWT timebase are already running, and the cycle
       limit assumes the 400 MHz clock set just above. */
    Shared_GpioLockInstall();

#if defined(GPIO_DRIVER_SELFTEST)
    /* Zeroes the selftest block in SRAM4 and opens a new session, so CPU2
       never acts on flags a previous run left there (SRAM4 survives a warm
       reset). Must happen before CPU2 is released. */
    GPIO_Selftest_CM7_PreRelease();
#endif
#if defined(TIM_DRIVER_SELFTEST)
    /* Same for the 03_tim selftest block (01_shared_drivers/03_tim). */
    TIM_Selftest_CM7_PreRelease();
#endif
#if defined(SPI_DRIVER_SELFTEST)
    /* Same for the 06_spi selftest block (01_shared_drivers/06_spi). */
    SPI_Selftest_CM7_PreRelease();
#endif

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

    /* Supply first. After a power-on reset the system runs with ACTVOSRDY
       invalid until PWR_CR3 is written, and VOS can only change once it is
       valid (RM0399 7.4.1): without this the VOSRDY wait below never ends. The
       configuration is write-once until the next power-on reset, so a warm
       reset hides the omission when an earlier image wrote it. It must match
       the board: MB1363 is "internal SMPS only" by default (UM2408 7.4.8,
       table 10); a mismatch locks the board until BOOT0 recovery. */
    if (HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY) != HAL_OK)
    {
        Error_Handler();
    }

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

    periph.PeriphClockSelection = RCC_PERIPHCLK_USART3;
    periph.Usart234578ClockSelection = RCC_USART234578CLKSOURCE_D2PCLK1;

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
