#include <CPU/UserMode.h>
#include <CPU/Syscall.h>

#include <Debug/KDebug.h>
#include <FileSystem/ext4.h>
#include <Memory/KMem.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uintptr_t UserMode_align_down(uintptr_t value, uintptr_t align)
{
    return value & ~(align - 1U);
}

static inline uintptr_t UserMode_align_up(uintptr_t value, uintptr_t align)
{
    return (value + (align - 1U)) & ~(align - 1U);
}

static bool UserMode_is_canonical_low(uint64_t value)
{
    return value < USERMODE_CANONICAL_LOW_MAX;
}

static inline uintptr_t UserMode_read_cr3_phys(void)
{
    uintptr_t cr3 = 0;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & FRAME;
}

static inline void UserMode_write_cr3_phys(uintptr_t cr3_phys)
{
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(cr3_phys) : "memory");
}

static uintptr_t UserMode_alloc_zero_page_phys(void)
{
    uintptr_t phys = (uintptr_t) PMM_alloc_page();
    if (phys == 0)
        return 0;

    memset((void*) P2V(phys), 0, 0x1000U);
    return phys;
}

static void UserMode_free_user_pt(uintptr_t pt_phys)
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
        if (page_phys != 0)
            PMM_dealloc_page((void*) page_phys);
    }

    PMM_dealloc_page((void*) pt_phys);
}

static void UserMode_free_user_pdt(uintptr_t pdt_phys)
{
    if (pdt_phys == 0)
        return;

    PDT_t* pdt = (PDT_t*) P2V(pdt_phys);
    for (uint32_t i = 0; i < 512; i++)
    {
        uintptr_t entry = pdt->entries[i];
        if ((entry & PRESENT) == 0)
            continue;
        if ((entry & USERMODE_PTE_PS) != 0)
            continue;

        UserMode_free_user_pt(entry & FRAME);
    }

    PMM_dealloc_page((void*) pdt_phys);
}

static void UserMode_free_user_pdpt(uintptr_t pdpt_phys)
{
    if (pdpt_phys == 0)
        return;

    PDPT_t* pdpt = (PDPT_t*) P2V(pdpt_phys);
    for (uint32_t i = 0; i < 512; i++)
    {
        uintptr_t entry = pdpt->entries[i];
        if ((entry & PRESENT) == 0)
            continue;
        if ((entry & USERMODE_PTE_PS) != 0)
            continue;

        UserMode_free_user_pdt(entry & FRAME);
    }

    PMM_dealloc_page((void*) pdpt_phys);
}

static void UserMode_free_address_space(uintptr_t cr3_phys)
{
    if (cr3_phys == 0)
        return;

    PML4_t* pml4 = (PML4_t*) P2V(cr3_phys);
    for (uint32_t i = 0; i < VMM_HHDM_PML4_INDEX; i++)
    {
        uintptr_t entry = pml4->entries[i];
        if ((entry & PRESENT) == 0)
            continue;

        UserMode_free_user_pdpt(entry & FRAME);
    }

    PMM_dealloc_page((void*) cr3_phys);
}

static bool UserMode_create_isolated_address_space(uintptr_t* out_cr3_phys)
{
    if (!out_cr3_phys)
        return false;

    uintptr_t kernel_cr3 = VMM_get_kernel_cr3_phys();
    if (kernel_cr3 == 0)
        kernel_cr3 = UserMode_read_cr3_phys();
    if (kernel_cr3 == 0)
        return false;

    uintptr_t user_cr3 = UserMode_alloc_zero_page_phys();
    if (user_cr3 == 0)
        return false;

    PML4_t* kernel_pml4 = (PML4_t*) P2V(kernel_cr3);
    PML4_t* user_pml4 = (PML4_t*) P2V(user_cr3);
    for (uint32_t i = VMM_HHDM_PML4_INDEX; i < 512; i++)
        user_pml4->entries[i] = kernel_pml4->entries[i];

    user_pml4->entries[VMM_RECURSIVE_INDEX] = user_cr3 | PRESENT | WRITABLE;
    *out_cr3_phys = user_cr3;
    return true;
}

static bool UserMode_map_user_range(uintptr_t base, size_t size)
{
    if (size == 0)
        return true;

    uintptr_t end_raw = base + (uintptr_t) size;
    if (end_raw < base)
        return false;

    uintptr_t start = UserMode_align_down(base, 0x1000U);
    uintptr_t end = UserMode_align_up(end_raw, 0x1000U);

    for (uintptr_t page = start; page < end; page += 0x1000U)
    {
        uintptr_t phys = 0;
        if (!VMM_virt_to_phys(page, &phys))
        {
            void* new_page = PMM_alloc_page();
            if (!new_page)
                return false;

            VMM_map_user_page(page, (uintptr_t) new_page);
            memset((void*) page, 0, 0x1000U);
        }
        else if (!VMM_is_user_accessible(page))
            return false;
    }

    return true;
}

