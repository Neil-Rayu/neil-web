// io.c - Unified I/O object
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "io.h"
#include "ioimpl.h"
#include "assert.h"
#include "string.h"
#include "heap.h"
#include "error.h"
#include "thread.h"
#include "memory.h"
#include "intr.h"

#include <stddef.h>
#include <limits.h>

// #define ENULLIO 239;
// INTERNAL TYPE DEFINITIONS
//

struct memio
{
    struct io io; // I/O struct of memory I/O
    void *buf;    // Block of memory
    size_t size;  // Size of memory block
};

// #define PIPE_BUFSZ PAGE_SIZE

struct seekio
{
    struct io io;           // I/O struct of seek I/O
    struct io *bkgio;       // Backing I/O supporting _readat_ and _writeat_
    unsigned long long pos; // Current position within backing endpoint
    unsigned long long end; // End position in backing endpoint
    int blksz;              // Block size of backing endpoint
};

// struct nullio
// {
//     struct io io;
// };

struct pipe
{
    struct io writeio;
    struct io readio;

    char *buf;

    unsigned long long headpos;
    unsigned long long tailpos;

    unsigned long long data;

    // struct lock rlock;
    // struct lock wlock;
    struct lock lock;

    struct condition notempty; // check this to add data
    struct condition notfull;  // check this when we want to remove data
};

// INTERNAL FUNCTION DEFINITIONS
//

static int memio_cntl(struct io *io, int cmd, void *arg);

static long memio_readat(
    struct io *io, unsigned long long pos, void *buf, long bufsz);

static long memio_writeat(
    struct io *io, unsigned long long pos, const void *buf, long len);

static void seekio_close(struct io *io);

static int seekio_cntl(struct io *io, int cmd, void *arg);

static long seekio_read(struct io *io, void *buf, long bufsz);

static long seekio_write(struct io *io, const void *buf, long len);

static long seekio_readat(
    struct io *io, unsigned long long pos, void *buf, long bufsz);

static long seekio_writeat(
    struct io *io, unsigned long long pos, const void *buf, long len);

static void pipe_close(struct io *io);
static long pipe_read(struct io *io, void *buf, long bufsz);
static long pipe_write(struct io *io, const void *buf, long len);
static int pipe_cntl(struct io *io, int cmd, void *arg);
int rbuf_full(const struct pipe *rbuf);
void rbuf_putc(struct pipe *rbuf, char c);
char rbuf_getc(struct pipe *rbuf);
int rbuf_empty(const struct pipe *rbuf);
// static long null_read(struct io *io, void *buf, long bufsz);
// static long null_write(struct io *io, const void *buf, long len);
// static int null_cntl(struct io *io, int cmd, void *arg);
// static void null_close(struct io *io);

// INTERNAL GLOBAL CONSTANTS
static const struct iointf seekio_iointf = {
    .close = &seekio_close,
    .cntl = &seekio_cntl,
    .read = &seekio_read,
    .write = &seekio_write,
    .readat = &seekio_readat,
    .writeat = &seekio_writeat};

static const struct iointf memio_iointf = {
    .readat = &memio_readat,
    .writeat = &memio_writeat,
    .cntl = &memio_cntl};

static const struct iointf pipe_read_intf = {
    .close = &pipe_close,
    .read = &pipe_read,
    .cntl = &pipe_cntl};

static const struct iointf pipe_write_intf = {
    .close = &pipe_close,
    .write = &pipe_write,
    .cntl = &pipe_cntl};

// static const struct iointf null_iointf = {
//     .read = &null_read,
//     .write = &null_write,
//     .cntl = &null_cntl,
//     .close = &null_close};

// EXPORTED FUNCTION DEFINITIONS
//

// Ask questions about this in OH

void create_pipe(struct io **wioptr, struct io **rioptr)
{
    struct pipe *p = kcalloc(1, sizeof(struct pipe));
    p->buf = (char *)alloc_phys_page();
    p->headpos = 0;
    p->tailpos = 0;
    p->data = 0;
    condition_init(&p->notempty, "notempty");
    condition_init(&p->notfull, "notfull");
    ioinit1(&p->readio, &pipe_read_intf);
    ioinit1(&p->writeio, &pipe_write_intf);
    lock_init(&p->lock);

    *rioptr = &p->readio;
    *wioptr = &p->writeio;
}

