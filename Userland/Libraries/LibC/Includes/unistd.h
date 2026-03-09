#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define SEEK_SET      0
#define SEEK_CUR      1
#define SEEK_END      2

typedef long off_t;

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int isatty(int fd);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
int sleep_state(unsigned int state);
int shutdown(void);
int reboot(void);

#endif
