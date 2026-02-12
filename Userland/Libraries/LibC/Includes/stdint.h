#ifndef _SDTINT_H
#define _SDTINT_H

#define null    0
#define NULL    null

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

typedef unsigned int uint8_t __attribute__((__mode__(__QI__)));
typedef unsigned int uint16_t __attribute__((__mode__(__HI__)));

typedef uint64_t uintptr_t;

typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long long int64_t;

typedef signed int int8_t __attribute__((__mode__(__QI__)));
typedef signed int int16_t __attribute__((__mode__(__HI__)));

#endif