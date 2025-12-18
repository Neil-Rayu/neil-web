//  thread.c - Threads
//
//  Copyright (c) 2024-2025 University of Illinois
//  SPDX-License-identifier: NCSA
//

#ifdef THREAD_TRACE
#define TRACE
#endif

#ifdef THREAD_DEBUG
#define DEBUG
#endif

#include "thread.h"
#include "timer.h"

#include <stddef.h>
#include <stdint.h>

#include "assert.h"
#include "heap.h"
#include "string.h"
#include "riscv.h"
#include "intr.h"
#include "memory.h"
#include "error.h"
#include "process.h"

#include <stdarg.h>

//  COMPILE-TIME PARAMETERS
//

//  NTHR is the maximum number of threads

#ifndef NTHR
#define NTHR 16
#endif

#ifndef STACK_SIZE
#define STACK_SIZE 4000
#endif

//  EXPORTED GLOBAL VARIABLES
//

char thrmgr_initialized = 0;

//  INTERNAL TYPE DEFINITIONS
//

enum thread_state
{
    THREAD_UNINITIALIZED = 0,
    THREAD_WAITING,
    THREAD_RUNNING,
    THREAD_READY,
    THREAD_EXITED
};

struct thread_context
{
    uint64_t s[12];
    void *ra;
    void *sp;
};

struct thread_stack_anchor
{
    struct thread *ktp;
    void *kgp;
};

struct thread
{
    struct thread_context ctx; //  must be first member (thrasm.s)
    int id;                    //  index into thrtab[]
    enum thread_state state;
    const char *name;
    struct thread_stack_anchor *stack_anchor;
    void *stack_lowest;
    struct thread *parent;
    struct thread *list_next;
    struct condition *wait_cond;
    struct condition child_exit;
    struct process *thr_proc;
    struct
    {
        struct lock *head;
        struct lock *tail;
    } lock_list;
};

//  INTERNAL MACRO DEFINITIONS
//  

//  Pointer to running thread, which is kept in the tp (x4) register.

#define TP ((struct thread *)__builtin_thread_pointer())

//  Macro for changing thread state. If compiled for debugging (DEBUG is
//  defined), prints function that changed thread state.

#define set_thread_state(t, s)                                               \
    do                                                                       \
    {                                                                        \
        debug("Thread <%s:%d> state changed from %s to %s by <%s:%d> in %s", \
              (t)->name, (t)->id,                                            \
              thread_state_name((t)->state),                                 \
              thread_state_name(s),                                          \
              TP->name, TP->id,                                              \
              __func__);                                                     \
        (t)->state = (s);                                                    \
    } while (0)

//  INTERNAL FUNCTION DECLARATIONS
//

//  Initializes the main and idle threads. called from threads_init().

static void init_main_thread(void);
static void init_idle_thread(void);

//  Sets the RISC-V thread pointer to point to a thread.

static void set_running_thread(struct thread *thr);

//  Returns a string representing the state name. Used by debug and trace
//  statements, so marked unused to avoid compiler warnings.

static const char *thread_state_name(enum thread_state state)
    __attribute__((unused));

//  void thread_reclaim(int tid)
//
//  Reclaims a thread's slot in thrtab and makes its parent the parent of its
//  children. Frees the struct thread of the thread.

static void thread_reclaim(int tid);

//  struct thread * create_thread(const char * name)
//
//  Creates and initializes a new thread structure. The new thread is not added
//  to any list and does not have a valid context (_thread_switch cannot be
//  called to switch to the new thread).

static struct thread *create_thread(const char *name);

//  void running_thread_suspend(void)
//  Suspends the currently running thread and resumes the next thread on the
//  ready-to-run list using _thread_swtch (in threasm.s). Must be called with
//  interrupts enabled. Returns when the current thread is next scheduled for
//  execution. If the current thread is TP, it is marked READY and placed
//  on the ready-to-run list. Note that running_thread_suspend will only return if the
//  current thread becomes READY.

static void running_thread_suspend(void);

