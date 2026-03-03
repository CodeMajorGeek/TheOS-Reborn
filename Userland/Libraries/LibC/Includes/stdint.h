#ifndef _STDINT_H
#define _STDINT_H

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long long int64_t;
typedef unsigned long long uint64_t;

typedef int8_t int_least8_t;
typedef uint8_t uint_least8_t;
typedef int16_t int_least16_t;
typedef uint16_t uint_least16_t;
typedef int32_t int_least32_t;
typedef uint32_t uint_least32_t;
typedef int64_t int_least64_t;
typedef uint64_t uint_least64_t;

typedef int8_t int_fast8_t;
typedef uint8_t uint_fast8_t;
typedef int16_t int_fast16_t;
typedef uint16_t uint_fast16_t;
typedef int32_t int_fast32_t;
typedef uint32_t uint_fast32_t;
typedef int64_t int_fast64_t;
typedef uint64_t uint_fast64_t;

typedef signed long intptr_t;
typedef unsigned long uintptr_t;
typedef int64_t intmax_t;
typedef uint64_t uintmax_t;

#define INT8_MAX        0x7f
#define INT8_MIN        (-INT8_MAX - 1)
#define UINT8_MAX       0xffu

#define INT16_MAX       0x7fff
#define INT16_MIN       (-INT16_MAX - 1)
#define UINT16_MAX      0xffffu

#define INT32_MAX       0x7fffffff
#define INT32_MIN       (-INT32_MAX - 1)
#define UINT32_MAX      0xffffffffu

#define INT64_MAX       0x7fffffffffffffffll
#define INT64_MIN       (-INT64_MAX - 1ll)
#define UINT64_MAX      0xffffffffffffffffull

#define INTPTR_MAX      __INTPTR_MAX__
#define INTPTR_MIN      (-INTPTR_MAX - 1L)
#define UINTPTR_MAX     __UINTPTR_MAX__
// MicroPython expects INTPTR_UMAX in mpconfig.h.
#define INTPTR_UMAX     UINTPTR_MAX

#define INTMAX_MAX      INT64_MAX
#define INTMAX_MIN      INT64_MIN
#define UINTMAX_MAX     UINT64_MAX

#ifndef SIZE_MAX
#define SIZE_MAX        __SIZE_MAX__
#endif

#endif
