#include <stddef.h>
#include <stdint.h>

uint64_t thetestdyn_magic(uint64_t value)
{
    return (value ^ 0x5A5AA5A50F0FF0F0ULL) + 0x1122334455667788ULL;
}

size_t thetestdyn_len_hint(const char* text)
{
    if (!text)
        return 0U;

    size_t len = 0U;
    while (text[len] != '\0')
        len++;
    return len;
}

int thetestdyn_sum3(int a, int b, int c)
{
    return a + b + c + 7;
}
