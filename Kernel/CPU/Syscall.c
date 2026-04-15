#include <CPU/Syscall.h>
#include <CPU/Syscall_private.h>

#include <CPU/ACPI.h>
#include <CPU/APIC.h>
#include <CPU/ISR.h>
#include <CPU/MSR.h>
#include <CPU/SMP.h>
#include <Device/HPET.h>
#include <Device/Keyboard.h>
#include <Device/Mouse.h>
#include <Device/DRM.h>
#include <Device/E1000.h>
#include <Device/HDA.h>
#include <Debug/KDebug.h>
#include <Debug/Spinlock.h>
#include <Memory/KMem.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>
#include <Network/ARP.h>
#include <Network/Socket.h>
#include <Network/TCP.h>
#include <Network/Unix.h>
#include <Storage/AHCI.h>
#include <Storage/VFS.h>
#include <Task/RCU.h>
#include <Task/Task.h>
#include <UAPI/Net.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static syscall_runtime_state_t Syscall_state = {
    .user_map_hint = SYSCALL_MAP_HINT_BASE,
    .next_pid = 1U
};
static uint32_t Syscall_bootstrap_domain = SYS_PROC_DOMAIN_USERLAND;
static uint64_t Syscall_debug_tick_calls = 0;
static uint64_t Syscall_debug_tick_runnable_le1 = 0;
static uint64_t Syscall_debug_need_resched_set = 0;
static uint64_t Syscall_debug_preempt_attempts = 0;
static uint64_t Syscall_debug_preempt_success = 0;
static uint64_t Syscall_debug_preempt_no_flag = 0;
static uint64_t Syscall_debug_preempt_no_current = 0;
static uint64_t Syscall_debug_preempt_same_slot = 0;
static uint64_t Syscall_debug_preempt_invalid_next = 0;
static uint64_t Syscall_debug_post_calls = 0;
static uint64_t Syscall_debug_post_pick_next = 0;
static uint64_t Syscall_debug_post_switch = 0;
static uint64_t Syscall_debug_need_resched_set_cpu[4] = { 0, 0, 0, 0 };
static uint64_t Syscall_debug_preempt_attempt_cpu[4] = { 0, 0, 0, 0 };
static uint64_t Syscall_debug_preempt_success_cpu[4] = { 0, 0, 0, 0 };
static uint64_t Syscall_debug_preempt_no_flag_cpu[4] = { 0, 0, 0, 0 };
static uint64_t Syscall_debug_irq_calls_cpu[4] = { 0, 0, 0, 0 };
static uint64_t Syscall_debug_post_calls_cpu[4] = { 0, 0, 0, 0 };
static uint64_t Syscall_debug_post_switch_cpu[4] = { 0, 0, 0, 0 };
static uint64_t Syscall_debug_nr_read = 0;
static uint64_t Syscall_debug_nr_write = 0;
static uint64_t Syscall_debug_nr_sleep = 0;
static uint64_t Syscall_debug_nr_other = 0;
static uint64_t Syscall_debug_flag_consume_timer_cpu[4] = { 0, 0, 0, 0 };
static uint64_t Syscall_debug_flag_consume_post_cpu[4] = { 0, 0, 0, 0 };
static uint64_t Syscall_debug_flag_set_sleep_cpu[4] = { 0, 0, 0, 0 };
static uint64_t Syscall_debug_idle_dispatch_attempts = 0;
static uint64_t Syscall_debug_idle_dispatch_success = 0;
static uint64_t Syscall_debug_idle_dispatch_rejected_running = 0;
static uint64_t Syscall_debug_idle_dispatch_rejected_empty = 0;
static uint64_t Syscall_debug_idle_dispatch_attempt_cpu[4] = { 0, 0, 0, 0 };
static uint64_t Syscall_debug_idle_dispatch_success_cpu[4] = { 0, 0, 0, 0 };
static uint8_t Syscall_idle_claimed_slots[SYSCALL_MAX_PROCS] = { 0 };

__attribute__((__noreturn__)) void Syscall_resume_user_context(const syscall_user_resume_context_t* ctx);

#define SYSCALL_KBD_INJECT_QUEUE_CAP 256U
typedef struct syscall_kbd_inject_entry
{
    uint32_t pid;
    uint8_t scancode;
} syscall_kbd_inject_entry_t;
static syscall_kbd_inject_entry_t Syscall_kbd_inject_queue[SYSCALL_KBD_INJECT_QUEUE_CAP];
static uint32_t Syscall_kbd_inject_count = 0U;
static spinlock_t Syscall_kbd_inject_lock;
static bool Syscall_kbd_inject_lock_ready = false;
static uint64_t Syscall_kbd_inject_push_ok = 0ULL;
static uint64_t Syscall_kbd_inject_push_drop = 0ULL;
static uint64_t Syscall_kbd_inject_pop_ok = 0ULL;
static uint32_t Syscall_kbd_inject_targets[32] = { 0U };
static uint64_t Syscall_kbd_inject_target_hits = 0ULL;
static uint32_t Syscall_kbd_hardware_capture_pid = 0U;

static void Syscall_kbd_inject_register_target(uint32_t pid)
{
    if (!Syscall_kbd_inject_lock_ready || pid == 0U)
        return;

    uint64_t flags = spin_lock_irqsave(&Syscall_kbd_inject_lock);
    for (uint32_t i = 0U; i < 32U; i++)
    {
        if (Syscall_kbd_inject_targets[i] == pid)
        {
            spin_unlock_irqrestore(&Syscall_kbd_inject_lock, flags);
            return;
        }
    }
    for (uint32_t i = 0U; i < 32U; i++)
    {
        if (Syscall_kbd_inject_targets[i] == 0U)
        {
            Syscall_kbd_inject_targets[i] = pid;
            spin_unlock_irqrestore(&Syscall_kbd_inject_lock, flags);
            return;
        }
    }
    spin_unlock_irqrestore(&Syscall_kbd_inject_lock, flags);
}

static bool Syscall_kbd_inject_is_target(uint32_t pid)
{
    if (!Syscall_kbd_inject_lock_ready || pid == 0U)
        return false;
    bool found = false;
    uint64_t flags = spin_lock_irqsave(&Syscall_kbd_inject_lock);
    for (uint32_t i = 0U; i < 32U; i++)
    {
        if (Syscall_kbd_inject_targets[i] == pid)
        {
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&Syscall_kbd_inject_lock, flags);
    return found;
}

static bool Syscall_kbd_inject_push(uint32_t pid, uint8_t scancode)
{
    if (!Syscall_kbd_inject_lock_ready || pid == 0U)
        return false;

    uint64_t flags = spin_lock_irqsave(&Syscall_kbd_inject_lock);
    if (Syscall_kbd_inject_count >= SYSCALL_KBD_INJECT_QUEUE_CAP)
    {
        Syscall_kbd_inject_push_drop++;
        spin_unlock_irqrestore(&Syscall_kbd_inject_lock, flags);
        return false;
    }

    Syscall_kbd_inject_queue[Syscall_kbd_inject_count].pid = pid;
    Syscall_kbd_inject_queue[Syscall_kbd_inject_count].scancode = scancode;
    Syscall_kbd_inject_count++;
    Syscall_kbd_inject_push_ok++;
    spin_unlock_irqrestore(&Syscall_kbd_inject_lock, flags);
    return true;
}

static bool Syscall_kbd_inject_pop_for_pid(uint32_t pid, uint8_t* out_scancode)
{
    if (!Syscall_kbd_inject_lock_ready || pid == 0U || !out_scancode)
        return false;

    uint64_t flags = spin_lock_irqsave(&Syscall_kbd_inject_lock);
    for (uint32_t i = 0U; i < Syscall_kbd_inject_count; i++)
    {
        if (Syscall_kbd_inject_queue[i].pid != pid)
            continue;

        *out_scancode = Syscall_kbd_inject_queue[i].scancode;
        for (uint32_t j = i + 1U; j < Syscall_kbd_inject_count; j++)
            Syscall_kbd_inject_queue[j - 1U] = Syscall_kbd_inject_queue[j];
        Syscall_kbd_inject_count--;
        Syscall_kbd_inject_pop_ok++;
        spin_unlock_irqrestore(&Syscall_kbd_inject_lock, flags);
        return true;
    }
    spin_unlock_irqrestore(&Syscall_kbd_inject_lock, flags);
    return false;
}

static inline uintptr_t Syscall_align_up_page(uintptr_t value)
{
    return (value + (SYSCALL_PAGE_SIZE - 1U)) & ~(uintptr_t) (SYSCALL_PAGE_SIZE - 1U);
}

static inline uintptr_t Syscall_align_down_page(uintptr_t value)
{
    return value & ~(uintptr_t) (SYSCALL_PAGE_SIZE - 1U);
}

static inline uintptr_t Syscall_align_up_pow2(uintptr_t value, uintptr_t align)
{
    if (align == 0 || (align & (align - 1U)) != 0)
        return 0;
    if (value > UINTPTR_MAX - (align - 1U))
        return 0;
    return (value + (align - 1U)) & ~(uintptr_t) (align - 1U);
}

static uint16_t Syscall_read_be16(const uint8_t* bytes)
{
    if (!bytes)
        return 0U;

    return (uint16_t) (((uint16_t) bytes[0] << 8) | (uint16_t) bytes[1]);
}

static uint32_t Syscall_read_be32(const uint8_t* bytes)
{
    if (!bytes)
        return 0U;

    return ((uint32_t) bytes[0] << 24) |
           ((uint32_t) bytes[1] << 16) |
           ((uint32_t) bytes[2] << 8) |
           (uint32_t) bytes[3];
}

static void Syscall_write_be16(uint8_t* bytes, uint16_t value)
{
    if (!bytes)
        return;

    bytes[0] = (uint8_t) (value >> 8);
    bytes[1] = (uint8_t) value;
}

static void Syscall_write_be32(uint8_t* bytes, uint32_t value)
{
    if (!bytes)
        return;

    bytes[0] = (uint8_t) (value >> 24);
    bytes[1] = (uint8_t) (value >> 16);
    bytes[2] = (uint8_t) (value >> 8);
    bytes[3] = (uint8_t) value;
}

static bool Syscall_user_range_in_bounds(uintptr_t user_ptr, size_t size)
{
    if (size == 0)
        return false;

    if (user_ptr < SYSCALL_USER_VADDR_MIN || user_ptr > SYSCALL_USER_VADDR_MAX)
        return false;

    uintptr_t end = user_ptr + (uintptr_t) size - 1U;
    if (end < user_ptr || end > SYSCALL_USER_VADDR_MAX)
        return false;

    return true;
}

static bool Syscall_mmap_window_in_bounds(uintptr_t base, size_t size)
{
    if (!Syscall_user_range_in_bounds(base, size))
        return false;

    uintptr_t end = base + (uintptr_t) size - 1U;
    if (base < SYSCALL_MAP_HINT_BASE)
        return false;
    if (end >= SYSCALL_MAP_HINT_LIMIT)
        return false;

    return true;
}

static bool Syscall_copy_to_user(void* user_dst, const void* kernel_src, size_t size)
{
    if (size == 0)
        return true;
    if (!user_dst || !kernel_src)
        return false;
    if (!Syscall_user_range_in_bounds((uintptr_t) user_dst, size))
        return false;

    if (Syscall_state.vm_lock_ready)
        spin_lock(&Syscall_state.vm_lock);

    size_t copied = 0;
    while (copied < size)
    {
        uintptr_t user_addr = (uintptr_t) user_dst + copied;
        uintptr_t page = user_addr & ~(uintptr_t) (SYSCALL_PAGE_SIZE - 1U);
        if (!VMM_is_user_accessible(page))
        {
            if (Syscall_state.vm_lock_ready)
                spin_unlock(&Syscall_state.vm_lock);
            return false;
        }

        uintptr_t current_cr3 = Syscall_read_cr3_phys();
        uint64_t* pte = Syscall_get_user_pte_ptr(current_cr3, page);
        if (!pte)
        {
            if (Syscall_state.vm_lock_ready)
                spin_unlock(&Syscall_state.vm_lock);
            return false;
        }

        uintptr_t entry = *pte;
        bool pte_updated = false;
        if ((entry & SYSCALL_PTE_COW) != 0)
        {
            uintptr_t old_phys = entry & FRAME;
            if (old_phys == 0)
            {
                if (Syscall_state.vm_lock_ready)
                    spin_unlock(&Syscall_state.vm_lock);
                return false;
            }

            uint32_t refs = Syscall_cow_ref_get(old_phys);
            if (refs <= 1U)
            {
                bool dummy_zero = false;
                (void) Syscall_cow_ref_sub(old_phys, &dummy_zero);
                entry &= ~SYSCALL_PTE_COW;
                entry |= WRITABLE;
                *pte = entry;
                pte_updated = true;
            }
            else
            {
                uintptr_t new_phys = (uintptr_t) PMM_alloc_page();
                if (new_phys == 0)
                {
                    if (Syscall_state.vm_lock_ready)
                        spin_unlock(&Syscall_state.vm_lock);
                    return false;
                }

                memcpy((void*) P2V(new_phys), (const void*) P2V(old_phys), SYSCALL_PAGE_SIZE);
                entry &= ~FRAME;
                entry |= new_phys;
                entry &= ~SYSCALL_PTE_COW;
                entry |= WRITABLE;
                *pte = entry;
                pte_updated = true;

                bool ref_zero = false;
                if (Syscall_cow_ref_sub(old_phys, &ref_zero) && ref_zero)
                    PMM_dealloc_page((void*) old_phys);
            }

            /* We are about to write into the same user page. Make the new PTE
             * visible immediately to avoid writing through a stale read-only TLB entry. */
            if (pte_updated)
                Syscall_write_cr3_phys(current_cr3);

            entry = *pte;
        }

        if ((entry & WRITABLE) == 0)
        {
            if (Syscall_state.vm_lock_ready)
                spin_unlock(&Syscall_state.vm_lock);
            return false;
        }

        size_t page_remaining = SYSCALL_PAGE_SIZE - (size_t) (user_addr & (SYSCALL_PAGE_SIZE - 1U));
        size_t chunk = size - copied;
        if (chunk > page_remaining)
            chunk = page_remaining;

        memcpy((void*) user_addr, (const uint8_t*) kernel_src + copied, chunk);
        copied += chunk;
    }

    if (Syscall_state.vm_lock_ready)
        spin_unlock(&Syscall_state.vm_lock);
    return true;
}

static bool Syscall_copy_from_user(void* kernel_dst, const void* user_src, size_t size)
{
    if (size == 0)
        return true;
    if (!kernel_dst || !user_src)
        return false;
    if (!Syscall_user_range_in_bounds((uintptr_t) user_src, size))
        return false;

    if (Syscall_state.vm_lock_ready)
        spin_lock(&Syscall_state.vm_lock);

    size_t copied = 0;
    while (copied < size)
    {
        uintptr_t user_addr = (uintptr_t) user_src + copied;
        uintptr_t page = user_addr & ~(uintptr_t) (SYSCALL_PAGE_SIZE - 1U);
        if (!VMM_is_user_accessible(page))
        {
            if (Syscall_state.vm_lock_ready)
                spin_unlock(&Syscall_state.vm_lock);
            return false;
        }

        size_t page_remaining = SYSCALL_PAGE_SIZE - (size_t) (user_addr & (SYSCALL_PAGE_SIZE - 1U));
        size_t chunk = size - copied;
        if (chunk > page_remaining)
            chunk = page_remaining;

        memcpy((uint8_t*) kernel_dst + copied, (const void*) user_addr, chunk);
        copied += chunk;
    }

    if (Syscall_state.vm_lock_ready)
        spin_unlock(&Syscall_state.vm_lock);
    return true;
}

static bool Syscall_copy_in_sockaddr_in(const void* user_addr, size_t user_len, uint32_t* out_addr_be, uint16_t* out_port)
{
    if (!user_addr || !out_addr_be || !out_port || user_len < sizeof(sys_sockaddr_t))
        return false;

    sys_sockaddr_t addr;
    if (!Syscall_copy_from_user(&addr, user_addr, sizeof(addr)))
        return false;
    if (addr.sa_family != AF_INET)
        return false;

    *out_port = Syscall_read_be16(&addr.sa_data[0]);
    *out_addr_be = Syscall_read_be32(&addr.sa_data[2]);
    return true;
}

static bool Syscall_copy_out_sockaddr_in(void* user_addr, void* user_addrlen_ptr, uint32_t addr_be, uint16_t port)
{
    if (!user_addr || !user_addrlen_ptr)
        return false;

    uint32_t user_len = 0U;
    if (!Syscall_copy_from_user(&user_len, user_addrlen_ptr, sizeof(user_len)))
        return false;

    sys_sockaddr_t out_addr;
    memset(&out_addr, 0, sizeof(out_addr));
    out_addr.sa_family = AF_INET;
    Syscall_write_be16(&out_addr.sa_data[0], port);
    Syscall_write_be32(&out_addr.sa_data[2], addr_be);

    uint32_t required_len = sizeof(out_addr);
    uint32_t copy_len = (user_len < required_len) ? user_len : required_len;
    if (copy_len != 0U && !Syscall_copy_to_user(user_addr, &out_addr, copy_len))
        return false;
    if (!Syscall_copy_to_user(user_addrlen_ptr, &required_len, sizeof(required_len)))
        return false;

    return true;
}

static bool Syscall_read_user_cstr(char* kernel_dst, size_t kernel_dst_size, const char* user_src)
{
    if (!kernel_dst || kernel_dst_size < 2 || !user_src)
        return false;

    if (Syscall_state.vm_lock_ready)
        spin_lock(&Syscall_state.vm_lock);

    for (size_t i = 0; i < kernel_dst_size - 1; i++)
    {
        uintptr_t user_addr = (uintptr_t) (user_src + i);
        if (!Syscall_user_range_in_bounds(user_addr, 1))
        {
            if (Syscall_state.vm_lock_ready)
                spin_unlock(&Syscall_state.vm_lock);
            return false;
        }

        uintptr_t page = user_addr & ~(uintptr_t) (SYSCALL_PAGE_SIZE - 1U);
        if (!VMM_is_user_accessible(page))
        {
            if (Syscall_state.vm_lock_ready)
                spin_unlock(&Syscall_state.vm_lock);
            return false;
        }

        char c = user_src[i];
        kernel_dst[i] = c;
        if (c == '\0')
        {
            if (Syscall_state.vm_lock_ready)
                spin_unlock(&Syscall_state.vm_lock);
            return true;
        }
    }

    if (Syscall_state.vm_lock_ready)
        spin_unlock(&Syscall_state.vm_lock);
    kernel_dst[kernel_dst_size - 1] = '\0';
    return false;
}

static void Syscall_exec_free_vec(char** vec, size_t count)
{
    if (!vec)
        return;

    for (size_t i = 0; i < count; i++)
    {
        if (vec[i])
        {
            kfree(vec[i]);
            vec[i] = NULL;
        }
    }
}

static bool Syscall_exec_read_user_vec(const char* const* user_vec,
                                       char** out_vec,
                                       size_t max_items,
                                       size_t* out_count)
{
    if (!out_vec || !out_count || max_items == 0)
        return false;

    for (size_t i = 0; i < max_items; i++)
        out_vec[i] = NULL;
    *out_count = 0;

    if (!user_vec)
        return true;

    for (size_t i = 0; i < max_items; i++)
    {
        uintptr_t user_item_ptr = 0;
        if (!Syscall_copy_from_user(&user_item_ptr, user_vec + i, sizeof(user_item_ptr)))
            return false;

        if (user_item_ptr == 0)
        {
            *out_count = i;
            return true;
        }

        char temp[SYSCALL_USER_CSTR_MAX];
        if (!Syscall_read_user_cstr(temp, sizeof(temp), (const char*) user_item_ptr))
            return false;

        size_t len = strlen(temp) + 1U;
        char* copy = (char*) kmalloc(len);
        if (!copy)
            return false;

        memcpy(copy, temp, len);
        out_vec[i] = copy;
        *out_count = i + 1U;
    }

    // Require a NULL terminator in user-provided argv/envp vectors.
    return false;
}

static bool Syscall_exec_build_initial_stack(uintptr_t* inout_rsp,
                                             const char* default_argv0,
                                             char* const* argv,
                                             size_t argc,
                                             char* const* envp,
                                             size_t envc)
{
    if (!inout_rsp || argc > SYSCALL_EXEC_MAX_ARGS || envc > SYSCALL_EXEC_MAX_ENVP)
        return false;

    const char* argv_items[SYSCALL_EXEC_MAX_ARGS];
    uintptr_t argv_ptrs[SYSCALL_EXEC_MAX_ARGS];
    uintptr_t env_ptrs[SYSCALL_EXEC_MAX_ENVP];

    size_t argc_effective = argc;
    for (size_t i = 0; i < argc; i++)
    {
        if (!argv || !argv[i] || argv[i][0] == '\0')
            return false;
        argv_items[i] = argv[i];
    }

    if (argc_effective == 0)
    {
        if (!default_argv0 || default_argv0[0] == '\0')
            return false;
        argv_items[0] = default_argv0;
        argc_effective = 1;
    }

    uintptr_t stack_bottom = SYSCALL_ELF_STACK_TOP - SYSCALL_ELF_STACK_SIZE;
    uintptr_t sp = *inout_rsp;

    for (size_t i = envc; i > 0; i--)
    {
        const char* value = envp[i - 1U];
        if (!value)
            return false;

        size_t len = strlen(value) + 1U;
        if (sp < stack_bottom + len)
            return false;

        sp -= len;
        memcpy((void*) sp, value, len);
        env_ptrs[i - 1U] = sp;
    }

    for (size_t i = argc_effective; i > 0; i--)
    {
        const char* value = argv_items[i - 1U];
        size_t len = strlen(value) + 1U;
        if (sp < stack_bottom + len)
            return false;

        sp -= len;
        memcpy((void*) sp, value, len);
        argv_ptrs[i - 1U] = sp;
    }

    sp &= ~(uintptr_t) 0xFULL;

    size_t words = argc_effective + envc + 3U;
    if ((words & 1U) != 0)
    {
        if (sp < stack_bottom + sizeof(uint64_t))
            return false;
        sp -= sizeof(uint64_t);
        *((uint64_t*) sp) = 0;
    }

    if (sp < stack_bottom + sizeof(uint64_t))
        return false;
    sp -= sizeof(uint64_t);
    *((uint64_t*) sp) = 0;

    for (size_t i = envc; i > 0; i--)
    {
        if (sp < stack_bottom + sizeof(uint64_t))
            return false;
        sp -= sizeof(uint64_t);
        *((uint64_t*) sp) = env_ptrs[i - 1U];
    }

    if (sp < stack_bottom + sizeof(uint64_t))
        return false;
    sp -= sizeof(uint64_t);
    *((uint64_t*) sp) = 0;

    for (size_t i = argc_effective; i > 0; i--)
    {
        if (sp < stack_bottom + sizeof(uint64_t))
            return false;
        sp -= sizeof(uint64_t);
        *((uint64_t*) sp) = argv_ptrs[i - 1U];
    }

    if (sp < stack_bottom + sizeof(uint64_t))
        return false;
    sp -= sizeof(uint64_t);
    *((uint64_t*) sp) = (uint64_t) argc_effective;

    *inout_rsp = sp;
    return true;
}

static bool Syscall_exec_install_initial_stack(uintptr_t new_cr3,
                                               uintptr_t* inout_rsp,
                                               const char* default_argv0,
                                               char* const* argv,
                                               size_t argc,
                                               char* const* envp,
                                               size_t envc)
{
    if (!inout_rsp || new_cr3 == 0)
        return false;

    uintptr_t previous_cr3 = Syscall_read_cr3_phys();
    if (previous_cr3 != new_cr3)
        Syscall_write_cr3_phys(new_cr3);

    bool ok = Syscall_exec_build_initial_stack(inout_rsp,
                                               default_argv0,
                                               argv,
                                               argc,
                                               envp,
                                               envc);

    if (previous_cr3 != new_cr3)
        Syscall_write_cr3_phys(previous_cr3);
    return ok;
}

static bool Syscall_user_range_unmapped(uintptr_t base, size_t size)
{
    if (!Syscall_user_range_in_bounds(base, size))
        return false;

    uintptr_t end = base + (uintptr_t) size;
    for (uintptr_t page = base; page < end; page += SYSCALL_PAGE_SIZE)
    {
        uintptr_t phys = 0;
        if (VMM_virt_to_phys(page, &phys))
            return false;
    }

    return true;
}

static bool Syscall_find_free_user_range(size_t page_count,
                                         uintptr_t start,
                                         uintptr_t end_exclusive,
                                         uintptr_t* out_base)
{
    if (!out_base || page_count == 0 || start >= end_exclusive)
        return false;

    uintptr_t span = page_count * SYSCALL_PAGE_SIZE;
    if (span == 0 || span > (end_exclusive - start))
        return false;

    uintptr_t candidate = Syscall_align_up_page(start);
    if (candidate >= end_exclusive)
        return false;

    uintptr_t last_start = end_exclusive - span;
    while (candidate <= last_start)
    {
        if (Syscall_user_range_unmapped(candidate, span))
        {
            *out_base = candidate;
            return true;
        }

        candidate += SYSCALL_PAGE_SIZE;
    }

    return false;
}

static bool Syscall_pick_user_map_base(size_t page_count, uintptr_t* out_base)
{
    if (!out_base || page_count == 0)
        return false;

    uintptr_t hint = __atomic_load_n(&Syscall_state.user_map_hint, __ATOMIC_RELAXED);
    if (hint < SYSCALL_MAP_HINT_BASE || hint >= SYSCALL_MAP_HINT_LIMIT)
        hint = SYSCALL_MAP_HINT_BASE;

    bool found = Syscall_find_free_user_range(page_count, hint, SYSCALL_MAP_HINT_LIMIT, out_base);
    if (!found)
        found = Syscall_find_free_user_range(page_count, SYSCALL_MAP_HINT_BASE, hint, out_base);

    if (!found)
        return false;

    uintptr_t next_hint = *out_base + (page_count * SYSCALL_PAGE_SIZE);
    if (next_hint < *out_base || next_hint >= SYSCALL_MAP_HINT_LIMIT)
        next_hint = SYSCALL_MAP_HINT_BASE;
    __atomic_store_n(&Syscall_state.user_map_hint, next_hint, __ATOMIC_RELAXED);
    return true;
}

static bool Syscall_pick_user_map_base_from_hint(size_t page_count,
                                                 uintptr_t hint,
                                                 uintptr_t* out_base)
{
    if (!out_base || page_count == 0)
        return false;

    if (hint < SYSCALL_MAP_HINT_BASE || hint >= SYSCALL_MAP_HINT_LIMIT)
        hint = SYSCALL_MAP_HINT_BASE;

    bool found = Syscall_find_free_user_range(page_count, hint, SYSCALL_MAP_HINT_LIMIT, out_base);
    if (!found)
        found = Syscall_find_free_user_range(page_count, SYSCALL_MAP_HINT_BASE, hint, out_base);

    if (!found)
        return false;

    uintptr_t next_hint = *out_base + (page_count * SYSCALL_PAGE_SIZE);
    if (next_hint < *out_base || next_hint >= SYSCALL_MAP_HINT_LIMIT)
        next_hint = SYSCALL_MAP_HINT_BASE;
    __atomic_store_n(&Syscall_state.user_map_hint, next_hint, __ATOMIC_RELAXED);
    return true;
}

static bool Syscall_normalize_write_path(const char* path, char* out_path, size_t out_size)
{
    if (!path || !out_path || out_size < 2)
        return false;

    char components[SYSCALL_PATH_MAX_COMPONENTS][SYSCALL_PATH_COMPONENT_MAX + 1U];
    size_t component_count = 0;
    const char* cursor = path;
    while (*cursor == '/')
        cursor++;

    while (*cursor != '\0')
    {
        char token[SYSCALL_PATH_COMPONENT_MAX + 1U];
        size_t token_len = 0;
        while (*cursor != '\0' && *cursor != '/')
        {
            if (token_len >= SYSCALL_PATH_COMPONENT_MAX)
                return false;
            token[token_len++] = *cursor++;
        }
        token[token_len] = '\0';

        while (*cursor == '/')
            cursor++;

        if (token_len == 0)
            continue;
        if (strcmp(token, ".") == 0)
            continue;
        if (strcmp(token, "..") == 0)
        {
            if (component_count > 0U)
                component_count--;
            continue;
        }

        if (component_count >= SYSCALL_PATH_MAX_COMPONENTS)
            return false;
        memcpy(components[component_count], token, token_len + 1U);
        component_count++;
    }

    if (component_count == 0)
        return false;

    size_t len = 0;
    out_path[len++] = '/';
    out_path[len] = '\0';
    for (size_t i = 0; i < component_count; i++)
    {
        size_t part_len = strlen(components[i]);
        if (len > 1U)
        {
            if (len + 1U >= out_size)
                return false;
            out_path[len++] = '/';
        }

        if (len + part_len >= out_size)
            return false;
        memcpy(out_path + len, components[i], part_len);
        len += part_len;
        out_path[len] = '\0';
    }

    return true;
}

static bool Syscall_is_drm_card_path(const char* path)
{
    if (!path)
        return false;

    return strcmp(path, DRM_NODE_PATH) == 0;
}

static bool Syscall_is_audio_dsp_path(const char* path)
{
    if (!path)
        return false;

    return strcmp(path, HDA_DSP_NODE_PATH) == 0 ||
           strcmp(path, HDA_AUDIO_NODE_PATH) == 0;
}

static bool Syscall_is_net_raw_path(const char* path)
{
    if (!path)
        return false;

    return strcmp(path, E1000_NET_NODE_PATH) == 0;
}

static bool Syscall_path_is_driverland_binary(const char* path)
{
    if (!path)
        return false;

    if (strcmp(path, "/drv") == 0)
        return true;
    if (strcmp(path, "/bin/TheApp") == 0)
        return true;

    const char prefix[] = "/drv/";
    return strncmp(path, prefix, sizeof(prefix) - 1U) == 0;
}

static uint32_t Syscall_process_domain_from_exec_path(const char* path)
{
    return Syscall_path_is_driverland_binary(path) ?
           SYS_PROC_DOMAIN_DRIVERLAND :
           SYS_PROC_DOMAIN_USERLAND;
}

static uint32_t Syscall_proc_owner_domain_locked(uint32_t owner_pid)
{
    if (owner_pid == 0U)
        return SYS_PROC_DOMAIN_USERLAND;

    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        const syscall_process_t* proc = &Syscall_state.procs[i];
        if (!proc->used || proc->is_thread)
            continue;
        if (proc->pid == owner_pid)
            return proc->domain;
    }

    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        const syscall_process_t* proc = &Syscall_state.procs[i];
        if (!proc->used)
            continue;
        if (proc->owner_pid == owner_pid)
            return proc->domain;
    }

    return SYS_PROC_DOMAIN_USERLAND;
}

static bool Syscall_proc_owner_is_driverland(uint32_t owner_pid)
{
    if (!Syscall_state.proc_lock_ready)
        return false;

    uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    uint32_t domain = Syscall_proc_owner_domain_locked(owner_pid);
    spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
    return domain == SYS_PROC_DOMAIN_DRIVERLAND;
}

static int32_t Syscall_fd_alloc_locked(void)
{
    for (uint32_t i = 0; i < SYSCALL_MAX_OPEN_FILES; i++)
    {
        if (!Syscall_state.fds[i].used)
            return (int32_t) i;
    }

    return -1;
}

static bool Syscall_fd_path_conflicts_locked(const char* path, bool want_exclusive)
{
    if (!path || path[0] == '\0')
        return false;

    for (uint32_t i = 0; i < SYSCALL_MAX_OPEN_FILES; i++)
    {
        const syscall_file_desc_t* entry = &Syscall_state.fds[i];
        if (!entry->used || entry->path[0] == '\0')
            continue;
        if (strcmp(entry->path, path) != 0)
            continue;

        if (want_exclusive || entry->exclusive)
            return true;
    }

    return false;
}

