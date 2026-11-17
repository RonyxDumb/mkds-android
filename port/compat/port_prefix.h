/* Force-included in every decomp translation unit of the native port (clang).
 * Maps CodeWarrior intrinsics and extensions onto clang equivalents. */
#pragma once
#define PLATFORM_PORT 1


/* mwcc intrinsics */
#define __memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define __builtin__clear(p, n) __builtin_memset((p), 0, (n))
#define __clear(p, n) __builtin_memset((p), 0, (n))

/* The game formats strings into the buffer it reads from (snprintf(buf, n, "%s...", buf, ...)), which CodeWarrior's
 * C library allows: these versions format into a temporary buffer first (port/platform/libc.c). */
#define sprintf port_sprintf
#define snprintf port_snprintf
#define vsprintf port_vsprintf
#define vsnprintf port_vsnprintf
