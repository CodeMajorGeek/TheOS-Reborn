#include <CPU/Syscall.h>

#include <CPU/APIC.h>
#include <CPU/ISR.h>
#include <CPU/MSR.h>
#include <CPU/SMP.h>
#include <Device/HPET.h>
#include <Debug/KDebug.h>
#include <FileSystem/ext4.h>
#include <Memory/KMem.h>
#include <Memory/VMM.h>
#include <Storage/AHCI.h>
#include <Task/RCU.h>
#include <Task/Task.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define SYSCALL_USER_VADDR_MIN         0x0000000020000000ULL
#define SYSCALL_USER_VADDR_MAX         0x00007FFFFFFFFFFFULL
#define SYSCALL_USER_CSTR_MAX          256U
#define SYSCALL_CONSOLE_MAX_WRITE      4096U

static volatile uint64_t Syscall_count_per_cpu[256] = { 0 };

static bool Syscall_user_range_valid(uintptr_t user_ptr, size_t size)
{
    if (size == 0)
        return true;

    if (user_ptr < SYSCALL_USER_VADDR_MIN || user_ptr > SYSCALL_USER_VADDR_MAX)
        return false;

    uintptr_t end = user_ptr + (uintptr_t) size - 1U;
    if (end < user_ptr || end > SYSCALL_USER_VADDR_MAX)
        return false;

    uintptr_t page = user_ptr & ~(uintptr_t) 0xFFFU;
    uintptr_t last_page = end & ~(uintptr_t) 0xFFFU;
    while (true)
    {
        if (!VMM_is_user_accessible(page))
            return false;

        if (page == last_page)
            break;
        page += 0x1000U;
    }

    return true;
}

static bool Syscall_copy_to_user(void* user_dst, const void* kernel_src, size_t size)
{
    if (size == 0)
        return true;
    if (!user_dst || !kernel_src)
        return false;
    if (!Syscall_user_range_valid((uintptr_t) user_dst, size))
        return false;

    memcpy(user_dst, kernel_src, size);
    return true;
}

static bool Syscall_copy_from_user(void* kernel_dst, const void* user_src, size_t size)
{
    if (size == 0)
        return true;
    if (!kernel_dst || !user_src)
        return false;
    if (!Syscall_user_range_valid((uintptr_t) user_src, size))
        return false;

    memcpy(kernel_dst, user_src, size);
    return true;
}

static bool Syscall_read_user_cstr(char* kernel_dst, size_t kernel_dst_size, const char* user_src)
{
    if (!kernel_dst || kernel_dst_size < 2 || !user_src)
        return false;

    for (size_t i = 0; i < kernel_dst_size - 1; i++)
    {
        if (!Syscall_user_range_valid((uintptr_t) (user_src + i), 1))
            return false;

        char c = user_src[i];
        kernel_dst[i] = c;
        if (c == '\0')
            return true;
    }

    kernel_dst[kernel_dst_size - 1] = '\0';
    return false;
}

void Syscall_init(void)
{
    MSR_set(IA32_LSTAR, (uint64_t) &syscall_handler_stub);
    MSR_set(IA32_FMASK, SYSCALL_FMASK_TF_BIT | SYSCALL_FMASK_DF_BIT);

    enable_syscall_ext();
}

