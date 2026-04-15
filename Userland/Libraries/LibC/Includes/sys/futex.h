#ifndef _SYS_FUTEX_H
#define _SYS_FUTEX_H

#ifdef __cplusplus
extern "C" {
#endif

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

int sys_futex(int* uaddr, int op, int val, unsigned int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
