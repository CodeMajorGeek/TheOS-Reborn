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
#include <CPU/NUMA.h>
#include <CPU/IDT.h>
#include <CPU/GDT.h>
#include <CPU/ISR.h>
#include <CPU/FPU.h>
#include <CPU/PCI.h>
#include <CPU/IO.h>
#include <CPU/x86.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void read_multiboot2_info(const void*);

extern void* kernel_phys_start;
extern void* kernel_phys_end;
extern void* kernel_virt_start;
extern void* kernel_virt_end;

extern void* kernel_stack_top;
extern void* kernel_stack_bottom;

static APIC_MADT_t* MADT = NULL;
static const uint32_t BSP_TIMER_HZ = 100;
static const uint32_t PIT_TIMER_HZ = 1000;
static TTY_framebuffer_info_t boot_framebuffer = { 0 };
static bool boot_framebuffer_available = false;
static bool boot_font_preloaded = false;

#define THEOS_PSF2_FONT_PATH "/system/fonts/ter-powerline-v14n.psf"

#ifdef THEOS_EARLY_PSF2_EMBEDDED
extern const uint8_t _binary_builtin_console_psf_start[];
extern const uint8_t _binary_builtin_console_psf_end[];
#endif

static void boot_load_early_psf2(void)
{
#ifdef THEOS_EARLY_PSF2_EMBEDDED
    size_t font_size = (size_t) (_binary_builtin_console_psf_end - _binary_builtin_console_psf_start);
    if (font_size == 0)
        return;

    if (TTY_load_psf2(_binary_builtin_console_psf_start, font_size))
    {
        boot_font_preloaded = true;
        kdebug_printf("[TTY] early embedded PSF2 loaded (%llu bytes)\n",
                      (unsigned long long) font_size);
    }
    else
    {
        kdebug_puts("[TTY] early embedded PSF2 load failed\n");
    }
#endif
}

uintptr_t ROOT_DEV = 1;

