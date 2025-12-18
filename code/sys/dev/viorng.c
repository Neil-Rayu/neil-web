// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "error.h"
#include "string.h"
#include "ioimpl.h"
#include "assert.h"
#include "conf.h"
#include "intr.h"
#include "console.h"
#include "thread.h"
#include "timer.h"

// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "rng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//

struct viorng_device {
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    int instno;

    struct condition viorng_buffer_condition;

    struct io io;

    struct {
        uint16_t last_used_idx;

        union {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(1)];
        };

        union {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(1)];
        };

        // The first descriptor is a regular descriptor and is the one used in
        // the avail and used rings.

        struct virtq_desc desc[1];
    } vq;

    // bufcnt is the number of bytes left in buffer. The usable bytes are
    // between buf+0 and buf+bufcnt. (We read from the end of the buffer.)

    unsigned int bufcnt;
    char buf[VIORNG_BUFSZ];
};

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_open(struct io ** ioptr, void * aux);
static void viorng_close(struct io * io);
static long viorng_read(struct io * io, void * buf, long bufsz);
static void viorng_isr(int irqno, void * aux);

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO rng device. Declared and called directly from virtio.c.

// void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno)
// Inputs: volatile struct virtio_mmio_regs * regs - pointer to the address of the memory-mapped viorng device registers, int irqno - external interrupt request source
// Outputs: None
// Description: This function attaches a VirtIO rng device. It initializes the device with the IO operation functions and sets the required bits. It also fills in the descriptor tables and attaches the avail and used structs together leading to the device being registered.
// Side Effects: Changes the device table and allocates a new device

void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    //           FIXME add additional declarations here if needed

    static const struct iointf viorng_iointf = { // intializes the IO subsystem and contains the fucntion pointers for device operations
        .close = &viorng_close,
        .read = &viorng_read,
    };

    struct viorng_device * viorng;

    viorng = kcalloc(1, sizeof(struct viorng_device)); // Allocates memory for the new VirtIO rng device struct
    viorng->regs = regs;
    viorng->irqno = irqno; 
    ioinit0(&viorng->io, &viorng_iointf); // Intializes the device structure
    viorng->io.refcnt = 0; // Registers the VirtIO rng device into the system
    viorng->instno = register_device(VIORNG_NAME, viorng_open, viorng);
    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;
    
    assert (regs->device_id == VIRTIO_ID_RNG);

    // Signal device that we found a driver
    regs->status |= VIRTIO_STAT_DRIVER;

    // fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    //           FIXME Finish viorng initialization here! 
    
    regs->status |= VIRTIO_STAT_FEATURES_OK; // sets the correct bits and checks them
    if((regs->status & VIRTIO_STAT_FEATURES_OK) == 0){
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    viorng->vq.desc[0].addr = (uint64_t)&viorng->buf; // fills in the descriptor tables
    viorng->vq.desc[0].flags = VIRTQ_DESC_F_WRITE;
    viorng->vq.desc[0].len = VIORNG_BUFSZ;

    virtio_attach_virtq(regs, 0, 1, (uint64_t)&viorng->vq.desc[0], (uint64_t)&viorng->vq.used, (uint64_t)&viorng->vq.avail); // attaches the avail and used structs togethe

    // fence o,oi
    regs->status |= VIRTIO_STAT_DRIVER_OK;    
    //           fence o,oi
    __sync_synchronize();
}

// int viorng_open(struct io ** ioptr, void * aux)
// Inputs: struct io ** ioptr - pointer to store the reference to the VirtIO rng structure for an instance, void * aux - pointer to the VirtIO rng device structure
// Outputs: Returns 0 if the VirtIO rng device opens successfully
// Description: This function makes the avail and used queues available to be used. It also enables the interrupt soruce for the device and all of the IO operations are done through the ioptr.
// Side Effects: Changes the buffers and the refcnt.

int viorng_open(struct io ** ioptr, void * aux) {
    //           FIXME your code here
    struct viorng_device * viorng = aux;
    //trace("%s(viorng=%x)", __func__, viorng);
    condition_init(&viorng->viorng_buffer_condition, "buffer_cond"); // Intializes the condition for the VirtIO rng device
    virtio_enable_virtq(viorng->regs, 0); // enables the avail and used queues
    enable_intr_source(viorng->irqno, VIORNG_IRQ_PRIO, viorng_isr, aux); // enables interrupt source for VirtIO rng device and sets up IO operations
    *ioptr = ioaddref(&viorng->io); 
    return 0;
}