static uint64_t Syscall_handle_socket(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (owner_pid == 0U)
        return (uint64_t) -1;

    int domain = (int) frame->rdi;
    int type = (int) frame->rsi;
    int protocol = (int) frame->rdx;

    uint32_t socket_id = 0U;
    uint32_t fd_type = SYSCALL_FD_TYPE_NONE;

    if (domain == (int) AF_UNIX)
    {
        if (type == (int) NET_UNIX_SOCK_STREAM || type == (int) NET_UNIX_SOCK_DGRAM)
        {
            if (!NET_unix_create(owner_pid, (uint32_t) type, &socket_id))
                return (uint64_t) -1;
            fd_type = SYSCALL_FD_TYPE_NET_UNIX_SOCKET;
        }
        else
            return (uint64_t) -1;
    }
    else if (domain == (int) NET_SOCKET_AF_INET)
    {
        if (type == (int) NET_SOCKET_SOCK_DGRAM &&
            (protocol == 0 || protocol == (int) NET_SOCKET_IPPROTO_UDP))
        {
            if (!NET_socket_create_udp(owner_pid, &socket_id))
                return (uint64_t) -1;
            fd_type = SYSCALL_FD_TYPE_NET_UDP_SOCKET;
        }
        else if (type == (int) NET_TCP_SOCK_STREAM &&
                 (protocol == 0 || protocol == (int) NET_TCP_IPPROTO_TCP))
        {
            if (!NET_tcp_create(owner_pid, &socket_id))
                return (uint64_t) -1;
            fd_type = SYSCALL_FD_TYPE_NET_TCP_SOCKET;
        }
        else
            return (uint64_t) -1;
    }
    else
    {
        return (uint64_t) -1;
    }

    spin_lock(&Syscall_state.fd_lock);
    int32_t fd = Syscall_fd_alloc_locked();
    if (fd < 0)
    {
        spin_unlock(&Syscall_state.fd_lock);
        if (fd_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
            (void) NET_socket_close_udp(owner_pid, socket_id);
        else if (fd_type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
            (void) NET_tcp_close(owner_pid, socket_id);
        else if (fd_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
            (void) NET_unix_close(owner_pid, socket_id);
        return (uint64_t) -1;
    }

    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->type = fd_type;
    entry->owner_pid = owner_pid;
    entry->can_read = true;
    entry->can_write = true;
    entry->net_socket_id = socket_id;
    spin_unlock(&Syscall_state.fd_lock);
    return (uint64_t) fd;
}

static uint64_t Syscall_handle_bind(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    const void* user_addr = (const void*) frame->rsi;
    size_t addr_len = (size_t) frame->rdx;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0U)
        return (uint64_t) -1;

    uint32_t socket_id = 0U;
    uint32_t fd_type = SYSCALL_FD_TYPE_NONE;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    fd_type = entry->type;
    if (fd_type != SYSCALL_FD_TYPE_NET_UDP_SOCKET &&
        fd_type != SYSCALL_FD_TYPE_NET_TCP_SOCKET &&
        fd_type != SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    socket_id = entry->net_socket_id;
    spin_unlock(&Syscall_state.fd_lock);

    if (fd_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
    {
        if (!user_addr || addr_len < 4U)
            return (uint64_t) -1;
        sys_sockaddr_t hdr;
        if (!Syscall_copy_from_user(&hdr, user_addr, sizeof(hdr)))
            return (uint64_t) -1;
        if (hdr.sa_family != AF_UNIX)
            return (uint64_t) -1;
        size_t path_len = addr_len - 2U;
        if (path_len == 0 || path_len >= NET_UNIX_PATH_MAX)
            return (uint64_t) -1;
        char path[NET_UNIX_PATH_MAX];
        if (!Syscall_copy_from_user(path, (const uint8_t*) user_addr + 2U, path_len))
            return (uint64_t) -1;
        path[path_len] = '\0';
        size_t real_len = 0;
        while (real_len < path_len && path[real_len] != '\0')
            real_len++;
        if (real_len == 0)
            return (uint64_t) -1;
        return NET_unix_bind(owner_pid, socket_id, path, real_len) ? 0 : (uint64_t) -1;
    }

    uint32_t local_addr_be = 0U;
    uint16_t local_port = 0U;
    if (!Syscall_copy_in_sockaddr_in(user_addr, addr_len, &local_addr_be, &local_port))
        return (uint64_t) -1;

    if (fd_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
        return NET_socket_bind_udp(owner_pid, socket_id, local_addr_be, local_port) ? 0 : (uint64_t) -1;

    return NET_tcp_bind(owner_pid, socket_id, local_addr_be, local_port) ? 0 : (uint64_t) -1;
}

static uint64_t Syscall_handle_connect(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    const void* user_addr = (const void*) frame->rsi;
    size_t addr_len = (size_t) frame->rdx;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0U)
        return (uint64_t) -1;

    uint32_t socket_id = 0U;
    uint32_t fd_type = SYSCALL_FD_TYPE_NONE;
    bool fd_non_blocking = false;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    fd_type = entry->type;
    if (fd_type != SYSCALL_FD_TYPE_NET_UDP_SOCKET &&
        fd_type != SYSCALL_FD_TYPE_NET_TCP_SOCKET &&
        fd_type != SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    socket_id = entry->net_socket_id;
    fd_non_blocking = entry->non_blocking;
    spin_unlock(&Syscall_state.fd_lock);

    if (!user_addr && addr_len == 0U)
    {
        if (fd_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
            return NET_socket_disconnect_udp(owner_pid, socket_id) ? 0 : (uint64_t) -1;
        if (fd_type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
            return NET_tcp_disconnect(owner_pid, socket_id) ? 0 : (uint64_t) -1;
        return (uint64_t) -1;
    }
    if (!user_addr || addr_len < sizeof(sys_sockaddr_t))
        return (uint64_t) -1;

    sys_sockaddr_t addr;
    if (!Syscall_copy_from_user(&addr, user_addr, sizeof(addr)))
        return (uint64_t) -1;

    if (fd_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
    {
        if (addr.sa_family != AF_UNIX)
            return (uint64_t) -1;
        size_t path_len = addr_len - 2U;
        if (path_len == 0 || path_len >= NET_UNIX_PATH_MAX)
            return (uint64_t) -1;
        char path[NET_UNIX_PATH_MAX];
        if (!Syscall_copy_from_user(path, (const uint8_t*) user_addr + 2U, path_len))
            return (uint64_t) -1;
        path[path_len] = '\0';
        size_t real_len = 0;
        while (real_len < path_len && path[real_len] != '\0')
            real_len++;
        if (real_len == 0)
            return (uint64_t) -1;
        bool would_block = false;
        if (!NET_unix_connect(owner_pid, socket_id, path, real_len, fd_non_blocking, &would_block))
            return (uint64_t) -1;
        return would_block ? (uint64_t) -2 : 0U;
    }

    if (addr.sa_family == AF_UNSPEC)
    {
        if (fd_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
            return NET_socket_disconnect_udp(owner_pid, socket_id) ? 0 : (uint64_t) -1;
        return NET_tcp_disconnect(owner_pid, socket_id) ? 0 : (uint64_t) -1;
    }
    if (addr.sa_family != AF_INET)
        return (uint64_t) -1;

    uint32_t peer_addr_be = 0U;
    uint16_t peer_port = 0U;
    if (!Syscall_copy_in_sockaddr_in(user_addr, addr_len, &peer_addr_be, &peer_port))
        return (uint64_t) -1;

    if (fd_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
        return NET_socket_connect_udp(owner_pid, socket_id, peer_addr_be, peer_port) ? 0 : (uint64_t) -1;

    bool in_progress = false;
    if (!NET_tcp_connect(owner_pid,
                         socket_id,
                         peer_addr_be,
                         peer_port,
                         fd_non_blocking,
                         &in_progress))
    {
        return (uint64_t) -1;
    }

    return in_progress ? (uint64_t) -2 : 0U;
}

static uint64_t Syscall_handle_listen(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    int64_t backlog = (int64_t) frame->rsi;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0U || backlog < 0)
        return (uint64_t) -1;

    uint32_t socket_id = 0U;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    uint32_t listen_fd_type = entry->type;
    if (listen_fd_type != SYSCALL_FD_TYPE_NET_TCP_SOCKET &&
        listen_fd_type != SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    socket_id = entry->net_socket_id;
    spin_unlock(&Syscall_state.fd_lock);

    if (listen_fd_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
        return NET_unix_listen(owner_pid, socket_id, (uint32_t) backlog) ? 0U : (uint64_t) -1;

    return NET_tcp_listen(owner_pid, socket_id, (uint32_t) backlog) ? 0U : (uint64_t) -1;
}

static uint64_t Syscall_handle_accept(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    void* user_addr = (void*) frame->rsi;
    void* user_addrlen_ptr = (void*) frame->rdx;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0U)
        return (uint64_t) -1;
    if ((user_addr == NULL) != (user_addrlen_ptr == NULL))
        return (uint64_t) -1;

    uint32_t listener_socket_id = 0U;
    bool listener_non_blocking = false;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    uint32_t accept_fd_type = entry->type;
    if (accept_fd_type != SYSCALL_FD_TYPE_NET_TCP_SOCKET &&
        accept_fd_type != SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    if (!entry->used || entry->owner_pid != owner_pid || !entry->can_read || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    entry->io_busy = true;
    listener_socket_id = entry->net_socket_id;
    listener_non_blocking = entry->non_blocking;
    spin_unlock(&Syscall_state.fd_lock);

    uint32_t child_socket_id = 0U;
    bool would_block = false;
    bool accepted = false;
    if (accept_fd_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
        accepted = NET_unix_accept(owner_pid, listener_socket_id, &child_socket_id,
                                   listener_non_blocking, &would_block);
    else
        accepted = NET_tcp_accept(owner_pid, listener_socket_id, &child_socket_id,
                                  listener_non_blocking, &would_block);

    spin_lock(&Syscall_state.fd_lock);
    entry = &Syscall_state.fds[(uint32_t) fd];
    if (entry->used && entry->owner_pid == owner_pid && entry->type == accept_fd_type)
        entry->io_busy = false;
    spin_unlock(&Syscall_state.fd_lock);

    if (!accepted)
        return (uint64_t) -1;
    if (would_block)
        return (uint64_t) -2;
    if (child_socket_id == 0U)
        return (uint64_t) -1;

    spin_lock(&Syscall_state.fd_lock);
    int32_t accepted_fd = Syscall_fd_alloc_locked();
    if (accepted_fd < 0)
    {
        spin_unlock(&Syscall_state.fd_lock);
        if (accept_fd_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
            (void) NET_unix_close(owner_pid, child_socket_id);
        else
            (void) NET_tcp_close(owner_pid, child_socket_id);
        return (uint64_t) -1;
    }

    syscall_file_desc_t* accepted_entry = &Syscall_state.fds[(uint32_t) accepted_fd];
    memset(accepted_entry, 0, sizeof(*accepted_entry));
    accepted_entry->used = true;
    accepted_entry->type = accept_fd_type;
    accepted_entry->owner_pid = owner_pid;
    accepted_entry->can_read = true;
    accepted_entry->can_write = true;
    accepted_entry->non_blocking = listener_non_blocking;
    accepted_entry->net_socket_id = child_socket_id;
    spin_unlock(&Syscall_state.fd_lock);

    if (accept_fd_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
        return (uint64_t) accepted_fd;

    if (user_addr && user_addrlen_ptr)
    {
        bool connected = false;
        uint32_t peer_addr_be = 0U;
        uint16_t peer_port = 0U;
        if (!NET_tcp_getpeername(owner_pid, child_socket_id, &connected, &peer_addr_be, &peer_port) || !connected)
        {
            spin_lock(&Syscall_state.fd_lock);
            syscall_file_desc_t* cleanup_entry = &Syscall_state.fds[(uint32_t) accepted_fd];
            if (cleanup_entry->used &&
                cleanup_entry->owner_pid == owner_pid &&
                cleanup_entry->type == SYSCALL_FD_TYPE_NET_TCP_SOCKET &&
                cleanup_entry->net_socket_id == child_socket_id)
            {
                memset(cleanup_entry, 0, sizeof(*cleanup_entry));
            }
            spin_unlock(&Syscall_state.fd_lock);
            (void) NET_tcp_close(owner_pid, child_socket_id);
            return (uint64_t) -1;
        }

        if (!Syscall_copy_out_sockaddr_in(user_addr, user_addrlen_ptr, peer_addr_be, peer_port))
        {
            spin_lock(&Syscall_state.fd_lock);
            syscall_file_desc_t* cleanup_entry = &Syscall_state.fds[(uint32_t) accepted_fd];
            if (cleanup_entry->used &&
                cleanup_entry->owner_pid == owner_pid &&
                cleanup_entry->type == SYSCALL_FD_TYPE_NET_TCP_SOCKET &&
                cleanup_entry->net_socket_id == child_socket_id)
            {
                memset(cleanup_entry, 0, sizeof(*cleanup_entry));
            }
            spin_unlock(&Syscall_state.fd_lock);
            (void) NET_tcp_close(owner_pid, child_socket_id);
            return (uint64_t) -1;
        }
    }

    return (uint64_t) accepted_fd;
}

static uint64_t Syscall_handle_sendto(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    const void* user_buf = (const void*) frame->rsi;
    size_t len = (size_t) frame->rdx;
    uint32_t flags = (uint32_t) frame->r10;
    const void* user_dest_addr = (const void*) frame->r8;
    size_t dest_addr_len = (size_t) frame->r9;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0U || (len != 0U && !user_buf))
        return (uint64_t) -1;
    if (len == 0U)
        return 0U;
    if ((flags & ~NET_SOCKET_MSG_DONTWAIT) != 0U)
        return (uint64_t) -1;
    if ((user_dest_addr == NULL) != (dest_addr_len == 0U))
        return (uint64_t) -1;

    uint32_t socket_id = 0U;
    uint32_t fd_type = SYSCALL_FD_TYPE_NONE;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || !entry->can_write || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    fd_type = entry->type;
    if (fd_type != SYSCALL_FD_TYPE_NET_UDP_SOCKET &&
        fd_type != SYSCALL_FD_TYPE_NET_TCP_SOCKET &&
        fd_type != SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    entry->io_busy = true;
    socket_id = entry->net_socket_id;
    spin_unlock(&Syscall_state.fd_lock);

    if (fd_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
    {
        size_t unix_cap = NET_UNIX_RX_MAX_PAYLOAD;
        if (len > unix_cap)
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == fd_type)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }
        uint8_t unix_payload[NET_UNIX_RX_MAX_PAYLOAD];
        if (!Syscall_copy_from_user(unix_payload, user_buf, len))
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == fd_type)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }
        char dest_path[NET_UNIX_PATH_MAX];
        size_t dest_path_real = 0;
        if (user_dest_addr && dest_addr_len > 2U)
        {
            sys_sockaddr_t sa_hdr;
            if (!Syscall_copy_from_user(&sa_hdr, user_dest_addr, sizeof(sa_hdr)) || sa_hdr.sa_family != AF_UNIX)
            {
                spin_lock(&Syscall_state.fd_lock);
                entry = &Syscall_state.fds[(uint32_t) fd];
                if (entry->used && entry->owner_pid == owner_pid && entry->type == fd_type)
                    entry->io_busy = false;
                spin_unlock(&Syscall_state.fd_lock);
                return (uint64_t) -1;
            }
            size_t plen = dest_addr_len - 2U;
            if (plen >= NET_UNIX_PATH_MAX)
                plen = NET_UNIX_PATH_MAX - 1U;
            if (!Syscall_copy_from_user(dest_path, (const uint8_t*) user_dest_addr + 2U, plen))
            {
                spin_lock(&Syscall_state.fd_lock);
                entry = &Syscall_state.fds[(uint32_t) fd];
                if (entry->used && entry->owner_pid == owner_pid && entry->type == fd_type)
                    entry->io_busy = false;
                spin_unlock(&Syscall_state.fd_lock);
                return (uint64_t) -1;
            }
            dest_path[plen] = '\0';
            while (dest_path_real < plen && dest_path[dest_path_real] != '\0')
                dest_path_real++;
        }
        size_t sent = 0;
        bool ok = NET_unix_sendto(owner_pid, socket_id, unix_payload, len,
                                  dest_path_real > 0 ? dest_path : NULL, dest_path_real, &sent);
        spin_lock(&Syscall_state.fd_lock);
        entry = &Syscall_state.fds[(uint32_t) fd];
        if (entry->used && entry->owner_pid == owner_pid && entry->type == fd_type)
            entry->io_busy = false;
        spin_unlock(&Syscall_state.fd_lock);
        return ok ? (uint64_t) sent : (uint64_t) -1;
    }

    size_t payload_cap = NET_SOCKET_UDP_MAX_PAYLOAD;
    if (fd_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET && len > payload_cap)
    {
        spin_lock(&Syscall_state.fd_lock);
        entry = &Syscall_state.fds[(uint32_t) fd];
        if (entry->used && entry->owner_pid == owner_pid && entry->type == fd_type)
            entry->io_busy = false;
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    uint8_t payload[NET_SOCKET_UDP_MAX_PAYLOAD];

    size_t sent = 0U;
    bool ok = false;
    bool would_block = false;
    if (fd_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
    {
        bool use_connected_peer = user_dest_addr == NULL;
        uint32_t dst_addr_be = 0U;
        uint16_t dst_port = 0U;
        if (!use_connected_peer &&
            !Syscall_copy_in_sockaddr_in(user_dest_addr, dest_addr_len, &dst_addr_be, &dst_port))
        {
            ok = false;
        }
        else
        {
            if (len != 0U && !Syscall_copy_from_user(payload, user_buf, len))
            {
                ok = false;
            }
            else if (use_connected_peer)
            {
                bool connected = false;
                if (!NET_socket_getpeername_udp(owner_pid, socket_id, &connected, &dst_addr_be, &dst_port) || !connected)
                    ok = false;
                else
                    ok = NET_socket_sendto_udp(owner_pid, socket_id, dst_addr_be, dst_port, payload, len, &sent);
            }
            else
            {
                ok = NET_socket_sendto_udp(owner_pid, socket_id, dst_addr_be, dst_port, payload, len, &sent);
            }
        }
    }
    else
    {
        if (user_dest_addr != NULL)
        {
            ok = false;
        }
        else
        {
            ok = true;
            const uint8_t* user_payload = (const uint8_t*) user_buf;
            bool force_non_blocking = (flags & NET_SOCKET_MSG_DONTWAIT) != 0U;
            while (sent < len)
            {
                size_t chunk_len = len - sent;
                if (chunk_len > NET_TCP_MAX_PAYLOAD)
                    chunk_len = NET_TCP_MAX_PAYLOAD;
                if (chunk_len > sizeof(payload))
                    chunk_len = sizeof(payload);

                if (!Syscall_copy_from_user(payload, user_payload + sent, chunk_len))
                {
                    ok = false;
                    break;
                }

                size_t chunk_sent = 0U;
                bool chunk_would_block = false;
                if (!NET_tcp_send(owner_pid,
                                  socket_id,
                                  payload,
                                  chunk_len,
                                  &chunk_sent,
                                  force_non_blocking,
                                  &chunk_would_block))
                {
                    ok = false;
                    break;
                }

                sent += chunk_sent;
                if (chunk_would_block)
                {
                    would_block = true;
                    break;
                }

                if (chunk_sent == 0U)
                    break;
            }
        }
    }

    spin_lock(&Syscall_state.fd_lock);
    entry = &Syscall_state.fds[(uint32_t) fd];
    if (entry->used && entry->owner_pid == owner_pid && entry->type == fd_type)
        entry->io_busy = false;
    spin_unlock(&Syscall_state.fd_lock);

    if (!ok)
    {
        if (sent != 0U)
            return (uint64_t) sent;
        return (uint64_t) -1;
    }
    if (would_block && sent == 0U)
        return (uint64_t) -2;

    return (uint64_t) sent;
}

static uint64_t Syscall_handle_recvfrom(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    void* user_buf = (void*) frame->rsi;
    size_t len = (size_t) frame->rdx;
    uint32_t flags = (uint32_t) frame->r10;
    void* user_src_addr = (void*) frame->r8;
    void* user_src_addrlen_ptr = (void*) frame->r9;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0U || (len != 0U && !user_buf))
        return (uint64_t) -1;
    if ((flags & ~NET_SOCKET_MSG_DONTWAIT) != 0U)
        return (uint64_t) -1;
    if ((user_src_addr == NULL) != (user_src_addrlen_ptr == NULL))
        return (uint64_t) -1;

    uint32_t socket_id = 0U;
    uint32_t fd_type = SYSCALL_FD_TYPE_NONE;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || !entry->can_read || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    fd_type = entry->type;
    if (fd_type != SYSCALL_FD_TYPE_NET_UDP_SOCKET &&
        fd_type != SYSCALL_FD_TYPE_NET_TCP_SOCKET &&
        fd_type != SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    if (len == 0U)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return 0U;
    }
    entry->io_busy = true;
    socket_id = entry->net_socket_id;
    spin_unlock(&Syscall_state.fd_lock);

    if (fd_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
    {
        size_t unix_cap = len < NET_UNIX_RX_MAX_PAYLOAD ? len : NET_UNIX_RX_MAX_PAYLOAD;
        uint8_t unix_buf[NET_UNIX_RX_MAX_PAYLOAD];
        size_t recv_len = 0U;
        bool would_block = false;
        bool ok = NET_unix_recvfrom(owner_pid, socket_id, unix_buf, unix_cap, &recv_len,
                                    (flags & NET_SOCKET_MSG_DONTWAIT) != 0U, &would_block);
        spin_lock(&Syscall_state.fd_lock);
        entry = &Syscall_state.fds[(uint32_t) fd];
        if (entry->used && entry->owner_pid == owner_pid && entry->type == fd_type)
            entry->io_busy = false;
        spin_unlock(&Syscall_state.fd_lock);
        if (!ok)
            return would_block ? (uint64_t) -2 : (uint64_t) -1;
        if (recv_len > 0U && !Syscall_copy_to_user(user_buf, unix_buf, recv_len))
            return (uint64_t) -1;
        return (uint64_t) recv_len;
    }

    size_t payload_cap = (fd_type == SYSCALL_FD_TYPE_NET_TCP_SOCKET) ? NET_TCP_MAX_PAYLOAD : NET_SOCKET_UDP_MAX_PAYLOAD;
    uint8_t payload[NET_SOCKET_UDP_MAX_PAYLOAD];
    size_t recv_cap = len;
    if (recv_cap > payload_cap)
        recv_cap = payload_cap;

    size_t recv_len = 0U;
    uint32_t src_addr_be = 0U;
    uint16_t src_port = 0U;
    bool would_block = false;
    bool ok = false;
    if (fd_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
    {
        ok = NET_socket_recvfrom_udp(owner_pid,
                                     socket_id,
                                     payload,
                                     recv_cap,
                                     &recv_len,
                                     &src_addr_be,
                                     &src_port,
                                     (flags & NET_SOCKET_MSG_DONTWAIT) != 0U,
                                     &would_block);
    }
    else
    {
        ok = NET_tcp_recv(owner_pid,
                          socket_id,
                          payload,
                          recv_cap,
                          &recv_len,
                          (flags & NET_SOCKET_MSG_DONTWAIT) != 0U,
                          &would_block);
        if (ok && recv_len != 0U)
        {
            bool connected = false;
            if (!NET_tcp_getpeername(owner_pid, socket_id, &connected, &src_addr_be, &src_port) || !connected)
            {
                src_addr_be = 0U;
                src_port = 0U;
            }
        }
    }

    spin_lock(&Syscall_state.fd_lock);
    entry = &Syscall_state.fds[(uint32_t) fd];
    if (entry->used && entry->owner_pid == owner_pid && entry->type == fd_type)
        entry->io_busy = false;
    spin_unlock(&Syscall_state.fd_lock);

    if (!ok)
        return (uint64_t) -1;
    if (would_block)
        return (uint64_t) -2;

    if (recv_len != 0U && !Syscall_copy_to_user(user_buf, payload, recv_len))
        return (uint64_t) -1;

    if (user_src_addr && user_src_addrlen_ptr)
    {
        if (!Syscall_copy_out_sockaddr_in(user_src_addr, user_src_addrlen_ptr, src_addr_be, src_port))
            return (uint64_t) -1;
    }

    return (uint64_t) recv_len;
}

static uint64_t Syscall_handle_getsockname(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    void* user_addr = (void*) frame->rsi;
    void* user_addrlen_ptr = (void*) frame->rdx;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0U || !user_addr || !user_addrlen_ptr)
        return (uint64_t) -1;

    uint32_t socket_id = 0U;
    uint32_t fd_type = SYSCALL_FD_TYPE_NONE;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    fd_type = entry->type;
    if (fd_type != SYSCALL_FD_TYPE_NET_UDP_SOCKET && fd_type != SYSCALL_FD_TYPE_NET_TCP_SOCKET)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    socket_id = entry->net_socket_id;
    spin_unlock(&Syscall_state.fd_lock);

    uint32_t local_addr_be = 0U;
    uint16_t local_port = 0U;
    if (fd_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
    {
        if (!NET_socket_getsockname_udp(owner_pid, socket_id, &local_addr_be, &local_port))
            return (uint64_t) -1;
    }
    else
    {
        if (!NET_tcp_getsockname(owner_pid, socket_id, &local_addr_be, &local_port))
            return (uint64_t) -1;
    }

    return Syscall_copy_out_sockaddr_in(user_addr, user_addrlen_ptr, local_addr_be, local_port) ? 0U : (uint64_t) -1;
}

static uint64_t Syscall_handle_getpeername(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    void* user_addr = (void*) frame->rsi;
    void* user_addrlen_ptr = (void*) frame->rdx;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0U || !user_addr || !user_addrlen_ptr)
        return (uint64_t) -1;

    uint32_t socket_id = 0U;
    uint32_t fd_type = SYSCALL_FD_TYPE_NONE;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    fd_type = entry->type;
    if (fd_type != SYSCALL_FD_TYPE_NET_UDP_SOCKET && fd_type != SYSCALL_FD_TYPE_NET_TCP_SOCKET)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    socket_id = entry->net_socket_id;
    spin_unlock(&Syscall_state.fd_lock);

    bool connected = false;
    uint32_t peer_addr_be = 0U;
    uint16_t peer_port = 0U;
    if (fd_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
    {
        if (!NET_socket_getpeername_udp(owner_pid, socket_id, &connected, &peer_addr_be, &peer_port) || !connected)
            return (uint64_t) -1;
    }
    else
    {
        if (!NET_tcp_getpeername(owner_pid, socket_id, &connected, &peer_addr_be, &peer_port) || !connected)
            return (uint64_t) -1;
    }

    return Syscall_copy_out_sockaddr_in(user_addr, user_addrlen_ptr, peer_addr_be, peer_port) ? 0U : (uint64_t) -1;
}

static uint64_t Syscall_handle_open(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (owner_pid == 0)
        return (uint64_t) -1;

    char path[SYSCALL_USER_CSTR_MAX];
    if (!Syscall_read_user_cstr(path, sizeof(path), (const char*) frame->rdi))
        return (uint64_t) -1;

    uint64_t flags = frame->rsi;
    const uint64_t valid_flags = SYS_OPEN_READ | SYS_OPEN_WRITE | SYS_OPEN_CREATE | SYS_OPEN_TRUNC | SYS_OPEN_LOCK;
    if ((flags & ~valid_flags) != 0)
        return (uint64_t) -1;

    bool can_read = (flags & SYS_OPEN_READ) != 0;
    bool can_write = (flags & SYS_OPEN_WRITE) != 0;
    bool want_create = (flags & SYS_OPEN_CREATE) != 0;
    bool want_trunc = (flags & SYS_OPEN_TRUNC) != 0;
    bool want_exclusive = (flags & SYS_OPEN_LOCK) != 0;

    if (!can_read && !can_write)
        can_read = true;
    if ((want_create || want_trunc) && !can_write)
        return (uint64_t) -1;

    if (Syscall_is_audio_dsp_path(path))
    {
        if (want_create || want_trunc || want_exclusive)
            return (uint64_t) -1;
        if (!can_write)
            return (uint64_t) -1;
        if (!HDA_is_available())
            return (uint64_t) -1;
        if (!HDA_dsp_open(owner_pid))
            return (uint64_t) -1;

        spin_lock(&Syscall_state.fd_lock);
        int32_t fd = Syscall_fd_alloc_locked();
        if (fd < 0)
        {
            spin_unlock(&Syscall_state.fd_lock);
            HDA_dsp_close(owner_pid);
            return (uint64_t) -1;
        }

        syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        entry->type = SYSCALL_FD_TYPE_AUDIO_DSP;
        entry->owner_pid = owner_pid;
        entry->can_read = false;
        entry->can_write = true;
        memcpy(entry->path, HDA_DSP_NODE_PATH, sizeof(HDA_DSP_NODE_PATH));
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) fd;
    }

    if (Syscall_is_net_raw_path(path))
    {
        if (want_create || want_trunc || want_exclusive)
            return (uint64_t) -1;
        if (!E1000_is_available())
            return (uint64_t) -1;

        spin_lock(&Syscall_state.fd_lock);
        int32_t fd = Syscall_fd_alloc_locked();
        if (fd < 0)
        {
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        entry->type = SYSCALL_FD_TYPE_NET_RAW;
        entry->owner_pid = owner_pid;
        entry->can_read = can_read;
        entry->can_write = can_write;
        entry->non_blocking = false;
        memcpy(entry->path, E1000_NET_NODE_PATH, sizeof(E1000_NET_NODE_PATH));
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) fd;
    }

    if (Syscall_is_drm_card_path(path))
    {
        if (want_create || want_trunc || want_exclusive)
            return (uint64_t) -1;

        uint32_t drm_file_id = 0;
        if (!DRM_open_file(owner_pid, &drm_file_id))
            return (uint64_t) -1;

        spin_lock(&Syscall_state.fd_lock);
        int32_t fd = Syscall_fd_alloc_locked();
        if (fd < 0)
        {
            spin_unlock(&Syscall_state.fd_lock);
            DRM_close_file(drm_file_id);
            return (uint64_t) -1;
        }

        syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        entry->type = SYSCALL_FD_TYPE_DRM_CARD;
        entry->owner_pid = owner_pid;
        entry->drm_file_id = drm_file_id;
        entry->can_read = true;
        entry->can_write = true;
        memcpy(entry->path, DRM_NODE_PATH, sizeof(DRM_NODE_PATH));
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) fd;
    }

    char lock_path[SYSCALL_USER_CSTR_MAX] = { 0 };
    bool lock_path_is_normalized = Syscall_normalize_write_path(path, lock_path, sizeof(lock_path));
    if (can_write || want_exclusive)
    {
        if (!lock_path_is_normalized)
            return (uint64_t) -1;
    }
    else
    {
        size_t raw_len = strlen(path);
        if (raw_len + 1U > sizeof(lock_path))
            return (uint64_t) -1;
        memcpy(lock_path, path, raw_len + 1U);
    }

    int32_t fd = -1;
    spin_lock(&Syscall_state.fd_lock);
    if (Syscall_fd_path_conflicts_locked(lock_path, want_exclusive))
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    fd = Syscall_fd_alloc_locked();
    if (fd < 0)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->type = SYSCALL_FD_TYPE_REGULAR;
    entry->owner_pid = owner_pid;
    entry->exclusive = want_exclusive;
    entry->io_busy = true;
    memcpy(entry->path, lock_path, sizeof(entry->path));
    spin_unlock(&Syscall_state.fd_lock);

    uint8_t* file_data = NULL;
    size_t file_size = 0;
    bool fs_ready = false;
    size_t block_size = 0;
    bool exists = false;

    fs_ready = VFS_is_ready();
    block_size = VFS_block_size();
    if (fs_ready && block_size != 0U)
        exists = VFS_read_file(path, &file_data, &file_size);

    if (!fs_ready || block_size == 0U)
        goto open_fail_slot;

    if (!exists)
    {
        if (!want_create)
            goto open_fail_slot;
        file_data = NULL;
        file_size = 0;
    }

    if (can_write && file_size > block_size)
        goto open_fail_file_data;

    if (want_trunc)
    {
        if (file_data)
        {
            kfree(file_data);
            file_data = NULL;
        }
        file_size = 0;
    }

    spin_lock(&Syscall_state.fd_lock);
    entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || !entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        goto open_fail_file_data;
    }

    entry->can_read = can_read;
    entry->can_write = can_write;
    entry->dirty = (want_trunc || (!exists && want_create));
    entry->io_busy = false;
    entry->data = file_data;
    entry->size = file_size;
    entry->capacity = file_size;
    entry->offset = 0;
    entry->max_size = can_write ? block_size : file_size;

    spin_unlock(&Syscall_state.fd_lock);
    return (uint64_t) fd;

open_fail_file_data:
    if (file_data)
        kfree(file_data);

open_fail_slot:
    spin_lock(&Syscall_state.fd_lock);
    entry = &Syscall_state.fds[(uint32_t) fd];
    if (entry->used && entry->owner_pid == owner_pid && entry->io_busy)
        memset(entry, 0, sizeof(*entry));
    spin_unlock(&Syscall_state.fd_lock);
    return (uint64_t) -1;
}

static uint64_t Syscall_handle_close(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0)
        return (uint64_t) -1;

    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    uint32_t entry_type = entry->type;
    if (entry_type == SYSCALL_FD_TYPE_DRM_CARD)
    {
        uint32_t drm_file_id = entry->drm_file_id;
        memset(entry, 0, sizeof(*entry));
        spin_unlock(&Syscall_state.fd_lock);
        DRM_close_file(drm_file_id);
        return 0;
    }

    if (entry_type == SYSCALL_FD_TYPE_DMABUF)
    {
        uint32_t dmabuf_id = entry->drm_dmabuf_id;
        memset(entry, 0, sizeof(*entry));
        spin_unlock(&Syscall_state.fd_lock);
        DRM_dmabuf_unref_fd(dmabuf_id);
        return 0;
    }

    if (entry_type == SYSCALL_FD_TYPE_AUDIO_DSP)
    {
        memset(entry, 0, sizeof(*entry));
        spin_unlock(&Syscall_state.fd_lock);
        HDA_dsp_close(owner_pid);
        return 0;
    }

    if (entry_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
    {
        uint32_t socket_id = entry->net_socket_id;
        memset(entry, 0, sizeof(*entry));
        spin_unlock(&Syscall_state.fd_lock);
        (void) NET_socket_close_udp(owner_pid, socket_id);
        return 0;
    }

    if (entry_type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
    {
        uint32_t socket_id = entry->net_socket_id;
        memset(entry, 0, sizeof(*entry));
        spin_unlock(&Syscall_state.fd_lock);
        (void) NET_tcp_close(owner_pid, socket_id);
        return 0;
    }

    if (entry_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
    {
        uint32_t socket_id = entry->net_socket_id;
        memset(entry, 0, sizeof(*entry));
        spin_unlock(&Syscall_state.fd_lock);
        (void) NET_unix_close(owner_pid, socket_id);
        return 0;
    }

    if (entry_type == SYSCALL_FD_TYPE_PIPE)
    {
        uint32_t pipe_id = entry->net_socket_id;
        bool was_reader = entry->can_read;
        bool was_writer = entry->can_write;
        memset(entry, 0, sizeof(*entry));
        spin_unlock(&Syscall_state.fd_lock);

        if (pipe_id < SYSCALL_PIPE_MAX)
        {
            syscall_pipe_t* p = &Syscall_state.pipes[pipe_id];
            if (p->used && p->lock_ready)
            {
                spin_lock(&p->lock);
                if (was_reader && p->readers > 0)
                    p->readers--;
                if (was_writer && p->writers > 0)
                    p->writers--;
                task_wait_queue_wake_all(&p->read_waitq);
                task_wait_queue_wake_all(&p->write_waitq);
                if (p->readers == 0 && p->writers == 0)
                {
                    spin_unlock(&p->lock);
                    spin_lock(&Syscall_state.pipe_lock);
                    p->used = false;
                    spin_unlock(&Syscall_state.pipe_lock);
                }
                else
                    spin_unlock(&p->lock);
            }
        }
        return 0;
    }

    if (entry_type == SYSCALL_FD_TYPE_NET_RAW)
    {
        memset(entry, 0, sizeof(*entry));
        spin_unlock(&Syscall_state.fd_lock);
        return 0;
    }

    if (entry_type != SYSCALL_FD_TYPE_REGULAR)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    bool flush_needed = entry->can_write && entry->dirty;
    bool flush_ok = true;
    char flush_path[SYSCALL_USER_CSTR_MAX];
    size_t flush_size = 0;
    const uint8_t* flush_data = NULL;
    if (flush_needed)
    {
        memcpy(flush_path, entry->path, sizeof(flush_path));
        flush_size = entry->size;
        flush_data = entry->data;
        entry->io_busy = true;
    }
    else
    {
        if (entry->data)
            kfree(entry->data);
        memset(entry, 0, sizeof(*entry));
        spin_unlock(&Syscall_state.fd_lock);
        return 0;
    }
    spin_unlock(&Syscall_state.fd_lock);

    size_t block_size = VFS_block_size();
    flush_ok = (VFS_is_ready() &&
                block_size != 0U &&
                flush_size <= block_size &&
                VFS_write_file(flush_path, flush_data, flush_size));

    spin_lock(&Syscall_state.fd_lock);
    entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    if (!flush_ok)
    {
        entry->io_busy = false;
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    if (entry->data)
        kfree(entry->data);
    memset(entry, 0, sizeof(*entry));
    spin_unlock(&Syscall_state.fd_lock);
    return 0;
}

static bool Syscall_pipe_rx_empty(void* ctx);
static bool Syscall_pipe_tx_full(void* ctx);

static uint64_t Syscall_handle_read(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    void* user_buf = (void*) frame->rsi;
    size_t len = (size_t) frame->rdx;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (len == 0)
        return 0;
    if (!user_buf || fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;
    if (owner_pid == 0)
        return (uint64_t) -1;

    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || !entry->can_read || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    uint32_t entry_type = entry->type;
    if (entry_type == SYSCALL_FD_TYPE_NET_RAW)
    {
        entry->io_busy = true;
        bool block = !entry->non_blocking;
        spin_unlock(&Syscall_state.fd_lock);

        uint8_t frame_buf[E1000_RX_BUFFER_BYTES];
        size_t frame_len = 0;
        if (!E1000_raw_read(frame_buf, sizeof(frame_buf), &frame_len, block))
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_RAW)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        size_t to_copy = frame_len;
        if (to_copy > len)
            to_copy = len;

        if (to_copy != 0 && !Syscall_copy_to_user(user_buf, frame_buf, to_copy))
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_RAW)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        spin_lock(&Syscall_state.fd_lock);
        entry = &Syscall_state.fds[(uint32_t) fd];
        if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_RAW)
            entry->io_busy = false;
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) to_copy;
    }

    if (entry_type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
    {
        entry->io_busy = true;
        uint32_t socket_id = entry->net_socket_id;
        spin_unlock(&Syscall_state.fd_lock);

        uint8_t payload[NET_TCP_MAX_PAYLOAD];
        size_t recv_cap = len;
        if (recv_cap > sizeof(payload))
            recv_cap = sizeof(payload);

        size_t recv_len = 0U;
        bool would_block = false;
        if (!NET_tcp_recv(owner_pid, socket_id, payload, recv_cap, &recv_len, false, &would_block))
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        if (would_block)
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -2;
        }

        if (recv_len != 0U && !Syscall_copy_to_user(user_buf, payload, recv_len))
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        spin_lock(&Syscall_state.fd_lock);
        entry = &Syscall_state.fds[(uint32_t) fd];
        if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
            entry->io_busy = false;
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) recv_len;
    }

    if (entry_type == SYSCALL_FD_TYPE_PIPE)
    {
        uint32_t pipe_id = entry->net_socket_id;
        spin_unlock(&Syscall_state.fd_lock);

        if (pipe_id >= SYSCALL_PIPE_MAX)
            return (uint64_t) -1;
        syscall_pipe_t* p = &Syscall_state.pipes[pipe_id];
        if (!p->used || !p->lock_ready)
            return (uint64_t) -1;

        spin_lock(&p->lock);
        if (p->count == 0 && p->writers > 0)
        {
            spin_unlock(&p->lock);
            task_waiter_t waiter;
            task_waiter_init(&waiter);
            task_wait_queue_wait_event(&p->read_waitq, &waiter,
                                       Syscall_pipe_rx_empty, p,
                                       TASK_WAIT_TIMEOUT_INFINITE);
            spin_lock(&p->lock);
        }

        if (p->count == 0)
        {
            spin_unlock(&p->lock);
            return 0;
        }

        size_t to_read = p->count < len ? p->count : len;
        uint8_t pipe_buf[SYSCALL_PIPE_BUF_SIZE];
        for (size_t i = 0; i < to_read; i++)
        {
            pipe_buf[i] = p->ring[p->head];
            p->head = (uint16_t) ((p->head + 1U) % SYSCALL_PIPE_BUF_SIZE);
        }
        __atomic_store_n(&p->count, p->count - (uint32_t) to_read, __ATOMIC_RELEASE);
        task_wait_queue_wake_all(&p->write_waitq);
        spin_unlock(&p->lock);

        if (!Syscall_copy_to_user(user_buf, pipe_buf, to_read))
            return (uint64_t) -1;
        return (uint64_t) to_read;
    }

    if (entry_type != SYSCALL_FD_TYPE_REGULAR)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    if (entry->offset >= entry->size)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return 0;
    }

    size_t remaining = entry->size - entry->offset;
    size_t to_copy = (len < remaining) ? len : remaining;
    if (!entry->data || !Syscall_copy_to_user(user_buf, entry->data + entry->offset, to_copy))
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    entry->offset += to_copy;
    spin_unlock(&Syscall_state.fd_lock);
    return (uint64_t) to_copy;
}

static uint64_t Syscall_handle_write(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    const void* user_buf = (const void*) frame->rsi;
    size_t len = (size_t) frame->rdx;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (len == 0)
        return 0;
    if (!user_buf || fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;
    if (owner_pid == 0)
        return (uint64_t) -1;

    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || !entry->can_write || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    uint32_t entry_type = entry->type;
    if (entry_type == SYSCALL_FD_TYPE_AUDIO_DSP)
    {
        entry->io_busy = true;
        spin_unlock(&Syscall_state.fd_lock);

        size_t copied_total = 0;
        uint8_t chunk_buf[1024];
        while (copied_total < len)
        {
            size_t chunk = len - copied_total;
            if (chunk > sizeof(chunk_buf))
                chunk = sizeof(chunk_buf);

            if (!Syscall_copy_from_user(chunk_buf, (const uint8_t*) user_buf + copied_total, chunk))
            {
                spin_lock(&Syscall_state.fd_lock);
                entry = &Syscall_state.fds[(uint32_t) fd];
                if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_AUDIO_DSP)
                    entry->io_busy = false;
                spin_unlock(&Syscall_state.fd_lock);
                return (copied_total > 0) ? (uint64_t) copied_total : (uint64_t) -1;
            }

            size_t pushed = HDA_dsp_write(chunk_buf, chunk);
            copied_total += pushed;
            if (pushed < chunk)
            {
                spin_lock(&Syscall_state.fd_lock);
                entry = &Syscall_state.fds[(uint32_t) fd];
                if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_AUDIO_DSP)
                    entry->io_busy = false;
                spin_unlock(&Syscall_state.fd_lock);
                return (copied_total > 0) ? (uint64_t) copied_total : (uint64_t) -1;
            }
        }

        spin_lock(&Syscall_state.fd_lock);
        entry = &Syscall_state.fds[(uint32_t) fd];
        if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_AUDIO_DSP)
            entry->io_busy = false;
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) copied_total;
    }

    if (entry_type == SYSCALL_FD_TYPE_NET_RAW)
    {
        if (len > E1000_ETH_FRAME_MAX_BYTES)
        {
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        entry->io_busy = true;
        spin_unlock(&Syscall_state.fd_lock);

        uint8_t frame_buf[E1000_TX_BUFFER_BYTES];
        if (!Syscall_copy_from_user(frame_buf, user_buf, len))
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_RAW)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        size_t written = 0;
        if (!E1000_raw_write(frame_buf, len, &written))
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_RAW)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        spin_lock(&Syscall_state.fd_lock);
        entry = &Syscall_state.fds[(uint32_t) fd];
        if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_RAW)
            entry->io_busy = false;
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) written;
    }

    if (entry_type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
    {
        if (len > NET_TCP_MAX_PAYLOAD)
        {
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        entry->io_busy = true;
        uint32_t socket_id = entry->net_socket_id;
        spin_unlock(&Syscall_state.fd_lock);

        uint8_t payload[NET_TCP_MAX_PAYLOAD];
        if (!Syscall_copy_from_user(payload, user_buf, len))
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        size_t sent = 0U;
        bool would_block = false;
        if (!NET_tcp_send(owner_pid, socket_id, payload, len, &sent, false, &would_block))
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        if (would_block)
        {
            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
                entry->io_busy = false;
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -2;
        }

        spin_lock(&Syscall_state.fd_lock);
        entry = &Syscall_state.fds[(uint32_t) fd];
        if (entry->used && entry->owner_pid == owner_pid && entry->type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
            entry->io_busy = false;
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) sent;
    }

    if (entry_type == SYSCALL_FD_TYPE_PIPE)
    {
        uint32_t pipe_id = entry->net_socket_id;
        spin_unlock(&Syscall_state.fd_lock);

        if (pipe_id >= SYSCALL_PIPE_MAX)
            return (uint64_t) -1;
        syscall_pipe_t* p = &Syscall_state.pipes[pipe_id];
        if (!p->used || !p->lock_ready)
            return (uint64_t) -1;

        uint8_t pipe_buf[SYSCALL_PIPE_BUF_SIZE];
        size_t to_write = len < SYSCALL_PIPE_BUF_SIZE ? len : SYSCALL_PIPE_BUF_SIZE;
        if (!Syscall_copy_from_user(pipe_buf, user_buf, to_write))
            return (uint64_t) -1;

        spin_lock(&p->lock);
        if (p->readers == 0)
        {
            spin_unlock(&p->lock);
            return (uint64_t) -1;
        }

        if (p->count >= SYSCALL_PIPE_BUF_SIZE)
        {
            spin_unlock(&p->lock);
            task_waiter_t waiter;
            task_waiter_init(&waiter);
            task_wait_queue_wait_event(&p->write_waitq, &waiter,
                                       Syscall_pipe_tx_full, p,
                                       TASK_WAIT_TIMEOUT_INFINITE);
            spin_lock(&p->lock);
            if (p->readers == 0)
            {
                spin_unlock(&p->lock);
                return (uint64_t) -1;
            }
        }

        uint32_t avail = SYSCALL_PIPE_BUF_SIZE - p->count;
        size_t actual = to_write < avail ? to_write : avail;
        for (size_t i = 0; i < actual; i++)
        {
            p->ring[p->tail] = pipe_buf[i];
            p->tail = (uint16_t) ((p->tail + 1U) % SYSCALL_PIPE_BUF_SIZE);
        }
        __atomic_store_n(&p->count, p->count + (uint32_t) actual, __ATOMIC_RELEASE);
        task_wait_queue_wake_all(&p->read_waitq);
        spin_unlock(&p->lock);
        return (uint64_t) actual;
    }

    if (entry_type != SYSCALL_FD_TYPE_REGULAR)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    if (entry->offset > entry->max_size || len > (entry->max_size - entry->offset))
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    size_t needed = entry->offset + len;
    if (needed > entry->capacity)
    {
        uint8_t* new_data = (uint8_t*) kmalloc(needed);
        if (!new_data)
        {
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }

        if (entry->size != 0 && entry->data)
            memcpy(new_data, entry->data, entry->size);
        if (needed > entry->size)
            memset(new_data + entry->size, 0, needed - entry->size);

        if (entry->data)
            kfree(entry->data);
        entry->data = new_data;
        entry->capacity = needed;
    }

    if (!entry->data || !Syscall_copy_from_user(entry->data + entry->offset, user_buf, len))
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    entry->offset += len;
    if (entry->size < entry->offset)
        entry->size = entry->offset;
    entry->dirty = true;
    spin_unlock(&Syscall_state.fd_lock);
    return (uint64_t) len;
}

static uint64_t Syscall_handle_lseek(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    int64_t offset = (int64_t) frame->rsi;
    int64_t whence = (int64_t) frame->rdx;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0)
        return (uint64_t) -1;

    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    if (entry->type != SYSCALL_FD_TYPE_REGULAR)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    size_t base = 0;
    switch (whence)
    {
        case SYS_SEEK_SET:
            base = 0;
            break;
        case SYS_SEEK_CUR:
            base = entry->offset;
            break;
        case SYS_SEEK_END:
            base = entry->size;
            break;
        default:
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
    }

    size_t limit = entry->can_write ? entry->max_size : entry->size;
    size_t new_pos = 0;
    if (offset >= 0)
    {
        uint64_t add = (uint64_t) offset;
        if (add > (uint64_t) (limit - base))
        {
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }
        new_pos = base + (size_t) add;
    }
    else
    {
        uint64_t sub = (uint64_t) (-offset);
        if (sub > (uint64_t) base)
        {
            spin_unlock(&Syscall_state.fd_lock);
            return (uint64_t) -1;
        }
        new_pos = base - (size_t) sub;
    }

    entry->offset = new_pos;
    spin_unlock(&Syscall_state.fd_lock);
    return (uint64_t) new_pos;
}

static uint64_t Syscall_handle_audio_ioctl(unsigned long request, void* user_arg)
{
    switch (request)
    {
        case SNDCTL_DSP_RESET:
        case SNDCTL_DSP_SYNC:
            return HDA_dsp_ioctl(request, NULL) ? 0 : (uint64_t) -1;

        case SNDCTL_DSP_SPEED:
        case SNDCTL_DSP_STEREO:
        case SNDCTL_DSP_SETFMT:
        case SNDCTL_DSP_SETFRAGMENT:
        case SNDCTL_DSP_GETFMTS:
        {
            if (!user_arg)
                return (uint64_t) -1;

            int32_t value = 0;
            if (!Syscall_copy_from_user(&value, user_arg, sizeof(value)))
                return (uint64_t) -1;
            if (!HDA_dsp_ioctl(request, &value))
                return (uint64_t) -1;
            if (!Syscall_copy_to_user(user_arg, &value, sizeof(value)))
                return (uint64_t) -1;
            return 0;
        }

        default:
            return (uint64_t) -1;
    }
}

static uint64_t Syscall_handle_net_raw_ioctl(uint32_t owner_pid, int64_t fd, unsigned long request, void* user_arg)
{
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0)
        return (uint64_t) -1;

    switch (request)
    {
        case FIONBIO:
        {
            if (!user_arg)
                return (uint64_t) -1;

            int32_t non_blocking = 0;
            if (!Syscall_copy_from_user(&non_blocking, user_arg, sizeof(non_blocking)))
                return (uint64_t) -1;

            bool set_non_blocking = non_blocking != 0;
            spin_lock(&Syscall_state.fd_lock);
            syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
            if (!entry->used || entry->owner_pid != owner_pid || entry->type != SYSCALL_FD_TYPE_NET_RAW || entry->io_busy)
            {
                spin_unlock(&Syscall_state.fd_lock);
                return (uint64_t) -1;
            }
            entry->non_blocking = set_non_blocking;
            spin_unlock(&Syscall_state.fd_lock);

            int32_t out_value = set_non_blocking ? 1 : 0;
            return Syscall_copy_to_user(user_arg, &out_value, sizeof(out_value)) ? 0 : (uint64_t) -1;
        }

        case FIONREAD:
        {
            if (!user_arg)
                return (uint64_t) -1;

            size_t pending_bytes = 0;
            if (!E1000_get_pending_rx_bytes(&pending_bytes))
                return (uint64_t) -1;

            int32_t out_value = (pending_bytes > 0x7FFFFFFFULL) ? 0x7FFFFFFF : (int32_t) pending_bytes;
            return Syscall_copy_to_user(user_arg, &out_value, sizeof(out_value)) ? 0 : (uint64_t) -1;
        }

        case NET_RAW_IOCTL_GET_STATS:
        {
            if (!user_arg)
                return (uint64_t) -1;

            sys_net_raw_stats_t stats;
            if (!E1000_get_stats(&stats))
                return (uint64_t) -1;
            return Syscall_copy_to_user(user_arg, &stats, sizeof(stats)) ? 0 : (uint64_t) -1;
        }

        case SIOCGARP:
        {
            if (!user_arg)
                return (uint64_t) -1;

            sys_arpreq_t request_data;
            if (!Syscall_copy_from_user(&request_data, user_arg, sizeof(request_data)))
                return (uint64_t) -1;

            sys_arpreq_t reply_data;
            if (!ARP_ioctl_get(&request_data, &reply_data))
                return (uint64_t) -1;

            return Syscall_copy_to_user(user_arg, &reply_data, sizeof(reply_data)) ? 0 : (uint64_t) -1;
        }

        case SIOCSARP:
        {
            if (!user_arg)
                return (uint64_t) -1;
            if (!Syscall_proc_owner_is_driverland(owner_pid))
                return (uint64_t) -1;

            sys_arpreq_t request_data;
            if (!Syscall_copy_from_user(&request_data, user_arg, sizeof(request_data)))
                return (uint64_t) -1;
            return ARP_ioctl_set(&request_data) ? 0 : (uint64_t) -1;
        }

        case SIOCDARP:
        {
            if (!user_arg)
                return (uint64_t) -1;
            if (!Syscall_proc_owner_is_driverland(owner_pid))
                return (uint64_t) -1;

            sys_arpreq_t request_data;
            if (!Syscall_copy_from_user(&request_data, user_arg, sizeof(request_data)))
                return (uint64_t) -1;
            return ARP_ioctl_delete(&request_data) ? 0 : (uint64_t) -1;
        }

        default:
            return (uint64_t) -1;
    }
}

static uint64_t Syscall_handle_udp_socket_ioctl(uint32_t owner_pid, int64_t fd, unsigned long request, void* user_arg)
{
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0U || !user_arg)
        return (uint64_t) -1;

    uint32_t socket_id = 0U;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->type != SYSCALL_FD_TYPE_NET_UDP_SOCKET || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    socket_id = entry->net_socket_id;
    spin_unlock(&Syscall_state.fd_lock);

    switch (request)
    {
        case FIONBIO:
        {
            int32_t non_blocking = 0;
            if (!Syscall_copy_from_user(&non_blocking, user_arg, sizeof(non_blocking)))
                return (uint64_t) -1;

            bool out_value = false;
            if (!NET_socket_set_non_blocking(owner_pid, socket_id, non_blocking != 0, &out_value))
                return (uint64_t) -1;

            int32_t out_int = out_value ? 1 : 0;
            return Syscall_copy_to_user(user_arg, &out_int, sizeof(out_int)) ? 0 : (uint64_t) -1;
        }

        case FIONREAD:
        {
            size_t pending = 0U;
            if (!NET_socket_pending_udp_bytes(owner_pid, socket_id, &pending))
                return (uint64_t) -1;

            int32_t out_int = (pending > 0x7FFFFFFFULL) ? 0x7FFFFFFF : (int32_t) pending;
            return Syscall_copy_to_user(user_arg, &out_int, sizeof(out_int)) ? 0 : (uint64_t) -1;
        }

        default:
            return (uint64_t) -1;
    }
}

static uint64_t Syscall_handle_tcp_socket_ioctl(uint32_t owner_pid, int64_t fd, unsigned long request, void* user_arg)
{
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0U || !user_arg)
        return (uint64_t) -1;

    uint32_t socket_id = 0U;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->type != SYSCALL_FD_TYPE_NET_TCP_SOCKET || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    socket_id = entry->net_socket_id;
    spin_unlock(&Syscall_state.fd_lock);

    switch (request)
    {
        case FIONBIO:
        {
            int32_t non_blocking = 0;
            if (!Syscall_copy_from_user(&non_blocking, user_arg, sizeof(non_blocking)))
                return (uint64_t) -1;

            bool out_value = false;
            if (!NET_tcp_set_non_blocking(owner_pid, socket_id, non_blocking != 0, &out_value))
                return (uint64_t) -1;

            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (!entry->used || entry->owner_pid != owner_pid || entry->type != SYSCALL_FD_TYPE_NET_TCP_SOCKET || entry->io_busy)
            {
                spin_unlock(&Syscall_state.fd_lock);
                return (uint64_t) -1;
            }
            entry->non_blocking = out_value;
            spin_unlock(&Syscall_state.fd_lock);

            int32_t out_int = out_value ? 1 : 0;
            return Syscall_copy_to_user(user_arg, &out_int, sizeof(out_int)) ? 0 : (uint64_t) -1;
        }

        case FIONREAD:
        {
            size_t pending = 0U;
            if (!NET_tcp_pending_bytes(owner_pid, socket_id, &pending))
                return (uint64_t) -1;

            int32_t out_int = (pending > 0x7FFFFFFFULL) ? 0x7FFFFFFF : (int32_t) pending;
            return Syscall_copy_to_user(user_arg, &out_int, sizeof(out_int)) ? 0 : (uint64_t) -1;
        }

        default:
            return (uint64_t) -1;
    }
}

static uint64_t Syscall_handle_unix_socket_ioctl(uint32_t owner_pid, int64_t fd, unsigned long request, void* user_arg)
{
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0U || !user_arg)
        return (uint64_t) -1;

    uint32_t socket_id = 0U;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->type != SYSCALL_FD_TYPE_NET_UNIX_SOCKET || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }
    socket_id = entry->net_socket_id;
    spin_unlock(&Syscall_state.fd_lock);

    switch (request)
    {
        case FIONBIO:
        {
            int32_t non_blocking = 0;
            if (!Syscall_copy_from_user(&non_blocking, user_arg, sizeof(non_blocking)))
                return (uint64_t) -1;

            bool out_value = false;
            if (!NET_unix_set_non_blocking(owner_pid, socket_id, non_blocking != 0, &out_value))
                return (uint64_t) -1;

            spin_lock(&Syscall_state.fd_lock);
            entry = &Syscall_state.fds[(uint32_t) fd];
            if (!entry->used || entry->owner_pid != owner_pid || entry->type != SYSCALL_FD_TYPE_NET_UNIX_SOCKET || entry->io_busy)
            {
                spin_unlock(&Syscall_state.fd_lock);
                return (uint64_t) -1;
            }
            entry->non_blocking = out_value;
            spin_unlock(&Syscall_state.fd_lock);

            int32_t out_int = out_value ? 1 : 0;
            return Syscall_copy_to_user(user_arg, &out_int, sizeof(out_int)) ? 0 : (uint64_t) -1;
        }

        default:
            return (uint64_t) -1;
    }
}

