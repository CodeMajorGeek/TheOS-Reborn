#define ASM_FILE
#include <Multiboot2/multiboot2.h>

.set HEADER_LEN,        mb2_hdr_end - mb2_hdr
.set CHECKSUM,          -(MULTIBOOT2_HEADER_MAGIC + MULTIBOOT_ARCHITECTURE_I386 + HEADER_LEN)

.set TYPE,              0
.set FLAGS,             0
.set TAG_LEN,           8

.set ID_FLAG,           1 << 21
.set LM_BIT,            1 << 29
.set PAE_FLAG,          1 << 5
.set EFER_MSR,          1 << 8
.set PAGING_FLAG,       (1 << 31) | (1 << 0)

.set CPUID_FLAG_MSR,    1 << 5

# Access bits.
.set PRESENT,           1 << 7
.set NOT_SYS,           1 << 4
.set EXEC,              1 << 3
.set DC,                1 << 2
.set RW,                1 << 1
.set ACCESSED,          1 << 0
.set RING_USER,         1 << 5
 
# Flags bits.
.set GRAN_4K,           1 << 7
.set SZ_32,             1 << 6
.set LONG_MODE,         1 << 5

.section .multiboot
.align MULTIBOOT_HEADER_ALIGN

# Start our kernel with the MULTIBOOT2 header.
mb2_hdr:
    .long MULTIBOOT2_HEADER_MAGIC
    .long MULTIBOOT_ARCHITECTURE_I386
    .long HEADER_LEN
    .long CHECKSUM

    .short TYPE
    .short FLAGS
    .long MULTIBOOT_TAG_ALIGN
mb2_hdr_end:

.code32
.section .text
.globl multiboot_entry

# Execute our first loader code segment starting at multiboot_entry.
multiboot_entry:   
    movl $kernel_stack_top, %esp    # Let's define our stack pointer where the kernel stack will be.
    movl $0, %ebp

    movl %ebx, %edi                 # Keep the Multiboot2 info ptr for the k_entry function.

    calll check_multiboot           # Check if this code was loaded using a MULTIBOOT2 compliant bootloader ("ERR: 0" show up if not).
    calll check_cpuid               # Check if the CPUID operand is avaliable on this CPU ("ERR: 1" show up if not).
    calll check_msr                 # Check if the MSR is avaliable on this CPU ("ERR: 2" show up if not).
    calll check_long_mode           # Check if the long mode is supported on this CPU to determine if it's a 64 bits on ("ERR: 3" show up if not).

    calll setup_page_tables         # It's an x86_64 CPU architecture so let's setup the 4 tables for paging.
    calll enable_paging             # Let's enable the paging on the CPU.

    lgdtl (gdt64.pointer)           # Now setup the GDTs to load our different segment of memory.

    jmpl $gdt64.kernel_code,$long_mode_entry    # Let's do a far jump to long mode (64 bits) at the kernel code segment.

    jmp end                                     # Should not be reached !

check_multiboot:
    cmpl $MULTIBOOT2_BOOTLOADER_MAGIC, %eax # Just have to compare eax with the MULTIBOOT2 bootloader magic number.
    jne check_multiboot.no_multiboot        # If not equal, this code was loaded by a not MULTIBOOT2 compliant bootloader, then print show "ERR: 0".
    retl
check_multiboot.no_multiboot:
    movb $'0', %al
    jmp error

check_cpuid:
    # Let's put the actual EFLAGS into eax.
    pushfl
    popl %eax

    movl %eax, %ecx     # Make a copy of it into ecx.

    xorl $ID_FLAG, %eax # Invert the ID_FLAG bit of it.

    # Push back the modified EFLAGS.
    pushl %eax
    popfl

    # Pop the EFLAGS back into eax.
    pushfl
    popl %eax

    # Restore back the original ELFAGS configuration stored in ecx.
    pushl %ecx
    popfl
    
    # If they are the same, it's mean no changes so CPUID is not avaliable on this CPU, then show "ERR: 1".
    xorl %ecx, %eax
    jz check_cpuid.no_cpuid
    retl
check_cpuid.no_cpuid:
    movb $'1', %al
    jmp error

check_msr:
    movl $1, %eax               # Use the CPUID code 1.
    cpuid
    andl $CPUID_FLAG_MSR, %edx  # Check if edx have the MSR FLAG BIT set.
    jz check_msr.no_msr         # If not, then show up "ERR: 2".
    retl
check_msr.no_msr:
    movb $'2', %al
    jmp error

check_long_mode:
    # We have to do 2 tests using CPUID and architecture specific arguments to determine if this CPU support long mode.
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb check_long_mode.no_long_mode

    movl $0x80000001, %eax
    cpuid
    testl $LM_BIT, %edx             # Check if the LM_BIT is set, if it isn't, then show "ERR: 2".
    jz check_long_mode.no_long_mode
    retl
