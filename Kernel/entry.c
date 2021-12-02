#include <Device/TTY.h>
#include <Device/COM.h>
#include <CPU/IDT.h>
#include <Device/PIT.h>
#include <CPU/ISR.h>
#include <CPU/IO.h>
#include <Device/APIC.h>
#include <Debug/logger.h>
#include <Multiboot2/multiboot2_info.h>
#include <CPU/ACPI.h>

#include <stdio.h>

extern struct multiboot_tag_new_acpi* new_acpi_tag;

__attribute__((__noreturn__)) void k_entry(const void* multiboot_info)
{
    TTY_init();
    logger_init();

    IDT_init();
    PIT_init();

    Multiboot2_info_read(multiboot_info);

    kputs(KDEBUG, APIC_check() ? "TRUE" : "FALSE");
    kputs(KDEBUG, ACPI_RSDP_check(new_acpi_tag) ? "TRUE" : "FALSE");
    kputs(KDEBUG, ACPI_SDT_check((void*) new_acpi_tag->rsdp) ? "TRUE" : "FALSE");

    puts("Je suis un test !\n");

    while (TRUE)
        __asm__ __volatile__("hlt");

    __builtin_unreachable();
}