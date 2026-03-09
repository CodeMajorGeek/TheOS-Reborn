#include <Boot/Limine.h>

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
#include <CPU/IO.h>
#include <CPU/x86.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void read_limine_info(void);
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
static TTY_framebuffer_info_t boot_framebuffer = { 0 };
static bool boot_framebuffer_available = false;
static char boot_cmdline[256] = { 0 };
static bool boot_via_limine = false;
static uintptr_t boot_limine_kernel_phys_base = 0;
static uintptr_t boot_limine_kernel_virt_base = 0;
static uint64_t boot_limine_hhdm_offset = 0;
static uintptr_t boot_limine_rsdp_addr = 0;
static bool boot_limine_mbr_disk_id_hint_present = false;
static uint32_t boot_limine_mbr_disk_id_hint = 0;
static bool boot_limine_slice_hint_present = false;
static int32_t boot_limine_slice_hint = -1;
static uint16_t boot_tty_shadow[VGA_WIDTH * VGA_HEIGHT];

__attribute__((__noreturn__)) void limine_entry(void);

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests_start_marker")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_entry_point_request limine_entry_point_request = {
    .id = LIMINE_ENTRY_POINT_REQUEST_ID,
    .revision = 0,
    .response = NULL,
    .entry = limine_entry
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request limine_executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request limine_cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_file_request limine_executable_file_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request limine_memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request limine_hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request limine_framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request limine_rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests_end_marker")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

#define THEOS_PSF2_FONT_PATH "/system/fonts/ter-powerline-v14n.psf"

uintptr_t ROOT_DEV = 1;

static void boot_init_acpi_from_limine_rsdp_if_needed(void)
{
    if (!boot_via_limine || boot_limine_rsdp_addr == 0)
        return;

    uint8_t* rsdp = (uint8_t*) boot_limine_rsdp_addr;
    if (ACPI_RSDP_new_check(rsdp))
    {
        ACPI_init_XSDT((ACPI_RSDP_descriptor20_t*) rsdp);
        kdebug_printf("[BOOT] Limine ACPI init via RSDP=0x%llX (RSDP2)\n",
                      (unsigned long long) boot_limine_rsdp_addr);
        return;
    }
    if (ACPI_RSDP_old_check(rsdp))
    {
        ACPI_init_RSDT((ACPI_RSDP_descriptor10_t*) rsdp);
        kdebug_printf("[BOOT] Limine ACPI init via RSDP=0x%llX (RSDP1)\n",
                      (unsigned long long) boot_limine_rsdp_addr);
        return;
    }

    if (boot_limine_hhdm_offset != 0)
    {
        uintptr_t hhdm_rsdp = (uintptr_t) (boot_limine_hhdm_offset + boot_limine_rsdp_addr);
        rsdp = (uint8_t*) hhdm_rsdp;

        if (ACPI_RSDP_new_check(rsdp))
        {
            ACPI_init_XSDT((ACPI_RSDP_descriptor20_t*) rsdp);
            kdebug_printf("[BOOT] Limine ACPI init via HHDM RSDP=0x%llX (RSDP2)\n",
                          (unsigned long long) hhdm_rsdp);
            return;
        }
        if (ACPI_RSDP_old_check(rsdp))
        {
            ACPI_init_RSDT((ACPI_RSDP_descriptor10_t*) rsdp);
            kdebug_printf("[BOOT] Limine ACPI init via HHDM RSDP=0x%llX (RSDP1)\n",
                          (unsigned long long) hhdm_rsdp);
            return;
        }
    }

    kdebug_printf("[BOOT] Limine ACPI init failed (RSDP=0x%llX)\n",
                  (unsigned long long) boot_limine_rsdp_addr);
}

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
        }
    }

    for (size_t i = 0; i < part_count; i++)
    {
        if (used[i])
            continue;

        if (!ext4_mount_lba(fs, port, part_lba[i]))
            continue;

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
                continue;

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
            continue;

        *lba_base_out = gpt_part_lba[i];
        kdebug_printf("[BOOT] ext4 probe ok dev=%d lba=0x%llX (GPT part=%u)\n",
                      device_index,
                      (unsigned long long) *lba_base_out,
                      gpt_part_index[i]);
        return true;
    }

    return false;
}

