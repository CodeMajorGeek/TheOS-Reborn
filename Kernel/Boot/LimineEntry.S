.section .text
.code64

.globl limine_entry
.type limine_entry, @function

.extern k_entry
.extern kernel_stack_top

limine_entry:
    lea kernel_stack_top(%rip), %rsp
    andq $-16, %rsp
    xorq %rbp, %rbp
    xorq %rdi, %rdi
    callq k_entry

1:
    cli
    hlt
    jmp 1b

.size limine_entry, . - limine_entry
