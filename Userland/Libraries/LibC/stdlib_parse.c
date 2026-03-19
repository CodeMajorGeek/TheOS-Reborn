#include <stdlib.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <unistd.h>

#define LIBC_ATEXIT_MAX_HANDLERS 64U

static void (*LibC_atexit_handlers[LIBC_ATEXIT_MAX_HANDLERS])(void);
static size_t LibC_atexit_count = 0U;
static volatile unsigned char LibC_atexit_lock = 0U;

static void stdlib_atexit_lock(void)
{
    while (__atomic_test_and_set(&LibC_atexit_lock, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&LibC_atexit_lock, __ATOMIC_RELAXED) != 0U)
            __asm__ __volatile__("pause");
    }
}

static void stdlib_atexit_unlock(void)
{
    __atomic_clear(&LibC_atexit_lock, __ATOMIC_RELEASE);
}

static const char* stdlib_skip_space(const char* p)
{
    while (p && isspace((unsigned char) *p))
        p++;
    return p;
}

static int stdlib_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'Z')
        return 10 + (c - 'A');
    return -1;
}

static const char* stdlib_parse_unsigned(const char* p,
                                         int base,
                                         unsigned long long* out_value,
                                         int* out_overflow,
                                         int* out_any)
{
    unsigned long long value = 0;
    int overflow = 0;
    int any = 0;

    while (p && *p)
    {
        int digit = stdlib_digit_value(*p);
        if (digit < 0 || digit >= base)
            break;

        any = 1;
        if (value > (ULLONG_MAX - (unsigned long long) digit) / (unsigned long long) base)
        {
            overflow = 1;
            value = ULLONG_MAX;
        }
        else
        {
            value = value * (unsigned long long) base + (unsigned long long) digit;
        }
        p++;
    }

    if (out_value)
        *out_value = value;
    if (out_overflow)
        *out_overflow = overflow;
    if (out_any)
        *out_any = any;
    return p;
}

