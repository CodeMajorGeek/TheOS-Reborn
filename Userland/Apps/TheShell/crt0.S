.section .text

.globl _start
.extern main

_start:
    xorq %rbp, %rbp
    call main
    movq %rax, %rdi    # main return code
    movq $12, %rax     # SYS_EXIT
    syscall

1:
    pause
    jmp 1b
