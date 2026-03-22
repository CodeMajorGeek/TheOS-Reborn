#include <syscall.h>
#include <libc_tls.h>

.section .text

.globl _start
.extern main
.extern __libc_tls_init_main

_start:
    xorq %rbp, %rbp
    call __libc_tls_init_main
    testl %eax, %eax
    jne .Ltls_fail
    movq (%rsp), %rdi          # argc
    leaq 8(%rsp), %rsi         # argv
    leaq 16(%rsp,%rdi,8), %rdx # envp
    call main
    movq %rax, %rdi
    movq $SYS_EXIT, %rax
    syscall

.Ltls_fail:
    movq $127, %rdi
    movq $SYS_EXIT, %rax
    syscall

1:
    pause
    jmp 1b
