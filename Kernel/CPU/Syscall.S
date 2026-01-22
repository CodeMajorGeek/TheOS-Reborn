.section .text

.globl enable_syscall_ext
.globl syscall_handler_stub

.extern Syscall_interupt_handler

# Segment selectors with RPL
.set USER_CODE_SEGMENT_RPL3,  0x23  # USER_CODE_SEGMENT (0x20) | RPL_3 (0x3)
.set USER_DATA_SEGMENT_RPL3,  0x1B  # USER_DATA_SEGMENT (0x18) | RPL_3 (0x3)

# Enable the system call ext (sysret & syscall):
enable_syscall_ext:
    movq $0xC0000080, %rcx   # IA32_EFER
    rdmsr
    orl  $1, %eax            # SCE = 1 (enable SYSCALL/SYSRET)
    wrmsr

    # IA32_STAR (0xC0000081)
    # We want:
    #   - Kernel CS = 0x08 (KERNEL_CODE_SEGMENT)
    #   - User base = 0x10 so SYSRET sets:
    #       CS = 0x20 (USER_CODE_SEGMENT) and SS = 0x18 (USER_DATA_SEGMENT)
    #
    # High 32 bits = (user_base << 16) | CS_kernel = 0x00100008
    # Low 32 bits = 0 (ignored in 64-bit)

    movq $0xC0000081, %rcx   # IA32_STAR
    movl $0x00000000, %eax   # low 32 bits
    movl $0x00100008, %edx   # high 32 bits: 0x00100008
    wrmsr

    retq

syscall_handler_stub:
    # Entry into kernel via SYSCALL:
    #   RAX = syscall number
    #   RCX = user return RIP
    #   R11 = user RFLAGS
    #   CS/SS kernel defined by IA32_STAR
    #   RSP remains unchanged (points to user stack)

    cli

    # Save necessary registers in callee-saved registers
    pushq %rbp
    pushq %rbx
    pushq %r12                # Use R12 to save RAX (syscall number)
    
    movq %rax, %r12          # R12 = RAX (syscall number)
    movq %rcx, %rbp          # RBP = RCX (user RIP)
    movq %r11, %rbx          # RBX = R11 (user RFLAGS)
    movq %rsp, %rax          # RAX = user RSP (temporary)

    # Align stack to 16 bytes before C call
    andq $0xFFFFFFFFFFFFFFF0, %rsp
    
    # Build frame on kernel stack
    pushq $USER_DATA_SEGMENT_RPL3  # SS (user stack segment)
    pushq %rax                      # userrsp (user RSP)
    pushq %rbx                      # eflags (user RFLAGS)
    pushq $USER_CODE_SEGMENT_RPL3  # CS (user code segment)
    pushq %rbp                      # rip (user RIP)
    pushq $0                        # err_code (no error for syscall)
    pushq $0x80                     # int_no (SYSCALL_INT = 0x80)
    
    # Call C handler with syscall number and frame
    movq %r12, %rdi          # RDI = syscall number (RAX saved in R12)
    movq %rsp, %rsi          # RSI = pointer to frame
    callq Syscall_interupt_handler

    # Restore RSP (remove frame from stack)
    addq $56, %rsp           # 7 * 8 bytes = 56 bytes for frame

    # Restore RCX/R11 for sysretq
    movq %rbp, %rcx          # RCX = user RIP
    movq %rbx, %r11          # R11 = user RFLAGS
    
    # Restore saved registers
    popq %r12
    popq %rbx
    popq %rbp

    # No sti here: interrupts will be re-enabled in user mode
    # according to R11 flags, masked by IA32_FMASK.
    sysretq
