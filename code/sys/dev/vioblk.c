// vioblk.c - VirtIO serial port (console)
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include "virtio.h"
#include "intr.h"
#include "assert.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "thread.h"
#include "string.h"
#include "assert.h"
#include "ioimpl.h"
#include "io.h"
#include "conf.h"
#include "error.h"
#include <limits.h>

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// INTERNAL CONSTANT DEFINITIONS
//

// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_TOPOLOGY 10
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14
#define VIOBLK_BLKSZ 512UL
#define NUM_DESCRIPTORS 4
#define VQ_SIZE 1

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_S_UNSUPP 2

// INTERNAL FUNCTION DECLARATIONS
//

static int vioblk_open(struct io **ioptr, void *aux);
static void vioblk_close(struct io *io);

static long vioblk_readat(
    struct io *io,
    unsigned long long pos,
    void *buf,
    long bufsz);

static long vioblk_writeat(
    struct io *io,
    unsigned long long pos,
    const void *buf,
    long len);

static int vioblk_cntl(
    struct io *io, int cmd, void *arg);

static void vioblk_isr(int srcno, void *aux);

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.
// vioblk device struct for MMIO comunication
struct header
{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};
struct vioblk_device
{
    volatile struct virtio_mmio_regs *regs;
    int irqno;
    int instno;
    struct io io;

    struct condition vioblk_buffer_condition;
    struct lock vioblk_lock;
    struct
    {
        uint16_t last_used_idx;

        union
        {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(1)];
        };

        union
        {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(1)];
        };

        //  The first descriptor is a regular descriptor and is the one used in
        //  the avail and used rings.

        struct virtq_desc desc[NUM_DESCRIPTORS]; // Do we need to change the ammount of descriptors here?
                                                 // is it supposed to be num queues defined in thd config?
    } vq;

    struct header header;
    uint8_t data[VIOBLK_BLKSZ];
    uint8_t status;
};
// void vioblk_attach(volatile struct virtio_mmio_regs *regs, int irqno)
// Inputs: volatile struct virtio_mmio_regs *regs - volatile pointer to the mmio vioblk registers
//         int irqno - interupt request number
// Outputs: None
// Description: Regesters the vioblk device, attaches the io interface, and sets the descriptor for vioblk.
// Side Effects: Allocates vioblk_device struct instance vioblk.
void vioblk_attach(volatile struct virtio_mmio_regs *regs, int irqno)
{

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    // Step 3,  Signal device that we found a driver
    regs->status |= VIRTIO_STAT_DRIVER;

    // Steps 4-6, Feature Negotiation
    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;
    uint32_t blksz;
    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs,
                                       enabled_features, wanted_features, needed_features);

    if (result != 0)
    {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert(((blksz - 1) & blksz) == 0);

    // FIX ME

    // Step 7, Device specific Set-up
    static const struct iointf vioblk_iointf = {
        .close = &vioblk_close,
        .readat = &vioblk_readat,
        .writeat = &vioblk_writeat,
        .cntl = &vioblk_cntl};

    // Allocate vioblk device and initalize regesters, irqno, and io.
    struct vioblk_device *vioblk = kcalloc(1, sizeof(struct vioblk_device));
    vioblk->regs = regs;
    vioblk->irqno = irqno;
    ioinit0(&vioblk->io, &vioblk_iointf);

    // Regester and init descriptors
    // TODO FIX!
    // Set up the indirect discriptors
    vioblk->instno = register_device(VIOBLK_NAME, vioblk_open, vioblk);

    // Descriptor 0 - inderect
    vioblk->vq.desc[0].addr = (uint64_t)&vioblk->vq.desc[1];
    vioblk->vq.desc[0].len = 3 * sizeof(struct virtq_desc);
    vioblk->vq.desc[0].flags = VIRTQ_DESC_F_INDIRECT;
    vioblk->vq.desc[0].next = -1;

    // descriptor 1 - header

    vioblk->vq.desc[1].addr = (uint64_t)&vioblk->header;
    vioblk->vq.desc[1].len = sizeof(struct header);
    vioblk->vq.desc[1].flags = VIRTQ_DESC_F_NEXT;
    vioblk->vq.desc[1].next = 1;

    // descriptor 2 - data

    vioblk->vq.desc[2].addr = (uint64_t)&vioblk->data;
    vioblk->vq.desc[2].len = sizeof(vioblk->data);
    vioblk->vq.desc[2].flags = VIRTQ_DESC_F_NEXT;
    vioblk->vq.desc[2].next = 2;

    // descriptor 3 - status

    vioblk->vq.desc[3].addr = (uint64_t)&vioblk->status;
    vioblk->vq.desc[3].len = sizeof(vioblk->status);
    vioblk->vq.desc[3].flags = VIRTQ_DESC_F_WRITE;
    vioblk->vq.desc[3].next = -1;

    // Desc 4 - end
    // vioblk->vq.desc[0].addr = 0;
    // vioblk->vq.desc[0].len = 0;
    // vioblk->vq.desc[0].flags = VIRTQ_DESC_F_INDIRECT;
    // vioblk->vq.desc[0].next = -1;

    // Attach
    virtio_attach_virtq(regs, 0, 1, (uint64_t)&vioblk->vq.desc[0], (uint64_t)&vioblk->vq.used, (uint64_t)&vioblk->vq.avail); // attaches the avail and used structs togethe

    // Step 8, Device is now live.
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    //  fence o,oi
    __sync_synchronize();
}

