#include <CPU/UserMode.h>

#include <Debug/KDebug.h>
#include <FileSystem/ext4.h>
#include <Memory/KMem.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define USERMODE_CANONICAL_LOW_MAX    0x0000800000000000ULL
#define USERMODE_MIN_VADDR            0x0000000020000000ULL
#define USERMODE_STACK_TOP            0x0000000070000000ULL
#define USERMODE_STACK_SIZE           (64ULL * 1024ULL)
#define USERMODE_ELF_MAX_SIZE         (4ULL * 1024ULL * 1024ULL)
#define USERMODE_ELF_MAX_PHDRS        64U

#define ELF_IDENT_SIZE                16U
#define ELF_MAGIC0                    0x7FU
#define ELF_MAGIC1                    'E'
#define ELF_MAGIC2                    'L'
#define ELF_MAGIC3                    'F'
#define ELF_CLASS_64                  2U
#define ELF_DATA_LITTLE               1U
#define ELF_TYPE_EXEC                 2U
#define ELF_MACHINE_X86_64            0x3EU
#define ELF_PT_LOAD                   1U

typedef struct elf64_ehdr
{
    uint8_t e_ident[ELF_IDENT_SIZE];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct elf64_phdr
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

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

    return true;
}

bool UserMode_run_elf(const char* file_name)
{
    if (!file_name || file_name[0] == '\0')
        return false;

    ext4_fs_t* fs = ext4_get_active();
    if (!fs)
    {
        kdebug_puts("[USER] no active ext4 filesystem\n");
        return false;
    }

    uint8_t* elf_image = NULL;
    size_t elf_size = 0;
    if (!ext4_read_file(fs, file_name, &elf_image, &elf_size))
    {
        kdebug_printf("[USER] ELF '%s' not found on ext4\n", file_name);
        return false;
    }

    if (!elf_image || elf_size < sizeof(elf64_ehdr_t) || elf_size > USERMODE_ELF_MAX_SIZE)
    {
        kfree(elf_image);
        kdebug_printf("[USER] ELF '%s' invalid size=%llu\n",
                      file_name,
                      (unsigned long long) elf_size);
        return false;
    }

    const elf64_ehdr_t* ehdr = (const elf64_ehdr_t*) elf_image;
    if (ehdr->e_ident[0] != ELF_MAGIC0 ||
        ehdr->e_ident[1] != ELF_MAGIC1 ||
        ehdr->e_ident[2] != ELF_MAGIC2 ||
        ehdr->e_ident[3] != ELF_MAGIC3 ||
        ehdr->e_ident[4] != ELF_CLASS_64 ||
        ehdr->e_ident[5] != ELF_DATA_LITTLE ||
        ehdr->e_type != ELF_TYPE_EXEC ||
        ehdr->e_machine != ELF_MACHINE_X86_64)
    {
        kfree(elf_image);
        kdebug_printf("[USER] ELF '%s' header validation failed\n", file_name);
        return false;
    }

    if (!UserMode_is_canonical_low(ehdr->e_entry) || ehdr->e_entry < USERMODE_MIN_VADDR)
    {
        kfree(elf_image);
        kdebug_printf("[USER] ELF '%s' entry out of user range: 0x%llX\n",
                      file_name,
                      (unsigned long long) ehdr->e_entry);
        return false;
    }

    if (ehdr->e_phnum == 0 || ehdr->e_phnum > USERMODE_ELF_MAX_PHDRS ||
        ehdr->e_phentsize < sizeof(elf64_phdr_t) ||
        ehdr->e_phoff > elf_size)
    {
        kfree(elf_image);
        kdebug_printf("[USER] ELF '%s' program header table invalid\n", file_name);
        return false;
    }

    uint64_t ph_table_size = (uint64_t) ehdr->e_phnum * (uint64_t) ehdr->e_phentsize;
    if (ph_table_size > (uint64_t) elf_size - ehdr->e_phoff)
    {
        kfree(elf_image);
        kdebug_printf("[USER] ELF '%s' program headers out of bounds\n", file_name);
        return false;
    }

    bool has_load_segment = false;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++)
    {
        const uint8_t* ph_ptr = elf_image + ehdr->e_phoff + ((uint64_t) i * ehdr->e_phentsize);
        const elf64_phdr_t* phdr = (const elf64_phdr_t*) ph_ptr;

        if (phdr->p_type != ELF_PT_LOAD)
            continue;

        has_load_segment = true;
        if (!UserMode_load_segment(elf_image, elf_size, phdr))
        {
            kfree(elf_image);
            kdebug_printf("[USER] ELF '%s' failed to load segment index=%u\n",
                          file_name,
                          i);
            return false;
        }
    }

    if (!has_load_segment)
    {
        kfree(elf_image);
        kdebug_printf("[USER] ELF '%s' has no PT_LOAD segment\n", file_name);
        return false;
    }

    uintptr_t stack_bottom = USERMODE_STACK_TOP - USERMODE_STACK_SIZE;
    if (!UserMode_map_user_range(stack_bottom, USERMODE_STACK_SIZE))
    {
        kfree(elf_image);
        kdebug_printf("[USER] ELF '%s' failed to map user stack\n", file_name);
        return false;
    }

    uintptr_t user_rsp = UserMode_align_down(USERMODE_STACK_TOP, 16U);
    kdebug_printf("[USER] launching '%s' entry=0x%llX stack=0x%llX\n",
                  file_name,
                  (unsigned long long) ehdr->e_entry,
                  (unsigned long long) user_rsp);

    kfree(elf_image);
    switch_to_usermode((uintptr_t) ehdr->e_entry, user_rsp);
    __builtin_unreachable();
}
