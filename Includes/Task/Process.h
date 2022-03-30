#ifndef _PROCESS_H
#define _PROCESS_H

#include <stdint.h>

typedef struct kernel_stack
{
    uint64_t stack_k[512];
} kernel_stack_t;

#endif