.section .text

.equ SYS_EXIT_NR, 9

.globl _start
.extern main

_start:
    xorq %rbp, %rbp
    movq (%rsp), %rdi          # argc
    leaq 8(%rsp), %rsi         # argv
    leaq 16(%rsp,%rdi,8), %rdx # envp
    call main
    movq %rax, %rdi    # main return code
    movq $SYS_EXIT_NR, %rax
    syscall

1:
    pause
    jmp 1b
