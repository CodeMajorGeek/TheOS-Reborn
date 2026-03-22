#ifndef _LIBC_ELF_H
#define _LIBC_ELF_H

#include <stdint.h>

#define LIBC_ELF_EI_NIDENT 16

#define LIBC_ELF_ET_DYN 3U

#define LIBC_ELF_EM_X86_64 62U

#define LIBC_ELF_PT_LOAD    1U
#define LIBC_ELF_PT_DYNAMIC 2U
#define LIBC_ELF_PT_TLS     7U

#define LIBC_ELF_PF_X (1U << 0)
#define LIBC_ELF_PF_W (1U << 1)
#define LIBC_ELF_PF_R (1U << 2)

#define LIBC_ELF_DT_NULL         0
#define LIBC_ELF_DT_NEEDED       1
#define LIBC_ELF_DT_PLTRELSZ     2
#define LIBC_ELF_DT_HASH         4
#define LIBC_ELF_DT_STRTAB       5
#define LIBC_ELF_DT_SYMTAB       6
#define LIBC_ELF_DT_RELA         7
#define LIBC_ELF_DT_RELASZ       8
#define LIBC_ELF_DT_RELAENT      9
#define LIBC_ELF_DT_STRSZ        10
#define LIBC_ELF_DT_SYMENT       11
#define LIBC_ELF_DT_INIT         12
#define LIBC_ELF_DT_FINI         13
#define LIBC_ELF_DT_SONAME       14
#define LIBC_ELF_DT_RPATH        15
#define LIBC_ELF_DT_SYMBOLIC     16
#define LIBC_ELF_DT_PLTREL       20
#define LIBC_ELF_DT_JMPREL       23
#define LIBC_ELF_DT_BIND_NOW     24
#define LIBC_ELF_DT_INIT_ARRAY   25
#define LIBC_ELF_DT_FINI_ARRAY   26
#define LIBC_ELF_DT_INIT_ARRAYSZ 27
#define LIBC_ELF_DT_FINI_ARRAYSZ 28
#define LIBC_ELF_DT_RUNPATH      29
#define LIBC_ELF_DT_FLAGS        30
#define LIBC_ELF_DT_FLAGS_1      0x6ffffffb

#define LIBC_ELF_STN_UNDEF 0U
#define LIBC_ELF_SHN_UNDEF 0U

#define LIBC_ELF_STB_LOCAL  0U
#define LIBC_ELF_STB_GLOBAL 1U
#define LIBC_ELF_STB_WEAK   2U

#define LIBC_ELF_STT_NOTYPE  0U
#define LIBC_ELF_STT_OBJECT  1U
#define LIBC_ELF_STT_FUNC    2U
#define LIBC_ELF_STT_SECTION 3U
#define LIBC_ELF_STT_FILE    4U
#define LIBC_ELF_STT_TLS     6U

#define LIBC_ELF_R_X86_64_NONE     0U
#define LIBC_ELF_R_X86_64_64       1U
#define LIBC_ELF_R_X86_64_GLOB_DAT 6U
#define LIBC_ELF_R_X86_64_JUMP_SLOT 7U
#define LIBC_ELF_R_X86_64_RELATIVE 8U
#define LIBC_ELF_R_X86_64_DTPMOD64 16U
#define LIBC_ELF_R_X86_64_DTPOFF64 17U

#define LIBC_ELF64_R_SYM(info)  ((uint32_t) ((info) >> 32))
#define LIBC_ELF64_R_TYPE(info) ((uint32_t) (info))
#define LIBC_ELF64_ST_BIND(info) ((uint8_t) ((info) >> 4))
#define LIBC_ELF64_ST_TYPE(info) ((uint8_t) ((info) & 0x0FU))

typedef struct libc_elf64_ehdr
{
    uint8_t e_ident[LIBC_ELF_EI_NIDENT];
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
} __attribute__((packed)) libc_elf64_ehdr_t;

typedef struct libc_elf64_phdr
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) libc_elf64_phdr_t;

typedef struct libc_elf64_dyn
{
    int64_t d_tag;
    union
    {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} __attribute__((packed)) libc_elf64_dyn_t;

typedef struct libc_elf64_sym
{
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) libc_elf64_sym_t;

typedef struct libc_elf64_rela
{
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} __attribute__((packed)) libc_elf64_rela_t;

#endif
