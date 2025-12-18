// process.c - user process
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PROCESS_TRACE
#define TRACE
#endif

#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "process.h"
#include "elf.h"
#include "fs.h"
#include "io.h"
#include "string.h"
#include "thread.h"
#include "riscv.h"
#include "trap.h"
#include "memory.h"
#include "heap.h"
#include "error.h"
#include "timer.h"
#include "intr.h"

// COMPILE-TIME PARAMETERS
//

#ifndef NPROC
#define NPROC 16
#endif

// INTERNAL FUNCTION DECLARATIONS
//

static int build_stack(void *stack, int argc, char **argv);

static void fork_func(struct condition *forked, struct trap_frame *tfr);

// INTERNAL GLOBAL VARIABLES
//

static struct process main_proc;

static struct process *proctab[NPROC] = {
    &main_proc};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void)
{
    assert(memory_initialized && heap_initialized);
    assert(!procmgr_initialized);

    main_proc.idx = 0;
    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    // main_proc.iotab[0] = create_null_io();
    procmgr_initialized = 1;
    timer_init();
}

int process_exec(struct io *exeio, int argc, char **argv)
{
    void *stack = alloc_phys_page();
    void (*eptr)(void) = 0;
    int size = build_stack(stack, argc, argv); // get the trap frame from the stack
    reset_active_mspace();
    map_page(UMEM_END_VMA - PAGE_SIZE, stack, PTE_R | PTE_W | PTE_U);
    sfence_vma();
    elf_load(exeio, &eptr);
    struct trap_frame *tfr = kcalloc(1, sizeof(struct trap_frame));
    tfr->sepc = eptr;
    tfr->a0 = argc;                // a0 stores the count
    tfr->a1 = UMEM_END_VMA - size; // a1 stores the location of the start of the trap frame
    tfr->sp = (void *)(UMEM_END_VMA - size);
    tfr->sstatus = csrr_sstatus();
    tfr->sstatus |= RISCV_SSTATUS_SPIE;
    tfr->sstatus &= ~(RISCV_SSTATUS_SPP);
    trap_frame_jump(tfr, get_stack_anchor());
    return 0;
}

int process_fork(const struct trap_frame *tfr)
{
    // kprintf("PAGE COUNT: %d\n", free_phys_page_count());
    for (int i = 0; i < NPROC; i++)
    {
        if (proctab[i] == NULL)
        {
            struct process *proc = kcalloc(1, sizeof(struct process));
            for (int j = 0; j < PROCESS_IOMAX; j++)
            {
                if (current_process()->iotab[j] == NULL)
                {
                    proc->iotab[j] = NULL;
                }
                else
                {
                    proc->iotab[j] = ioaddref(current_process()->iotab[j]);
                }
            }
            proc->mtag = clone_active_mspace();
            // struct condition * forked = kcalloc(1, sizeof(struct condition));
            // condition_init(forked, "forked");
            // Ask OH Can we only do it with condition?
            // Why can we not access this??
            proc->idx = i;
            struct trap_frame *child_tfr = kcalloc(1, sizeof(struct trap_frame));
            memcpy(child_tfr, tfr, sizeof(struct trap_frame));
            // Ask OH is there any requirment on condition or child tfr?
            proc->tid = thread_spawn("newthr", (void *)&fork_func, NULL, child_tfr);
            thread_set_process(proc->tid, proc);
            // kprintf("Fork set process %p for thread %d\n", proc, proc->tid);
            proctab[i] = proc;
            return proc->tid;
        }
    }
    return -EINVAL;
}

void process_exit(void)
{
    struct process *proc = running_thread_process();
    fsflush();
    if (proc->tid == 0)
    {
        panic("Main process exited.");
    }
    // kprintf("PRE-DISCARD: %d\n", free_phys_page_count());
    discard_active_mspace();
    // kprintf("POST-DISCARD: %d\n", free_phys_page_count());
    for (int i = 0; i < PROCESS_IOMAX; i++)
    {
        if (proc->iotab[i] != NULL)
        {
            ioclose(proc->iotab[i]);
        }
    }
    proctab[proc->idx] = NULL;
    thread_exit();
}

// INTERNAL FUNCTION DEFINITIONS
//

int build_stack(void *stack, int argc, char **argv)
{
    size_t stksz, argsz;
    uintptr_t *newargv;
    char *p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char *) - 1 < argc)
        return -ENOMEM;

    stksz = (argc + 1) * sizeof(char *);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++)
    {
        argsz = strlen(argv[i]) + 1;
        if (PAGE_SIZE - stksz < argsz)
            return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert(stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz;
    p = (char *)(newargv + argc + 1);

    for (i = 0; i < argc; i++)
    {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void *)p - (void *)stack);
        argsz = strlen(argv[i]) + 1;
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0;
    return stksz;
}

void fork_func(struct condition *done, struct trap_frame *tfr)
{
    tfr->a0 = 0;
    trap_frame_jump(tfr, get_stack_anchor());
    return;
}