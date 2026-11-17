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

// ------------------------------------------------------------------------------------------------ 16-bit wide strings
#include <stdint.h>
typedef uint16_t wc16;

wc16 *port_wcschr(const wc16 *s, wc16 c) {
    for (;; s++) {
        if (*s == c) return (wc16 *) s;
        if (!*s) return NULL;
    }
}
size_t port_wcslen(const wc16 *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
wc16 *port_wcscpy(wc16 *d, const wc16 *s) {
    wc16 *r = d;
    while ((*d++ = *s++)) {
    }
    return r;
}
wc16 *port_wcsncpy(wc16 *d, const wc16 *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
wc16 *port_wcsncat(wc16 *d, const wc16 *s, size_t n) {
    wc16 *e = d + port_wcslen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) e[i] = s[i];
    e[i] = 0;
    return d;
}
int port_wcsncmp(const wc16 *a, const wc16 *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        if (!a[i]) return 0;
    }
    return 0;
}
wc16 *port_wmemchr(const wc16 *s, wc16 c, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] == c) return (wc16 *) &s[i];
    return NULL;
}
wc16 *port_wmemcpy(wc16 *d, const wc16 *s, size_t n) {
    memmove(d, s, n * 2);
    return d;
}

// Formats with the narrow printf: the formats the game uses are ASCII (numbers, %s of narrow strings is not used).
int port_vswprintf(wc16 *buf, size_t n, const wc16 *fmt, va_list ap) {
    char nfmt[256], out[512];
    size_t i = 0;
    for (; fmt[i] && i < sizeof(nfmt) - 1; i++) nfmt[i] = (char) fmt[i];
    nfmt[i] = 0;
    int len = vsnprintf(out, sizeof(out), nfmt, ap);
    if (len < 0) return -1;
    if (n == 0) return -1;
    size_t k = 0;
    for (; k < n - 1 && out[k]; k++) buf[k] = (unsigned char) out[k];
    buf[k] = 0;
    return (size_t) len >= n ? -1 : len; // C99: -1 when it does not fit
}
int port_swprintf(wc16 *buf, size_t n, const wc16 *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = port_vswprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
size_t port_mbstowcs(wc16 *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = (unsigned char) s[i];
    if (i < n) d[i] = 0;
    return i;
}
size_t port_wcstombs(char *d, const wc16 *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = (char) (s[i] < 0x80 ? s[i] : '?');
    if (i < n) d[i] = 0;
    return i;
}
int port_wctomb(char *d, wc16 c) {
    if (!d) return 0;
    *d = (char) (c < 0x80 ? c : '?');
    return 1;
}

// ------------------------------------------------------------------------------------------------ SDK printf
// OS_SPrintf / OS_SNPrintf take the address of their variable arguments with va_start (the decomp computes it from
// the last fixed argument, which only works with CodeWarrior's calling convention). The va_list of both ARM (a
// struct holding the pointer) and x86 (the pointer) starts with that address.
int OS_VSNPrintf(char *dst, size_t len, const char *fmt, char *args);

int OS_SPrintf(char *dst, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = OS_VSNPrintf(dst, 0x7fffffff, fmt, *(char **) &ap);
    va_end(ap);
    return r;
}

int OS_SNPrintf(char *dst, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = OS_VSNPrintf(dst, len, fmt, *(char **) &ap);
    va_end(ap);
    return r;
}
