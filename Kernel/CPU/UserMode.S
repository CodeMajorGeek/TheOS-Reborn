.section .text

.globl switch_to_usermode

# Segment selectors with RPL
.set USER_CODE_SEGMENT_RPL3,  0x23
.set USER_DATA_SEGMENT_RPL3,  0x1B

# RFLAGS bits
.set RFLAGS_IF,               1 << 9

# switch_to_usermode(entry_rip, user_stack_top)
#   rdi = user RIP
#   rsi = user RSP top
switch_to_usermode:
    movq %rsi, %rax
    andq $-16, %rax

    pushq $USER_DATA_SEGMENT_RPL3
    pushq %rax
    pushfq
    orq $RFLAGS_IF, (%rsp)
    pushq $USER_CODE_SEGMENT_RPL3
    pushq %rdi
    iretq
