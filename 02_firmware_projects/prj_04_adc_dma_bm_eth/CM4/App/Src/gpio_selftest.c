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

static bool wait_var_cycles(volatile uint32_t *var, uint32_t expected, uint32_t timeout_cycles)
{
    uint32_t start = DWT->CYCCNT;
    while (*var != expected)
    {
        if ((DWT->CYCCNT - start) >= timeout_cycles)
        {
            return false;
        }
    }
    return true;
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

    /* Wait for CM7 to initialize SRAM4 block and write non-zero session_id (B1) */
    uint32_t start_cyc = DWT->CYCCNT;
    while (GPIO_TEST_SHARED->session_id == 0U)
    {
        /* 5 seconds @ 200 MHz = 1,000,000,000 cycles */
        if ((DWT->CYCCNT - start_cyc) >= 1000000000UL)
        {
            Line_Begin();
            Line_Str("TIMEOUT waiting for session_id from CM7");
            Line_End();
            GPIO_TEST_SHARED->cm4_all_passed = 0U;
            GPIO_TEST_SHARED->cm4_done = 1U;
            return;
        }
    }

    uint32_t session = GPIO_TEST_SHARED->session_id;

    /* Signal CM4 ready to CM7 with session_id */
    GPIO_TEST_SHARED->cm4_ready = session;
    __DMB();

    /* ---- Phase (a): Wait for CM7 solo run to finish ----------------------- */
    /* CM7 will set t15_phase = 2 and go_signal = session to start phase (b) */
    if (!wait_var_cycles(&GPIO_TEST_SHARED->t15_phase, 2U, 1000000000UL) ||
        !wait_var_cycles(&GPIO_TEST_SHARED->go_signal, session, 1000000000UL))
    {
        Line_Begin();
        Line_Str("TIMEOUT waiting for T15 phase (b) start");
        Line_End();
        GPIO_TEST_SHARED->cm4_all_passed = 0U;
        GPIO_TEST_SHARED->cm4_done = 1U;
        return;
    }

    /* ---- Phase (b): CM4 solo run (50,000 iterations) ---------------------- */
    GPIO_TEST_SHARED->in_loop_cm4 = 1U;
    __DMB();

    uint32_t corruptions_b = 0U;
    for (uint32_t i = 0U; i < 50000U; i++)
    {
        uint32_t val = (i & 1U);
        uint32_t m = GPIOF->MODER;
        m &= ~(3UL << (11U * 2U));
        m |= (val << (11U * 2U));
        GPIOF->MODER = m;

        /* Verify own bits */
        uint32_t readback = (GPIOF->MODER >> (11U * 2U)) & 3U;
        if (readback != val)
        {
            corruptions_b++;
        }
        /* Verify CM7 bits (PF10: 21:20) are 0 or 1 */
        uint32_t m7_bits = (GPIOF->MODER >> (10U * 2U)) & 3U;
        if (m7_bits > 1U)
        {
            corruptions_b++;
        }
    }

    GPIO_TEST_SHARED->in_loop_cm4 = 0U;
    GPIO_TEST_SHARED->t15_corruptions_phase_b = corruptions_b;
    /* Signal Phase (b) done */
    GPIO_TEST_SHARED->cm4_ready = session + 1U;
    __DMB();

    /* ---- Phase (c): Concurrent run (500,000 iterations) -------------------- */
    /* Wait for CM7 to signal phase 3 with go_signal = session + 1 */
    if (!wait_var_cycles(&GPIO_TEST_SHARED->t15_phase, 3U, 1000000000UL) ||
        !wait_var_cycles(&GPIO_TEST_SHARED->go_signal, session + 1U, 1000000000UL))
    {
        Line_Begin();
        Line_Str("TIMEOUT waiting for T15 phase (c) start");
        Line_End();
        GPIO_TEST_SHARED->cm4_all_passed = 0U;
        GPIO_TEST_SHARED->cm4_done = 1U;
        return;
    }

    GPIO_TEST_SHARED->in_loop_cm4 = 1U;
    __DMB();

    uint32_t corruptions_c = 0U;
    uint32_t overlap_c = 0U;

    for (uint32_t i = 0U; i < 500000U; i++)
    {
        if (GPIO_TEST_SHARED->in_loop_cm7 != 0U)
        {
            overlap_c++;
        }

        uint32_t val = (i & 1U);
        uint32_t m = GPIOF->MODER;
        m &= ~(3UL << (11U * 2U));
        m |= (val << (11U * 2U));

        __asm__ volatile("nop; nop; nop; nop;");
        GPIOF->MODER = m;
        __asm__ volatile("nop; nop;");

        uint32_t readback = (GPIOF->MODER >> (11U * 2U)) & 3U;
        if (readback != val)
        {
            corruptions_c++;
        }

        uint32_t m7_bits = (GPIOF->MODER >> (10U * 2U)) & 3U;
        if (m7_bits > 1U)
        {
            corruptions_c++;
        }
    }

    /* Restore PF11 to input mode (00) */
    GPIOF->MODER &= ~(3UL << (11U * 2U));
    GPIO_TEST_SHARED->in_loop_cm4 = 0U;
    GPIO_TEST_SHARED->t15_cm4_overlap_cnt = overlap_c;
    GPIO_TEST_SHARED->t15_total_calls_cm4 = 500000U;
    GPIO_TEST_SHARED->t15_cm4_corruptions_c = corruptions_c;
    __DMB();

    /* Wait for CM7 to complete Phase (c) */
    wait_var_cycles(&GPIO_TEST_SHARED->in_loop_cm7, 0U, 1000000000UL);

    uint32_t total = corruptions_c + GPIO_TEST_SHARED->t15_cm7_corruptions_c;

    /* B2: Calculated verdict for CM4 */
    bool cm4_ok = (corruptions_b == 0U) && (overlap_c > 0U) && (total > 0U);
    GPIO_TEST_SHARED->cm4_all_passed = cm4_ok ? 1U : 0U;
    GPIO_TEST_SHARED->cm4_done = 1U;
    __DMB();

    Line_Begin();
    Line_Str("T15 phase (b) CM4 solo: corruptions=");
    Line_U32(corruptions_b);
    Line_End();

    Line_Begin();
    Line_Str("T15 phase (c) CM4 corruptions=");
    Line_U32(corruptions_c);
    Line_Str(" overlap=");
    Line_U32(overlap_c);
    Line_End();

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
