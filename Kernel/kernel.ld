ENTRY(limine_entry)

KERNEL_PHYS_BASE = 0x00100000;
KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;

PHDRS
{
    text PT_LOAD;
    data PT_LOAD;
    got PT_LOAD;
    bss PT_LOAD;
}

SECTIONS
{
    PROVIDE(kernel_phys_start = KERNEL_PHYS_BASE);
    PROVIDE(kernel_virt_start = KERNEL_VIRT_BASE + KERNEL_PHYS_BASE);
    PROVIDE(kernel_start = kernel_phys_start);

    . = KERNEL_VIRT_BASE + KERNEL_PHYS_BASE + SIZEOF_HEADERS;

    .text ALIGN(4K) : AT (ADDR(.text) - KERNEL_VIRT_BASE)
    {
        *(.text)
        *(.ap_trampoline)
    } :text

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE)
    {
        *(.rodata*)
    } :data

    .limine_requests ALIGN(8) : AT(ADDR(.limine_requests) - KERNEL_VIRT_BASE)
    {
        KEEP(*(.limine_requests_start_marker))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end_marker))
    } :data

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRT_BASE)
    {
        *(.data*)
    } :data

    .got ALIGN(4K) : AT(ADDR(.got) - KERNEL_VIRT_BASE)
    {
        *(.got*)
    } :got

    .bss ALIGN(4K) (NOLOAD) : AT(ADDR(.bss) - KERNEL_VIRT_BASE)
    {
        *(COMMON)
        *(.bss)
    } :bss

    . = ALIGN(4K);
    PROVIDE(kernel_virt_end = .);
    PROVIDE(kernel_phys_end = kernel_virt_end - KERNEL_VIRT_BASE);
    PROVIDE(kernel_end = kernel_phys_end);

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.gnu.build-id)
    }
}
