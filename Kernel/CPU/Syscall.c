#include <CPU/Syscall.h>
#include <CPU/Syscall_private.h>

#include <CPU/ACPI.h>
#include <CPU/APIC.h>
#include <CPU/ISR.h>
#include <CPU/MSR.h>
#include <CPU/SMP.h>
#include <Device/HPET.h>
#include <Device/Keyboard.h>
#include <Debug/KDebug.h>
#include <Debug/Spinlock.h>
#include <FileSystem/ext4.h>
#include <Memory/KMem.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>
#include <Storage/AHCI.h>
#include <Task/RCU.h>
#include <Task/Task.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static syscall_runtime_state_t Syscall_state = {
    .user_map_hint = SYSCALL_MAP_HINT_BASE,
    .next_pid = 1U
};

static inline uintptr_t Syscall_align_up_page(uintptr_t value)
{
    return (value + (SYSCALL_PAGE_SIZE - 1U)) & ~(uintptr_t) (SYSCALL_PAGE_SIZE - 1U);
}

static uint8_t Syscall_dirent_type_from_ext4(uint8_t ext4_type)
{
    switch (ext4_type)
    {
        case EXT4_FT_DIR:
            return SYS_DT_DIR;
        case EXT4_FT_REG_FILE:
            return SYS_DT_REG;
        default:
            return SYS_DT_UNKNOWN;
    }
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
    bool tlb_needs_reload = false;
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
                tlb_needs_reload = true;
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
                tlb_needs_reload = true;

                bool ref_zero = false;
                if (Syscall_cow_ref_sub(old_phys, &ref_zero) && ref_zero)
                    PMM_dealloc_page((void*) old_phys);
            }

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

    if (tlb_needs_reload)
        Syscall_write_cr3_phys(Syscall_read_cr3_phys());

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

static uint64_t Syscall_handle_open(uint32_t cpu_index, const syscall_frame_t* frame)
{
    if (!frame || !Syscall_state.fd_lock_ready || !Syscall_state.fs_lock_ready)
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

    char write_path[SYSCALL_USER_CSTR_MAX] = { 0 };
    if (can_write)
        memcpy(write_path, lock_path, sizeof(write_path));

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
    entry->owner_pid = owner_pid;
    entry->exclusive = want_exclusive;
    entry->io_busy = true;
    memcpy(entry->path, lock_path, sizeof(entry->path));
    spin_unlock(&Syscall_state.fd_lock);

    uint8_t* file_data = NULL;
    size_t file_size = 0;
    size_t block_size = 0;
    bool fs_ready = false;
    bool exists = false;

    spin_lock(&Syscall_state.fs_lock);
    ext4_fs_t* fs = ext4_get_active();
    if (fs && fs->block_size != 0)
    {
        fs_ready = true;
        block_size = fs->block_size;
        exists = ext4_read_file(fs, path, &file_data, &file_size);
    }
    spin_unlock(&Syscall_state.fs_lock);

    if (!fs_ready)
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
    if (!frame || !Syscall_state.fd_lock_ready || !Syscall_state.fs_lock_ready)
        return (uint64_t) -1;

    int64_t fd = (int64_t) frame->rdi;
    uint32_t owner_pid = Syscall_proc_current_pid(cpu_index, frame);
    if (fd < 0 || (uint64_t) fd >= SYSCALL_MAX_OPEN_FILES || owner_pid == 0)
        return (uint64_t) -1;

    bool flush_needed = false;
    bool flush_ok = true;
    char flush_path[SYSCALL_USER_CSTR_MAX];
    size_t flush_size = 0;
    const uint8_t* flush_data = NULL;

    spin_lock(&Syscall_state.fd_lock);
    syscall_file_desc_t* entry = &Syscall_state.fds[(uint32_t) fd];
    if (!entry->used || entry->owner_pid != owner_pid || entry->io_busy)
    {
        spin_unlock(&Syscall_state.fd_lock);
        return (uint64_t) -1;
    }

    flush_needed = entry->can_write && entry->dirty;
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

    spin_lock(&Syscall_state.fs_lock);
    ext4_fs_t* fs = ext4_get_active();
    flush_ok = (fs && flush_size <= fs->block_size &&
                ext4_create_file(fs, flush_path, flush_data, flush_size));
    spin_unlock(&Syscall_state.fs_lock);

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
            memset((void*) page, 0, SYSCALL_PAGE_SIZE);
        }
        else if (!VMM_is_user_accessible(page))
            return false;
    }

    return true;
}

