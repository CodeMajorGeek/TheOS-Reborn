#include <Device/TTY.h>
#include <Device/COM.h>
#include <CPU/IDT.h>
#include <Device/PIT.h>
#include <CPU/ISR.h>
#include <CPU/IO.h>

void k_entry(const void* multiboot_info)
{
    TTY_init();
    IDT_init();

    PIT_init();

    const char* str = "Je suis un test !\n";
    TTY_puts(str);

    COM_init();
    COM_puts(str);

    while (TRUE)
        __asm__ __volatile__("hlt");
}