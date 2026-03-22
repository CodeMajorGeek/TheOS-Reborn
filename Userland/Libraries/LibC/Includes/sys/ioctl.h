#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal ioctl request set required by current userland ports.
 * Values match the Linux ABI for source compatibility.
 */
#ifndef FIONREAD
#define FIONREAD 0x541BUL
#endif

#ifndef FIONBIO
#define FIONBIO  0x5421UL
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
