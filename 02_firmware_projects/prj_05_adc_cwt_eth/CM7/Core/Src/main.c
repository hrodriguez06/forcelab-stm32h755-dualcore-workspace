/**
  ******************************************************************************
  * @file    main.c
  * @brief   CPU1 (Cortex-M7) boot trampoline (`prj_05_adc_cwt_eth`).
  *
  *          Boot order:
  *            1. LED_Init() + g_shared.magic = 0U
  *            2. Enable CPU2 boot and wait for CPU2 to park in STOP (D2CKRDY=0)
  *            3. MPU_Config() BEFORE caches
  *            4. SCB_EnableICache() + SCB_EnableDCache()
  *            5. ITCM_Init() (copies .itcm_text from Flash into ITCM RAM)
  *            6. HAL_Init() + HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4)
  *            7. SystemClock_Config() (400 MHz CM7 / 200 MHz CM4, VOS1, SMPS)
  *            8. CompensationCell_Init() + PeripheralClock_Config() (PLL2P 25 MHz ADC)
  *            9. Pre-enable CM4 peripheral clocks (DMA1, ADC12, GPIOB, TIM3)
  *           10. Shared_Init() + Shared_GpioLockInstall() + HSEM_BM_BootReleaseCpu2()
  *           11. app_main() -> starts FreeRTOS tasks (Heartbeat, Status, CWT, Net)
  ******************************************************************************
  */

#include "main.h"
#include "mpu_config.h"
#include "app_main.h"
#include "led_bm.h"
#include "shared_memory.h"
#include "hsem_bm.h"
#include "gpio_bm.h"

static void SystemClock_Config(void);
static void CompensationCell_Init(void);
static void PeripheralClock_Config(void);
static void ITCM_Init(void);

extern uint32_t _si_itcm;
extern uint32_t _s_itcm;
extern uint32_t _e_itcm;

int main(void)
{
    LED_Init();
    LED_BootStage(LED_BOOT_LEDS_ALIVE);

    /* Immediately invalidate SHARED_MAGIC upon boot / warm reset. */
    g_shared.magic = 0U;

#if CM4_BOOT_ENABLED
    HAL_RCCEx_EnableBootCore(RCC_BOOT_C2);

    HSEM_BM_Init();
    HSEM_BM_TimebaseStart();
    HSEM_BM_SetClearKey(BOARD_HSEM_CLEAR_KEY);
    if (HSEM_BM_BootWaitCpu2Parked(32000000UL) != HSEM_BM_OK)
    {
        Error_Handler();
    }
#endif

    MPU_Config();
    LED_BootStage(LED_BOOT_MPU_DONE);

    SCB_EnableICache();
    SCB_EnableDCache();
    LED_BootStage(LED_BOOT_CACHES_DONE);

    ITCM_Init();

    HAL_Init();
    LED_BootStage(LED_BOOT_HAL_DONE);

    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    SystemClock_Config();
    CompensationCell_Init();
    PeripheralClock_Config();
    LED_BootStage(LED_BOOT_CLOCKS_DONE);

    HSEM_BM_TimebaseStart();

#if CM4_BOOT_ENABLED
    /* Enable CM4 peripheral clocks from CM7 upfront to avoid cross-core
       read-modify-write races on RCC registers. */
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();

    /* Initialise shared RAM and publish SHARED_MAGIC before CPU2 can read it. */
    Shared_Init();

    /* Install cross-core GPIO lock hook before releasing CPU2 */
    Shared_GpioLockInstall();

    /* Release CPU2 via HSEM 0 */
    if (HSEM_BM_BootReleaseCpu2(HSEM_ID_BOOT_SYNC, 400000000UL) != HSEM_BM_OK)
    {
        Error_Handler();
    }
#endif

    app_main();

    for (;;)
    {
    }
}

static void ITCM_Init(void)
{
    uint32_t *src = &_si_itcm;
    uint32_t *dst = &_s_itcm;

    while (dst < &_e_itcm)
    {
        *dst++ = *src++;
    }

    SCB_CleanDCache();
    SCB_InvalidateICache();
    __DSB();
    __ISB();
}

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
}

#define COMP_CELL_CSI_MAX_CYCLES    800000UL
#define COMP_CELL_MAX_CYCLES        2000000UL

static void CompensationCell_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    *((volatile uint32_t *)0xE0001FB0) = 0xC5ACCE55U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    __HAL_RCC_CSI_ENABLE();

    uint32_t start = DWT->CYCCNT;
    while (__HAL_RCC_GET_FLAG(RCC_FLAG_CSIRDY) == RESET)
    {
        if ((uint32_t)(DWT->CYCCNT - start) >= COMP_CELL_CSI_MAX_CYCLES)
        {
            Error_Handler();
        }
    }

    if (GPIO_BM_EnableCompensationCell(COMP_CELL_MAX_CYCLES) != GPIO_BM_OK)
    {
        Error_Handler();
    }
}

static void PeripheralClock_Config(void)
{
    RCC_PeriphCLKInitTypeDef periph = {0};

    periph.PeriphClockSelection = RCC_PERIPHCLK_USART3 | RCC_PERIPHCLK_ADC;
    periph.Usart234578ClockSelection = RCC_USART234578CLKSOURCE_D2PCLK1;

    /* PLL2 configuration for ADC kernel clock (adc_ker_ck = 25 MHz) */
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
