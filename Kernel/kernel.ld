ENTRY(multiboot_entry)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 1M;

    .text : ALIGN(4K)
    {
        KEEP(*(.multiboot))
        *(.text)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata)
    }
    
    .data : ALIGN(4K)
    {
        *(.data)
    }

    .bss : ALIGN(4K)
    {
        *(.bss)
    }

    _end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.gnu.build-id)
    }
}