__attribute__((__noreturn__)) void k_entry(const void* mbt2_info)
{
    TTY_init();
    logger_init();
    kdebug_init();
    kdebug_puts("[BOOT] k_entry start\n");

    PMM_init((uintptr_t) &kernel_phys_start,
             (uintptr_t) &kernel_phys_end,
             (uintptr_t) &kernel_virt_start,
             (uintptr_t) &kernel_virt_end);
    printf("Kernel phys [0x%llX..0x%llX) virt [0x%llX..0x%llX)\n",
           (unsigned long long) (uintptr_t) &kernel_phys_start,
           (unsigned long long) (uintptr_t) &kernel_phys_end,
           (unsigned long long) (uintptr_t) &kernel_virt_start,
           (unsigned long long) (uintptr_t) &kernel_virt_end);
    kdebug_puts("[BOOT] PMM base set\n");
    
    read_multiboot2_info(mbt2_info);
    kdebug_puts("[BOOT] multiboot parsed\n");
    boot_load_early_psf2();

    VMM_map_kernel();
    kdebug_puts("[BOOT] VMM kernel map done\n");
    // VMM_map_userland_stack();

    MADT = (APIC_MADT_t*) ACPI_get_table(ACPI_APIC_SIGNATURE);
    kdebug_puts("[BOOT] ACPI MADT fetched\n");

    VMM_hardware_mapping();
    VMM_load_cr3();
    kdebug_puts("[BOOT] CR3 loaded\n");
    GDT_load_kernel_segments();
    kdebug_puts("[BOOT] GDT reloaded\n");

    if (boot_framebuffer_available)
    {
        if (TTY_init_framebuffer(&boot_framebuffer))
            kdebug_puts("[BOOT] framebuffer init done\n");
        else
            kdebug_puts("[BOOT] framebuffer init failed\n");
    }
    else
    {
        kdebug_puts("[BOOT] framebuffer unavailable\n");
    }

    IDT_init();
    kdebug_puts("[BOOT] IDT loaded\n");

    if (!FPU_init_cpu(0))
    {
        kdebug_puts("[BOOT] FPU init failed\n");
        abort();
    }
    kdebug_puts("[BOOT] FPU init done\n");

    if (APIC_check())
    {
        kdebug_puts("[BOOT] APIC supported\n");
        PIC_disable();
        kdebug_puts("[BOOT] PIC disabled\n");
        APIC_init(MADT);
        kdebug_puts("[BOOT] APIC init done\n");
        NUMA_init();
        kdebug_printf("[BOOT] NUMA %s nodes=%u\n",
                      NUMA_is_available() ? "enabled" : "disabled",
                      NUMA_get_node_count());
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
            kdebug_puts("[BOOT] ext4 mounted\n");

            if (!boot_font_preloaded)
            {
                uint8_t* font_data = NULL;
                size_t font_size = 0;
                const char* font_path = THEOS_PSF2_FONT_PATH;
                if (ext4_read_file(&fs, font_path, &font_data, &font_size))
                {
                    if (TTY_load_psf2(font_data, font_size))
                        kdebug_printf("[TTY] loaded PSF2 from %s (%llu bytes)\n",
                                      font_path,
                                      (unsigned long long) font_size);
                    else
                        kdebug_printf("[TTY] invalid PSF2 file at %s\n", font_path);
                    kfree(font_data);
                }
                else
                {
                    kdebug_printf("[TTY] no PSF2 font found at %s\n", font_path);
                }
            }
            else
            {
                kdebug_puts("[TTY] using early embedded PSF2 (disk font load skipped)\n");
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

    VMM_drop_startup_identity_map();
    kdebug_puts("[BOOT] startup identity map dropped\n");

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
            if (SMP_get_online_cpu_count() > 1)
            {
                if (SMP_start_ap_timers())
                    kdebug_puts("[BOOT] LAPIC timer AP active on all online CPUs\n");
                else
                    kdebug_puts("[BOOT] LAPIC timer AP activation incomplete\n");
            }
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

    if (root_port && AHCI_get_irq_mode() != AHCI_IRQ_MODE_POLL)
    {
        uint8_t irq_test_buf[AHCI_SECTOR_SIZE];
        memset(irq_test_buf, 0, sizeof(irq_test_buf));

        uint64_t irq_before = AHCI_get_irq_count();
        int irq_read_rc = AHCI_sata_read(root_port, 1, 0, 1, irq_test_buf);

        uint64_t irq_after = AHCI_get_irq_count();
        for (uint32_t spin = 0; spin < 2000000U && irq_after == irq_before; spin++)
        {
            __asm__ __volatile__("pause");
            irq_after = AHCI_get_irq_count();
        }

        kdebug_printf("[AHCI] irq smoke mode=%s read_rc=%d irq_before=%llu irq_after=%llu\n",
                      AHCI_get_irq_mode_name(),
                      irq_read_rc,
                      (unsigned long long) irq_before,
                      (unsigned long long) irq_after);
    }

    RTC_t rtc;
    RTC_read(&rtc);
    printf("%d:%d:%d %s %d/%d/%d\n", rtc.hours, rtc.minutes, rtc.seconds, rtc.weekday, rtc.month_day, rtc.month, rtc.year);

    if (!UserMode_run_elf("/bin/TheApp"))
        kdebug_puts("[USER] launch failed, staying in kernel idle loop\n");

    while (TRUE)
        __asm__ __volatile__("sti\nhlt");

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
            case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
            {
                struct multiboot_tag_framebuffer* fb = (struct multiboot_tag_framebuffer*) tag;
                struct multiboot_tag_framebuffer_common* common = &fb->common;
                boot_framebuffer_available = false;

                if (common->framebuffer_type == MULTIBOOT_FRAMEBUFFER_TYPE_RGB &&
                    common->framebuffer_width != 0 &&
                    common->framebuffer_height != 0 &&
                    common->framebuffer_pitch != 0)
                {
                    boot_framebuffer.phys_addr = common->framebuffer_addr;
                    boot_framebuffer.pitch = common->framebuffer_pitch;
                    boot_framebuffer.width = common->framebuffer_width;
                    boot_framebuffer.height = common->framebuffer_height;
                    boot_framebuffer.bpp = common->framebuffer_bpp;
                    boot_framebuffer.type = common->framebuffer_type;
                    boot_framebuffer.red_field_position = fb->framebuffer_red_field_position;
                    boot_framebuffer.red_mask_size = fb->framebuffer_red_mask_size;
                    boot_framebuffer.green_field_position = fb->framebuffer_green_field_position;
                    boot_framebuffer.green_mask_size = fb->framebuffer_green_mask_size;
                    boot_framebuffer.blue_field_position = fb->framebuffer_blue_field_position;
                    boot_framebuffer.blue_mask_size = fb->framebuffer_blue_mask_size;
                    boot_framebuffer_available = true;

                    kdebug_printf("[BOOT] MB2 framebuffer addr=0x%llX %ux%u pitch=%u bpp=%u rgb=(r%u:%u g%u:%u b%u:%u)\n",
                                  (unsigned long long) boot_framebuffer.phys_addr,
                                  (unsigned int) boot_framebuffer.width,
                                  (unsigned int) boot_framebuffer.height,
                                  (unsigned int) boot_framebuffer.pitch,
                                  (unsigned int) boot_framebuffer.bpp,
                                  (unsigned int) boot_framebuffer.red_field_position,
                                  (unsigned int) boot_framebuffer.red_mask_size,
                                  (unsigned int) boot_framebuffer.green_field_position,
                                  (unsigned int) boot_framebuffer.green_mask_size,
                                  (unsigned int) boot_framebuffer.blue_field_position,
                                  (unsigned int) boot_framebuffer.blue_mask_size);
                }
                else
                {
                    kdebug_printf("[BOOT] MB2 framebuffer ignored type=%u width=%u height=%u pitch=%u\n",
                                  (unsigned int) common->framebuffer_type,
                                  (unsigned int) common->framebuffer_width,
                                  (unsigned int) common->framebuffer_height,
                                  (unsigned int) common->framebuffer_pitch);
                }
                break;
            }
        }
    }
}
