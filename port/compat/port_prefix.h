/* Force-included in every decomp translation unit of the native port (clang).
 * Maps CodeWarrior intrinsics and extensions onto clang equivalents. */
#pragma once
#define PLATFORM_PORT 1


/* mwcc intrinsics */
#define __memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define __builtin__clear(p, n) __builtin_memset((p), 0, (n))
#define __clear(p, n) __builtin_memset((p), 0, (n))