static void pipe_close(struct io *io)
{
    if (io == NULL)
    {
        return;
    }
    struct pipe *p;

    if (io->intf == &pipe_read_intf)
    {
        p = (struct pipe *)((char *)io - offsetof(struct pipe, readio));
    }
    else if (io->intf == &pipe_write_intf)
    {
        p = (struct pipe *)((char *)io - offsetof(struct pipe, writeio));
    }

    if (p->readio.refcnt == 0 && p->writeio.refcnt == 0)
    {
        free_phys_page(p->buf);
        // kfree(p);
    }
}

static long pipe_read(struct io *io, void *buf, long bufsz)
{
    if (io == NULL)
    {
        return -EINVAL;
    }
    if (buf == NULL)
    {
        return -EINVAL;
    }
    if (bufsz == 0)
    {
        return 0;
    }
    if (bufsz < 0)
    {
        return -EINVAL;
    }
    struct pipe *p;
    // long bytes_read = 0;

    if (io->intf == &pipe_read_intf)
    {
        p = (struct pipe *)((char *)io - offsetof(struct pipe, readio));
    }

    else
    {
        return -EINVAL;
    }

    // lock_acquire(&p->lock);

    // //What is EOF how should we repersent it?
    // if ((p->tailpos - p->headpos == PAGE_SIZE) && (p->writeio.refcnt == 0)) {
    //     return 0; //Reprsent EOF;
    // }

    // Are we supposed to do short read?
    int pie = disable_interrupts();

    while (rbuf_empty(p) && p->writeio.refcnt > 0)
    {
        //  put thread to sleep via condition wait
        condition_wait(&p->notempty);
    }
    if (rbuf_empty(p) && p->writeio.refcnt == 0)
    {
        restore_interrupts(pie);
        return 0; // EOF
    }

    restore_interrupts(pie);

    for (long i = 0; i < bufsz; i++)
    {
        if (!rbuf_empty(p))
        {
            char c = rbuf_getc(p);
            lock_acquire(&p->lock);
            ((char *)(buf))[i] = c;
            p->data--;
            lock_release(&p->lock);
            // bytes_read++;
        }
        else
        {
            condition_broadcast(&p->notfull);
            return i;
        }
    }

    condition_broadcast(&p->notfull);
    return bufsz;
}

static long pipe_write(struct io *io, const void *buf, long len)
{
    if (io == NULL)
    {
        return -EINVAL;
    }
    if (buf == NULL)
    {
        return -EINVAL;
    }
    if (len == 0)
    {
        return 0;
    }
    if (len < 0)
    {
        return -EINVAL;
    }

    struct pipe *p;
    long bytes_written;

    // if (io->intf == &pipe_read_intf) {
    //     p = (struct pipe *)((char *)io - offsetof(struct pipe, readio));
    // }
    if (io->intf == &pipe_write_intf)
    {
        p = (struct pipe *)((char *)io - offsetof(struct pipe, writeio));
    }
    if (p->readio.refcnt == 0)
    {
        return -EPIPE;
    }
    for (bytes_written = 0; bytes_written < len; bytes_written++)
    {
        int pie = disable_interrupts();
        while (rbuf_full(p))
        {
            condition_wait(&p->notfull);
        }
        restore_interrupts(pie);

        if (p->readio.refcnt == 0)
        {
            if (bytes_written > 0)
            {
                condition_broadcast(&p->notempty);
                return bytes_written;
            }
            return -EPIPE; // epipe or bytes written?
        }
        lock_acquire(&p->lock);
        rbuf_putc(p, ((char *)buf)[bytes_written]);
        p->data++;
        lock_release(&p->lock);
        // if (p->data == 1)
        // {
        //     condition_broadcast(&p->notempty);
        // }

        if ((bytes_written % PAGE_SIZE) == 0)
        {
            condition_broadcast(&p->notempty);
            // condition_wait(&p->notfull);
        }
    }
    condition_broadcast(&p->notempty); // data ready to read
    return bytes_written;
}

