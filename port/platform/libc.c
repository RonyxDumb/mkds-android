// C library functions the game uses with CodeWarrior's semantics (see port/compat/port_prefix.h).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int port_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    char tmp[2048];
    int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n) {
        size_t copy = (size_t) len < n - 1 ? (size_t) len : n - 1;
        if (copy > sizeof(tmp) - 1) copy = sizeof(tmp) - 1;
        memmove(buf, tmp, copy);
        buf[copy] = 0;
    }
    return len;
}

int port_vsprintf(char *buf, const char *fmt, va_list ap) { return port_vsnprintf(buf, 2048, fmt, ap); }

int port_snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = port_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int port_sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = port_vsnprintf(buf, 2048, fmt, ap);
    va_end(ap);
    return r;
}
