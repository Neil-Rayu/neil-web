/*! @file memory.c
    @brief Physical and virtual memory manager
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"
#include "conf.h"
#include "riscv.h"
#include "heap.h"
#include "elf.h"
#include "console.h"
#include "assert.h"
#include "string.h"
#include "thread.h"
#include "process.h"
#include "error.h"
#include "intr.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE) // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE) // gigapage size
#define PPN_TO_PMA 12

#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#define RISCV_SATP_MODE_shift 60UL
#define RISCV_SATP_ASID_shift 44UL
#define RISCV_SATP_PPN_shift 0U
#define PTE_SIZE (PAGE_SIZE / PTE_CNT)

#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk
{
    struct page_chunk *next; ///< Next page in list
    unsigned long pagecnt;   ///< Number of pages in chunk
};

/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte
{
    uint64_t flags : 8;
    uint64_t rsw : 2;
    uint64_t ppn : 44;
    uint64_t reserved : 7;
    uint64_t pbmt : 2;
    uint64_t n : 1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2 * 9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1 * 9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0 * 9)) % PTE_CNT)

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

// #define ROUND_UP(n, k) (((n) + (k) - 1) / (k) * (k))
// #define ROUND_DOWN(n, k) ((n) / (k) * (k))

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//

static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte *root, unsigned int asid);
static inline struct pte *mtag_to_ptab(mtag_t mtag);
static inline struct pte *active_space_ptab(void);

static inline void *pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void *p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);
int pt_empty(struct pte *pt_start);

// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk *free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
//

void memory_init(void)
{
    const void *const text_start = _kimg_text_start;
    const void *const text_end = _kimg_text_end;
    const void *const rodata_start = _kimg_rodata_start;
    const void *const rodata_end = _kimg_rodata_end;
    const void *const data_start = _kimg_data_start;

    void *heap_start;
    void *heap_end;

    uintptr_t pma;
    const void *pp;

    trace("%s()", __func__);

    assert(RAM_START == _kimg_start);

    kprintf("           RAM: [%p,%p): %zu MB\n",
            RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    kprintf("  Kernel image: [%p,%p)\n", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)

    if (MEGA_SIZE < _kimg_end - _kimg_start)
        panic(NULL);

    // Initialize main page table with the following direct mapping:
    //
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB

    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void *)pma, PTE_R | PTE_W | PTE_G);

    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE)
    {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE)
    {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE)
    {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE)
    {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void *)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN)
    {
        heap_end += ROUND_UP(
            HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end)
        panic("out of memory");

    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    kprintf("Heap allocator: [%p,%p): %zu KB free\n",
            heap_start, heap_end, (heap_end - heap_start) / 1024);

    // TODO: Initialize the free chunk list here
    // free_chunk_list = kmalloc(sizeof(struct page_chunk));
    free_chunk_list = heap_end;

    free_chunk_list->pagecnt = (RAM_END - heap_end) / PAGE_SIZE; // Question about the pagecount and next
    // kprintf("\nPagecount:%d\n", free_chunk_list->pagecnt);
    free_chunk_list->next = NULL;

    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}

// Reads satp to retrieve tag for active memory space.
mtag_t active_mspace(void)
{
    return active_space_mtag();
}

// Switches the active memory space by writing the satp register.
mtag_t switch_mspace(mtag_t mtag)
{
    mtag_t prev;

    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}

// Copies all pages and page tables from the active memory space into newly allocated memory.
// ASK OH: DO WE NEED THIS FOR CKPT2?
mtag_t clone_active_mspace(void)
{
    void *pp2;
    void *pp1;
    void *pp0;
    void *leaf_page;
    struct pte *level_2_pte = active_space_ptab();
    mtag_t new_mtag;

    pp2 = alloc_phys_page();
    memset(pp2, 0, PAGE_SIZE);
    struct pte *new_main_pte = (struct pte *)pp2;
    new_mtag = ptab_to_mtag(new_main_pte, 0);

    for (int i = 0; i < PTE_CNT; i++)
    {
        if (!PTE_VALID(level_2_pte[i]))
        {
            new_main_pte[i] = null_pte();
            continue;
        }
        else if (PTE_GLOBAL(level_2_pte[i]))
        {
            new_main_pte[i] = level_2_pte[i];
            continue;
        }
        else
        {
            pp1 = alloc_phys_page();
            memset(pp1, 0, PAGE_SIZE);
            new_main_pte[i] = ptab_pte((struct pte *)pp1, 0);
        }
        struct pte *level_1_pte = (struct pte *)pageptr(level_2_pte[i].ppn);
        if (level_1_pte == NULL)
        {
            continue;
        }
        struct pte *new_level_1_pte = (struct pte *)pageptr(new_main_pte[i].ppn);
        for (int j = 0; j < PTE_CNT; j++)
        {

            if (!PTE_VALID(level_1_pte[j]))
            {
                level_1_pte[j] = null_pte();
                continue;
            }
            pp0 = alloc_phys_page();
            memset(pp0, 0, PAGE_SIZE);
            new_level_1_pte[j] = ptab_pte((struct pte *)pp0, 0);
            struct pte *level_0_pte = (struct pte *)pageptr(level_1_pte[j].ppn);
            if (level_0_pte == NULL)
            {
                continue;
            }
            struct pte *new_level_0_pte = (struct pte *)pageptr(new_level_1_pte[j].ppn);

            for (int k = 0; k < PTE_CNT; k++)
            {
                if (!PTE_VALID(level_0_pte[k]))
                {
                    level_0_pte[k] = null_pte();
                    continue;
                }
                leaf_page = alloc_phys_page();
                memset(leaf_page, 0, PAGE_SIZE);
                memcpy(leaf_page, pageptr(level_0_pte[k].ppn), PAGE_SIZE);
                new_level_0_pte[k] = leaf_pte(leaf_page, level_0_pte[k].flags);
                sfence_vma();
            }
            sfence_vma();
        }
        sfence_vma();
    }
    return new_mtag;
}

// Unmaps and frees all non-global pages from the active memory space.
void reset_active_mspace(void)
{
    struct pte *level_2_pte = active_space_ptab();
    for (int i = 0; i < PTE_CNT; i++)
    {
        if (!PTE_VALID(level_2_pte[i]) || PTE_GLOBAL(level_2_pte[i]))
        {
            continue;
        }
        struct pte *level_1_pte = (struct pte *)pageptr(level_2_pte[i].ppn);
        for (int j = 0; j < PTE_CNT; j++)
        {
            if (!PTE_VALID(level_1_pte[j]) || PTE_GLOBAL(level_1_pte[j]))
            {
                continue;
            }
            struct pte *level_0_pte = (struct pte *)pageptr(level_1_pte[j].ppn);
            for (int k = 0; k < PTE_CNT; k++)
            {
                if (!PTE_VALID(level_0_pte[k]) || PTE_GLOBAL(level_0_pte[k]))
                {
                    continue;
                }
                memset(pageptr(level_0_pte[k].ppn), 0, PAGE_SIZE);
                free_phys_page(pageptr(level_0_pte[k].ppn));
                level_0_pte[k] = null_pte();
                sfence_vma();
            }
            if (pt_empty(level_0_pte))
            {
                memset(level_0_pte, 0, PAGE_SIZE);
                free_phys_page(level_0_pte);
                level_1_pte[j] = null_pte();
                // free_phys_page(pageptr(level_1_pte[j].ppn));
                sfence_vma();
            }
        }
        if (pt_empty(level_1_pte))
        {
            memset(level_1_pte, 0, PAGE_SIZE);
            free_phys_page(level_1_pte);
            level_2_pte[i] = null_pte();
            sfence_vma();
        }
        // free_phys_page(pageptr(level_2_pte[i].ppn));
    }
    sfence_vma();
    return;
}

// Switches memory spaces to main, unmaps and frees all non-global pages from the previously active memory space.
mtag_t discard_active_mspace(void)
{
    reset_active_mspace();
    switch_mspace(main_mtag);
    return main_mtag;
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.

// Adds page with provided virtual memory address and flags to page table.
// Parameters
// vma	Virtual memory address for page (must be a PAGE_SIZE increment)
// pp	Pointer to page to be added to page table
// rwxug_flags	Flags to set on page
// Returns
// Newly mapped virtual memory address

void *map_page(uintptr_t vma, void *pp, int rwxug_flags)
{
    // Main PT2, OH Question do we have to change the vma mapping, why do we return vma

    if (!(wellformed(vma)))
    {
        return NULL;
    }

    vma = ROUND_DOWN(vma, PAGE_SIZE);

    struct pte *pt1;
    struct pte *pt0;
    struct pte *level_2_pte = active_space_ptab();

    if (PTE_VALID(level_2_pte[VPN2(vma)]))
    {
        // uintptr_t pt1_ppn = main_pt2[VPN2(vma)].ppn; // Make sure to make a new entry if the pointer to the subtable dosen't exist
        // uintptr_t pt1_pma = pt1_ppn << PPN_TO_PMA;
        pt1 = (struct pte *)pageptr(level_2_pte[VPN2(vma)].ppn);
    }
    else
    {
        void *pp1 = alloc_phys_page();
        // kprintf("\nWas PTE: %p\n", pp1);
        memset(pp1, 0, PAGE_SIZE);
        // are we supposed to do something to pp here or can we just use it?
        level_2_pte[VPN2(vma)] = ptab_pte((const struct pte *)pp1, 0);
        // uintptr_t pt1_ppn = main_pt2[VPN2(vma)].ppn;
        // uintptr_t pt1_pma = pt1_ppn << PPN_TO_PMA;
        pt1 = (struct pte *)pageptr(level_2_pte[VPN2(vma)].ppn);
        // pt1 = (struct pte *)pp1;
    }
    if (PTE_VALID(pt1[VPN1(vma)]))
    {
        // uintptr_t pt0_ppn = pt1[VPN1(vma)].ppn;
        // uintptr_t pt0_pma = pt0_ppn << PPN_TO_PMA;
        pt0 = (struct pte *)pageptr(pt1[VPN1(vma)].ppn);
    }
    else
    {
        void *pp0 = alloc_phys_page();
        // kprintf("\nWas PTE: %p\n", pp0);
        memset(pp0, 0, PAGE_SIZE);
        // pt1[VPN2(vma)] = ptab_pte(&main_pt2[VPN2(vma)], 0);
        pt1[VPN1(vma)] = ptab_pte((const struct pte *)pp0, 0);
        // uintptr_t pt0_ppn = pt1[VPN1(vma)].ppn;
        // uintptr_t pt0_pma = pt0_ppn << PPN_TO_PMA;
        pt0 = (struct pte *)pageptr(pt1[VPN1(vma)].ppn);
        // pt0 = (struct pte *)pp0;
    }
    // Do we check if leaf exists and return it and use it to convert between vma to pma
    // Throw error if leaf already exists?
    if (PTE_VALID(pt0[VPN0(vma)]))
    {
        sfence_vma();
        return (void *)vma;
    }
    // DO WE NEED THIS ^
    pt0[VPN0(vma)] = leaf_pte(pp, rwxug_flags);
    sfence_vma();
    return (void *)vma; //(void *)&pt0[VPN0(vma)];
}
struct pte *find_physical_page(uintptr_t vma)
{
    vma = ROUND_DOWN(vma, PAGE_SIZE);
    if (!(wellformed(vma)))
    {
        return NULL;
    }
    struct pte *pt1;
    struct pte *pt0;
    struct pte *level_2_pte = active_space_ptab();
    if (PTE_VALID(level_2_pte[VPN2(vma)]))
    {
        // uintptr_t pt1_ppn = main_pt2[VPN2(vma)].ppn; // Make sure to make a new entry if the pointer to the subtable dosen't exist
        // uintptr_t pt1_pma = pt1_ppn << 12;
        pt1 = (struct pte *)pageptr(level_2_pte[VPN2(vma)].ppn);
    }
    if (PTE_VALID(pt1[VPN1(vma)]))
    {
        // uintptr_t pt0_ppn = pt1[VPN1(vma)].ppn;
        // uintptr_t pt0_pma = pt0_ppn << 12;
        pt0 = (struct pte *)pageptr(pt1[VPN1(vma)].ppn);
        return pt0;
    }
    // Do we check if leaf exists and return it and use it to convert between vma to pma
    // Throw error if leaf already exists?
    // kprintf("%d\n", pt0[VPN0(vma)].flags);
    // return &pt0[VPN0(vma)];
    return NULL;
}

struct pte *find_pte_level_1(uintptr_t vma)
{
    if (!(wellformed(vma)))
    {
        return NULL;
    }
    struct pte *pt1;
    // struct pte *pt0;
    struct pte *level_2_pte = active_space_ptab();
    if (PTE_VALID(level_2_pte[VPN2(vma)]))
    {
        // uintptr_t pt1_ppn = main_pt2[VPN2(vma)].ppn; // Make sure to make a new entry if the pointer to the subtable dosen't exist
        // uintptr_t pt1_pma = pt1_ppn << 12;
        pt1 = (struct pte *)pageptr(level_2_pte[VPN2(vma)].ppn);
        return pt1;
    }
    // if (PTE_VALID(pt1[VPN1(vma)]))
    // {
    //     // uintptr_t pt0_ppn = pt1[VPN1(vma)].ppn;
    //     // uintptr_t pt0_pma = pt0_ppn << 12;
    //     pt0 = (struct pte *)pageptr(pt1[VPN1(vma)].ppn);
    //     return pt0;
    // }
    return NULL;
}

struct pte *find_pte_level_2(uintptr_t vma)
{
    if (!(wellformed(vma)))
    {
        return NULL;
    }
    // struct pte *pt1;
    struct pte *level_2_pte = active_space_ptab();
    if (PTE_VALID(level_2_pte[VPN2(vma)]))
    {
        // uintptr_t pt1_ppn = main_pt2[VPN2(vma)].ppn; // Make sure to make a new entry if the pointer to the subtable dosen't exist
        // uintptr_t pt1_pma = pt1_ppn << 12;
        // pt1 = (struct pte *)pt1_pma;
        return pageptr(level_2_pte[VPN2(vma)].ppn);
    }
    return NULL;
}

void set_pte_flags(uintptr_t vma, int rwxug_flags)
{
    struct pte *entry = find_physical_page(vma);
    if (entry == NULL)
    {
        return;
    }
    // assert(entry != -EINVAL);
    entry[VPN0(vma)].flags &= ~(PTE_R | PTE_W | PTE_X | PTE_U | PTE_G);
    entry[VPN0(vma)].flags |= rwxug_flags;
}

void *map_range(uintptr_t vma, size_t size, void *pp, int rwxug_flags)
{
    vma = ROUND_DOWN(vma, PAGE_SIZE); // ask in OH if we need to round up the vma to match with page size intially
    for (size_t i = 0; i < size; i += PAGE_SIZE)
    {
        void *mappedvma = map_page((uintptr_t)(vma + (i)), (void *)(pp + (i)), rwxug_flags);
        if (mappedvma == NULL)
        {
            return NULL;
        }
    }
    return (void *)vma;
}

// Allocates memory for and maps a range of pages starting at provided virtual memory address. Rounds up size to be a multiple of PAGE_SIZE.

// For this the virtual addresses will be contiguous but the physical addresses may not necessarily be

void *alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags)
{
    size_t rsize = ROUND_UP(size, PAGE_SIZE);
    void *pp; // = (struct page_chunk *)alloc_phys_pages(rsize / PAGE_SIZE);
    // void *mapped_range = map_range(vma, rsize, pp, rwxug_flags);
    // if (mapped_range == NULL)
    // {
    //     return NULL;
    // }
    for (size_t i = 0; i < rsize; i += PAGE_SIZE)
    {
        pp = alloc_phys_page();
        void *mappedvma = map_page((uintptr_t)(vma + (i)), (void *)(pp), rwxug_flags);
        if (mappedvma == NULL)
        {
            return NULL;
        }
    }
    return (void *)vma;
}

// Sets passed flags for pages in range. Rounds up size to be a multiple of PAGE_SIZE.

// Parameters
// vp	Virtual memory address to begin setting flags at (must be a multiple of PAGE_SIZE)
// size	Size (in bytes) of range
// rwxug_flags	Flags to set
// Returns
// None

void set_range_flags(const void *vp, size_t size, int rwxug_flags)
{
    // OH Question they pass in a virtual memory address what does that mean? can we just read from there?
    size_t rsize = ROUND_UP(size, PAGE_SIZE);
    // struct pte *mapped_range = map_range(vp, rsize, NULL, rwxug_flags);
    for (size_t i = 0; i < rsize; i += PAGE_SIZE) // How do we change vma? do we have to + i
    {                                             // Should it map continous vma to continuos pma
        set_pte_flags((uintptr_t)(vp + i), rwxug_flags);
    }
    return;
}

// Unmaps a range of pages starting at provided virtual memory address and frees the pages. Rounds up size to be a multiple of PAGE_SIZE.
// Parameters
// vp	Virtual memory address to begin unmapping at (must be a multiple of PAGE_SIZE)
// size	Size (in bytes) of range
// Returns
// None
void unmap_and_free_range(void *vp, size_t size)
{
    struct pte *entry;
    // struct pte *pte_page;
    struct pte *pt1_table;
    struct pte *pt2_table;

    if ((uint64_t)vp % PAGE_SIZE != 0)
    {
        return;
    }
    size_t rsize = ROUND_UP(size, PAGE_SIZE);
    for (size_t i = 0; i < rsize; i += PAGE_SIZE)
    {
        void *newvp = (void *)(vp + i);
        entry = (struct pte *)find_physical_page((uintptr_t)newvp);
        if (entry == NULL)
        {
            return;
        }
        if (PTE_LEAF(entry[VPN0((uintptr_t)newvp)]))
        {
            void *pp = pageptr(entry[VPN0((uintptr_t)newvp)].ppn);
            free_phys_page(pp); // Should it just be pageptr(entry[VPN0((uintptr_t)newvp)].ppn)
            entry[VPN0((uintptr_t)newvp)] = null_pte();
            sfence_vma();
            pt1_table = find_pte_level_1((uintptr_t)newvp); // ask in OH
            if (pt1_table == NULL)
            {
                return;
            }
            if (pt_empty(pageptr(pt1_table[VPN1((uintptr_t)newvp)].ppn)))
            {
                free_phys_page(pageptr(pt1_table[VPN1((uintptr_t)newvp)].ppn));
                // change this null pte logic.
                pt1_table[VPN1((uintptr_t)newvp)] = null_pte();
                sfence_vma();

                pt2_table = active_space_ptab();
                if (pt2_table == NULL)
                {
                    return;
                }
                if (pt_empty(pageptr(pt2_table[VPN2((uintptr_t)newvp)].ppn)))
                {
                    free_phys_page(pageptr(pt2_table[VPN2((uintptr_t)newvp)].ppn));
                    pt2_table[VPN2((uintptr_t)newvp)] = null_pte();
                    sfence_vma();
                }
            }
        }
    }
}

int pt_empty(struct pte *pt_start)
{

    for (int i = 0; i < PTE_CNT; i++)
    {
        // Why is this load page fault?
        if (PTE_VALID(pt_start[i]))
        {
            return 0;
        }
    }
    return 1;
}

void *alloc_phys_page(void)
{
    return alloc_phys_pages(1);
}

void free_phys_page(void *pp)
{
    free_phys_pages(pp, 1);
}

// search through chunk list to find the smallest chunk that can accommodate cnt pages
// When  find a suitable chunk, need to:
// If the chunk size exactly matches cnt, remove it from the list
// If the chunk is larger, split it and return a chunk of size cnt
// Return a pointer to the physical memory address

void *alloc_phys_pages(unsigned int cnt)
{
    // int pie = disable_interrupts();
    struct page_chunk *curr = free_chunk_list;
    struct page_chunk *prev = NULL;
    while (curr != NULL)
    {
        if (curr->pagecnt == cnt)
        {
            if (prev == NULL)
            {
                free_chunk_list = curr->next;
            }
            else
            {
                prev->next = curr->next;
            }
            // restore_interrupts(pie);
            return (void *)curr;
        }
        prev = curr;
        curr = curr->next;
    }

    curr = free_chunk_list;
    prev = NULL;
    struct page_chunk *target = NULL;
    struct page_chunk *targetprev = NULL;

    while (curr != NULL)
    {
        if (curr->pagecnt > cnt)
        {
            if (target == NULL)
            {
                target = curr;
                targetprev = prev;
            }
            else if (curr->pagecnt < target->pagecnt)
            {
                target = curr;
                targetprev = prev;
            }
        }
        prev = curr;
        curr = curr->next;
    }
    // kprintf("\nPage count: %d\n", free_phys_page_count());
    // print_chunklist();
    struct page_chunk *chunk = (void *)target + (cnt * PAGE_SIZE); // issue for some reason
    // if (chunk == NULL || target == NULL)
    // {
    //     kprintf("HELLO BROKE\n");
    // }
    chunk->pagecnt = target->pagecnt - cnt;

    chunk->next = target->next;
    // What are we supposed to do here?
    // if prev is null
    if (targetprev == NULL)
    {
        free_chunk_list = chunk;
        // restore_interrupts(pie);
        return target;
    }

    targetprev->next = chunk;
    // restore_interrupts(pie);
    return target;
}

// adds a chunk of cnt pages starting at physical address pp back to the free list
// need to maintain list in a way that makes sense for allocation strategy

void free_phys_pages(void *pp, unsigned int cnt)
{
    // int pie = disable_interrupts();
    if (pp == NULL)
    {
        // restore_interrupts(pie);
        return;
    }
    if (cnt == 0)
    {
        // restore_interrupts(pie);
        return;
    }
    memset(pp, 0, PAGE_SIZE);
    struct page_chunk *chunk = (struct page_chunk *)pp;
    chunk->pagecnt = cnt;

    // need to insert back to the free list order by adderess?
    if (free_chunk_list == NULL)
    {
        chunk->next = NULL;
        free_chunk_list = chunk;
        // restore_interrupts(pie);
        return;
    }

    else if (chunk < free_chunk_list)
    {
        chunk->next = free_chunk_list;
        free_chunk_list = chunk;
        // restore_interrupts(pie);
        return;
    }

    struct page_chunk *curr = free_chunk_list->next;
    struct page_chunk *prev = free_chunk_list;
    while (curr != NULL && curr < chunk)
    {
        prev = curr;
        curr = curr->next;
    }
    prev->next = chunk;
    chunk->next = curr;
    sfence_vma();
    // restore_interrupts(pie);
}

unsigned long free_phys_page_count(void)
{
    int count = 0;
    struct page_chunk *curr = free_chunk_list;
    while (curr != NULL)
    {
        count += curr->pagecnt;
        curr = curr->next;
    }
    return count;
}

// Called by handle_umode_exception() in excp.c to handle U mode load and store page faults.
// It returns 1 to indicate the fault has been handled (the instruction should be restarted) and 0 to indicate that the page fault
// is fatal and the process should be terminated.
int handle_umode_page_fault(struct trap_frame *tfr, uintptr_t vma)
{

    if (vma >= UMEM_START_VMA && vma < UMEM_END_VMA)
    {
        // kprintf("\nVMA: %llu", vma);
        void *pp = alloc_phys_page();
        if (pp == NULL)
        {
            return 0;
        }
        void *map = map_page(vma, pp, PTE_R | PTE_W | PTE_U);
        if (map == NULL)
        {
            free_phys_page(pp);
            return 0;
        }
        return 1;
    }
    return 0; // not handled
}

mtag_t active_space_mtag(void)
{
    return csrr_satp();
}

static inline mtag_t ptab_to_mtag(struct pte *ptab, unsigned int asid)
{
    return (
        ((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
        ((unsigned long)asid << RISCV_SATP_ASID_shift) |
        pagenum(ptab) << RISCV_SATP_PPN_shift);
}

static inline struct pte *mtag_to_ptab(mtag_t mtag)
{
    return (struct pte *)((mtag << 20) >> 8);
}

static inline struct pte *active_space_ptab(void)
{
    return mtag_to_ptab(active_space_mtag());
}

static inline void *pageptr(uintptr_t n)
{
    return (void *)(n << PAGE_ORDER);
}

static inline unsigned long pagenum(const void *p)
{
    return (unsigned long)p >> PAGE_ORDER;
}

static inline int wellformed(uintptr_t vma)
{
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits + 1));
}

static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags)
{
    return (struct pte){
        .flags = rwxug_flags | PTE_A | PTE_D | PTE_V,
        .ppn = pagenum(pp)};
}

static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag)
{
    return (struct pte){
        .flags = g_flag | PTE_V,
        .ppn = pagenum(pt)};
}

static inline struct pte null_pte(void)
{
    return (struct pte){};
}

void print_chunklist(void)
{
    struct page_chunk *tmp = free_chunk_list;
    while (tmp != NULL)
    {
        // if (tmp->pagecnt == -9)
        // {
        //     break;
        //     return;
        // }
        kprintf("Node: %p, ", tmp);
        kprintf("Page Count:%d\n", tmp->pagecnt);

        tmp = tmp->next;
    }
    return;
}