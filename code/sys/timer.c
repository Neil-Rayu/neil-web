//  timer.c
//
//  Copyright (c) 2024-2025 University of Illinois
//  SPDX-License-identifier: NCSA
//

#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "assert.h"
#include "intr.h"
#include "conf.h"
#include "string.h"
#include "see.h" // for set_stcmp

//  EXPORTED GLOBAL VARIABLE DEFINITIONS
//  

char timer_initialized = 0;

//  INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm *sleep_list;
struct alarm interruptAlrm;

//  INTERNAL FUNCTION DECLARATIONS
//

//  EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void)
{
    if(timer_initialized == 1){
        return;
    }
    set_stcmp(UINT64_MAX);
    alarm_init(&interruptAlrm, "interrupter");
    alarm_sleep_ms(&interruptAlrm, 20);
    timer_initialized = 1;
}

// void interrupter(void)
// {
//     struct alarm al;

//     alarm_init(&al, "interrupter");

//     for (;;)
//     {
//         // alarm_sleep_us(&al, 100);
//         alarm_sleep_ms(&al, 5);
//     }
// }

// void start_interrupter(void)
// {
//     // thread_spawn("interrupter", &interrupter);
//     alarm_init(&interruptAlrm, "interrupter");

//     alarm_sleep_us(&interruptAlrm, 2000);
// }

// void alarm_init(struct alarm * al, const char * name)
// Inputs: struct alarm * al - pointer to the alarm object to initialize the members of, const char * name - name of the alarm
// Outputs: None
// Description: This function intializes the alarm object and all of its fields. It intializes the conditions of the alarm and sets the twake of the alarm to the recent time in ticks.
// Side Effects: Changes the alarm's condition and the fields inside of alarm

void alarm_init(struct alarm *al, const char *name)
{
    //  FIXME your code goes here
    if (name == NULL)
    { // if the name is NULL, then set the name to "alarm"
        name = "alarm";
    }
    condition_init(&al->cond, name); // Intializes the alarm object and all of its fields
    al->next = NULL;
    al->twake = rdtime();
}

// void alarm_sleep(struct alarm * al, unsigned long long tcnt)
// Inputs: struct alarm * al - pointer to the alarm object to initialize the members of, unsigned long long tcnt - the number of ticks to put the thread to sleep
// Outputs: None
// Description: This function has the current thread sleep. It makes sure the thread is put to sleep until the correct time in ticks at which the sleep duration has ended. It adds an alarm to the sleep list, sets the interrupt expiry time, and enables new timer interrupts.
// Side Effects: Changes the sleep list and calls condition wait

void alarm_sleep(struct alarm *al, unsigned long long tcnt)
{
    unsigned long long now;
    struct alarm *prev;
    int pie;

    now = rdtime();

    //  If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;

    //  If the wake-up time has already passed, return

    if (al->twake < now)
        return;

    //  FIXME your code goes here

    pie = disable_interrupts(); // disables interrupts here for critical section as I am modifiying the sleep_list and eventually going to condition_wait

    if (sleep_list == NULL)
    { // if there is nothing in the sleep_list, add the alarm in the sleep_list
        sleep_list = al;
    }
    else if (al->twake < sleep_list->twake)
    { // if the alarm's twake is less than the sleep_list's twake, insert the alarm at the sleep_list spot and make that alarm the new head of the sleep_list
        al->next = sleep_list;
        sleep_list = al;
    }
    else
    {
        struct alarm *curr = sleep_list;
        prev = curr;
        curr = curr->next;
        while (curr != NULL && curr->twake < al->twake)
        { // look through the sleep_list until you find where the alarm's twake is less than the curr's twake and insert a node at that spot of the sleep_list
            prev = curr;
            curr = curr->next;
        }
        prev->next = al;
        al->next = curr;
    }

    set_stcmp(sleep_list->twake); // set the interrupt expiry time
    csrs_sie(RISCV_SIE_STIE);     // enable timer interrupts
    if (strcmp(al->cond.name, "interrupter") != 0)
    {
        condition_wait(&al->cond); // condition wait to sleep the alarm
    }
    restore_interrupts(pie); // restores interrupts as it has reached the end of the critical section
}

//  Resets the alarm so that the next sleep increment is relative to the time
//  alarm_reset is called.

// void alarm_reset(struct alarm * al)
// Inputs: struct alarm * al - pointer to the alarm object to initialize the members of
// Outputs: None
// Description: This function resets the alarm against the epoch. It updates the twake of the alarm to the current time of ticks.
// Side Effects: None

void alarm_reset(struct alarm *al)
{
    al->twake = rdtime(); // sets the alarm's twake to the current time in ticks
}

void alarm_sleep_sec(struct alarm *al, unsigned int sec)
{
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm *al, unsigned long ms)
{
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm *al, unsigned long us)
{
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec)
{
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms)
{
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us)
{
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}

//  handle_timer_interrupt() is dispatched from intr_handler in intr.c

// void handle_timer_interrupt(void)
// Inputs: None
// Outputs: None
// Description: This function services the timer interrupts. It is an ISR and handle timer interrupts by waking all ready threads and updates the sleep_list and mtimecmp registers
// Side Effects: Changes the sleep list and calls condition broadcast

void handle_timer_interrupt(void)
{
    struct alarm *next;
    uint64_t now;
    int pie;

    now = rdtime();

    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());

    //  FIXME your code goes here

    pie = disable_interrupts(); // disables interrupts here for critical section as I am modifiying the condition's wait list
    while (sleep_list != NULL && sleep_list->twake < now)
    {
        if (strcmp(sleep_list->cond.name, "interrupter") == 0)
        {
            alarm_reset(sleep_list);
            next = sleep_list->next;
            sleep_list->next = NULL; // removing the alarms from the wait list
            sleep_list = next;
            alarm_sleep_ms(&interruptAlrm, 20);
        }
        else
        {
            condition_broadcast(&sleep_list->cond); // wake up the alarms using condition broadcast
            next = sleep_list->next;
            sleep_list->next = NULL; // removing the alarms from the wait list
            sleep_list = next;
        }
    }
    // sleep_list = head;
    restore_interrupts(pie); // restores interrupts as it has reached the end of the critical section

    if (sleep_list != NULL)
    {
        set_stcmp(sleep_list->twake); //  set the timer interrupt threshold for waiting for the next wake-up event on the sleep_list
    }
    else
    {
        csrc_sie(RISCV_SIE_STIE); // disables timer interrupts
    }
}