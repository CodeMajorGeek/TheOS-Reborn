#include <Device/TTY.h>
#include <CPU/IDT.h>

void k_entry(const void* multiboot_info)
{
    TTY_init();
    IDT_init();
    
    const char* str = "\tJe suis un \ntest !\r\n";

    TTY_puts(str);

}