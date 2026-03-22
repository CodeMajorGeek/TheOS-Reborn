#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define SEEK_SET      0
#define SEEK_CUR      1
#define SEEK_END      2
#define F_OK          0
#define X_OK          1
#define W_OK          2
#define R_OK          4

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int open(const char* path, int flags, ...);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int isatty(int fd);
int access(const char* path, int mode);
int brk(void* addr);
void* sbrk(intptr_t increment);
int clear_screen(void);

pid_t fork(void);
int execve(const char* path, char* const argv[], char* const envp[]);
int execv(const char* path, char* const argv[]);
int execvp(const char* file, char* const argv[]);
int execl(const char* path, const char* arg, ...);
int execlp(const char* file, const char* arg, ...);
int kill(pid_t pid, int sig);
__attribute__((__noreturn__)) void _exit(int status);
int daemon(int nochdir, int noclose);

unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
int sleep_state(unsigned int state);
int shutdown(void);
int reboot(void);

#endif