static uint64_t Syscall_handle_ioctl(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    unsigned long request = (unsigned long) ((uint32_t) frame->rsi);
    void* user_arg = (void*) frame->rdx;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0)
        return (uint64_t) -1;

    uint32_t fd_type = SYSCALL_FD_TYPE_NONE;
    uint32_t drm_file_id = 0;
    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    fd_type = entry->type;
    drm_file_id = entry->drm_file_id;
    spin_unlock(&Syscall_state.fd_lock);

    if (fd_type == SYSCALL_FD_TYPE_AUDIO_DSP)
        return Syscall_handle_audio_ioctl(request, user_arg);
    if (fd_type == SYSCALL_FD_TYPE_NET_RAW)
        return Syscall_handle_net_raw_ioctl(owner_pid, fd, request, user_arg);
    if (fd_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
        return Syscall_handle_udp_socket_ioctl(owner_pid, fd, request, user_arg);
    if (fd_type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
        return Syscall_handle_tcp_socket_ioctl(owner_pid, fd, request, user_arg);
    if (fd_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
        return Syscall_handle_unix_socket_ioctl(owner_pid, fd, request, user_arg);

    if (fd_type != SYSCALL_FD_TYPE_DRM_CARD || drm_file_id == 0)
        return (uint64_t) -1;

    switch (request)
    {
        case DRM_IOCTL_MODE_GET_RESOURCES:
        {
            if (!user_arg)
                return (uint64_t) -1;

            drm_mode_get_resources_t io;
            if (!Syscall_copy_from_user(&io, user_arg, sizeof(io)))
                return (uint64_t) -1;
            if (!DRM_get_resources(drm_file_id, &io))
                return (uint64_t) -1;

            if (io.crtc_id_ptr != 0)
            {
                uint32_t crtc_id = DRM_CRTC_ID;
                if (!Syscall_copy_to_user((void*) (uintptr_t) io.crtc_id_ptr, &crtc_id, sizeof(crtc_id)))
                    return (uint64_t) -1;
            }
            if (io.connector_id_ptr != 0)
            {
                uint32_t connector_id = DRM_CONNECTOR_ID;
                if (!Syscall_copy_to_user((void*) (uintptr_t) io.connector_id_ptr, &connector_id, sizeof(connector_id)))
                    return (uint64_t) -1;
            }
            if (io.plane_id_ptr != 0)
            {
                uint32_t plane_id = DRM_PLANE_ID;
                if (!Syscall_copy_to_user((void*) (uintptr_t) io.plane_id_ptr, &plane_id, sizeof(plane_id)))
                    return (uint64_t) -1;
            }

            return Syscall_copy_to_user(user_arg, &io, sizeof(io)) ? 0 : (uint64_t) -1;
        }

        case DRM_IOCTL_MODE_GET_CONNECTOR:
        {
            if (!user_arg)
                return (uint64_t) -1;

            drm_mode_get_connector_t io;
            if (!Syscall_copy_from_user(&io, user_arg, sizeof(io)))
                return (uint64_t) -1;

            uint32_t user_modes_capacity = io.count_modes;
            uint32_t kernel_modes_capacity = user_modes_capacity;
            if (kernel_modes_capacity > DRM_MAX_CONNECTOR_MODES)
                kernel_modes_capacity = DRM_MAX_CONNECTOR_MODES;

            drm_mode_modeinfo_t modes[DRM_MAX_CONNECTOR_MODES];
            memset(modes, 0, sizeof(modes));
            uint32_t modes_written = 0;
            if (!DRM_get_connector(drm_file_id,
                                   &io,
                                   (kernel_modes_capacity > 0) ? modes : NULL,
                                   kernel_modes_capacity,
                                   &modes_written))
            {
                return (uint64_t) -1;
            }

            if (io.modes_ptr != 0 && user_modes_capacity > 0 && modes_written > 0)
            {
                uint32_t to_copy = modes_written;
                if (to_copy > kernel_modes_capacity)
                    to_copy = kernel_modes_capacity;
                if (to_copy > user_modes_capacity)
                    to_copy = user_modes_capacity;

                if (!Syscall_copy_to_user((void*) (uintptr_t) io.modes_ptr,
                                          modes,
                                          (size_t) to_copy * sizeof(modes[0])))
                    return (uint64_t) -1;
            }

            return Syscall_copy_to_user(user_arg, &io, sizeof(io)) ? 0 : (uint64_t) -1;
        }

        case DRM_IOCTL_MODE_GET_CRTC:
        {
            if (!user_arg)
                return (uint64_t) -1;

            drm_mode_get_crtc_t io;
            if (!Syscall_copy_from_user(&io, user_arg, sizeof(io)))
                return (uint64_t) -1;
            if (!DRM_get_crtc(drm_file_id, &io))
                return (uint64_t) -1;
            return Syscall_copy_to_user(user_arg, &io, sizeof(io)) ? 0 : (uint64_t) -1;
        }

        case DRM_IOCTL_MODE_GET_PLANE:
        {
            if (!user_arg)
                return (uint64_t) -1;

            drm_mode_get_plane_t io;
            if (!Syscall_copy_from_user(&io, user_arg, sizeof(io)))
                return (uint64_t) -1;

            uint32_t formats_capacity = io.count_format_types;
            uint32_t format = DRM_FORMAT_XRGB8888;
            uint32_t formats_written = 0;
            if (!DRM_get_plane(drm_file_id,
                               &io,
                               (formats_capacity > 0) ? &format : NULL,
                               (formats_capacity > 0) ? 1U : 0U,
                               &formats_written))
            {
                return (uint64_t) -1;
            }

            if (io.format_type_ptr != 0 && formats_capacity > 0 && formats_written > 0)
            {
                if (!Syscall_copy_to_user((void*) (uintptr_t) io.format_type_ptr, &format, sizeof(format)))
                    return (uint64_t) -1;
            }

            return Syscall_copy_to_user(user_arg, &io, sizeof(io)) ? 0 : (uint64_t) -1;
        }

        case DRM_IOCTL_MODE_CREATE_DUMB:
        {
            if (!user_arg)
                return (uint64_t) -1;

            drm_mode_create_dumb_t io;
            if (!Syscall_copy_from_user(&io, user_arg, sizeof(io)))
                return (uint64_t) -1;
            if (!DRM_create_dumb(drm_file_id, &io))
                return (uint64_t) -1;
            return Syscall_copy_to_user(user_arg, &io, sizeof(io)) ? 0 : (uint64_t) -1;
        }

        case DRM_IOCTL_MODE_DESTROY_DUMB:
        {
            if (!user_arg)
                return (uint64_t) -1;

            drm_mode_destroy_dumb_t io;
            if (!Syscall_copy_from_user(&io, user_arg, sizeof(io)))
                return (uint64_t) -1;
            return DRM_destroy_dumb(drm_file_id, io.handle) ? 0 : (uint64_t) -1;
        }

        case DRM_IOCTL_MODE_CREATE_BLOB:
        {
            if (!user_arg)
                return (uint64_t) -1;

            drm_mode_create_blob_t io;
            if (!Syscall_copy_from_user(&io, user_arg, sizeof(io)))
                return (uint64_t) -1;
            if (io.length != sizeof(drm_mode_modeinfo_t) || io.data == 0)
                return (uint64_t) -1;

            drm_mode_modeinfo_t mode;
            if (!Syscall_copy_from_user(&mode, (const void*) (uintptr_t) io.data, sizeof(mode)))
                return (uint64_t) -1;

            uint32_t blob_id = 0;
            if (!DRM_create_mode_blob(drm_file_id, &mode, sizeof(mode), &blob_id))
                return (uint64_t) -1;

            io.blob_id = blob_id;
            return Syscall_copy_to_user(user_arg, &io, sizeof(io)) ? 0 : (uint64_t) -1;
        }

        case DRM_IOCTL_MODE_DESTROY_BLOB:
        {
            if (!user_arg)
                return (uint64_t) -1;

            drm_mode_destroy_blob_t io;
            if (!Syscall_copy_from_user(&io, user_arg, sizeof(io)))
                return (uint64_t) -1;
            return DRM_destroy_mode_blob(drm_file_id, io.blob_id) ? 0 : (uint64_t) -1;
        }

        case DRM_IOCTL_PRIME_HANDLE_TO_FD:
        {
            if (!user_arg)
                return (uint64_t) -1;

            drm_prime_handle_t io;
            if (!Syscall_copy_from_user(&io, user_arg, sizeof(io)))
                return (uint64_t) -1;

            uint32_t dmabuf_id = 0;
            if (!DRM_prime_handle_to_dmabuf(drm_file_id, io.handle, &dmabuf_id))
                return (uint64_t) -1;
            if (!DRM_dmabuf_ref_fd(dmabuf_id))
                return (uint64_t) -1;

            int32_t dma_fd = -1;
            spin_lock(&Syscall_state.fd_lock);
            dma_fd = Syscall_fd_alloc_locked();
            if (dma_fd >= 0)
            {
                syscall_file_desc_t* dma_entry = &Syscall_state.fds[(uint32_t) dma_fd];
                memset(dma_entry, 0, sizeof(*dma_entry));
                dma_entry->used = true;
                dma_entry->type = SYSCALL_FD_TYPE_DMABUF;
                dma_entry->owner_pid = owner_pid;
                dma_entry->drm_dmabuf_id = dmabuf_id;
                dma_entry->can_read = true;
                dma_entry->can_write = true;
            }
            spin_unlock(&Syscall_state.fd_lock);

            if (dma_fd < 0)
            {
                DRM_dmabuf_unref_fd(dmabuf_id);
                return (uint64_t) -1;
            }

            io.fd = dma_fd;
            if (!Syscall_copy_to_user(user_arg, &io, sizeof(io)))
            {
                spin_lock(&Syscall_state.fd_lock);
                syscall_file_desc_t* dma_entry = &Syscall_state.fds[(uint32_t) dma_fd];
                if (dma_entry->used && dma_entry->owner_pid == owner_pid &&
                    dma_entry->type == SYSCALL_FD_TYPE_DMABUF &&
                    dma_entry->drm_dmabuf_id == dmabuf_id)
                {
                    memset(dma_entry, 0, sizeof(*dma_entry));
                }
                spin_unlock(&Syscall_state.fd_lock);
                DRM_dmabuf_unref_fd(dmabuf_id);
                return (uint64_t) -1;
            }
            return 0;
        }

        case DRM_IOCTL_PRIME_FD_TO_HANDLE:
        {
            if (!user_arg)
                return (uint64_t) -1;

            drm_prime_handle_t io;
            if (!Syscall_copy_from_user(&io, user_arg, sizeof(io)))
                return (uint64_t) -1;

            int32_t dma_fd = io.fd;
            if (dma_fd < 0 || (uint64_t) dma_fd >= SYSCALL_MAX_OPEN_FILES)
                return (uint64_t) -1;

            uint32_t dmabuf_id = 0;
            spin_lock(&Syscall_state.fd_lock);
            syscall_file_desc_t* dma_entry = &Syscall_state.fds[(uint32_t) dma_fd];
            if (dma_entry->used && dma_entry->owner_pid == owner_pid &&
                dma_entry->type == SYSCALL_FD_TYPE_DMABUF)
            {
                dmabuf_id = dma_entry->drm_dmabuf_id;
            }
            spin_unlock(&Syscall_state.fd_lock);

            if (dmabuf_id == 0)
                return (uint64_t) -1;

            uint32_t handle = 0;
            if (!DRM_prime_dmabuf_to_handle(drm_file_id, dmabuf_id, &handle))
                return (uint64_t) -1;

            io.handle = handle;
            return Syscall_copy_to_user(user_arg, &io, sizeof(io)) ? 0 : (uint64_t) -1;
        }

        case DRM_IOCTL_MODE_ATOMIC:
        {
            if (!user_arg)
                return (uint64_t) -1;

            drm_mode_atomic_req_t io;
            if (!Syscall_copy_from_user(&io, user_arg, sizeof(io)))
                return (uint64_t) -1;

            return DRM_atomic_commit(drm_file_id, &io) ? 0 : (uint64_t) -1;
        }

        case DRM_IOCTL_SET_MASTER:
            return DRM_set_master(drm_file_id) ? 0 : (uint64_t) -1;

        case DRM_IOCTL_DROP_MASTER:
            return DRM_drop_master(drm_file_id) ? 0 : (uint64_t) -1;

        default:
            return (uint64_t) -1;
    }
}

static inline uintptr_t Syscall_read_cr3_phys(void)
{
    uintptr_t cr3 = 0;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & FRAME;
}

static inline void Syscall_write_cr3_phys(uintptr_t cr3_phys)
{
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(cr3_phys) : "memory");
}

static inline uintptr_t Syscall_read_fs_base(void)
{
    return (uintptr_t) MSR_get(IA32_FS_BASE);
}

static inline void Syscall_write_fs_base(uintptr_t fs_base)
{
    MSR_set(IA32_FS_BASE, (uint64_t) fs_base);
}

static uintptr_t Syscall_alloc_zero_page_phys(void)
{
    uintptr_t phys = (uintptr_t) PMM_alloc_page();
    if (phys == 0)
        return 0;

    memset((void*) P2V(phys), 0, SYSCALL_PAGE_SIZE);
    return phys;
}

static bool Syscall_is_canonical_low(uint64_t value)
{
    return value < SYSCALL_ELF_CANONICAL_LOW_MAX;
}

static bool Syscall_user_fs_base_is_valid(uintptr_t fs_base)
{
    if (fs_base == 0)
        return true;

    return Syscall_is_canonical_low(fs_base) &&
           fs_base >= SYSCALL_USER_VADDR_MIN &&
           fs_base <= SYSCALL_USER_VADDR_MAX;
}

static int32_t Syscall_cow_ref_find_locked(uintptr_t phys)
{
    for (uint32_t i = 0; i < SYSCALL_COW_MAX_REFS; i++)
    {
        if (Syscall_state.cow_refs[i].used && Syscall_state.cow_refs[i].phys == phys)
            return (int32_t) i;
    }
    return -1;
}

static int32_t Syscall_cow_ref_alloc_locked(void)
{
    for (uint32_t i = 0; i < SYSCALL_COW_MAX_REFS; i++)
    {
        if (!Syscall_state.cow_refs[i].used)
            return (int32_t) i;
    }
    return -1;
}

static bool Syscall_cow_ref_add(uintptr_t phys, uint32_t delta)
{
    if (phys == 0 || delta == 0 || !Syscall_state.cow_lock_ready)
        return false;

    spin_lock(&Syscall_state.cow_lock);
    int32_t slot = Syscall_cow_ref_find_locked(phys);
    if (slot >= 0)
    {
        syscall_cow_ref_t* ref = &Syscall_state.cow_refs[(uint32_t) slot];
        if (ref->refs > UINT32_MAX - delta)
        {
            spin_unlock(&Syscall_state.cow_lock);
            return false;
        }
        ref->refs += delta;
        spin_unlock(&Syscall_state.cow_lock);
        return true;
    }

    slot = Syscall_cow_ref_alloc_locked();
    if (slot < 0)
    {
        spin_unlock(&Syscall_state.cow_lock);
        return false;
    }

    syscall_cow_ref_t* ref = &Syscall_state.cow_refs[(uint32_t) slot];
    ref->used = true;
    ref->phys = phys;
    ref->refs = delta;
    spin_unlock(&Syscall_state.cow_lock);
    return true;
}

static bool Syscall_cow_ref_sub(uintptr_t phys, bool* out_zero)
{
    if (out_zero)
        *out_zero = false;
    if (phys == 0 || !Syscall_state.cow_lock_ready)
        return false;

    spin_lock(&Syscall_state.cow_lock);
    int32_t slot = Syscall_cow_ref_find_locked(phys);
    if (slot < 0)
    {
        spin_unlock(&Syscall_state.cow_lock);
        return false;
    }

    syscall_cow_ref_t* ref = &Syscall_state.cow_refs[(uint32_t) slot];
    if (ref->refs <= 1U)
    {
        ref->used = false;
        ref->phys = 0;
        ref->refs = 0;
        if (out_zero)
            *out_zero = true;
    }
    else
    {
        ref->refs -= 1U;
    }

    spin_unlock(&Syscall_state.cow_lock);
    return true;
}

static uint32_t Syscall_cow_ref_get(uintptr_t phys)
{
    if (phys == 0 || !Syscall_state.cow_lock_ready)
        return 0;

    spin_lock(&Syscall_state.cow_lock);
    int32_t slot = Syscall_cow_ref_find_locked(phys);
    uint32_t refs = (slot >= 0) ? Syscall_state.cow_refs[(uint32_t) slot].refs : 0U;
    spin_unlock(&Syscall_state.cow_lock);
    return refs;
}

static uint64_t* Syscall_get_user_pte_ptr(uintptr_t cr3_phys, uintptr_t virt)
{
    if (cr3_phys == 0 || !Syscall_is_canonical_low(virt) || virt < SYSCALL_USER_VADDR_MIN)
        return NULL;

    uint16_t pml4_index = PML4_INDEX(virt);
    uint16_t pdpt_index = PDPT_INDEX(virt);
    uint16_t pdt_index = PDT_INDEX(virt);
    uint16_t pt_index = PT_INDEX(virt);
    if (pml4_index >= VMM_HHDM_PML4_INDEX)
        return NULL;

    PML4_t* pml4 = (PML4_t*) P2V(cr3_phys);
    uintptr_t pml4_entry = pml4->entries[pml4_index];
    if ((pml4_entry & PRESENT) == 0 || (pml4_entry & USER_MODE) == 0)
        return NULL;

    PDPT_t* pdpt = (PDPT_t*) P2V(pml4_entry & FRAME);
    uintptr_t pdpt_entry = pdpt->entries[pdpt_index];
    if ((pdpt_entry & PRESENT) == 0 || (pdpt_entry & USER_MODE) == 0)
        return NULL;
    if ((pdpt_entry & SYSCALL_PTE_PS) != 0)
        return NULL;

    PDT_t* pdt = (PDT_t*) P2V(pdpt_entry & FRAME);
    uintptr_t pdt_entry = pdt->entries[pdt_index];
    if ((pdt_entry & PRESENT) == 0 || (pdt_entry & USER_MODE) == 0)
        return NULL;
    if ((pdt_entry & SYSCALL_PTE_PS) != 0)
        return NULL;

    PT_t* pt = (PT_t*) P2V(pdt_entry & FRAME);
    return &pt->entries[pt_index];
}

static void Syscall_free_user_pt(uintptr_t pt_phys)
{
    if (pt_phys == 0)
        return;

    PT_t* pt = (PT_t*) P2V(pt_phys);
    for (uint32_t i = 0; i < 512; i++)
    {
        uintptr_t entry = pt->entries[i];
        if ((entry & PRESENT) == 0)
            continue;

        uintptr_t page_phys = entry & FRAME;
        if (page_phys == 0)
            continue;

        if ((entry & SYSCALL_PTE_DMABUF) != 0)
        {
            (void) DRM_dmabuf_unref_map_pages_by_phys(page_phys, 1U);
            continue;
        }

        if ((entry & SYSCALL_PTE_COW) != 0)
        {
            bool ref_zero = false;
            if (Syscall_cow_ref_sub(page_phys, &ref_zero) && ref_zero)
                PMM_dealloc_page((void*) page_phys);
            continue;
        }

        PMM_dealloc_page((void*) page_phys);
    }

    PMM_dealloc_page((void*) pt_phys);
}

static void Syscall_free_user_pdt(uintptr_t pdt_phys)
{
    if (pdt_phys == 0)
        return;

    PDT_t* pdt = (PDT_t*) P2V(pdt_phys);
    for (uint32_t i = 0; i < 512; i++)
    {
        uintptr_t entry = pdt->entries[i];
        if ((entry & PRESENT) == 0)
            continue;
        if ((entry & SYSCALL_PTE_PS) != 0)
            continue;

        Syscall_free_user_pt(entry & FRAME);
    }

    PMM_dealloc_page((void*) pdt_phys);
}

static void Syscall_free_user_pdpt(uintptr_t pdpt_phys)
{
    if (pdpt_phys == 0)
        return;

    PDPT_t* pdpt = (PDPT_t*) P2V(pdpt_phys);
    for (uint32_t i = 0; i < 512; i++)
    {
        uintptr_t entry = pdpt->entries[i];
        if ((entry & PRESENT) == 0)
            continue;
        if ((entry & SYSCALL_PTE_PS) != 0)
            continue;

        Syscall_free_user_pdt(entry & FRAME);
    }

    PMM_dealloc_page((void*) pdpt_phys);
}

static void Syscall_free_address_space(uintptr_t cr3_phys)
{
    if (cr3_phys == 0)
        return;

    PML4_t* pml4 = (PML4_t*) P2V(cr3_phys);
    for (uint32_t i = 0; i < VMM_HHDM_PML4_INDEX; i++)
    {
        uintptr_t entry = pml4->entries[i];
        if ((entry & PRESENT) == 0)
            continue;

        Syscall_free_user_pdpt(entry & FRAME);
    }

    PMM_dealloc_page((void*) cr3_phys);
}

static bool Syscall_clone_user_pt(uintptr_t src_pt_phys, uintptr_t* out_dst_pt_phys)
{
    if (!out_dst_pt_phys || src_pt_phys == 0)
        return false;

    uintptr_t dst_pt_phys = Syscall_alloc_zero_page_phys();
    if (dst_pt_phys == 0)
        return false;

    PT_t* src_pt = (PT_t*) P2V(src_pt_phys);
    PT_t* dst_pt = (PT_t*) P2V(dst_pt_phys);

    for (uint32_t i = 0; i < 512; i++)
    {
        uintptr_t src_entry = src_pt->entries[i];
        if ((src_entry & PRESENT) == 0)
            continue;
        if ((src_entry & SYSCALL_PTE_PS) != 0)
        {
            Syscall_free_user_pt(dst_pt_phys);
            return false;
        }

        uintptr_t src_page_phys = src_entry & FRAME;
        if (src_page_phys == 0)
        {
            dst_pt->entries[i] = src_entry;
            continue;
        }

        if ((src_entry & SYSCALL_PTE_DMABUF) != 0)
        {
            if (!DRM_dmabuf_ref_map_pages_by_phys(src_page_phys, 1U))
            {
                Syscall_free_user_pt(dst_pt_phys);
                return false;
            }

            dst_pt->entries[i] = src_entry;
            continue;
        }

        bool writable = (src_entry & WRITABLE) != 0;
        bool already_cow = (src_entry & SYSCALL_PTE_COW) != 0;
        if (writable || already_cow)
        {
            uintptr_t shared_entry = src_entry & ~WRITABLE;
            shared_entry |= SYSCALL_PTE_COW;

            uint32_t delta = already_cow ? 1U : 2U;
            if (!Syscall_cow_ref_add(src_page_phys, delta))
            {
                Syscall_free_user_pt(dst_pt_phys);
                return false;
            }

            src_pt->entries[i] = shared_entry;
            dst_pt->entries[i] = shared_entry;
            continue;
        }

        uintptr_t dst_page_phys = Syscall_alloc_zero_page_phys();
        if (dst_page_phys == 0)
        {
            Syscall_free_user_pt(dst_pt_phys);
            return false;
        }

        memcpy((void*) P2V(dst_page_phys), (const void*) P2V(src_page_phys), SYSCALL_PAGE_SIZE);
        dst_pt->entries[i] = (src_entry & ~FRAME) | dst_page_phys;
    }

    *out_dst_pt_phys = dst_pt_phys;
    return true;
}

static bool Syscall_clone_user_pdt(uintptr_t src_pdt_phys, uintptr_t* out_dst_pdt_phys)
{
    if (!out_dst_pdt_phys || src_pdt_phys == 0)
        return false;

    uintptr_t dst_pdt_phys = Syscall_alloc_zero_page_phys();
    if (dst_pdt_phys == 0)
        return false;

    PDT_t* src_pdt = (PDT_t*) P2V(src_pdt_phys);
    PDT_t* dst_pdt = (PDT_t*) P2V(dst_pdt_phys);
    for (uint32_t i = 0; i < 512; i++)
    {
        uintptr_t src_entry = src_pdt->entries[i];
        if ((src_entry & PRESENT) == 0)
            continue;
        if ((src_entry & SYSCALL_PTE_PS) != 0)
        {
            Syscall_free_user_pdt(dst_pdt_phys);
            return false;
        }

        uintptr_t dst_pt_phys = 0;
        if (!Syscall_clone_user_pt(src_entry & FRAME, &dst_pt_phys))
        {
            Syscall_free_user_pdt(dst_pdt_phys);
            return false;
        }

        dst_pdt->entries[i] = (src_entry & ~FRAME) | dst_pt_phys;
    }

    *out_dst_pdt_phys = dst_pdt_phys;
    return true;
}

static bool Syscall_clone_user_pdpt(uintptr_t src_pdpt_phys, uintptr_t* out_dst_pdpt_phys)
{
    if (!out_dst_pdpt_phys || src_pdpt_phys == 0)
        return false;

    uintptr_t dst_pdpt_phys = Syscall_alloc_zero_page_phys();
    if (dst_pdpt_phys == 0)
        return false;

    PDPT_t* src_pdpt = (PDPT_t*) P2V(src_pdpt_phys);
    PDPT_t* dst_pdpt = (PDPT_t*) P2V(dst_pdpt_phys);
    for (uint32_t i = 0; i < 512; i++)
    {
        uintptr_t src_entry = src_pdpt->entries[i];
        if ((src_entry & PRESENT) == 0)
            continue;
        if ((src_entry & SYSCALL_PTE_PS) != 0)
        {
            Syscall_free_user_pdpt(dst_pdpt_phys);
            return false;
        }

        uintptr_t dst_pdt_phys = 0;
        if (!Syscall_clone_user_pdt(src_entry & FRAME, &dst_pdt_phys))
        {
            Syscall_free_user_pdpt(dst_pdpt_phys);
            return false;
        }

        dst_pdpt->entries[i] = (src_entry & ~FRAME) | dst_pdt_phys;
    }

    *out_dst_pdpt_phys = dst_pdpt_phys;
    return true;
}

static bool Syscall_create_kernel_mirrored_address_space(uintptr_t src_cr3_phys, uintptr_t* out_dst_cr3_phys)
{
    if (!out_dst_cr3_phys || src_cr3_phys == 0)
        return false;

    uintptr_t dst_cr3_phys = Syscall_alloc_zero_page_phys();
    if (dst_cr3_phys == 0)
        return false;

    PML4_t* src_pml4 = (PML4_t*) P2V(src_cr3_phys);
    PML4_t* dst_pml4 = (PML4_t*) P2V(dst_cr3_phys);
    for (uint32_t i = VMM_HHDM_PML4_INDEX; i < 512; i++)
        dst_pml4->entries[i] = src_pml4->entries[i];

    // Keep recursive slot coherent in the new PML4.
    dst_pml4->entries[VMM_RECURSIVE_INDEX] = dst_cr3_phys | PRESENT | WRITABLE;

    *out_dst_cr3_phys = dst_cr3_phys;
    return true;
}

static bool Syscall_clone_address_space(uintptr_t src_cr3_phys, uintptr_t* out_dst_cr3_phys)
{
    if (!out_dst_cr3_phys || src_cr3_phys == 0)
        return false;

    uintptr_t dst_cr3_phys = 0;
    if (!Syscall_create_kernel_mirrored_address_space(src_cr3_phys, &dst_cr3_phys))
        return false;

    PML4_t* src_pml4 = (PML4_t*) P2V(src_cr3_phys);
    PML4_t* dst_pml4 = (PML4_t*) P2V(dst_cr3_phys);
    for (uint32_t i = 0; i < VMM_HHDM_PML4_INDEX; i++)
    {
        uintptr_t src_entry = src_pml4->entries[i];
        if ((src_entry & PRESENT) == 0)
            continue;

        uintptr_t dst_pdpt_phys = 0;
        if (!Syscall_clone_user_pdpt(src_entry & FRAME, &dst_pdpt_phys))
        {
            Syscall_free_address_space(dst_cr3_phys);
            return false;
        }

        dst_pml4->entries[i] = (src_entry & ~FRAME) | dst_pdpt_phys;
    }

    if (src_cr3_phys == Syscall_read_cr3_phys())
        Syscall_write_cr3_phys(src_cr3_phys);

    *out_dst_cr3_phys = dst_cr3_phys;
    return true;
}

static bool Syscall_map_user_range_current(uintptr_t base, size_t size)
{
    if (size == 0)
        return true;

    uintptr_t end_raw = base + (uintptr_t) size;
    if (end_raw < base)
        return false;

    uintptr_t start = base & ~(uintptr_t) (SYSCALL_PAGE_SIZE - 1U);
    uintptr_t end = Syscall_align_up_page(end_raw);

    for (uintptr_t page = start; page < end; page += SYSCALL_PAGE_SIZE)
    {
        uintptr_t phys = 0;
        if (!VMM_virt_to_phys(page, &phys))
        {
            uintptr_t new_page = (uintptr_t) PMM_alloc_page();
            if (new_page == 0)
                return false;

            VMM_map_user_page(page, new_page);
            memset((void*) P2V(new_page), 0, SYSCALL_PAGE_SIZE);
        }
        else if (!VMM_is_user_accessible(page))
            return false;
    }

    return true;
}

static bool Syscall_zero_user_phys(uintptr_t user_dst, size_t size)
{
    if (size == 0)
        return true;
    if (!Syscall_user_range_in_bounds(user_dst, size))
        return false;

    size_t done = 0;
    while (done < size)
    {
        uintptr_t cur = user_dst + done;
        uintptr_t phys = 0;
        if (!VMM_virt_to_phys(cur, &phys))
            return false;

        size_t page_rem = SYSCALL_PAGE_SIZE - (size_t) (cur & (SYSCALL_PAGE_SIZE - 1U));
        size_t chunk = size - done;
        if (chunk > page_rem)
            chunk = page_rem;

        memset((void*) P2V(phys), 0, chunk);
        done += chunk;
    }

    return true;
}

static bool Syscall_copy_into_user_phys(uintptr_t user_dst, const void* src, size_t size)
{
    if (size == 0)
        return true;
    if (!src || !Syscall_user_range_in_bounds(user_dst, size))
        return false;

    size_t done = 0;
    while (done < size)
    {
        uintptr_t cur = user_dst + done;
        uintptr_t phys = 0;
        if (!VMM_virt_to_phys(cur, &phys))
            return false;

        size_t page_rem = SYSCALL_PAGE_SIZE - (size_t) (cur & (SYSCALL_PAGE_SIZE - 1U));
        size_t chunk = size - done;
        if (chunk > page_rem)
            chunk = page_rem;

        memcpy((void*) P2V(phys), (const uint8_t*) src + done, chunk);
        done += chunk;
    }

    return true;
}

typedef struct syscall_exec_segment
{
    uintptr_t start;
    uintptr_t end;
    bool writable;
    bool executable;
} syscall_exec_segment_t;

typedef struct syscall_exec_module
{
    char path[SYSCALL_USER_CSTR_MAX];
    char soname[SYSCALL_USER_CSTR_MAX];
    uint8_t* image;
    size_t image_size;
    uintptr_t load_bias;
    uintptr_t map_start;
    uintptr_t map_end;
    uintptr_t entry;
    uintptr_t tls_vaddr;
    size_t tls_filesz;
    size_t tls_memsz;
    size_t tls_align;
    size_t tls_rounded_size;
    const char* strtab;
    size_t strsz;
    const syscall_elf64_sym_t* symtab;
    size_t sym_count;
    const syscall_elf64_rela_t* rela;
    size_t rela_count;
    const syscall_elf64_rela_t* plt_rela;
    size_t plt_rela_count;
    uint32_t needed_offsets[SYSCALL_EXEC_MAX_NEEDED];
    size_t needed_count;
    syscall_exec_segment_t segments[SYSCALL_EXEC_MAX_SEGMENTS];
    size_t segment_count;
} syscall_exec_module_t;

static bool Syscall_exec_read_file(const char* path, uint8_t** out_data, size_t* out_size)
{
    if (!path || !out_data || !out_size)
        return false;

    *out_data = NULL;
    *out_size = 0;

    if (!VFS_is_ready())
        return false;
    return VFS_read_file(path, out_data, out_size);
}

static bool Syscall_exec_module_addr_range_valid(const syscall_exec_module_t* module,
                                                 uintptr_t addr,
                                                 size_t size)
{
    if (!module || size == 0)
        return false;
    if (addr < module->map_start)
        return false;
    uintptr_t end = addr + (uintptr_t) size;
    if (end < addr || end > module->map_end)
        return false;
    return true;
}

static bool Syscall_exec_has_nul_terminator(const char* str, size_t max_len)
{
    if (!str)
        return false;

    for (size_t i = 0; i < max_len; i++)
    {
        if (str[i] == '\0')
            return true;
    }
    return false;
}

static const char* Syscall_exec_dynstr_at(const syscall_exec_module_t* module, uint32_t offset)
{
    if (!module || !module->strtab || module->strsz == 0U)
        return NULL;
    if ((size_t) offset >= module->strsz)
        return NULL;

    const char* str = module->strtab + offset;
    size_t max_len = module->strsz - (size_t) offset;
    if (!Syscall_exec_has_nul_terminator(str, max_len))
        return NULL;
    return str;
}

static bool Syscall_exec_build_needed_path(const char* needed_name, char* out_path, size_t out_size)
{
    if (!needed_name || !out_path || out_size == 0)
        return false;

    if (needed_name[0] == '/')
    {
        size_t len = strlen(needed_name);
        if (len + 1U > out_size)
            return false;
        memcpy(out_path, needed_name, len + 1U);
        return true;
    }

    static const char lib_prefix[] = "/lib/";
    size_t prefix_len = sizeof(lib_prefix) - 1U;
    size_t name_len = strlen(needed_name);
    if (prefix_len + name_len + 1U > out_size)
        return false;

    memcpy(out_path, lib_prefix, prefix_len);
    memcpy(out_path + prefix_len, needed_name, name_len + 1U);
    return true;
}

static bool Syscall_exec_vaddr_to_file_ptr(const uint8_t* elf_image,
                                           size_t elf_size,
                                           const syscall_elf64_ehdr_t* ehdr,
                                           uint64_t vaddr,
                                           size_t size,
                                           const void** out_ptr)
{
    if (!elf_image || !ehdr || !out_ptr || size == 0U)
        return false;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++)
    {
        const uint8_t* ph_ptr = elf_image + ehdr->e_phoff + ((uint64_t) i * ehdr->e_phentsize);
        const syscall_elf64_phdr_t* phdr = (const syscall_elf64_phdr_t*) ph_ptr;
        if (phdr->p_type != SYSCALL_ELF_PT_LOAD || phdr->p_filesz == 0U)
            continue;

        uint64_t seg_start = phdr->p_vaddr;
        uint64_t seg_end = seg_start + phdr->p_filesz;
        if (seg_end < seg_start)
            return false;
        if (vaddr < seg_start || vaddr >= seg_end)
            continue;

        uint64_t rel = vaddr - seg_start;
        if (rel > UINT64_MAX - size)
            return false;
        uint64_t rel_end = rel + (uint64_t) size;
        if (rel_end > phdr->p_filesz)
            continue;
        if (phdr->p_offset > elf_size)
            return false;
        if (rel_end > (uint64_t) elf_size - phdr->p_offset)
            return false;

        *out_ptr = elf_image + phdr->p_offset + (size_t) rel;
        return true;
    }

    return false;
}

