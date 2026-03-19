#ifndef _SIGNAL_H
#define _SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal POSIX-compatible signal set. */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGFPE   8
#define SIGKILL  9
#define SIGSEGV  11
#define SIGALRM  14
#define SIGTERM  15
#define SIGSTOP  19

typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t) 0)
#define SIG_IGN ((sighandler_t) 1)
#define SIG_ERR ((sighandler_t) -1)

sighandler_t signal(int sig, sighandler_t handler);
int raise(int sig);

#ifdef __cplusplus
}
#endif

#endif
