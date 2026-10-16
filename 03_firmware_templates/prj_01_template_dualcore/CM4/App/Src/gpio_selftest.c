/**
  ******************************************************************************
  * @file    gpio_selftest.c
  * @brief   00_gpio selftest, CPU2 side (preset selftest-gpio).
  *
  *          CPU2 has no console. It serves the commands CPU1 writes in the
  *          SRAM4 block (Common/Inc/gpio_selftest.h), runs the common tests on
  *          its own copy of the driver, publishes the results there, and CPU1
  *          prints them. It never touches a pin the bench wires to the AD2:
  *          only PF6..PF11 (T1, T2, TB, B1, T15), which nothing is wired to.
  ******************************************************************************
  */

#include "gpio_selftest.h"
#include "gpio_bm.h"
#include "fw_build_id.h"
#include "shared_memory.h"
#include "stm32h7xx_hal.h"

#include <string.h>

#define PE_SPARE6   6U

static volatile uint32_t s_cm4_exti3_count = 0U;
static volatile uint32_t s_cm4_exti4_count = 0U;

void EXTI3_IRQHandler(void)
{
    GPIOE->BSRR = (1UL << PE_SPARE6);
    const uint32_t pend = (uint32_t)GPIO_BM_ExtiTakePending(1UL << 3U);
    s_cm4_exti3_count++;
    GST_AREA->m4_exti_count++;
    GST_AREA->m4_exti_last_pending = pend;
    GPIOE->BSRR = (1UL << (PE_SPARE6 + 16U));
}

void EXTI4_IRQHandler(void)
{
    GPIOE->BSRR = (1UL << PE_SPARE6);
    const uint32_t pend = (uint32_t)GPIO_BM_ExtiTakePending(1UL << 4U);
    s_cm4_exti4_count++;
    GST_AREA->m4_exti_count++;
    GST_AREA->m4_exti_last_pending = pend;
    GPIOE->BSRR = (1UL << (PE_SPARE6 + 16U));
}

/* CPU1 sends the next command within seconds; a minute of silence means it
   stopped (a fault, or a bench abort that skipped the DONE). CPU2 then goes on
   to the application loop instead of waiting for ever. */
#define GST_CM4_IDLE_TIMEOUT_MS   60000U

