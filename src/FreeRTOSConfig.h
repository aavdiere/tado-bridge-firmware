#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* FreeRTOS V11.1.0, port GCC/ARM_CM4F, heap_4 (#8). Kernel in
 * vendor/FreeRTOS-Kernel. Only the options that differ from the defaults. */

#ifndef __ASSEMBLER__
#include <stdint.h>
extern uint32_t clock_hz; /* src/clock.c: 96 MHz after clock_init() */
void            fw_panic(const char *what, const char *file, int line);
#endif

#define configCPU_CLOCK_HZ (clock_hz) /* read when the scheduler starts, after clock_init() */
#define configTICK_RATE_HZ ((TickType_t)1000)

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_16_BIT_TICKS                  0
#define configMAX_PRIORITIES                    5
#define configMINIMAL_STACK_SIZE                ((uint16_t)128) /* words */
#define configMAX_TASK_NAME_LEN                 12
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_TRACE_FACILITY                0

#define configUSE_IDLE_HOOK            0
#define configUSE_TICK_HOOK            0
#define configUSE_MALLOC_FAILED_HOOK   1
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_TIMERS               0
#define configUSE_CO_ROUTINES          0

/* Memory: heap_4 over a fixed array in .bss. PROVISIONAL: 32 KB is a guess for
 * #17 (RAM budget); TLS alone is estimated at 30 KB peak, so expect to revisit
 * it in P4. Nothing may use newlib malloc (see src/syscalls.c). */
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configSUPPORT_STATIC_ALLOCATION  0
#define configTOTAL_HEAP_SIZE            ((size_t)(32 * 1024))
#define configAPPLICATION_ALLOCATED_HEAP 0
#define configUSE_NEWLIB_REENTRANT       0 /* log.c serialises printf with a mutex; no errno per task */

#define configENABLE_FPU 1
#define configENABLE_MPU 0

/* Cortex-M4 on STM32F4: 4 priority bits. Interrupts that call FromISR APIs
 * (GDO0/GDO2 in P2) must use a numeric priority >= 5 (0x50 and less urgent).
 * The kernel (PendSV, SysTick) runs at the lowest priority. */
#define configPRIO_BITS                       4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* A failed assert logs over RTT and halts with interrupts off; the watchdog
 * then resets the board. */
#define configASSERT(x) \
    do { if (!(x)) fw_panic("configASSERT", __FILE__, __LINE__); } while (0)

#define INCLUDE_vTaskDelay          1
#define INCLUDE_vTaskDelayUntil     1
#define INCLUDE_vTaskDelete         0
#define INCLUDE_vTaskSuspend        1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1

/* Kernel handlers under the libopencm3 vector table names. */
#define vPortSVCHandler     sv_call_handler
#define xPortPendSVHandler  pend_sv_handler
#define xPortSysTickHandler sys_tick_handler

#endif
