#include <Boot/LimineHelper.h>

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
#include <Device/VGA.h>
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
#include <CPU/x86.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

__attribute__((__noreturn__)) void k_entry(void);

extern void* kernel_phys_start;
extern void* kernel_phys_end;
extern void* kernel_virt_start;
extern void* kernel_virt_end;

extern void* kernel_stack_top;
extern void* kernel_stack_bottom;

static APIC_MADT_t* MADT = NULL;
static const uint32_t BSP_TIMER_HZ = 100;
static const uint32_t PIT_TIMER_HZ = 1000;
static uint16_t boot_tty_shadow[VGA_WIDTH * VGA_HEIGHT];

#define THEOS_PSF2_FONT_PATH "/system/fonts/ter-powerline-v14n.psf"

uintptr_t ROOT_DEV = 1;

static uint32_t boot_read_le32(const uint8_t* ptr)
{
    return ((uint32_t) ptr[0]) |
           ((uint32_t) ptr[1] << 8) |
           ((uint32_t) ptr[2] << 16) |
           ((uint32_t) ptr[3] << 24);
}

static uint64_t boot_read_le64(const uint8_t* ptr)
{
    return ((uint64_t) ptr[0]) |
           ((uint64_t) ptr[1] << 8) |
           ((uint64_t) ptr[2] << 16) |
           ((uint64_t) ptr[3] << 24) |
           ((uint64_t) ptr[4] << 32) |
           ((uint64_t) ptr[5] << 40) |
           ((uint64_t) ptr[6] << 48) |
           ((uint64_t) ptr[7] << 56);
}

static size_t boot_collect_mbr_partitions(HBA_PORT_t* port,
                                          uint32_t* part_index_out,
                                          uint32_t* lba_out,
                                          uint8_t* type_out,
                                          size_t cap)
{
    if (!port || !part_index_out || !lba_out || !type_out || cap == 0)
        return 0;

    uint8_t sector[AHCI_SECTOR_SIZE];
    if (AHCI_sata_read(port, 0, 0, 1, sector) != 0)
        return 0;

    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return 0;

    size_t count = 0;
    for (uint32_t part = 0; part < 4 && count < cap; part++)
    {
        const uint8_t* entry = &sector[446 + part * 16];
        uint8_t type = entry[4];
        uint32_t first_lba = boot_read_le32(&entry[8]);
        uint32_t sectors = boot_read_le32(&entry[12]);
        if (type == 0 || type == 0xEE || first_lba == 0 || sectors == 0)
            continue;

        part_index_out[count] = part;
        lba_out[count] = first_lba;
        type_out[count] = type;
        count++;
    }

    return count;
}

static bool boot_read_mbr_disk_signature(HBA_PORT_t* port, uint32_t* disk_id_out)
{
    if (!port || !disk_id_out)
        return false;

    uint8_t sector[AHCI_SECTOR_SIZE];
    if (AHCI_sata_read(port, 0, 0, 1, sector) != 0)
        return false;

    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return false;

    *disk_id_out = boot_read_le32(&sector[440]);
    return true;
}

static void boot_log_ext4_probe_failure(HBA_PORT_t* port,
                                        int device_index,
                                        uint64_t lba_base,
                                        const char* source)
{
    if (!port)
        return;

    uint8_t superblock[AHCI_SECTOR_SIZE * 2];
    memset(superblock, 0, sizeof(superblock));

    uint64_t super_lba = lba_base + ((uint64_t) EXT4_SUPERBLOCK_ADDR / AHCI_SECTOR_SIZE);
    int rc = AHCI_sata_read(port,
                            (uint32_t) super_lba,
                            (uint32_t) (super_lba >> 32),
                            2,
                            superblock);
    if (rc != 0)
    {
        kdebug_printf("[BOOT] ext4 probe fail dev=%d lba=0x%llX (%s) read_rc=%d\n",
                      device_index,
                      (unsigned long long) lba_base,
                      source ? source : "unknown",
                      rc);
        return;
    }

    uint16_t magic = (uint16_t) superblock[56] | ((uint16_t) superblock[57] << 8);
    kdebug_printf("[BOOT] ext4 probe fail dev=%d lba=0x%llX (%s) read_rc=%d magic=0x%X\n",
                  device_index,
                  (unsigned long long) lba_base,
                  source ? source : "unknown",
                  rc,
                  magic);
}

