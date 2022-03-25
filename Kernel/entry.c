#include <Multiboot2/multiboot2.h>

#include <Device/TTY.h>
#include <Device/COM.h>
#include <CPU/IDT.h>
#include <Device/PIT.h>
#include <CPU/ISR.h>
#include <CPU/IO.h>
#include <Device/APIC.h>
#include <Debug/logger.h>
#include <CPU/ACPI.h>
#include <Memory/Memory.h>
#include <CPU/UserMode.h>

#include <stdio.h>

void read_multiboot2_info(const void*);

static uint32_t mem_lower;
static uint32_t mem_upper;

__attribute__((__noreturn__)) void k_entry(const void* mbt2_info)
{
    TTY_init();
    logger_init();

    read_multiboot2_info(mbt2_info);

    IDT_init();
    PIT_init();

    kmem_init();

    
    void* ptr1 = kmalloc(128);
    printf("Addresse ptr1: 0x%H\n\n", ptr1);

    void* ptr2 = kmalloc(3);
    printf("Addresse ptr2: 0x%H\n\n", ptr2);

    kfree(ptr1);

    void* ptr3 = kmalloc(90);
    printf("Addresse ptr3: 0x%H\n", ptr3);
    

    kputs(KDEBUG, APIC_check() ? "TRUE" : "FALSE");

    puts("Je suis un test !\n");

    // switch_to_user_mode();

    while (TRUE)
        __asm__ __volatile__("hlt");

    __builtin_unreachable();
}

void read_multiboot2_info(const void* mbt2_info)
{
    struct multiboot_tag *tag;
    unsigned size;

    size = *(unsigned *) mbt2_info;
    for (tag = (struct multiboot_tag *) (mbt2_info + 8);
       tag->type != MULTIBOOT_TAG_TYPE_END;
       tag = (struct multiboot_tag *) ((multiboot_uint8_t *) tag 
                                       + ((tag->size + 7) & ~7)))
    {
        switch (tag->type)
        {
            case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO:
                struct multiboot_tag_basic_meminfo* meminfo = (struct multiboot_tag_basic_meminfo*) tag;
                mem_lower = meminfo->mem_lower;
                mem_upper = meminfo->mem_upper;
                break;
        }
    }
}