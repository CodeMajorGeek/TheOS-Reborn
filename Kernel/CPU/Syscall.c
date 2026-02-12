#include <CPU/Syscall.h>

#include <CPU/APIC.h>
#include <CPU/ISR.h>
#include <CPU/MSR.h>
#include <CPU/SMP.h>
#include <Device/HPET.h>
#include <Debug/KDebug.h>
#include <FileSystem/ext4.h>
#include <Memory/KMem.h>
#include <Storage/AHCI.h>
#include <Task/RCU.h>
#include <Task/Task.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>

static volatile uint64_t Syscall_count_per_cpu[256] = { 0 };

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

            const char* name = (const char*) frame->rdi;
            uint8_t* user_buf = (uint8_t*) frame->rsi;
            size_t buf_size = (size_t) frame->rdx;
            size_t* out_size = (size_t*) frame->r10;

            uint8_t* data = NULL;
            size_t size = 0;
            if (!ext4_read_file(fs, name, &data, &size))
                return (uint64_t) -1;
            if (size > buf_size)
            {
                kfree(data);
                return (uint64_t) -1;
            }
            memcpy(user_buf, data, size);
            if (out_size)
                *out_size = size;
            kfree(data);
            return 0;
        }

        case SYS_FS_CREATE:
        {
            ext4_fs_t* fs = ext4_get_active();
            if (!fs)
                return (uint64_t) -1;

            const char* name = (const char*) frame->rdi;
            const uint8_t* data = (const uint8_t*) frame->rsi;
            size_t size = (size_t) frame->rdx;
            return ext4_create_file(fs, name, data, size) ? 0 : (uint64_t) -1;
        }

        case SYS_SLEEP_MS:
            return HPET_sleep_ms((uint32_t) frame->rdi) ? 0 : (uint64_t) -1;

        case SYS_TICK_GET:
            return ISR_get_timer_ticks();

        case SYS_CPU_INFO_GET:
        {
            syscall_cpu_info_t* out_info = (syscall_cpu_info_t*) frame->rdi;
            if (!out_info)
                return (uint64_t) -1;

            out_info->cpu_index = cpu_index;
            out_info->apic_id = apic_id;
            out_info->online_cpus = SMP_get_online_cpu_count();
            out_info->tick_hz = ISR_get_tick_hz();
            out_info->ticks = ISR_get_timer_ticks();
            return 0;
        }

        case SYS_SCHED_INFO_GET:
        {
            syscall_sched_info_t* out_info = (syscall_sched_info_t*) frame->rdi;
            if (!out_info)
                return (uint64_t) -1;

            out_info->current_cpu = cpu_index;
            out_info->preempt_count = task_get_preempt_count();
            out_info->local_rq_depth = task_runqueue_depth();
            out_info->total_rq_depth = task_runqueue_depth_total();
            return 0;
        }

        case SYS_AHCI_IRQ_INFO_GET:
        {
            syscall_ahci_irq_info_t* out_info = (syscall_ahci_irq_info_t*) frame->rdi;
            if (!out_info)
                return (uint64_t) -1;

            out_info->mode = AHCI_get_irq_mode();
            out_info->reserved = 0;
            out_info->count = AHCI_get_irq_count();
            return 0;
        }

        case SYS_RCU_SYNC:
            return RCU_synchronize() ? 0 : (uint64_t) -1;

        case SYS_RCU_INFO_GET:
        {
            syscall_rcu_info_t* out_info = (syscall_rcu_info_t*) frame->rdi;
            if (!out_info)
                return (uint64_t) -1;

            rcu_stats_t stats = { 0 };
            RCU_get_stats(&stats);
            out_info->gp_seq = stats.gp_seq;
            out_info->gp_target = stats.gp_target;
            out_info->callbacks_pending = stats.callbacks_pending;
            out_info->local_read_depth = stats.local_read_depth;
            out_info->local_preempt_count = stats.local_preempt_count;
            return 0;
        }

        default:
            return (uint64_t) -1;
    }
}
