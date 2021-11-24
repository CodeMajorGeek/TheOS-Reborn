#include <Device/TTY.h>

void k_entry(const void* multiboot_info)
{
    TTY_init();
    
    const char* str = "\tJe suis un \ntest !\r\n";

    // TTY_puts(str);

}