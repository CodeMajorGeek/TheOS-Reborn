#include <Device/TTY.h>
#include <CPU/IDT.h>
#include <Device/PIT.h>
#include <CPU/ISR.h>
#include <CPU/IO.h>

#include <Device/APIC.h>

void k_entry(const void* multiboot_info)
{
    TTY_init();
    IDT_init();

    PIT_init();

    TTY_puts(APIC_check() ? "TRUE" : "FALSE");

    const char* str = "Je suis un test !\n";
    TTY_puts(str);

    while (TRUE)
        __asm__ __volatile__("hlt");
}