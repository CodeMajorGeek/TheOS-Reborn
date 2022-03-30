ENTRY(multiboot_entry)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    kernel_start = .;
    kernel_mem = 1M + kernel_start;

    . = kernel_mem + SIZEOF_HEADERS;

    .text :
    {
        KEEP(*(.multiboot))
        *(.text)
    }

    .rodata :
    {
        *(.rodata)
    }

    .bss : ALIGN(4K)
    {
        *(.bss)
    }

    .data :
    {
        *(.data)
    }

    _end = .;
    kernel_end = _end;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.gnu.build-id)
    }
}