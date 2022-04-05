#include <CPU/GDT.h>

.section .text

.globl switch_to_usermode
.globl enable_syscall_ext

# Enable the system call ext (sysret & syscall):
enable_syscall_ext:
    movq $0xC0000082, %rcx
    wrmsr
    movq $0xC0000080, %rcx
    rdmsr
    orl $1, %eax
    wrmsr
    movq $0xC0000081, %rcx
    rdmsr
    movq $0x00180008, %rdx
    wrmsr
    retq

switch_to_usermode:
    movq $in_user_mode, %rcx    # To be loaded into RIP.
    pushfq
    popq %r11
    orq $0x200, %r11            # To be loaded into EFLAGS.        
    sysretq

in_user_mode:
    syscall
    retq