int rbuf_empty(const struct pipe *pipe)
{
    return (pipe->headpos == pipe->tailpos);
}

int rbuf_full(const struct pipe *pipe)
{
    return ((uint16_t)(pipe->tailpos - pipe->headpos) == PAGE_SIZE);
}

void rbuf_putc(struct pipe *pipe, char c)
{
    uint_fast16_t tpos;

    tpos = pipe->tailpos;
    pipe->buf[tpos % PAGE_SIZE] = c;
    asm volatile("" ::: "memory");
    pipe->tailpos = tpos + 1;
}

char rbuf_getc(struct pipe *pipe)
{
    uint_fast16_t hpos;
    char c;

    hpos = pipe->headpos;
    c = pipe->buf[hpos % PAGE_SIZE];
    asm volatile("" ::: "memory");
    pipe->headpos = hpos + 1;
    return c;
}

static int pipe_cntl(struct io *io, int cmd, void *arg)
{
    if (io == NULL)
    {
        return -EINVAL;
    }
    struct pipe *p;

    if (io->intf == &pipe_read_intf)
    {
        p = (struct pipe *)((char *)io - offsetof(struct pipe, readio));
    }
    else if (io->intf == &pipe_write_intf)
    {
        p = (struct pipe *)((char *)io - offsetof(struct pipe, writeio));
    }

    unsigned long long *ullarg;

    switch (cmd)
    {
    case IOCTL_GETBLKSZ:
        return 1;
    case IOCTL_GETEND:
        if (arg == NULL)
        {
            return -EINVAL;
        }

        ullarg = (unsigned long long *)arg;

        lock_acquire(&p->lock);

        if (io->intf == &pipe_read_intf)
        {
            *ullarg = p->data;
        }
        else
        {
            *ullarg = PAGE_SIZE - p->data;
        }
        lock_release(&p->lock);
        return 0;

    default:
        return -ENOTSUP;
    }
}

struct io *ioinit0(struct io *io, const struct iointf *intf)
{
    assert(io != NULL);
    assert(intf != NULL);
    io->intf = intf;
    io->refcnt = 0;
    return io;
}

struct io *ioinit1(struct io *io, const struct iointf *intf)
{
    assert(io != NULL);
    io->intf = intf;
    io->refcnt = 1;
    return io;
}

unsigned long iorefcnt(const struct io *io)
{
    assert(io != NULL);
    return io->refcnt;
}

struct io *ioaddref(struct io *io)
{
    assert(io != NULL);
    io->refcnt += 1;
    return io;
}

void ioclose(struct io *io)
{
    assert(io != NULL);
    assert(io->intf != NULL);

    assert(io->refcnt != 0);
    io->refcnt -= 1;

    if (io->refcnt == 0 && io->intf->close != NULL)
        io->intf->close(io);
}

long ioread(struct io *io, void *buf, long bufsz)
{
    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;

    if (bufsz < 0)
        return -EINVAL;

    return io->intf->read(io, buf, bufsz);
}

long iofill(struct io *io, void *buf, long bufsz)
{
    long bufpos = 0; // position in buffer for next read
    long nread;      // result of last read

    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;

    if (bufsz < 0)
        return -EINVAL;

    while (bufpos < bufsz)
    {
        nread = io->intf->read(io, buf + bufpos, bufsz - bufpos);

        if (nread <= 0)
            return (nread < 0) ? nread : bufpos;

        bufpos += nread;
    }

    return bufpos;
}

long iowrite(struct io *io, const void *buf, long len)
{
    // kprintf("Wrote %c bytes from buf: %p\n", *((char *)(buf)), buf);
    long bufpos = 0; // position in buffer for next write
    long n;          // result of last write

    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->write == NULL)
        return -ENOTSUP;

    if (len < 0)
        return -EINVAL;

    do
    {
        n = io->intf->write(io, buf + bufpos, len - bufpos);

        if (n <= 0)
            return (n < 0) ? n : bufpos;

        bufpos += n;
    } while (bufpos < len);

    return bufpos;
}

