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
#include "shared_memory.h"
#include "stm32h7xx_hal.h"

#include <string.h>

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
