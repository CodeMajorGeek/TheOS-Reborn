#include <Multiboot2/multiboot2.h>

#include <Device/Keyboard.h>
#include <FileSystem/ext4.h>
#include <Debug/Logger.h>
#include <CPU/UserMode.h>
#include <CPU/Syscall.h>
#include <Device/PIC.h>
#include <Device/TTY.h>
#include <Device/COM.h>
#include <Device/PIT.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>
#include <Task/Task.h>
#include <CPU/APIC.h>
#include <CPU/ACPI.h>
#include <CPU/IDT.h>
#include <CPU/ISR.h>
#include <CPU/PCI.h>
#include <CPU/IO.h>

#include <stdio.h>

void read_multiboot2_info(const void*);

extern void* kernel_start;
extern void* kernel_end;

extern void* kernel_stack_top;
extern void* kernel_stack_bottom;

static APIC_MADT_t* MADT = NULL;

uintptr_t ROOT_DEV = 1;

__attribute__((__noreturn__)) void k_entry(const void* mbt2_info)
{
    TTY_init();
    logger_init();

    PMM_init((uint64_t) &kernel_start, (uint64_t) &kernel_end);
    printf("Kernel start at 0x%X%X and end at 0x%X%X\n",
        (unsigned) ((uint64_t) &kernel_start >> 32), (unsigned) ((uint64_t) &kernel_start & 0xffffffff),
        (unsigned) ((uint64_t) &kernel_end >> 32), (unsigned) ((uint64_t) &kernel_end & 0xffffffff));
    
    read_multiboot2_info(mbt2_info);

    VMM_map_kernel();

    MADT = (APIC_MADT_t*) ACPI_get_table(ACPI_APIC_SIGNATURE);

    VMM_hardware_mapping();
    VMM_load_cr3();

    IDT_init();

    if (APIC_check())
    {
        PIC_disable();
        APIC_init(MADT);
        APIC_enable();
    }

    PCI_init();

    Keyboard_init();
    Syscall_init();

    task_init((uintptr_t) &kernel_stack_bottom);
    PIT_init();

    printf("Je suis un petit test ! :)\n");

    // switch_to_usermode();
    
    while (TRUE)
        __asm__ __volatile__("nop");

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
                        printf("MMAP avaliable found at addr 0x%X%X with size of 0x%X%X !\n",
                            (unsigned) (mmap->addr >> 32),
                            (unsigned) (mmap->addr & 0xffffffff),
                            (unsigned) (mmap->len >> 32),
                            (unsigned) (mmap->len & 0xffffffff));
                        PMM_init_region(mmap->addr, mmap->len);
                    }   
                }
                break;
            case MULTIBOOT_TAG_TYPE_ACPI_OLD:
                struct multiboot_tag_old_acpi* old_acpi = (struct multiboot_tag_old_acpi*) tag;
                if (ACPI_RSDP_old_check(old_acpi->rsdp))
                    ACPI_init_RSDT((ACPI_RSDP_descriptor10_t*) old_acpi->rsdp);
                break;
            case MULTIBOOT_TAG_TYPE_ACPI_NEW:
                struct multiboot_tag_new_acpi* new_acpi = (struct multiboot_tag_new_acpi*) tag;
                if (ACPI_RSDP_new_check(new_acpi->rsdp))
                    ACPI_init_XSDT((ACPI_RSDP_descriptor20_t*) new_acpi->rsdp);
                break;
        }
    }
}