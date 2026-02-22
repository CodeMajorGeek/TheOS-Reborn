.section .text

.globl enable_syscall_ext
.globl syscall_handler_stub

.extern Syscall_interupt_handler
.extern Syscall_post_handler

.set TASK_CPU_LOCAL_SYSCALL_RSP0_OFF, 0
.set TASK_CPU_LOCAL_CPU_INDEX_OFF, 8
.set TASK_CPU_LOCAL_SYSCALL_USER_RSP_OFF, 40

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
    # Save user RSP in per-CPU storage, then move to the trusted kernel stack.
    movq %rsp, %gs:TASK_CPU_LOCAL_SYSCALL_USER_RSP_OFF
    movq %gs:TASK_CPU_LOCAL_SYSCALL_RSP0_OFF, %rsp

    # Save callee-saved registers.
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    # Build syscall_frame_t on stack (128 bytes, 16-byte aligned).
    subq $128, %rsp
    movq %r15, 0(%rsp)       # r15
    movq %r14, 8(%rsp)       # r14
    movq %r13, 16(%rsp)      # r13
    movq %r12, 24(%rsp)      # r12
    movq %rbp, 32(%rsp)      # rbp
    movq %rbx, 40(%rsp)      # rbx
    movq %rdi, 48(%rsp)      # rdi
    movq %rsi, 56(%rsp)      # rsi
    movq %rdx, 64(%rsp)      # rdx
    movq %r10, 72(%rsp)      # r10 (arg4)
    movq %r8, 80(%rsp)       # r8
    movq %r9, 88(%rsp)       # r9
    movq %rcx, 96(%rsp)      # rip (saved by SYSCALL)
    movq %r11, 104(%rsp)     # rflags (saved by SYSCALL)
    movq %gs:TASK_CPU_LOCAL_SYSCALL_USER_RSP_OFF, %r10
    movq %r10, 112(%rsp)     # rsp (saved user stack pointer)
    movq $0, 120(%rsp)       # reserved

    # Call C handler with syscall number and frame
    movq %rax, %rdi          # RDI = syscall number
    movq %rsp, %rsi          # RSI = pointer to syscall_frame_t
    movl %gs:TASK_CPU_LOCAL_CPU_INDEX_OFF, %edx # RDX = cpu_index (per-CPU via GS after SWAPGS)
    sti
    callq Syscall_interupt_handler
    movq %rax, %rdi          # RDI = syscall return value
    movq %rsp, %rsi          # RSI = pointer to syscall_frame_t
    movl %gs:TASK_CPU_LOCAL_CPU_INDEX_OFF, %edx # RDX = cpu_index
    callq Syscall_post_handler
    cli

    # Restore RCX/R11/RSP and callee-saved regs from (possibly updated) syscall frame.
    movq 96(%rsp), %rcx      # RCX = user RIP
    movq 104(%rsp), %r11     # R11 = user RFLAGS
    movq 112(%rsp), %r10     # R10 = user RSP

    movq 0(%rsp), %r15       # R15
    movq 8(%rsp), %r14       # R14
    movq 16(%rsp), %r13      # R13
    movq 24(%rsp), %r12      # R12
    movq 32(%rsp), %rbp      # RBP
    movq 40(%rsp), %rbx      # RBX

    addq $128, %rsp          # Drop syscall_frame_t
    addq $48, %rsp           # Drop saved callee-saved snapshot

    movq %r10, %rsp
    swapgs

    # No sti here: interrupts will be re-enabled in user mode
    # according to R11 flags, masked by IA32_FMASK.
    sysretq
