.section .text

.globl enable_syscall_ext
.globl syscall_handler_stub

.extern Syscall_interupt_handler

.set TASK_CPU_LOCAL_SYSCALL_RSP0_OFF, 0

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
    swapgs

    # SYSCALL does not switch stack automatically in long mode.
    # Save user RSP, then move to the per-CPU trusted kernel stack (TSS.rsp0 mirror).
    movq %rsp, %r14
    movq %gs:TASK_CPU_LOCAL_SYSCALL_RSP0_OFF, %rsp

    # Save callee-saved registers and syscall context.
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14               # saved user RSP
    pushq %r15

    movq %rax, %r12          # R12 = syscall number
    movq %rcx, %rbp          # RBP = user RIP
    movq %r11, %rbx          # RBX = user RFLAGS

    # Build syscall_frame_t on stack
    subq $80, %rsp
    movq %rdi, 0(%rsp)       # rdi
    movq %rsi, 8(%rsp)       # rsi
    movq %rdx, 16(%rsp)      # rdx
    movq %r10, 24(%rsp)      # r10
    movq %r8, 32(%rsp)       # r8
    movq %r9, 40(%rsp)       # r9
    movq %rbp, 48(%rsp)      # rip
    movq %rbx, 56(%rsp)      # rflags
    movq %r14, 64(%rsp)      # rsp (saved user stack pointer)

    # Call C handler with syscall number and frame
    movq %r12, %rdi          # RDI = syscall number (RAX saved in R12)
    movq %rsp, %rsi          # RSI = pointer to syscall_frame_t
    callq Syscall_interupt_handler
    addq $80, %rsp           # Drop syscall_frame_t

    # Restore RCX/R11 for sysretq
    movq %rbp, %rcx          # RCX = user RIP
    movq %rbx, %r11          # R11 = user RFLAGS
    
    # Restore saved registers and switch back to user RSP for SYSRET.
    popq %r15
    popq %r14                # user RSP
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    movq %r14, %rsp
    swapgs

    # No sti here: interrupts will be re-enabled in user mode
    # according to R11 flags, masked by IA32_FMASK.
    sysretq
