/* FreeRTOS application hooks: log over RTT, halt, let the watchdog reset. */
#include "log.h"

#include "FreeRTOS.h"
#include "task.h"

void vApplicationMallocFailedHook(void) {
    fw_panic("FreeRTOS heap exhausted (configTOTAL_HEAP_SIZE)", __FILE__, __LINE__);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    fw_panic("stack overflow in task", name ? name : "?", __LINE__);
}

/* With configSUPPORT_STATIC_ALLOCATION (#24, the update task) the kernel
 * creates the idle task in memory the application gives it: the same
 * configMINIMAL_STACK_SIZE as before, now in .bss instead of the heap. */
void vApplicationGetIdleTaskMemory(StaticTask_t          **tcb,
                                   StackType_t           **stack,
                                   configSTACK_DEPTH_TYPE *words) {
    static StaticTask_t idle_tcb;
    static StackType_t  idle_stack[configMINIMAL_STACK_SIZE];
    *tcb   = &idle_tcb;
    *stack = idle_stack;
    *words = configMINIMAL_STACK_SIZE;
}
