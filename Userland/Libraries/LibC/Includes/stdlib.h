#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

long long strtoll(const char* nptr, char** endptr, int base);
unsigned long long strtoull(const char* nptr, char** endptr, int base);
double strtod(const char* nptr, char** endptr);
float strtof(const char* nptr, char** endptr);
int atoi(const char* nptr);
long atol(const char* nptr);
long long atoll(const char* nptr);
int abs(int value);

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
int posix_memalign(void** memptr, size_t alignment, size_t size);
void* aligned_alloc(size_t alignment, size_t size);
int atexit(void (*function)(void));
__attribute__((__noreturn__)) void exit(int status);

__attribute__((__noreturn__)) void panic(char* s);
__attribute__((__noreturn__)) void abort(void);

#endif
