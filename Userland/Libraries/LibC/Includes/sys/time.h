#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _SUSECONDS_T_DECLARED
typedef long suseconds_t;
#define _SUSECONDS_T_DECLARED 1
#endif

#ifndef _STRUCT_TIMEVAL
#define _STRUCT_TIMEVAL 1
struct timeval
{
    time_t tv_sec;
    suseconds_t tv_usec;
};
#endif

struct timezone
{
    int tz_minuteswest;
    int tz_dsttime;
};

struct itimerval
{
    struct timeval it_interval;
    struct timeval it_value;
};

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

#define timerclear(tvp)      \
    do                       \
    {                        \
        (tvp)->tv_sec = 0;   \
        (tvp)->tv_usec = 0;  \
    } while (0)

#define timerisset(tvp) (((tvp)->tv_sec != 0) || ((tvp)->tv_usec != 0))

#define timercmp(a, b, cmp)                                             \
    (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec cmp (b)->tv_usec)    \
                                   : ((a)->tv_sec cmp (b)->tv_sec))

int gettimeofday(struct timeval* tv, struct timezone* tz);
int settimeofday(const struct timeval* tv, const struct timezone* tz);
int getitimer(int which, struct itimerval* curr_value);
int setitimer(int which, const struct itimerval* new_value, struct itimerval* old_value);

#ifdef __cplusplus
}
#endif

#endif