// void viorng_close(struct io * io)
// Inputs: struct io * io - pointer to the IO structure associated with the VirtIO rng device
// Outputs: None
// Description: This function resets the avail and used queues and prevents further interrupts.
// Side Effects: Changes the device table and also affects PLIC registers

void viorng_close(struct io * io) {
    //           FIXME your code here
    // Get the VirtIO rng device structure from the I/O interface pointer
    struct viorng_device * viorng = (void*)io - offsetof(struct viorng_device, io);
    virtio_reset_virtq(viorng->regs, 0); //resets the avail and used queues and prevents further interrupts
    disable_intr_source(viorng->irqno);
}

// long viorng_read(struct io * io, void * buf, long bufsz)
// Inputs: struct io * io - pointer to the IO structure associated with the VirtIO rng device, void * buf -  pointer to the buffer to where the bytes read will be, long bufsz - size of the buffer in bytes
// Outputs: Returns the minimum number of bytes read, (bufsz or viorng->vq.used.ring[viorng->vq.used.idx % 1].len), or -EINVAL if the read fails
// Description:  This function reads up to minimum number of bytes that need to be read from the VirtIO Entropy device and writes them to buf. It sets certain registers to request entropy from the device and waits until the randomess has been placed into a buffer and writes it to buf.
// Side Effects: Changes the buffer, calls condition wait, and calls virtio_notify_avail

long viorng_read(struct io * io, void * buf, long bufsz) {
    //           FIXME your code here
    struct viorng_device * viorng = (void*)io - offsetof(struct viorng_device, io);
    viorng->vq.avail.ring[viorng->vq.avail.idx % 1] = 0; // setting the avail ring buffer items in the head of the descriptor table
    __sync_synchronize(); // memory barrier
    viorng->vq.avail.idx++; // increasing the index of the availiable ring buffer
    __sync_synchronize();
    virtio_notify_avail(viorng->regs, 0); // Notifies the device of new elements in the avail ring.
    if (bufsz > 0){
        char * word = (char *) buf;
        int pie = disable_interrupts(); //disables interrupts here for critical section for the condition wait
        while(viorng->vq.avail.idx != viorng->vq.used.idx){ //ensures that the device does not overwrite or reprocess buffers
            condition_wait(&viorng->viorng_buffer_condition);
        }
        restore_interrupts(pie); //restores interrupts here for end of critical sections
        long min_bytes_read = (bufsz > viorng->vq.used.ring[viorng->vq.used.idx % 1].len) ? viorng->vq.used.ring[viorng->vq.used.idx % 1].len : bufsz;
        for(int i = 0; i < min_bytes_read; i++){
            char c; // writing the data from the device buffer into the buffer in the parameter
            c = viorng->buf[i];
            word[i] = c;
        }
        return min_bytes_read; 
    }
    else if(bufsz == 0){
        return 0;
    }
    else{
        return -EINVAL;
    }
}

// void viorng_isr(int irqno, void * aux)
// Inputs: int irqno - external interrupt request source, void * aux - pointer to the VirtIO rng device structure
// Outputs: None
// Description: This function sets certain device registers. It will wake the thread up after waiting for the device to finish servicing the request. It will also condition a boradcast based on a certain condition from the device.
// Side Effects: Changes the registers and calls condition broadcast.

void viorng_isr(int irqno, void * aux) {
    //           FIXME your code here
    struct viorng_device * viorng = aux;
    const uint32_t USED_BUFFER_NOTIF = (1 << 0);
    if((viorng->regs->interrupt_status & USED_BUFFER_NOTIF)){ // if the interrupt status is the same as the user buffer notification bit, then it will set the interrupt acknowledge bit and condtion broadcast
        viorng->regs->interrupt_ack |= USED_BUFFER_NOTIF;
        condition_broadcast(&viorng->viorng_buffer_condition);
        __sync_synchronize();
    }
}