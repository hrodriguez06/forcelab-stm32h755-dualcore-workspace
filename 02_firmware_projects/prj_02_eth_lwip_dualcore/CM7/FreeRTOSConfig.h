/**
  ******************************************************************************
  * @file    FreeRTOSConfig.h
  * @brief   FreeRTOS kernel configuration for STM32H755ZI CPU1 (Cortex-M7).
  *
  *          Interrupt priority contract (README.md section 5.B)
  *          --------------------------------------------------
  *          NVIC_PRIORITYGROUP_4: all four implemented bits are preemption
  *          priority, no subpriority. An ISR may call the ...FromISR API only
  *          if its priority is numerically >= 5. The Ethernet interrupt is at
  *          6 (Custom_Drivers/08_eth/eth_hal.c), so it qualifies.
  *
  *          configMAX_PRIORITIES is 56 because ST's lwIP sys_arch is written
  *          against CMSIS-RTOS v2, whose FreeRTOS binding refuses to compile
  *          with any other value.
  ******************************************************************************
  */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t SystemCoreClock;

/* ---- Scheduler --------------------------------------------------------- */
#define configUSE_PREEMPTION                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0  /* mandatory above 32
                                                       priorities           */
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                       (SystemCoreClock)
#define configTICK_RATE_HZ                       ((TickType_t)1000)
#define configMAX_PRIORITIES                     56
#define configMINIMAL_STACK_SIZE                 ((uint16_t)128)
#define configMAX_TASK_NAME_LEN                  16
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configQUEUE_REGISTRY_SIZE                8
#define configUSE_TASK_NOTIFICATIONS             1
#define configUSE_TRACE_FACILITY                 1
#define configUSE_STATS_FORMATTING_FUNCTIONS     0

/* ---- Memory ------------------------------------------------------------ *
 * heap_4 in .bss, which the linker script places in DTCM: zero wait state and
 * never touched by any DMA. Every DMA-visible buffer lives in D2 SRAM through
 * an explicit section attribute instead.
 */
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configSUPPORT_STATIC_ALLOCATION          0
#define configTOTAL_HEAP_SIZE                    ((size_t)(28 * 1024))
#define configAPPLICATION_ALLOCATED_HEAP         0

/* ---- Hooks ------------------------------------------------------------- */
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             1
#define configUSE_DAEMON_TASK_STARTUP_HOOK       0

/* ---- Run time and co-routines ------------------------------------------ */
#define configGENERATE_RUN_TIME_STATS            0
#define configUSE_CO_ROUTINES                    0
#define configMAX_CO_ROUTINE_PRIORITIES          2

/* ---- Software timers --------------------------------------------------- */
#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             256

/* ---- Optional API ------------------------------------------------------ */
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskCleanUpResources            0
#define INCLUDE_vTaskSuspend                     1
/* INCLUDE_vTaskDelayUntil is the legacy spelling; this kernel rejects having
   both defined at once (FreeRTOS.h line 230). */
#define INCLUDE_xTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
/* osThreadResume() in cmsis_os2.c calls xTaskAbortDelay() unconditionally. */
#define INCLUDE_xTaskAbortDelay                  1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_xTaskGetIdleTaskHandle           1
#define INCLUDE_eTaskGetState                    1
#define INCLUDE_xTimerPendFunctionCall           1
#define INCLUDE_xSemaphoreGetMutexHolder         1
#define INCLUDE_xQueueGetMutexHolder             1

/* ---- Cortex-M7 interrupt priorities ------------------------------------ */
#ifdef __NVIC_PRIO_BITS
#define configPRIO_BITS                          __NVIC_PRIO_BITS
#else
#define configPRIO_BITS                          4
#endif

/* Lowest possible priority, used for PendSV and SysTick. */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY       15

/* Any ISR at or below this number (numerically) must NOT call the FreeRTOS
 * API. Any ISR that does call it must be at this number or higher. */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY   5

#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#define configASSERT(x) \
    if ((x) == 0) { taskDISABLE_INTERRUPTS(); for (;;) {} }

/* ---- Handler names ----------------------------------------------------- *
 * The port supplies these two under the CMSIS vector-table names, so
 * stm32h7xx_it.c must not define them.
 *
 * xPortSysTickHandler is deliberately NOT mapped onto SysTick_Handler:
 * cmsis_os2.c already defines SysTick_Handler and calls xPortSysTickHandler()
 * from it (guarded by xTaskGetSchedulerState()). Mapping it here as well gives
 * two definitions of SysTick_Handler and the link fails.
 *
 * SysTick therefore still belongs to the kernel, which is why HAL_GetTick() is
 * moved to TIM6 (README.md section 5.B rule 3).
 */
#define vPortSVCHandler                          SVC_Handler
#define xPortPendSVHandler                       PendSV_Handler

#endif /* FREERTOS_CONFIG_H */
