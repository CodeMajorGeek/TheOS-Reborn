#include <Device/TTY.h>
#include <CPU/IDT.h>

void k_entry(const void* multiboot_info)
{
    TTY_init();
    IDT_init();
    
    uint64_t* err = 0xFFFFFFFFF;
    err[0] = 0xDEADBEEF;

    const char* str = "\tJe suis un \ntest !\r\n";

    TTY_puts(str);
}