static bool UserMode_load_segment(const uint8_t* elf_image,
                                  size_t elf_size,
                                  const elf64_phdr_t* phdr)
{
    if (!phdr || phdr->p_type != ELF_PT_LOAD)
        return false;

    if (phdr->p_memsz == 0)
        return true;

    if (phdr->p_filesz > phdr->p_memsz)
        return false;
    if (phdr->p_offset > elf_size)
        return false;
    if (phdr->p_filesz > (uint64_t) elf_size - phdr->p_offset)
        return false;

    if (!UserMode_is_canonical_low(phdr->p_vaddr))
        return false;
    if (phdr->p_vaddr < USERMODE_MIN_VADDR)
        return false;

    uint64_t seg_end = phdr->p_vaddr + phdr->p_memsz;
    if (seg_end < phdr->p_vaddr || !UserMode_is_canonical_low(seg_end - 1))
        return false;

    if (!UserMode_map_user_range((uintptr_t) phdr->p_vaddr, (size_t) phdr->p_memsz))
        return false;

    memset((void*) (uintptr_t) phdr->p_vaddr, 0, (size_t) phdr->p_memsz);
    if (phdr->p_filesz != 0)
    {
        memcpy((void*) (uintptr_t) phdr->p_vaddr,
               elf_image + phdr->p_offset,
               (size_t) phdr->p_filesz);
    }

    uintptr_t start = UserMode_align_down((uintptr_t) phdr->p_vaddr, 0x1000U);
    uintptr_t end = UserMode_align_up((uintptr_t) phdr->p_vaddr + (uintptr_t) phdr->p_memsz, 0x1000U);
    bool writable = (phdr->p_flags & ELF_PF_W) != 0;
    bool executable = (phdr->p_flags & ELF_PF_X) != 0;
    if (writable && executable)
        return false;
    uintptr_t set_bits = writable ? WRITABLE : 0;
    uintptr_t clear_bits = writable ? 0 : WRITABLE;
    if (!executable)
        set_bits |= NO_EXECUTE;
    else
        clear_bits |= NO_EXECUTE;

    for (uintptr_t page = start; page < end; page += 0x1000U)
    {
        if (!VMM_update_page_flags(page, set_bits, clear_bits))
            return false;
    }

    return true;
}

static bool UserMode_build_initial_stack(uintptr_t* inout_rsp, const char* argv0)
{
    if (!inout_rsp || !argv0 || argv0[0] == '\0')
        return false;

    uintptr_t sp = *inout_rsp;
    uintptr_t stack_bottom = USERMODE_STACK_TOP - USERMODE_STACK_SIZE;

    size_t argv0_len = strlen(argv0) + 1U;
    if (sp < stack_bottom + argv0_len)
        return false;

    sp -= argv0_len;
    memcpy((void*) sp, argv0, argv0_len);
    uintptr_t argv0_ptr = sp;

    sp &= ~(uintptr_t) 0xFULL;

    // Stack layout at user entry:
    // argc, argv[0], NULL, envp[0]=NULL
    if (sp < stack_bottom + (4U * sizeof(uint64_t)))
        return false;

    sp -= sizeof(uint64_t);
    *((uint64_t*) sp) = 0;

    sp -= sizeof(uint64_t);
    *((uint64_t*) sp) = 0;

    sp -= sizeof(uint64_t);
    *((uint64_t*) sp) = argv0_ptr;

    sp -= sizeof(uint64_t);
    *((uint64_t*) sp) = 1;

    *inout_rsp = sp;
    return true;
}

bool UserMode_run_elf(const char* file_name)
{
    if (!file_name || file_name[0] == '\0')
        return false;

    uintptr_t user_cr3 = 0;
    uintptr_t user_entry = 0;
    uintptr_t user_rsp = 0;
    if (!Syscall_prepare_initial_user_process(file_name, &user_cr3, &user_entry, &user_rsp))
    {
        kdebug_printf("[USER] launch setup failed for '%s'\n", file_name);
        return false;
    }

    kdebug_printf("[USER] launching '%s' entry=0x%llX stack=0x%llX cr3=0x%llX\n",
                  file_name,
                  (unsigned long long) user_entry,
                  (unsigned long long) user_rsp,
                  (unsigned long long) user_cr3);

    UserMode_write_cr3_phys(user_cr3);
    switch_to_usermode(user_entry, user_rsp);
    __builtin_unreachable();
}
