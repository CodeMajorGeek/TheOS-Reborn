#ifndef _STDIO_H
#define _STDIO_H

#include <Device/TTY.h>

#include <stdint.h>
#include <stdarg.h>
#include <limits.h>

#define EOF         (-1)

#define BINARY      2
#define DECIMAL     10
#define HEXADECIMAL 16

int putchar(int c);

int puts(const char* s);

int printf(const char* __restrict, ...);
char* itoa(int, char*, size_t, unsigned int);

#endif