//  The following functions manipulate a thread list (struct thread_list). Note
//  that threads form a linked list via the list_next member of each thread
//  structure. Thread lists are used for the ready-to-run list (ready_list) and
//  for the list of waiting threads of each condition variable. These functions
//  are not interrupt-safe! The caller must disable interrupts before calling any
//  thread list function that may modify a list that is used in an ISR.

static void tlclear(struct thread_list *list);
static int tlempty(const struct thread_list *list);
static void tlinsert(struct thread_list *list, struct thread *thr);
static struct thread *tlremove(struct thread_list *list);
// static void tlappend(struct thread_list * l0, struct thread_list * l1);
static void llinsert(struct thread *thr, struct lock *lock);
static void llremove(struct thread *thr, struct lock *lock);
static void llclear(struct thread *thr);

static void
idle_thread_func(void);

//  IMPORTED FUNCTION DECLARATIONS
//  defined in thrasm.s
//

extern struct thread *_thread_swtch(struct thread *thr);

extern void _thread_startup(void);

//  INTERNAL GLOBAL VARIABLES
//

#define MAIN_TID 0
#define IDLE_TID (NTHR - 1)

static struct thread main_thread;
static struct thread idle_thread;

extern char _main_stack_lowest[]; //  from start.s
extern char _main_stack_anchor[]; //  from start.s

static struct thread main_thread = {
    .id = MAIN_TID,
    .name = "main",
    .state = THREAD_RUNNING,
    .stack_anchor = (void *)_main_stack_anchor,
    .stack_lowest = _main_stack_lowest,
    .child_exit.name = "main.child_exit"};

extern char _idle_stack_lowest[]; //  from thrasm.s
extern char _idle_stack_anchor[]; //  from thrasm.s

static struct thread idle_thread = {
    .id = IDLE_TID,
    .name = "idle",
    .state = THREAD_READY,
    .parent = &main_thread,
    .stack_anchor = (void *)_idle_stack_anchor,
    .stack_lowest = _idle_stack_lowest,
    .ctx.sp = _idle_stack_anchor,
    .ctx.ra = (void *)&_thread_startup,
    .ctx.s[10] = (uint64_t)&thread_exit,     // set correct s register to address of thread_exit
    .ctx.s[9] = (uint64_t)&idle_thread_func, // set correct s register to address of idle_thread_func
    //  FIXME your code goes here
};

static struct thread *thrtab[NTHR] = {
    [MAIN_TID] = &main_thread,
    [IDLE_TID] = &idle_thread};

static struct thread_list ready_list = {
    .head = &idle_thread,
    .tail = &idle_thread};

//  EXPORTED FUNCTION DEFINITIONS
//

int running_thread(void)
{
    return TP->id;
}

void *get_stack_anchor(void)
{
    return TP->stack_anchor;
}

void thrmgr_init(void)
{
    trace("%s()", __func__);
    init_main_thread();
    init_idle_thread();
    set_running_thread(&main_thread);
    thrmgr_initialized = 1;
}

// int thread_spawn(const char * name, void (*entry)(void), ...)
// Inputs: const char * name - name of a new thread, void (*entry)(void) - a function pointer to the function that the thread will start executing
// Outputs: Returns the new threads's id, or -EMTHR if you can't add any more threads
// Description: This function creates a new thread and adds it to the ready list. It also goes to the entry function with the correct arguments for entry, which ensures that the new thread ready to be executued.
// Side Effects: Changes the ready list and calls the create_thread function

int thread_spawn(
    const char *name,
    void (*entry)(void),
    ...)
{
    trace("%s()", __func__);
    struct thread *child;
    va_list ap;
    int pie;
    int i;

    child = create_thread(name);

    if (child == NULL)
        return -EMTHR;

    set_thread_state(child, THREAD_READY);

    pie = disable_interrupts();
    tlinsert(&ready_list, child);
    restore_interrupts(pie);

    //  FIXME your code goes here
    //  filling in entry function arguments is given below, the rest is up to you
    va_start(ap, entry);
    for (i = 0; i < 8; i++)
        child->ctx.s[i] = va_arg(ap, uint64_t);
    va_end(ap);

    child->ctx.s[10] = (uint64_t)&thread_exit;   // set correct s register to address of thread_exit
    child->ctx.ra = (void *)&_thread_startup;    // set ra address of thread_startup
    child->ctx.s[9] = (uint64_t)entry;           // set correct s register to address of entry
    child->ctx.sp = (void *)child->stack_anchor; // set sp to stack anchor of child

    return child->id;
}

