#ifndef _USERMODE_H
#define _USERMODE_H

#include <stdbool.h>
#include <stdint.h>

bool UserMode_run_elf(const char* file_name);
__attribute__((__noreturn__)) void switch_to_usermode(uintptr_t entry, uintptr_t user_stack_top);

#endif
