.altmacro

.extern ISR_exception_handler
.extern IRQ_handler

.globl ISR_stub_table

.section .text

irq_common_stub:
    callq IRQ_handler
    sti
    iretq

isr_common_stub:
    callq ISR_exception_handler
    sti
    iretq

.macro isr_err_stub no
    isr_stub_\no:
        pushq $\no
        jmp isr_common_stub
.endm

.macro isr_no_err_stub no
    isr_stub_\no:
        cli
        pushq $0
        pushq $\no
        jmp isr_common_stub
.endm

.macro irq_stub isrno, irqno
    irq_stub_\irqno:
        cli
        pushq $\irqno
        pushq $\isrno
        jmp irq_common_stub
.endm

.macro idt_isr_entry_stub no
    .quad isr_stub_\no
.endm

.macro idt_irq_entry_stub irqno
    .quad irq_stub_\irqno
.endm

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
irq_stub 32, 0
irq_stub 33, 1
irq_stub 34, 2
irq_stub 35, 3
irq_stub 36, 4
irq_stub 37, 5
irq_stub 38, 6
irq_stub 39, 7
irq_stub 40, 8
irq_stub 41, 9
irq_stub 42, 10
irq_stub 43, 11
irq_stub 44, 12
irq_stub 45, 13
irq_stub 46, 14
irq_stub 47, 15

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