// int vioblk_open(struct io **ioptr, void *aux)
// Inputs: struct io **ioptr - contains pointer to the referance of the vioblk io struct (for a given vioblk instance)
//         void *aux - contains a pointer to the vioblk Device struct
// Outputs: 0 if succesful
// Description: Associates io reference with the vioblk device, initialize virt queues, and enable vioblk intr source
// Side Effects: Modifies ioref count
static int vioblk_open(struct io **ioptr, void *aux)
{
    trace("%s()", __func__);
    if (ioptr == NULL || aux == NULL)
    {
        return -EINVAL;
    }
    struct vioblk_device *vioblk = aux;
    // Enable virtqueue, interupts, and set io refereance
    condition_init(&vioblk->vioblk_buffer_condition, "voiblk buffer_cond");
    virtio_enable_virtq(vioblk->regs, 0);
    enable_intr_source(vioblk->irqno, VIOBLK_INTR_PRIO, vioblk_isr, aux);
    *ioptr = ioaddref(&vioblk->io);
    lock_init(&vioblk->vioblk_lock);
    return 0;
}

static void vioblk_close(struct io *io)
{
    trace("%s()", __func__);
    struct vioblk_device *vioblk = (void *)io - offsetof(struct vioblk_device, io);
    virtio_reset_virtq(vioblk->regs, 0); // resets the avail and used queues and prevents further interrupts
    disable_intr_source(vioblk->irqno);
}