// void thread_exit(void)
// Inputs: None
// Outputs: None
// Description: This function exits out of the currently running thread. If it is the main thread, it should halt success. Otherwise, it should say the current thread is exiting and signal to the parent that the currently running thread is going to exit.
// Side Effects: Call condition broadcast and running_thread_suspend

void thread_exit(void)
{
    trace("%s()", __func__);
    // kprintf("Fork exit process %p for thread %d\n", TP->thr_proc, TP->thr_proc->tid);
    // kprintf("\nTP ID:%s", TP->name);
    //   FIXME your code goes here
    if (TP->id == main_thread.id)
    { // if its the main thread, then it should halt success, otherwise set the TP's state to exited
        halt_success();
    }
    else
    {
        TP->state = THREAD_EXITED;
    }
    condition_broadcast(&TP->parent->child_exit); // signal to the parent that the current thread is going to exit and then call running thread suspend to suspend the current thread
    running_thread_suspend();
    halt_failure(); // if running_thread_suspend returns something, then it should halt a failure
}

void thread_yield(void)
{
    trace("%s() in <%s:%d>", __func__, TP->name, TP->id);
    running_thread_suspend();
}

// int thread_join(int tid)
// Inputs: int tid - thread id of the identified child
// Outputs: Returns the thread is of the identified child, other -EINVAL if we weren't able to find the child
// Description: This function waits for the child thread to exit. If the tid isn't 0, we have identified the child and want to see if it exists. If the child exists, then you wait until the child has exited. Otherwise, you have to go through the entire thread table to see if there is a child for the currently running thread and if that child also exists. Then you also wait until the child thread has exited.
// Side Effects: Calls thread_reclaim, calls condition_wait which is going to modify the wait list

int thread_join(int tid)
{
    trace("%s()", __func__);
    //  FIXME your code goes here
    if (tid != 0)
    {
        if (thrtab[tid] != NULL && thrtab[tid]->state == THREAD_EXITED)
        { // if the identified child exists and the state of the child is exited, then reclaim the tid and return the tid
            thread_reclaim(tid);
            return tid;
        }
        else if (thrtab[tid] != NULL)
        {
            int pie = disable_interrupts(); // disables interrupts here for critical section for the condition wait of the child exit
            while (thrtab[tid]->state != THREAD_EXITED)
            {
                condition_wait(&TP->child_exit); // otherwise condition wait until the child has exited and then reclaim the tid and return the tid
            }
            restore_interrupts(pie); // restores interrupts as it has reached the end of the critical section
            thread_reclaim(tid);
            return tid;
        }
        return -EINVAL;
    }
    else
    {
        for (int i = 1; i < NTHR; i++)
        {
            if (thrtab[i] != NULL && thrtab[i]->parent == TP && thrtab[i]->state == THREAD_EXITED)
            { // if you have found the identified child, the child exists, and the state of the child is exited, then reclaim the i and return the i
                thread_reclaim(i);
                return i;
            }
            else if (thrtab[i] != NULL && thrtab[i]->parent == TP)
            {
                int pie = disable_interrupts(); // disables interrupts here for critical section for the condition wait of the child exit
                while (thrtab[i]->state != THREAD_EXITED)
                {
                    condition_wait(&TP->child_exit); // otherwise condition wait until the child has exited and then reclaim the i and return the i
                }
                restore_interrupts(pie); // restores interrupts as it has reached the end of the critical section
                thread_reclaim(i);
                return i;
            }
        }
        return -EINVAL;
    }
}