__attribute__((__noreturn__)) void k_entry(void)
{
    uintptr_t kernel_phys_start_runtime = (uintptr_t) &kernel_phys_start;
    uintptr_t kernel_phys_end_runtime = (uintptr_t) &kernel_phys_end;
    uintptr_t kernel_virt_start_runtime = (uintptr_t) &kernel_virt_start;
    uintptr_t kernel_virt_end_runtime = (uintptr_t) &kernel_virt_end;

    TTY_set_buffer(boot_tty_shadow);
    TTY_init();
    logger_init();
    kdebug_init();
    kdebug_puts("[BOOT] k_entry start\n");

    boot_via_limine = true;
    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false)
        panic("Booted via Limine with unsupported base revision");

    if (limine_executable_address_request.response)
    {
        boot_limine_kernel_phys_base = (uintptr_t) limine_executable_address_request.response->physical_base;
        boot_limine_kernel_virt_base = (uintptr_t) limine_executable_address_request.response->virtual_base;

        uintptr_t linked_phys_span = (uintptr_t) &kernel_phys_end - (uintptr_t) &kernel_phys_start;
        uintptr_t linked_virt_span = (uintptr_t) &kernel_virt_end - (uintptr_t) &kernel_virt_start;

        kernel_phys_start_runtime = boot_limine_kernel_phys_base;
        kernel_phys_end_runtime = boot_limine_kernel_phys_base + linked_phys_span;
        kernel_virt_start_runtime = boot_limine_kernel_virt_base;
        kernel_virt_end_runtime = boot_limine_kernel_virt_base + linked_virt_span;
    }
    else
    {
        kdebug_puts("[BOOT] Limine executable address response missing, using linked bases\n");
    }

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
    
    read_limine_info();
    kdebug_puts("[BOOT] limine parsed\n");

    VMM_map_kernel();
    kdebug_puts("[BOOT] VMM kernel map done\n");
    // VMM_map_userland_stack();

    VMM_hardware_mapping();
    VMM_load_cr3();
    kdebug_puts("[BOOT] CR3 loaded\n");
    GDT_load_kernel_segments();
    kdebug_puts("[BOOT] GDT reloaded\n");

    boot_init_acpi_from_limine_rsdp_if_needed();

    MADT = (APIC_MADT_t*) ACPI_get_table(ACPI_APIC_SIGNATURE);
    kdebug_puts("[BOOT] ACPI MADT fetched\n");
    if (ACPI_power_init())
        kdebug_puts("[BOOT] ACPI power states initialized\n");
    else
        kdebug_puts("[BOOT] ACPI power states unavailable\n");

    if (boot_framebuffer_available)
        kdebug_puts("[BOOT] framebuffer detected, switch deferred until PSF2 load\n");
    else
        kdebug_puts("[BOOT] framebuffer unavailable\n");

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
    HBA_PORT_t* root_port = NULL;
    uint64_t root_lba_base = 0;
    int root_device_index = -1;
    bool root_from_limine_hint = false;
    int device_count = AHCI_get_device_count();

    if (device_count > 0)
    {
        int ordered_devices[AHCI_MAX_SLOT];
        int ordered_count = 0;
        int preferred_device = -1;
        bool preferred_is_limine_hint = false;

        if (boot_limine_mbr_disk_id_hint_present)
        {
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
        }

        if (preferred_device >= 0)
            ordered_devices[ordered_count++] = preferred_device;

        for (int i = 0; i < device_count && ordered_count < AHCI_MAX_SLOT; i++)
        {
            if (i == preferred_device)
                continue;
            ordered_devices[ordered_count++] = i;
        }

        for (int i = 0; i < ordered_count; i++)
        {
            int dev_index = ordered_devices[i];
            HBA_PORT_t* candidate = AHCI_get_device(dev_index);
            if (!candidate)
                continue;

            int32_t slice_hint = -1;
            if (preferred_is_limine_hint && dev_index == preferred_device && boot_limine_slice_hint_present)
                slice_hint = boot_limine_slice_hint;

            if (!boot_try_mount_ext4_on_port(&fs, candidate, dev_index, slice_hint, &root_lba_base))
                continue;

            root_port = candidate;
            root_device_index = dev_index;
            root_from_limine_hint = (preferred_is_limine_hint && dev_index == preferred_device);
            break;
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
            printf("AHCI: no SATA device detected\n");
        else
            printf("Unable to mount ext4 filesystem on AHCI devices\n");
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

void read_limine_info(void)
{
    if (limine_hhdm_request.response)
        boot_limine_hhdm_offset = limine_hhdm_request.response->offset;

    if (limine_cmdline_request.response && limine_cmdline_request.response->cmdline)
    {
        const char* cmdline = limine_cmdline_request.response->cmdline;
        size_t len = strlen(cmdline);
        if (len >= sizeof(boot_cmdline))
            len = sizeof(boot_cmdline) - 1U;
        memcpy(boot_cmdline, cmdline, len);
        boot_cmdline[len] = '\0';

        kdebug_printf("[BOOT] Limine cmdline: %s\n", boot_cmdline);
    }
    else
    {
        boot_cmdline[0] = '\0';
        kdebug_puts("[BOOT] Limine cmdline unavailable\n");
    }

    if (limine_memmap_request.response)
    {
        for (uint64_t i = 0; i < limine_memmap_request.response->entry_count; i++)
        {
            struct limine_memmap_entry* entry = limine_memmap_request.response->entries[i];
            if (!entry)
                continue;

            if (entry->type == LIMINE_MEMMAP_USABLE)
            {
                printf("MMAP avaliable found at addr 0x%llX with size of 0x%llX !\n",
                       (unsigned long long) entry->base,
                       (unsigned long long) entry->length);
                PMM_init_region((uintptr_t) entry->base, (uintptr_t) entry->length);
            }
        }
    }
    else
    {
        kdebug_puts("[BOOT] Limine memmap unavailable\n");
    }

    if (limine_rsdp_request.response && limine_rsdp_request.response->address)
    {
        /* Limine may provide an address that is not directly accessible
           before our own virtual memory setup on all machines/firmwares. */
        boot_limine_rsdp_addr = (uintptr_t) limine_rsdp_request.response->address;
        kdebug_printf("[BOOT] Limine RSDP deferred at 0x%llX\n",
                      (unsigned long long) boot_limine_rsdp_addr);
    }

    boot_limine_mbr_disk_id_hint_present = false;
    boot_limine_mbr_disk_id_hint = 0;
    boot_limine_slice_hint_present = false;
    boot_limine_slice_hint = -1;

    if (limine_executable_file_request.response &&
        limine_executable_file_request.response->executable_file)
    {
        struct limine_file* executable = limine_executable_file_request.response->executable_file;
        if (executable->mbr_disk_id != 0)
        {
            boot_limine_mbr_disk_id_hint_present = true;
            boot_limine_mbr_disk_id_hint = executable->mbr_disk_id;
        }
        if (executable->partition_index != 0)
        {
            boot_limine_slice_hint_present = true;
            boot_limine_slice_hint = (int32_t) executable->partition_index - 1;
        }

        kdebug_printf("[BOOT] Limine executable source media=%u mbr_disk_id=0x%X part_index=%u\n",
                      executable->media_type,
                      executable->mbr_disk_id,
                      executable->partition_index);
    }

    boot_framebuffer_available = false;
    if (limine_framebuffer_request.response &&
        limine_framebuffer_request.response->framebuffer_count > 0 &&
        limine_framebuffer_request.response->framebuffers &&
        limine_framebuffer_request.response->framebuffers[0])
    {
        struct limine_framebuffer* fb = limine_framebuffer_request.response->framebuffers[0];
        if (fb->memory_model == LIMINE_FRAMEBUFFER_RGB &&
            fb->width != 0 && fb->height != 0 && fb->pitch != 0)
        {
            uintptr_t fb_addr = (uintptr_t) fb->address;
            if (boot_limine_hhdm_offset != 0 && fb_addr >= (uintptr_t) boot_limine_hhdm_offset)
                boot_framebuffer.phys_addr = fb_addr - (uintptr_t) boot_limine_hhdm_offset;
            else
                boot_framebuffer.phys_addr = fb_addr;
            boot_framebuffer.pitch = (uint32_t) fb->pitch;
            boot_framebuffer.width = (uint32_t) fb->width;
            boot_framebuffer.height = (uint32_t) fb->height;
            boot_framebuffer.bpp = (uint8_t) fb->bpp;
            boot_framebuffer.type = TTY_FRAMEBUFFER_TYPE_RGB;
            boot_framebuffer.red_field_position = fb->red_mask_shift;
            boot_framebuffer.red_mask_size = fb->red_mask_size;
            boot_framebuffer.green_field_position = fb->green_mask_shift;
            boot_framebuffer.green_mask_size = fb->green_mask_size;
            boot_framebuffer.blue_field_position = fb->blue_mask_shift;
            boot_framebuffer.blue_mask_size = fb->blue_mask_size;
            boot_framebuffer_available = true;
            kdebug_printf("[BOOT] Limine framebuffer addr=0x%llX %ux%u pitch=%u bpp=%u\n",
                          (unsigned long long) boot_framebuffer.phys_addr,
                          (unsigned int) boot_framebuffer.width,
                          (unsigned int) boot_framebuffer.height,
                          (unsigned int) boot_framebuffer.pitch,
                          (unsigned int) boot_framebuffer.bpp);
        }
    }
}
