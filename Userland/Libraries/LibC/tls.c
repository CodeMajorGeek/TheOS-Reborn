#include <libc_tls.h>

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syscall.h>

#define LIBC_TLS_MAX_THREADS 128U

extern uint8_t __theos_tls_tdata_start[];
extern uint8_t __theos_tls_tdata_end[];
extern uint8_t __theos_tls_tbss_end[];
extern uintptr_t __theos_tls_align;

typedef struct libc_tls_module_desc
{
    bool used;
    const void* init_image;
    size_t init_size;
    size_t image_size;
    size_t align;
} libc_tls_module_desc_t;

typedef struct libc_tls_thread_slot
{
    bool used;
    libc_tls_tcb_t* tcb;
} libc_tls_thread_slot_t;

static volatile uint8_t LibC_tls_global_lock = 0U;
static bool LibC_tls_registry_ready = false;
static uint64_t LibC_tls_generation = 1ULL;
static libc_tls_module_desc_t LibC_tls_modules[LIBC_TLS_MAX_MODULES];
static libc_tls_thread_slot_t LibC_tls_threads[LIBC_TLS_MAX_THREADS];

static bool LibC_tls_is_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static size_t LibC_tls_align_up(size_t value, size_t align)
{
    if (!LibC_tls_is_power_of_two(align))
        return 0U;
    if (value > (SIZE_MAX - (align - 1U)))
        return 0U;
    return (value + (align - 1U)) & ~(align - 1U);
}

static size_t LibC_tls_clamp_alignment(size_t align)
{
    if (!LibC_tls_is_power_of_two(align) || align < LIBC_TLS_MIN_ALIGN)
        return LIBC_TLS_MIN_ALIGN;
    if (align > LIBC_TLS_PAGE_SIZE)
        return 0U;
    return align;
}

static void LibC_tls_lock(void)
{
    while (__atomic_test_and_set(&LibC_tls_global_lock, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&LibC_tls_global_lock, __ATOMIC_RELAXED) != 0U)
            (void) sched_yield();
    }
}

static void LibC_tls_unlock(void)
{
    __atomic_clear(&LibC_tls_global_lock, __ATOMIC_RELEASE);
}

static void LibC_tls_set_errno_if_ready(int value)
{
    if (__libc_tls_current_tp() != 0U)
        errno = value;
}

static int LibC_tls_static_layout(size_t* out_init_size,
                                  size_t* out_image_size,
                                  size_t* out_rounded_size,
                                  size_t* out_align)
{
    if (!out_init_size || !out_image_size || !out_rounded_size || !out_align)
        return -1;

    uintptr_t tdata_start = (uintptr_t) &__theos_tls_tdata_start;
    uintptr_t tdata_end = (uintptr_t) &__theos_tls_tdata_end;
    uintptr_t tbss_end = (uintptr_t) &__theos_tls_tbss_end;
    if (tdata_end < tdata_start || tbss_end < tdata_end)
        return -1;

    size_t align = LibC_tls_clamp_alignment((size_t) (uintptr_t) &__theos_tls_align);
    if (align == 0U)
        return -1;

    size_t init_size = (size_t) (tdata_end - tdata_start);
    size_t image_size = (size_t) (tbss_end - tdata_start);
    size_t rounded_size = LibC_tls_align_up(image_size, align);
    if (rounded_size == 0U && image_size != 0U)
        return -1;

    *out_init_size = init_size;
    *out_image_size = image_size;
    *out_rounded_size = rounded_size;
    *out_align = align;
    return 0;
}

static int LibC_tls_registry_init_locked(void)
{
    if (LibC_tls_registry_ready)
        return 0;

    memset(LibC_tls_modules, 0, sizeof(LibC_tls_modules));
    memset(LibC_tls_threads, 0, sizeof(LibC_tls_threads));

    size_t init_size = 0;
    size_t image_size = 0;
    size_t rounded_size = 0;
    size_t align = 0;
    if (LibC_tls_static_layout(&init_size, &image_size, &rounded_size, &align) < 0)
        return -1;

    if (LIBC_TLS_MODULE_MAIN >= LIBC_TLS_MAX_MODULES)
        return -1;

    LibC_tls_modules[LIBC_TLS_MODULE_MAIN].used = true;
    LibC_tls_modules[LIBC_TLS_MODULE_MAIN].init_image = (const void*) (uintptr_t) &__theos_tls_tdata_start;
    LibC_tls_modules[LIBC_TLS_MODULE_MAIN].init_size = init_size;
    LibC_tls_modules[LIBC_TLS_MODULE_MAIN].image_size = image_size;
    LibC_tls_modules[LIBC_TLS_MODULE_MAIN].align = align;

    LibC_tls_generation = 1ULL;
    LibC_tls_registry_ready = true;
    return 0;
}

