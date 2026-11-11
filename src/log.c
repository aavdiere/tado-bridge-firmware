#include "log.h"

#include "rtt.h"

#include <stdarg.h>
#include <stdio.h>

void log_init(void) {
    rtt_init();
}

void log_printf(const char *fmt, ...) {
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 1) {
        n = sizeof line - 1;
    }
    if (n > 0) {
        rtt_write(line, (size_t)n);
    }
}
