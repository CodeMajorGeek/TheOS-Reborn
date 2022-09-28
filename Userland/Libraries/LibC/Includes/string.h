#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>
#include <stdint.h>

void* memset(void* ptr, uint8_t value, size_t count);
void* memsetw(void* ptr, uint16_t value, size_t count);
void* memsetq(void* ptr, uint64_t value, size_t count);

void* memcpy(void* dest, void* src, size_t count);
void* memcpyw(void* dest, void* src, size_t count);
void* memcpyq(void* dest, void* src, size_t count);

size_t strlen(const char* str);

char* strcpy(char* __restrict, const char* __restrict);

int strncmp(const char* first, const char* second, size_t length);
int strcmp(const char* first, const char* second);

int memcmp(const void* aptr, const void* bptr, size_t size);

#endif