#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF         (-1)

#define BINARY      2
#define DECIMAL     10
#define HEXADECIMAL 16

int putc(int c);
int puts(const char* s);

int __printf(char*, size_t, const char* __restrict, va_list);
int printf(const char* __restrict, ...);
int sprintf(char*, const char* __restrict, ...);
int snprintf(char*, size_t, const char* __restrict, ...);

char* itoa(int, char*, size_t, unsigned int);
char* lltoa(unsigned long long, char*, size_t, unsigned int);

#endif