.section .text

.globl TSS_flush

TSS_flush:
    movw %di, %ax
    ltr %ax

    retq