static size_t boot_collect_gpt_partitions(HBA_PORT_t* port,
                                          uint32_t* part_index_out,
                                          uint64_t* lba_out,
                                          size_t cap)
{
    if (!port || !part_index_out || !lba_out || cap == 0)
        return 0;

    uint8_t header[AHCI_SECTOR_SIZE];
    if (AHCI_sata_read(port, 1, 0, 1, header) != 0)
        return 0;

    if (memcmp(header, "EFI PART", 8) != 0)
        return 0;

    uint32_t entry_count = boot_read_le32(&header[80]);
    uint32_t entry_size = boot_read_le32(&header[84]);
    uint64_t entry_lba = boot_read_le64(&header[72]);
    if (entry_count == 0 || entry_size < 128 || entry_size > 1024)
        return 0;

    if (entry_count > 128)
        entry_count = 128;

    uint64_t table_bytes_u64 = (uint64_t) entry_count * entry_size;
    uint64_t table_sectors_u64 = (table_bytes_u64 + AHCI_SECTOR_SIZE - 1) / AHCI_SECTOR_SIZE;
    if (table_sectors_u64 == 0 || table_sectors_u64 > 0xFFFFFFFFULL)
        return 0;

    size_t table_bytes = (size_t) (table_sectors_u64 * AHCI_SECTOR_SIZE);
    uint8_t* table = (uint8_t*) kmalloc(table_bytes);
    if (!table)
        return 0;

    if (AHCI_sata_read(port,
                       (uint32_t) entry_lba,
                       (uint32_t) (entry_lba >> 32),
                       (uint32_t) table_sectors_u64,
                       table) != 0)
    {
        kfree(table);
        return 0;
    }

    size_t count = 0;
    for (uint32_t i = 0; i < entry_count && count < cap; i++)
    {
        uint8_t* entry = table + (size_t) i * entry_size;
        bool type_is_zero = true;
        for (uint32_t j = 0; j < 16; j++)
        {
            if (entry[j] != 0)
            {
                type_is_zero = false;
                break;
            }
        }
        if (type_is_zero)
            continue;

        uint64_t first_lba = boot_read_le64(&entry[32]);
        uint64_t last_lba = boot_read_le64(&entry[40]);
        if (first_lba == 0 || last_lba < first_lba)
            continue;

        part_index_out[count] = i + 1;
        lba_out[count] = first_lba;
        count++;
    }

    kfree(table);
    return count;
}

