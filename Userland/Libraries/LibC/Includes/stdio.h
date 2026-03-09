#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF         (-1)

#define BINARY      2
#define DECIMAL     10
#define HEXADECIMAL 16

#define STDIO_KEY_UP       0x80
#define STDIO_KEY_DOWN     0x81
#define STDIO_KEY_LEFT     0x82
#define STDIO_KEY_RIGHT    0x83
#define STDIO_KEY_DELETE   0x84

typedef struct FILE
{
    int fd;
} FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

int putc(int c);
int puts(const char* s);
int getchar(void);
char* fgets(char* str, int size);
int keyboard_load_config(const char* config_path);

int __printf(char*, size_t, const char* __restrict, va_list);
int printf(const char* __restrict, ...);
int vfprintf(FILE* stream, const char* __restrict format, va_list ap);
int fprintf(FILE* stream, const char* __restrict format, ...);
int sprintf(char*, const char* __restrict, ...);
int snprintf(char*, size_t, const char* __restrict, ...);

char* itoa(int, char*, size_t, unsigned int);
char* lltoa(unsigned long long, char*, size_t, unsigned int);

#endif
