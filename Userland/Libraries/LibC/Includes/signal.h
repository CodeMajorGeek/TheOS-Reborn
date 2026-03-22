#ifndef _SIGNAL_H
#define _SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* TheOS UNIX-like signal numbering (aligned with kernel UAPI). */
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGEMT      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGBUS      10
#define SIGSEGV     11
#define SIGSYS      12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGUSR1     16
#define SIGUSR2     17
#define SIGCHLD     18
#define SIGPWR      19
#define SIGWINCH    20
#define SIGURG      21
#define SIGPOLL     22
#define SIGSTOP     23
#define SIGTSTP     24
#define SIGCONT     25
#define SIGTTIN     26
#define SIGTTOU     27
#define SIGVTALRM   28
#define SIGPROF     29
#define SIGXCPU     30
#define SIGXFSZ     31
#define SIGWAITING  32
#define SIGLWP      33
#define SIGAIO      34
#define NSIG        35

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
