#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <syscall.h>
#include <time.h>

static const char* LibC_time_weekday_short[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char* LibC_time_weekday_long[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

static const char* LibC_time_month_short[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char* LibC_time_month_long[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static struct tm LibC_time_gmtime_buf;
static struct tm LibC_time_localtime_buf;
static char LibC_time_asctime_buf[32];

static int LibC_time_is_leap_year(int year)
{
    if ((year % 4) != 0)
        return 0;
    if ((year % 100) != 0)
        return 1;
    return (year % 400) == 0;
}

static int LibC_time_days_in_month(int year, int month_zero_based)
{
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month_zero_based == 1)
        return days[1] + (LibC_time_is_leap_year(year) ? 1 : 0);
    return days[month_zero_based];
}

static int LibC_time_days_in_year(int year)
{
    return LibC_time_is_leap_year(year) ? 366 : 365;
}

static int LibC_time_append_str(char* dst, size_t max, size_t* io_pos, const char* src)
{
    if (!dst || !io_pos || !src || max == 0U)
        return 0;
    size_t pos = *io_pos;
    while (*src != '\0')
    {
        if (pos + 1U >= max)
            return 0;
        dst[pos++] = *src++;
    }
    *io_pos = pos;
    return 1;
}

static int LibC_time_append_num(char* dst, size_t max, size_t* io_pos, int value, int width, int zero_pad)
{
    char fmt[8];
    if (width < 0 || width > 4)
        return 0;
    if (zero_pad)
        (void) snprintf(fmt, sizeof(fmt), "%%0%dd", width);
    else
        (void) snprintf(fmt, sizeof(fmt), "%%%dd", width);

    char buf[16];
    (void) snprintf(buf, sizeof(buf), fmt, value);
    return LibC_time_append_str(dst, max, io_pos, buf);
}

clock_t clock(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
        return (clock_t) -1;
    return (clock_t) ((tv.tv_sec * (time_t) CLOCKS_PER_SEC) + ((tv.tv_usec * CLOCKS_PER_SEC) / 1000000L));
}

double difftime(time_t time1, time_t time0)
{
    return (double) time1 - (double) time0;
}

time_t time(time_t* tloc)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
        return (time_t) -1;
    if (tloc)
        *tloc = tv.tv_sec;
    return tv.tv_sec;
}

int nanosleep(const struct timespec* req, struct timespec* rem)
{
    if (!req)
    {
        errno = EINVAL;
        return -1;
    }
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L)
    {
        errno = EINVAL;
        return -1;
    }
    if (rem)
    {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }

    uint64_t ms = (uint64_t) req->tv_sec * 1000ULL;
    ms += (uint64_t) (req->tv_nsec / 1000000L);
    if (req->tv_nsec % 1000000L)
        ms += 1ULL;
    if (ms > (uint64_t) UINT32_MAX)
        ms = (uint64_t) UINT32_MAX;

    return sys_sleep_ms((uint32_t) ms);
}

int timespec_get(struct timespec* ts, int base)
{
    if (!ts || base != TIME_UTC)
        return 0;

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
        return 0;

    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000L;
    return base;
}

struct tm* gmtime(const time_t* timer)
{
    if (!timer)
        return NULL;

    int64_t sec = (int64_t) *timer;
    int64_t days = sec / 86400;
    int64_t rem = sec % 86400;
    if (rem < 0)
    {
        rem += 86400;
        days -= 1;
    }

    int year = 1970;
    while (days < 0)
    {
        year--;
        days += LibC_time_days_in_year(year);
    }
    while (days >= LibC_time_days_in_year(year))
    {
        days -= LibC_time_days_in_year(year);
        year++;
    }

    int yday = (int) days;
    int mon = 0;
    while (mon < 12)
    {
        int dim = LibC_time_days_in_month(year, mon);
        if (days < dim)
            break;
        days -= dim;
        mon++;
    }
    int mday = (int) days + 1;

    struct tm* out = &LibC_time_gmtime_buf;
    memset(out, 0, sizeof(*out));
    out->tm_year = year - 1900;
    out->tm_mon = mon;
    out->tm_mday = mday;
    out->tm_yday = yday;
    int64_t total_days_since_epoch = sec / 86400;
    if ((sec % 86400) < 0)
        total_days_since_epoch -= 1;
    out->tm_wday = (int) ((total_days_since_epoch + 4) % 7);
    if (out->tm_wday < 0)
        out->tm_wday += 7;
    out->tm_hour = (int) (rem / 3600);
    out->tm_min = (int) ((rem % 3600) / 60);
    out->tm_sec = (int) (rem % 60);
    out->tm_isdst = 0;
    return out;
}

struct tm* localtime(const time_t* timer)
{
    struct tm* utc = gmtime(timer);
    if (!utc)
        return NULL;
    LibC_time_localtime_buf = *utc;
    return &LibC_time_localtime_buf;
}

time_t mktime(struct tm* tm)
{
    if (!tm)
        return (time_t) -1;

    int year = tm->tm_year + 1900;
    int month = tm->tm_mon;
    while (month < 0)
    {
        month += 12;
        year -= 1;
    }
    while (month >= 12)
    {
        month -= 12;
        year += 1;
    }

    int64_t days = 0;
    if (year >= 1970)
    {
        for (int y = 1970; y < year; y++)
            days += LibC_time_days_in_year(y);
    }
    else
    {
        for (int y = 1969; y >= year; y--)
            days -= LibC_time_days_in_year(y);
    }

    for (int m = 0; m < month; m++)
        days += LibC_time_days_in_month(year, m);

    days += (int64_t) (tm->tm_mday - 1);

    int64_t sec = days * 86400LL;
    sec += (int64_t) tm->tm_hour * 3600LL;
    sec += (int64_t) tm->tm_min * 60LL;
    sec += (int64_t) tm->tm_sec;

    struct tm* normalized = gmtime((const time_t*) &sec);
    if (normalized)
        *tm = *normalized;

    return (time_t) sec;
}

char* asctime(const struct tm* tm)
{
    if (!tm)
        return NULL;
    (void) snprintf(LibC_time_asctime_buf,
                    sizeof(LibC_time_asctime_buf),
                    "%.3s %.3s %02d %02d:%02d:%02d %04d\n",
                    (tm->tm_wday >= 0 && tm->tm_wday < 7) ? LibC_time_weekday_short[tm->tm_wday] : "???",
                    (tm->tm_mon >= 0 && tm->tm_mon < 12) ? LibC_time_month_short[tm->tm_mon] : "???",
                    tm->tm_mday,
                    tm->tm_hour,
                    tm->tm_min,
                    tm->tm_sec,
                    tm->tm_year + 1900);
    return LibC_time_asctime_buf;
}

char* ctime(const time_t* timer)
{
    struct tm* tm = localtime(timer);
    if (!tm)
        return NULL;
    return asctime(tm);
}

size_t strftime(char* s, size_t max, const char* format, const struct tm* tm)
{
    if (!s || !format || !tm || max == 0U)
        return 0U;

    size_t pos = 0U;
    for (size_t i = 0U; format[i] != '\0'; i++)
    {
        if (format[i] != '%')
        {
            if (pos + 1U >= max)
                return 0U;
            s[pos++] = format[i];
            continue;
        }

        i++;
        char c = format[i];
        if (c == '\0')
            break;

        switch (c)
        {
            case '%':
                if (pos + 1U >= max)
                    return 0U;
                s[pos++] = '%';
                break;
            case 'Y':
                if (!LibC_time_append_num(s, max, &pos, tm->tm_year + 1900, 4, 1))
                    return 0U;
                break;
            case 'y':
                if (!LibC_time_append_num(s, max, &pos, (tm->tm_year + 1900) % 100, 2, 1))
                    return 0U;
                break;
            case 'm':
                if (!LibC_time_append_num(s, max, &pos, tm->tm_mon + 1, 2, 1))
                    return 0U;
                break;
            case 'd':
                if (!LibC_time_append_num(s, max, &pos, tm->tm_mday, 2, 1))
                    return 0U;
                break;
            case 'H':
                if (!LibC_time_append_num(s, max, &pos, tm->tm_hour, 2, 1))
                    return 0U;
                break;
            case 'M':
                if (!LibC_time_append_num(s, max, &pos, tm->tm_min, 2, 1))
                    return 0U;
                break;
            case 'S':
                if (!LibC_time_append_num(s, max, &pos, tm->tm_sec, 2, 1))
                    return 0U;
                break;
            case 'a':
                if (!LibC_time_append_str(s, max, &pos,
                                          (tm->tm_wday >= 0 && tm->tm_wday < 7) ? LibC_time_weekday_short[tm->tm_wday] : "???"))
                    return 0U;
                break;
            case 'A':
                if (!LibC_time_append_str(s, max, &pos,
                                          (tm->tm_wday >= 0 && tm->tm_wday < 7) ? LibC_time_weekday_long[tm->tm_wday] : "???"))
                    return 0U;
                break;
            case 'b':
            case 'h':
                if (!LibC_time_append_str(s, max, &pos,
                                          (tm->tm_mon >= 0 && tm->tm_mon < 12) ? LibC_time_month_short[tm->tm_mon] : "???"))
                    return 0U;
                break;
            case 'B':
                if (!LibC_time_append_str(s, max, &pos,
                                          (tm->tm_mon >= 0 && tm->tm_mon < 12) ? LibC_time_month_long[tm->tm_mon] : "???"))
                    return 0U;
                break;
            default:
                if (pos + 2U >= max)
                    return 0U;
                s[pos++] = '%';
                s[pos++] = c;
                break;
        }
    }

    s[pos] = '\0';
    return pos;
}