static bool boot_try_mount_ext4_on_port(ext4_fs_t* fs,
                                        HBA_PORT_t* port,
                                        int device_index,
                                        int32_t slice_hint,
                                        uint64_t* lba_base_out)
{
    if (!fs || !port || !lba_base_out)
        return false;

    if (ext4_mount(fs, port))
    {
        *lba_base_out = 0;
        kdebug_printf("[BOOT] ext4 probe ok dev=%d lba=0x%llX (whole disk)\n",
                      device_index,
                      (unsigned long long) *lba_base_out);
        return true;
    }
    boot_log_ext4_probe_failure(port, device_index, 0, "whole-disk");

    uint32_t part_index[4];
    uint32_t part_lba[4];
    uint8_t part_type[4];
    size_t part_count = boot_collect_mbr_partitions(port, part_index, part_lba, part_type, 4);
    for (size_t i = 0; i < part_count; i++)
    {
        kdebug_printf("[BOOT] part found dev=%d scheme=MBR slice=%u type=0x%X lba=0x%X\n",
                      device_index,
                      part_index[i],
                      part_type[i],
                      part_lba[i]);
    }
    bool used[4] = { false, false, false, false };
    if (part_count > 0 && slice_hint >= 0)
    {
        for (size_t i = 0; i < part_count; i++)
        {
            if ((int32_t) part_index[i] != slice_hint)
                continue;

            used[i] = true;
            if (ext4_mount_lba(fs, port, part_lba[i]))
            {
                *lba_base_out = part_lba[i];
                kdebug_printf("[BOOT] ext4 probe ok dev=%d lba=0x%llX (MBR slice=%u type=0x%X)\n",
                              device_index,
                              (unsigned long long) *lba_base_out,
                              part_index[i],
                              part_type[i]);
                return true;
            }

            boot_log_ext4_probe_failure(port, device_index, part_lba[i], "MBR-slice-hint");
        }
    }

    for (size_t i = 0; i < part_count; i++)
    {
        if (used[i])
            continue;

        if (!ext4_mount_lba(fs, port, part_lba[i]))
        {
            boot_log_ext4_probe_failure(port, device_index, part_lba[i], "MBR-slice-scan");
            continue;
        }

        *lba_base_out = part_lba[i];
        kdebug_printf("[BOOT] ext4 probe ok dev=%d lba=0x%llX (MBR slice=%u type=0x%X)\n",
                      device_index,
                      (unsigned long long) *lba_base_out,
                      part_index[i],
                      part_type[i]);
        return true;
    }

    uint32_t gpt_part_index[16];
    uint64_t gpt_part_lba[16];
    size_t gpt_part_count = boot_collect_gpt_partitions(port, gpt_part_index, gpt_part_lba, 16);
    for (size_t i = 0; i < gpt_part_count; i++)
    {
        kdebug_printf("[BOOT] part found dev=%d scheme=GPT part=%u lba=0x%llX\n",
                      device_index,
                      gpt_part_index[i],
                      (unsigned long long) gpt_part_lba[i]);
    }
    bool gpt_used[16] = { false };

    if (gpt_part_count > 0 && slice_hint >= 0)
    {
        for (size_t i = 0; i < gpt_part_count; i++)
        {
            bool matches_hint = ((int32_t) gpt_part_index[i] == slice_hint) ||
                                ((int32_t) gpt_part_index[i] == (slice_hint + 1));
            if (!matches_hint)
                continue;

            gpt_used[i] = true;
            if (!ext4_mount_lba(fs, port, gpt_part_lba[i]))
            {
                boot_log_ext4_probe_failure(port, device_index, gpt_part_lba[i], "GPT-part-hint");
                continue;
            }

            *lba_base_out = gpt_part_lba[i];
            kdebug_printf("[BOOT] ext4 probe ok dev=%d lba=0x%llX (GPT part=%u)\n",
                          device_index,
                          (unsigned long long) *lba_base_out,
                          gpt_part_index[i]);
            return true;
        }
    }

    for (size_t i = 0; i < gpt_part_count; i++)
    {
        if (gpt_used[i])
            continue;

        if (!ext4_mount_lba(fs, port, gpt_part_lba[i]))
        {
            boot_log_ext4_probe_failure(port, device_index, gpt_part_lba[i], "GPT-part-scan");
            continue;
        }

        *lba_base_out = gpt_part_lba[i];
        kdebug_printf("[BOOT] ext4 probe ok dev=%d lba=0x%llX (GPT part=%u)\n",
                      device_index,
                      (unsigned long long) *lba_base_out,
                      gpt_part_index[i]);
        return true;
    }

    return false;
}

static bool boot_init_acpi_early(APIC_MADT_t** out_madt)
{
    if (out_madt)
        *out_madt = NULL;

    if (!LimineHelper_init_acpi_from_rsdp_if_needed())
    {
        kdebug_puts("[BOOT] ACPI init skipped (Limine RSDP unavailable)\n");
        return false;
    }

    APIC_MADT_t* madt = (APIC_MADT_t*) ACPI_get_table(ACPI_APIC_SIGNATURE);
    if (!madt)
    {
        kdebug_puts("[BOOT] ACPI MADT missing after Limine RSDP init\n");
        return true;
    }

    if (out_madt)
        *out_madt = madt;

    kdebug_printf("[BOOT] ACPI MADT ready len=%u lapic=0x%X flags=0x%X\n",
                  madt->SDT_header.length,
                  madt->lapic_ptr,
                  madt->flags);
    return true;
}