uint64_t Syscall_interupt_handler(uint64_t syscall_num, syscall_frame_t* frame, uint32_t cpu_index)
{
    task_cpu_local_t* cpu_local = task_get_cpu_local();
    uint8_t apic_id = APIC_get_current_lapic_id();
    if (cpu_local)
    {
        cpu_index = __atomic_load_n(&cpu_local->cpu_index, __ATOMIC_RELAXED);
        apic_id = __atomic_load_n(&cpu_local->apic_id, __ATOMIC_RELAXED);
    }

    if (cpu_index >= 256)
        cpu_index = apic_id;

    uint64_t count = __atomic_add_fetch(&Syscall_count_per_cpu[cpu_index], 1, __ATOMIC_RELAXED);
    if (cpu_local)
        __atomic_store_n(&cpu_local->syscall_count, count, __ATOMIC_RELAXED);

    if ((count % 1024ULL) == 0)
        kdebug_printf("[SYSCALL] cpu=%u apic=%u count=%llu\n",
                      cpu_index,
                      apic_id,
                      (unsigned long long) count);

    switch (syscall_num)
    {
        case SYS_FS_LS:
        {
            ext4_fs_t* fs = ext4_get_active();
            return (fs && ext4_list_root(fs)) ? 0 : (uint64_t) -1;
        }

        case SYS_FS_READ:
        {
            ext4_fs_t* fs = ext4_get_active();
            if (!fs)
                return (uint64_t) -1;

            char name[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_read_user_cstr(name, sizeof(name), (const char*) frame->rdi))
                return (uint64_t) -1;

            uint8_t* user_buf = (uint8_t*) frame->rsi;
            size_t buf_size = (size_t) frame->rdx;
            size_t* out_size = (size_t*) frame->r10;

            uint8_t* data = NULL;
            size_t size = 0;
            if (!ext4_read_file(fs, name, &data, &size) || !data)
                return (uint64_t) -1;
            if (size > buf_size)
            {
                kfree(data);
                return (uint64_t) -1;
            }

            if (!Syscall_copy_to_user(user_buf, data, size))
            {
                kfree(data);
                return (uint64_t) -1;
            }

            if (out_size && !Syscall_copy_to_user(out_size, &size, sizeof(size)))
            {
                kfree(data);
                return (uint64_t) -1;
            }
            kfree(data);
            return 0;
        }

        case SYS_FS_CREATE:
        {
            ext4_fs_t* fs = ext4_get_active();
            if (!fs)
                return (uint64_t) -1;

            char name[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_read_user_cstr(name, sizeof(name), (const char*) frame->rdi))
                return (uint64_t) -1;

            const uint8_t* user_data = (const uint8_t*) frame->rsi;
            size_t size = (size_t) frame->rdx;
            uint8_t* kernel_data = NULL;

            if (size != 0)
            {
                kernel_data = (uint8_t*) kmalloc(size);
                if (!kernel_data)
                    return (uint64_t) -1;
                if (!Syscall_copy_from_user(kernel_data, user_data, size))
                {
                    kfree(kernel_data);
                    return (uint64_t) -1;
                }
            }

            bool ok = ext4_create_file(fs, name, kernel_data, size);
            if (kernel_data)
                kfree(kernel_data);

            return ok ? 0 : (uint64_t) -1;
        }

        case SYS_SLEEP_MS:
            return HPET_sleep_ms((uint32_t) frame->rdi) ? 0 : (uint64_t) -1;

        case SYS_TICK_GET:
            return ISR_get_timer_ticks();

        case SYS_CPU_INFO_GET:
        {
            syscall_cpu_info_t info;
            memset(&info, 0, sizeof(info));

            info.cpu_index = cpu_index;
            info.apic_id = apic_id;
            info.online_cpus = SMP_get_online_cpu_count();
            info.tick_hz = ISR_get_tick_hz();
            info.ticks = ISR_get_timer_ticks();
            return Syscall_copy_to_user((void*) frame->rdi, &info, sizeof(info)) ? 0 : (uint64_t) -1;
        }

        case SYS_SCHED_INFO_GET:
        {
            syscall_sched_info_t info;
            memset(&info, 0, sizeof(info));

            info.current_cpu = cpu_index;
            info.preempt_count = task_get_preempt_count();
            info.local_rq_depth = task_runqueue_depth();
            info.total_rq_depth = task_runqueue_depth_total();
            return Syscall_copy_to_user((void*) frame->rdi, &info, sizeof(info)) ? 0 : (uint64_t) -1;
        }

        case SYS_AHCI_IRQ_INFO_GET:
        {
            syscall_ahci_irq_info_t info;
            memset(&info, 0, sizeof(info));

            info.mode = AHCI_get_irq_mode();
            info.reserved = 0;
            info.count = AHCI_get_irq_count();
            return Syscall_copy_to_user((void*) frame->rdi, &info, sizeof(info)) ? 0 : (uint64_t) -1;
        }

        case SYS_RCU_SYNC:
            return RCU_synchronize() ? 0 : (uint64_t) -1;

        case SYS_RCU_INFO_GET:
        {
            syscall_rcu_info_t info;
            memset(&info, 0, sizeof(info));

            rcu_stats_t stats = { 0 };
            RCU_get_stats(&stats);
            info.gp_seq = stats.gp_seq;
            info.gp_target = stats.gp_target;
            info.callbacks_pending = stats.callbacks_pending;
            info.local_read_depth = stats.local_read_depth;
            info.local_preempt_count = stats.local_preempt_count;
            return Syscall_copy_to_user((void*) frame->rdi, &info, sizeof(info)) ? 0 : (uint64_t) -1;
        }

        case SYS_CONSOLE_WRITE:
        {
            const char* user_buf = (const char*) frame->rdi;
            size_t len = (size_t) frame->rsi;
            if (!user_buf || len == 0)
                return 0;

            if (len > SYSCALL_CONSOLE_MAX_WRITE)
                len = SYSCALL_CONSOLE_MAX_WRITE;

            char* kernel_buf = (char*) kmalloc(len);
            if (!kernel_buf)
                return (uint64_t) -1;

            if (!Syscall_copy_from_user(kernel_buf, user_buf, len))
            {
                kfree(kernel_buf);
                return (uint64_t) -1;
            }

            for (size_t i = 0; i < len; i++)
            {
                putc(kernel_buf[i]);
#if defined(THEOS_ENABLE_KDEBUG)
                kdebug_putc(kernel_buf[i]);
#endif
            }

            kfree(kernel_buf);
            return (uint64_t) len;
        }

        default:
            return (uint64_t) -1;
    }
}