long ioreadat(
    struct io *io, unsigned long long pos, void *buf, long bufsz)
{
    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->readat == NULL)
        return -ENOTSUP;

    if (bufsz < 0)
        return -EINVAL;

    return io->intf->readat(io, pos, buf, bufsz);
}

long iowriteat(
    struct io *io, unsigned long long pos, const void *buf, long len)
{
    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->writeat == NULL)
        return -ENOTSUP;

    if (len < 0)
        return -EINVAL;

    return io->intf->writeat(io, pos, buf, len);
}

int ioctl(struct io *io, int cmd, void *arg)
{
    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->cntl != NULL)
        return io->intf->cntl(io, cmd, arg);
    else if (cmd == IOCTL_GETBLKSZ)
        return 1; // default block size
    else
        return -ENOTSUP;
}

int ioblksz(struct io *io)
{
    return ioctl(io, IOCTL_GETBLKSZ, NULL);
}

int ioseek(struct io *io, unsigned long long pos)
{
    return ioctl(io, IOCTL_SETPOS, &pos);
}

struct io *create_memory_io(void *buf, size_t size)
{
    // FIX ME
    struct memio *mio;

    mio = kcalloc(1, sizeof(struct memio));
    mio->buf = buf;
    mio->size = size;

    return ioinit1(&mio->io, &memio_iointf);
}

struct io *create_seekable_io(struct io *io)
{
    struct seekio *sio;
    unsigned long end;
    int result;
    int blksz;

    blksz = ioblksz(io);
    // kprintf("\n%d", blksz);
    assert(0 < blksz);

    // block size must be power of two
    assert((blksz & (blksz - 1)) == 0);

    result = ioctl(io, IOCTL_GETEND, &end);
    assert(result == 0);

    sio = kcalloc(1, sizeof(struct seekio));

    sio->pos = 0;
    sio->end = end;
    sio->blksz = blksz;
    sio->bkgio = ioaddref(io);

    return ioinit1(&sio->io, &seekio_iointf);
};

// INTERNAL FUNCTION DEFINITIONS
//

long memio_readat(
    struct io *io,
    unsigned long long pos,
    void *buf, long bufsz)
{
    struct memio *const mio = (void *)io - offsetof(struct memio, io);

    if (pos > mio->size)
    {
        return -EINVAL;
    }
    int read_size = pos + bufsz > mio->size ? mio->size - pos : bufsz;
    memcpy(buf, (((char *)mio->buf) + pos), read_size);
    return read_size;
}

long memio_writeat(
    struct io *io,
    unsigned long long pos,
    const void *buf, long len)
{
    struct memio *const mio = (void *)io - offsetof(struct memio, io);
    if (pos > mio->size)
    {
        return -EINVAL;
    }
    int write_size = pos + len > mio->size ? mio->size - pos : len;
    memcpy(((mio->buf) + pos), buf, write_size);
    return write_size;
}

int memio_cntl(struct io *io, int cmd, void *arg)
{
    struct memio *const mio = (void *)io - offsetof(struct memio, io);

    if (arg == NULL)
    {
        return -EINVAL;
    }

    unsigned long long *ullarg = arg;

    switch (cmd)
    {
    case IOCTL_GETBLKSZ:
        return 1;
    case IOCTL_GETEND:
        *ullarg = mio->size;
        return 0;
    case IOCTL_SETEND:
        // Call backing endpoint ioctl and save result
        // result = ioctl(&mio->io, IOCTL_SETEND, ullarg);

        if (*ullarg < mio->size)
        {
            size_t end = 0;
            end = *ullarg;
            mio->size = end;
            return 0;
        }
        else
        {
            return -EINVAL;
        }
    default:
        return -ENOTSUP;
    }
}