static const char* stdlib_normalize_base(const char* p, int* base)
{
    if (*base == 0)
    {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        {
            *base = 16;
            return p + 2;
        }
        if (p[0] == '0')
            *base = 8;
        else
            *base = 10;
        return p;
    }

    if (*base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        return p + 2;

    return p;
}

unsigned long long strtoull(const char* nptr, char** endptr, int base)
{
    const char* p = nptr;
    int neg = 0;

    if (!p || (base != 0 && (base < 2 || base > 36)))
    {
        if (endptr)
            *endptr = (char*) nptr;
        return 0;
    }

    p = stdlib_skip_space(p);
    if (*p == '+' || *p == '-')
    {
        neg = (*p == '-');
        p++;
    }

    p = stdlib_normalize_base(p, &base);

    unsigned long long value = 0;
    int overflow = 0;
    int any = 0;
    const char* end = stdlib_parse_unsigned(p, base, &value, &overflow, &any);
    if (!any)
    {
        if (endptr)
            *endptr = (char*) nptr;
        return 0;
    }

    if (endptr)
        *endptr = (char*) end;

    if (overflow)
    {
        errno = ERANGE;
        return ULLONG_MAX;
    }

    if (neg)
        return 0ULL - value;
    return value;
}

long long strtoll(const char* nptr, char** endptr, int base)
{
    const char* p = nptr;
    int neg = 0;

    if (!p || (base != 0 && (base < 2 || base > 36)))
    {
        if (endptr)
            *endptr = (char*) nptr;
        return 0;
    }

    p = stdlib_skip_space(p);
    if (*p == '+' || *p == '-')
    {
        neg = (*p == '-');
        p++;
    }

    p = stdlib_normalize_base(p, &base);

    unsigned long long value = 0;
    int overflow = 0;
    int any = 0;
    const char* end = stdlib_parse_unsigned(p, base, &value, &overflow, &any);
    if (!any)
    {
        if (endptr)
            *endptr = (char*) nptr;
        return 0;
    }

    if (endptr)
        *endptr = (char*) end;

    if (overflow)
    {
        errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }

    unsigned long long abs_min = (unsigned long long) LLONG_MAX + 1ULL;
    if (neg)
    {
        if (value > abs_min)
        {
            errno = ERANGE;
            return LLONG_MIN;
        }
        if (value == abs_min)
            return LLONG_MIN;
        return -(long long) value;
    }

    if (value > (unsigned long long) LLONG_MAX)
    {
        errno = ERANGE;
        return LLONG_MAX;
    }

    return (long long) value;
}

double strtod(const char* nptr, char** endptr)
{
    const char* p = nptr;
    int neg = 0;
    int any = 0;
    double value = 0.0;
    double frac_scale = 0.1;

    if (!p)
    {
        if (endptr)
            *endptr = (char*) nptr;
        return 0.0;
    }

    p = stdlib_skip_space(p);
    if (*p == '+' || *p == '-')
    {
        neg = (*p == '-');
        p++;
    }

    while (*p >= '0' && *p <= '9')
    {
        any = 1;
        value = (value * 10.0) + (double) (*p - '0');
        p++;
    }

    if (*p == '.')
    {
        p++;
        while (*p >= '0' && *p <= '9')
        {
            any = 1;
            value += (double) (*p - '0') * frac_scale;
            frac_scale *= 0.1;
            p++;
        }
    }

    if (!any)
    {
        if (endptr)
            *endptr = (char*) nptr;
        return 0.0;
    }

    if (*p == 'e' || *p == 'E')
    {
        const char* exp_start = p;
        p++;
        int exp_neg = 0;
        if (*p == '+' || *p == '-')
        {
            exp_neg = (*p == '-');
            p++;
        }

        int exp_any = 0;
        int exp_value = 0;
        while (*p >= '0' && *p <= '9')
        {
            exp_any = 1;
            if (exp_value < 10000)
                exp_value = exp_value * 10 + (int) (*p - '0');
            p++;
        }

        if (exp_any)
        {
            while (exp_value-- > 0)
            {
                if (exp_neg)
                    value /= 10.0;
                else
                    value *= 10.0;
            }
        }
        else
        {
            p = exp_start;
        }
    }

    if (value > __DBL_MAX__)
    {
        errno = ERANGE;
        value = __DBL_MAX__;
    }

    if (endptr)
        *endptr = (char*) p;

    return neg ? -value : value;
}

float strtof(const char* nptr, char** endptr)
{
    double value = strtod(nptr, endptr);
    if (value > (double) __FLT_MAX__)
    {
        errno = ERANGE;
        return __FLT_MAX__;
    }
    if (value < (double) -__FLT_MAX__)
    {
        errno = ERANGE;
        return -__FLT_MAX__;
    }
    return (float) value;
}

int atoi(const char* nptr)
{
    char* endptr = NULL;
    long long value = strtoll(nptr, &endptr, 10);
    if (endptr == nptr)
        return 0;
    if (value > (long long) INT_MAX)
        return INT_MAX;
    if (value < (long long) INT_MIN)
        return INT_MIN;
    return (int) value;
}

long atol(const char* nptr)
{
    char* endptr = NULL;
    long long value = strtoll(nptr, &endptr, 10);
    if (endptr == nptr)
        return 0L;
    if (value > (long long) LONG_MAX)
        return LONG_MAX;
    if (value < (long long) LONG_MIN)
        return LONG_MIN;
    return (long) value;
}

long long atoll(const char* nptr)
{
    char* endptr = NULL;
    long long value = strtoll(nptr, &endptr, 10);
    if (endptr == nptr)
        return 0LL;
    return value;
}

int abs(int value)
{
    return (value < 0) ? -value : value;
}

int atexit(void (*function)(void))
{
    if (!function)
    {
        errno = EINVAL;
        return -1;
    }

    stdlib_atexit_lock();
    if (LibC_atexit_count >= LIBC_ATEXIT_MAX_HANDLERS)
    {
        stdlib_atexit_unlock();
        errno = ENOMEM;
        return -1;
    }

    LibC_atexit_handlers[LibC_atexit_count++] = function;
    stdlib_atexit_unlock();
    return 0;
}

__attribute__((__noreturn__)) void exit(int status)
{
    for (;;)
    {
        void (*handler)(void) = NULL;

        stdlib_atexit_lock();
        if (LibC_atexit_count > 0U)
        {
            LibC_atexit_count--;
            handler = LibC_atexit_handlers[LibC_atexit_count];
            LibC_atexit_handlers[LibC_atexit_count] = NULL;
        }
        stdlib_atexit_unlock();

        if (!handler)
            break;

        handler();
    }

    _exit(status);
}