const char *thread_name(int tid)
{
    assert(0 <= tid && tid < NTHR);
    assert(thrtab[tid] != NULL);
    return thrtab[tid]->name;
}

const char *running_thread_name(void)
{
    return TP->name;
}

void condition_init(struct condition *cond, const char *name)
{
    tlclear(&cond->wait_list);
    cond->name = name;
}

void condition_wait(struct condition *cond)
{
    int pie;

    trace("%s(cond=<%s>) in <%s:%d>", __func__,
          cond->name, TP->name, TP->id);

    assert(TP->state == THREAD_RUNNING);

    //  Insert current thread into condition wait list

    set_thread_state(TP, THREAD_WAITING);
    TP->wait_cond = cond;
    TP->list_next = NULL;

    pie = disable_interrupts();
    tlinsert(&cond->wait_list, TP);
    restore_interrupts(pie);

    running_thread_suspend();
}

// void condition_broadcast(struct condition * cond)
// Inputs: struct condition * cond - the condition that has all of the threads that we want to wake up in its wait list.
// Outputs: None
// Description: This function wakes all of the threads that are waiting on the condition variable. It adds all of the waiting threads on the wait list and makes them ready threads on the ready list. It also removes all of those threads from the wait list as well.
// Side Effects: Modifies the ready list and the wait list

void condition_broadcast(struct condition *cond)
{
    trace("%s()", __func__);
    //  FIXME your code goes here
    int pie = disable_interrupts(); // disable the interrupts as its a critical section as you're modifying a thread list
    while (tlempty(&cond->wait_list) == 0)
    {
        struct thread *ready_thread = tlremove(&cond->wait_list); // Sets all of the threads in the wait list to ready. Removes all of the threads from the wait list and adds them to the ready list
        ready_thread->state = THREAD_READY;
        tlinsert(&ready_list, ready_thread);
    }
    restore_interrupts(pie); // restores interrupts as it has reached the end of the critical section
}

void lock_init(struct lock *lock)
{
    trace("%s()", __func__);
    memset(lock, 0, sizeof(struct lock));
    condition_init(&lock->released, NULL);
    lock->count = 0;
    lock->next = NULL;
}

void lock_acquire(struct lock *lock)
{
    trace("%s()", __func__);
    if (lock->owner == TP)
    {
        lock->count++;
    }
    else
    {
        while (lock->owner != NULL)
        {
            // kprintf("Waiting:%s\n", TP->name);
            condition_wait(&lock->released);
        }
        lock->owner = TP;
        int pie = disable_interrupts();
        llinsert(TP, lock);
        restore_interrupts(pie);
    }
    // kprintf("Owner:%s\n", lock->owner->name);
}

void lock_release(struct lock *lock)
{
    trace("%s()", __func__);
    // assert(lock != NULL);
    // assert(lock->owner != NULL);
    // assert(lock->owner->id == TP->id);
    // kprintf("Releasing:%s\n", lock->owner->name);
    if (lock->count == 0)
    {
        // kprintf("Lock successfully released:%s\n", lock->owner->name);
        int pie = disable_interrupts();
        llremove(TP, lock);
        restore_interrupts(pie);
        lock->owner = NULL;
        condition_broadcast(&lock->released);
    }
    else
    {
        lock->count--;
    }
}

struct process *thread_process(int tid)
{
    assert(tid >= 0 && tid < NTHR);
    return thrtab[tid]->thr_proc;
}

void thread_set_process(int tid, struct process *proc)
{
    assert(tid >= 0 && tid < NTHR);
    thrtab[tid]->thr_proc = proc;
}

struct process *running_thread_process(void)
{
    return TP->thr_proc;
}

//  INTERNAL FUNCTION DEFINITIONS
//

void init_main_thread(void)
{
    //  Initialize stack anchor with pointer to self
    main_thread.stack_anchor->ktp = &main_thread;
}

void init_idle_thread(void)
{
    //  Initialize stack anchor with pointer to self
    idle_thread.stack_anchor->ktp = &idle_thread;
}

static void set_running_thread(struct thread *thr)
{
    asm inline("mv tp, %0" ::"r"(thr) : "tp");
}

