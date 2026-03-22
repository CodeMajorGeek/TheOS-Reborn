#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <UAPI/Net.h>

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