check_long_mode.no_long_mode:
    movb $'3', %al
    jmp error

setup_page_tables:
    movl $pdp, %eax                 # Let's store the pdp table offset into eax.
    orl $0b11, %eax                 # Make this entry present and writable.
    movl %eax, (pml4)               # Now move the pdp offset into the first pml4 table entry.

    movl $pd, %eax                  # Let's store the pd table offset into eax.
    orl $0b11, %eax                 # Make this entry present and writable.
    movl %eax, (pdp)                # Now move the pd offset into the first pdp table entry.

    movl $0, %ecx                   # Let's define a counter start at 0 to loop 512 times.
setup_page_tables.map_pd_table:
    # Let's map pd entry to a huge page that starts at address 2MiB * ecx (counter).
    movl $0x200000, %eax            # 2 MiB page.
    mull %ecx                       # The start address of the ecx-th page.  
    orl $0b10000011, %eax           # Make it present, writable and huge.
    movl %eax, pd(,%ecx,8)          # Map this entry.

    incl %ecx                       # Increment the counter
    cmpl $512, %ecx                 # Countinue while ecx < 512.
    jne setup_page_tables.map_pd_table

    retl

enable_paging:
    movl $pml4, %eax                # Store the pml4 table offset into eax.
    movl %eax, %cr3                 # Transfer it into the cr3 special register (So the CPU know where we store our pages data).

    movl %cr4, %eax                 # Store the cr4 special register into eax.
    orl $PAE_FLAG, %eax             # Flip the PAE bit to 1.
    movl %eax, %cr4                 # Push back to cr4.

    movl $0xC0000080, %ecx          # Move the argument for seting model specific register.
    rdmsr                           # Read our model specific register.
    orl $EFER_MSR, %eax             # Flip the EFFER_MSR bit to 1.
    wrmsr                           # Write our model specific register.

    movl %cr0, %eax                 # Store the cr0 special register into eax.
    orl $PAGING_FLAG, %eax          # Flip the PAGING bit to 1.
    movl %eax, %cr0                 # Push back to cr0.

    retl

error:
    # Just print "ERR: [al]" to show up error codes.
    movl $0x4f524f45, 0xb8000
    movl $0x4f3a4f52, 0xb8004
    movl $0x4f204f20, 0xb8008
    movb %al, 0xb800a
    jmp end

.section .bss
.align 4096

.globl pml4
.globl pdp
.globl pd

.globl kernel_heap_bottom
.globl kernel_heap_top

kernel_heap_top:
    .space 4096 * 8 # 32KiB of kernel heap.
kernel_heap_bottom:

# We store our pages into the bss section (3 page of 4KiB with an 4KiB alignement).
pml4:
    .zero 4096
pdp:
    .zero 4096
pd:
    .zero 4096
kernel_stack_bottom:
    .space 4096 * 16 # 64KiB of kernel stack.
kernel_stack_top:

.section .rodata
.align 8

# We define our GDTs in the rodata section.
gdt64:
# Null segment.
    .quad 0

# Kernel code segment.
.equ gdt64.kernel_code, . - gdt64           # 0x08
    .long 0xFFFF                            # limit & base 0 to 15 bits.
    .byte 0                                 # base 16 to 23 bits.
    .byte PRESENT | NOT_SYS | EXEC | RW     # access byte.
    .byte GRAN_4K | LONG_MODE | 0xF         # high 4 bits for flags and lower 4 bits for limit.
    .byte 0                                 # base last 24 to 31 bits.

# Kernel data segment.
.equ gdt64.kernel_data, . - gdt64           # 0x10
    .long 0xFFFF
    .byte 0
    .byte PRESENT | NOT_SYS | RW
    .byte GRAN_4K | SZ_32 | 0xF
    .byte 0

# User code segment.
.equ gdt64.user_code, . - gdt64             #0x18
    .long 0xFFFF
    .byte 0
    .byte PRESENT | NOT_SYS | EXEC | RING_USER
    .byte LONG_MODE
    .byte 0

# User code segment.
.equ gdt64.user_data, . - gdt64             #0x20
    .long 0xFFFF
    .byte 0
    .byte PRESENT | NOT_SYS | RW | RING_USER
    .byte LONG_MODE
    .byte 0

gdt64.pointer:
    .word . - gdt64 - 1
    .quad gdt64

.section .text
.code64

.extern k_entry

long_mode_entry:    # Finally, we can do some stuff into long mode !
    # Let's zeroing all the segment registers (ax, ss, ds, es, fs and gs).
    movw $0, %ax
    movw %ax, %ss
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushq %rdi      # Let's push the MULTIBOOT2 multiboot_info offset onto the stack to parse it in the C as argument.
    callq k_entry   # Now we can call our k_entry C function.
    
end:                # This part should be never be reached !
    cli
    hlt
    jmp end
