#include <ctype.h>
#include <string.h>
#include <strings.h>

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

void* memcpy(void* dest, const void* src, size_t count)
{
    uint8_t* d = dest;
    const uint8_t* s = src;
    while (count--)
        *d++ = *s++;

    return dest;
}

void* memmove(void* dest, const void* src, size_t count)
{
    uint8_t* d = (uint8_t*) dest;
    const uint8_t* s = (const uint8_t*) src;

    if (d == s || count == 0)
        return dest;

    if (d < s || d >= (s + count))
    {
        while (count--)
            *d++ = *s++;
    }
    else
    {
        d += count;
        s += count;
        while (count--)
            *--d = *--s;
    }

    return dest;
}

void* memcpyw(void* dest, const void* src, size_t count)
{
    uint16_t* d = dest;
    const uint16_t* s = src;
    while (count--)
        *d++ = *s++;

    return dest;
}

void* memcpyq(void* dest, const void* src, size_t count)
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

char* strcpy(char* __restrict dest, const char* __restrict src)
{
    size_t i = 0;
    while (src[i] != '\0')
    {
        dest[i] = src[i];
        i++;
    }

    dest[i] = '\0';
    return dest;
}

char* strncpy(char* __restrict dest, const char* __restrict src, size_t length)
{
    size_t i = 0;

    while (i < length && src[i] != '\0')
    {
        dest[i] = src[i];
        i++;
    }

    while (i < length)
    {
        dest[i] = '\0';
        i++;
    }

    return dest;
}

char* strcat(char* __restrict dest, const char* __restrict src)
{
    size_t dst_len = strlen(dest);
    size_t i = 0;

    while (src[i] != '\0')
    {
        dest[dst_len + i] = src[i];
        i++;
    }

    dest[dst_len + i] = '\0';
    return dest;
}

int strncmp(const char* first, const char* second, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        unsigned char a = (unsigned char) first[i];
        unsigned char b = (unsigned char) second[i];

        if (a != b)
            return (a < b) ? -1 : 1;

        if (a == '\0')
            return 0;
    }

    return 0;
}

int strcmp(const char* first, const char* second)
{
    while (*first != '\0' && *second != '\0')
    {
        if (*first != *second)
            return ((unsigned char) *first < (unsigned char) *second) ? -1 : 1;

        first++;
        second++;
    }

    if (*first == *second)
        return 0;

    return ((unsigned char) *first < (unsigned char) *second) ? -1 : 1;
}

char* strchr(const char* str, int ch)
{
    if (!str)
        return NULL;

    char c = (char) ch;
    while (*str)
    {
        if (*str == c)
            return (char*) str;
        str++;
    }

    return (c == '\0') ? (char*) str : NULL;
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

int tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');

    return c;
}

int toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');

    return c;
}

int isspace(int c)
{
    return c == ' ' || c == '\f' || c == '\n' ||
           c == '\r' || c == '\t' || c == '\v';
}

int strcasecmp(const char* first, const char* second)
{
    if (!first || !second)
        return (first == second) ? 0 : (first ? 1 : -1);

    while (*first != '\0' && *second != '\0')
    {
        unsigned char a = (unsigned char) tolower((unsigned char) *first);
        unsigned char b = (unsigned char) tolower((unsigned char) *second);
        if (a != b)
            return (a < b) ? -1 : 1;

        first++;
        second++;
    }

    unsigned char a = (unsigned char) tolower((unsigned char) *first);
    unsigned char b = (unsigned char) tolower((unsigned char) *second);
    if (a == b)
        return 0;

    return (a < b) ? -1 : 1;
}

int strncasecmp(const char* first, const char* second, size_t length)
{
    if (length == 0U)
        return 0;
    if (!first || !second)
        return (first == second) ? 0 : (first ? 1 : -1);

    for (size_t i = 0; i < length; i++)
    {
        unsigned char a = (unsigned char) tolower((unsigned char) first[i]);
        unsigned char b = (unsigned char) tolower((unsigned char) second[i]);
        if (a != b)
            return (a < b) ? -1 : 1;
        if (first[i] == '\0')
            return 0;
    }

    return 0;
}
