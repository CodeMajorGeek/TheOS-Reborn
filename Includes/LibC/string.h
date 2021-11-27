#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>
#include <stdint.h>

void* memset(void* ptr, int value, size_t count);
void* memsetw(void* ptr, int value, size_t count);

size_t strlen(const char* str);

int memcmp(const void* aptr, const void* bptr, size_t size);

#endif