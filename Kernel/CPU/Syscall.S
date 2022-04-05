.section .text

.globl enable_syscall_ext
.globl syscall_handler_stub

.extern syscall_handler

# Enable the system call ext (sysret & syscall):
enable_syscall_ext:
    movq $0xC0000080, %rcx
    rdmsr
    orl $1, %eax
    wrmsr
    movq $0xC0000081, %rcx
    rdmsr
    movq $0x00180008, %rdx
    wrmsr
    retq

syscall_handler_stub:
    pushq %rsp
    callq syscall_handler
    popq %rsp
    sysretq