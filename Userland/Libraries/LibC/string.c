#include <string.h>

void* memset(void* ptr, uint8_t value, size_t count)
{
    uint8_t* buf = (uint8_t*) ptr;
    for (size_t i = 0; i < count; ++i)
        buf[i] = (uint8_t) value;

    return ptr;
}

void* memsetw(void* ptr, uint16_t value, size_t count)
{
    uint16_t* buf = (uint16_t*) ptr;
    for (size_t i = 0; i < count; ++i)
        buf[i] = (uint16_t) value;

    return ptr;
}

void* memsetq(void* ptr, uint64_t value, size_t count)
{
    uint64_t* buf = (uint64_t*) ptr;
    for (size_t i = 0; i < count; ++i)
        buf[i] = value;

    return ptr;
}

void* memcpy(void* dest, void* src, size_t count)
{
    uint8_t* d = dest;
    const uint8_t* s = src;
    while (count--)
        *d++ = *s++;

    return dest;
}

void* memcpyw(void* dest, void* src, size_t count)
{
    uint16_t* d = dest;
    const uint16_t* s = src;
    while (count--)
        *d++ = *s++;

    return dest;
}

void* memcpyq(void* dest, void* src, size_t count)
{
    uint64_t* d = dest;
    const uint64_t* s = src;
    while (count--)
        *d++ = *s++;

    return dest;
}

size_t strlen(const char* str)
{
    size_t len = 0;
    while (str[len] != '\0')
        len++;

    return len;
}

char* strcpy(char* __restrict dest, const char * __restrict src)
{
    return memcpy(dest, src, strlen(src));
}

int memcmp(const void* aptr, const void* bptr, size_t size)
{
    const unsigned char* a = (const unsigned char*) aptr;
    const unsigned char* b = (const unsigned char*) bptr;

    for (size_t i = 0; i < size; ++i)
    {
        if (a[i] < b[i])
            return -1;
        else if (a[i] > b[i])
            return 1;
    }

    return 0;
}