static bool Syscall_exec_record_segment(syscall_exec_module_t* module,
                                        uintptr_t seg_start,
                                        uintptr_t seg_end,
                                        bool writable,
                                        bool executable)
{
    if (!module || seg_end <= seg_start)
        return false;
    if (module->segment_count >= SYSCALL_EXEC_MAX_SEGMENTS)
        return false;

    syscall_exec_segment_t* seg = &module->segments[module->segment_count++];
    seg->start = seg_start;
    seg->end = seg_end;
    seg->writable = writable;
    seg->executable = executable;
    return true;
}

static bool Syscall_load_elf_segment_current(const uint8_t* elf_image,
                                             size_t elf_size,
                                             const syscall_elf64_phdr_t* phdr,
                                             uintptr_t load_bias,
                                             syscall_exec_module_t* module)
{
    if (!phdr || phdr->p_type != SYSCALL_ELF_PT_LOAD || !module)
        return false;

    if (phdr->p_memsz == 0)
        return true;

    if (phdr->p_filesz > phdr->p_memsz)
        return false;
    if (phdr->p_offset > elf_size)
        return false;
    if (phdr->p_filesz > (uint64_t) elf_size - phdr->p_offset)
        return false;

    uintptr_t seg_base = load_bias + (uintptr_t) phdr->p_vaddr;
    if (seg_base < load_bias)
        return false;
    if (!Syscall_is_canonical_low(seg_base) || seg_base < SYSCALL_USER_VADDR_MIN)
        return false;

    uintptr_t seg_end = seg_base + (uintptr_t) phdr->p_memsz;
    if (seg_end < seg_base || !Syscall_is_canonical_low(seg_end - 1U))
        return false;

    uintptr_t page_start = Syscall_align_down_page(seg_base);
    uintptr_t page_end = Syscall_align_up_page(seg_end);
    if (page_end <= page_start)
        return false;

    bool writable = (phdr->p_flags & SYSCALL_ELF_PF_W) != 0;
    bool executable = (phdr->p_flags & SYSCALL_ELF_PF_X) != 0;
    if (writable && executable)
        return false;

    if (!Syscall_map_user_range_current(seg_base, (size_t) phdr->p_memsz))
        return false;
    if (!Syscall_zero_user_phys(seg_base, (size_t) phdr->p_memsz))
        return false;
    if (phdr->p_filesz != 0 &&
        !Syscall_copy_into_user_phys(seg_base, elf_image + phdr->p_offset, (size_t) phdr->p_filesz))
        return false;

    return Syscall_exec_record_segment(module, page_start, page_end, writable, executable);
}

static bool Syscall_exec_load_module_current(const char* path,
                                             bool is_main,
                                             uintptr_t* io_dyn_cursor,
                                             syscall_exec_module_t* modules,
                                             size_t* io_module_count)
{
    if (!path || !io_dyn_cursor || !modules || !io_module_count)
        return false;
    if (*io_module_count >= SYSCALL_EXEC_MAX_MODULES)
        return false;

    uint8_t* elf_image = NULL;
    size_t elf_size = 0;
    if (!Syscall_exec_read_file(path, &elf_image, &elf_size))
    {
        kdebug_printf("[USER] exec reject '%s': read failed\n", path);
        return false;
    }

    if (!elf_image || elf_size < sizeof(syscall_elf64_ehdr_t) || elf_size > SYSCALL_ELF_MAX_SIZE)
    {
        kdebug_printf("[USER] exec reject '%s': ELF size=%llu (max=%llu)\n",
                      path,
                      (unsigned long long) elf_size,
                      (unsigned long long) SYSCALL_ELF_MAX_SIZE);
        if (elf_image)
            kfree(elf_image);
        return false;
    }

    const syscall_elf64_ehdr_t* ehdr = (const syscall_elf64_ehdr_t*) elf_image;
    if (ehdr->e_ident[0] != 0x7FU ||
        ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' ||
        ehdr->e_ident[3] != 'F' ||
        ehdr->e_ident[4] != 2U ||
        ehdr->e_ident[5] != 1U ||
        (ehdr->e_type != SYSCALL_ELF_TYPE_EXEC && ehdr->e_type != SYSCALL_ELF_TYPE_DYN) ||
        ehdr->e_machine != 0x3EU)
    {
        kdebug_printf("[USER] exec reject '%s': invalid ELF header\n", path);
        kfree(elf_image);
        return false;
    }

    if (!is_main && ehdr->e_type != SYSCALL_ELF_TYPE_DYN)
    {
        kdebug_printf("[USER] exec reject '%s': dependency must be ET_DYN\n", path);
        kfree(elf_image);
        return false;
    }

    if (ehdr->e_phnum == 0 || ehdr->e_phnum > SYSCALL_ELF_MAX_PHDRS ||
        ehdr->e_phentsize < sizeof(syscall_elf64_phdr_t) ||
        ehdr->e_phoff > elf_size)
    {
        kdebug_printf("[USER] exec reject '%s': invalid program header table\n", path);
        kfree(elf_image);
        return false;
    }

    uint64_t ph_table_size = (uint64_t) ehdr->e_phnum * (uint64_t) ehdr->e_phentsize;
    if (ph_table_size > (uint64_t) elf_size - ehdr->e_phoff)
    {
        kdebug_printf("[USER] exec reject '%s': program headers out of bounds\n", path);
        kfree(elf_image);
        return false;
    }

    bool has_load = false;
    uintptr_t min_load_vaddr = UINTPTR_MAX;
    uintptr_t max_load_vaddr = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++)
    {
        const uint8_t* ph_ptr = elf_image + ehdr->e_phoff + ((uint64_t) i * ehdr->e_phentsize);
        const syscall_elf64_phdr_t* phdr = (const syscall_elf64_phdr_t*) ph_ptr;
        if (phdr->p_type != SYSCALL_ELF_PT_LOAD)
            continue;

        has_load = true;
        if (phdr->p_memsz == 0)
            continue;

        uintptr_t seg_start = (uintptr_t) phdr->p_vaddr;
        uintptr_t seg_end = seg_start + (uintptr_t) phdr->p_memsz;
        if (seg_end < seg_start)
        {
            kdebug_printf("[USER] exec reject '%s': PT_LOAD #%u overflow\n",
                          path,
                          (unsigned int) i);
            kfree(elf_image);
            return false;
        }

        uintptr_t seg_page_start = Syscall_align_down_page(seg_start);
        uintptr_t seg_page_end = Syscall_align_up_page(seg_end);
        if (seg_page_end <= seg_page_start)
        {
            kdebug_printf("[USER] exec reject '%s': PT_LOAD #%u page range invalid\n",
                          path,
                          (unsigned int) i);
            kfree(elf_image);
            return false;
        }

        if (seg_page_start < min_load_vaddr)
            min_load_vaddr = seg_page_start;
        if (seg_page_end > max_load_vaddr)
            max_load_vaddr = seg_page_end;
    }

    if (!has_load || min_load_vaddr == UINTPTR_MAX || max_load_vaddr <= min_load_vaddr)
    {
        kdebug_printf("[USER] exec reject '%s': no PT_LOAD segments\n", path);
        kfree(elf_image);
        return false;
    }

    uintptr_t load_bias = 0;
    uintptr_t elf_entry = 0;
    if (ehdr->e_type == SYSCALL_ELF_TYPE_EXEC)
    {
        if (!Syscall_is_canonical_low(ehdr->e_entry) || ehdr->e_entry < SYSCALL_USER_VADDR_MIN)
        {
            kdebug_printf("[USER] exec reject '%s': entry out of user range (0x%llX)\n",
                          path,
                          (unsigned long long) ehdr->e_entry);
            kfree(elf_image);
            return false;
        }
        elf_entry = (uintptr_t) ehdr->e_entry;
    }
    else if (is_main)
    {
        uintptr_t dyn_base = Syscall_align_up_pow2(SYSCALL_ELF_DYN_BASE, SYSCALL_ELF_DYN_ALIGN);
        if (dyn_base == 0 || min_load_vaddr > dyn_base)
        {
            kdebug_printf("[USER] exec reject '%s': invalid ET_DYN base\n", path);
            kfree(elf_image);
            return false;
        }

        load_bias = dyn_base - min_load_vaddr;
        uintptr_t dyn_top = load_bias + max_load_vaddr;
        if (dyn_top < load_bias || !Syscall_is_canonical_low(dyn_top - 1U))
        {
            kdebug_printf("[USER] exec reject '%s': ET_DYN mapped range invalid\n", path);
            kfree(elf_image);
            return false;
        }

        elf_entry = load_bias + (uintptr_t) ehdr->e_entry;
        if (elf_entry < load_bias || !Syscall_is_canonical_low(elf_entry) || elf_entry < SYSCALL_USER_VADDR_MIN)
        {
            kdebug_printf("[USER] exec reject '%s': ET_DYN entry invalid (0x%llX)\n",
                          path,
                          (unsigned long long) elf_entry);
            kfree(elf_image);
            return false;
        }
    }
    else
    {
        uintptr_t cursor = *io_dyn_cursor;
        if (cursor < SYSCALL_ELF_DSO_BASE)
            cursor = SYSCALL_ELF_DSO_BASE;

        uintptr_t base = Syscall_align_up_pow2(cursor, SYSCALL_ELF_DSO_ALIGN);
        if (base == 0 || min_load_vaddr > base)
        {
            kdebug_printf("[USER] exec reject '%s': dependency base allocation failed\n", path);
            kfree(elf_image);
            return false;
        }

        load_bias = base - min_load_vaddr;
        uintptr_t map_top = load_bias + max_load_vaddr;
        if (map_top < load_bias || map_top > SYSCALL_ELF_DSO_LIMIT || !Syscall_is_canonical_low(map_top - 1U))
        {
            kdebug_printf("[USER] exec reject '%s': dependency mapped range invalid\n", path);
            kfree(elf_image);
            return false;
        }

        uintptr_t next_cursor = Syscall_align_up_pow2(map_top, SYSCALL_ELF_DSO_ALIGN);
        if (next_cursor == 0 || next_cursor > SYSCALL_ELF_DSO_LIMIT)
        {
            kdebug_printf("[USER] exec reject '%s': dependency cursor overflow\n", path);
            kfree(elf_image);
            return false;
        }
        *io_dyn_cursor = next_cursor;

        elf_entry = load_bias + (uintptr_t) ehdr->e_entry;
    }

    syscall_exec_module_t* module = &modules[*io_module_count];
    memset(module, 0, sizeof(*module));
    size_t path_len = strlen(path);
    if (path_len >= sizeof(module->path))
        path_len = sizeof(module->path) - 1U;
    memcpy(module->path, path, path_len);
    module->path[path_len] = '\0';
    module->load_bias = load_bias;
    module->map_start = load_bias + min_load_vaddr;
    module->map_end = load_bias + max_load_vaddr;
    module->entry = elf_entry;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++)
    {
        const uint8_t* ph_ptr = elf_image + ehdr->e_phoff + ((uint64_t) i * ehdr->e_phentsize);
        const syscall_elf64_phdr_t* phdr = (const syscall_elf64_phdr_t*) ph_ptr;
        if (phdr->p_type != SYSCALL_ELF_PT_LOAD)
            continue;

        if (!Syscall_load_elf_segment_current(elf_image, elf_size, phdr, load_bias, module))
        {
            kdebug_printf("[USER] exec reject '%s': failed to load PT_LOAD segment #%u\n",
                          path,
                          (unsigned int) i);
            kfree(elf_image);
            return false;
        }
    }
    bool has_dynamic = false;
    uintptr_t strtab_addr = 0;
    uintptr_t symtab_addr = 0;
    uintptr_t hash_addr = 0;
    uintptr_t rela_addr = 0;
    size_t rela_size = 0;
    size_t rela_ent = sizeof(syscall_elf64_rela_t);
    uintptr_t jmprel_addr = 0;
    size_t plt_rela_size = 0;
    uintptr_t plt_rel_type = SYSCALL_ELF_DT_RELA;
    size_t sym_ent = sizeof(syscall_elf64_sym_t);
    uint32_t soname_offset = UINT32_MAX;
    bool dynamic_ptr_overflow = false;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++)
    {
        const uint8_t* ph_ptr = elf_image + ehdr->e_phoff + ((uint64_t) i * ehdr->e_phentsize);
        const syscall_elf64_phdr_t* phdr = (const syscall_elf64_phdr_t*) ph_ptr;
        if (phdr->p_type == SYSCALL_ELF_PT_DYNAMIC)
        {
            if (phdr->p_filesz == 0U ||
                phdr->p_filesz > phdr->p_memsz ||
                (phdr->p_filesz % sizeof(syscall_elf64_dyn_t)) != 0U ||
                phdr->p_offset > elf_size ||
                phdr->p_filesz > (uint64_t) elf_size - phdr->p_offset)
            {
                kdebug_printf("[USER] exec reject '%s': invalid PT_DYNAMIC size\n", path);
                kfree(elf_image);
                return false;
            }

            const uint8_t* dyn_bytes = elf_image + phdr->p_offset;
            size_t dyn_count = (size_t) (phdr->p_filesz / sizeof(syscall_elf64_dyn_t));
            has_dynamic = true;
            for (size_t d = 0; d < dyn_count; d++)
            {
                syscall_elf64_dyn_t dyn_entry;
                memcpy(&dyn_entry, dyn_bytes + (d * sizeof(syscall_elf64_dyn_t)), sizeof(dyn_entry));

                if (dyn_entry.d_tag == SYSCALL_ELF_DT_NULL)
                    break;

                switch (dyn_entry.d_tag)
                {
                    case SYSCALL_ELF_DT_NEEDED:
                        if (module->needed_count >= SYSCALL_EXEC_MAX_NEEDED ||
                            dyn_entry.d_un.d_val > UINT32_MAX)
                        {
                            kdebug_printf("[USER] exec reject '%s': too many DT_NEEDED entries\n", path);
                            kfree(elf_image);
                            return false;
                        }
                        module->needed_offsets[module->needed_count++] = (uint32_t) dyn_entry.d_un.d_val;
                        break;
                    case SYSCALL_ELF_DT_HASH:
                        if ((uintptr_t) dyn_entry.d_un.d_ptr > UINTPTR_MAX - load_bias)
                        {
                            dynamic_ptr_overflow = true;
                            break;
                        }
                        hash_addr = load_bias + (uintptr_t) dyn_entry.d_un.d_ptr;
                        break;
                    case SYSCALL_ELF_DT_STRTAB:
                        if ((uintptr_t) dyn_entry.d_un.d_ptr > UINTPTR_MAX - load_bias)
                        {
                            dynamic_ptr_overflow = true;
                            break;
                        }
                        strtab_addr = load_bias + (uintptr_t) dyn_entry.d_un.d_ptr;
                        break;
                    case SYSCALL_ELF_DT_STRSZ:
                        module->strsz = (size_t) dyn_entry.d_un.d_val;
                        break;
                    case SYSCALL_ELF_DT_SYMTAB:
                        if ((uintptr_t) dyn_entry.d_un.d_ptr > UINTPTR_MAX - load_bias)
                        {
                            dynamic_ptr_overflow = true;
                            break;
                        }
                        symtab_addr = load_bias + (uintptr_t) dyn_entry.d_un.d_ptr;
                        break;
                    case SYSCALL_ELF_DT_SYMENT:
                        sym_ent = (size_t) dyn_entry.d_un.d_val;
                        break;
                    case SYSCALL_ELF_DT_RELA:
                        if ((uintptr_t) dyn_entry.d_un.d_ptr > UINTPTR_MAX - load_bias)
                        {
                            dynamic_ptr_overflow = true;
                            break;
                        }
                        rela_addr = load_bias + (uintptr_t) dyn_entry.d_un.d_ptr;
                        break;
                    case SYSCALL_ELF_DT_RELASZ:
                        rela_size = (size_t) dyn_entry.d_un.d_val;
                        break;
                    case SYSCALL_ELF_DT_RELAENT:
                        rela_ent = (size_t) dyn_entry.d_un.d_val;
                        break;
                    case SYSCALL_ELF_DT_JMPREL:
                        if ((uintptr_t) dyn_entry.d_un.d_ptr > UINTPTR_MAX - load_bias)
                        {
                            dynamic_ptr_overflow = true;
                            break;
                        }
                        jmprel_addr = load_bias + (uintptr_t) dyn_entry.d_un.d_ptr;
                        break;
                    case SYSCALL_ELF_DT_PLTRELSZ:
                        plt_rela_size = (size_t) dyn_entry.d_un.d_val;
                        break;
                    case SYSCALL_ELF_DT_PLTREL:
                        plt_rel_type = (uintptr_t) dyn_entry.d_un.d_val;
                        break;
                    case SYSCALL_ELF_DT_SONAME:
                        if (dyn_entry.d_un.d_val <= UINT32_MAX)
                            soname_offset = (uint32_t) dyn_entry.d_un.d_val;
                        break;
                    default:
                        break;
                }

                if (dynamic_ptr_overflow)
                    break;
            }
            if (dynamic_ptr_overflow)
            {
                kdebug_printf("[USER] exec reject '%s': dynamic pointer overflow\n", path);
                kfree(elf_image);
                return false;
            }
        }
        else if (phdr->p_type == SYSCALL_ELF_PT_TLS)
        {
            module->tls_vaddr = (uintptr_t) phdr->p_vaddr;
            module->tls_filesz = (size_t) phdr->p_filesz;
            module->tls_memsz = (size_t) phdr->p_memsz;
            module->tls_align = (size_t) phdr->p_align;
            if (module->tls_memsz != 0U)
            {
                uintptr_t tls_align = (uintptr_t) phdr->p_align;
                if (tls_align < 16U || (tls_align & (tls_align - 1U)) != 0U)
                    tls_align = 16U;

                uintptr_t rounded_tls = Syscall_align_up_pow2((uintptr_t) module->tls_memsz, tls_align);
                if (rounded_tls == 0)
                {
                    kdebug_printf("[USER] exec reject '%s': TLS size overflow\n", path);
                    kfree(elf_image);
                    return false;
                }
                module->tls_rounded_size = (size_t) rounded_tls;
            }
        }
    }

    if (has_dynamic)
    {
        if (!strtab_addr || !symtab_addr || !hash_addr || module->strsz == 0U)
        {
            kdebug_printf("[USER] exec reject '%s': missing dynamic tables\n", path);
            kfree(elf_image);
            return false;
        }
        if (sym_ent != sizeof(syscall_elf64_sym_t))
        {
            kdebug_printf("[USER] exec reject '%s': unsupported symbol entry size\n", path);
            kfree(elf_image);
            return false;
        }
        if (rela_ent != sizeof(syscall_elf64_rela_t))
        {
            kdebug_printf("[USER] exec reject '%s': unsupported relocation entry size\n", path);
            kfree(elf_image);
            return false;
        }
        if (plt_rel_type != SYSCALL_ELF_DT_RELA)
        {
            kdebug_printf("[USER] exec reject '%s': unsupported PLT relocation format\n", path);
            kfree(elf_image);
            return false;
        }

        if (hash_addr < load_bias || strtab_addr < load_bias || symtab_addr < load_bias)
        {
            kdebug_printf("[USER] exec reject '%s': dynamic pointer underflow\n", path);
            kfree(elf_image);
            return false;
        }

        const uint32_t* hash = NULL;
        if (!Syscall_exec_vaddr_to_file_ptr(elf_image,
                                            elf_size,
                                            ehdr,
                                            (uint64_t) (hash_addr - load_bias),
                                            sizeof(uint32_t) * 2U,
                                            (const void**) &hash))
        {
            kdebug_printf("[USER] exec reject '%s': invalid DT_HASH\n", path);
            kfree(elf_image);
            return false;
        }
        uint32_t bucket_count = hash[0];
        uint32_t chain_count = hash[1];
        size_t hash_bytes = (size_t) (2U + bucket_count + chain_count) * sizeof(uint32_t);
        if (chain_count == 0U ||
            !Syscall_exec_vaddr_to_file_ptr(elf_image,
                                            elf_size,
                                            ehdr,
                                            (uint64_t) (hash_addr - load_bias),
                                            hash_bytes,
                                            (const void**) &hash))
        {
            kdebug_printf("[USER] exec reject '%s': invalid DT_HASH bounds\n", path);
            kfree(elf_image);
            return false;
        }

        const char* strtab_file = NULL;
        if (!Syscall_exec_vaddr_to_file_ptr(elf_image,
                                            elf_size,
                                            ehdr,
                                            (uint64_t) (strtab_addr - load_bias),
                                            module->strsz,
                                            (const void**) &strtab_file))
        {
            kdebug_printf("[USER] exec reject '%s': invalid DT_STRTAB bounds\n", path);
            kfree(elf_image);
            return false;
        }

        size_t sym_bytes = (size_t) chain_count * sizeof(syscall_elf64_sym_t);
        const syscall_elf64_sym_t* symtab_file = NULL;
        if (!Syscall_exec_vaddr_to_file_ptr(elf_image,
                                            elf_size,
                                            ehdr,
                                            (uint64_t) (symtab_addr - load_bias),
                                            sym_bytes,
                                            (const void**) &symtab_file))
        {
            kdebug_printf("[USER] exec reject '%s': invalid DT_SYMTAB bounds\n", path);
            kfree(elf_image);
            return false;
        }

        module->strtab = strtab_file;
        module->symtab = symtab_file;
        module->sym_count = (size_t) chain_count;

        if (rela_size != 0U)
        {
            if ((rela_size % sizeof(syscall_elf64_rela_t)) != 0U ||
                rela_addr < load_bias ||
                !Syscall_exec_vaddr_to_file_ptr(elf_image,
                                                elf_size,
                                                ehdr,
                                                (uint64_t) (rela_addr - load_bias),
                                                rela_size,
                                                (const void**) &module->rela))
            {
                kdebug_printf("[USER] exec reject '%s': invalid DT_RELA bounds\n", path);
                kfree(elf_image);
                return false;
            }
            module->rela_count = rela_size / sizeof(syscall_elf64_rela_t);
        }

        if (plt_rela_size != 0U)
        {
            if ((plt_rela_size % sizeof(syscall_elf64_rela_t)) != 0U ||
                jmprel_addr < load_bias ||
                !Syscall_exec_vaddr_to_file_ptr(elf_image,
                                                elf_size,
                                                ehdr,
                                                (uint64_t) (jmprel_addr - load_bias),
                                                plt_rela_size,
                                                (const void**) &module->plt_rela))
            {
                kdebug_printf("[USER] exec reject '%s': invalid DT_JMPREL bounds\n", path);
                kfree(elf_image);
                return false;
            }
            module->plt_rela_count = plt_rela_size / sizeof(syscall_elf64_rela_t);
        }

        if (soname_offset != UINT32_MAX)
        {
            const char* soname = Syscall_exec_dynstr_at(module, soname_offset);
            if (!soname)
            {
                kdebug_printf("[USER] exec reject '%s': invalid DT_SONAME\n", path);
                kfree(elf_image);
                return false;
            }
            size_t soname_len = strlen(soname);
            if (soname_len >= sizeof(module->soname))
                soname_len = sizeof(module->soname) - 1U;
            memcpy(module->soname, soname, soname_len);
            module->soname[soname_len] = '\0';
        }
    }
    else if (!is_main && ehdr->e_type == SYSCALL_ELF_TYPE_DYN)
    {
        kdebug_printf("[USER] exec reject '%s': missing PT_DYNAMIC\n", path);
        kfree(elf_image);
        return false;
    }

    module->image = elf_image;
    module->image_size = elf_size;
    (*io_module_count)++;
    return true;
}

static void Syscall_exec_release_modules(syscall_exec_module_t* modules, size_t module_count)
{
    if (!modules)
        return;

    for (size_t i = 0; i < module_count; i++)
    {
        if (modules[i].image)
        {
            kfree(modules[i].image);
            modules[i].image = NULL;
            modules[i].image_size = 0U;
        }
    }
}

static bool Syscall_exec_lookup_symbol_in_module(const syscall_exec_module_t* module,
                                                 const char* symbol,
                                                 uintptr_t* out_addr)
{
    if (!module || !symbol || !out_addr || !module->symtab || !module->strtab)
        return false;

    bool weak_match = false;
    uintptr_t weak_addr = 0;
    for (size_t i = 1; i < module->sym_count; i++)
    {
        const syscall_elf64_sym_t* sym = &module->symtab[i];
        if (sym->st_shndx == SYSCALL_ELF_SHN_UNDEF || sym->st_name == 0U)
            continue;
        if ((size_t) sym->st_name >= module->strsz)
            continue;

        uint8_t bind = SYSCALL_ELF64_ST_BIND(sym->st_info);
        if (bind != SYSCALL_ELF_STB_GLOBAL && bind != SYSCALL_ELF_STB_WEAK)
            continue;

        const char* name = module->strtab + sym->st_name;
        if (!Syscall_exec_has_nul_terminator(name, module->strsz - (size_t) sym->st_name))
            continue;
        if (strcmp(name, symbol) != 0)
            continue;

        uintptr_t addr = module->load_bias + (uintptr_t) sym->st_value;
        if (bind == SYSCALL_ELF_STB_GLOBAL)
        {
            *out_addr = addr;
            return true;
        }

        weak_match = true;
        weak_addr = addr;
    }

    if (weak_match)
    {
        *out_addr = weak_addr;
        return true;
    }
    return false;
}

static bool Syscall_exec_lookup_symbol_global(const syscall_exec_module_t* modules,
                                              size_t module_count,
                                              const char* symbol,
                                              uintptr_t* out_addr,
                                              const syscall_exec_module_t** out_owner)
{
    if (!modules || module_count == 0 || !symbol || !out_addr)
        return false;

    for (size_t i = 0; i < module_count; i++)
    {
        uintptr_t addr = 0;
        if (!Syscall_exec_lookup_symbol_in_module(&modules[i], symbol, &addr))
            continue;
        *out_addr = addr;
        if (out_owner)
            *out_owner = &modules[i];
        return true;
    }

    return false;
}

