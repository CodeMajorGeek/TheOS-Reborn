#include <errno.h>
#include <stdint.h>
#include <sys/time.h>
#include <syscall.h>

static uint32_t LibC_time_tick_hz_cache = 100U;

static int LibC_time_valid_which(int which)
{
    return (which == ITIMER_REAL || which == ITIMER_VIRTUAL || which == ITIMER_PROF);
}

static int LibC_time_validate_timeval(const struct timeval* tv)
{
    if (!tv)
        return 1;

    if (tv->tv_sec < (time_t) 0)
        return 0;
    if (tv->tv_usec < 0 || tv->tv_usec >= 1000000L)
        return 0;

    return 1;
}

static void LibC_time_clear_itimer(struct itimerval* itv)
{
    if (!itv)
        return;

    itv->it_interval.tv_sec = 0;
    itv->it_interval.tv_usec = 0;
    itv->it_value.tv_sec = 0;
    itv->it_value.tv_usec = 0;
}

static void LibC_time_fill_timeval_from_ticks(struct timeval* tv, uint64_t ticks, uint32_t tick_hz)
{
    if (!tv)
        return;

    if (tick_hz == 0)
        tick_hz = 100U;

    tv->tv_sec = (time_t) (ticks / (uint64_t) tick_hz);
    uint64_t rem_ticks = ticks % (uint64_t) tick_hz;
    tv->tv_usec = (suseconds_t) ((rem_ticks * 1000000ULL) / (uint64_t) tick_hz);
}

int gettimeofday(struct timeval* tv, struct timezone* tz)
{
    if (tv)
    {
        uint64_t rtc_seconds = sys_rtc_time_get();
        if (rtc_seconds != 0ULL)
        {
            tv->tv_sec = (time_t) rtc_seconds;
            tv->tv_usec = 0;
        }
        else
        {
            syscall_cpu_info_t info;
            uint64_t ticks = 0;
            uint32_t tick_hz = 0;

            if (sys_cpu_info_get(&info) == 0 && info.tick_hz != 0)
            {
                ticks = info.ticks;
                tick_hz = info.tick_hz;
                LibC_time_tick_hz_cache = tick_hz;
            }
            else
            {
                ticks = sys_tick_get();
                tick_hz = LibC_time_tick_hz_cache;
                if (tick_hz == 0)
                    tick_hz = 100U;
            }

            LibC_time_fill_timeval_from_ticks(tv, ticks, tick_hz);
        }
    }

    if (tz)
    {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }

    return 0;
}

int settimeofday(const struct timeval* tv, const struct timezone* tz)
{
    (void) tz;

    if (!LibC_time_validate_timeval(tv))
    {
        errno = EINVAL;
        return -1;
    }

    errno = ENOSYS;
    return -1;
}

int getitimer(int which, struct itimerval* curr_value)
{
    if (!LibC_time_valid_which(which))
    {
        errno = EINVAL;
        return -1;
    }

    if (!curr_value)
    {
        errno = EFAULT;
        return -1;
    }

    LibC_time_clear_itimer(curr_value);
    errno = ENOSYS;
    return -1;
}

int setitimer(int which, const struct itimerval* new_value, struct itimerval* old_value)
{
    if (!LibC_time_valid_which(which))
    {
        errno = EINVAL;
        return -1;
    }

    if (new_value && (!LibC_time_validate_timeval(&new_value->it_value) ||
                      !LibC_time_validate_timeval(&new_value->it_interval)))
    {
        errno = EINVAL;
        return -1;
    }

    if (old_value)
        LibC_time_clear_itimer(old_value);

    errno = ENOSYS;
    return -1;
}
