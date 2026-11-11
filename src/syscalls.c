/* Newlib-nano stub: heap grows from the end of .bss up to the stack limit.
 * (The toolchain's libnosys _sbrk does not link on this setup.) The firmware
 * does not malloc; this serves printf/vsnprintf internals only. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

extern char end;         /* from libopencm3 cortex-m-generic.ld */
extern char _heap_limit; /* cmake/stm32f411ce.ld: top of RAM minus the main stack */
#define HEAP_LIMIT (&_heap_limit)

__attribute__((used, externally_visible)) void *_sbrk(ptrdiff_t incr) {
    static char *brk;
    if (!brk) {
        brk = &end;
    }
    if (brk + incr > HEAP_LIMIT) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *prev = brk;
    brk += incr;
    return prev;
}
