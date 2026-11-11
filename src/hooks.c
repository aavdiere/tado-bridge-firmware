/* FreeRTOS application hooks: log over RTT, halt, let the watchdog reset. */
#include "log.h"

#include "FreeRTOS.h"
#include "task.h"

void vApplicationMallocFailedHook(void) {
    fw_panic("FreeRTOS heap exhausted (configTOTAL_HEAP_SIZE)", __FILE__, __LINE__);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    fw_panic(name ? name : "?", "stack overflow in task", __LINE__);
}
