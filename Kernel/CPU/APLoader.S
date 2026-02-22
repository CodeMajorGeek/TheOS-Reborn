#define ASM_FILE
#include <CPU/SMP.h>
#include <Memory/VMMLayout.h>

.set AP_GDT_CODE32_SEL,    0x08
.set AP_GDT_CODE64_SEL,    0x10
.set AP_GDT_DATA_SEL,      0x18

.set AP_CR0_PE,            0x00000001
.set AP_CR0_PG,            0x80000000
.set AP_CR4_PAE,           0x00000020

.set AP_IA32_EFER_MSR,     0xC0000080
.set AP_EFER_LME,          0x00000100
.set AP_HHDM_BASE,         VMM_HHDM_BASE
.set AP_HANDOFF_VIRT,      AP_HHDM_BASE + SMP_HANDOFF_PHYS

.section .ap_trampoline, "ax"
.code16

.globl SMP_ap_trampoline_start
.globl SMP_ap_trampoline_end

SMP_ap_trampoline_start:
    cli
    cld

    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x0F00, %sp

    call .Lget_base
.Lget_base:
    popw %bx
    subw $(.Lget_base - SMP_ap_trampoline_start), %bx

    movw %cs, %ax
    movzwl %ax, %eax
    shll $4, %eax
    movzwl %bx, %edx
    addl %edx, %eax
    movl %eax, %ebp

    movw %bx, %si
    addw $(ap_gdtr - SMP_ap_trampoline_start), %si
    movl %ebp, %eax
    addl $(ap_gdt - SMP_ap_trampoline_start), %eax
    movl %eax, 2(%si)

    movw %bx, %si
    addw $(ap_jump32_ptr - SMP_ap_trampoline_start), %si
    movl %ebp, %eax
    addl $(ap32_entry - SMP_ap_trampoline_start), %eax
    movl %eax, (%si)

    movw %bx, %si
    addw $(ap_gdtr - SMP_ap_trampoline_start), %si
    .byte 0x66
    lgdt (%si)

    movl %cr0, %eax
    orl $AP_CR0_PE, %eax
    movl %eax, %cr0

    movw %bx, %si
    addw $(ap_jump32_ptr - SMP_ap_trampoline_start), %si
    .byte 0x66
    ljmp *(%si)

.code32
ap32_entry:
    movw $AP_GDT_DATA_SEL, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl $0x7F00, %esp

    movl SMP_HANDOFF_PHYS + SMP_HANDOFF_CR3_OFF, %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl $AP_CR4_PAE, %eax
    movl %eax, %cr4

    movl $AP_IA32_EFER_MSR, %ecx
    rdmsr
    orl $AP_EFER_LME, %eax
    wrmsr

    movl %cr0, %eax
    orl $AP_CR0_PG, %eax
    movl %eax, %cr0

    movl %ebp, %edi
    addl $(ap_jump64_ptr - SMP_ap_trampoline_start), %edi
    movl %ebp, %eax
    addl $(ap64_entry - SMP_ap_trampoline_start), %eax
    movl %eax, (%edi)
    ljmp *(%edi)

.code64
ap64_entry:
    cli
    movw $AP_GDT_DATA_SEL, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movabsq $AP_HANDOFF_VIRT, %rsi

    movq SMP_HANDOFF_MAGIC_OFF(%rsi), %rax
    movq $SMP_HANDOFF_MAGIC_VALUE, %rdx
    cmpq %rdx, %rax
    jne .Lap_hang

    movq SMP_HANDOFF_STACK_TOP_OFF(%rsi), %rsp
    xorq %rbp, %rbp

    movq SMP_HANDOFF_ARG_OFF(%rsi), %rdi
    movq SMP_HANDOFF_ENTRY64_OFF(%rsi), %rax
    # Match the C ABI stack contract for SMP_ap_entry.
    call *%rax
    jmp .Lap_hang

.Lap_hang:
    cli
.Lap_hang_loop:
    hlt
    jmp .Lap_hang_loop

.align 8
ap_gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF
    .quad 0x00AF9A000000FFFF
    .quad 0x00CF92000000FFFF
ap_gdt_end:

ap_gdtr:
    .word ap_gdt_end - ap_gdt - 1
    .long 0

ap_jump32_ptr:
    .long 0
    .word AP_GDT_CODE32_SEL

ap_jump64_ptr:
    .long 0
    .word AP_GDT_CODE64_SEL

SMP_ap_trampoline_end:
