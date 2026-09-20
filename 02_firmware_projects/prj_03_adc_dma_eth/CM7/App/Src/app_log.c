/**
  ******************************************************************************
  * @file    app_log.c
  * @brief   Line-oriented console log for the bring-up, over the ST-LINK VCP.
  ******************************************************************************
  */

#include "app_log.h"
#include "uart_hal.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define APP_LOG_LINE_MAX                 160

static SemaphoreHandle_t g_log_mutex;
static char              g_log_line[APP_LOG_LINE_MAX];

void App_Log_Init(void)
{
    (void)UART_Console_Init(UART_CONSOLE_DEFAULT_BAUDRATE);
    g_log_mutex = xSemaphoreCreateMutex();
}

void App_Log(const char *format, ...)
{
    va_list args;
    int     written;
    int     locked = 0;

    /* The log buffer is shared, so it needs the mutex once several tasks can
       run. Before the scheduler starts there is only one context. */
    if ((g_log_mutex != NULL) && (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING))
    {
        if (xSemaphoreTake(g_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            return;
        }
        locked = 1;
    }

    va_start(args, format);
    written = vsnprintf(g_log_line, sizeof(g_log_line) - 2U, format, args);
    va_end(args);

    if (written > 0)
    {
        if ((size_t)written > (sizeof(g_log_line) - 3U))
        {
            written = (int)(sizeof(g_log_line) - 3U);
        }

        g_log_line[written]     = '\r';
        g_log_line[written + 1] = '\n';

        UART_Console_Write((const uint8_t *)g_log_line, (uint16_t)(written + 2));
    }

    if (locked != 0)
    {
        (void)xSemaphoreGive(g_log_mutex);
    }
}
