#ifndef _ALLOCA_H
#define _ALLOCA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * alloca() is a compiler/runtime extension (not ISO C).
 * On this toolchain we intentionally bind it to the compiler builtin
 * so allocation remains stack-based and auto-freed on return.
 */
#if defined(__GNUC__) || defined(__clang__)
#define alloca(size) __builtin_alloca(size)
#else
void* alloca(size_t size);
#endif

#ifdef __cplusplus
}
#endif

#endif