const char *thread_state_name(enum thread_state state)
{
    static const char *const names[] = {
        [THREAD_UNINITIALIZED] = "UNINITIALIZED",
        [THREAD_WAITING] = "WAITING",
        [THREAD_RUNNING] = "RUNNING",
        [THREAD_READY] = "READY",
        [THREAD_EXITED] = "EXITED"};

    if (0 <= (int)state && (int)state < sizeof(names) / sizeof(names[0]))
        return names[state];
    else
        return "UNDEFINED";
};

void thread_reclaim(int tid)
{
    struct thread *const thr = thrtab[tid];
    int ctid;

    assert(0 < tid && tid < NTHR && thr != NULL);
    assert(thr->state == THREAD_EXITED);

    //  Make our parent thread the parent of our child threads. We need to scan
    //  all threads to find our children. We could keep a list of all of a
    //  thread's children to make this operation more efficient.

    for (ctid = 1; ctid < NTHR; ctid++)
    {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == thr)
            thrtab[ctid]->parent = thr->parent;
    }

    thrtab[tid] = NULL;
    kfree(thr);
}

struct thread *create_thread(const char *name)
{
    trace("%s()", __func__);
    struct thread_stack_anchor *anchor;
    void *stack_page;
    struct thread *thr;
    int tid;

    trace("%s(name=\"%s\") in <%s:%d>", __func__, name, TP->name, TP->id);

    //  Find a free thread slot.

    tid = 0;
    while (++tid < NTHR)
        if (thrtab[tid] == NULL)
            break;

    if (tid == NTHR)
        return NULL;

    //  Allocate a struct thread and a stack

    thr = kcalloc(1, sizeof(struct thread));

    stack_page = alloc_phys_page();
    anchor = stack_page + STACK_SIZE;
    anchor -= 1; //  anchor is at base of stack
    thr->stack_lowest = stack_page;
    thr->stack_anchor = anchor;
    anchor->ktp = thr;
    anchor->kgp = NULL;

    thrtab[tid] = thr;

    thr->id = tid;
    thr->name = name;
    thr->parent = TP;
    thr->lock_list.head = NULL;
    thr->lock_list.tail = NULL;
    return thr;
}

// void running_thread_suspend(void)
// Inputs: None
// Outputs: None
// Description: This function suspends the current one and switches to a ready thread with the help of _thread_swtch, which needs to be called with interrupts enabled. It should also free the old thread that was running before from the stack.
// Side Effects: Modifies the ready list and calls _thread_swtch. Also frees the old thread's stack

void running_thread_suspend(void)
{
    trace("%s()", __func__);
    //  FIXME your code goes here
    int pie = disable_interrupts(); // disable the interrupts as its a critical section as you're modifying a thread list
    if (TP->state == THREAD_RUNNING)
    { // if the current thread is running, then change the state of it to ready and insert it into the ready list
        TP->state = THREAD_READY;
        tlinsert(&ready_list, TP);
    }
    struct thread *next_thread = tlremove(&ready_list); // remove the next thread from the ready list and change the state of that to running
    next_thread->state = THREAD_RUNNING;
    restore_interrupts(pie); // restores interrupts as it has reached the end of the critical section

    enable_interrupts();
    if (next_thread->thr_proc != NULL)
    {
        switch_mspace(next_thread->thr_proc->mtag);
    }
    struct thread *old_thread = _thread_swtch(next_thread); // enable interrupts for the thread switch and set the old thread equal to it as it is the previous thread before you switch to the next one

    if (old_thread->state == THREAD_EXITED)
    {
        int pie = disable_interrupts();
        llclear(old_thread);
        restore_interrupts(pie);
        free_phys_page(old_thread->stack_lowest); // free the old thread's stack from the memory
    }
}

void tlclear(struct thread_list *list)
{
    trace("%s()", __func__);
    list->head = NULL;
    list->tail = NULL;
}

int tlempty(const struct thread_list *list)
{
    trace("%s()", __func__);
    return (list->head == NULL);
}

