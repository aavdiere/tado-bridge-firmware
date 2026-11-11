/* Logging fault handlers (replace libopencm3's silent blocking_handler).
 * The naked stub picks MSP or PSP from EXC_RETURN bit 2 and passes the stacked
 * frame to fw_fault_report(), which logs and spins until the IWDG resets. */
#include "faults.h"

#include "log.h"

#include <libopencm3/cm3/scb.h>

#define FAULT_STUB(handler, label)                                                        \
    __attribute__((naked)) void handler(void) {                                           \
        __asm volatile("tst lr, #4\n"                                                     \
                       "ite eq\n"                                                         \
                       "mrseq r1, msp\n"                                                  \
                       "mrsne r1, psp\n"                                                  \
                       "adr r0, 1f\n"                                                     \
                       "b fw_fault_report\n"                                              \
                       ".align 2\n"                                                       \
                       "1: .asciz \"" label "\"\n");                                      \
    }

FAULT_STUB(hard_fault_handler, "HardFault")
FAULT_STUB(mem_manage_handler, "MemManage")
FAULT_STUB(bus_fault_handler, "BusFault")
FAULT_STUB(usage_fault_handler, "UsageFault")

void faults_init(void) {
    SCB_SHCSR |= SCB_SHCSR_MEMFAULTENA | SCB_SHCSR_BUSFAULTENA | SCB_SHCSR_USGFAULTENA;
}