static bool Syscall_exec_apply_relocation_list(const syscall_exec_module_t* module,
                                               const syscall_exec_module_t* modules,
                                               size_t module_count,
                                               const syscall_elf64_rela_t* relocs,
                                               size_t reloc_count)
{
    if (!module || !modules || module_count == 0 || !relocs || reloc_count == 0)
        return true;

    for (size_t i = 0; i < reloc_count; i++)
    {
        const syscall_elf64_rela_t* rela = &relocs[i];
        uint32_t type = SYSCALL_ELF64_R_TYPE(rela->r_info);
        uint32_t sym_index = SYSCALL_ELF64_R_SYM(rela->r_info);

        uintptr_t reloc_addr = module->load_bias + (uintptr_t) rela->r_offset;
        if (!Syscall_exec_module_addr_range_valid(module, reloc_addr, sizeof(uint64_t)))
        {
            kdebug_printf("[USER] exec reject '%s': relocation address out of range\n", module->path);
            return false;
        }

        if (type == SYSCALL_ELF_R_X86_64_NONE)
            continue;

        uint64_t value = 0;
        if (type == SYSCALL_ELF_R_X86_64_RELATIVE)
        {
            value = (uint64_t) (module->load_bias + (uintptr_t) rela->r_addend);
            if (!Syscall_copy_into_user_phys(reloc_addr, &value, sizeof(value)))
                return false;
            continue;
        }

        if (!module->symtab || sym_index >= module->sym_count)
        {
            kdebug_printf("[USER] exec reject '%s': invalid relocation symbol index\n", module->path);
            return false;
        }

        const syscall_elf64_sym_t* sym = &module->symtab[sym_index];
        uintptr_t sym_addr = 0;
        const syscall_exec_module_t* sym_owner = module;
        bool resolved = false;
        const char* unresolved_name = NULL;
        bool relocation_uses_local_tls_base = (sym_index == 0U &&
                                               (type == SYSCALL_ELF_R_X86_64_DTPOFF64 ||
                                                type == SYSCALL_ELF_R_X86_64_TPOFF64));

        if (relocation_uses_local_tls_base)
        {
            if (module->tls_memsz == 0U)
            {
                kdebug_printf("[USER] exec reject '%s': TLS relocation without TLS segment\n", module->path);
                return false;
            }

            sym_owner = module;
            sym_addr = module->load_bias + module->tls_vaddr;
            resolved = true;
        }

        if (!resolved && sym->st_shndx != SYSCALL_ELF_SHN_UNDEF)
        {
            sym_addr = module->load_bias + (uintptr_t) sym->st_value;
            resolved = true;
        }
        else if (!resolved)
        {
            if (!module->strtab || (size_t) sym->st_name >= module->strsz)
            {
                kdebug_printf("[USER] exec reject '%s': invalid relocation symbol name\n", module->path);
                return false;
            }

            const char* sym_name = module->strtab + sym->st_name;
            if (!Syscall_exec_has_nul_terminator(sym_name, module->strsz - (size_t) sym->st_name))
            {
                kdebug_printf("[USER] exec reject '%s': unterminated relocation symbol name\n", module->path);
                return false;
            }
            unresolved_name = sym_name;

            if (module->tls_memsz != 0U)
            {
                uintptr_t tls_base = module->load_bias + module->tls_vaddr;
                uintptr_t tls_files_end = tls_base + module->tls_filesz;
                uintptr_t tls_block_end = tls_base + module->tls_memsz;
                uintptr_t tls_align_value = module->tls_align;
                if (tls_align_value < 16U || (tls_align_value & (tls_align_value - 1U)) != 0U)
                    tls_align_value = 16U;

                if (strcmp(sym_name, "__theos_tls_tdata_start") == 0)
                {
                    sym_addr = tls_base;
                    sym_owner = module;
                    resolved = true;
                }
                else if (strcmp(sym_name, "__theos_tls_tdata_end") == 0)
                {
                    sym_addr = tls_files_end;
                    sym_owner = module;
                    resolved = true;
                }
                else if (strcmp(sym_name, "__theos_tls_tbss_end") == 0)
                {
                    sym_addr = tls_block_end;
                    sym_owner = module;
                    resolved = true;
                }
                else if (strcmp(sym_name, "__theos_tls_align") == 0)
                {
                    sym_addr = tls_align_value;
                    sym_owner = module;
                    resolved = true;
                }
            }

            if (resolved)
                goto relocation_symbol_resolved;

            resolved = Syscall_exec_lookup_symbol_global(modules,
                                                         module_count,
                                                         sym_name,
                                                         &sym_addr,
                                                         &sym_owner);
            if (!resolved)
            {
                uint8_t bind = SYSCALL_ELF64_ST_BIND(sym->st_info);
                if (bind == SYSCALL_ELF_STB_WEAK)
                {
                    sym_addr = 0;
                    sym_owner = NULL;
                    resolved = true;
                }
            }
        }
relocation_symbol_resolved:

        if (!resolved)
        {
            if (unresolved_name)
            {
                kdebug_printf("[USER] exec reject '%s': unresolved symbol '%s' in relocation\n",
                              module->path,
                              unresolved_name);
            }
            else
            {
                kdebug_printf("[USER] exec reject '%s': unresolved relocation symbol index=%u\n",
                              module->path,
                              (unsigned int) sym_index);
            }
            return false;
        }

        switch (type)
        {
            case SYSCALL_ELF_R_X86_64_64:
            case SYSCALL_ELF_R_X86_64_GLOB_DAT:
            case SYSCALL_ELF_R_X86_64_JUMP_SLOT:
                value = (uint64_t) (sym_addr + (uintptr_t) rela->r_addend);
                break;

            case SYSCALL_ELF_R_X86_64_COPY:
            {
                size_t copy_size = (size_t) sym->st_size;
                uintptr_t source_addr = sym_addr + (uintptr_t) rela->r_addend;
                if (copy_size == 0U)
                    break;

                if (!Syscall_exec_module_addr_range_valid(module, reloc_addr, copy_size))
                {
                    kdebug_printf("[USER] exec reject '%s': COPY relocation destination out of range\n",
                                  module->path);
                    return false;
                }

                bool source_is_zero = (sym_owner == NULL || sym_addr == 0U);
                if (!source_is_zero &&
                    (!sym_owner || !Syscall_exec_module_addr_range_valid(sym_owner, source_addr, copy_size)))
                {
                    kdebug_printf("[USER] exec reject '%s': COPY relocation source out of range\n",
                                  module->path);
                    return false;
                }

                uint8_t bounce[256];
                size_t copied = 0U;
                while (copied < copy_size)
                {
                    size_t chunk = copy_size - copied;
                    if (chunk > sizeof(bounce))
                        chunk = sizeof(bounce);

                    if (source_is_zero)
                    {
                        memset(bounce, 0, chunk);
                    }
                    else if (!Syscall_copy_from_user(bounce, (const void*) (source_addr + copied), chunk))
                    {
                        return false;
                    }

                    if (!Syscall_copy_into_user_phys(reloc_addr + copied, bounce, chunk))
                        return false;

                    copied += chunk;
                }
                continue;
            }

            case SYSCALL_ELF_R_X86_64_DTPMOD64:
                value = 1U;
                break;

            case SYSCALL_ELF_R_X86_64_DTPOFF64:
            {
                if (!sym_owner || sym_owner->tls_memsz == 0U)
                {
                    kdebug_printf("[USER] exec reject '%s': DTPOFF64 without TLS owner\n", module->path);
                    return false;
                }
                int64_t dtpoff = (int64_t) (sym_addr + (uintptr_t) rela->r_addend) -
                                 (int64_t) (sym_owner->load_bias + sym_owner->tls_vaddr);
                value = (uint64_t) dtpoff;
                break;
            }

            case SYSCALL_ELF_R_X86_64_TPOFF64:
            {
                if (!sym_owner || sym_owner->tls_memsz == 0U || sym_owner->tls_rounded_size == 0U)
                {
                    kdebug_printf("[USER] exec reject '%s': TPOFF64 without TLS owner\n", module->path);
                    return false;
                }
                int64_t tpoff = (int64_t) (sym_addr + (uintptr_t) rela->r_addend) -
                                (int64_t) (sym_owner->load_bias +
                                           sym_owner->tls_vaddr +
                                           sym_owner->tls_rounded_size);
                value = (uint64_t) tpoff;
                break;
            }

            default:
                kdebug_printf("[USER] exec reject '%s': unsupported relocation type=%u\n",
                              module->path,
                              (unsigned int) type);
                return false;
        }

        if (!Syscall_copy_into_user_phys(reloc_addr, &value, sizeof(value)))
            return false;
    }

    return true;
}

static bool Syscall_exec_apply_module_relocations(const syscall_exec_module_t* module,
                                                  const syscall_exec_module_t* modules,
                                                  size_t module_count)
{
    if (!module)
        return false;

    if (!Syscall_exec_apply_relocation_list(module, modules, module_count, module->rela, module->rela_count))
        return false;
    if (!Syscall_exec_apply_relocation_list(module, modules, module_count, module->plt_rela, module->plt_rela_count))
        return false;
    return true;
}

static bool Syscall_exec_apply_module_protections(const syscall_exec_module_t* module)
{
    if (!module)
        return false;

    for (size_t i = 0; i < module->segment_count; i++)
    {
        const syscall_exec_segment_t* seg = &module->segments[i];
        if (seg->writable && seg->executable)
            return false;

        uintptr_t set_bits = seg->writable ? WRITABLE : 0;
        uintptr_t clear_bits = seg->writable ? 0 : WRITABLE;
        if (!seg->executable)
            set_bits |= NO_EXECUTE;
        else
            clear_bits |= NO_EXECUTE;

        for (uintptr_t page = seg->start; page < seg->end; page += SYSCALL_PAGE_SIZE)
        {
            if (!VMM_update_page_flags(page, set_bits, clear_bits))
                return false;
        }
    }
    return true;
}

static bool Syscall_load_elf_current(const char* path, uintptr_t* out_entry, uintptr_t* out_rsp)
{
    if (!path || !out_entry || !out_rsp)
        return false;

    if (!VFS_is_ready())
    {
        kdebug_printf("[USER] exec reject '%s': no mounted root VFS backend\n", path);
        return false;
    }

    size_t modules_bytes = sizeof(syscall_exec_module_t) * SYSCALL_EXEC_MAX_MODULES;
    syscall_exec_module_t* modules = (syscall_exec_module_t*) kmalloc(modules_bytes);
    if (!modules)
    {
        kdebug_printf("[USER] exec reject '%s': out of memory for module state\n", path);
        return false;
    }
    memset(modules, 0, modules_bytes);
    size_t module_count = 0;
    uintptr_t dyn_cursor = SYSCALL_ELF_DSO_BASE;

    if (!Syscall_exec_load_module_current(path, true, &dyn_cursor, modules, &module_count))
        goto fail;
    kdebug_puts("[USER] exec loader: main loaded\n");

    for (size_t i = 0; i < module_count; i++)
    {
        syscall_exec_module_t* module = &modules[i];
        for (size_t n = 0; n < module->needed_count; n++)
        {
            const char* needed_name = Syscall_exec_dynstr_at(module, module->needed_offsets[n]);
            if (!needed_name)
            {
                kdebug_printf("[USER] exec reject '%s': invalid DT_NEEDED string\n", module->path);
                goto fail;
            }

            char needed_path[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_exec_build_needed_path(needed_name, needed_path, sizeof(needed_path)))
            {
                kdebug_printf("[USER] exec reject '%s': DT_NEEDED path overflow\n", module->path);
                goto fail;
            }

            bool already_loaded = false;
            for (size_t m = 0; m < module_count; m++)
            {
                if (strcmp(modules[m].path, needed_path) == 0 ||
                    (modules[m].soname[0] != '\0' && strcmp(modules[m].soname, needed_name) == 0))
                {
                    already_loaded = true;
                    break;
                }
            }
            if (already_loaded)
                continue;

            if (!Syscall_exec_load_module_current(needed_path, false, &dyn_cursor, modules, &module_count))
            {
                kdebug_printf("[USER] exec reject '%s': failed to load dependency '%s'\n",
                              module->path,
                              needed_name);
                goto fail;
            }
        }
    }
    kdebug_puts("[USER] exec loader: dependencies loaded\n");

    for (size_t i = 0; i < module_count; i++)
    {
        if (!Syscall_exec_apply_module_relocations(&modules[i], modules, module_count))
            goto fail;
    }
    kdebug_puts("[USER] exec loader: relocations applied\n");

    for (size_t i = 0; i < module_count; i++)
    {
        if (!Syscall_exec_apply_module_protections(&modules[i]))
            goto fail;
    }
    kdebug_puts("[USER] exec loader: protections applied\n");

    uintptr_t stack_bottom = SYSCALL_ELF_STACK_TOP - SYSCALL_ELF_STACK_SIZE;
    if (!Syscall_map_user_range_current(stack_bottom, SYSCALL_ELF_STACK_SIZE))
    {
        kdebug_printf("[USER] exec reject '%s': failed to map user stack (%llu bytes)\n",
                      path,
                      (unsigned long long) SYSCALL_ELF_STACK_SIZE);
        goto fail;
    }

    *out_entry = modules[0].entry;
    *out_rsp = SYSCALL_ELF_STACK_TOP & ~(uintptr_t) 0xFULL;
    kdebug_puts("[USER] exec loader: stack mapped\n");
    Syscall_exec_release_modules(modules, module_count);
    kfree(modules);
    return true;

fail:
    Syscall_exec_release_modules(modules, module_count);
    kfree(modules);
    return false;
}

static bool Syscall_execve_build_address_space(const char* path,
                                               uintptr_t* out_cr3_phys,
                                               uintptr_t* out_entry,
                                               uintptr_t* out_rsp)
{
    if (!path || !out_cr3_phys || !out_entry || !out_rsp)
        return false;

    uintptr_t current_cr3 = Syscall_read_cr3_phys();
    uintptr_t new_cr3 = 0;
    if (!Syscall_create_kernel_mirrored_address_space(current_cr3, &new_cr3))
        return false;

    Syscall_write_cr3_phys(new_cr3);
    bool ok = Syscall_load_elf_current(path, out_entry, out_rsp);
    Syscall_write_cr3_phys(current_cr3);

    if (!ok)
    {
        Syscall_free_address_space(new_cr3);
        return false;
    }

    *out_cr3_phys = new_cr3;
    return true;
}

bool Syscall_prepare_initial_user_process(const char* path,
                                          uintptr_t* out_cr3_phys,
                                          uintptr_t* out_entry,
                                          uintptr_t* out_rsp)
{
    if (!path || !out_cr3_phys || !out_entry || !out_rsp)
        return false;

    uintptr_t new_cr3 = 0;
    uintptr_t new_entry = 0;
    uintptr_t new_rsp = 0;
    if (!Syscall_execve_build_address_space(path, &new_cr3, &new_entry, &new_rsp))
        return false;

    if (!Syscall_exec_install_initial_stack(new_cr3, &new_rsp, path, NULL, 0, NULL, 0))
    {
        Syscall_free_address_space(new_cr3);
        return false;
    }
    Syscall_bootstrap_domain = Syscall_process_domain_from_exec_path(path);
    *out_cr3_phys = new_cr3;
    *out_entry = new_entry;
    *out_rsp = new_rsp;
    return true;
}

static void Syscall_fd_cleanup_owner(uint32_t owner_pid)
{
    if (!Syscall_state.fd_lock_ready || owner_pid == 0)
        return;

    for (uint32_t i = 0; i < SYSCALL_MAX_OPEN_FILES; i++)
    {
        bool should_flush = false;
        size_t flush_size = 0;
        const uint8_t* flush_data = NULL;
        char flush_path[SYSCALL_USER_CSTR_MAX];
        uint32_t entry_type = SYSCALL_FD_TYPE_NONE;
        uint32_t drm_file_id = 0;
        uint32_t dmabuf_id = 0;

        spin_lock(&Syscall_state.fd_lock);
        syscall_file_desc_t* entry = &Syscall_state.fds[i];
        if (!entry->used || entry->owner_pid != owner_pid)
        {
            spin_unlock(&Syscall_state.fd_lock);
            continue;
        }

        entry_type = entry->type;
        if (entry_type == SYSCALL_FD_TYPE_DRM_CARD)
        {
            drm_file_id = entry->drm_file_id;
            memset(entry, 0, sizeof(*entry));
            spin_unlock(&Syscall_state.fd_lock);
            DRM_close_file(drm_file_id);
            continue;
        }

        if (entry_type == SYSCALL_FD_TYPE_DMABUF)
        {
            dmabuf_id = entry->drm_dmabuf_id;
            memset(entry, 0, sizeof(*entry));
            spin_unlock(&Syscall_state.fd_lock);
            DRM_dmabuf_unref_fd(dmabuf_id);
            continue;
        }

        if (entry_type == SYSCALL_FD_TYPE_AUDIO_DSP)
        {
            memset(entry, 0, sizeof(*entry));
            spin_unlock(&Syscall_state.fd_lock);
            HDA_dsp_close(owner_pid);
            continue;
        }

        if (entry_type == SYSCALL_FD_TYPE_NET_UDP_SOCKET)
        {
            uint32_t socket_id = entry->net_socket_id;
            memset(entry, 0, sizeof(*entry));
            spin_unlock(&Syscall_state.fd_lock);
            (void) NET_socket_close_udp(owner_pid, socket_id);
            continue;
        }

        if (entry_type == SYSCALL_FD_TYPE_NET_TCP_SOCKET)
        {
            uint32_t socket_id = entry->net_socket_id;
            memset(entry, 0, sizeof(*entry));
            spin_unlock(&Syscall_state.fd_lock);
            (void) NET_tcp_close(owner_pid, socket_id);
            continue;
        }

        if (entry_type == SYSCALL_FD_TYPE_NET_UNIX_SOCKET)
        {
            uint32_t socket_id = entry->net_socket_id;
            memset(entry, 0, sizeof(*entry));
            spin_unlock(&Syscall_state.fd_lock);
            (void) NET_unix_close(owner_pid, socket_id);
            continue;
        }

        if (entry_type != SYSCALL_FD_TYPE_REGULAR)
        {
            memset(entry, 0, sizeof(*entry));
            spin_unlock(&Syscall_state.fd_lock);
            continue;
        }

        entry->io_busy = true;
        if (entry->can_write && entry->dirty)
        {
            should_flush = true;
            flush_size = entry->size;
            flush_data = entry->data;
            memcpy(flush_path, entry->path, sizeof(flush_path));
        }
        spin_unlock(&Syscall_state.fd_lock);

        if (should_flush)
        {
            size_t block_size = VFS_block_size();
            if (VFS_is_ready() && block_size != 0U && flush_size <= block_size)
                (void) VFS_write_file(flush_path, flush_data, flush_size);
        }

        spin_lock(&Syscall_state.fd_lock);
        entry = &Syscall_state.fds[i];
        if (entry->used && entry->owner_pid == owner_pid)
        {
            if (entry->data)
                kfree(entry->data);
            memset(entry, 0, sizeof(*entry));
        }
        spin_unlock(&Syscall_state.fd_lock);
    }
}

static int32_t Syscall_proc_alloc_locked(void)
{
    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        if (!Syscall_state.procs[i].used)
            return (int32_t) i;
    }

    return -1;
}

static int32_t Syscall_proc_get_current_slot_locked(uint32_t cpu_index)
{
    if (cpu_index >= 256)
        return -1;

    uint32_t slot = Syscall_state.cpu_current_proc[cpu_index];
    if (slot >= SYSCALL_MAX_PROCS)
        return -1;
    if (!Syscall_state.procs[slot].used)
        return -1;

    return (int32_t) slot;
}

static int32_t Syscall_proc_ensure_current_locked(uint32_t cpu_index, const syscall_frame_t* frame)
{
    int32_t slot = Syscall_proc_get_current_slot_locked(cpu_index);
    if (slot >= 0)
        return slot;

    int32_t new_slot = Syscall_proc_alloc_locked();
    if (new_slot < 0 || !frame)
        return -1;

    syscall_process_t* proc = &Syscall_state.procs[new_slot];
    memset(proc, 0, sizeof(*proc));
    Syscall_idle_claimed_slots[(uint32_t) new_slot] = 0U;
    proc->used = true;
    proc->exiting = false;
    proc->terminated_by_signal = false;
    proc->is_thread = false;
    proc->exit_status = 0;
    proc->thread_exit_value = 0;
    proc->term_signal = 0;
    uintptr_t current_cr3 = Syscall_read_cr3_phys();
    uintptr_t kernel_cr3 = VMM_get_kernel_cr3_phys();
    uintptr_t current_fs_base = Syscall_read_fs_base();
    proc->owns_cr3 = (current_cr3 != 0 && current_cr3 != kernel_cr3);
    proc->pid = Syscall_state.next_pid++;
    proc->ppid = 0;
    proc->owner_pid = proc->pid;
    proc->console_sid = proc->pid;
    proc->domain = (proc->pid == 1U) ? Syscall_bootstrap_domain : SYS_PROC_DOMAIN_USERLAND;
    proc->cr3_phys = current_cr3;
    proc->fs_base = current_fs_base;
    proc->rax = 0;
    proc->rcx = 0;
    proc->rdx = frame->rdx;
    proc->rsi = frame->rsi;
    proc->rdi = frame->rdi;
    proc->r8 = frame->r8;
    proc->r9 = frame->r9;
    proc->r10 = frame->r10;
    proc->r11 = 0;
    proc->r15 = frame->r15;
    proc->r14 = frame->r14;
    proc->r13 = frame->r13;
    proc->r12 = frame->r12;
    proc->rbp = frame->rbp;
    proc->rbx = frame->rbx;
    proc->rip = frame->rip;
    proc->rflags = frame->rflags | SYSCALL_RFLAGS_IF;
    proc->rsp = frame->rsp;
    proc->pending_rax = 0;
    proc->last_cpu = cpu_index;

    Syscall_state.cpu_current_proc[cpu_index] = (uint32_t) new_slot;
    return new_slot;
}

static uint32_t Syscall_proc_current_pid(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!Syscall_state.proc_lock_ready)
        return 0;

    uint64_t flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
    uint32_t pid = (slot >= 0) ? Syscall_state.procs[slot].owner_pid : 0;
    spin_unlock_irqrestore(&Syscall_state.proc_lock, flags);
    return pid;
}

static uint32_t Syscall_proc_current_console_sid(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!Syscall_state.proc_lock_ready)
        return 0;

    uint64_t flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
    uint32_t sid = (slot >= 0) ? Syscall_state.procs[slot].console_sid : 0;
    spin_unlock_irqrestore(&Syscall_state.proc_lock, flags);
    return sid;
}

static bool Syscall_console_sid_has_other_live_locked(uint32_t console_sid, int32_t exclude_slot)
{
    if (console_sid == 0)
        return false;

    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        if ((int32_t) i == exclude_slot)
            continue;
        if (!Syscall_state.procs[i].used)
            continue;
        if (Syscall_state.procs[i].console_sid != console_sid)
            continue;
        return true;
    }

    return false;
}

static int32_t Syscall_console_route_find_locked(uint32_t console_sid)
{
    if (console_sid == 0)
        return -1;

    for (uint32_t i = 0; i < SYSCALL_MAX_CONSOLE_ROUTES; i++)
    {
        if (!Syscall_state.console_routes[i].used)
            continue;
        if (Syscall_state.console_routes[i].console_sid == console_sid)
            return (int32_t) i;
    }

    return -1;
}

static int32_t Syscall_console_route_alloc_locked(uint32_t owner_pid, uint32_t console_sid)
{
    if (owner_pid == 0U || console_sid == 0U)
        return -1;

    for (uint32_t i = 0; i < SYSCALL_MAX_CONSOLE_ROUTES; i++)
    {
        if (Syscall_state.console_routes[i].used)
            continue;
        memset(&Syscall_state.console_routes[i], 0, sizeof(Syscall_state.console_routes[i]));
        Syscall_state.console_routes[i].used = true;
        Syscall_state.console_routes[i].owner_pid = owner_pid;
        Syscall_state.console_routes[i].console_sid = console_sid;
        return (int32_t) i;
    }

    return -1;
}

static void Syscall_console_route_clear_sid(uint32_t console_sid)
{
    if (!Syscall_state.console_lock_ready || console_sid == 0)
        return;

    uint64_t flags = spin_lock_irqsave(&Syscall_state.console_lock);
    int32_t slot = Syscall_console_route_find_locked(console_sid);
    if (slot >= 0)
        memset(&Syscall_state.console_routes[(uint32_t) slot], 0, sizeof(syscall_console_route_t));
    spin_unlock_irqrestore(&Syscall_state.console_lock, flags);
}

static bool Syscall_console_route_exists_for_sid(uint32_t console_sid)
{
    if (!Syscall_state.console_lock_ready || console_sid == 0U)
        return false;

    bool exists = false;
    uint64_t flags = spin_lock_irqsave(&Syscall_state.console_lock);
    exists = (Syscall_console_route_find_locked(console_sid) >= 0);
    spin_unlock_irqrestore(&Syscall_state.console_lock, flags);
    return exists;
}

static bool Syscall_console_sid_manageable_by(uint32_t caller_pid, uint32_t console_sid)
{
    if (!Syscall_state.proc_lock_ready || caller_pid == 0U || console_sid == 0U)
        return false;

    bool manageable = false;
    uint64_t flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        const syscall_process_t* proc = &Syscall_state.procs[i];
        if (!proc->used)
            continue;
        if (proc->console_sid != console_sid)
            continue;
        if (proc->pid == caller_pid || proc->ppid == caller_pid || proc->owner_pid == caller_pid)
            manageable = true;
        break;
    }
    spin_unlock_irqrestore(&Syscall_state.proc_lock, flags);
    return manageable;
}

static bool Syscall_console_sid_assign_process(uint32_t caller_pid, uint32_t console_sid)
{
    if (!Syscall_state.proc_lock_ready || caller_pid == 0U || console_sid == 0U)
        return false;

    bool assigned = false;
    uint64_t flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        syscall_process_t* proc = &Syscall_state.procs[i];
        if (!proc->used)
            continue;
        if (proc->pid != console_sid)
            continue;
        if (proc->pid == caller_pid || proc->ppid == caller_pid || proc->owner_pid == caller_pid)
        {
            proc->console_sid = console_sid;
            assigned = true;
        }
        break;
    }
    spin_unlock_irqrestore(&Syscall_state.proc_lock, flags);
    return assigned;
}

static void Syscall_console_route_push_locked(syscall_console_route_t* route, const char* data, size_t len)
{
    if (!route || !data || len == 0U || (route->flags & SYS_CONSOLE_ROUTE_FLAG_CAPTURE) == 0U)
        return;

    for (size_t i = 0; i < len; i++)
    {
        route->buffer[route->head] = data[i];
        route->head = (route->head + 1U) % SYSCALL_CONSOLE_CAPTURE_SIZE;
        if (route->count == SYSCALL_CONSOLE_CAPTURE_SIZE)
            route->tail = (route->tail + 1U) % SYSCALL_CONSOLE_CAPTURE_SIZE;
        else
            route->count++;
    }
}

static size_t Syscall_console_route_pop_locked(syscall_console_route_t* route, char* out, size_t out_size)
{
    if (!route || !out || out_size == 0U || route->count == 0U)
        return 0U;

    size_t copied = 0U;
    while (copied < out_size && route->count > 0U)
    {
        out[copied++] = route->buffer[route->tail];
        route->tail = (route->tail + 1U) % SYSCALL_CONSOLE_CAPTURE_SIZE;
        route->count--;
    }

    if (route->count == 0U)
    {
        route->head = 0U;
        route->tail = 0U;
    }

    return copied;
}

static void Syscall_console_route_input_push_locked(syscall_console_route_t* route, const char* data, size_t len)
{
    if (!route || !data || len == 0U || (route->flags & SYS_CONSOLE_ROUTE_FLAG_PTY_INPUT) == 0U)
        return;

    for (size_t i = 0; i < len; i++)
    {
        route->in_buffer[route->in_head] = data[i];
        route->in_head = (route->in_head + 1U) % SYSCALL_CONSOLE_PTY_INPUT_SIZE;
        if (route->in_count == SYSCALL_CONSOLE_PTY_INPUT_SIZE)
            route->in_tail = (route->in_tail + 1U) % SYSCALL_CONSOLE_PTY_INPUT_SIZE;
        else
            route->in_count++;
    }
}

static size_t Syscall_console_route_input_pop_locked(syscall_console_route_t* route, char* out, size_t out_size)
{
    if (!route || !out || out_size == 0U || route->in_count == 0U)
        return 0U;

    size_t copied = 0U;
    while (copied < out_size && route->in_count > 0U)
    {
        out[copied++] = route->in_buffer[route->in_tail];
        route->in_tail = (route->in_tail + 1U) % SYSCALL_CONSOLE_PTY_INPUT_SIZE;
        route->in_count--;
    }

    if (route->in_count == 0U)
    {
        route->in_head = 0U;
        route->in_tail = 0U;
    }

    return copied;
}

static bool Syscall_proc_is_on_other_cpu_locked(uint32_t slot, uint32_t cpu_index)
{
    uint32_t owner = 0U;
    if (slot < SYSCALL_MAX_PROCS)
    {
        syscall_process_t* cand = &Syscall_state.procs[slot];
        if (cand->used)
            owner = cand->owner_pid;
    }

    for (uint32_t i = 0; i < 256; i++)
    {
        if (i == cpu_index)
            continue;

        uint32_t running_slot = Syscall_state.cpu_current_proc[i];
        if (running_slot >= SYSCALL_MAX_PROCS)
            continue;
        if (running_slot == slot)
            return true;
        if (owner == 0U)
            continue;

        syscall_process_t* running = &Syscall_state.procs[running_slot];
        if (running->used && !running->exiting && running->owner_pid == owner)
            return true;
    }
    return false;
}

static int32_t Syscall_proc_pick_next_locked(int32_t current_slot, uint32_t cpu_index)
{
    if (current_slot >= 0)
    {
        for (uint32_t step = 1; step < SYSCALL_MAX_PROCS; step++)
        {
            uint32_t idx = ((uint32_t) current_slot + step) % SYSCALL_MAX_PROCS;
            syscall_process_t* p = &Syscall_state.procs[idx];
            if (p->used && p->cr3_phys != 0 &&
                !Syscall_proc_is_on_other_cpu_locked(idx, cpu_index))
                return (int32_t) idx;
        }

        syscall_process_t* cur = &Syscall_state.procs[(uint32_t) current_slot];
        if (cur->used && cur->cr3_phys != 0)
            return current_slot;
    }

    for (uint32_t idx = 0; idx < SYSCALL_MAX_PROCS; idx++)
    {
        syscall_process_t* p = &Syscall_state.procs[idx];
        if (p->used && p->cr3_phys != 0 &&
            !Syscall_proc_is_on_other_cpu_locked(idx, cpu_index))
            return (int32_t) idx;
    }

    return -1;
}

static int32_t Syscall_proc_pick_next_same_owner_locked(int32_t current_slot, uint32_t cpu_index)
{
    if (current_slot < 0 || (uint32_t) current_slot >= SYSCALL_MAX_PROCS)
        return Syscall_proc_pick_next_locked(current_slot, cpu_index);

    const syscall_process_t* cur = &Syscall_state.procs[(uint32_t) current_slot];
    if (!cur->used || cur->owner_pid == 0U)
        return Syscall_proc_pick_next_locked(current_slot, cpu_index);

    const uint32_t owner = cur->owner_pid;
    for (uint32_t step = 1; step < SYSCALL_MAX_PROCS; step++)
    {
        uint32_t idx = ((uint32_t) current_slot + step) % SYSCALL_MAX_PROCS;
        syscall_process_t* p = &Syscall_state.procs[idx];
        if (p->used && !p->exiting && p->cr3_phys != 0 && p->owner_pid == owner &&
            !Syscall_proc_is_on_other_cpu_locked(idx, cpu_index))
            return (int32_t) idx;
    }

    if (cur->used && cur->cr3_phys != 0)
        return current_slot;

    return Syscall_proc_pick_next_locked(current_slot, cpu_index);
}

static bool Syscall_proc_has_other_owner_peer_locked(int32_t current_slot)
{
    if (current_slot < 0 || (uint32_t) current_slot >= SYSCALL_MAX_PROCS)
        return false;

    const syscall_process_t* current = &Syscall_state.procs[(uint32_t) current_slot];
    if (!current->used || current->owner_pid == 0U)
        return false;

    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        if ((int32_t) i == current_slot)
            continue;
        const syscall_process_t* p = &Syscall_state.procs[i];
        if (!p->used || p->exiting || p->cr3_phys == 0)
            continue;
        if (p->owner_pid == current->owner_pid)
            return true;
    }
    return false;
}

static void Syscall_proc_save_from_interrupt(syscall_process_t* proc,
                                             const interrupt_frame_t* frame,
                                             uintptr_t cr3_phys,
                                             uintptr_t fs_base)
{
    if (!proc || !frame)
        return;

    proc->rax = frame->rax;
    proc->rcx = frame->rcx;
    proc->rdx = frame->rdx;
    proc->rsi = frame->rsi;
    proc->rdi = frame->rdi;
    proc->r8 = frame->r8;
    proc->r9 = frame->r9;
    proc->r10 = frame->r10;
    proc->r11 = frame->r11;
    proc->r15 = frame->r15;
    proc->r14 = frame->r14;
    proc->r13 = frame->r13;
    proc->r12 = frame->r12;
    proc->rbp = frame->rbp;
    proc->rbx = frame->rbx;
    proc->rip = frame->rip;
    proc->rflags = frame->rflags | SYSCALL_RFLAGS_IF;
    proc->rsp = frame->rsp;
    proc->pending_rax = frame->rax;
    proc->cr3_phys = cr3_phys;
    proc->fs_base = fs_base;
}

static void Syscall_proc_load_to_interrupt(const syscall_process_t* proc, interrupt_frame_t* frame)
{
    if (!proc || !frame)
        return;

    frame->rax = proc->rax;
    frame->rcx = proc->rcx;
    frame->rdx = proc->rdx;
    frame->rsi = proc->rsi;
    frame->rdi = proc->rdi;
    frame->r8 = proc->r8;
    frame->r9 = proc->r9;
    frame->r10 = proc->r10;
    frame->r11 = proc->r11;
    frame->r15 = proc->r15;
    frame->r14 = proc->r14;
    frame->r13 = proc->r13;
    frame->r12 = proc->r12;
    frame->rbp = proc->rbp;
    frame->rbx = proc->rbx;
    frame->rip = proc->rip;
    frame->rflags = proc->rflags | SYSCALL_RFLAGS_IF;
    frame->rsp = proc->rsp;
}

static bool Syscall_resolve_cow_fault(uint32_t cpu_index, uintptr_t fault_addr, uint64_t err_code)
{
    if ((err_code & (SYSCALL_PAGE_FAULT_PRESENT | SYSCALL_PAGE_FAULT_WRITE)) !=
        (SYSCALL_PAGE_FAULT_PRESENT | SYSCALL_PAGE_FAULT_WRITE))
        return false;
    if (!Syscall_state.proc_lock_ready || !Syscall_state.vm_lock_ready)
        return false;

    uintptr_t page = fault_addr & FRAME;
    if (!Syscall_user_range_in_bounds(page, 1))
        return false;

    uintptr_t current_cr3 = Syscall_read_cr3_phys();
    uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    int32_t slot = Syscall_proc_get_current_slot_locked(cpu_index);
    if (slot < 0)
    {
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        return false;
    }

    syscall_process_t* proc = &Syscall_state.procs[(uint32_t) slot];
    uintptr_t proc_cr3 = proc->cr3_phys;
    spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
    if (proc_cr3 == 0 || proc_cr3 != current_cr3)
        return false;

    spin_lock(&Syscall_state.vm_lock);
    uint64_t* pte = Syscall_get_user_pte_ptr(proc_cr3, page);
    if (!pte)
    {
        spin_unlock(&Syscall_state.vm_lock);
        return false;
    }

    uintptr_t entry = *pte;
    if ((entry & (PRESENT | USER_MODE)) != (PRESENT | USER_MODE))
    {
        spin_unlock(&Syscall_state.vm_lock);
        return false;
    }

    /*
     * SMP race: another CPU sharing the same CR3 may have already resolved
     * this COW fault and cleared SYSCALL_PTE_COW+set WRITABLE, while this CPU
     * still faults on a stale local TLB entry.
     */
    if ((entry & SYSCALL_PTE_COW) == 0)
    {
        bool writable = (entry & WRITABLE) != 0;
        spin_unlock(&Syscall_state.vm_lock);
        if (!writable)
            return false;
        Syscall_write_cr3_phys(proc_cr3);
        return true;
    }

    uintptr_t old_phys = entry & FRAME;
    if (old_phys == 0)
    {
        spin_unlock(&Syscall_state.vm_lock);
        return false;
    }

    uint32_t refs = Syscall_cow_ref_get(old_phys);
    if (refs <= 1U)
    {
        bool dummy_zero = false;
        (void) Syscall_cow_ref_sub(old_phys, &dummy_zero);
        entry &= ~SYSCALL_PTE_COW;
        entry |= WRITABLE;
        *pte = entry;
        spin_unlock(&Syscall_state.vm_lock);
        Syscall_write_cr3_phys(proc_cr3);
        return true;
    }

    uintptr_t new_phys = (uintptr_t) PMM_alloc_page();
    if (new_phys == 0)
    {
        spin_unlock(&Syscall_state.vm_lock);
        return false;
    }

    memcpy((void*) P2V(new_phys), (const void*) P2V(old_phys), SYSCALL_PAGE_SIZE);
    entry &= ~FRAME;
    entry |= new_phys;
    entry &= ~SYSCALL_PTE_COW;
    entry |= WRITABLE;
    *pte = entry;
    spin_unlock(&Syscall_state.vm_lock);

    bool ref_zero = false;
    if (Syscall_cow_ref_sub(old_phys, &ref_zero) && ref_zero)
        PMM_dealloc_page((void*) old_phys);

    Syscall_write_cr3_phys(proc_cr3);
    return true;
}

static int32_t Syscall_user_exception_signal_num(uint64_t int_no)
{
    switch (int_no)
    {
        case 0:
        case 16:
        case 19:
            return SYS_SIGFPE;
        case 1:
        case 3:
            return SYS_SIGTRAP;
        case 6:
            return SYS_SIGILL;
        case 17:
            return SYS_SIGBUS;
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
            return SYS_SIGSEGV;
        default:
            return SYS_SIGSYS;
    }
}

static bool Syscall_signal_is_valid(int32_t signal)
{
    return (signal >= SYS_SIGNAL_MIN && signal <= SYS_SIGNAL_MAX);
}