static int vioblk_cntl(struct io *io, int cmd, void *arg)
{
    trace("%s()", __func__);
    struct vioblk_device *vioblk = (void *)io - offsetof(struct vioblk_device, io);
    if (arg == NULL)
    {
        return -EINVAL;
    }
    unsigned long long *ullarg = arg;

    switch (cmd)
    {
    case IOCTL_GETBLKSZ: //
        size_t msize = vioblk->regs->config.blk.blk_size;
        return msize;
    case IOCTL_GETEND:
        *ullarg = vioblk->regs->config.blk.capacity * vioblk->regs->config.blk.blk_size;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static long vioblk_readat(struct io *io, unsigned long long pos, void *buf, long bufsz)
{
    trace("%s()", __func__);
    // kprintf("\nVIOREAD CALLED");
    struct vioblk_device *vioblk = (void *)io - offsetof(struct vioblk_device, io);
    unsigned long long byteread = 0;
    uint32_t blkSize = vioblk->regs->config.blk.blk_size;
    unsigned long long end = vioblk->regs->config.blk.capacity * vioblk->regs->config.blk.blk_size;

    if (pos == end)
    {
        return 0;
    }

    if (buf == NULL)
    {
        return -EINVAL;
    }

    if (pos > end)
    {
        return -EINVAL;
    }

    if (bufsz < 0)
    { // check to make sure busz is not negative
        return -EINVAL;
    }

    if (bufsz == 0)
    { // if bufsz is 0, return 0
        return 0;
    }
    vioblk->vq.desc[2].flags |= VIRTQ_DESC_F_WRITE; // FLAG FOR READ?
    vioblk->header.type = VIRTIO_BLK_T_IN;
    vioblk->header.sector = pos / blkSize;

    byteread = pos + bufsz > end ? end - pos : bufsz;

    int numBlocks = byteread / blkSize;
    lock_acquire(&vioblk->vioblk_lock);
    for (int i = 0; i < numBlocks; i++)
    {
        vioblk->header.sector = (pos / blkSize) + i;
        vioblk->vq.avail.ring[vioblk->vq.avail.idx % VQ_SIZE] = 0;
        __sync_synchronize();
        vioblk->vq.avail.idx++;
        __sync_synchronize();
        virtio_notify_avail(vioblk->regs, 0);

        int pie = disable_interrupts();
        while (vioblk->vq.avail.idx != vioblk->vq.used.idx)
        { // ensures that the device does not overwrite or reprocess buffers
            condition_wait(&vioblk->vioblk_buffer_condition);
        }
        restore_interrupts(pie);

        if (vioblk->status & VIRTIO_BLK_S_IOERR)
        {
            lock_release(&vioblk->vioblk_lock);
            return -EIO;
        }
        if (vioblk->status & VIRTIO_BLK_S_UNSUPP)
        {
            lock_release(&vioblk->vioblk_lock);
            return -ENOTSUP;
        }
        memcpy((((char *)buf) + (i * blkSize)), vioblk->data, blkSize);
    }

    lock_release(&vioblk->vioblk_lock);
    return byteread;
}

static long vioblk_writeat(struct io *io, unsigned long long pos, const void *buf, long len)
{
    trace("%s()", __func__);
    if (buf == NULL)
    {
        return -EINVAL;
    }

    struct vioblk_device *vioblk = (void *)io - offsetof(struct vioblk_device, io);
    long bytewritten = 0;
    uint32_t blkSize = vioblk->regs->config.blk.blk_size;
    unsigned long long end = vioblk->regs->config.blk.capacity * vioblk->regs->config.blk.blk_size;
    if (pos == end)
    {
        return 0;
    }

    if (pos > end)
    {
        return -EINVAL;
    }

    if (len < 0)
    { // check to make sure len is not negative
        return -EINVAL;
    }

    if (len == 0)
    { // if len is 0, return 0
        return 0;
    }

    vioblk->vq.desc[2].flags &= ~(VIRTQ_DESC_F_WRITE); // FLAG FOR READ?
    vioblk->header.type = VIRTIO_BLK_T_OUT;
    vioblk->header.sector = pos / blkSize;

    bytewritten = pos + len > end ? end - pos : len;

    int numBlocks = bytewritten / blkSize;
    lock_acquire(&vioblk->vioblk_lock);
    for (int i = 0; i < numBlocks; i++)
    {
        vioblk->header.sector = (pos / blkSize) + i;
        memcpy(vioblk->data, (buf + i * blkSize), blkSize);
        vioblk->vq.avail.ring[vioblk->vq.avail.idx % VQ_SIZE] = 0;
        __sync_synchronize();
        vioblk->vq.avail.idx++;
        __sync_synchronize();
        virtio_notify_avail(vioblk->regs, 0);

        int pie = disable_interrupts();
        while (vioblk->vq.avail.idx != vioblk->vq.used.idx)
        { // ensures that the device does not overwrite or reprocess buffers
            condition_wait(&vioblk->vioblk_buffer_condition);
        }
        restore_interrupts(pie);

        if (vioblk->status & VIRTIO_BLK_S_IOERR)
        {
            lock_release(&vioblk->vioblk_lock);
            return -EIO;
        }
        if (vioblk->status & VIRTIO_BLK_S_UNSUPP)
        {
            lock_release(&vioblk->vioblk_lock);
            return -ENOTSUP;
        }
    }
    lock_release(&vioblk->vioblk_lock);
    return bytewritten;
}

static void vioblk_isr(int srcno, void *aux)
{
    trace("%s()", __func__);
    struct vioblk_device *const vioblk = aux; // sets blk device to aux
    // const uint32_t USED_BUFFER_NOTIF = (1 << 0);
    // vioblk->irqno = srcno;
    // if the interrupt status is the same as the user buffer notification bit, then it will set the interrupt acknowledge bit and condtion broadcast
    vioblk->regs->interrupt_ack |= vioblk->regs->interrupt_status;
    condition_broadcast(&vioblk->vioblk_buffer_condition);
    __sync_synchronize();
}