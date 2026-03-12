#ifndef _USERMODE_H
#define _USERMODE_H

#include <stdbool.h>
#include <stdint.h>

#define USERMODE_CANONICAL_LOW_MAX    0x0000800000000000ULL
#define USERMODE_MIN_VADDR            0x0000000020000000ULL
#define USERMODE_STACK_TOP            0x0000000070000000ULL
#define USERMODE_STACK_SIZE           (64ULL * 1024ULL)
#define USERMODE_ELF_MAX_SIZE         (4ULL * 1024ULL * 1024ULL)
#define USERMODE_ELF_MAX_PHDRS        64U
#define USERMODE_PTE_PS               (1ULL << 7)

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
#define ELF_PF_X                      (1U << 0)
#define ELF_PF_W                      (1U << 1)

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

bool UserMode_run_elf(const char* file_name);
__attribute__((__noreturn__)) void switch_to_usermode(uintptr_t entry, uintptr_t user_stack_top);

#endif
