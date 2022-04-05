ENTRY(multiboot_entry)

PHDRS
{
    boot PT_LOAD;
    text PT_LOAD;
    data PT_LOAD;
    got PT_LOAD;
    bss PT_LOAD;
}

SECTIONS
{
    PROVIDE(kernel_start = 1M);

    . = kernel_start + SIZEOF_HEADERS;

    .boot ALIGN(4K) : AT (ADDR(.boot))
    {
        KEEP(*(.multiboot))
    } :boot

    .text ALIGN(4K) : AT (ADDR(.text))
    {
        *(.text)
    } :text

    .rodata ALIGN(4K) : AT(ADDR(.rodata))
    {
        *(.rodata*)
    } :data

    .data ALIGN(4K) : AT(ADDR(.data))
    {
        *(.data*)
    } :data

    .got ALIGN(4K) : AT(ADDR(.got))
    {
        *(.got*)
    } :got

    .bss ALIGN(4K) (NOLOAD) : AT(ADDR(.bss))
    {
        *(COMMON)
        *(.bss)
    } :bss
    
    PROVIDE(kernel_end = .);

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.gnu.build-id)
    }
}