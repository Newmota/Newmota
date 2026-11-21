/*
  FreeRTOSConfig.h - FreeRTOS configuration for Newmota on Bouffalo BL602

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <stdint.h>

#define configSUPPORT_STATIC_ALLOCATION 1
#define configUSE_PREEMPTION 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configCPU_CLOCK_HZ ((uint32_t)(1000 * 1000))
#define configTICK_RATE_HZ ((TickType_t)1000)
#define configMAX_PRIORITIES 32
#define configMINIMAL_STACK_SIZE ((unsigned short)128)
#define configTOTAL_HEAP_SIZE ((size_t)(100 * 1024))
#define configMAX_TASK_NAME_LEN 16
#define configUSE_TRACE_FACILITY 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define configUSE_STATS_FORMATTING_FUNCTIONS 1
#define configUSE_16_BIT_TICKS 0
#define configIDLE_SHOULD_YIELD 0
#define configUSE_MUTEXES 1
#define configQUEUE_REGISTRY_SIZE 8
#define configCHECK_FOR_STACK_OVERFLOW 0
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_MALLOC_FAILED_HOOK 0
#define configUSE_APPLICATION_TASK_TAG 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configGENERATE_RUN_TIME_STATS 0
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE 0
#define configUSE_POSIX_ERRNO 1

#define configUSE_CO_ROUTINES 0
#define configMAX_CO_ROUTINE_PRIORITIES 2

#define configUSE_TIMERS 1
#define configTIMER_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH 4
#define configTIMER_TASK_STACK_DEPTH 1024

#define INCLUDE_vTaskPrioritySet 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskCleanUpResources 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_vTaskDelayUntil 1
#define INCLUDE_vTaskDelay 1
#define INCLUDE_eTaskGetState 1
#define INCLUDE_xTimerPendFunctionCall 1
#define INCLUDE_xTaskAbortDelay 1
#define INCLUDE_xTaskGetHandle 1
#define INCLUDE_xSemaphoreGetMutexHolder 1

#define configASSERT(expression) ((void)(expression))

#define config_ENABLE_OS_TLS_SWITCH
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 1
#define configTHREAD_LOCAL_STORAGE_DELETE_CALLBACKS 1

#ifdef __cplusplus
extern "C" {
#endif
/** @brief Stops the runtime when the native BLE host asserts. */
void vAssertCalled(void);
#ifdef __cplusplus
}
#endif
