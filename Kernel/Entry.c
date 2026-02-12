#include <Multiboot2/multiboot2.h>

#include <Device/Keyboard.h>
#include <FileSystem/ext4.h>
#include <Debug/Logger.h>
#include <Debug/KDebug.h>
#include <CPU/UserMode.h>
#include <CPU/Syscall.h>
#include <Device/PIC.h>
#include <Device/TTY.h>
#include <Device/COM.h>
#include <Device/HPET.h>
#include <Device/PIT.h>
#include <Device/RTC.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>
#include <Memory/KMem.h>
#include <Task/Task.h>
#include <CPU/APIC.h>
#include <CPU/ACPI.h>
#include <CPU/SMP.h>
#include <CPU/IDT.h>
#include <CPU/ISR.h>
#include <CPU/PCI.h>
#include <CPU/IO.h>
#include <CPU/x86.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void read_multiboot2_info(const void*);

extern void* kernel_start;
extern void* kernel_end;

extern void* kernel_stack_top;
extern void* kernel_stack_bottom;

static APIC_MADT_t* MADT = NULL;
static const uint32_t BSP_TIMER_HZ = 100;
static const uint32_t PIT_TIMER_HZ = 1000;

uintptr_t ROOT_DEV = 1;

__attribute__((__noreturn__)) void k_entry(const void* mbt2_info)
{
    TTY_init();
    logger_init();
    kdebug_init();
    kdebug_puts("[BOOT] k_entry start\n");

    PMM_init((uint64_t) &kernel_start, (uint64_t) &kernel_end);
    printf("Kernel start at 0x%llX and end at 0x%llX\n", &kernel_start, &kernel_end);
    kdebug_puts("[BOOT] PMM base set\n");
    
    read_multiboot2_info(mbt2_info);
    kdebug_puts("[BOOT] multiboot parsed\n");

    VMM_map_kernel();
    kdebug_puts("[BOOT] VMM kernel map done\n");
    // VMM_map_userland_stack();

    MADT = (APIC_MADT_t*) ACPI_get_table(ACPI_APIC_SIGNATURE);
    kdebug_puts("[BOOT] ACPI MADT fetched\n");

    VMM_hardware_mapping();
    VMM_load_cr3();
    kdebug_puts("[BOOT] CR3 loaded\n");

    IDT_init();
    kdebug_puts("[BOOT] IDT loaded\n");

    if (APIC_check())
    {
        kdebug_puts("[BOOT] APIC supported\n");
        PIC_disable();
        kdebug_puts("[BOOT] PIC disabled\n");
        APIC_init(MADT);
        kdebug_puts("[BOOT] APIC init done\n");
        APIC_enable();
        kdebug_puts("[BOOT] APIC enabled\n");
    }
    else
    {
        kdebug_puts("[BOOT] APIC skipped (PIC mode)\n");
    }

    PCI_init();
    kdebug_puts("[BOOT] PCI scanned\n");

    Keyboard_init();
    kdebug_puts("[BOOT] keyboard init\n");
    Syscall_init();
    kdebug_puts("[BOOT] syscall init\n");

    static ext4_fs_t fs;
    HBA_PORT_t* root_port = AHCI_get_device(0);

    if (root_port)
    {
        if (ext4_mount(&fs, root_port))
        {
            ext4_set_active(&fs);
            if (!ext4_list_root(&fs))
                printf("Unable to list ext4 root directory\n");

            const char* msg = "Hello from ext4!\n";
            if (ext4_create_file(&fs, "hello.txt", (const uint8_t*) msg, strlen(msg)))
                printf("hello.txt created !\n");
            else
                printf("Unable to create hello.txt\n");

            if (!ext4_list_root(&fs))
                printf("Unable to list ext4 root directory\n");

            uint8_t* data = NULL;
            size_t size = 0;
            if (ext4_read_file(&fs, "test.txt", &data, &size))
            {
                printf("cat test.txt:\n%s\n", data);
                kfree(data);
            }
            else
            {
                printf("Unable to read hello.txt\n");
            }
        }
        else
        {
            printf("Unable to mount ext4 filesystem\n");
        }
    }
    else
    {
        printf("AHCI: no SATA device detected\n");
    }

    task_init((uintptr_t) &kernel_stack_top);
    kdebug_puts("[BOOT] task init\n");

    if (APIC_is_enabled())
    {
        if (SMP_init())
            kdebug_printf("[BOOT] SMP online cpus=%u\n", (unsigned) SMP_get_online_cpu_count());
        else
            kdebug_puts("[BOOT] SMP bring-up failed\n");
    }

    bool hpet_ready = false;
    bool pit_started = false;

    if (APIC_is_enabled())
    {
        hpet_ready = HPET_init();
        if (hpet_ready)
            kdebug_puts("[BOOT] HPET init (LAPIC calibration source)\n");
    }

    if (!APIC_is_enabled() || !hpet_ready)
    {
        ISR_set_tick_source(TICK_SOURCE_PIT_IOAPIC, PIT_TIMER_HZ);
        PIT_init();
        pit_started = true;

        if (APIC_is_enabled())
            kdebug_puts("[BOOT] PIT init (LAPIC calibration fallback)\n");
        else
            kdebug_puts("[BOOT] PIT init\n");
    }

    sti();
    kdebug_puts("[BOOT] interrupts enabled\n");

    if (APIC_is_enabled())
    {
        if (APIC_timer_init_bsp(BSP_TIMER_HZ))
        {
            ISR_set_tick_source(TICK_SOURCE_LAPIC_TIMER, BSP_TIMER_HZ);
            if (hpet_ready)
                kdebug_puts("[BOOT] LAPIC timer BSP active, HPET calibrated\n");
            else
                kdebug_puts("[BOOT] LAPIC timer BSP active, PIT stopped\n");
        }
        else
        {
            if (!pit_started)
            {
                ISR_set_tick_source(TICK_SOURCE_PIT_IOAPIC, PIT_TIMER_HZ);
                PIT_init();
                pit_started = true;
            }
            ISR_set_tick_source(TICK_SOURCE_PIT_IOAPIC, PIT_TIMER_HZ);
            kdebug_puts("[BOOT] LAPIC timer BSP init failed, PIT kept active\n");
        }
    }

    RTC_t rtc;
    RTC_read(&rtc);
    printf("%d:%d:%d %s %d/%d/%d\n", rtc.hours, rtc.minutes, rtc.seconds, rtc.weekday, rtc.month_day, rtc.month, rtc.year);

    switch_to_usermode();
    
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
                        printf("MMAP avaliable found at addr 0x%llX with size of 0x%llX !\n", mmap->addr, mmap->len);
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
