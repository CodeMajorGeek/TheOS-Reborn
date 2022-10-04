.section .text

.globl GDT_flush

GDT_flush:
    lgdtl %rdi
    retq