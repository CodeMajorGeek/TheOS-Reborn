ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x0000000040000000;

    .text ALIGN(4K) :
    {
        *(.text*)
    } :text

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
        *(.eh_frame*)
    } :text

    .data ALIGN(4K) :
    {
        *(.data*)
    } :data

    .bss ALIGN(4K) (NOLOAD) :
    {
        *(COMMON)
        *(.bss*)
    } :data
}
