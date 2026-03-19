#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long clock_t;

#ifndef CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 1000000L
#endif

struct tm
{
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

struct timespec
{
    time_t tv_sec;
    long tv_nsec;
};

#define TIME_UTC 1

clock_t clock(void);
double difftime(time_t time1, time_t time0);
time_t mktime(struct tm* tm);
time_t time(time_t* tloc);
int nanosleep(const struct timespec* req, struct timespec* rem);
int timespec_get(struct timespec* ts, int base);

char* asctime(const struct tm* tm);
char* ctime(const time_t* timer);
struct tm* gmtime(const time_t* timer);
struct tm* localtime(const time_t* timer);
size_t strftime(char* s, size_t max, const char* format, const struct tm* tm);

#ifdef __cplusplus
}
#endif

#endif
