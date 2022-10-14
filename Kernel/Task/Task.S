.section .text

.globl read_rip
.globl read_flags
.globl perform_task_switch

read_rip:
    popq %rax
    jmpq *%rax

read_flags:
    pushfq
    popq %rax
    retq

perform_task_switch:
    cli

    movq 8(%rdi), %rbp

    movq 16(%rdi), %rax
    movq %rax, %cr3

    movq 80(%rdi), %rax
    pushq %rax
    popfq
    
    movq 24(%rdi), %rax
    movq 32(%rdi), %rcx
    movq 40(%rdi), %rdx
    movq 48(%rdi), %r8
    movq 56(%rdi), %r9
    movq 64(%rdi), %r10
    movq 72(%rdi), %r11
    
    movq $0xDEADBEEF, %rax
    sti
    jmpq *(%rdi)
