#pragma once

#include <stdint.h>
#include <stddef.h>

/* Minimal ELF64 definitions — just enough to load static ET_EXEC images. */

#define ELF_CLASS64    2
#define ELF_DATA2LSB   1
#define ELF_ET_EXEC    2
#define ELF_EM_X86_64  62
#define ELF_PT_LOAD    1

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

/* Load a static ELF64 executable image (already fully in kernel memory)
 * into the user half of `pml4`. Returns 0 on success (entry point in
 * *entry_out, page-aligned end of the highest segment in *brk_out) or -1
 * on any malformed input. Never panics. On failure the caller is expected
 * to vmm_destroy_user(pml4), which frees any pages mapped so far. */
int elf_load(uint64_t *pml4, const void *image, size_t size,
             uint64_t *entry_out, uint64_t *brk_out);