void tlinsert(struct thread_list *list, struct thread *thr)
{

    trace("%s()", __func__);
    thr->list_next = NULL;

    if (thr == NULL)
        return;
    // kprintf("stuck at 661");

    if (list->tail != NULL)
    {
        assert(list->head != NULL);
        list->tail->list_next = thr;
        // kprintf("stuck at 666");
    }
    else
    {
        assert(list->head == NULL);
        list->head = thr;
        // kprintf("stuck at 672");
    }

    list->tail = thr;
    // kprintf("stuck at 676");
}

struct thread *tlremove(struct thread_list *list)
{
    trace("%s()", __func__);
    struct thread *thr;

    thr = list->head;

    if (thr == NULL)
        return NULL;

    list->head = thr->list_next;

    if (list->head != NULL)
        thr->list_next = NULL;
    else
        list->tail = NULL;

    thr->list_next = NULL;
    return thr;
}

void llinsert(struct thread *thread, struct lock *lock)
{
    trace("%s()", __func__);
    if (lock == NULL)
        return;

    if (thread->lock_list.tail != NULL)
    {
        // assert(thread->lock_list.head != NULL);
        thread->lock_list.tail->next = lock;
        lock->next = NULL;
    }
    else
    {
        // assert(thread->lock_list.head == NULL);
        thread->lock_list.head = lock;
        lock->next = NULL;
    }

    thread->lock_list.tail = lock;
}

void llremove(struct thread *thread, struct lock *lock)
{
    trace("%s()", __func__);
    if (thread->lock_list.head == NULL)
    {
        return;
    }

    if (thread->lock_list.head == lock)
    {
        struct lock *new_head = thread->lock_list.head->next;
        thread->lock_list.head = new_head;

        if (new_head == NULL)
        {
            thread->lock_list.tail = NULL;
        }

        lock->next = NULL;
        return;
    }
    struct lock *curr = thread->lock_list.head;
    while (curr != NULL && curr->next != lock)
    {
        curr = curr->next;
    }

    if (curr != NULL && curr->next == lock)
    {
        curr->next = lock->next;

        if (lock == thread->lock_list.tail)
        {
            thread->lock_list.tail = curr;
        }

        lock->next = NULL;
    }
}
void llclear(struct thread *thread)
{
    trace("%s()", __func__);
    while (thread->lock_list.head != NULL)
    {
        for (int i = 0; i <= thread->lock_list.head->count; i++)
        {
            lock_release(thread->lock_list.head);
        }
        thread->lock_list.head = thread->lock_list.head->next;
    }
    thread->lock_list.head = NULL;
    thread->lock_list.tail = NULL;
}
//  Appends elements of l1 to the end of l0 and clears l1.

// void tlappend(struct thread_list * l0, struct thread_list * l1) {
//     if (l0->head != NULL) {
//         assert(l0->tail != NULL);

//         if (l1->head != NULL) {
//             assert(l1->tail != NULL);
//             l0->tail->list_next = l1->head;
//             l0->tail = l1->tail;
//         }
//     } else {
//         assert(l0->tail == NULL);
//         l0->head = l1->head;
//         l0->tail = l1->tail;
//     }

//     l1->head = NULL;
//     l1->tail = NULL;
// }

void idle_thread_func(void)
{
    trace("%s()", __func__);
    //  The idle thread sleeps using wfi if the ready list is empty. Note that we
    //  need to disable interrupts before checking if the thread list is empty to
    //  avoid a race condition where an ISR marks a thread ready to run between
    //  the call to tlempty() and the wfi instruction.

    for (;;)
    {
        //  If there are runnable threads, yield to them.

        while (!tlempty(&ready_list))
            thread_yield();

        //  No runnable threads. Sleep using the wfi instruction. Note that we
        //  need to disable interrupts and check the runnable thread list one
        //  more time (make sure it is empty) to avoid a race condition where an
        //  ISR marks a thread ready before we call the wfi instruction.

        disable_interrupts();
        if (tlempty(&ready_list))
            asm("wfi");
        enable_interrupts();
    }
}