static const char* Syscall_signal_name(int32_t signal)
{
    switch (signal)
    {
        case SYS_SIGHUP:
            return "SIGHUP";
        case SYS_SIGINT:
            return "SIGINT";
        case SYS_SIGQUIT:
            return "SIGQUIT";
        case SYS_SIGILL:
            return "SIGILL";
        case SYS_SIGTRAP:
            return "SIGTRAP";
        case SYS_SIGABRT:
            return "SIGABRT";
        case SYS_SIGEMT:
            return "SIGEMT";
        case SYS_SIGFPE:
            return "SIGFPE";
        case SYS_SIGKILL:
            return "SIGKILL";
        case SYS_SIGBUS:
            return "SIGBUS";
        case SYS_SIGSEGV:
            return "SIGSEGV";
        case SYS_SIGSYS:
            return "SIGSYS";
        case SYS_SIGPIPE:
            return "SIGPIPE";
        case SYS_SIGALRM:
            return "SIGALRM";
        case SYS_SIGTERM:
            return "SIGTERM";
        case SYS_SIGUSR1:
            return "SIGUSR1";
        case SYS_SIGUSR2:
            return "SIGUSR2";
        case SYS_SIGCHLD:
            return "SIGCHLD";
        case SYS_SIGPWR:
            return "SIGPWR";
        case SYS_SIGWINCH:
            return "SIGWINCH";
        case SYS_SIGURG:
            return "SIGURG";
        case SYS_SIGPOLL:
            return "SIGPOLL";
        case SYS_SIGSTOP:
            return "SIGSTOP";
        case SYS_SIGTSTP:
            return "SIGTSTP";
        case SYS_SIGCONT:
            return "SIGCONT";
        case SYS_SIGTTIN:
            return "SIGTTIN";
        case SYS_SIGTTOU:
            return "SIGTTOU";
        case SYS_SIGVTALRM:
            return "SIGVTALRM";
        case SYS_SIGPROF:
            return "SIGPROF";
        case SYS_SIGXCPU:
            return "SIGXCPU";
        case SYS_SIGXFSZ:
            return "SIGXFSZ";
        case SYS_SIGWAITING:
            return "SIGWAITING";
        case SYS_SIGLWP:
            return "SIGLWP";
        case SYS_SIGAIO:
            return "SIGAIO";
        default:
            return "SIGUNKNOWN";
    }
}

static char Syscall_signal_default_action(int32_t signal)
{
    switch (signal)
    {
        case SYS_SIGHUP:
        case SYS_SIGINT:
        case SYS_SIGKILL:
        case SYS_SIGPIPE:
        case SYS_SIGALRM:
        case SYS_SIGTERM:
        case SYS_SIGUSR1:
        case SYS_SIGUSR2:
        case SYS_SIGVTALRM:
        case SYS_SIGPROF:
            return 'E';

        case SYS_SIGQUIT:
        case SYS_SIGILL:
        case SYS_SIGTRAP:
        case SYS_SIGABRT:
        case SYS_SIGEMT:
        case SYS_SIGFPE:
        case SYS_SIGBUS:
        case SYS_SIGSEGV:
        case SYS_SIGSYS:
        case SYS_SIGXCPU:
        case SYS_SIGXFSZ:
            return 'C';

        case SYS_SIGCHLD:
        case SYS_SIGPWR:
        case SYS_SIGWINCH:
        case SYS_SIGURG:
        case SYS_SIGPOLL:
        case SYS_SIGCONT:
        case SYS_SIGWAITING:
        case SYS_SIGLWP:
        case SYS_SIGAIO:
            return 'I';

        case SYS_SIGSTOP:
        case SYS_SIGTSTP:
        case SYS_SIGTTIN:
        case SYS_SIGTTOU:
            return 'S';

        default:
            return '?';
    }
}

static bool Syscall_signal_terminate_owner_locked(uint32_t owner_pid, int32_t signal)
{
    if (owner_pid == 0 || !Syscall_signal_is_valid(signal))
        return false;

    bool terminated = false;
    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        syscall_process_t* proc = &Syscall_state.procs[i];
        if (!proc->used || proc->owner_pid != owner_pid)
            continue;

        proc->exiting = true;
        proc->terminated_by_signal = true;
        proc->term_signal = signal;
        proc->exit_status = 128 + signal;
        proc->thread_exit_value = 0;
        terminated = true;
    }

    if (!terminated)
        return false;

    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t running_slot = Syscall_state.cpu_current_proc[i];
        if (running_slot >= SYSCALL_MAX_PROCS)
            continue;
        if (!Syscall_state.procs[running_slot].used)
            continue;
        if (Syscall_state.procs[running_slot].owner_pid != owner_pid)
            continue;
        __atomic_store_n(&Syscall_state.cpu_need_resched[i], 1, __ATOMIC_RELEASE);
    }

    return true;
}

static void Syscall_exit_event_push_locked(uint32_t ppid, uint32_t pid, int64_t status, int32_t signal)
{
    if (ppid == 0 || pid == 0)
        return;

    int32_t slot = -1;
    for (uint32_t i = 0; i < SYSCALL_MAX_EXIT_EVENTS; i++)
    {
        if (Syscall_state.exit_events[i].used &&
            Syscall_state.exit_events[i].ppid == ppid &&
            Syscall_state.exit_events[i].pid == pid)
        {
            slot = (int32_t) i;
            break;
        }

        if (!Syscall_state.exit_events[i].used && slot < 0)
            slot = (int32_t) i;
    }

    if (slot < 0)
        slot = 0;

    syscall_exit_event_t* event = &Syscall_state.exit_events[(uint32_t) slot];
    event->used = true;
    event->ppid = ppid;
    event->pid = pid;
    event->status = status;
    event->signal = signal;
}

static void Syscall_thread_exit_event_push_locked(uint32_t owner_pid, uint32_t tid, uint64_t value)
{
    if (owner_pid == 0 || tid == 0)
        return;

    int32_t slot = -1;
    for (uint32_t i = 0; i < SYSCALL_MAX_THREAD_EXIT_EVENTS; i++)
    {
        if (Syscall_state.thread_exit_events[i].used &&
            Syscall_state.thread_exit_events[i].owner_pid == owner_pid &&
            Syscall_state.thread_exit_events[i].tid == tid)
        {
            slot = (int32_t) i;
            break;
        }

        if (!Syscall_state.thread_exit_events[i].used && slot < 0)
            slot = (int32_t) i;
    }

    if (slot < 0)
        slot = 0;

    syscall_thread_exit_event_t* event = &Syscall_state.thread_exit_events[(uint32_t) slot];
    event->used = true;
    event->owner_pid = owner_pid;
    event->tid = tid;
    event->value = value;
}

static bool Syscall_exit_event_pop_locked(uint32_t ppid,
                                          int32_t wait_pid,
                                          uint32_t* out_pid,
                                          int64_t* out_status,
                                          int32_t* out_signal)
{
    for (uint32_t i = 0; i < SYSCALL_MAX_EXIT_EVENTS; i++)
    {
        syscall_exit_event_t* event = &Syscall_state.exit_events[i];
        if (!event->used || event->ppid != ppid)
            continue;
        if (wait_pid > 0 && event->pid != (uint32_t) wait_pid)
            continue;

        if (out_pid)
            *out_pid = event->pid;
        if (out_status)
            *out_status = event->status;
        if (out_signal)
            *out_signal = event->signal;
        memset(event, 0, sizeof(*event));
        return true;
    }

    return false;
}

static bool Syscall_thread_exit_event_pop_locked(uint32_t owner_pid,
                                                 uint32_t tid,
                                                 uint64_t* out_value)
{
    for (uint32_t i = 0; i < SYSCALL_MAX_THREAD_EXIT_EVENTS; i++)
    {
        syscall_thread_exit_event_t* event = &Syscall_state.thread_exit_events[i];
        if (!event->used || event->owner_pid != owner_pid || event->tid != tid)
            continue;

        if (out_value)
            *out_value = event->value;
        memset(event, 0, sizeof(*event));
        return true;
    }

    return false;
}

static bool Syscall_proc_owner_has_other_live_locked(uint32_t owner_pid, int32_t exclude_slot)
{
    if (owner_pid == 0)
        return false;

    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        if ((int32_t) i == exclude_slot)
            continue;
        if (!Syscall_state.procs[i].used)
            continue;
        if (Syscall_state.procs[i].owner_pid != owner_pid)
            continue;
        return true;
    }

    return false;
}

/* Same owner_pid as pthreads, but threads must not block fork() on the main task. */
static bool Syscall_proc_owner_has_other_live_non_thread_locked(uint32_t owner_pid, int32_t exclude_slot)
{
    if (owner_pid == 0)
        return false;

    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        if ((int32_t) i == exclude_slot)
            continue;
        if (!Syscall_state.procs[i].used)
            continue;
        if (Syscall_state.procs[i].is_thread)
            continue;
        if (Syscall_state.procs[i].owner_pid != owner_pid)
            continue;
        return true;
    }

    return false;
}

static bool Syscall_proc_has_live_thread_locked(uint32_t owner_pid, uint32_t tid)
{
    if (owner_pid == 0 || tid == 0)
        return false;

    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        syscall_process_t* proc = &Syscall_state.procs[i];
        if (!proc->used || !proc->is_thread)
            continue;
        if (proc->owner_pid != owner_pid || proc->pid != tid)
            continue;
        return true;
    }

    return false;
}

static bool Syscall_proc_has_matching_child_locked(uint32_t ppid, int32_t wait_pid)
{
    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        if (!Syscall_state.procs[i].used || Syscall_state.procs[i].ppid != ppid)
            continue;
        if (wait_pid > 0 &&
            Syscall_state.procs[i].pid != (uint32_t) wait_pid &&
            Syscall_state.procs[i].owner_pid != (uint32_t) wait_pid)
            continue;
        return true;
    }

    return false;
}

void Syscall_init(void)
{
    Syscall_bootstrap_domain = SYS_PROC_DOMAIN_USERLAND;
    memset(Syscall_state.fds, 0, sizeof(Syscall_state.fds));
    spinlock_init(&Syscall_state.fd_lock);
    Syscall_state.fd_lock_ready = true;
    spinlock_init(&Syscall_state.vm_lock);
    Syscall_state.vm_lock_ready = true;

    memset(Syscall_state.procs, 0, sizeof(Syscall_state.procs));
    memset(Syscall_state.console_routes, 0, sizeof(Syscall_state.console_routes));
    memset(Syscall_state.exit_events, 0, sizeof(Syscall_state.exit_events));
    memset(Syscall_state.thread_exit_events, 0, sizeof(Syscall_state.thread_exit_events));
    memset(Syscall_state.cow_refs, 0, sizeof(Syscall_state.cow_refs));
    for (uint32_t i = 0; i < 256; i++)
    {
        Syscall_state.cpu_current_proc[i] = SYSCALL_PROC_NONE;
        Syscall_state.cpu_need_resched[i] = 0;
        Syscall_state.cpu_yield_same_owner_pick[i] = 0;
        Syscall_state.cpu_need_timer_preempt[i] = 0;
        Syscall_state.cpu_slice_ticks[i] = 0;
    }
    Syscall_state.next_pid = 1U;
    spinlock_init(&Syscall_state.proc_lock);
    Syscall_state.proc_lock_ready = true;
    spinlock_init(&Syscall_state.console_lock);
    Syscall_state.console_lock_ready = true;
    spinlock_init(&Syscall_state.cow_lock);
    Syscall_state.cow_lock_ready = true;
    spinlock_init(&Syscall_kbd_inject_lock);
    Syscall_kbd_inject_lock_ready = true;

    memset(Syscall_state.pipes, 0, sizeof(Syscall_state.pipes));
    spinlock_init(&Syscall_state.pipe_lock);
    Syscall_state.pipe_lock_ready = true;

    memset(Syscall_state.futex_buckets, 0, sizeof(Syscall_state.futex_buckets));
    spinlock_init(&Syscall_state.futex_lock);
    Syscall_state.futex_lock_ready = true;

    memset(Syscall_state.shm_segments, 0, sizeof(Syscall_state.shm_segments));
    spinlock_init(&Syscall_state.shm_lock);
    Syscall_state.shm_lock_ready = true;

    memset(Syscall_state.msg_queues, 0, sizeof(Syscall_state.msg_queues));

    MSR_set(IA32_LSTAR, (uint64_t) &syscall_handler_stub);
    MSR_set(IA32_FMASK, SYSCALL_FMASK_TF_BIT | SYSCALL_FMASK_DF_BIT);

    enable_syscall_ext();
    DRM_init();
    NET_socket_init();
    NET_tcp_init();
    NET_unix_init();
}

/* ------------------------------------------------------------------ */
/*  Pipe handlers                                                      */
/* ------------------------------------------------------------------ */

static bool Syscall_pipe_rx_empty(void* ctx) { syscall_pipe_t* p = (syscall_pipe_t*) ctx; return p && __atomic_load_n(&p->count, __ATOMIC_ACQUIRE) == 0 && __atomic_load_n(&p->writers, __ATOMIC_ACQUIRE) > 0; }
static bool Syscall_pipe_tx_full(void* ctx)  { syscall_pipe_t* p = (syscall_pipe_t*) ctx; return p && __atomic_load_n(&p->count, __ATOMIC_ACQUIRE) >= SYSCALL_PIPE_BUF_SIZE && __atomic_load_n(&p->readers, __ATOMIC_ACQUIRE) > 0; }

static uint64_t Syscall_handle_pipe(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.pipe_lock_ready)
        return (uint64_t) -1;

    int* user_fds = (int*) frame->rdi;
    if (!user_fds)
        return (uint64_t) -1;

    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (owner_pid == 0U)
        return (uint64_t) -1;

    spin_lock(&Syscall_state.pipe_lock);
    int32_t pipe_id = -1;
    for (uint32_t i = 0; i < SYSCALL_PIPE_MAX; i++)
    {
        if (!Syscall_state.pipes[i].used)
        {
            pipe_id = (int32_t) i;
            break;
        }
    }
    if (pipe_id < 0)
    {
        spin_unlock(&Syscall_state.pipe_lock);
        return (uint64_t) -1;
    }

    syscall_pipe_t* p = &Syscall_state.pipes[(uint32_t) pipe_id];
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->readers = 1;
    p->writers = 1;
    spinlock_init(&p->lock);
    p->lock_ready = true;
    task_wait_queue_init(&p->read_waitq);
    task_wait_queue_init(&p->write_waitq);
    spin_unlock(&Syscall_state.pipe_lock);

    spin_lock(&Syscall_state.fd_lock);
    int32_t read_fd = Syscall_fd_alloc_locked();
    if (read_fd < 0)
    {
        spin_unlock(&Syscall_state.fd_lock);
        spin_lock(&Syscall_state.pipe_lock);
        p->used = false;
        spin_unlock(&Syscall_state.pipe_lock);
        return (uint64_t) -1;
    }
    syscall_file_desc_t* rentry = &Syscall_state.fds[(uint32_t) read_fd];
    memset(rentry, 0, sizeof(*rentry));
    rentry->used = true;
    rentry->type = SYSCALL_FD_TYPE_PIPE;
    rentry->owner_pid = owner_pid;
    rentry->can_read = true;
    rentry->can_write = false;
    rentry->net_socket_id = (uint32_t) pipe_id;

    int32_t write_fd = Syscall_fd_alloc_locked();
    if (write_fd < 0)
    {
        memset(rentry, 0, sizeof(*rentry));
        spin_unlock(&Syscall_state.fd_lock);
        spin_lock(&Syscall_state.pipe_lock);
        p->used = false;
        spin_unlock(&Syscall_state.pipe_lock);
        return (uint64_t) -1;
    }
    syscall_file_desc_t* wentry = &Syscall_state.fds[(uint32_t) write_fd];
    memset(wentry, 0, sizeof(*wentry));
    wentry->used = true;
    wentry->type = SYSCALL_FD_TYPE_PIPE;
    wentry->owner_pid = owner_pid;
    wentry->can_read = false;
    wentry->can_write = true;
    wentry->net_socket_id = (uint32_t) pipe_id;
    spin_unlock(&Syscall_state.fd_lock);

    int result[2] = { (int) read_fd, (int) write_fd };
    if (!Syscall_copy_to_user(user_fds, result, sizeof(result)))
    {
        spin_lock(&Syscall_state.fd_lock);
        memset(&Syscall_state.fds[(uint32_t) read_fd], 0, sizeof(syscall_file_desc_t));
        memset(&Syscall_state.fds[(uint32_t) write_fd], 0, sizeof(syscall_file_desc_t));
        spin_unlock(&Syscall_state.fd_lock);
        spin_lock(&Syscall_state.pipe_lock);
        p->used = false;
        spin_unlock(&Syscall_state.pipe_lock);
        return (uint64_t) -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Futex handler                                                      */
/* ------------------------------------------------------------------ */

static uint32_t Syscall_futex_hash(uintptr_t cr3_phys, uintptr_t vaddr)
{
    uintptr_t page = vaddr & ~0xFFFULL;
    uint64_t h = (uint64_t) cr3_phys ^ (uint64_t) page;
    h ^= (h >> 16);
    h *= 0x45d9f3bULL;
    h ^= (h >> 16);
    return (uint32_t) (h % SYSCALL_FUTEX_BUCKETS);
}

typedef struct futex_wait_ctx
{
    volatile int* uaddr;
    int expected;
} futex_wait_ctx_t;

static bool Syscall_futex_still_equal(void* ctx)
{
    futex_wait_ctx_t* fc = (futex_wait_ctx_t*) ctx;
    return fc && __atomic_load_n(fc->uaddr, __ATOMIC_ACQUIRE) == fc->expected;
}

static uint64_t Syscall_handle_futex(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.futex_lock_ready)
        return (uint64_t) -1;

    volatile int* uaddr = (volatile int*) frame->rdi;
    int op = (int) frame->rsi;
    int val = (int) frame->rdx;

    if (!uaddr)
        return (uint64_t) -1;

    uintptr_t cr3_phys = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_phys));
    uint32_t bucket = Syscall_futex_hash(cr3_phys, (uintptr_t) uaddr);

    spin_lock(&Syscall_state.futex_lock);
    syscall_futex_bucket_t* b = &Syscall_state.futex_buckets[bucket];
    if (!b->init)
    {
        task_wait_queue_init(&b->waitq);
        b->init = true;
    }
    spin_unlock(&Syscall_state.futex_lock);

    if (op == SYS_FUTEX_WAIT)
    {
        int current_val = __atomic_load_n(uaddr, __ATOMIC_ACQUIRE);
        if (current_val != val)
            return (uint64_t) -11; /* EAGAIN */

        futex_wait_ctx_t wc = { .uaddr = uaddr, .expected = val };
        task_waiter_t waiter;
        task_waiter_init(&waiter);

        uint64_t timeout = (frame->r10 != 0) ? (uint64_t) frame->r10 : TASK_WAIT_TIMEOUT_INFINITE;
        task_wait_queue_wait_event(&b->waitq, &waiter,
                                   Syscall_futex_still_equal, &wc,
                                   timeout);
        return 0;
    }

    if (op == SYS_FUTEX_WAKE)
    {
        if (val <= 0)
            return 0;
        uint32_t woken = 0;
        for (int i = 0; i < val; i++)
        {
            task_wait_queue_wake_one(&b->waitq);
            woken++;
        }
        return (uint64_t) woken;
    }

    return (uint64_t) -1;
}

/* ------------------------------------------------------------------ */
/*  Shared Memory handlers                                             */
/* ------------------------------------------------------------------ */

static uint64_t Syscall_handle_shmget(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.shm_lock_ready)
        return (uint64_t) -1;
    (void) cpu_index;

    int32_t key = (int32_t) frame->rdi;
    size_t size = (size_t) frame->rsi;
    uint32_t flags = (uint32_t) frame->rdx;
    bool create = (flags & 0x200U) != 0; /* IPC_CREAT */

    spin_lock(&Syscall_state.shm_lock);

    for (uint32_t i = 0; i < SYSCALL_SHM_MAX_SEGMENTS; i++)
    {
        if (Syscall_state.shm_segments[i].used && Syscall_state.shm_segments[i].key == key)
        {
            spin_unlock(&Syscall_state.shm_lock);
            return (uint64_t) i;
        }
    }

    if (!create)
    {
        spin_unlock(&Syscall_state.shm_lock);
        return (uint64_t) -1;
    }

    if (size == 0 || size > SYSCALL_SHM_MAX_PAGES * 4096ULL)
    {
        spin_unlock(&Syscall_state.shm_lock);
        return (uint64_t) -1;
    }

    int32_t slot = -1;
    for (uint32_t i = 0; i < SYSCALL_SHM_MAX_SEGMENTS; i++)
    {
        if (!Syscall_state.shm_segments[i].used)
        {
            slot = (int32_t) i;
            break;
        }
    }
    if (slot < 0)
    {
        spin_unlock(&Syscall_state.shm_lock);
        return (uint64_t) -1;
    }

    uint32_t num_pages = (uint32_t) ((size + 4095ULL) / 4096ULL);
    syscall_shm_segment_t* seg = &Syscall_state.shm_segments[slot];
    memset(seg, 0, sizeof(*seg));
    seg->used = true;
    seg->key = key;
    seg->size = size;
    seg->num_pages = num_pages;
    seg->mode = flags & 0x1FFU;

    for (uint32_t i = 0; i < num_pages; i++)
    {
        void* page_ptr = PMM_alloc_page();
        if (!page_ptr)
        {
            for (uint32_t j = 0; j < i; j++)
                PMM_dealloc_page((void*) seg->pages[j]);
            memset(seg, 0, sizeof(*seg));
            spin_unlock(&Syscall_state.shm_lock);
            return (uint64_t) -1;
        }
        seg->pages[i] = (uintptr_t) page_ptr;
        memset((void*) P2V(seg->pages[i]), 0, 4096);
    }

    spin_unlock(&Syscall_state.shm_lock);
    return (uint64_t) slot;
}

static uint64_t Syscall_handle_shmat(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.shm_lock_ready)
        return (uint64_t) -1;
    (void) cpu_index;

    int32_t shmid = (int32_t) frame->rdi;
    uintptr_t hint = (uintptr_t) frame->rsi;

    if (shmid < 0 || (uint32_t) shmid >= SYSCALL_SHM_MAX_SEGMENTS)
        return (uint64_t) -1;

    spin_lock(&Syscall_state.shm_lock);
    syscall_shm_segment_t* seg = &Syscall_state.shm_segments[shmid];
    if (!seg->used)
    {
        spin_unlock(&Syscall_state.shm_lock);
        return (uint64_t) -1;
    }

    uint32_t num_pages = seg->num_pages;
    uintptr_t pages_copy[SYSCALL_SHM_MAX_PAGES];
    for (uint32_t i = 0; i < num_pages; i++)
        pages_copy[i] = seg->pages[i];
    seg->refcount++;
    spin_unlock(&Syscall_state.shm_lock);

    uintptr_t base = hint;
    if (base == 0)
    {
        spin_lock(&Syscall_state.vm_lock);
        base = Syscall_state.user_map_hint;
        Syscall_state.user_map_hint += (uintptr_t) num_pages * 4096ULL;
        spin_unlock(&Syscall_state.vm_lock);
    }

    uintptr_t cr3_phys = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_phys));

    for (uint32_t i = 0; i < num_pages; i++)
    {
        uintptr_t virt = base + (uintptr_t) i * 4096ULL;
        VMM_map_user_page(virt, pages_copy[i]);
    }

    return (uint64_t) base;
}

static uint64_t Syscall_handle_shmdt(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.shm_lock_ready)
        return (uint64_t) -1;
    (void) cpu_index;

    uintptr_t addr = (uintptr_t) frame->rdi;
    if (addr == 0)
        return (uint64_t) -1;

    spin_lock(&Syscall_state.shm_lock);
    for (uint32_t i = 0; i < SYSCALL_SHM_MAX_SEGMENTS; i++)
    {
        syscall_shm_segment_t* seg = &Syscall_state.shm_segments[i];
        if (!seg->used)
            continue;

        bool match = false;
        uintptr_t cr3_phys = 0;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_phys));
        for (uint32_t p = 0; p < seg->num_pages; p++)
        {
            uintptr_t virt = addr + (uintptr_t) p * 4096ULL;
            uint64_t* pte = Syscall_get_user_pte_ptr(cr3_phys, virt);
            if (pte && (*pte & 1ULL) && ((*pte & ~0xFFFULL) == seg->pages[p]))
            {
                match = true;
                break;
            }
        }

        if (match)
        {
            for (uint32_t p = 0; p < seg->num_pages; p++)
            {
                uintptr_t virt = addr + (uintptr_t) p * 4096ULL;
                uintptr_t old_phys = 0;
                VMM_unmap_page(virt, &old_phys);
            }
            if (seg->refcount > 0)
                seg->refcount--;
            if (seg->refcount == 0 && seg->marked_remove)
            {
                for (uint32_t p = 0; p < seg->num_pages; p++)
                    PMM_dealloc_page((void*) seg->pages[p]);
                memset(seg, 0, sizeof(*seg));
            }
            spin_unlock(&Syscall_state.shm_lock);
            return 0;
        }
    }
    spin_unlock(&Syscall_state.shm_lock);
    return (uint64_t) -1;
}

static uint64_t Syscall_handle_shmctl(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.shm_lock_ready)
        return (uint64_t) -1;
    (void) cpu_index;

    int32_t shmid = (int32_t) frame->rdi;
    int32_t cmd = (int32_t) frame->rsi;

    if (shmid < 0 || (uint32_t) shmid >= SYSCALL_SHM_MAX_SEGMENTS)
        return (uint64_t) -1;

    if (cmd != 0) /* IPC_RMID = 0 */
        return (uint64_t) -1;

    spin_lock(&Syscall_state.shm_lock);
    syscall_shm_segment_t* seg = &Syscall_state.shm_segments[shmid];
    if (!seg->used)
    {
        spin_unlock(&Syscall_state.shm_lock);
        return (uint64_t) -1;
    }
    seg->marked_remove = true;
    if (seg->refcount == 0)
    {
        for (uint32_t p = 0; p < seg->num_pages; p++)
            PMM_dealloc_page((void*) seg->pages[p]);
        memset(seg, 0, sizeof(*seg));
    }
    spin_unlock(&Syscall_state.shm_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Message Queue handlers                                             */
/* ------------------------------------------------------------------ */

static bool Syscall_msgq_send_full(void* ctx)
{
    syscall_msgq_t* q = (syscall_msgq_t*) ctx;
    return q && __atomic_load_n(&q->count, __ATOMIC_ACQUIRE) >= SYSCALL_MSG_RING_SIZE;
}

static bool Syscall_msgq_recv_empty(void* ctx)
{
    syscall_msgq_t* q = (syscall_msgq_t*) ctx;
    return q && __atomic_load_n(&q->count, __ATOMIC_ACQUIRE) == 0;
}

static uint64_t Syscall_handle_msgget(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame)
        return (uint64_t) -1;
    (void) cpu_index;

    int32_t key = (int32_t) frame->rdi;
    uint32_t flags = (uint32_t) frame->rsi;
    bool create = (flags & 0x200U) != 0; /* IPC_CREAT */

    for (uint32_t i = 0; i < SYSCALL_MSG_MAX_QUEUES; i++)
    {
        if (Syscall_state.msg_queues[i].used && Syscall_state.msg_queues[i].key == key)
            return (uint64_t) i;
    }

    if (!create)
        return (uint64_t) -1;

    for (uint32_t i = 0; i < SYSCALL_MSG_MAX_QUEUES; i++)
    {
        syscall_msgq_t* q = &Syscall_state.msg_queues[i];
        if (!q->used)
        {
            memset(q, 0, sizeof(*q));
            q->used = true;
            q->key = key;
            spinlock_init(&q->lock);
            q->lock_ready = true;
            task_wait_queue_init(&q->send_waitq);
            task_wait_queue_init(&q->recv_waitq);
            return (uint64_t) i;
        }
    }
    return (uint64_t) -1;
}

static uint64_t Syscall_handle_msgsnd(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame)
        return (uint64_t) -1;
    (void) cpu_index;

    int32_t msqid = (int32_t) frame->rdi;
    const void* user_msgp = (const void*) frame->rsi;
    size_t msgsz = (size_t) frame->rdx;
    uint32_t msgflg = (uint32_t) frame->r10;

    if (msqid < 0 || (uint32_t) msqid >= SYSCALL_MSG_MAX_QUEUES || !user_msgp)
        return (uint64_t) -1;

    syscall_msgq_t* q = &Syscall_state.msg_queues[msqid];
    if (!q->used || !q->lock_ready)
        return (uint64_t) -1;

    if (msgsz > SYSCALL_MSG_MAX_TEXT)
        return (uint64_t) -1;

    int32_t mtype = 0;
    if (!Syscall_copy_from_user(&mtype, user_msgp, sizeof(mtype)))
        return (uint64_t) -1;

    uint8_t mtext[SYSCALL_MSG_MAX_TEXT];
    if (msgsz > 0 && !Syscall_copy_from_user(mtext, (const uint8_t*) user_msgp + sizeof(int32_t), msgsz))
        return (uint64_t) -1;

    spin_lock(&q->lock);
    if (q->count >= SYSCALL_MSG_RING_SIZE)
    {
        if (msgflg & 0x800U) /* IPC_NOWAIT */
        {
            spin_unlock(&q->lock);
            return (uint64_t) -2;
        }
        spin_unlock(&q->lock);

        task_waiter_t waiter;
        task_waiter_init(&waiter);
        task_wait_queue_wait_event(&q->send_waitq, &waiter,
                                   Syscall_msgq_send_full, q,
                                   TASK_WAIT_TIMEOUT_INFINITE);

        spin_lock(&q->lock);
        if (!q->used || q->count >= SYSCALL_MSG_RING_SIZE)
        {
            spin_unlock(&q->lock);
            return (uint64_t) -1;
        }
    }

    syscall_msg_t* slot = &q->ring[q->tail];
    slot->mtype = mtype;
    slot->len = (uint16_t) msgsz;
    if (msgsz > 0)
        memcpy(slot->mtext, mtext, msgsz);
    q->tail = (uint16_t) ((q->tail + 1U) % SYSCALL_MSG_RING_SIZE);
    __atomic_store_n(&q->count, q->count + 1U, __ATOMIC_RELEASE);
    task_wait_queue_wake_all(&q->recv_waitq);
    spin_unlock(&q->lock);
    return 0;
}

static uint64_t Syscall_handle_msgrcv(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame)
        return (uint64_t) -1;
    (void) cpu_index;

    int32_t msqid = (int32_t) frame->rdi;
    void* user_msgp = (void*) frame->rsi;
    size_t msgsz = (size_t) frame->rdx;
    int32_t msgtyp = (int32_t) frame->r10;
    uint32_t msgflg = (uint32_t) frame->r8;

    if (msqid < 0 || (uint32_t) msqid >= SYSCALL_MSG_MAX_QUEUES || !user_msgp)
        return (uint64_t) -1;

    syscall_msgq_t* q = &Syscall_state.msg_queues[msqid];
    if (!q->used || !q->lock_ready)
        return (uint64_t) -1;

    spin_lock(&q->lock);
retry:
    if (q->count == 0U)
    {
        if (msgflg & 0x800U) /* IPC_NOWAIT */
        {
            spin_unlock(&q->lock);
            return (uint64_t) -2;
        }
        spin_unlock(&q->lock);

        task_waiter_t waiter;
        task_waiter_init(&waiter);
        task_wait_queue_wait_event(&q->recv_waitq, &waiter,
                                   Syscall_msgq_recv_empty, q,
                                   TASK_WAIT_TIMEOUT_INFINITE);

        spin_lock(&q->lock);
        if (!q->used || q->count == 0U)
        {
            spin_unlock(&q->lock);
            return (uint64_t) -1;
        }
    }

    /* Search for matching message */
    uint32_t found_idx = SYSCALL_MSG_RING_SIZE;
    for (uint32_t i = 0; i < q->count; i++)
    {
        uint32_t idx = (q->head + i) % SYSCALL_MSG_RING_SIZE;
        if (msgtyp == 0 || q->ring[idx].mtype == msgtyp)
        {
            found_idx = idx;
            break;
        }
    }

    if (found_idx >= SYSCALL_MSG_RING_SIZE)
        goto retry;

    syscall_msg_t found_msg = q->ring[found_idx];

    /* Remove from ring - if it's the head, just advance head */
    if (found_idx == q->head)
    {
        q->head = (uint16_t) ((q->head + 1U) % SYSCALL_MSG_RING_SIZE);
    }
    else
    {
        /* Shift entries to fill the gap */
        uint32_t pos = found_idx;
        uint32_t end_pos = (q->head + q->count - 1U) % SYSCALL_MSG_RING_SIZE;
        while (pos != end_pos)
        {
            uint32_t next = (pos + 1U) % SYSCALL_MSG_RING_SIZE;
            q->ring[pos] = q->ring[next];
            pos = next;
        }
    }
    __atomic_store_n(&q->count, q->count - 1U, __ATOMIC_RELEASE);
    task_wait_queue_wake_all(&q->send_waitq);
    spin_unlock(&q->lock);

    size_t copy_len = found_msg.len < msgsz ? found_msg.len : msgsz;
    if (!Syscall_copy_to_user(user_msgp, &found_msg.mtype, sizeof(found_msg.mtype)))
        return (uint64_t) -1;
    if (copy_len > 0 && !Syscall_copy_to_user((uint8_t*) user_msgp + sizeof(int32_t), found_msg.mtext, copy_len))
        return (uint64_t) -1;

    return (uint64_t) copy_len;
}

void Syscall_on_timer_tick(uint32_t cpu_index)
{
    if (!Syscall_state.proc_lock_ready || cpu_index >= 256)
        return;
    Syscall_debug_tick_calls++;

    uint32_t runnable = 0;
    uint64_t flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
    {
        if (Syscall_state.procs[i].used)
            runnable++;
    }
    spin_unlock_irqrestore(&Syscall_state.proc_lock, flags);

    if (runnable <= 1U)
    {
        Syscall_state.cpu_slice_ticks[cpu_index] = 0;
        Syscall_debug_tick_runnable_le1++;
        return;
    }

    uint8_t slice = (uint8_t) (Syscall_state.cpu_slice_ticks[cpu_index] + 1U);
    Syscall_state.cpu_slice_ticks[cpu_index] = slice;
    if (slice >= SYSCALL_PREEMPT_QUANTUM_TICKS)
    {
        __atomic_store_n(&Syscall_state.cpu_need_timer_preempt[cpu_index], 1, __ATOMIC_RELEASE);
        Syscall_debug_need_resched_set++;
        Syscall_debug_need_resched_set_cpu[cpu_index & 0x3U]++;
    }

}

bool Syscall_try_dispatch_user_from_idle(uint32_t cpu_index)
{
    if (!Syscall_state.proc_lock_ready || cpu_index >= 256)
        return false;

    Syscall_debug_idle_dispatch_attempts++;
    Syscall_debug_idle_dispatch_attempt_cpu[cpu_index & 0x3U]++;

    syscall_user_resume_context_t resume_ctx;
    memset(&resume_ctx, 0, sizeof(resume_ctx));
    uintptr_t next_cr3 = 0;
    uintptr_t next_fs_base = 0;
    uint32_t next_pid = 0;
    bool has_target = false;

    uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    int32_t current_slot = Syscall_proc_get_current_slot_locked(cpu_index);
    if (current_slot >= 0)
    {
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        Syscall_debug_idle_dispatch_rejected_running++;
        return false;
    }

    int32_t next_slot = -1;
    syscall_process_t* next = NULL;
    for (uint32_t idx = 0; idx < SYSCALL_MAX_PROCS; idx++)
    {
        syscall_process_t* cand = &Syscall_state.procs[idx];
        if (!cand->used || cand->exiting || cand->cr3_phys == 0)
            continue;
        if (cand->pid == 1U || cand->ppid == 0U)
            continue;
        if (Syscall_idle_claimed_slots[idx] != 0U)
            continue;
        if (Syscall_proc_is_on_other_cpu_locked(idx, cpu_index))
            continue;

        next_slot = (int32_t) idx;
        next = cand;
        break;
    }
    if (next_slot < 0 || !next)
    {
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        Syscall_debug_idle_dispatch_rejected_empty++;
        return false;
    }

    Syscall_state.cpu_current_proc[cpu_index] = (uint32_t) next_slot;
    next->last_cpu = cpu_index;
    Syscall_idle_claimed_slots[(uint32_t) next_slot] = 1U;
    Syscall_state.cpu_slice_ticks[cpu_index] = 0U;
    __atomic_store_n(&Syscall_state.cpu_need_resched[cpu_index], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&Syscall_state.cpu_need_timer_preempt[cpu_index], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&Syscall_state.cpu_yield_same_owner_pick[cpu_index], 0, __ATOMIC_RELEASE);

    resume_ctx.rax = next->rax;
    resume_ctx.rcx = next->rcx;
    resume_ctx.rdx = next->rdx;
    resume_ctx.rsi = next->rsi;
    resume_ctx.rdi = next->rdi;
    resume_ctx.r8 = next->r8;
    resume_ctx.r9 = next->r9;
    resume_ctx.r10 = next->r10;
    resume_ctx.r11 = next->r11;
    resume_ctx.r15 = next->r15;
    resume_ctx.r14 = next->r14;
    resume_ctx.r13 = next->r13;
    resume_ctx.r12 = next->r12;
    resume_ctx.rbp = next->rbp;
    resume_ctx.rbx = next->rbx;
    resume_ctx.rip = next->rip;
    resume_ctx.rflags = next->rflags | SYSCALL_RFLAGS_IF;
    resume_ctx.rsp = next->rsp;

    next_cr3 = next->cr3_phys;
    next_fs_base = next->fs_base;
    next_pid = next->pid;
    has_target = true;
    spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

    if (!has_target)
        return false;

    if (next_cr3 != Syscall_read_cr3_phys())
        Syscall_write_cr3_phys(next_cr3);
    if (next_fs_base != Syscall_read_fs_base())
        Syscall_write_fs_base(next_fs_base);

    /*
     * Idle->user handoff may run outside the syscall stub path.
     * Re-establish GS/KERNEL_GS_BASE invariants expected by SYSCALL entry:
     *   - IA32_GS_BASE         = user GS base (currently 0)
     *   - IA32_KERNEL_GS_BASE  = per-CPU kernel local pointer
     */
    task_cpu_local_t* cpu_local = task_get_cpu_local();
    if (cpu_local)
    {
        MSR_set(IA32_GS_BASE, 0);
        MSR_set(IA32_KERNEL_GS_BASE, (uint64_t) (uintptr_t) cpu_local);
    }

    Syscall_debug_idle_dispatch_success++;
    Syscall_debug_idle_dispatch_success_cpu[cpu_index & 0x3U]++;
    kdebug_printf("[SMPUSR] idle dispatch cpu=%u pid=%u rip=0x%llx\n",
                  (unsigned) cpu_index,
                  (unsigned) next_pid,
                  (unsigned long long) resume_ctx.rip);
    Syscall_resume_user_context(&resume_ctx);
}