void GPIO_Selftest_CM4_Run(void)
{
    GstArea_t *a = GST_AREA;
    uint32_t   last_seq;
    const uint32_t session = a->session;

    /* CPU1 opened the session before releasing this core. */
    if (a->magic != GST_AREA_MAGIC)
    {
        return;
    }

    for (uint32_t i = 0U; i < sizeof(a->m4_rev); i++)
    {
        a->m4_rev[i] = '\0';
    }
    const char *rev = FW_GIT_REV;
    for (uint32_t i = 0U; (i + 1U < sizeof(a->m4_rev)) && (rev[i] != '\0'); i++)
    {
        a->m4_rev[i] = rev[i];
    }
    for (uint32_t i = 0U; i < sizeof(a->m4_build_id); i++)
    {
        a->m4_build_id[i] = '\0';
    }
    const char *id = FW_BUILD_ID;
    for (uint32_t i = 0U; (i + 1U < sizeof(a->m4_build_id)) && (id[i] != '\0'); i++)
    {
        a->m4_build_id[i] = id[i];
    }

    last_seq = a->cmd_seq;
    __DMB();
    a->m4_ready = session;

    for (;;)
    {
        const uint32_t t0 = HAL_GetTick();
        while (a->cmd_seq == last_seq)
        {
            if ((HAL_GetTick() - t0) > GST_CM4_IDLE_TIMEOUT_MS)
            {
                a->m4_timeout = 1U;
                return;
            }
        }
        __DMB();
        last_seq = a->cmd_seq;
        const uint32_t cmd = a->cmd;

        switch (cmd)
        {
        case GST_CMD_T1:
            gst_t1_run(GST_TIER_BM,  &a->m4_t1[GST_TIER_BM]);
            gst_t1_run(GST_TIER_LL,  &a->m4_t1[GST_TIER_LL]);
            gst_t1_run(GST_TIER_HAL, &a->m4_t1[GST_TIER_HAL]);
            break;
        case GST_CMD_T2:
            gst_t2_run(&a->m4_t2);
            break;
        case GST_CMD_TB:
            gst_tb_run(&a->m4_tb);
            break;
        case GST_CMD_B1:
            gst_b1_run(&a->m4_b1);
            break;
        case GST_CMD_SHIM_ON:
            gst_shim_install(true);
            break;
        case GST_CMD_SHIM_OFF:
            gst_shim_install(false);
            break;
        case GST_CMD_RACE_ARM:
        {
            GstRaceResult_t res;
            const GstRace_t m     = (GstRace_t)a->arg0;
            const uint32_t  iters = a->arg1;
            __DMB();
            a->armed_seq = last_seq;
            const uint32_t tg = HAL_GetTick();
            while (a->go == 0U)
            {
                if ((HAL_GetTick() - tg) > GST_CM4_IDLE_TIMEOUT_MS)
                {
                    a->m4_timeout = 1U;
                    return;
                }
            }
            gst_race_run(m, iters, &res);
            memcpy((void *)&a->m4_race, &res, sizeof(res));
            break;
        }
        case GST_CMD_F2_CONFIG:
        {
            const bool pe6_auth = (a->arg1 & GST_F2_AUTH_PE6) != 0U;
            if (!pe6_auth)
            {
                a->m4_exti_status = (uint32_t)GPIO_BM_ERR_PARAM;
                break;
            }
            const uint32_t pin = a->arg0 & 0xFFU;
            const uint32_t trigger = (a->arg0 >> 8U) & 0xFFU;
            const bool interrupt = ((a->arg1 >> 1U) & 1U) != 0U;
            const bool event = (a->arg1 & 1U) != 0U;
            (void)GPIO_BM_ConfigOutput(GPIOE, PE_SPARE6, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 0U);
            a->m4_exti_status = (uint32_t)GPIO_BM_ExtiConfig(GPIOE, pin, trigger, interrupt, event);
            /* M4: Keep NVIC enabled even for EVENT-only to verify EXTI masking isolates the interrupt */
            const IRQn_Type irqn = GPIO_BM_ExtiGetIRQn(pin);
            if (irqn != GPIO_BM_EXTI_IRQN_INVALID)
            {
                NVIC_SetPriority(irqn, 2U);
                NVIC_ClearPendingIRQ(irqn);
                NVIC_EnableIRQ(irqn);
            }
            break;
        }
        case GST_CMD_F2_DISABLE:
        {
            const uint32_t pin = a->arg0 & 0xFFU;
            const bool cleanup = (a->arg1 & 1U) != 0U;
            if (cleanup)
            {
                const IRQn_Type irqn = GPIO_BM_ExtiGetIRQn(pin);
                if (irqn != GPIO_BM_EXTI_IRQN_INVALID)
                {
                    NVIC_DisableIRQ(irqn);
                    NVIC_ClearPendingIRQ(irqn);
                }
                (void)GPIO_BM_ConfigInput(GPIOE, PE_SPARE6, GPIO_BM_PULL_NONE);
            }
            a->m4_exti_status = (uint32_t)GPIO_BM_ExtiDisable(GPIOE, pin);
            break;
        }
        case GST_CMD_F2_COUNT_RESET:
            a->m4_exti_count = 0U;
            a->m4_exti_last_pending = 0U;
            s_cm4_exti3_count = 0U;
            s_cm4_exti4_count = 0U;
            break;
        case GST_CMD_DONE:
            __DMB();
            a->ack_seq = last_seq;
            return;
        default:
            break;
        }

        __DMB();
        a->ack_seq = last_seq;
    }
}