static bool Syscall_load_elf_segment_current(const uint8_t* elf_image,
                                             size_t elf_size,
                                             const syscall_elf64_phdr_t* phdr)
{
    if (!phdr || phdr->p_type != 1U)
        return false;

    if (phdr->p_memsz == 0)
        return true;

    if (phdr->p_filesz > phdr->p_memsz)
        return false;
    if (phdr->p_offset > elf_size)
        return false;
    if (phdr->p_filesz > (uint64_t) elf_size - phdr->p_offset)
        return false;
    if (!Syscall_is_canonical_low(phdr->p_vaddr) || phdr->p_vaddr < SYSCALL_USER_VADDR_MIN)
        return false;

    uint64_t seg_end = phdr->p_vaddr + phdr->p_memsz;
    if (seg_end < phdr->p_vaddr || !Syscall_is_canonical_low(seg_end - 1U))
        return false;

    if (!Syscall_map_user_range_current((uintptr_t) phdr->p_vaddr, (size_t) phdr->p_memsz))
        return false;

    memset((void*) (uintptr_t) phdr->p_vaddr, 0, (size_t) phdr->p_memsz);
    if (phdr->p_filesz != 0)
    {
        memcpy((void*) (uintptr_t) phdr->p_vaddr,
               elf_image + phdr->p_offset,
               (size_t) phdr->p_filesz);
    }

    uintptr_t start = (uintptr_t) phdr->p_vaddr & ~(uintptr_t) (SYSCALL_PAGE_SIZE - 1U);
    uintptr_t end = Syscall_align_up_page((uintptr_t) phdr->p_vaddr + (uintptr_t) phdr->p_memsz);
    bool writable = (phdr->p_flags & SYSCALL_ELF_PF_W) != 0;
    bool executable = (phdr->p_flags & SYSCALL_ELF_PF_X) != 0;
    if (writable && executable)
        return false;
    uintptr_t set_bits = writable ? WRITABLE : 0;
    uintptr_t clear_bits = writable ? 0 : WRITABLE;
    if (!executable)
        set_bits |= NO_EXECUTE;
    else
        clear_bits |= NO_EXECUTE;

    for (uintptr_t page = start; page < end; page += SYSCALL_PAGE_SIZE)
    {
        if (!VMM_update_page_flags(page, set_bits, clear_bits))
            return false;
    }

    return true;
}