bool Syscall_handle_timer_preempt(interrupt_frame_t* frame, uint32_t cpu_index)
{
    if (!frame || !Syscall_state.proc_lock_ready || cpu_index >= 256)
        return false;

    if ((frame->cs & 0x3ULL) != 0x3ULL)
        return false;

    Syscall_debug_preempt_attempts++;
    Syscall_debug_preempt_attempt_cpu[cpu_index & 0x3U]++;
    uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    if (__atomic_exchange_n(&Syscall_state.cpu_need_timer_preempt[cpu_index], 0, __ATOMIC_ACQ_REL) == 0)
    {
        Syscall_debug_preempt_no_flag++;
        Syscall_debug_preempt_no_flag_cpu[cpu_index & 0x3U]++;
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        return false;
    }
    Syscall_debug_flag_consume_timer_cpu[cpu_index & 0x3U]++;

    int32_t current_slot = Syscall_proc_get_current_slot_locked(cpu_index);
    if (current_slot < 0)
    {
        syscall_frame_t bootstrap_frame;
        memset(&bootstrap_frame, 0, sizeof(bootstrap_frame));
        bootstrap_frame.r15 = frame->r15;
        bootstrap_frame.r14 = frame->r14;
        bootstrap_frame.r13 = frame->r13;
        bootstrap_frame.r12 = frame->r12;
        bootstrap_frame.rbp = frame->rbp;
        bootstrap_frame.rbx = frame->rbx;
        bootstrap_frame.rdi = frame->rdi;
        bootstrap_frame.rsi = frame->rsi;
        bootstrap_frame.rdx = frame->rdx;
        bootstrap_frame.r10 = frame->r10;
        bootstrap_frame.r8 = frame->r8;
        bootstrap_frame.r9 = frame->r9;
        bootstrap_frame.rip = frame->rip;
        bootstrap_frame.rflags = frame->rflags;
        bootstrap_frame.rsp = frame->rsp;
        current_slot = Syscall_proc_ensure_current_locked(cpu_index, &bootstrap_frame);
        if (current_slot < 0)
        {
            Syscall_debug_preempt_no_current++;
            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
            return false;
        }
    }

    syscall_process_t* current = &Syscall_state.procs[(uint32_t) current_slot];
    if (!current->used || current->exiting)
    {
        Syscall_state.cpu_slice_ticks[cpu_index] = 0;
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        return false;
    }

    uintptr_t current_cr3 = Syscall_read_cr3_phys();
    uintptr_t current_fs_base = Syscall_read_fs_base();
    Syscall_proc_save_from_interrupt(current, frame, current_cr3, current_fs_base);

    int32_t next_slot = Syscall_proc_pick_next_locked(current_slot, cpu_index);
    if (next_slot < 0 || next_slot == current_slot)
    {
        Syscall_debug_preempt_same_slot++;
        Syscall_state.cpu_slice_ticks[cpu_index] = 0;
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        return false;
    }

    syscall_process_t* next = &Syscall_state.procs[(uint32_t) next_slot];
    if (!next->used || next->exiting || next->cr3_phys == 0)
    {
        Syscall_debug_preempt_invalid_next++;
        Syscall_state.cpu_slice_ticks[cpu_index] = 0;
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        return false;
    }

    Syscall_state.cpu_current_proc[cpu_index] = (uint32_t) next_slot;
    next->last_cpu = cpu_index;
    Syscall_state.cpu_slice_ticks[cpu_index] = 0;
    Syscall_proc_load_to_interrupt(next, frame);
    uintptr_t next_cr3 = next->cr3_phys;
    uintptr_t next_fs_base = next->fs_base;
    spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

    if (next_cr3 != current_cr3)
        Syscall_write_cr3_phys(next_cr3);
    if (next_fs_base != current_fs_base)
        Syscall_write_fs_base(next_fs_base);

    Syscall_debug_preempt_success++;
    Syscall_debug_preempt_success_cpu[cpu_index & 0x3U]++;
    return true;
}

uint64_t Syscall_interrupt_handler(uint64_t syscall_num, syscall_frame_t* frame, uint32_t cpu_index)
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

    Syscall_debug_irq_calls_cpu[cpu_index & 0x3U]++;
    uint64_t count = __atomic_add_fetch(&Syscall_state.count_per_cpu[cpu_index], 1, __ATOMIC_RELAXED);
    if (cpu_local)
        __atomic_store_n(&cpu_local->syscall_count, count, __ATOMIC_RELAXED);

    if (syscall_num == SYS_READ)
        Syscall_debug_nr_read++;
    else if (syscall_num == SYS_WRITE)
        Syscall_debug_nr_write++;
    else if (syscall_num == SYS_SLEEP_MS)
        Syscall_debug_nr_sleep++;
    else
        Syscall_debug_nr_other++;

    switch (syscall_num)
    {
        case SYS_FS_MKDIR:
        {
            char path[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_read_user_cstr(path, sizeof(path), (const char*) frame->rdi))
                return (uint64_t) -1;

            char normalized[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_normalize_write_path(path, normalized, sizeof(normalized)))
                return (uint64_t) -1;

            bool ok = VFS_mkdir(normalized);
            return ok ? 0 : (uint64_t) -1;
        }

        case SYS_SLEEP_MS:
        {
            bool sleep_ok = HPET_sleep_ms((uint32_t) frame->rdi);
            if (sleep_ok && cpu_index < 256)
            {
                __atomic_store_n(&Syscall_state.cpu_need_resched[cpu_index], 1, __ATOMIC_RELEASE);
                Syscall_debug_flag_set_sleep_cpu[cpu_index & 0x3U]++;
            }
            return sleep_ok ? 0 : (uint64_t) -1;
        }

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
            task_get_activity_counters(&info.sched_exec_total, &info.sched_idle_hlt_total);
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

        case SYS_PROC_INFO_GET:
        {
            if (!Syscall_state.proc_lock_ready)
                return (uint64_t) -1;

            syscall_proc_info_t snapshot[SYSCALL_MAX_PROCS];
            uint32_t running_cpu[SYSCALL_MAX_PROCS];
            uint32_t last_cpu[SYSCALL_MAX_PROCS];
            for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
            {
                memset(&snapshot[i], 0, sizeof(snapshot[i]));
                running_cpu[i] = SYS_PROC_CPU_NONE;
                last_cpu[i] = SYS_PROC_CPU_NONE;
            }

            uint32_t snapshot_count = 0;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            for (uint32_t cpu = 0; cpu < 256; cpu++)
            {
                uint32_t slot = Syscall_state.cpu_current_proc[cpu];
                if (slot >= SYSCALL_MAX_PROCS)
                    continue;
                if (!Syscall_state.procs[slot].used)
                    continue;
                running_cpu[slot] = cpu;
            }

            for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
            {
                const syscall_process_t* proc = &Syscall_state.procs[i];
                if (!proc->used)
                    continue;
                if (snapshot_count >= SYSCALL_MAX_PROCS)
                    break;
                if (proc->last_cpu < 256U)
                    last_cpu[i] = proc->last_cpu;

                syscall_proc_info_t* out = &snapshot[snapshot_count++];
                out->pid = proc->pid;
                out->ppid = proc->ppid;
                out->owner_pid = proc->owner_pid;
                out->domain = proc->domain;
                out->flags = 0;
                if (proc->is_thread)
                    out->flags |= SYS_PROC_FLAG_THREAD;
                if (proc->exiting)
                    out->flags |= SYS_PROC_FLAG_EXITING;
                if (proc->terminated_by_signal)
                    out->flags |= SYS_PROC_FLAG_TERMINATED_BY_SIGNAL;
                if (proc->domain == SYS_PROC_DOMAIN_DRIVERLAND)
                    out->flags |= SYS_PROC_FLAG_DRIVERLAND;
                if (running_cpu[i] != SYS_PROC_CPU_NONE)
                {
                    out->flags |= SYS_PROC_FLAG_ON_CPU;
                    out->current_cpu = running_cpu[i];
                    out->last_cpu = running_cpu[i];
                }
                else
                {
                    out->current_cpu = last_cpu[i];
                    out->last_cpu = last_cpu[i];
                }
                out->term_signal = proc->terminated_by_signal ? (uint32_t) proc->term_signal : 0U;
                out->exit_status = proc->exit_status;
            }
            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

            uint32_t max_entries = (uint32_t) frame->rsi;
            uint32_t copy_count = snapshot_count;
            if (copy_count > max_entries)
                copy_count = max_entries;

            syscall_proc_info_t* user_entries = (syscall_proc_info_t*) frame->rdi;
            if (copy_count > 0U &&
                !Syscall_copy_to_user(user_entries, snapshot, (size_t) copy_count * sizeof(snapshot[0])))
            {
                return (uint64_t) -1;
            }

            uint32_t* user_total = (uint32_t*) frame->rdx;
            if (user_total && !Syscall_copy_to_user(user_total, &snapshot_count, sizeof(snapshot_count)))
                return (uint64_t) -1;

            return (uint64_t) copy_count;
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

            uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
            bool owner_is_driverland = Syscall_proc_owner_is_driverland(owner_pid);
            bool write_tty = !owner_is_driverland;
            bool mirror_kdebug = true;
            uint32_t console_sid = Syscall_proc_current_console_sid(cpu_index, frame);
            if (Syscall_state.console_lock_ready && console_sid != 0U)
            {
                uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.console_lock);
                int32_t route_slot = Syscall_console_route_find_locked(console_sid);
                if (route_slot >= 0)
                {
                    syscall_console_route_t* route = &Syscall_state.console_routes[(uint32_t) route_slot];
                    uint32_t route_flags = route->flags;
                    write_tty = (route_flags & SYS_CONSOLE_ROUTE_FLAG_TTY) != 0U;
                    if (!owner_is_driverland &&
                        (route_flags & SYS_CONSOLE_ROUTE_FLAG_CAPTURE) != 0U &&
                        (route_flags & SYS_CONSOLE_ROUTE_FLAG_TTY) == 0U)
                    {
                        /* Capture-only routes (GUI bridges) should not flood kdebug/serial. */
                        mirror_kdebug = false;
                    }
                    Syscall_console_route_push_locked(route, kernel_buf, len);
                }
                spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
            }

            for (size_t i = 0; i < len; i++)
            {
                if (write_tty)
                    putc(kernel_buf[i]);
                if (mirror_kdebug)
                    kdebug_putc(kernel_buf[i]);
            }

            kfree(kernel_buf);
            return (uint64_t) len;
        }

        case SYS_CONSOLE_ROUTE_SET:
        {
            uint32_t flags = (uint32_t) frame->rdi;
            uint32_t allowed =
                SYS_CONSOLE_ROUTE_FLAG_CAPTURE | SYS_CONSOLE_ROUTE_FLAG_TTY | SYS_CONSOLE_ROUTE_FLAG_PTY_INPUT;
            if ((flags & ~allowed) != 0U)
                return (uint64_t) -1;

            if (!Syscall_state.console_lock_ready)
                return (uint64_t) -1;

            uint32_t console_sid = Syscall_proc_current_console_sid(cpu_index, frame);
            if (console_sid == 0U)
                return (uint64_t) -1;
            uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
            if (owner_pid == 0U)
                return (uint64_t) -1;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.console_lock);
            int32_t route_slot = Syscall_console_route_find_locked(console_sid);
            if (flags == 0U)
            {
                if (route_slot >= 0)
                    memset(&Syscall_state.console_routes[(uint32_t) route_slot], 0, sizeof(syscall_console_route_t));
                spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
                return 0ULL;
            }

            if (route_slot < 0)
            {
                route_slot = Syscall_console_route_alloc_locked(owner_pid, console_sid);
                if (route_slot < 0)
                {
                    spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
                    return (uint64_t) -1;
                }
            }

            syscall_console_route_t* route = &Syscall_state.console_routes[(uint32_t) route_slot];
            if (route->owner_pid != owner_pid)
            {
                spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
                return (uint64_t) -1;
            }
            route->flags = flags;
            if ((flags & SYS_CONSOLE_ROUTE_FLAG_CAPTURE) == 0U)
            {
                route->head = 0U;
                route->tail = 0U;
                route->count = 0U;
            }
            if ((flags & SYS_CONSOLE_ROUTE_FLAG_PTY_INPUT) == 0U)
            {
                route->in_head = 0U;
                route->in_tail = 0U;
                route->in_count = 0U;
            }
            spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
            return 0ULL;
        }

        case SYS_CONSOLE_ROUTE_READ:
        {
            char* user_buf = (char*) frame->rdi;
            size_t cap = (size_t) frame->rsi;
            if (!user_buf || cap == 0U)
                return 0ULL;

            if (cap > SYSCALL_CONSOLE_MAX_WRITE)
                cap = SYSCALL_CONSOLE_MAX_WRITE;

            if (!Syscall_state.console_lock_ready)
                return 0ULL;

            uint32_t console_sid = Syscall_proc_current_console_sid(cpu_index, frame);
            if (console_sid == 0U)
                return 0ULL;
            uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
            if (owner_pid == 0U)
                return 0ULL;

            char* kernel_buf = (char*) kmalloc(cap);
            if (!kernel_buf)
                return (uint64_t) -1;

            size_t copied = 0U;
            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.console_lock);
            int32_t route_slot = Syscall_console_route_find_locked(console_sid);
            if (route_slot >= 0)
            {
                syscall_console_route_t* route = &Syscall_state.console_routes[(uint32_t) route_slot];
                if (route->owner_pid == owner_pid &&
                    (route->flags & SYS_CONSOLE_ROUTE_FLAG_CAPTURE) != 0U)
                {
                    copied = Syscall_console_route_pop_locked(route, kernel_buf, cap);
                }
            }
            spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);

            if (copied == 0U)
            {
                kfree(kernel_buf);
                return 0ULL;
            }

            if (!Syscall_copy_to_user(user_buf, kernel_buf, copied))
            {
                kfree(kernel_buf);
                return (uint64_t) -1;
            }

            kfree(kernel_buf);
            return (uint64_t) copied;
        }

        case SYS_CONSOLE_ROUTE_SET_SID:
        {
            uint32_t console_sid = (uint32_t) frame->rdi;
            uint32_t flags = (uint32_t) frame->rsi;
            uint32_t allowed =
                SYS_CONSOLE_ROUTE_FLAG_CAPTURE | SYS_CONSOLE_ROUTE_FLAG_TTY | SYS_CONSOLE_ROUTE_FLAG_PTY_INPUT;
            if (console_sid == 0U || (flags & ~allowed) != 0U)
                return (uint64_t) -1;

            if (!Syscall_state.console_lock_ready)
                return (uint64_t) -1;

            uint32_t caller_owner_pid = Syscall_proc_current_pid(cpu_index, frame);
            if (caller_owner_pid == 0U)
                return (uint64_t) -1;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.console_lock);
            int32_t route_slot = Syscall_console_route_find_locked(console_sid);
            if (flags == 0U)
            {
                if (route_slot >= 0 &&
                    Syscall_state.console_routes[(uint32_t) route_slot].owner_pid == caller_owner_pid)
                {
                    memset(&Syscall_state.console_routes[(uint32_t) route_slot], 0, sizeof(syscall_console_route_t));
                }
                spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
                return 0ULL;
            }

            if (route_slot < 0)
            {
                spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
                if (!Syscall_console_sid_manageable_by(caller_owner_pid, console_sid) &&
                    !Syscall_console_sid_assign_process(caller_owner_pid, console_sid))
                    return (uint64_t) -1;

                lock_flags = spin_lock_irqsave(&Syscall_state.console_lock);
                route_slot = Syscall_console_route_alloc_locked(caller_owner_pid, console_sid);
                if (route_slot < 0)
                {
                    spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
                    return (uint64_t) -1;
                }
            }

            syscall_console_route_t* route = &Syscall_state.console_routes[(uint32_t) route_slot];
            if (route->owner_pid != caller_owner_pid)
            {
                spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
                return (uint64_t) -1;
            }
            route->flags = flags;
            if ((flags & SYS_CONSOLE_ROUTE_FLAG_CAPTURE) == 0U)
            {
                route->head = 0U;
                route->tail = 0U;
                route->count = 0U;
            }
            if ((flags & SYS_CONSOLE_ROUTE_FLAG_PTY_INPUT) == 0U)
            {
                route->in_head = 0U;
                route->in_tail = 0U;
                route->in_count = 0U;
            }
            spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
            return 0ULL;
        }

        case SYS_CONSOLE_ROUTE_READ_SID:
        {
            uint32_t console_sid = (uint32_t) frame->rdi;
            char* user_buf = (char*) frame->rsi;
            size_t cap = (size_t) frame->rdx;
            if (console_sid == 0U || !user_buf || cap == 0U)
                return 0ULL;

            if (cap > SYSCALL_CONSOLE_MAX_WRITE)
                cap = SYSCALL_CONSOLE_MAX_WRITE;

            if (!Syscall_state.console_lock_ready)
                return 0ULL;

            uint32_t caller_owner_pid = Syscall_proc_current_pid(cpu_index, frame);
            if (caller_owner_pid == 0U)
                return 0ULL;

            char* kernel_buf = (char*) kmalloc(cap);
            if (!kernel_buf)
                return (uint64_t) -1;

            size_t copied = 0U;
            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.console_lock);
            int32_t route_slot = Syscall_console_route_find_locked(console_sid);
            if (route_slot >= 0)
            {
                syscall_console_route_t* route = &Syscall_state.console_routes[(uint32_t) route_slot];
                if (route->owner_pid == caller_owner_pid &&
                    (route->flags & SYS_CONSOLE_ROUTE_FLAG_CAPTURE) != 0U)
                {
                    copied = Syscall_console_route_pop_locked(route, kernel_buf, cap);
                }
            }
            spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);

            if (copied == 0U)
            {
                kfree(kernel_buf);
                return 0ULL;
            }

            if (!Syscall_copy_to_user(user_buf, kernel_buf, copied))
            {
                kfree(kernel_buf);
                return (uint64_t) -1;
            }

            kfree(kernel_buf);
            return (uint64_t) copied;
        }

        case SYS_CONSOLE_ROUTE_INPUT_WRITE_SID:
        {
            uint32_t console_sid = (uint32_t) frame->rdi;
            const char* user_buf = (const char*) frame->rsi;
            size_t len = (size_t) frame->rdx;
            if (console_sid == 0U || !user_buf || len == 0U)
                return (uint64_t) -1;

            if (len > SYSCALL_CONSOLE_MAX_WRITE)
                len = SYSCALL_CONSOLE_MAX_WRITE;

            if (!Syscall_state.console_lock_ready)
                return (uint64_t) -1;

            uint32_t caller_owner_pid = Syscall_proc_current_pid(cpu_index, frame);
            if (caller_owner_pid == 0U)
                return (uint64_t) -1;

            char* kernel_buf = (char*) kmalloc(len);
            if (!kernel_buf)
                return (uint64_t) -1;

            if (!Syscall_copy_from_user(kernel_buf, user_buf, len))
            {
                kfree(kernel_buf);
                return (uint64_t) -1;
            }

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.console_lock);
            int32_t route_slot = Syscall_console_route_find_locked(console_sid);
            if (route_slot < 0)
            {
                spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
                kfree(kernel_buf);
                return (uint64_t) -1;
            }

            syscall_console_route_t* route = &Syscall_state.console_routes[(uint32_t) route_slot];
            if (route->owner_pid != caller_owner_pid || (route->flags & SYS_CONSOLE_ROUTE_FLAG_PTY_INPUT) == 0U)
            {
                spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
                kfree(kernel_buf);
                return (uint64_t) -1;
            }

            Syscall_console_route_input_push_locked(route, kernel_buf, len);
            spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);
            kfree(kernel_buf);
            return (uint64_t) len;
        }

        case SYS_CONSOLE_ROUTE_INPUT_READ:
        {
            char* user_buf = (char*) frame->rdi;
            size_t cap = (size_t) frame->rsi;
            if (!user_buf || cap == 0U)
                return 0ULL;

            if (cap > SYSCALL_CONSOLE_MAX_WRITE)
                cap = SYSCALL_CONSOLE_MAX_WRITE;

            if (!Syscall_state.console_lock_ready)
                return 0ULL;

            uint32_t console_sid = Syscall_proc_current_console_sid(cpu_index, frame);
            if (console_sid == 0U)
                return 0ULL;

            char* kernel_buf = (char*) kmalloc(cap);
            if (!kernel_buf)
                return (uint64_t) -1;

            size_t copied = 0U;
            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.console_lock);
            int32_t route_slot = Syscall_console_route_find_locked(console_sid);
            if (route_slot >= 0)
            {
                syscall_console_route_t* route = &Syscall_state.console_routes[(uint32_t) route_slot];
                if ((route->flags & SYS_CONSOLE_ROUTE_FLAG_PTY_INPUT) != 0U)
                    copied = Syscall_console_route_input_pop_locked(route, kernel_buf, cap);
            }
            spin_unlock_irqrestore(&Syscall_state.console_lock, lock_flags);

            if (copied == 0U)
            {
                kfree(kernel_buf);
                return 0ULL;
            }

            if (!Syscall_copy_to_user(user_buf, kernel_buf, copied))
            {
                kfree(kernel_buf);
                return (uint64_t) -1;
            }

            kfree(kernel_buf);
            return (uint64_t) copied;
        }

        case SYS_EXIT:
        {
            int64_t status = (int64_t) frame->rdi;
            kdebug_printf("[USER] exit status=%lld\n", (long long) status);
            if (!Syscall_state.proc_lock_ready)
                return (uint64_t) status;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
            if (slot >= 0)
            {
                Syscall_state.procs[slot].exiting = true;
                Syscall_state.procs[slot].terminated_by_signal = false;
                Syscall_state.procs[slot].exit_status = status;
                Syscall_state.procs[slot].thread_exit_value = (uint64_t) status;
                Syscall_state.procs[slot].term_signal = 0;
            }
            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
            return (uint64_t) status;
        }

        case SYS_FORK:
        {
            if (!Syscall_state.proc_lock_ready)
                return (uint64_t) -1;

            uint32_t parent_pid = 0;
            uintptr_t parent_cr3 = 0;
            uintptr_t parent_fs_base = 0;
            uint32_t parent_console_sid = 0;
            uint32_t parent_domain = SYS_PROC_DOMAIN_USERLAND;
            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t parent_slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
            if (parent_slot < 0)
            {
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                return (uint64_t) -1;
            }

            syscall_process_t* parent = &Syscall_state.procs[parent_slot];
            if (Syscall_proc_owner_has_other_live_non_thread_locked(parent->owner_pid, parent_slot))
            {
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                return (uint64_t) -1;
            }

            parent_pid = parent->owner_pid;
            parent_cr3 = parent->cr3_phys;
            parent_fs_base = parent->fs_base;
            parent_console_sid = parent->console_sid;
            parent_domain = parent->domain;
            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

            uintptr_t child_cr3 = 0;
            if (!Syscall_clone_address_space(parent_cr3, &child_cr3))
                return (uint64_t) -1;

            lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t child_slot = Syscall_proc_alloc_locked();
            if (child_slot < 0)
            {
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                Syscall_free_address_space(child_cr3);
                return (uint64_t) -1;
            }

            uint32_t child_pid = Syscall_state.next_pid++;
            uint32_t child_console_sid = child_pid;
            if (Syscall_console_route_exists_for_sid(parent_console_sid))
                child_console_sid = parent_console_sid;
            syscall_process_t* child = &Syscall_state.procs[child_slot];
            memset(child, 0, sizeof(*child));
            Syscall_idle_claimed_slots[(uint32_t) child_slot] = 0U;
            child->used = true;
            child->exiting = false;
            child->terminated_by_signal = false;
            child->owns_cr3 = true;
            child->is_thread = false;
            child->pid = child_pid;
            child->ppid = parent_pid;
            child->owner_pid = child_pid;
            child->console_sid = child_console_sid;
            child->domain = parent_domain;
            child->exit_status = 0;
            child->thread_exit_value = 0;
            child->term_signal = 0;
            child->cr3_phys = child_cr3;
            child->fs_base = parent_fs_base;
            child->rax = 0;
            child->rcx = 0;
            child->rdx = frame->rdx;
            child->rsi = frame->rsi;
            child->rdi = frame->rdi;
            child->r8 = frame->r8;
            child->r9 = frame->r9;
            child->r10 = frame->r10;
            child->r11 = 0;
            child->r15 = frame->r15;
            child->r14 = frame->r14;
            child->r13 = frame->r13;
            child->r12 = frame->r12;
            child->rbp = frame->rbp;
            child->rbx = frame->rbx;
            child->rip = frame->rip;
            child->rflags = frame->rflags | SYSCALL_RFLAGS_IF;
            child->rsp = frame->rsp;
            child->pending_rax = 0;
            child->last_cpu = cpu_index;
            if (cpu_index < 256)
                Syscall_state.cpu_need_resched[cpu_index] = 1;
            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

            return (uint64_t) child_pid;
        }

        case SYS_EXECVE:
        {
            char path[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_read_user_cstr(path, sizeof(path), (const char*) frame->rdi))
                return (uint64_t) -1;
            uint32_t exec_domain = Syscall_process_domain_from_exec_path(path);

            if (Syscall_state.proc_lock_ready)
            {
                uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
                int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
                if (slot < 0)
                {
                    spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                    return (uint64_t) -1;
                }

                syscall_process_t* proc = &Syscall_state.procs[slot];
                if (Syscall_proc_owner_has_other_live_locked(proc->owner_pid, slot))
                {
                    spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                    return (uint64_t) -1;
                }
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
            }

            char* argv_copy[SYSCALL_EXEC_MAX_ARGS];
            char* envp_copy[SYSCALL_EXEC_MAX_ENVP];
            size_t argc_copy = 0;
            size_t envc_copy = 0;
            bool argv_ok = Syscall_exec_read_user_vec((const char* const*) frame->rsi,
                                                      argv_copy,
                                                      SYSCALL_EXEC_MAX_ARGS,
                                                      &argc_copy);
            if (!argv_ok)
            {
                Syscall_exec_free_vec(argv_copy, argc_copy);
                return (uint64_t) -1;
            }

            bool envp_ok = Syscall_exec_read_user_vec((const char* const*) frame->rdx,
                                                      envp_copy,
                                                      SYSCALL_EXEC_MAX_ENVP,
                                                      &envc_copy);
            if (!envp_ok)
            {
                Syscall_exec_free_vec(envp_copy, envc_copy);
                Syscall_exec_free_vec(argv_copy, argc_copy);
                return (uint64_t) -1;
            }

            uintptr_t new_cr3 = 0;
            uintptr_t new_entry = 0;
            uintptr_t new_rsp = 0;
            if (!Syscall_execve_build_address_space(path, &new_cr3, &new_entry, &new_rsp))
            {
                Syscall_exec_free_vec(envp_copy, envc_copy);
                Syscall_exec_free_vec(argv_copy, argc_copy);
                return (uint64_t) -1;
            }

            bool stack_ok = Syscall_exec_install_initial_stack(new_cr3,
                                                               &new_rsp,
                                                               path,
                                                               argv_copy,
                                                               argc_copy,
                                                               envp_copy,
                                                               envc_copy);
            Syscall_exec_free_vec(envp_copy, envc_copy);
            Syscall_exec_free_vec(argv_copy, argc_copy);
            if (!stack_ok)
            {
                Syscall_free_address_space(new_cr3);
                return (uint64_t) -1;
            }

            uintptr_t old_cr3 = 0;
            bool free_old = false;
            if (Syscall_state.proc_lock_ready)
            {
                uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
                int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
                if (slot >= 0)
                {
                    syscall_process_t* proc = &Syscall_state.procs[slot];
                    old_cr3 = proc->cr3_phys;
                    free_old = proc->owns_cr3 && old_cr3 != 0 && old_cr3 != new_cr3;
                    proc->cr3_phys = new_cr3;
                    proc->owns_cr3 = true;
                    proc->is_thread = false;
                    proc->owner_pid = proc->pid;
                    proc->domain = exec_domain;
                    proc->exiting = false;
                    proc->terminated_by_signal = false;
                    proc->exit_status = 0;
                    proc->thread_exit_value = 0;
                    proc->term_signal = 0;
                    proc->rax = 0;
                    proc->rcx = 0;
                    proc->rdx = 0;
                    proc->rsi = 0;
                    proc->rdi = 0;
                    proc->r8 = 0;
                    proc->r9 = 0;
                    proc->r10 = 0;
                    proc->r11 = 0;
                    proc->r15 = 0;
                    proc->r14 = 0;
                    proc->r13 = 0;
                    proc->r12 = 0;
                    proc->rbp = 0;
                    proc->rbx = 0;
                    proc->rip = new_entry;
                    proc->rflags = frame->rflags | SYSCALL_RFLAGS_IF;
                    proc->rsp = new_rsp;
                    proc->pending_rax = 0;
                    proc->fs_base = 0;
                }
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
            }

            frame->rdx = 0;
            frame->rsi = 0;
            frame->rdi = 0;
            frame->r8 = 0;
            frame->r9 = 0;
            frame->r10 = 0;
            frame->r15 = 0;
            frame->r14 = 0;
            frame->r13 = 0;
            frame->r12 = 0;
            frame->rbp = 0;
            frame->rbx = 0;
            frame->rip = new_entry;
            frame->rsp = new_rsp;
            Syscall_write_cr3_phys(new_cr3);
            Syscall_write_fs_base(0);
            if (free_old)
                Syscall_free_address_space(old_cr3);

            return 0;
        }

        case SYS_YIELD:
            if (cpu_index < 256)
            {
                if (Syscall_state.proc_lock_ready)
                {
                    uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
                    int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
                    if (slot >= 0 && Syscall_proc_has_other_owner_peer_locked(slot))
                        __atomic_store_n(&Syscall_state.cpu_yield_same_owner_pick[cpu_index], 1, __ATOMIC_RELEASE);
                    spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                }
                __atomic_store_n(&Syscall_state.cpu_need_resched[cpu_index], 1, __ATOMIC_RELEASE);
            }
            return 0;

        case SYS_MAP:
        {
            uintptr_t requested = (uintptr_t) frame->rdi;
            size_t len = (size_t) frame->rsi;
            uint64_t prot = frame->rdx;
            uint64_t map_flags = frame->r10;
            int64_t map_fd = (int64_t) frame->r8;
            uint64_t map_offset = frame->r9;
            const uint64_t prot_mask = SYS_PROT_READ | SYS_PROT_WRITE | SYS_PROT_EXEC;
            if (len == 0 || (prot & SYS_PROT_READ) == 0 || (prot & ~prot_mask) != 0)
                return (uint64_t) -1;
            if (len > ((size_t) -1 - (SYSCALL_PAGE_SIZE - 1U)))
                return (uint64_t) -1;

            size_t page_count = (len + (SYSCALL_PAGE_SIZE - 1U)) / SYSCALL_PAGE_SIZE;
            if (page_count == 0 || page_count > SYSCALL_MAP_MAX_PAGES)
                return (uint64_t) -1;

            bool legacy_map = (map_flags == 0 && map_fd == 0 && map_offset == 0);
            if (legacy_map)
            {
                map_flags = SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS;
                map_fd = -1;
            }

            const uint64_t supported_map_flags = SYS_MAP_PRIVATE | SYS_MAP_SHARED | SYS_MAP_ANONYMOUS;
            if ((map_flags & ~supported_map_flags) != 0)
                return (uint64_t) -1;

            bool is_private = (map_flags & SYS_MAP_PRIVATE) != 0;
            bool is_shared = (map_flags & SYS_MAP_SHARED) != 0;
            bool is_anon = (map_flags & SYS_MAP_ANONYMOUS) != 0;
            if (is_private == is_shared)
                return (uint64_t) -1;
            if (is_anon)
            {
                if (map_fd != -1 || map_offset != 0)
                    return (uint64_t) -1;
            }
            else
            {
                if (map_fd < 0)
                    return (uint64_t) -1;
                if ((map_offset & (SYSCALL_PAGE_SIZE - 1U)) != 0)
                    return (uint64_t) -1;
            }

            if (!Syscall_state.vm_lock_ready)
                return (uint64_t) -1;

            spin_lock(&Syscall_state.vm_lock);
            uint64_t ret = (uint64_t) -1;
            size_t map_size = page_count * SYSCALL_PAGE_SIZE;
            uintptr_t base = 0;
            if (requested == 0)
            {
                if (!Syscall_pick_user_map_base(page_count, &base))
                    goto map_out;
            }
            else
            {
                if ((requested & (SYSCALL_PAGE_SIZE - 1U)) != 0)
                    goto map_out;
                if (!Syscall_mmap_window_in_bounds(requested, map_size))
                    goto map_out;

                // UNIX-like hint behavior: when addr is non-null but MAP_FIXED is not requested,
                // prefer the hinted address and fall back to another free range in the mmap window.
                if (Syscall_user_range_unmapped(requested, map_size))
                {
                    base = requested;
                }
                else
                {
                    if (!Syscall_pick_user_map_base_from_hint(page_count, requested, &base))
                        goto map_out;
                }
            }

            bool writable = (prot & SYS_PROT_WRITE) != 0;
            bool executable = (prot & SYS_PROT_EXEC) != 0;
            if (writable && executable)
                goto map_out;
            uintptr_t set_bits = 0;
            uintptr_t clear_bits = 0;
            if (!writable)
                clear_bits |= WRITABLE;
            if (!executable)
                set_bits |= NO_EXECUTE;
            else
                clear_bits |= NO_EXECUTE;

            if (is_anon)
            {
                size_t mapped_pages = 0;
                for (size_t i = 0; i < page_count; i++)
                {
                    uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                    uintptr_t phys = (uintptr_t) PMM_alloc_page();
                    if (phys == 0)
                        break;

                    VMM_map_user_page(virt, phys);
                    mapped_pages++;
                    memset((void*) virt, 0, SYSCALL_PAGE_SIZE);

                    if ((set_bits | clear_bits) != 0 &&
                        !VMM_update_page_flags(virt, set_bits, clear_bits))
                        break;
                }

                if (mapped_pages != page_count)
                {
                    for (size_t i = 0; i < mapped_pages; i++)
                    {
                        uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                        uintptr_t phys = 0;
                        if (VMM_unmap_page(virt, &phys) && phys != 0)
                            PMM_dealloc_page((void*) phys);
                    }
                    goto map_out;
                }
            }
            else
            {
                uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
                if (owner_pid == 0 || (uint64_t) map_fd >= SYSCALL_MAX_OPEN_FILES)
                    goto map_out;

                uint32_t entry_type = SYSCALL_FD_TYPE_NONE;
                uint32_t dmabuf_id = 0;
                const uint8_t* regular_data = NULL;
                size_t regular_size = 0;

                if (!Syscall_state.fd_lock_ready)
                    goto map_out;

                spin_lock(&Syscall_state.fd_lock);
                syscall_file_desc_t* map_entry = &Syscall_state.fds[(uint32_t) map_fd];
                if (!map_entry->used || map_entry->owner_pid != owner_pid || map_entry->io_busy)
                {
                    spin_unlock(&Syscall_state.fd_lock);
                    goto map_out;
                }

                entry_type = map_entry->type;
                if (entry_type == SYSCALL_FD_TYPE_DMABUF)
                {
                    dmabuf_id = map_entry->drm_dmabuf_id;
                    spin_unlock(&Syscall_state.fd_lock);
                }
                else if (entry_type == SYSCALL_FD_TYPE_REGULAR)
                {
                    if (!is_private)
                    {
                        spin_unlock(&Syscall_state.fd_lock);
                        goto map_out;
                    }
                    if (!map_entry->can_read || !map_entry->data)
                    {
                        spin_unlock(&Syscall_state.fd_lock);
                        goto map_out;
                    }

                    regular_data = map_entry->data;
                    regular_size = map_entry->size;
                    map_entry->io_busy = true;
                    spin_unlock(&Syscall_state.fd_lock);
                }
                else
                {
                    spin_unlock(&Syscall_state.fd_lock);
                    goto map_out;
                }

                if (entry_type == SYSCALL_FD_TYPE_DMABUF)
                {
                    if (!is_shared || dmabuf_id == 0)
                        goto map_out;

                    uint64_t dmabuf_size = 0;
                    uint32_t dmabuf_pages = 0;
                    if (!DRM_dmabuf_get_layout(dmabuf_id, &dmabuf_size, &dmabuf_pages))
                        goto map_out;
                    uint64_t request_len = (uint64_t) len;
                    if (request_len > (uint64_t) -1 - map_offset)
                        goto map_out;
                    uint64_t request_end = map_offset + request_len;
                    if (request_end > dmabuf_size)
                        goto map_out;

                    if ((uint64_t) map_size > (uint64_t) -1 - map_offset)
                        goto map_out;
                    uint64_t map_end = map_offset + (uint64_t) map_size;
                    uint64_t dmabuf_mappable_end = (uint64_t) dmabuf_pages * (uint64_t) SYSCALL_PAGE_SIZE;
                    if (map_end > dmabuf_mappable_end)
                        goto map_out;

                    size_t mapped_pages = 0;
                    uint32_t start_page = (uint32_t) (map_offset / SYSCALL_PAGE_SIZE);
                    for (size_t i = 0; i < page_count; i++)
                    {
                        uint32_t page_index = start_page + (uint32_t) i;
                        if (page_index >= dmabuf_pages)
                            break;

                        uintptr_t phys = 0;
                        if (!DRM_dmabuf_get_page_phys(dmabuf_id, page_index, &phys) || phys == 0)
                            break;

                        uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                        VMM_map_page_flags(virt, phys, USER_MODE);
                        mapped_pages++;

                        uint64_t* pte = Syscall_get_user_pte_ptr(Syscall_read_cr3_phys(), virt);
                        if (!pte)
                            break;
                        *pte |= SYSCALL_PTE_DMABUF;

                        if ((set_bits | clear_bits) != 0 &&
                            !VMM_update_page_flags(virt, set_bits, clear_bits))
                            break;
                    }

                    if (mapped_pages != page_count)
                    {
                        for (size_t i = 0; i < mapped_pages; i++)
                        {
                            uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                            (void) VMM_unmap_page(virt, NULL);
                        }
                        goto map_out;
                    }

                    if (!DRM_dmabuf_ref_map_pages(dmabuf_id, (uint32_t) page_count))
                    {
                        for (size_t i = 0; i < mapped_pages; i++)
                        {
                            uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                            (void) VMM_unmap_page(virt, NULL);
                        }
                        goto map_out;
                    }
                }
                else
                {
                    bool clear_io_busy = true;
                    uint64_t request_len = (uint64_t) len;
                    if (request_len > (uint64_t) -1 - map_offset)
                        goto map_regular_out;

                    uint64_t request_end = map_offset + request_len;
                    if (request_end > (uint64_t) regular_size)
                        goto map_regular_out;

                    size_t mapped_pages = 0;
                    for (size_t i = 0; i < page_count; i++)
                    {
                        uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                        uintptr_t phys = (uintptr_t) PMM_alloc_page();
                        if (phys == 0)
                            break;

                        VMM_map_user_page(virt, phys);
                        mapped_pages++;
                        memset((void*) virt, 0, SYSCALL_PAGE_SIZE);

                        uint64_t page_off = map_offset + (uint64_t) (i * SYSCALL_PAGE_SIZE);
                        if (page_off < (uint64_t) regular_size)
                        {
                            uint64_t remain64 = (uint64_t) regular_size - page_off;
                            size_t copy_size = (remain64 > SYSCALL_PAGE_SIZE) ? SYSCALL_PAGE_SIZE : (size_t) remain64;
                            memcpy((void*) virt, regular_data + (size_t) page_off, copy_size);
                        }

                        if ((set_bits | clear_bits) != 0 &&
                            !VMM_update_page_flags(virt, set_bits, clear_bits))
                            break;
                    }

                    if (mapped_pages != page_count)
                    {
                        for (size_t i = 0; i < mapped_pages; i++)
                        {
                            uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                            uintptr_t phys = 0;
                            if (VMM_unmap_page(virt, &phys) && phys != 0)
                                PMM_dealloc_page((void*) phys);
                        }
                        goto map_regular_out;
                    }

                    clear_io_busy = false;
                    ret = (uint64_t) base;

map_regular_out:
                    if (Syscall_state.fd_lock_ready)
                    {
                        spin_lock(&Syscall_state.fd_lock);
                        map_entry = &Syscall_state.fds[(uint32_t) map_fd];
                        if (map_entry->used &&
                            map_entry->owner_pid == owner_pid &&
                            map_entry->type == SYSCALL_FD_TYPE_REGULAR)
                        {
                            map_entry->io_busy = false;
                        }
                        spin_unlock(&Syscall_state.fd_lock);
                    }
                    if (clear_io_busy)
                        goto map_out;
                    goto map_out;
                }
            }
            ret = (uint64_t) base;

map_out:
            spin_unlock(&Syscall_state.vm_lock);
            return ret;
        }

        case SYS_UNMAP:
        {
            uintptr_t base = (uintptr_t) frame->rdi;
            size_t len = (size_t) frame->rsi;
            if (len == 0 || (base & (SYSCALL_PAGE_SIZE - 1U)) != 0)
                return (uint64_t) -1;
            if (len > ((size_t) -1 - (SYSCALL_PAGE_SIZE - 1U)))
                return (uint64_t) -1;

            size_t page_count = (len + (SYSCALL_PAGE_SIZE - 1U)) / SYSCALL_PAGE_SIZE;
            if (page_count == 0 || page_count > SYSCALL_MAP_MAX_PAGES)
                return (uint64_t) -1;
            if (!Syscall_state.vm_lock_ready)
                return (uint64_t) -1;

            spin_lock(&Syscall_state.vm_lock);
            uint64_t ret = (uint64_t) -1;
            size_t map_size = page_count * SYSCALL_PAGE_SIZE;
            if (!Syscall_mmap_window_in_bounds(base, map_size))
                goto unmap_out;

            for (size_t i = 0; i < page_count; i++)
            {
                uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                if (!VMM_is_user_accessible(virt))
                    goto unmap_out;
            }

            for (size_t i = 0; i < page_count; i++)
            {
                uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                uintptr_t phys = 0;
                bool was_cow = false;
                bool was_dmabuf = false;
                uint64_t* pte = Syscall_get_user_pte_ptr(Syscall_read_cr3_phys(), virt);
                if (pte && ((*pte & SYSCALL_PTE_COW) != 0))
                    was_cow = true;
                if (pte && ((*pte & SYSCALL_PTE_DMABUF) != 0))
                    was_dmabuf = true;
                if (!VMM_unmap_page(virt, &phys))
                    goto unmap_out;
                if (phys != 0)
                {
                    if (was_dmabuf)
                    {
                        (void) DRM_dmabuf_unref_map_pages_by_phys(phys, 1U);
                    }
                    else if (was_cow)
                    {
                        bool ref_zero = false;
                        if (Syscall_cow_ref_sub(phys, &ref_zero) && ref_zero)
                            PMM_dealloc_page((void*) phys);
                    }
                    else
                        PMM_dealloc_page((void*) phys);
                }
            }
            ret = 0;

unmap_out:
            spin_unlock(&Syscall_state.vm_lock);
            return ret;
        }

        case SYS_MPROTECT:
        {
            uintptr_t base = (uintptr_t) frame->rdi;
            size_t len = (size_t) frame->rsi;
            uint64_t prot = frame->rdx;
            const uint64_t prot_mask = SYS_PROT_READ | SYS_PROT_WRITE | SYS_PROT_EXEC;
            if (len == 0 || (base & (SYSCALL_PAGE_SIZE - 1U)) != 0 ||
                (prot & SYS_PROT_READ) == 0 || (prot & ~prot_mask) != 0)
                return (uint64_t) -1;
            if (len > ((size_t) -1 - (SYSCALL_PAGE_SIZE - 1U)))
                return (uint64_t) -1;

            size_t page_count = (len + (SYSCALL_PAGE_SIZE - 1U)) / SYSCALL_PAGE_SIZE;
            if (page_count == 0 || page_count > SYSCALL_MAP_MAX_PAGES)
                return (uint64_t) -1;
            if (!Syscall_state.vm_lock_ready)
                return (uint64_t) -1;

            spin_lock(&Syscall_state.vm_lock);
            uint64_t ret = (uint64_t) -1;
            size_t map_size = page_count * SYSCALL_PAGE_SIZE;
            if (!Syscall_mmap_window_in_bounds(base, map_size))
                goto mprotect_out;

            for (size_t i = 0; i < page_count; i++)
            {
                uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                if (!VMM_is_user_accessible(virt))
                    goto mprotect_out;
            }

            bool writable = (prot & SYS_PROT_WRITE) != 0;
            bool executable = (prot & SYS_PROT_EXEC) != 0;
            if (writable && executable)
                goto mprotect_out;
            uintptr_t set_bits = writable ? WRITABLE : 0;
            uintptr_t clear_bits = writable ? 0 : WRITABLE;
            if (!executable)
                set_bits |= NO_EXECUTE;
            else
                clear_bits |= NO_EXECUTE;

            if (writable)
            {
                uintptr_t current_cr3 = Syscall_read_cr3_phys();
                for (size_t i = 0; i < page_count; i++)
                {
                    uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                    uint64_t* pte = Syscall_get_user_pte_ptr(current_cr3, virt);
                    if (pte && ((*pte & SYSCALL_PTE_COW) != 0))
                        goto mprotect_out;
                }
            }
            for (size_t i = 0; i < page_count; i++)
            {
                uintptr_t virt = base + (i * SYSCALL_PAGE_SIZE);
                if (!VMM_update_page_flags(virt, set_bits, clear_bits))
                    goto mprotect_out;
            }
            ret = 0;

mprotect_out:
            spin_unlock(&Syscall_state.vm_lock);
            return ret;
        }

        case SYS_OPEN:
            return Syscall_handle_open(cpu_index, frame);

        case SYS_CLOSE:
            return Syscall_handle_close(cpu_index, frame);

        case SYS_READ:
            return Syscall_handle_read(cpu_index, frame);

        case SYS_WRITE:
            return Syscall_handle_write(cpu_index, frame);

        case SYS_LSEEK:
            return Syscall_handle_lseek(cpu_index, frame);

        case SYS_IOCTL:
            return Syscall_handle_ioctl(cpu_index, frame);

        case SYS_SOCKET:
            return Syscall_handle_socket(cpu_index, frame);

        case SYS_BIND:
            return Syscall_handle_bind(cpu_index, frame);

        case SYS_CONNECT:
            return Syscall_handle_connect(cpu_index, frame);

        case SYS_LISTEN:
            return Syscall_handle_listen(cpu_index, frame);

        case SYS_ACCEPT:
            return Syscall_handle_accept(cpu_index, frame);

        case SYS_SENDTO:
            return Syscall_handle_sendto(cpu_index, frame);

        case SYS_RECVFROM:
            return Syscall_handle_recvfrom(cpu_index, frame);

        case SYS_GETSOCKNAME:
            return Syscall_handle_getsockname(cpu_index, frame);

        case SYS_GETPEERNAME:
            return Syscall_handle_getpeername(cpu_index, frame);

        case SYS_KBD_GET_SCANCODE:
        {
            uint32_t caller_pid = Syscall_proc_current_pid(cpu_index, frame);
            uint8_t injected = 0U;
            if (caller_pid != 0U && Syscall_kbd_inject_pop_for_pid(caller_pid, &injected))
                return (uint64_t) injected;
            if (caller_pid != 0U && Syscall_kbd_inject_is_target(caller_pid))
            {
                Syscall_kbd_inject_target_hits++;
                return 0ULL;
            }
            if (Syscall_kbd_hardware_capture_pid != 0U && caller_pid != Syscall_kbd_hardware_capture_pid)
                return 0ULL;
            return (uint64_t) Keyboard_get_scancode();
        }

        case SYS_KBD_CAPTURE_SET:
        {
            uint32_t caller_pid = Syscall_proc_current_pid(cpu_index, frame);
            uint32_t want = (uint32_t) frame->rdi;
            if (caller_pid == 0U)
                return (uint64_t) -1;
            if (want == 0U)
            {
                if (Syscall_kbd_hardware_capture_pid == caller_pid)
                    Syscall_kbd_hardware_capture_pid = 0U;
                return 0ULL;
            }
            if (want != caller_pid)
                return (uint64_t) -1;
            Syscall_kbd_hardware_capture_pid = want;
            return 0ULL;
        }

        case SYS_KBD_INJECT_SCANCODE:
        {
            uint32_t target_pid = (uint32_t) frame->rdi;
            uint8_t scancode = (uint8_t) (frame->rsi & 0xFFU);
            Syscall_kbd_inject_register_target(target_pid);
            if (!Syscall_kbd_inject_push(target_pid, scancode))
                return (uint64_t) -1;
            return 0ULL;
        }

        case SYS_MOUSE_GET_EVENT:
        {
            syscall_mouse_event_t* user_event = (syscall_mouse_event_t*) frame->rdi;
            if (!user_event)
                return (uint64_t) -1;

            syscall_mouse_event_t event;
            if (!Mouse_get_event(&event))
                return 0ULL;

            if (!Syscall_copy_to_user(user_event, &event, sizeof(event)))
                return (uint64_t) -1;

            return 1ULL;
        }

        case SYS_MOUSE_DEBUG_INFO_GET:
        {
            syscall_mouse_debug_info_t* user_info = (syscall_mouse_debug_info_t*) frame->rdi;
            if (!user_info)
                return (uint64_t) -1;

            syscall_mouse_debug_info_t info;
            memset(&info, 0, sizeof(info));
            if (!Mouse_get_debug_info(&info))
                return (uint64_t) -1;
            if (!Syscall_copy_to_user(user_info, &info, sizeof(info)))
                return (uint64_t) -1;
            return 0ULL;
        }

        case SYS_FS_ISDIR:
        {
            char path[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_read_user_cstr(path, sizeof(path), (const char*) frame->rdi))
                return (uint64_t) -1;

            bool is_dir = VFS_path_is_dir(path);
            return is_dir ? 1ULL : 0ULL;
        }

        case SYS_FS_READDIR:
        {
            char path[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_read_user_cstr(path, sizeof(path), (const char*) frame->rdi))
                return (uint64_t) -1;

            size_t index = (size_t) frame->rsi;
            syscall_dirent_t* user_out = (syscall_dirent_t*) frame->rdx;
            if (!user_out)
                return (uint64_t) -1;

            vfs_dirent_info_t info;
            memset(&info, 0, sizeof(info));

            bool ok = VFS_read_dirent_at(path, index, &info);

            if (!ok)
                return 0;

            syscall_dirent_t out;
            memset(&out, 0, sizeof(out));
            out.d_ino = info.inode;
            out.d_type = info.type;
            size_t name_len = strlen(info.name);
            if (name_len > SYS_DIRENT_NAME_MAX)
                name_len = SYS_DIRENT_NAME_MAX;
            memcpy(out.d_name, info.name, name_len);
            out.d_name[name_len] = '\0';

            if (!Syscall_copy_to_user(user_out, &out, sizeof(out)))
                return (uint64_t) -1;

            return 1;
        }

        case SYS_WAITPID:
        {
            if (!Syscall_state.proc_lock_ready)
                return (uint64_t) -1;

            int32_t wait_pid = (int32_t) frame->rdi;
            int* out_status = (int*) frame->rsi;
            int* out_signal = (int*) frame->rdx;
            uint32_t reaped_pid = 0;
            int64_t reaped_status = 0;
            int32_t reaped_signal = 0;
            bool has_child = false;
            bool got_event = false;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
            if (slot < 0)
            {
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                return (uint64_t) -1;
            }

            uint32_t parent_pid = Syscall_state.procs[slot].owner_pid;
            got_event = Syscall_exit_event_pop_locked(parent_pid,
                                                      wait_pid,
                                                      &reaped_pid,
                                                      &reaped_status,
                                                      &reaped_signal);
            if (!got_event)
                has_child = Syscall_proc_has_matching_child_locked(parent_pid, wait_pid);

            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

            if (!got_event)
            {
                if (!has_child)
                    return (uint64_t) -1;

                if (cpu_index < 256)
                    __atomic_store_n(&Syscall_state.cpu_need_resched[cpu_index], 1, __ATOMIC_RELEASE);
                return 0;
            }

            int status32 = (int) reaped_status;
            if (out_status && !Syscall_copy_to_user(out_status, &status32, sizeof(status32)))
                return (uint64_t) -1;
            if (out_signal && !Syscall_copy_to_user(out_signal, &reaped_signal, sizeof(reaped_signal)))
                return (uint64_t) -1;

            return (uint64_t) reaped_pid;
        }

        case SYS_KILL:
        {
            if (!Syscall_state.proc_lock_ready)
                return (uint64_t) -1;

            int32_t target_pid = (int32_t) frame->rdi;
            int32_t signal = (int32_t) frame->rsi;
            if (target_pid <= 0)
                return (uint64_t) -1;
            if (signal < 0)
                return (uint64_t) -1;
            if (signal != 0 && !Syscall_signal_is_valid(signal))
                return (uint64_t) -1;

            uint32_t sender_pid = 0;
            bool delivered = false;
            bool core_dump_not_implemented = false;
            bool stop_semantics_not_implemented = false;
            bool ignored = false;
            uint32_t target_owner_pid = 0;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t sender_slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
            if (sender_slot >= 0)
                sender_pid = Syscall_state.procs[sender_slot].pid;

            int32_t target_slot = -1;
            for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
            {
                if (!Syscall_state.procs[i].used || Syscall_state.procs[i].pid != (uint32_t) target_pid)
                    continue;
                target_slot = (int32_t) i;
                target_owner_pid = Syscall_state.procs[i].owner_pid;
                break;
            }

            if (target_slot >= 0 && target_owner_pid != 0)
            {
                if (signal == 0)
                {
                    delivered = true;
                }
                else
                {
                    char action = Syscall_signal_default_action(signal);
                    switch (action)
                    {
                        case 'E':
                            delivered = Syscall_signal_terminate_owner_locked(target_owner_pid, signal);
                            break;
                        case 'C':
                            delivered = Syscall_signal_terminate_owner_locked(target_owner_pid, signal);
                            core_dump_not_implemented = delivered;
                            break;
                        case 'I':
                            delivered = true;
                            ignored = true;
                            break;
                        case 'S':
                            delivered = true;
                            stop_semantics_not_implemented = true;
                            break;
                        default:
                            delivered = false;
                            break;
                    }
                }
            }

            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

            if (!delivered)
                return (uint64_t) -1;

            if (signal != 0)
            {
                const char* signal_name = Syscall_signal_name(signal);
                if (stop_semantics_not_implemented)
                {
                    kdebug_printf("[USER] pid=%u sent %s to pid=%d (stop semantics not implemented yet)\n",
                                  (unsigned int) sender_pid,
                                  signal_name,
                                  (int) target_pid);
                }
                else if (core_dump_not_implemented)
                {
                    kdebug_printf("[USER] pid=%u sent %s to pid=%d (core dump not implemented yet, process terminated)\n",
                                  (unsigned int) sender_pid,
                                  signal_name,
                                  (int) target_pid);
                }
                else if (ignored)
                {
                    kdebug_printf("[USER] pid=%u sent %s to pid=%d (ignored by default)\n",
                                  (unsigned int) sender_pid,
                                  signal_name,
                                  (int) target_pid);
                }
                else
                {
                    kdebug_printf("[USER] pid=%u sent %s to pid=%d\n",
                                  (unsigned int) sender_pid,
                                  signal_name,
                                  (int) target_pid);
                }
            }
            return 0;
        }

        case SYS_THREAD_CREATE:
        {
            if (!Syscall_state.proc_lock_ready)
                return (uint64_t) -1;

            uintptr_t start_rip = (uintptr_t) frame->rdi;
            uintptr_t start_arg = (uintptr_t) frame->rsi;
            uintptr_t stack_top = (uintptr_t) frame->rdx & ~(uintptr_t) 0xFULL;
            uintptr_t requested_fs_base = (uintptr_t) frame->r10;
            if (!Syscall_is_canonical_low(start_rip) ||
                start_rip < SYSCALL_USER_VADDR_MIN ||
                start_rip > SYSCALL_USER_VADDR_MAX)
            {
                return (uint64_t) -1;
            }

            if (stack_top <= (SYSCALL_USER_VADDR_MIN + 8U) || stack_top > SYSCALL_USER_VADDR_MAX)
                return (uint64_t) -1;
            if (!Syscall_user_fs_base_is_valid(requested_fs_base))
                return (uint64_t) -1;

            uintptr_t thread_rsp = stack_top - 8U;
            if (!Syscall_user_range_in_bounds(thread_rsp, sizeof(uint64_t)))
                return (uint64_t) -1;

            uint64_t fake_ret = 0;
            if (!Syscall_copy_to_user((void*) thread_rsp, &fake_ret, sizeof(fake_ret)))
                return (uint64_t) -1;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t parent_slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
            if (parent_slot < 0)
            {
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                return (uint64_t) -1;
            }

            syscall_process_t* parent = &Syscall_state.procs[(uint32_t) parent_slot];
            int32_t thread_slot = Syscall_proc_alloc_locked();
            if (thread_slot < 0)
            {
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                return (uint64_t) -1;
            }

            uint32_t tid = Syscall_state.next_pid++;
            syscall_process_t* thread = &Syscall_state.procs[(uint32_t) thread_slot];
            memset(thread, 0, sizeof(*thread));
            thread->used = true;
            thread->exiting = false;
            thread->terminated_by_signal = false;
            thread->owns_cr3 = false;
            thread->is_thread = true;
            thread->pid = tid;
            thread->ppid = parent->ppid;
            thread->owner_pid = parent->owner_pid;
            thread->console_sid = parent->console_sid;
            thread->domain = parent->domain;
            thread->exit_status = 0;
            thread->thread_exit_value = 0;
            thread->term_signal = 0;
            thread->cr3_phys = parent->cr3_phys;
            thread->fs_base = (requested_fs_base != 0) ? requested_fs_base : parent->fs_base;
            thread->rax = 0;
            thread->rcx = 0;
            thread->rdx = 0;
            thread->rsi = 0;
            thread->rdi = start_arg;
            thread->r8 = 0;
            thread->r9 = 0;
            thread->r10 = 0;
            thread->r11 = 0;
            thread->r15 = 0;
            thread->r14 = 0;
            thread->r13 = 0;
            thread->r12 = 0;
            thread->rbp = 0;
            thread->rbx = 0;
            thread->rip = start_rip;
            thread->rflags = parent->rflags | SYSCALL_RFLAGS_IF;
            thread->rsp = thread_rsp;
            thread->pending_rax = 0;
            thread->last_cpu = cpu_index;

            if (cpu_index < 256)
                __atomic_store_n(&Syscall_state.cpu_need_resched[cpu_index], 1, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
            return (uint64_t) tid;
        }

        case SYS_THREAD_JOIN:
        {
            if (!Syscall_state.proc_lock_ready)
                return (uint64_t) -1;

            int32_t tid = (int32_t) frame->rdi;
            uint64_t* out_retval = (uint64_t*) frame->rsi;
            if (tid <= 0)
                return (uint64_t) -1;

            uint32_t owner_pid = 0;
            uint32_t self_tid = 0;
            uint64_t thread_retval = 0;
            bool got_event = false;
            bool has_live_thread = false;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
            if (slot < 0)
            {
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                return (uint64_t) -1;
            }

            owner_pid = Syscall_state.procs[slot].owner_pid;
            self_tid = Syscall_state.procs[slot].pid;
            if ((uint32_t) tid == self_tid)
            {
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                return (uint64_t) -1;
            }

            got_event = Syscall_thread_exit_event_pop_locked(owner_pid, (uint32_t) tid, &thread_retval);
            if (!got_event)
                has_live_thread = Syscall_proc_has_live_thread_locked(owner_pid, (uint32_t) tid);
            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

            if (!got_event)
            {
                if (!has_live_thread)
                    return (uint64_t) -1;
                if (cpu_index < 256)
                    __atomic_store_n(&Syscall_state.cpu_need_resched[cpu_index], 1, __ATOMIC_RELEASE);
                return 0;
            }

            if (out_retval && !Syscall_copy_to_user(out_retval, &thread_retval, sizeof(thread_retval)))
                return (uint64_t) -1;
            return (uint64_t) tid;
        }

        case SYS_THREAD_EXIT:
        {
            uint64_t retval = frame->rdi;
            if (!Syscall_state.proc_lock_ready)
                return retval;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
            if (slot >= 0)
            {
                syscall_process_t* proc = &Syscall_state.procs[slot];
                proc->exiting = true;
                proc->terminated_by_signal = false;
                proc->exit_status = (int64_t) retval;
                proc->thread_exit_value = retval;
                proc->term_signal = 0;
            }
            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
            return retval;
        }

        case SYS_THREAD_SELF:
        {
            if (!Syscall_state.proc_lock_ready)
                return (uint64_t) -1;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
            uint64_t tid = (slot >= 0) ? Syscall_state.procs[slot].pid : (uint64_t) -1;
            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
            return tid;
        }

        case SYS_THREAD_SET_FSBASE:
        {
            uintptr_t fs_base = (uintptr_t) frame->rdi;
            if (!Syscall_user_fs_base_is_valid(fs_base) || !Syscall_state.proc_lock_ready)
                return (uint64_t) -1;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
            if (slot < 0)
            {
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                return (uint64_t) -1;
            }

            Syscall_state.procs[slot].fs_base = fs_base;
            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
            Syscall_write_fs_base(fs_base);
            return 0;
        }

        case SYS_THREAD_GET_FSBASE:
        {
            uintptr_t fs_base = Syscall_read_fs_base();
            if (!Syscall_state.proc_lock_ready)
                return (uint64_t) fs_base;

            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
            if (slot >= 0)
                Syscall_state.procs[slot].fs_base = fs_base;
            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
            return (uint64_t) fs_base;
        }

        case SYS_POWER:
        {
            uint32_t cmd = (uint32_t) frame->rdi;
            uint32_t arg = (uint32_t) frame->rsi;
            switch (cmd)
            {
                case SYS_POWER_CMD_SHUTDOWN:
                    return ACPI_shutdown() ? 0 : (uint64_t) -1;
                case SYS_POWER_CMD_SLEEP:
                    if (arg > (uint32_t) ACPI_SLEEP_S5)
                        return (uint64_t) -1;
                    return ACPI_sleep((ACPI_sleep_state_t) arg) ? 0 : (uint64_t) -1;
                case SYS_POWER_CMD_REBOOT:
                    return ACPI_reboot() ? 0 : (uint64_t) -1;
                default:
                    return (uint64_t) -1;
            }
        }

        case SYS_PIPE:
            return Syscall_handle_pipe(cpu_index, frame);

        case SYS_FUTEX:
            return Syscall_handle_futex(cpu_index, frame);

        case SYS_SHMGET:
            return Syscall_handle_shmget(cpu_index, frame);

        case SYS_SHMAT:
            return Syscall_handle_shmat(cpu_index, frame);

        case SYS_SHMDT:
            return Syscall_handle_shmdt(cpu_index, frame);

        case SYS_SHMCTL:
            return Syscall_handle_shmctl(cpu_index, frame);

        case SYS_MSGGET:
            return Syscall_handle_msgget(cpu_index, frame);

        case SYS_MSGSND:
            return Syscall_handle_msgsnd(cpu_index, frame);

        case SYS_MSGRCV:
            return Syscall_handle_msgrcv(cpu_index, frame);

        default:
        {
            if (Syscall_state.proc_lock_ready)
            {
                uint32_t pid = 0;
                bool delivered = false;

                uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
                int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
                if (slot >= 0)
                {
                    syscall_process_t* proc = &Syscall_state.procs[slot];
                    pid = proc->pid;

                    delivered = Syscall_signal_terminate_owner_locked(proc->owner_pid, SYS_SIGSYS);
                    if (!delivered)
                    {
                        proc->exiting = true;
                        proc->terminated_by_signal = true;
                        proc->term_signal = SYS_SIGSYS;
                        proc->exit_status = 128 + SYS_SIGSYS;
                        proc->thread_exit_value = 0;
                        delivered = true;
                    }
                }
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

                if (delivered)
                {
                    kdebug_printf("[USER] pid=%u killed by SIGSYS (unknown syscall=%llu)\n",
                                  (unsigned int) pid,
                                  (unsigned long long) syscall_num);
                }
            }

            return (uint64_t) -1;
        }
    }
}