static int LibC_tls_dtv_allocate(size_t min_capacity,
                                 libc_tls_dtv_entry_t** out_entries,
                                 size_t* out_capacity,
                                 size_t* out_map_size)
{
    if (!out_entries || !out_capacity || !out_map_size)
        return -1;

    if (min_capacity < LIBC_TLS_INITIAL_DTV_CAPACITY)
        min_capacity = LIBC_TLS_INITIAL_DTV_CAPACITY;
    if (min_capacity >= LIBC_TLS_MAX_MODULES)
        min_capacity = LIBC_TLS_MAX_MODULES - 1U;

    if (min_capacity == 0U)
        return -1;
    if (min_capacity > (SIZE_MAX / sizeof(libc_tls_dtv_entry_t)))
        return -1;

    size_t bytes = min_capacity * sizeof(libc_tls_dtv_entry_t);
    size_t map_size = LibC_tls_align_up(bytes, LIBC_TLS_PAGE_SIZE);
    if (map_size == 0U)
        return -1;

    void* map = sys_map(NULL, map_size, SYS_PROT_READ | SYS_PROT_WRITE);
    if (!map)
        return -1;
    memset(map, 0, map_size);

    size_t capacity = map_size / sizeof(libc_tls_dtv_entry_t);
    if (capacity >= LIBC_TLS_MAX_MODULES)
        capacity = LIBC_TLS_MAX_MODULES - 1U;
    if (capacity < min_capacity)
    {
        (void) sys_unmap(map, map_size);
        return -1;
    }

    *out_entries = (libc_tls_dtv_entry_t*) map;
    *out_capacity = capacity;
    *out_map_size = map_size;
    return 0;
}

static int LibC_tls_dtv_ensure_capacity(libc_tls_tcb_t* tcb, size_t module_id)
{
    if (!tcb || module_id == 0U || module_id >= LIBC_TLS_MAX_MODULES)
        return -1;
    if (module_id < tcb->dtv_capacity)
        return 0;

    size_t new_capacity = tcb->dtv_capacity;
    if (new_capacity < LIBC_TLS_INITIAL_DTV_CAPACITY)
        new_capacity = LIBC_TLS_INITIAL_DTV_CAPACITY;
    while (new_capacity <= module_id)
    {
        if (new_capacity >= (LIBC_TLS_MAX_MODULES - 1U))
        {
            new_capacity = LIBC_TLS_MAX_MODULES - 1U;
            break;
        }
        new_capacity *= 2U;
    }
    if (new_capacity <= module_id)
        return -1;

    libc_tls_dtv_entry_t* new_entries = NULL;
    size_t new_effective_capacity = 0;
    size_t new_map_size = 0;
    if (LibC_tls_dtv_allocate(new_capacity, &new_entries, &new_effective_capacity, &new_map_size) < 0)
        return -1;

    size_t copy_count = tcb->dtv_capacity;
    if (copy_count > new_effective_capacity)
        copy_count = new_effective_capacity;
    memcpy(new_entries, tcb->dtv_entries, copy_count * sizeof(libc_tls_dtv_entry_t));

    if (tcb->dtv_entries && tcb->dtv_map_size != 0U)
        (void) sys_unmap(tcb->dtv_entries, tcb->dtv_map_size);

    tcb->dtv_entries = new_entries;
    tcb->dtv_capacity = new_effective_capacity;
    tcb->dtv_map_size = new_map_size;
    return 0;
}

static int LibC_tls_thread_register_locked(libc_tls_tcb_t* tcb)
{
    if (!tcb)
        return -1;

    int32_t free_slot = -1;
    for (uint32_t i = 0; i < LIBC_TLS_MAX_THREADS; i++)
    {
        if (LibC_tls_threads[i].used)
        {
            if (LibC_tls_threads[i].tcb == tcb)
                return 0;
            continue;
        }

        if (free_slot < 0)
            free_slot = (int32_t) i;
    }

    if (free_slot < 0)
        return -1;

    LibC_tls_threads[(uint32_t) free_slot].used = true;
    LibC_tls_threads[(uint32_t) free_slot].tcb = tcb;
    return 0;
}

