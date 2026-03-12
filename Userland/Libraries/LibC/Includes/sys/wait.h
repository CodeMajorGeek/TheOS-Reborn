#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG 1

#define WIFEXITED(status)   (((status) & 0x7F) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) ((((status) & 0x7F) != 0) && (((status) & 0x7F) != 0x7F))
#define WTERMSIG(status)    ((status) & 0x7F)

int waitpid(pid_t pid, int* status, int options);

#endif