void seekio_close(struct io *io)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    sio->bkgio->refcnt--;
    ioclose(sio->bkgio);
    kfree(sio);
}

int seekio_cntl(struct io *io, int cmd, void *arg)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    unsigned long long *ullarg = arg;
    int result;

    switch (cmd)
    {
    case IOCTL_GETBLKSZ:
        return sio->blksz;
    case IOCTL_GETPOS:
        *ullarg = sio->pos;
        return 0;
    case IOCTL_SETPOS:
        // New position must be multiple of block size
        if ((*ullarg & (sio->blksz - 1)) != 0)
            return -EINVAL;

        // New position must not be past end
        if (*ullarg > sio->end)
            return -EINVAL;

        sio->pos = *ullarg;
        return 0;
    case IOCTL_GETEND:
        *ullarg = sio->end;
        return 0;
    case IOCTL_SETEND:
        // Call backing endpoint ioctl and save result
        result = ioctl(sio->bkgio, IOCTL_SETEND, ullarg);
        if (result == 0)
            sio->end = *ullarg;
        return result;
    default:
        return ioctl(sio->bkgio, cmd, arg);
    }
}

long seekio_read(struct io *io, void *buf, long bufsz)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long const end = sio->end;
    long rcnt;

    // Cannot read past end
    if (end - pos < bufsz)
        bufsz = end - pos;

    if (bufsz == 0)
        return 0;

    // Request must be for at least blksz bytes if not zero
    if (bufsz < sio->blksz)
        return -EINVAL;

    // Truncate buffer size to multiple of blksz
    bufsz &= ~(sio->blksz - 1);

    rcnt = ioreadat(sio->bkgio, pos, buf, bufsz);
    sio->pos = pos + ((rcnt < 0) ? 0 : rcnt);
    return rcnt;
}

long seekio_write(struct io *io, const void *buf, long len)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long end = sio->end;
    int result;
    long wcnt;

    if (len == 0)
        return 0;

    // Request must be for at least blksz bytes
    if (len < sio->blksz)
        return -EINVAL;

    // Truncate length to multiple of blksz
    len &= ~(sio->blksz - 1);

    // Check if write is past end. If it is, we need to change end position.

    if (end - pos < len)
    {
        if (ULLONG_MAX - pos < len)
            return -EINVAL;

        end = pos + len;
        // kprintf("\nEnd: %d", end);
        result = ioctl(sio->bkgio, IOCTL_SETEND, &end);

        if (result != 0)
            return result;

        sio->end = end;
    }

    wcnt = iowriteat(sio->bkgio, sio->pos, buf, len);
    sio->pos = pos + ((wcnt < 0) ? 0 : wcnt);
    return wcnt;
}

long seekio_readat(
    struct io *io, unsigned long long pos, void *buf, long bufsz)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    return ioreadat(sio->bkgio, pos, buf, bufsz);
}

long seekio_writeat(
    struct io *io, unsigned long long pos, const void *buf, long len)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    return iowriteat(sio->bkgio, pos, buf, len);
}

// struct io *create_null_io(void)
// {
//     // return NULL;
//     struct nullio *null_io = kcalloc(1, sizeof(struct nullio));
//     if (null_io == NULL)
//     {
//         // kprintf("got fucked");
//         return NULL;
//     }
//     return ioinit1(null_io, &null_iointf);
// }

// static long null_read(struct io *io, void *buf, long bufsz)
// {
//     return -ENULLIO;
// }
// static long null_write(struct io *io, const void *buf, long len)
// {
//     return len;
// }
// static int null_cntl(struct io *io, int cmd, void *arg)
// {
//     unsigned long long *ullarg;

//     switch (cmd)
//     {
//     case IOCTL_GETBLKSZ:
//         return 1;
//     case IOCTL_GETEND:
//         if (arg == NULL)
//         {
//             return -EINVAL;
//         }
//         ullarg = (unsigned long long *)arg;
//         *ullarg = 0;
//         return 0;
//     default:
//         return -ENOTSUP;
//     }
// }

// static void null_close(struct io *io)
// {
//     kfree(io);
// }