.section .text

.globl switch_to_usermode

switch_to_usermode:
    movq $in_user_mode, %rcx    # To be loaded into RIP.
    pushfq
    popq %r11
    orq $0x200, %r11            # To be loaded into EFLAGS.        
    sysretq

in_user_mode:
    retq