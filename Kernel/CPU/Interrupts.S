.altmacro

.extern ISR_handler
.extern IRQ_handler

.globl ISR_stub_table

.section .text

# ------------------------------------------------
# Sauvegarde/restauration des registres
# ------------------------------------------------
.macro PUSH_REGS
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro POP_REGS
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
.endm

# ------------------------------------------------
# IRQ common stub
# Stack layout at call time (from top to bottom):
#   [RSP+0]   = R15 (last pushed by PUSH_REGS)
#   ...
#   [RSP+120] = RAX (first pushed by PUSH_REGS)
#   [RSP+128] = int_no (pushed by stub)
#   [RSP+136] = err_code (pushed by stub, always 0 for IRQ)
#   [RSP+144] = RIP (pushed by CPU)
#   [RSP+152] = CS (pushed by CPU)
#   [RSP+160] = RFLAGS (pushed by CPU)
#   [RSP+168] = RSP (pushed by CPU if user mode)
#   [RSP+176] = SS (pushed by CPU if user mode)
# ------------------------------------------------
irq_common_stub:
    cli

    PUSH_REGS

    # RSP points to top of PUSH_REGS (R15)
    # Frame pointer is RSP (points to interrupt_frame_t structure)
    movq %rsp, %rdi
    call IRQ_handler

    POP_REGS

    addq $16, %rsp  # Remove int_no and err_code
    # Now stack has: RIP, CS, RFLAGS, (RSP, SS if user mode)
    # Check CS RPL to determine if we need to fix SS before iretq
    movq 8(%rsp), %rax       # Load CS from stack
    andq $0x3, %rax          # Extract RPL
    cmpw $0x3, %ax           # RPL = 3 (user mode)?
    jne .iretq_kernel_irq    # No, kernel mode, skip SS check
    
    # Returning to user mode: verify SS is valid
    # Stack frame: [RSP+0] = RIP, [RSP+8] = CS, [RSP+16] = RFLAGS, [RSP+24] = RSP, [RSP+32] = SS
    movq 32(%rsp), %rax      # Load SS from stack
    test %rax, %rax          # SS is NULL?
    jz .fix_ss_irq           # Yes, fix it
    cmpw $0x1B, %ax          # SS = USER_DATA_SEGMENT_RPL3 (0x1B)?
    je .iretq_user_irq       # Yes, correct
    
.fix_ss_irq:
    # Fix SS on stack to USER_DATA_SEGMENT_RPL3
    movq $0x1B, 32(%rsp)     # SS = USER_DATA_SEGMENT_RPL3
    
.iretq_user_irq:
    # User mode: RIP, CS, RFLAGS, RSP, SS on stack
    # iretq will read all 5 values
    iretq
    
.iretq_kernel_irq:
    # Kernel mode: only RIP, CS, RFLAGS on stack
    # iretq will not read RSP/SS
    iretq

# ------------------------------------------------
# ISR common stub
# Stack layout at call time (from top to bottom):
#   [RSP+0]   = R15 (last pushed by PUSH_REGS)
#   ...
#   [RSP+120] = RAX (first pushed by PUSH_REGS)
#   [RSP+128] = int_no (pushed by stub)
#   [RSP+136] = err_code (pushed by CPU for isr_err_stub, or by stub for isr_no_err_stub)
#   [RSP+144] = RIP (pushed by CPU)
#   [RSP+152] = CS (pushed by CPU)
#   [RSP+160] = RFLAGS (pushed by CPU)
#   [RSP+168] = RSP (pushed by CPU if user mode)
#   [RSP+176] = SS (pushed by CPU if user mode)
# ------------------------------------------------
isr_common_stub:
    cli

    PUSH_REGS

    # RSP points to top of PUSH_REGS (R15)
    # Frame pointer is RSP (points to interrupt_frame_t structure)
    movq %rsp, %rdi
    call ISR_handler

    POP_REGS

    addq $16, %rsp  # Remove int_no and err_code
    # Now stack has: RIP, CS, RFLAGS, (RSP, SS if user mode)
    # Check CS RPL to determine if we need to fix SS before iretq
    movq 8(%rsp), %rax       # Load CS from stack
    andq $0x3, %rax          # Extract RPL
    cmpw $0x3, %ax           # RPL = 3 (user mode)?
    jne .iretq_kernel_isr    # No, kernel mode, skip SS check
    
    # Returning to user mode: verify SS is valid
    # Stack frame: [RSP+0] = RIP, [RSP+8] = CS, [RSP+16] = RFLAGS, [RSP+24] = RSP, [RSP+32] = SS
    movq 32(%rsp), %rax      # Load SS from stack
    test %rax, %rax          # SS is NULL?
    jz .fix_ss_isr           # Yes, fix it
    cmpw $0x1B, %ax          # SS = USER_DATA_SEGMENT_RPL3 (0x1B)?
    je .iretq_user_isr       # Yes, correct
    
