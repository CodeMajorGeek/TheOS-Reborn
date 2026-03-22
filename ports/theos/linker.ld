ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
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
        *(.eh_frame_hdr*)
        *(.gcc_except_table*)
    } :text

    .plt ALIGN(16) :
    {
        *(.plt*)
    } :text

    .hash ALIGN(8) :
    {
        *(.hash)
    } :text

    .gnu.hash ALIGN(8) :
    {
        *(.gnu.hash)
    } :text

    .dynsym ALIGN(8) :
    {
        *(.dynsym)
    } :text

    .dynstr ALIGN(8) :
    {
        *(.dynstr)
    } :text

    /* Keep text/data PT_LOAD pages disjoint to avoid X/W permission conflicts. */
    . = ALIGN(4K);

    .rela.dyn ALIGN(8) :
    {
        *(.rela.dyn)
    } :data

    .rela.plt ALIGN(8) :
    {
        *(.rela.plt)
    } :data

    .dynamic ALIGN(8) :
    {
        *(.dynamic)
    } :data :dynamic

    .got ALIGN(8) :
    {
        *(.got*)
    } :data

    .data ALIGN(4K) :
    {
        *(.data*)
    } :data

    .tdata ALIGN(16) :
    {
        __theos_tls_tdata_start = .;
        *(.tdata*)
        __theos_tls_tdata_end = .;
    } :data

    .tbss ALIGN(16) (NOLOAD) :
    {
        *(.tbss*)
        *(.tcommon)
        __theos_tls_tbss_end = .;
    } :data

    __theos_tls_align = MAX(16, MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));

    .bss ALIGN(4K) (NOLOAD) :
    {
        *(COMMON)
        *(.bss*)
    } :data
}