static void LibC_tls_thread_unregister_locked(libc_tls_tcb_t* tcb)
{
    if (!tcb)
        return;

    for (uint32_t i = 0; i < LIBC_TLS_MAX_THREADS; i++)
    {
        if (!LibC_tls_threads[i].used || LibC_tls_threads[i].tcb != tcb)
            continue;

        LibC_tls_threads[i].used = false;
        LibC_tls_threads[i].tcb = NULL;
        return;
    }
}

static int LibC_tls_map_module_block(size_t image_size,
                                     size_t align,
                                     const void* init_image,
                                     size_t init_size,
                                     void** out_map_base,
                                     size_t* out_map_size,
                                     void** out_module_tp)
{
    if (!out_map_base || !out_map_size || !out_module_tp)
        return -1;

    size_t clamped_align = LibC_tls_clamp_alignment(align);
    if (clamped_align == 0U)
        return -1;

    size_t rounded_image = LibC_tls_align_up(image_size, clamped_align);
    if (rounded_image == 0U)
        rounded_image = clamped_align;

    size_t map_size = LibC_tls_align_up(rounded_image, LIBC_TLS_PAGE_SIZE);
    if (map_size == 0U)
        return -1;

    void* map_base = sys_map(NULL, map_size, SYS_PROT_READ | SYS_PROT_WRITE);
    if (!map_base)
        return -1;

    memset(map_base, 0, map_size);
    if (init_size != 0U)
        memcpy(map_base, init_image, init_size);

    *out_map_base = map_base;
    *out_map_size = map_size;
    *out_module_tp = (void*) ((uintptr_t) map_base + rounded_image);
    return 0;
}

static int LibC_tls_create_dynamic_entry_locked(libc_tls_tcb_t* tcb, size_t module_id)
{
    if (!tcb || module_id == 0U || module_id >= LIBC_TLS_MAX_MODULES)
        return -1;
    if (!LibC_tls_registry_ready || !LibC_tls_modules[module_id].used)
        return -1;

    libc_tls_dtv_entry_t* entry = &tcb->dtv_entries[module_id];
    if (entry->module_tp && entry->map_base)
        return 0;

    if (module_id == LIBC_TLS_MODULE_MAIN)
    {
        entry->module_tp = (void*) (uintptr_t) tcb->self;
        entry->map_base = tcb->static_tls_base;
        entry->map_size = tcb->static_tls_size;
        entry->owns_block = 0U;
        tcb->dtv_generation = LibC_tls_generation;
        return 0;
    }

    void* map_base = NULL;
    size_t map_size = 0;
    void* module_tp = NULL;
    libc_tls_module_desc_t* desc = &LibC_tls_modules[module_id];
    if (LibC_tls_map_module_block(desc->image_size,
                                  desc->align,
                                  desc->init_image,
                                  desc->init_size,
                                  &map_base,
                                  &map_size,
                                  &module_tp) < 0)
    {
        return -1;
    }

    entry->module_tp = module_tp;
    entry->map_base = map_base;
    entry->map_size = map_size;
    entry->owns_block = 1U;
    tcb->dtv_generation = LibC_tls_generation;
    return 0;
}

static void* LibC_tls_compute_addr(const libc_tls_dtv_entry_t* entry, uint64_t raw_offset)
{
    if (!entry)
        return NULL;

    intptr_t offset = (intptr_t) raw_offset;
    if (offset < 0)
    {
        uintptr_t base = (uintptr_t) entry->module_tp;
        uintptr_t neg = (uintptr_t) (-offset);
        if (base == 0U || base < neg)
            return NULL;
        return (void*) (base - neg);
    }

    uintptr_t base = (uintptr_t) entry->map_base;
    uintptr_t pos = (uintptr_t) offset;
    if (base == 0U || base > (UINTPTR_MAX - pos))
        return NULL;
    return (void*) (base + pos);
}

