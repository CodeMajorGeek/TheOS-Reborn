#include <CPU/SMP.h>

#include <CPU/APIC.h>
#include <CPU/FPU.h>
#include <CPU/GDT.h>
#include <CPU/IDT.h>
#include <CPU/Syscall.h>
#include <CPU/x86.h>
#include <Debug/KDebug.h>
#include <Memory/VMM.h>
#include <Task/Task.h>

void SMP_ap_entry(uintptr_t handoff_phys)
{
    volatile SMP_handoff_t* handoff = (volatile SMP_handoff_t*) P2V(handoff_phys);

    cli();

    GDT_load_kernel_segments();
    IDT_load();
    VMM_enable_nx_current_cpu();

    uint32_t cpu_index = handoff->cpu_index;
    uint8_t apic_id = (uint8_t) handoff->apic_id;
    uintptr_t stack_top = (uintptr_t) handoff->stack_top;

    kdebug_printf("[SMP] ap_entry start apic_id=%u cpu=%u stack=0x%llX\n",
                  apic_id,
                  cpu_index,
                  (unsigned long long) stack_top);

    if (!FPU_init_cpu(cpu_index))
        goto ap_idle;

    APIC_enable();
    APIC_send_EOI();

    if (!task_init_cpu(cpu_index, stack_top, apic_id))
        goto ap_idle;

    Syscall_init();

    uint8_t current_apic_id = APIC_get_current_lapic_id();
    if (current_apic_id != 0)
        apic_id = current_apic_id;

    SMP_notify_ap_ready(cpu_index, apic_id);
    __asm__ __volatile__("" : : : "memory");
    handoff->ready = 1;

    sti();

    kdebug_printf("[SMP] AP online apic_id=%u cpu=%u\n", apic_id, cpu_index);

ap_idle:
    task_idle_loop();
}
