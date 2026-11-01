#pragma once
// Lets CaDiCaL (written for GCC/Clang on POSIX) build with MSVC unchanged.
#ifdef _MSC_VER
#include <intrin.h>
#define __attribute__(x)
static inline int __builtin_clz(unsigned x) { unsigned long i; _BitScanReverse(&i, x); return 31 - (int)i; }
static inline int __builtin_ctz(unsigned x) { unsigned long i; _BitScanForward(&i, x); return (int)i; }
static inline int __builtin_clzll(unsigned long long x) { unsigned long i; _BitScanReverse64(&i, x); return 63 - (int)i; }
static inline int __builtin_ctzll(unsigned long long x) { unsigned long i; _BitScanForward64(&i, x); return (int)i; }
static inline int __builtin_popcount(unsigned x) { return (int)__popcnt(x); }
static inline int __builtin_popcountll(unsigned long long x) { return (int)__popcnt64(x); }
#include <stdio.h>
#include <sys/stat.h>
#include <xmmintrin.h>
// CaDiCaL turns release-mode asserts into unreachable hints; some do not hold
// in practice and MSVC then miscompiles, so the hint does nothing here.
#define __builtin_unreachable() ((void)0)
#define CADICAL_PREFETCH_FIRST(p, ...) p
#define __builtin_prefetch(...) _mm_prefetch((const char *)(CADICAL_PREFETCH_FIRST(__VA_ARGS__, 0)), _MM_HINT_T0)
#define __PRETTY_FUNCTION__ __FUNCSIG__
#define getc_unlocked _getc_nolock
#define putc_unlocked _putc_nolock
#define popen _popen
#define pclose _pclose
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m) (((m) & _S_IFMT) == _S_IFIFO)
#endif
#endif