int __libc_tls_create(libc_tls_region_t* out_region)
{
    if (!out_region)
        return -1;

    memset(out_region, 0, sizeof(*out_region));

    LibC_tls_lock();
    if (LibC_tls_registry_init_locked() < 0)
    {
        LibC_tls_unlock();
        return -1;
    }

    size_t init_size = 0;
    size_t image_size = 0;
    size_t rounded_size = 0;
    size_t align = 0;
    if (LibC_tls_static_layout(&init_size, &image_size, &rounded_size, &align) < 0)
    {
        LibC_tls_unlock();
        return -1;
    }

    if (rounded_size > (SIZE_MAX - sizeof(libc_tls_tcb_t)))
    {
        LibC_tls_unlock();
        return -1;
    }

    size_t total_size = rounded_size + sizeof(libc_tls_tcb_t);
    size_t map_size = LibC_tls_align_up(total_size, LIBC_TLS_PAGE_SIZE);
    if (map_size == 0U)
    {
        LibC_tls_unlock();
        return -1;
    }

    void* mapping = sys_map(NULL, map_size, SYS_PROT_READ | SYS_PROT_WRITE);
    if (!mapping)
    {
        LibC_tls_unlock();
        return -1;
    }

    memset(mapping, 0, map_size);
    if (init_size != 0U)
        memcpy(mapping, (const void*) (uintptr_t) &__theos_tls_tdata_start, init_size);

    uintptr_t thread_pointer = (uintptr_t) mapping + rounded_size;
    libc_tls_tcb_t* tcb = (libc_tls_tcb_t*) thread_pointer;
    memset(tcb, 0, sizeof(*tcb));
    tcb->self = (void*) thread_pointer;
    tcb->static_tls_base = mapping;
    tcb->static_tls_size = rounded_size;
    tcb->static_tls_align = align;

    if (LibC_tls_dtv_allocate(LIBC_TLS_INITIAL_DTV_CAPACITY,
                              &tcb->dtv_entries,
                              &tcb->dtv_capacity,
                              &tcb->dtv_map_size) < 0)
    {
        (void) sys_unmap(mapping, map_size);
        LibC_tls_unlock();
        return -1;
    }

    if (LIBC_TLS_MODULE_MAIN >= tcb->dtv_capacity)
    {
        (void) sys_unmap(tcb->dtv_entries, tcb->dtv_map_size);
        (void) sys_unmap(mapping, map_size);
        LibC_tls_unlock();
        return -1;
    }

    tcb->dtv_entries[LIBC_TLS_MODULE_MAIN].module_tp = (void*) thread_pointer;
    tcb->dtv_entries[LIBC_TLS_MODULE_MAIN].map_base = mapping;
    tcb->dtv_entries[LIBC_TLS_MODULE_MAIN].map_size = rounded_size;
    tcb->dtv_entries[LIBC_TLS_MODULE_MAIN].owns_block = 0U;
    tcb->dtv_generation = LibC_tls_generation;

    if (LibC_tls_thread_register_locked(tcb) < 0)
    {
        (void) sys_unmap(tcb->dtv_entries, tcb->dtv_map_size);
        (void) sys_unmap(mapping, map_size);
        LibC_tls_unlock();
        return -1;
    }

    out_region->mapping_base = mapping;
    out_region->mapping_size = map_size;
    out_region->thread_pointer = thread_pointer;
    LibC_tls_unlock();
    return 0;
}

void __libc_tls_destroy(const libc_tls_region_t* region)
{
    if (!region || !region->mapping_base || region->mapping_size == 0U)
        return;

    libc_tls_dtv_entry_t* dtv_entries = NULL;
    size_t dtv_map_size = 0;

    libc_tls_tcb_t* tcb = (libc_tls_tcb_t*) region->thread_pointer;
    LibC_tls_lock();
    if (tcb && (uintptr_t) tcb->self == region->thread_pointer)
    {
        LibC_tls_thread_unregister_locked(tcb);

        for (size_t i = 1; i < tcb->dtv_capacity; i++)
        {
            libc_tls_dtv_entry_t* entry = &tcb->dtv_entries[i];
            if (entry->owns_block && entry->map_base && entry->map_size != 0U)
                (void) sys_unmap(entry->map_base, entry->map_size);
            memset(entry, 0, sizeof(*entry));
        }

        dtv_entries = tcb->dtv_entries;
        dtv_map_size = tcb->dtv_map_size;
        tcb->dtv_entries = NULL;
        tcb->dtv_capacity = 0;
        tcb->dtv_map_size = 0;
    }
    LibC_tls_unlock();

    if (dtv_entries && dtv_map_size != 0U)
        (void) sys_unmap(dtv_entries, dtv_map_size);

    (void) sys_unmap(region->mapping_base, region->mapping_size);
}

int __libc_tls_activate(uintptr_t thread_pointer)
{
    if (thread_pointer == 0U)
        return -1;
    return sys_thread_set_fsbase(thread_pointer);
}

uintptr_t __libc_tls_current_tp(void)
{
    return sys_thread_get_fsbase();
}

int __libc_tls_init_main(void)
{
    if (__libc_tls_current_tp() != 0U)
        return 0;

    libc_tls_region_t region;
    if (__libc_tls_create(&region) < 0)
        return -1;

    if (__libc_tls_activate(region.thread_pointer) < 0)
    {
        __libc_tls_destroy(&region);
        return -1;
    }

    return 0;
}

