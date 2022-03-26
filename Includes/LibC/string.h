#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>
#include <stdint.h>

void* memset(void* ptr, int value, size_t count);
void* memsetw(void* ptr, int value, size_t count);
void* memsetq(void* ptr, int value, size_t count);

void* memcpy(void* dest, void* src, size_t count);
void* memcpyw(void* dest, void* src, size_t count);
void* memcpyq(void* dest, void* src, size_t count);

size_t strlen(const char* str);

char* strcpy(char* __restrict, const char* __restrict);

int memcmp(const void* aptr, const void* bptr, size_t size);

#endif