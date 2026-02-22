.section .text

.globl _start
.extern main

_start:
    xorq %rbp, %rbp
    call main

1:
    movq $4, %rax      # SYS_SLEEP_MS
    movq $1000, %rdi
    syscall
    jmp 1b
