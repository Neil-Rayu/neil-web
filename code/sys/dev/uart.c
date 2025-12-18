// uart.c - NS8250-compatible uart port
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "uart.h"
#include "device.h"
#include "intr.h"
#include "heap.h"

#include "ioimpl.h"
#include "console.h"

#include "error.h"

#include <stdint.h>
#include "thread.h"

// COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_NAME
#define UART_NAME "uart"
#endif

// INTERNAL TYPE DEFINITIONS
//

struct uart_regs
{
    union
    {
        char rbr;    // DLAB=0 read
        char thr;    // DLAB=0 write
        uint8_t dll; // DLAB=1
    };

    union
    {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };

    union
    {
        uint8_t iir; // read
        uint8_t fcr; // write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

struct ringbuf
{
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

struct uart_device
{
    volatile struct uart_regs *regs;
    int irqno;
    int instno;

    struct io io;

    unsigned long rxovrcnt; // number of times OE was set

    struct ringbuf rxbuf;
    struct ringbuf txbuf;
    struct condition uart_read_cond;
    struct condition uart_write_cond;
    struct lock uart_lock;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_open(struct io **ioptr, void *aux);
static void uart_close(struct io *io);
static long uart_read(struct io *io, void *buf, long bufsz);
static long uart_write(struct io *io, const void *buf, long len);

static void uart_isr(int srcno, void *driver_private);

static void rbuf_init(struct ringbuf *rbuf);
static int rbuf_empty(const struct ringbuf *rbuf);
static int rbuf_full(const struct ringbuf *rbuf);
static void rbuf_putc(struct ringbuf *rbuf, char c);
static char rbuf_getc(struct ringbuf *rbuf);

// EXPORTED FUNCTION DEFINITIONS
//

// void uart_attach(void *mmio_base, int irqno)
// Inputs: void *mmio_base - base pointer to the base address of the mmio uart registers
//         int irqno - interupt request number
// Outputs: None
// Description: Regesters the uart device, attaches the io interface, and sets the base address of the device regs.
// Side Effects: Allocates uart_device struct instance uart.
void uart_attach(void *mmio_base, int irqno)
{
    static const struct iointf uart_iointf = {
        .close = &uart_close,
        .read = &uart_read,
        .write = &uart_write};

    struct uart_device *uart;

    uart = kcalloc(1, sizeof(struct uart_device));

    uart->regs = mmio_base;
    uart->irqno = irqno;

    ioinit0(&uart->io, &uart_iointf);

    // Check if we're trying to attach UART0, which is used for the console. It
    // had already been initialized and should not be accessed as a normal
    // device.

    if (mmio_base != (void *)UART0_MMIO_BASE)
    {

        uart->regs->ier = 0;
        uart->regs->lcr = LCR_DLAB;
        // fence o,o ?
        uart->regs->dll = 0x01;
        uart->regs->dlm = 0x00;
        // fence o,o ?
        uart->regs->lcr = 0; // DLAB=0

        uart->instno = register_device(UART_NAME, uart_open, uart);
    }
    else
        uart->instno = register_device(UART_NAME, NULL, NULL);
}

// int uart_open(struct io **ioptr, void *aux)
// Inputs: struct io **ioptr - contains pointer to the referance of the uart io struct (for a given uart instance)
//         void *aux - contains a pointer to the uart Device struct
// Outputs: 0 if succesful and -EBUSY if there is already a uart ref
// Description: Associates io reference with the uart device, initialize rbufs, and enable uart intr source
// Side Effects: Flushes uart->regs->rbr, and modifes other registers
int uart_open(struct io **ioptr, void *aux)
{
    struct uart_device *const uart = aux;
    condition_init(&uart->uart_read_cond, "uartRead");
    condition_init(&uart->uart_write_cond, "uartWrite");
    lock_init(&uart->uart_lock);
    if (ioptr == NULL || uart == NULL)
    {
        panic("Bad Args for uart_open");
    }

    if (iorefcnt(&uart->io) != 0)
        return -EBUSY;

    // Reset receive and transmit buffers

    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // FIXME your code goes here
    uart->regs->ier |= IER_DRIE;
    enable_intr_source(uart->irqno, UART_INTR_PRIO, &uart_isr, aux);
    *ioptr = ioaddref(&uart->io);
    return 0;
}

// void uart_close(struct io *io)
// Inputs: struct io *io - io pointer to the io associated with the uart device
// Outputs: None
// Description: ensures the io reference count for the io argument is 0, and disables interupts for uart source.
// Side Effects: None.
void uart_close(struct io *io)
{
    if (io == NULL)
    {
        panic("Bad Args for uart_close");
    }
    struct uart_device *const uart =
        (void *)io - offsetof(struct uart_device, io);

    trace("%s()", __func__);
    assert(iorefcnt(io) == 0);

    // FIXME your code goes here
    disable_intr_source(uart->irqno);
}

// long uart_read(struct io *io, void *buf, long bufsz)
// Inputs: struct io *io - io pointer to the io associated with the uart device
//         void *buf -  pointer to the buffer where the recivied characters will be stored
//         long bufsz -  size of the buffer in bytes
// Outputs: long - returns size of the data written or -EINVAL if invalid bufsz
// Description: gets the uart character data and writes it to buf.
// Side Effects: Calls condition (may modify a wait list).
long uart_read(struct io *io, void *buf, long bufsz)
{
    if (io == NULL || buf == NULL)
    {
        panic("Bad Args for uart_read");
    }
    struct uart_device *const uart =
        (void *)io - offsetof(struct uart_device, io);
    // FIXME your code goes here
    if (bufsz > 0) //&& bufsz < UART_RBUFSZ) ??? do I need this
    {
        lock_acquire(&uart->uart_lock);
        int pie = disable_interrupts();
        while (rbuf_empty(&uart->rxbuf))
        {
            // continue;
            //  put thread to sleep via condition wait
            condition_wait(&uart->uart_read_cond);
        }
        restore_interrupts(pie);
        for (long i = 0; i < bufsz; i++)
        {
            if (!rbuf_empty(&uart->rxbuf))
            {
                char c = rbuf_getc(&uart->rxbuf);
                ((char *)(buf))[i] = c;
                uart->regs->ier |= IER_DRIE;
            }
            else
            {
                lock_release(&uart->uart_lock);
                return i;
            }
        }
        lock_release(&uart->uart_lock);
        return bufsz;
    }
    else if (bufsz == 0)
    {
        return 0;
    }

    return -EINVAL;
}

// long uart_write(struct io *io, const void *buf, long len)
// Inputs: struct io *io - io pointer to the io associated with the uart device
//         void *buf -  pointer to the buffer where the recivied characters will be stored
//         long bufsz -  size of the buffer in bytes
// Outputs: long - returns size of the data written or -EINVAL if invalid bufsz
// Description: writes data from buf to txbux ring buffer.
// Side Effects: Calls condition (may modify a wait list).
long uart_write(struct io *io, const void *buf, long len)
{
    assert(io != NULL);
    if(buf == NULL){
        return 0; //???
    }
    // if (io == NULL || buf == NULL)
    // {
    //     panic("Bad Args for uart_write");
    // }
    // FIXME your code goes here
    struct uart_device *const uart =
        (void *)io - offsetof(struct uart_device, io);
    if (len > 0)
    {
        lock_acquire(&uart->uart_lock);
        for (long i = 0; i < len; i++)
        {
            int pie = disable_interrupts();
            while (rbuf_full(&uart->txbuf))
            {
                // continue;
                condition_wait(&uart->uart_write_cond);
            }
            restore_interrupts(pie);
            char c = ((char *)buf)[i];
            rbuf_putc(&uart->txbuf, c);
            uart->regs->ier |= IER_THREIE;
        }
        lock_release(&uart->uart_lock);

        return len;
    }
    return -EINVAL;
}
// void uart_isr(int srcno, void *aux)
// Inputs: int srcno - the source number for the interreupt.
//         void *aux - contains a pointer to the uart Device struct
// Outputs: None
// Description: This function serves as the uart interrupt service routine.
// Side Effects: None.
void uart_isr(int srcno, void *aux)
{
    // FIXME your code goes here
    trace("%s()", __func__);
    struct uart_device *const uart = (struct uart_device *)aux;
    if (uart == NULL)
    {
        panic("Bad Args for uart_isr");
    }
    if (((uart->regs->lsr & LSR_DR) == LSR_DR) && !rbuf_full(&uart->rxbuf))
    {
        rbuf_putc(&uart->rxbuf, uart->regs->rbr);
        condition_broadcast(&uart->uart_read_cond);
    }
    if (!rbuf_empty(&uart->txbuf))
    {
        uart->regs->thr = rbuf_getc(&uart->txbuf);
        condition_broadcast(&uart->uart_write_cond);
    }
    if (rbuf_full(&uart->rxbuf))
    {
        uart->regs->ier &= ~(IER_DRIE);
    }
    if (rbuf_empty(&uart->txbuf))
    {
        uart->regs->ier &= ~(IER_THREIE);
    }
}

void rbuf_init(struct ringbuf *rbuf)
{
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}

int rbuf_empty(const struct ringbuf *rbuf)
{
    return (rbuf->hpos == rbuf->tpos);
}

int rbuf_full(const struct ringbuf *rbuf)
{
    return ((uint16_t)(rbuf->tpos - rbuf->hpos) == UART_RBUFSZ);
}

void rbuf_putc(struct ringbuf *rbuf, char c)
{
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf *rbuf)
{
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs *)UART0_MMIO_BASE)

void console_device_init(void)
{
    UART0.ier = 0x00;

    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.

    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    // The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c)
{
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void)
{
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;

    return UART0.rbr;
}