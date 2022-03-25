#include <string.h>

void* memset(void* ptr, int value, size_t count)
{
    uint8_t* buf = (uint8_t*) ptr;
    for (size_t i = 0; i < count; ++i)
        buf[i] = (uint8_t) value;

    return ptr;
}

void* memsetw(void* ptr, int value, size_t count)
{
    uint16_t* buf = (uint16_t*) ptr;
    for (size_t i = 0; i < count; ++i)
        buf[i] = (uint16_t) value;

    return ptr;
}

void* memsetq(void* ptr, int value, size_t count)
{
    uint64_t* buf = (uint64_t*) ptr;
    for (size_t i = 0; i < count; ++i)
        buf[i] = (uint64_t) value;

    return ptr;
}

size_t strlen(const char* str)
{
    size_t len = 0;
    while (str[len] != '\0')
        len++;

    return len;
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