.section .text

.globl _start
.extern main

_start:
    xorq %rbp, %rbp
    movq (%rsp), %rdi          # argc
    leaq 8(%rsp), %rsi         # argv
    leaq 16(%rsp,%rdi,8), %rdx # envp
    call main
    movq %rax, %rdi    # main return code
    movq $12, %rax     # SYS_EXIT
    syscall

1:
    pause
    jmp 1b