static bool Syscall_load_elf_current(const char* path, uintptr_t* out_entry, uintptr_t* out_rsp)
{
    if (!path || !out_entry || !out_rsp)
        return false;

    if (!Syscall_state.fs_lock_ready)
        return false;

    spin_lock(&Syscall_state.fs_lock);
    ext4_fs_t* fs = ext4_get_active();
    if (!fs)
    {
        spin_unlock(&Syscall_state.fs_lock);
        return false;
    }

    uint8_t* elf_image = NULL;
    size_t elf_size = 0;
    bool read_ok = ext4_read_file(fs, path, &elf_image, &elf_size);
    spin_unlock(&Syscall_state.fs_lock);
    if (!read_ok)
        return false;
    if (!elf_image || elf_size < sizeof(syscall_elf64_ehdr_t) || elf_size > SYSCALL_ELF_MAX_SIZE)
    {
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
        ehdr->e_type != 2U ||
        ehdr->e_machine != 0x3EU)
    {
        kfree(elf_image);
        return false;
    }

    if (!Syscall_is_canonical_low(ehdr->e_entry) || ehdr->e_entry < SYSCALL_USER_VADDR_MIN)
    {
        kfree(elf_image);
        return false;
    }

    if (ehdr->e_phnum == 0 || ehdr->e_phnum > SYSCALL_ELF_MAX_PHDRS ||
        ehdr->e_phentsize < sizeof(syscall_elf64_phdr_t) ||
        ehdr->e_phoff > elf_size)
    {
        kfree(elf_image);
        return false;
    }

    uint64_t ph_table_size = (uint64_t) ehdr->e_phnum * (uint64_t) ehdr->e_phentsize;
    if (ph_table_size > (uint64_t) elf_size - ehdr->e_phoff)
    {
        kfree(elf_image);
        return false;
    }

    bool has_load = false;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++)
    {
        const uint8_t* ph_ptr = elf_image + ehdr->e_phoff + ((uint64_t) i * ehdr->e_phentsize);
        const syscall_elf64_phdr_t* phdr = (const syscall_elf64_phdr_t*) ph_ptr;
        if (phdr->p_type != 1U)
            continue;

        has_load = true;
        if (!Syscall_load_elf_segment_current(elf_image, elf_size, phdr))
        {
            kfree(elf_image);
            return false;
        }
    }

    if (!has_load)
    {
        kfree(elf_image);
        return false;
    }

    uintptr_t stack_bottom = SYSCALL_ELF_STACK_TOP - SYSCALL_ELF_STACK_SIZE;
    if (!Syscall_map_user_range_current(stack_bottom, SYSCALL_ELF_STACK_SIZE))
    {
        kfree(elf_image);
        return false;
    }

    *out_entry = (uintptr_t) ehdr->e_entry;
    *out_rsp = SYSCALL_ELF_STACK_TOP & ~(uintptr_t) 0xFULL;
    kfree(elf_image);
    return true;
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

        spin_lock(&Syscall_state.fd_lock);
        syscall_file_desc_t* entry = &Syscall_state.fds[i];
        if (!entry->used || entry->owner_pid != owner_pid)
        {
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

        if (should_flush && Syscall_state.fs_lock_ready)
        {
            spin_lock(&Syscall_state.fs_lock);
            ext4_fs_t* fs = ext4_get_active();
            if (fs && flush_size <= fs->block_size)
                (void) ext4_create_file(fs, flush_path, flush_data, flush_size);
            spin_unlock(&Syscall_state.fs_lock);
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

static int32_t Syscall_proc_pick_next_locked(int32_t current_slot)
{
    if (current_slot >= 0)
    {
        for (uint32_t step = 1; step < SYSCALL_MAX_PROCS; step++)
        {
            uint32_t idx = ((uint32_t) current_slot + step) % SYSCALL_MAX_PROCS;
            if (Syscall_state.procs[idx].used)
                return (int32_t) idx;
        }
    }

    if (current_slot >= 0 && Syscall_state.procs[current_slot].used)
        return current_slot;

    for (uint32_t idx = 0; idx < SYSCALL_MAX_PROCS; idx++)
    {
        if (Syscall_state.procs[idx].used)
            return (int32_t) idx;
    }

    return -1;
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
    if ((entry & (PRESENT | USER_MODE | SYSCALL_PTE_COW)) != (PRESENT | USER_MODE | SYSCALL_PTE_COW))
    {
        spin_unlock(&Syscall_state.vm_lock);
        return false;
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
            return SYS_SIGFPE;
        case 3:
            return SYS_SIGTRAP;
        case 6:
            return SYS_SIGILL;
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 17:
            return SYS_SIGSEGV;
        default:
            return SYS_SIGFAULT;
    }
}

static const char* Syscall_signal_name(int32_t signal)
{
    switch (signal)
    {
        case SYS_SIGFPE:
            return "SIGFPE";
        case SYS_SIGKILL:
            return "SIGKILL";
        case SYS_SIGTRAP:
            return "SIGTRAP";
        case SYS_SIGILL:
            return "SIGILL";
        case SYS_SIGSEGV:
            return "SIGSEGV";
        case SYS_SIGFAULT:
            return "SIGFAULT";
        default:
            return "SIGUNKNOWN";
    }
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
    memset(Syscall_state.fds, 0, sizeof(Syscall_state.fds));
    spinlock_init(&Syscall_state.fd_lock);
    Syscall_state.fd_lock_ready = true;
    spinlock_init(&Syscall_state.fs_lock);
    Syscall_state.fs_lock_ready = true;
    spinlock_init(&Syscall_state.vm_lock);
    Syscall_state.vm_lock_ready = true;

    memset(Syscall_state.procs, 0, sizeof(Syscall_state.procs));
    memset(Syscall_state.exit_events, 0, sizeof(Syscall_state.exit_events));
    memset(Syscall_state.thread_exit_events, 0, sizeof(Syscall_state.thread_exit_events));
    memset(Syscall_state.cow_refs, 0, sizeof(Syscall_state.cow_refs));
    for (uint32_t i = 0; i < 256; i++)
    {
        Syscall_state.cpu_current_proc[i] = SYSCALL_PROC_NONE;
        Syscall_state.cpu_need_resched[i] = 0;
        Syscall_state.cpu_slice_ticks[i] = 0;
    }
    Syscall_state.next_pid = 1U;
    spinlock_init(&Syscall_state.proc_lock);
    Syscall_state.proc_lock_ready = true;
    spinlock_init(&Syscall_state.cow_lock);
    Syscall_state.cow_lock_ready = true;

    MSR_set(IA32_LSTAR, (uint64_t) &syscall_handler_stub);
    MSR_set(IA32_FMASK, SYSCALL_FMASK_TF_BIT | SYSCALL_FMASK_DF_BIT);

    enable_syscall_ext();
}

void Syscall_on_timer_tick(uint32_t cpu_index)
{
    if (!Syscall_state.proc_lock_ready || cpu_index >= 256)
        return;

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
        return;
    }

    uint8_t slice = (uint8_t) (Syscall_state.cpu_slice_ticks[cpu_index] + 1U);
    Syscall_state.cpu_slice_ticks[cpu_index] = slice;
    if (slice >= SYSCALL_PREEMPT_QUANTUM_TICKS)
        __atomic_store_n(&Syscall_state.cpu_need_resched[cpu_index], 1, __ATOMIC_RELEASE);
}

bool Syscall_handle_timer_preempt(interrupt_frame_t* frame, uint32_t cpu_index)
{
    if (!frame || !Syscall_state.proc_lock_ready || cpu_index >= 256)
        return false;

    if ((frame->cs & 0x3ULL) != 0x3ULL)
        return false;

    uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
    if (__atomic_exchange_n(&Syscall_state.cpu_need_resched[cpu_index], 0, __ATOMIC_ACQ_REL) == 0)
    {
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        return false;
    }

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

    int32_t next_slot = Syscall_proc_pick_next_locked(current_slot);
    if (next_slot < 0 || next_slot == current_slot)
    {
        Syscall_state.cpu_slice_ticks[cpu_index] = 0;
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        return false;
    }

    syscall_process_t* next = &Syscall_state.procs[(uint32_t) next_slot];
    if (!next->used || next->exiting || next->cr3_phys == 0)
    {
        Syscall_state.cpu_slice_ticks[cpu_index] = 0;
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        return false;
    }

    Syscall_state.cpu_current_proc[cpu_index] = (uint32_t) next_slot;
    Syscall_state.cpu_slice_ticks[cpu_index] = 0;
    Syscall_proc_load_to_interrupt(next, frame);
    uintptr_t next_cr3 = next->cr3_phys;
    uintptr_t next_fs_base = next->fs_base;
    spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

    if (next_cr3 != current_cr3)
        Syscall_write_cr3_phys(next_cr3);
    if (next_fs_base != current_fs_base)
        Syscall_write_fs_base(next_fs_base);

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

    uint64_t count = __atomic_add_fetch(&Syscall_state.count_per_cpu[cpu_index], 1, __ATOMIC_RELAXED);
    if (cpu_local)
        __atomic_store_n(&cpu_local->syscall_count, count, __ATOMIC_RELAXED);

    if ((count % 1024ULL) == 0)
        kdebug_printf("[SYSCALL] cpu=%u apic=%u count=%llu\n",
                      cpu_index,
                      apic_id,
                      (unsigned long long) count);

    switch (syscall_num)
    {
        case SYS_FS_MKDIR:
        {
            if (!Syscall_state.fs_lock_ready)
                return (uint64_t) -1;

            char path[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_read_user_cstr(path, sizeof(path), (const char*) frame->rdi))
                return (uint64_t) -1;

            char normalized[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_normalize_write_path(path, normalized, sizeof(normalized)))
                return (uint64_t) -1;

            spin_lock(&Syscall_state.fs_lock);
            ext4_fs_t* fs = ext4_get_active();
            bool ok = fs && ext4_create_dir(fs, normalized);
            spin_unlock(&Syscall_state.fs_lock);
            return ok ? 0 : (uint64_t) -1;
        }

        case SYS_SLEEP_MS:
        {
            bool sleep_ok = HPET_sleep_ms((uint32_t) frame->rdi);
            if (sleep_ok && cpu_index < 256)
                __atomic_store_n(&Syscall_state.cpu_need_resched[cpu_index], 1, __ATOMIC_RELEASE);
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
            uint64_t lock_flags = spin_lock_irqsave(&Syscall_state.proc_lock);
            int32_t parent_slot = Syscall_proc_ensure_current_locked(cpu_index, frame);
            if (parent_slot < 0)
            {
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                return (uint64_t) -1;
            }

            syscall_process_t* parent = &Syscall_state.procs[parent_slot];
            if (Syscall_proc_owner_has_other_live_locked(parent->owner_pid, parent_slot))
            {
                spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
                return (uint64_t) -1;
            }

            parent_pid = parent->owner_pid;
            parent_cr3 = parent->cr3_phys;
            parent_fs_base = parent->fs_base;
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
            syscall_process_t* child = &Syscall_state.procs[child_slot];
            memset(child, 0, sizeof(*child));
            child->used = true;
            child->exiting = false;
            child->terminated_by_signal = false;
            child->owns_cr3 = true;
            child->is_thread = false;
            child->pid = child_pid;
            child->ppid = parent_pid;
            child->owner_pid = child_pid;
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
                __atomic_store_n(&Syscall_state.cpu_need_resched[cpu_index], 1, __ATOMIC_RELEASE);
            return 0;

        case SYS_MAP:
        {
            uintptr_t requested = (uintptr_t) frame->rdi;
            size_t len = (size_t) frame->rsi;
            uint64_t prot = frame->rdx;
            const uint64_t prot_mask = SYS_PROT_READ | SYS_PROT_WRITE | SYS_PROT_EXEC;
            if (len == 0 || (prot & SYS_PROT_READ) == 0 || (prot & ~prot_mask) != 0)
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
                if (!Syscall_user_range_unmapped(requested, map_size))
                    goto map_out;
                base = requested;
            }

            bool writable = (prot & SYS_PROT_WRITE) != 0;
            bool executable = (prot & SYS_PROT_EXEC) != 0;
            if (writable && executable)
                goto map_out;
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

                uintptr_t set_bits = 0;
                uintptr_t clear_bits = 0;
                if (!writable)
                    clear_bits |= WRITABLE;
                if (!executable)
                    set_bits |= NO_EXECUTE;
                else
                    clear_bits |= NO_EXECUTE;

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
                uint64_t* pte = Syscall_get_user_pte_ptr(Syscall_read_cr3_phys(), virt);
                if (pte && ((*pte & SYSCALL_PTE_COW) != 0))
                    was_cow = true;
                if (!VMM_unmap_page(virt, &phys))
                    goto unmap_out;
                if (phys != 0)
                {
                    if (was_cow)
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

        case SYS_KBD_GET_SCANCODE:
            return (uint64_t) Keyboard_get_scancode();

        case SYS_FS_ISDIR:
        {
            if (!Syscall_state.fs_lock_ready)
                return (uint64_t) -1;

            char path[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_read_user_cstr(path, sizeof(path), (const char*) frame->rdi))
                return (uint64_t) -1;

            spin_lock(&Syscall_state.fs_lock);
            ext4_fs_t* fs = ext4_get_active();
            bool is_dir = (fs && ext4_path_is_dir(fs, path));
            spin_unlock(&Syscall_state.fs_lock);
            return is_dir ? 1ULL : 0ULL;
        }

        case SYS_FS_READDIR:
        {
            if (!Syscall_state.fs_lock_ready)
                return (uint64_t) -1;

            char path[SYSCALL_USER_CSTR_MAX];
            if (!Syscall_read_user_cstr(path, sizeof(path), (const char*) frame->rdi))
                return (uint64_t) -1;

            size_t index = (size_t) frame->rsi;
            syscall_dirent_t* user_out = (syscall_dirent_t*) frame->rdx;
            if (!user_out)
                return (uint64_t) -1;

            ext4_dirent_info_t info;
            memset(&info, 0, sizeof(info));

            spin_lock(&Syscall_state.fs_lock);
            ext4_fs_t* fs = ext4_get_active();
            bool ok = (fs && ext4_read_dirent_at(fs, path, index, &info));
            spin_unlock(&Syscall_state.fs_lock);

            if (!ok)
                return 0;

            syscall_dirent_t out;
            memset(&out, 0, sizeof(out));
            out.d_ino = info.inode;
            out.d_type = Syscall_dirent_type_from_ext4(info.file_type);
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
            if (target_pid <= 0 || signal != SYS_SIGKILL)
                return (uint64_t) -1;

            uint32_t sender_pid = 0;
            bool killed = false;
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
                for (uint32_t i = 0; i < SYSCALL_MAX_PROCS; i++)
                {
                    syscall_process_t* proc = &Syscall_state.procs[i];
                    if (!proc->used || proc->owner_pid != target_owner_pid)
                        continue;
                    proc->exiting = true;
                    proc->terminated_by_signal = true;
                    proc->term_signal = SYS_SIGKILL;
                    proc->exit_status = 128 + SYS_SIGKILL;
                    proc->thread_exit_value = 0;
                }

                for (uint32_t i = 0; i < 256; i++)
                {
                    uint32_t running_slot = Syscall_state.cpu_current_proc[i];
                    if (running_slot >= SYSCALL_MAX_PROCS)
                        continue;
                    if (!Syscall_state.procs[running_slot].used)
                        continue;
                    if (Syscall_state.procs[running_slot].owner_pid != target_owner_pid)
                        continue;
                    __atomic_store_n(&Syscall_state.cpu_need_resched[i], 1, __ATOMIC_RELEASE);
                }
                killed = true;
            }

            spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

            if (!killed)
                return (uint64_t) -1;

            kdebug_printf("[USER] pid=%u sent SIGKILL to pid=%d\n",
                          (unsigned int) sender_pid,
                          (int) target_pid);
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

        default:
            return (uint64_t) -1;
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

    if (cpu_index >= 256)
        cpu_index = 0;

    uintptr_t old_cr3_to_free = 0;
    bool free_old_cr3 = false;
    bool cleanup_fds = false;
    uint32_t cleanup_pid = 0;
    uint32_t cleanup_ppid = 0;
    uint32_t cleanup_owner_pid = 0;
    uint32_t cleanup_tid = 0;
    int64_t cleanup_status = 0;
    int32_t cleanup_signal = 0;
    uint64_t cleanup_thread_value = 0;
    bool queue_exit_event = false;
    bool queue_thread_exit_event = false;
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
        cleanup_tid = current->pid;
        cleanup_status = current->exit_status;
        cleanup_signal = current->terminated_by_signal ? current->term_signal : 0;
        cleanup_thread_value = current->thread_exit_value;
        queue_thread_exit_event = current->is_thread && cleanup_owner_pid != 0 && cleanup_tid != 0;
        queue_exit_event = (!owner_has_other_live && cleanup_ppid != 0 && cleanup_pid != 0);
        cleanup_fds = !owner_has_other_live;
        memset(current, 0, sizeof(*current));
        Syscall_state.cpu_current_proc[cpu_index] = SYSCALL_PROC_NONE;
        current_slot = -1;
    }

    bool pick_next = (current_slot < 0);
    if (__atomic_exchange_n(&Syscall_state.cpu_need_resched[cpu_index], 0, __ATOMIC_ACQ_REL) != 0)
        pick_next = true;

    int32_t next_slot = current_slot;
    if (pick_next)
        next_slot = Syscall_proc_pick_next_locked(current_slot);
    if (next_slot < 0)
    {
        spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);
        if (free_old_cr3)
            Syscall_free_address_space(old_cr3_to_free);
        task_idle_loop();
        __builtin_unreachable();
    }

    Syscall_state.cpu_current_proc[cpu_index] = (uint32_t) next_slot;
    Syscall_state.cpu_slice_ticks[cpu_index] = 0;
    syscall_process_t* next = &Syscall_state.procs[next_slot];
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
    proc->exiting = true;
    proc->terminated_by_signal = true;
    proc->term_signal = signal;
    proc->exit_status = 128 + signal;
    spin_unlock_irqrestore(&Syscall_state.proc_lock, lock_flags);

    const char* signal_name = Syscall_signal_name(signal);
    if (frame->int_no == 14)
    {
        kdebug_printf("[USER] pid=%u killed by %s (#PF) rip=0x%llX err=0x%llX cr2=0x%llX\n",
                      (unsigned int) pid,
                      signal_name,
                      (unsigned long long) frame->rip,
                      (unsigned long long) frame->err_code,
                      (unsigned long long) fault_addr);
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
