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

    .tdata ALIGN(16) :
    {
        PROVIDE_HIDDEN(__theos_tls_tdata_start = .);
        *(.tdata*)
        PROVIDE_HIDDEN(__theos_tls_tdata_end = .);
    } :data

    .tbss ALIGN(16) (NOLOAD) :
    {
        *(.tbss*)
        *(.tcommon)
        PROVIDE_HIDDEN(__theos_tls_tbss_end = .);
    } :data

    PROVIDE_HIDDEN(__theos_tls_align = MAX(16, MAX(ALIGNOF(.tdata), ALIGNOF(.tbss))));

    .bss ALIGN(4K) (NOLOAD) :
    {
        *(COMMON)
        *(.bss*)
    } :data
}
