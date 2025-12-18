// elf.c - ELF file loader
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "elf.h"
#include "conf.h"
#include "io.h"
#include "string.h"
#include "memory.h"
#include "assert.h"
#include "error.h"
#include "heap.h"
#include "string.h"

#include <stdint.h>

// min + max address
#define MIN_HEADER UMEM_START_VMA
#define MAX_HEADER UMEM_END_VMA

// magic numbers
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

// Offsets into e_ident

#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8
#define EI_PAD 9

// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE 0
#define EV_CURRENT 1

// ELF header e_type values

enum elf_et
{
    ET_NONE = 0,
    ET_REL = 1,
    ET_EXEC = 2,
    ET_DYN = 3,
    ET_CORE = 4
};

struct elf64_ehdr
{
    unsigned char e_ident[16];
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
};

enum elf_pt
{
    PT_NULL = 0,
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
    PT_INTERP = 3,
    PT_NOTE = 4,
    PT_SHLIB = 5,
    PT_PHDR = 6,
    PT_TLS = 7
};

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

struct elf64_phdr
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define EM_RISCV 243

int elf_load(struct io *elfio, void (**eptr)(void))
{
    // FIX ME

    // Elf File Layout:
    // Header
    // Program Header
    // Segments that we need to check

    struct elf64_ehdr *header = kcalloc(1, sizeof(struct elf64_ehdr)); // pointer to header
    struct elf64_phdr *phdr = kcalloc(1, sizeof(struct elf64_phdr));
    // size_t size;

    if (elfio == NULL)
    {
        return -EINVAL;
    }

    unsigned char magicnums[4] = {ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3}; // magic nums

    if (ioreadat(elfio, 0, header, sizeof(struct elf64_ehdr)) != sizeof(struct elf64_ehdr))
    {
        return -EIO;
    }

    for (int i = 0; i < 4; i++)
    {
        if (magicnums[i] != header->e_ident[i])
        {
            return -EBADFMT;
        }
    }

    if (header->e_ident[EI_CLASS] != ELFCLASS64)
    {
        return -EBADFMT;
    }

    if (header->e_ident[EI_DATA] != ELFDATA2LSB)
    {
        return -EBADFMT;
    }

    if (header->e_ident[EI_VERSION] != EV_CURRENT)
    {
        return -EBADFMT;
    }
    if (header->e_machine != EM_RISCV)
    {
        return -EBADFMT;
    }

    // do we uses umem or MIN/MAX HEADER
    if ((header->e_entry < UMEM_START_VMA) || (header->e_entry + header->e_ehsize > UMEM_END_VMA))
    {
        return -EACCESS;
    }

    if (header->e_type != ET_EXEC)
    {
        return -EBADFMT;
    }

    for (int i = 0; i < header->e_phnum; i++)
    {
        // Seek to end of header
        if (ioreadat(elfio, header->e_phoff + i * header->e_phentsize, phdr, header->e_phentsize) != header->e_phentsize) // phentsize vs sizeof(phdr)
        {                                                                                                                 // try to read pgrm header?
            return -EIO;
        }
        if (phdr->p_type != PT_LOAD)
        {
            continue;
        }
        if (phdr->p_vaddr < UMEM_START_VMA || phdr->p_vaddr + phdr->p_memsz > UMEM_END_VMA)
        {
            return -EACCESS;
        }

        if (phdr->p_filesz > phdr->p_memsz)
        {
            continue;
        }

        // size = phdr->p_memsz;

        // kprintf("PAGE COUNT: %d\n", free_phys_page_count());
        // kprintf("Memsize: %d\n", phdr->p_memsz);
        void *newvp = alloc_and_map_range(phdr->p_vaddr, phdr->p_memsz, PTE_R | PTE_W | PTE_U);
        // kprintf("PAGE COUNT: %d\n", free_phys_page_count());
        if (newvp == NULL)
        {
            return -ENOMEM;
        }
        ioreadat(elfio, phdr->p_offset, (void *)phdr->p_vaddr, phdr->p_filesz);
        // size = phdr->p_memsz;
        int flag = 0;
        if ((phdr->p_flags & PF_X) == PF_X)
        {
            flag |= PTE_X;
        }
        if ((phdr->p_flags & PF_R) == PF_R)
        {
            flag |= PTE_R;
        }
        if ((phdr->p_flags & PF_W) == PF_W)
        {
            flag |= PTE_W;
        }
        set_range_flags((void *)phdr->p_vaddr, phdr->p_memsz, flag | PTE_U);
        memset((void *)(phdr->p_vaddr + phdr->p_filesz), 0, phdr->p_memsz - phdr->p_filesz);
    }

    *eptr = (void (*)(void))(header->e_entry);
    return 0;
}