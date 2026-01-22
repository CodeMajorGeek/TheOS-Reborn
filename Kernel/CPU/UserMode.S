.section .text

.globl switch_to_usermode
.extern userland_stack_top
.extern userland_stack_bottom

# Requested Privilege Level (RPL)
.set RPL_3,               0x3

# Segment selectors with RPL
.set USER_CODE_SEGMENT_RPL3,  0x23  # USER_CODE_SEGMENT (0x20) | RPL_3 (0x3)
.set USER_DATA_SEGMENT_RPL3,  0x1B  # USER_DATA_SEGMENT (0x18) | RPL_3 (0x3)

# RFLAGS bits
.set RFLAGS_IF,           1 << 9  # Interrupt Flag

# For iretq, we need to build a stack frame on the stack:
#   [RSP+24] = SS (USER_DATA_SEGMENT | RPL_3)
#   [RSP+16] = RSP (user stack)
#   [RSP+8]  = RFLAGS (with IF enabled)
#   [RSP+0]  = CS:RIP (USER_CODE_SEGMENT | RPL_3 : in_user_mode)

switch_to_usermode:
    # Build stack frame for iretq on current kernel stack
    # Format (top to bottom): SS, RSP, RFLAGS, CS, RIP
    movabsq $userland_stack_top, %rax  # User stack address
    
    pushq $USER_DATA_SEGMENT_RPL3      # SS = USER_DATA_SEGMENT | RPL_3
    pushq %rax                          # RSP = user stack address
    pushfq                              # RFLAGS
    orq $RFLAGS_IF, (%rsp)              # Enable IF (bit 9) in RFLAGS on stack
    pushq $USER_CODE_SEGMENT_RPL3       # CS = USER_CODE_SEGMENT | RPL_3
    pushq $in_user_mode                # RIP
    
    # Do NOT load DS/ES/FS/GS here: we're still in ring 0 (kernel)
    # Loading user segments (0x1B) in ring 0 causes a #GP
    # iretq will automatically load SS from stack
    # In 64-bit, DS/ES/FS/GS are largely ignored, can be configured later
    
    # iretq will load CS, RIP, RFLAGS, SS, RSP from stack
    # RSP will be changed to point to user stack
    iretq

in_user_mode:
    # We are now in user mode (ring 3)!
    # In 64-bit, DS/ES/FS/GS are largely ignored
    # Do not load them to avoid any potential issues
    
    # Test: make a syscall to verify we're in ring 3
    movq $1, %rax    # Test syscall number
    movq $42, %rdi   # First argument (test)
    syscall          # Call kernel (returns to ring 0, then back to ring 3)
    
    # After syscall, we're back in ring 3
    # Infinite loop
    jmp in_user_mode
    retq
