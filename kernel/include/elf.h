#pragma once

#include <stdint.h>
#include <stddef.h>

/* Minimal ELF64 definitions — just enough to load static ET_EXEC images. */

#define ELF_CLASS64    2
#define ELF_DATA2LSB   1
#define ELF_ET_EXEC    2
#define ELF_EM_X86_64  62
#define ELF_PT_LOAD    1
#define ELF_PF_W       2

struct task;

struct elf64_ehdr {
    uint8_t  e_ident[16];
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
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

/* Validate a static ELF64 image and register its PT_LOAD records as lazy,
 * file-backed VM areas on `task`. No executable page is allocated here. */
struct file;
int elf_load(struct task *task, struct file *backing,
             const void *image, size_t size,
             uint64_t *entry_out, uint64_t *brk_out);