int __libc_tls_module_register(const void* init_image,
                               size_t init_size,
                               size_t image_size,
                               size_t align,
                               size_t* out_module_id)
{
    if (!out_module_id || init_size > image_size || (init_size != 0U && !init_image))
    {
        LibC_tls_set_errno_if_ready(EINVAL);
        return -1;
    }

    size_t clamped_align = LibC_tls_clamp_alignment(align == 0U ? LIBC_TLS_MIN_ALIGN : align);
    if (clamped_align == 0U)
    {
        LibC_tls_set_errno_if_ready(EINVAL);
        return -1;
    }

    int err = 0;
    size_t module_id = 0;

    LibC_tls_lock();
    if (LibC_tls_registry_init_locked() < 0)
    {
        err = EINVAL;
        goto register_out;
    }

    for (size_t i = LIBC_TLS_MODULE_MAIN + 1U; i < LIBC_TLS_MAX_MODULES; i++)
    {
        if (LibC_tls_modules[i].used)
            continue;
        module_id = i;
        break;
    }

    if (module_id == 0U)
    {
        err = ENOSPC;
        goto register_out;
    }

    LibC_tls_modules[module_id].used = true;
    LibC_tls_modules[module_id].init_image = init_image;
    LibC_tls_modules[module_id].init_size = init_size;
    LibC_tls_modules[module_id].image_size = image_size;
    LibC_tls_modules[module_id].align = clamped_align;
    LibC_tls_generation++;

register_out:
    LibC_tls_unlock();
    if (err != 0)
    {
        LibC_tls_set_errno_if_ready(err);
        return -1;
    }

    *out_module_id = module_id;
    return 0;
}

int __libc_tls_module_unregister(size_t module_id)
{
    if (module_id <= LIBC_TLS_MODULE_MAIN || module_id >= LIBC_TLS_MAX_MODULES)
    {
        LibC_tls_set_errno_if_ready(EINVAL);
        return -1;
    }

    int err = 0;

    LibC_tls_lock();
    if (LibC_tls_registry_init_locked() < 0)
    {
        err = EINVAL;
        goto unregister_out;
    }

    if (!LibC_tls_modules[module_id].used)
    {
        err = ENOENT;
        goto unregister_out;
    }

    memset(&LibC_tls_modules[module_id], 0, sizeof(LibC_tls_modules[module_id]));

    uint64_t new_generation = LibC_tls_generation + 1ULL;
    for (size_t t = 0; t < LIBC_TLS_MAX_THREADS; t++)
    {
        if (!LibC_tls_threads[t].used || !LibC_tls_threads[t].tcb)
            continue;

        libc_tls_tcb_t* tcb = LibC_tls_threads[t].tcb;
        if (module_id >= tcb->dtv_capacity)
            continue;

        libc_tls_dtv_entry_t* entry = &tcb->dtv_entries[module_id];
        if (entry->owns_block && entry->map_base && entry->map_size != 0U)
            (void) sys_unmap(entry->map_base, entry->map_size);
        memset(entry, 0, sizeof(*entry));
        tcb->dtv_generation = new_generation;
    }

    LibC_tls_generation = new_generation;

unregister_out:
    LibC_tls_unlock();
    if (err != 0)
    {
        LibC_tls_set_errno_if_ready(err);
        return -1;
    }

    return 0;
}

void* __tls_get_addr(const libc_tls_index_t* index)
{
    if (!index)
        return NULL;

    size_t module_id = (size_t) index->module_id;
    if (module_id == 0U || module_id >= LIBC_TLS_MAX_MODULES)
        return NULL;

    uintptr_t tp = __libc_tls_current_tp();
    if (tp == 0U)
        return NULL;

    libc_tls_tcb_t* tcb = (libc_tls_tcb_t*) tp;
    if ((uintptr_t) tcb->self != tp)
        return NULL;

    void* addr = NULL;

    LibC_tls_lock();
    if (LibC_tls_registry_init_locked() < 0)
        goto tls_get_out;
    if (!LibC_tls_modules[module_id].used)
        goto tls_get_out;
    if (LibC_tls_dtv_ensure_capacity(tcb, module_id) < 0)
        goto tls_get_out;

    if (LibC_tls_create_dynamic_entry_locked(tcb, module_id) < 0)
        goto tls_get_out;

    addr = LibC_tls_compute_addr(&tcb->dtv_entries[module_id], index->offset);

 tls_get_out:
    LibC_tls_unlock();
    return addr;
}
