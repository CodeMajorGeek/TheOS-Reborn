#include <Multiboot2/multiboot2.h>

#include <Device/TTY.h>
#include <Device/COM.h>
#include <CPU/IDT.h>
#include <Device/PIT.h>
#include <CPU/ISR.h>
#include <CPU/IO.h>
#include <Device/APIC.h>
#include <Debug/Logger.h>
#include <CPU/ACPI.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>
#include <CPU/UserMode.h>
#include <Device/Keyboard.h>
#include <Storage/ATA.h>
#include <Device/AHCI.h>
#include <CPU/PCI.h>
#include <CPU/Syscall.h>

#include <stdio.h>

void read_multiboot2_info(const void*);

extern void* kernel_start;
extern void* kernel_end;

__attribute__((__noreturn__)) void k_entry(const void* mbt2_info)
{
    TTY_init();
    logger_init();

    PMM_init((uint64_t) &kernel_start, (uint64_t) &kernel_end);
    printf("Kernel start at 0x%H%H and end at 0x%H%H\n",
        (unsigned) ((uint64_t) &kernel_start >> 32), (unsigned) ((uint64_t) &kernel_start & 0xffffffff),
        (unsigned) ((uint64_t) &kernel_end >> 32), (unsigned) ((uint64_t) &kernel_end & 0xffffffff));

    PMM_init_AHCI();

    read_multiboot2_info(mbt2_info);

    IDT_init();
    PIT_init();

    VMM_map_kernel();
    VMM_identity_mapping();
    VMM_load_cr3();

    printf("Je suis un petit test ! :)\n");

    ATA_init();
    PCI_init();

    Keyboard_init();

    Syscall_init();
    // switch_to_usermode();

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
            case MULTIBOOT_TAG_TYPE_MMAP:
                multiboot_memory_map_t* mmap;
      
                for (mmap = ((struct multiboot_tag_mmap*) tag)->entries; (multiboot_uint8_t*) mmap < (multiboot_uint8_t*) tag + tag->size;
                    mmap = (multiboot_memory_map_t*) ((unsigned long) mmap + ((struct multiboot_tag_mmap*) tag)->entry_size))
                {
                    if (mmap->type == MULTIBOOT_MEMORY_AVAILABLE)
                    {
                        printf("MMAP avaliable found at addr 0x%H%H with size of 0x%H%H !\n",
                            (unsigned) (mmap->addr >> 32),
                            (unsigned) (mmap->addr & 0xffffffff),
                            (unsigned) (mmap->len >> 32),
                            (unsigned) (mmap->len & 0xffffffff));
                        PMM_init_region(mmap->addr, mmap->len);
                    }   
                }
                break;
        }
    }
}