uint64_t Syscall_interupt_handler(uint64_t syscall_num, syscall_frame_t* frame, uint32_t cpu_index)
{
    return Syscall_interrupt_handler(syscall_num, frame, cpu_index);
}

uint64_t Syscall_post_handler(uint64_t syscall_ret, syscall_frame_t* frame, uint32_t cpu_index)
{
    if (!frame || !Syscall_state.proc_lock_ready)
        return syscall_ret;

    task_cpu_local_t* post_cpu_local = task_get_cpu_local();
    uint8_t post_apic_id = APIC_get_current_lapic_id();
    if (post_cpu_local)
    {
        cpu_index = __atomic_load_n(&post_cpu_local->cpu_index, __ATOMIC_RELAXED);
        post_apic_id = __atomic_load_n(&post_cpu_local->apic_id, __ATOMIC_RELAXED);
    }

    if (cpu_index >= 256)
        cpu_index = post_apic_id;

    Syscall_debug_post_calls++;
    Syscall_debug_post_calls_cpu[cpu_index & 0x3U]++;

    uintptr_t old_cr3_to_free = 0;
    bool free_old_cr3 = false;
    bool cleanup_fds = false;
    uint32_t cleanup_pid = 0;
    uint32_t cleanup_ppid = 0;
    uint32_t cleanup_owner_pid = 0;
    uint32_t cleanup_console_sid = 0;
    uint32_t cleanup_tid = 0;
    int64_t cleanup_status = 0;
    int32_t cleanup_signal = 0;
    uint64_t cleanup_thread_value = 0;
    bool queue_exit_event = false;
    bool queue_thread_exit_event = false;
    bool cleanup_console_route = false;
    uint64_t ret_for_next = syscall_ret;
    uintptr_t next_cr3 = Syscall_read_cr3_phys();
    uintptr_t next_fs_base = Syscall_read_fs_base();

    uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    int32_t current_slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
    if (current_slot < 0)
    {
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        return syscall_ret;
    }

    syscall_process_t* current = &Syscall_state.procs[current_slot];
    if (!current->exiting)
    {
        current->rax = syscall_ret;
        current->rcx = 0;
        current->rdx = frame->rdx;
        current->rsi = frame->rsi;
        current->rdi = frame->rdi;
        current->r8 = frame->r8;
        current->r9 = frame->r9;
        current->r10 = frame->r10;
        current->r11 = 0;
        current->r15 = frame->r15;
        current->r14 = frame->r14;
        current->r13 = frame->r13;
        current->r12 = frame->r12;
        current->rbp = frame->rbp;
        current->rbx = frame->rbx;
        current->rip = frame->rip;
        current->rflags = frame->rflags | SYSCALL_RFLAGS_IF;
        current->rsp = frame->rsp;
        current->cr3_phys = Syscall_read_cr3_phys();
        current->fs_base = Syscall_read_fs_base();
        current->pending_rax = syscall_ret;
    }
    else
    {
        bool owner_has_other_live = Syscall_proc_owner_has_other_live_locked(current->owner_pid, current_slot);
        if (!owner_has_other_live && current->owns_cr3)
        {
            old_cr3_to_free = current->cr3_phys;
            free_old_cr3 = (old_cr3_to_free != 0);
        }

        cleanup_pid = current->owner_pid;
        cleanup_ppid = current->ppid;
        cleanup_owner_pid = current->owner_pid;
        cleanup_console_sid = current->console_sid;
        cleanup_tid = current->pid;
        cleanup_status = current->exit_status;
        cleanup_signal = current->terminated_by_signal ? current->term_signal : 0;
        cleanup_thread_value = current->thread_exit_value;
        queue_thread_exit_event = current->is_thread && cleanup_owner_pid != 0 && cleanup_tid != 0;
        queue_exit_event = (!owner_has_other_live && cleanup_ppid != 0 && cleanup_pid != 0);
        cleanup_fds = !owner_has_other_live;
        cleanup_console_route = false;
        Syscall_idle_claimed_slots[(uint32_t) current_slot] = 0U;
        memset(current, 0, sizeof(*current));
        Syscall_state.cpu_current_proc[cpu_index] = SYSCALL_PROC_NONE;
        current_slot = -1;
    }

    bool pick_next = (current_slot < 0);
    bool consumed_resched = __atomic_exchange_n(&Syscall_state.cpu_need_resched[cpu_index], 0, __ATOMIC_ACQ_REL) != 0;
    if (consumed_resched)
    {
        pick_next = true;
        Syscall_debug_flag_consume_post_cpu[cpu_index & 0x3U]++;
    }
    if (pick_next)
        Syscall_debug_post_pick_next++;

    bool same_owner_coop = false;
    if (consumed_resched)
    {
        bool yield_route = __atomic_exchange_n(&Syscall_state.cpu_yield_same_owner_pick[cpu_index], 0, __ATOMIC_ACQ_REL) != 0;
        same_owner_coop = yield_route && current_slot >= 0;
    }

    int32_t next_slot = current_slot;
    if (pick_next)
    {
        if (same_owner_coop)
            next_slot = Syscall_proc_pick_next_same_owner_locked(current_slot, cpu_index);
        else
            next_slot = Syscall_proc_pick_next_locked(current_slot, cpu_index);
    }
    if (next_slot < 0)
    {
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        if (free_old_cr3)
            Syscall_free_address_space(old_cr3_to_free);
        task_idle_loop();
        __builtin_unreachable();
    }

    Syscall_state.cpu_current_proc[cpu_index] = (uint32_t) next_slot;
    if (current_slot >= 0 && next_slot != current_slot)
    {
        Syscall_debug_post_switch++;
        Syscall_debug_post_switch_cpu[cpu_index & 0x3U]++;
    }
    Syscall_state.cpu_slice_ticks[cpu_index] = 0;
    syscall_process_t* next = &Syscall_state.procs[next_slot];
    next->last_cpu = cpu_index;
    frame->rdi = next->rdi;
    frame->rsi = next->rsi;
    frame->rdx = next->rdx;
    frame->r10 = next->r10;
    frame->r8 = next->r8;
    frame->r9 = next->r9;
    frame->r15 = next->r15;
    frame->r14 = next->r14;
    frame->r13 = next->r13;
    frame->r12 = next->r12;
    frame->rbp = next->rbp;
    frame->rbx = next->rbx;
    frame->rip = next->rip;
    frame->rflags = next->rflags | SYSCALL_RFLAGS_IF;
    frame->rsp = next->rsp;
    ret_for_next = next->pending_rax;
    next_cr3 = next->cr3_phys;
    next_fs_base = next->fs_base;
    if (queue_exit_event)
        Syscall_exit_event_push_locked(cleanup_ppid, cleanup_pid, cleanup_status, cleanup_signal);
    if (queue_thread_exit_event)
        Syscall_thread_exit_event_push_locked(cleanup_owner_pid, cleanup_tid, cleanup_thread_value);
    spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

    if (next_cr3 != Syscall_read_cr3_phys())
        Syscall_write_cr3_phys(next_cr3);
    if (next_fs_base != Syscall_read_fs_base())
        Syscall_write_fs_base(next_fs_base);
    if (cleanup_fds)
        Syscall_fd_cleanup_owner(cleanup_pid);
    if (cleanup_fds && cleanup_pid != 0U && Syscall_kbd_hardware_capture_pid == cleanup_pid)
        Syscall_kbd_hardware_capture_pid = 0U;
    if (cleanup_console_route)
        Syscall_console_route_clear_sid(cleanup_console_sid);
    if (free_old_cr3 && old_cr3_to_free != next_cr3)
        Syscall_free_address_space(old_cr3_to_free);

    return ret_for_next;
}

bool Syscall_handle_user_exception(interrupt_frame_t* frame, uintptr_t fault_addr)
{
    if (!frame || !Syscall_state.proc_lock_ready)
        return false;

    uint32_t cpu_index = 0;
    uint8_t apic_id = APIC_get_current_lapic_id();
    task_cpu_local_t* cpu_local = task_get_cpu_local();
    if (cpu_local)
    {
        cpu_index = __atomic_load_n(&cpu_local->cpu_index, __ATOMIC_RELAXED);
        apic_id = __atomic_load_n(&cpu_local->apic_id, __ATOMIC_RELAXED);
    }
    else
        cpu_index = apic_id;

    if (cpu_index >= 256)
        cpu_index = apic_id;

    if (frame->int_no == 14 &&
        Syscall_resolve_cow_fault(cpu_index, fault_addr, frame->err_code))
    {
        return true;
    }

    syscall_frame_t syscall_frame;
    memset(&syscall_frame, 0, sizeof(syscall_frame));
    syscall_frame.r15 = frame->r15;
    syscall_frame.r14 = frame->r14;
    syscall_frame.r13 = frame->r13;
    syscall_frame.r12 = frame->r12;
    syscall_frame.rbp = frame->rbp;
    syscall_frame.rbx = frame->rbx;
    syscall_frame.rdi = frame->rdi;
    syscall_frame.rsi = frame->rsi;
    syscall_frame.rdx = frame->rdx;
    syscall_frame.r10 = frame->rcx;
    syscall_frame.r8 = frame->r8;
    syscall_frame.r9 = frame->r9;
    syscall_frame.rip = frame->rip;
    syscall_frame.rflags = frame->rflags;
    syscall_frame.rsp = frame->rsp;

    uint32_t pid = 0;
    uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    int32_t slot = Syscall_proc_ensure_current_locked(cpu_index, &syscall_frame);
    if (slot < 0)
    {
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        return false;
    }

    int32_t signal = Syscall_user_exception_signal_num(frame->int_no);
    syscall_process_t* proc = &Syscall_state.procs[slot];
    pid = proc->pid;
    uint32_t owner_pid = proc->owner_pid;
    char action = Syscall_signal_default_action(signal);
    bool terminated = false;
    bool core_dump_not_implemented = false;

    if (action == 'E' || action == 'C')
    {
        terminated = Syscall_signal_terminate_owner_locked(owner_pid, signal);
        core_dump_not_implemented = (action == 'C');
    }

    if (!terminated)
    {
        proc->exiting = true;
        proc->terminated_by_signal = true;
        proc->term_signal = signal;
        proc->exit_status = 128 + signal;
    }
    spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

    const char* signal_name = Syscall_signal_name(signal);
    if (frame->int_no == 14)
    {
        if (core_dump_not_implemented)
        {
            kdebug_printf("[USER] pid=%u killed by %s (#PF) rip=0x%llX err=0x%llX cr2=0x%llX (core dump not implemented yet)\n",
                          (unsigned int) pid,
                          signal_name,
                          (unsigned long long) frame->rip,
                          (unsigned long long) frame->err_code,
                          (unsigned long long) fault_addr);
        }
        else
        {
            kdebug_printf("[USER] pid=%u killed by %s (#PF) rip=0x%llX err=0x%llX cr2=0x%llX\n",
                          (unsigned int) pid,
                          signal_name,
                          (unsigned long long) frame->rip,
                          (unsigned long long) frame->err_code,
                          (unsigned long long) fault_addr);
        }
    }
    else
    {
        if (core_dump_not_implemented)
        {
            kdebug_printf("[USER] pid=%u killed by %s (vec=%llu) rip=0x%llX err=0x%llX (core dump not implemented yet)\n",
                          (unsigned int) pid,
                          signal_name,
                          (unsigned long long) frame->int_no,
                          (unsigned long long) frame->rip,
                          (unsigned long long) frame->err_code);
        }
        else
        {
            kdebug_printf("[USER] pid=%u killed by %s (vec=%llu) rip=0x%llX err=0x%llX\n",
                          (unsigned int) pid,
                          signal_name,
                          (unsigned long long) frame->int_no,
                          (unsigned long long) frame->rip,
                          (unsigned long long) frame->err_code);
        }
    }

    uint64_t next_rax = Syscall_post_handler(frame->rax, &syscall_frame, cpu_index);
    frame->r15 = syscall_frame.r15;
    frame->r14 = syscall_frame.r14;
    frame->r13 = syscall_frame.r13;
    frame->r12 = syscall_frame.r12;
    frame->rbp = syscall_frame.rbp;
    frame->rbx = syscall_frame.rbx;
    frame->rip = syscall_frame.rip;
    frame->rflags = syscall_frame.rflags;
    frame->rsp = syscall_frame.rsp;
    frame->rax = next_rax;
    return true;
}