.fix_ss_isr:
    # Fix SS on stack to USER_DATA_SEGMENT_RPL3
    movq $0x1B, 32(%rsp)     # SS = USER_DATA_SEGMENT_RPL3
    
.iretq_user_isr:
    # User mode: RIP, CS, RFLAGS, RSP, SS on stack
    # iretq will read all 5 values
    iretq
    
.iretq_kernel_isr:
    # Kernel mode: only RIP, CS, RFLAGS on stack
    # iretq will not read RSP/SS
    iretq

# ------------------------------------------------
# Macros ISR / IRQ
# ------------------------------------------------
.macro isr_err_stub no
    isr_stub_\()no:
        cli
        # CPU already pushed error_code
        pushq $\no
        jmp isr_common_stub
.endm

.macro isr_no_err_stub no
    isr_stub_\()no:
        cli
        pushq $0
        pushq $\no
        jmp isr_common_stub
.endm

.macro irq_stub isrno, irqno
    irq_stub_\()irqno:
        cli
        pushq $0              # error code = 0 pour IRQ
        pushq $\isrno         # numéro d’interruption (32+)
        jmp irq_common_stub
.endm

.macro idt_isr_entry_stub no
    .quad isr_stub_\()no
.endm

.macro idt_irq_entry_stub irqno
    .quad irq_stub_\()irqno
.endm

# ------------------------------------------------
# ISRs CPU
# ------------------------------------------------
isr_no_err_stub 0
isr_no_err_stub 1
isr_no_err_stub 2
isr_no_err_stub 3
isr_no_err_stub 4
isr_no_err_stub 5
isr_no_err_stub 6
isr_no_err_stub 7
isr_err_stub    8
isr_no_err_stub 9
isr_err_stub    10
isr_err_stub    11
isr_err_stub    12
isr_err_stub    13
isr_err_stub    14
isr_no_err_stub 15
isr_no_err_stub 16
isr_err_stub    17
isr_no_err_stub 18
isr_no_err_stub 19
isr_no_err_stub 20
isr_no_err_stub 21
isr_no_err_stub 22
isr_no_err_stub 23
isr_no_err_stub 24
isr_no_err_stub 25
isr_no_err_stub 26
isr_no_err_stub 27
isr_no_err_stub 28
isr_no_err_stub 29
isr_err_stub    30
isr_no_err_stub 31

# ------------------------------------------------
# IRQs (PIC/APIC)
# ------------------------------------------------
.set isrno, 32
.set irqno, 0
.rept 16
    irq_stub %isrno, %irqno
    .set isrno, isrno + 1
    .set irqno, irqno + 1
.endr

# ------------------------------------------------
# Autres interruptions
# ------------------------------------------------
.set isrno, 48
.rept 255 - 48
    isr_no_err_stub %isrno
    .set isrno, isrno + 1
.endr

# ------------------------------------------------
# Table des stubs pour l’IDT
# ------------------------------------------------
ISR_stub_table:
    .set isrno, 0
    .rept 32
        idt_isr_entry_stub %isrno
        .set isrno, isrno + 1
    .endr

    .set irqno, 0
    .rept 16
        idt_irq_entry_stub %irqno
        .set irqno, irqno + 1
    .endr

    .set isrno, 48
    .rept 255 - 48
        idt_isr_entry_stub %isrno
        .set isrno, isrno + 1
    .endr