__attribute__((__noreturn__)) void k_entry(void)
{
    uintptr_t kernel_phys_start_runtime = (uintptr_t) &kernel_phys_start;
    uintptr_t kernel_phys_end_runtime = (uintptr_t) &kernel_phys_end;
    uintptr_t kernel_virt_start_runtime = (uintptr_t) &kernel_virt_start;
    uintptr_t kernel_virt_end_runtime = (uintptr_t) &kernel_virt_end;
    TTY_framebuffer_info_t boot_framebuffer = { 0 };
    bool boot_framebuffer_available = false;
    bool boot_limine_mbr_disk_id_hint_present = false;
    uint32_t boot_limine_mbr_disk_id_hint = 0;
    bool boot_limine_slice_hint_present = false;
    int32_t boot_limine_slice_hint = -1;

    TTY_set_buffer(boot_tty_shadow);
    TTY_init();
    logger_init();
    kdebug_init();
    kdebug_puts("[BOOT] k_entry start\n");

    if (!LimineHelper_base_revision_supported())
        panic("Booted via Limine with unsupported base revision");

    LimineHelper_resolve_runtime_kernel_bases((uintptr_t) &kernel_phys_start,
                                              (uintptr_t) &kernel_phys_end,
                                              (uintptr_t) &kernel_virt_start,
                                              (uintptr_t) &kernel_virt_end,
                                              &kernel_phys_start_runtime,
                                              &kernel_phys_end_runtime,
                                              &kernel_virt_start_runtime,
                                              &kernel_virt_end_runtime);

    PMM_init(kernel_phys_start_runtime,
             kernel_phys_end_runtime,
             kernel_virt_start_runtime,
             kernel_virt_end_runtime);
    printf("Kernel phys [0x%llX..0x%llX) virt [0x%llX..0x%llX)\n",
           (unsigned long long) kernel_phys_start_runtime,
           (unsigned long long) kernel_phys_end_runtime,
           (unsigned long long) kernel_virt_start_runtime,
           (unsigned long long) kernel_virt_end_runtime);
    kdebug_puts("[BOOT] PMM base set\n");
    
    LimineHelper_read_boot_info();
    kdebug_puts("[BOOT] limine parsed\n");

    VMM_map_kernel();
    kdebug_puts("[BOOT] VMM kernel map done\n");
    // VMM_map_userland_stack();

    VMM_hardware_mapping();
    VMM_load_cr3();
    kdebug_puts("[BOOT] CR3 loaded\n");
    GDT_load_kernel_segments();
    kdebug_puts("[BOOT] GDT reloaded\n");
    LimineHelper_promote_bootloader_reclaimable();

    bool acpi_ready = boot_init_acpi_early(&MADT);
    if (acpi_ready && ACPI_power_init())
        kdebug_puts("[BOOT] ACPI power states initialized (Limine RSDP path)\n");
    else if (acpi_ready)
        kdebug_puts("[BOOT] ACPI power states unavailable (Limine RSDP path)\n");
    else
        kdebug_puts("[BOOT] ACPI power init skipped (no Limine RSDP)\n");

    IDT_init();
    kdebug_puts("[BOOT] IDT loaded\n");

    if (!FPU_init_cpu(0))
    {
        kdebug_puts("[BOOT] FPU init failed\n");
        abort();
    }
    kdebug_puts("[BOOT] FPU init done\n");

    boot_framebuffer_available = LimineHelper_get_framebuffer(&boot_framebuffer);
    boot_limine_mbr_disk_id_hint_present = LimineHelper_get_mbr_disk_id_hint(&boot_limine_mbr_disk_id_hint);
    boot_limine_slice_hint_present = LimineHelper_get_slice_hint(&boot_limine_slice_hint);

    if (boot_framebuffer_available)
        kdebug_puts("[BOOT] framebuffer detected, switch deferred until PSF2 load\n");
    else
        kdebug_puts("[BOOT] framebuffer unavailable\n");

    if (!MADT)
    {
        kdebug_puts("[BOOT] APIC skipped (MADT unavailable from Limine ACPI)\n");
    }
    else if (APIC_check())
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

    kdebug_puts("[BOOT] PCI scan start (ACPI/RSDP/MADT already resolved via Limine)\n");
    PCI_init();
    kdebug_puts("[BOOT] PCI scanned\n");

    Keyboard_init();
    kdebug_puts("[BOOT] keyboard init\n");
    Syscall_init();
    kdebug_puts("[BOOT] syscall init\n");

    static ext4_fs_t fs;
    HBA_PORT_t* root_port = NULL;
    uint64_t root_lba_base = 0;
    int root_device_index = -1;
    bool root_from_limine_hint = false;
    int device_count = AHCI_get_device_count();

    if (device_count > 0)
    {
        int preferred_device = -1;
        bool preferred_is_limine_hint = false;
        bool preferred_mount_attempted = false;

        if (boot_limine_mbr_disk_id_hint_present)
        {
            kdebug_printf("[BOOT] Limine root hint mbr_disk_id=0x%X slice_hint=%d\n",
                          boot_limine_mbr_disk_id_hint,
                          boot_limine_slice_hint_present ? (int) boot_limine_slice_hint : -1);

            for (int i = 0; i < device_count; i++)
            {
                HBA_PORT_t* candidate = AHCI_get_device(i);
                uint32_t disk_sig = 0;
                if (!candidate || !boot_read_mbr_disk_signature(candidate, &disk_sig))
                    continue;
                if (disk_sig != boot_limine_mbr_disk_id_hint)
                    continue;
                preferred_device = i;
                preferred_is_limine_hint = true;
                break;
            }

            if (preferred_device >= 0)
            {
                HBA_PORT_t* preferred_port = AHCI_get_device(preferred_device);
                int32_t slice_hint = boot_limine_slice_hint_present ? boot_limine_slice_hint : -1;
                preferred_mount_attempted = true;
                kdebug_printf("[BOOT] Limine root hint matched AHCI dev=%d, probing preferred device first\n",
                              preferred_device);

                if (preferred_port &&
                    boot_try_mount_ext4_on_port(&fs, preferred_port, preferred_device, slice_hint, &root_lba_base))
                {
                    root_port = preferred_port;
                    root_device_index = preferred_device;
                    root_from_limine_hint = true;
                }
                else
                {
                    kdebug_printf("[BOOT] Limine preferred dev=%d mount failed, fallback probing enabled\n",
                                  preferred_device);
                }
            }
            else
            {
                kdebug_printf("[BOOT] Limine mbr_disk_id hint 0x%X not found on AHCI devices, fallback probing all devices\n",
                              boot_limine_mbr_disk_id_hint);
            }
        }

        if (!root_port)
        {
            for (int dev_index = 0; dev_index < device_count; dev_index++)
            {
                if (preferred_mount_attempted && dev_index == preferred_device)
                    continue;

                HBA_PORT_t* candidate = AHCI_get_device(dev_index);
                if (!candidate)
                    continue;

                if (!boot_try_mount_ext4_on_port(&fs, candidate, dev_index, -1, &root_lba_base))
                    continue;

                root_port = candidate;
                root_device_index = dev_index;
                root_from_limine_hint = (preferred_is_limine_hint && dev_index == preferred_device);
                break;
            }
        }
    }

    if (root_port)
    {
        ext4_set_active(&fs);
        kdebug_printf("[BOOT] ext4 mounted dev=%d lba_base=0x%llX%s\n",
                      root_device_index,
                      (unsigned long long) root_lba_base,
                      (root_from_limine_hint && root_device_index >= 0)
                          ? " source=limine-file"
                          : ""
                      );
        kdebug_file_sink_ready();

        bool psf_loaded = false;
        uint8_t* font_data = NULL;
        size_t font_size = 0;
        const char* font_path = THEOS_PSF2_FONT_PATH;
        if (ext4_read_file(&fs, font_path, &font_data, &font_size))
        {
            if (TTY_load_psf2(font_data, font_size))
            {
                psf_loaded = true;
                kdebug_printf("[TTY] loaded PSF2 from %s (%llu bytes)\n",
                              font_path,
                              (unsigned long long) font_size);
            }
            else
            {
                kdebug_printf("[TTY] invalid PSF2 file at %s\n", font_path);
            }
            kfree(font_data);
        }
        else
        {
            kdebug_printf("[TTY] no PSF2 font found at %s\n", font_path);
        }

        if (boot_framebuffer_available && psf_loaded)
        {
            if (TTY_init_framebuffer(&boot_framebuffer))
                kdebug_puts("[BOOT] framebuffer switch done\n");
            else
                kdebug_puts("[BOOT] framebuffer switch failed, staying in VGA mode\n");
        }
    }
    else
    {
        if (device_count <= 0)
            printf("AHCI: no block device detected\n");
        else
            printf("Unable to mount ext4 filesystem on AHCI block devices\n");
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
    printf("%02u/%02u/%04u %s %02u:%02u:%02u\n",
           (unsigned) rtc.month_day,
           (unsigned) rtc.month,
           (unsigned) rtc.year,
           rtc.weekday,
           (unsigned) rtc.hours,
           (unsigned) rtc.minutes,
           (unsigned) rtc.seconds);

    if (!UserMode_run_elf("/bin/TheApp"))
        kdebug_puts("[USER] launch failed, staying in kernel idle loop\n");

    while (TRUE)
        __asm__ __volatile__("sti\nhlt");

    __builtin_unreachable();
}
