#include <Device/TTY.h>
#include <Device/COM.h>
#include <CPU/IDT.h>
#include <Device/PIT.h>
#include <CPU/ISR.h>
#include <CPU/IO.h>

#include <stdio.h>
#include <Device/APIC.h>
#include <Debug/logger.h>

void k_entry(const void* multiboot_info)
{
    TTY_init();
    logger_init();
    IDT_init();

    PIT_init();

    kputs(KDEBUG, APIC_check() ? "TRUE" : "FALSE");

    puts("Je suis un test !\n");

    while (TRUE)
        __asm__ __volatile__("hlt");
}