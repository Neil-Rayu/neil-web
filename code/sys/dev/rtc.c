// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#define RTC_BLKSZ 8
#define RTC_SHIFT 32

#include "conf.h"
#include "assert.h"
#include "rtc.h"
#include "device.h"
#include "ioimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    // TODO
    uint32_t time_low;
    uint32_t time_high;
};

struct rtc_device {
    // TODO
    struct rtc_regs *regs;
    struct io rtcio;
    int instno;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct io ** ioptr, void * aux);
static void rtc_close(struct io * io);
static int rtc_cntl(struct io * io, int cmd, void * arg);
static long rtc_read(struct io * io, void * buf, long bufsz);

static uint64_t read_real_time(struct rtc_regs * regs);

// EXPORTED FUNCTION DEFINITIONS
// 

// void rtc_attach(void * mmio_base)
// Inputs: void * mmio_base -  pointer to the base address of the memory-mapped RTC registers
// Outputs: None
// Description: This function registers the device, sets up the I/O interface and its memory-mapped registers. The function will dynamically allocate memory for the RTC device structure and intialize the IO subsystem. Then the device is registered with the system under the "rtc" name.
// Side Effects: Changes the device table and allocates a new device

void rtc_attach(void * mmio_base) {
    // TODO

    if(mmio_base == NULL){
        panic("Bad arguments into rtc_attach");
    }

    static const struct iointf rtc_iointf = { // intializes the IO subsystem and contains the fucntion pointers for device operations
        .cntl = &rtc_cntl,
        .read = &rtc_read,
        .close = &rtc_close
    };

    struct rtc_device *rtc;
    rtc = kcalloc(1,sizeof(struct rtc_device)); // Allocates memory for the new RTC device struct

    if(rtc == NULL){
        panic("rtc failed to kcalloc");
    }

    rtc->regs = mmio_base;
    rtc->rtcio.intf = (struct iointf*) &rtc_iointf; // Intializes the device structure
    rtc->rtcio.refcnt = 0; // Registers the RTC device into the system
    rtc->instno = register_device("rtc", rtc_open, rtc);
}

// int rtc_open(struct io ** ioptr, void * aux)
// Inputs: struct io ** ioptr - pointer to store the reference to the RTC IO structure for an RTC instance, void * aux - pointer to the RTC device structure
// Outputs: Returns 0 on success
// Description: This function connects an IO reference with the RTC devie to allow it to be used by other system components. The function also ensures that the device is referenced properly as well before giving control.
// Side Effects: Changes the refcount of ioptr

int rtc_open(struct io ** ioptr, void * aux) {
    // TODO
    struct rtc_device *rtc = (struct rtc_device *)aux; // Sets the auxillary pointer to RTC device struct
    if(aux == NULL || ioptr == NULL){ // Validates the parameters
        panic("Bad arguments into rtc_open");
    }

    *ioptr = &rtc->rtcio; // Set the I/O interface pointer to point to RTC's I/O structure and increments reference counter for this I/O interface
    rtc->rtcio.refcnt++;  
    return 0; 
}

// void rtc_close(struct io * io)
// Inputs: struct io * io - pointer to the IO structure associated with the RTC device
// Outputs: None
// Description: This function closes the RTC device. It also ensures that the reference count is 0 for the io struct.
// Side Effects: None

void rtc_close(struct io * io) {
    // TODO
    if(io == NULL){ // if the io pointer is NULL, then the function panics
        panic("Bad arguments into rtc_close");
    }
    assert(io->refcnt == 0);
    //kfree(rtc);
    return;
}

// int rtc_cntl(struct io * io, int cmd, void * arg)
// Inputs: struct io * io - pointer to the IO structure associated with the RTC device, int cmd - command identifier for the requested operation, void * arg - the optional argument for the command
// Outputs: Returns the block size, or -ENOTSUP if the operation isn't supported
// Description: This function handles the control and miscellaneous IO operations. It will also support anything with the block size of RTC data and check if the cmd matches with the desired command for the block size. 
// Side Effects: None

int rtc_cntl(struct io * io, int cmd, void * arg) {
    // TODO
    if(io == NULL){ // if the io pointer is NULL, then the function panics
        panic("Bad arguments into rtc_cntl");
    }

    switch (cmd){ // Handles different command controls
        case IOCTL_GETBLKSZ:
            return RTC_BLKSZ; // Return the block size for RTC operations
        break;

        default:
            return -ENOTSUP; // Return "not supported" error for unknown commands
        break;
    }
    return -ENOTSUP;
}

// long rtc_read(struct io * io, void * buf, long bufsz)
// Inputs: struct io * io - pointer to the IO structure associated with the RTC device, void * buf -  pointer to the buffer where the timestamp will be stored, long bufsz - size of the buffer in bytes
// Outputs: Returns the size of the buffer, or -EINVAL if the read fails
// Description: This function reads the current time from the RTC device and copies it into the buffer. If the buffer can't hold the 64-bit timestamp, then the function will fail. The function will calculate the base address of the rtc_device using the offset of the provided pointer. Then it should ensure that the buffer is large enough to hold the timestamp.
// Side Effects: Changes the buf and also calls read_real_time

long rtc_read(struct io * io, void * buf, long bufsz) {
    // TODO
    if(io == NULL || buf == NULL){ // Validates the parameters
        panic("Bad arguments into rtc_read");
    }

    // Get the RTC device structure from the I/O interface pointer
    struct rtc_device * rtc = (struct rtc_device*)((void *)io - offsetof(struct rtc_device, rtcio)); 

    if (bufsz < sizeof(uint64_t)){ // Ensures that the buffer size is more than 8 bytes
        return -EINVAL;
    }

     // Read the current time from RTC hardware and copies it to the buffer
    uint64_t real_time = read_real_time(((struct rtc_device *)rtc)->regs);
    memcpy(buf, &real_time, sizeof(real_time));
    return bufsz;
}

// uint64_t read_real_time(struct rtc_regs * regs)
// Inputs: struct rtc_regs * regs - pointer to the memory-mapped RTC registers
// Outputs: Returns combined 64-bit time value
// Description: This function gets the time from the registers. It will combine the time's low 32 bits and the high 32 bits to create the timestamp that we want.
// Side Effects: Modifies the regs

uint64_t read_real_time(struct rtc_regs * regs) {
    // TODO

    if(regs == NULL){
        panic("Bad arguments into read_real_time");
    }

    // Reads the lower 32 bits and upper 32 bits and then combines them into a 64-bit time value
    uint32_t low = regs->time_low;
    uint32_t high = regs->time_high;
    uint64_t res = ((uint64_t)high << RTC_SHIFT) | low;
    return res;

}