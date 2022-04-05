#include <CPU/GDT.h>

.section .text

.globl switch_to_user_mode

switch_to_user_mode:
    callq enable_sys_call_ext
    
    movq $in_user_mode, %rcx    # To be loaded into RIP.
    movq $0x202, %r11           # To be loaded into EFLAGS.
    sysretq

# Enable the system call ext (sysret & syscall):
enable_sys_call_ext:
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

in_user_mode:
    nop
    nop
    nop
    nop
    nop
