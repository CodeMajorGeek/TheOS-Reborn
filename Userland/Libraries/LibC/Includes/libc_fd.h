#ifndef _LIBC_FD_H
#define _LIBC_FD_H

int libc_fd_get_kernel(int fd);
int libc_fd_adopt_kernel(int kernel_fd);

#endif
