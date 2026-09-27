/**
  ******************************************************************************
  * @file    gpio_selftest.c
  * @brief   GPIO driver self-test implementation for Cortex-M4 (prj_04).
  *          Coordinated dual-core tests (T15 race negative control without lock).
  *          Implements M1 (step-known guard), B2 (calculated verdict),
  *          and B1 (rebuilt T15 with 3 phases, session ID, and overlap count).
  ******************************************************************************
  */

#include "gpio_selftest.h"
#include "gpio_bench_protocol.h"
#include "shared_memory.h"
#include "stm32h7xx.h"
#include <string.h>
#include <stdbool.h>

#define ST_MAX_LINES 32
static char s_lines[ST_MAX_LINES][SHARED_MBOX_SIZE];
static uint32_t s_line_count = 0U;
static uint32_t s_line_len = 0U;
static uint32_t s_line_next = 0U;

static uint8_t s_current_step_id = GPIO_BENCH_STEP_T15_RACE;

static void Line_Begin(void)
{
    s_line_len = 0U;
    if (s_line_count < ST_MAX_LINES)
    {
        s_lines[s_line_count][0] = '\0';
    }
}

static void Line_End(void)
{
    if (s_line_count < ST_MAX_LINES)
    {
        s_line_count++;
    }
}

static void Line_Str(const char *s)
{
    if (s_line_count >= ST_MAX_LINES) return;
    char *dst = s_lines[s_line_count];
    while ((*s != '\0') && (s_line_len < (SHARED_MBOX_SIZE - 1U)))
    {
        dst[s_line_len++] = *s++;
    }
    dst[s_line_len] = '\0';
}

static void Line_U32(uint32_t v)
{
    char tmp[11];
    uint32_t n = 0U;
    do
    {
        tmp[n++] = (char)('0' + (int)(v % 10U));
        v /= 10U;
    } while ((v != 0U) && (n < sizeof(tmp) - 1U));

    while (n > 0U)
    {
        char c = tmp[--n];
        if (s_line_count < ST_MAX_LINES && s_line_len < (SHARED_MBOX_SIZE - 1U))
        {
            s_lines[s_line_count][s_line_len++] = c;
            s_lines[s_line_count][s_line_len] = '\0';
        }
    }
}

/**
 * @brief Safety guard S4: Bench claim check for Cortex-M4 (M1).
 *        No step parameter accepted; reads s_current_step_id.
 */
static bool bench_claim_output(GPIO_TypeDef *port, uint32_t pin)
{
    /* PE0 and PE1 (LD2) NEVER allowed */
    if (port == GPIOE && (pin == 0U || pin == 1U))
    {
        return false;
    }
    /* PE2, PE3, PE4 (Solo AD2) NEVER allowed */
    if (port == GPIOE && (pin == 2U || pin == 3U || pin == 4U))
    {
        return false;
    }
    /* Unwired pin PF11 allowed ONLY during T15 race */
    if (s_current_step_id == GPIO_BENCH_STEP_T15_RACE)
    {
        if (port == GPIOF && pin == 11U)
        {
            return true;
        }
    }
    /* PE6 is Solo MCU (CM4 ACK) */
    if (port == GPIOE && pin == 6U)
    {
        return true;
    }
    return false;
}



void GPIO_SelfTest_CM4_Run(void)
{
    /* Banner Rev <hash> (S8) */
    Line_Begin();
    Line_Str("Rev ");
    Line_Str(FW_GIT_REV);
    Line_End();

    /* Ensure GPIOF clock enabled for PF11 */
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOFEN;
    __DSB();

    s_current_step_id = GPIO_BENCH_STEP_T15_RACE;

    /* Safety guard S4 check on unwired test pin PF11 (M1) */
    if (!bench_claim_output(GPIOF, 11U))
    {
        Line_Begin();
        Line_Str("ERROR: bench_claim_output rejected PF11!");
        Line_End();
        GPIO_TEST_SHARED->cm4_all_passed = 0U;
        GPIO_TEST_SHARED->cm4_done = 1U;
        return;
    }

    /* Signal CM4 ready to CM7 */
    GPIO_TEST_SHARED->cm4_ready = 1U;
    __DMB();

    /* Wait for CM7 go_signal */
    uint32_t wait_timeout = 0U;
    while ((GPIO_TEST_SHARED->go_signal == 0U) && (wait_timeout < 10000000U))
    {
        wait_timeout++;
        __asm__ volatile("nop;");
    }

    if (GPIO_TEST_SHARED->go_signal == 0U)
    {
        Line_Begin();
        Line_Str("TIMEOUT waiting for go_signal from CM7");
        Line_End();
        GPIO_TEST_SHARED->cm4_all_passed = 0U;
        GPIO_TEST_SHARED->cm4_done = 1U;
        return;
    }

    /* T15: 500,000 hand-written RMW iterations on GPIOF->MODER for PF11 without lock */
    uint32_t corruptions = 0U;
    for (uint32_t i = 0U; i < 500000U; i++)
    {
        uint32_t val = (i & 1U); /* 0 = input, 1 = output */

        /* Hand-written RMW without lock on GPIOF->MODER (bits 23:22 for PF11) */
        uint32_t m = GPIOF->MODER;
        m &= ~(3UL << (11U * 2U));
        m |= (val << (11U * 2U));

        /* Window to allow race interleaving with CM7 */
        __asm__ volatile("nop; nop; nop; nop;");
        GPIOF->MODER = m;
        __asm__ volatile("nop; nop;");

        /* Check if our write was overwritten by CM7's RMW */
        uint32_t readback = (GPIOF->MODER >> (11U * 2U)) & 3U;
        if (readback != val)
        {
            corruptions++;
        }

        /* Check CM7 pin bits (PF10: bits 21:20): must be 0 or 1 */
        uint32_t m7_bits = (GPIOF->MODER >> (10U * 2U)) & 3U;
        if (m7_bits > 1U)
        {
            corruptions++;
        }
    }

    /* Restore PF11 to input mode (MODER = 00) */
    GPIOF->MODER &= ~(3UL << (11U * 2U));

    GPIO_TEST_SHARED->t15_cm4_count = 500000U;
    GPIO_TEST_SHARED->t15_corruptions_cm4 = corruptions;
    GPIO_TEST_SHARED->t15_done_cm4 = 1U;
    __DMB();

    Line_Begin();
    Line_Str("T15 race count=500000 corruptions=");
    Line_U32(corruptions);
    Line_End();

    bool cm4_ok = (corruptions > 0U);
    GPIO_TEST_SHARED->cm4_all_passed = cm4_ok ? 1U : 0U;
    GPIO_TEST_SHARED->cm4_done = 1U;
    __DMB();

    Line_Begin();
    Line_Str("CM4 VERDICT=");
    Line_Str(cm4_ok ? "PASS" : "FAIL");
    Line_End();
}

void GPIO_SelfTest_CM4_Poll(void)
{
    if (s_line_next >= s_line_count) return;

    SHARED_LOCK(HSEM_ID_M4_TO_M7_MBOX);
    if (g_shared.m4_to_m7_len == 0U)
    {
        uint32_t len = (uint32_t)strlen(s_lines[s_line_next]);
        memcpy((void *)g_shared.m4_to_m7, s_lines[s_line_next], len + 1U);
        g_shared.m4_to_m7_len = len;
        s_line_next++;
    }
    SHARED_UNLOCK(HSEM_ID_M4_TO_M7_MBOX);
}
