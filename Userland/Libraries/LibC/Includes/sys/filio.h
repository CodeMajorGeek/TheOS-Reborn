#ifndef _SYS_FILIO_H
#define _SYS_FILIO_H

#include <sys/ioctl.h>

/*
 * Historic BSD-compatible location for file/socket ioctls.
 * Keep aliases aligned with <sys/ioctl.h>.
 */
#ifndef FIONREAD
#define FIONREAD 0x541BUL
#endif

#ifndef FIONBIO
#define FIONBIO  0x5421UL
#endif

#endif
