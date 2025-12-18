
// #include "conf.h"
// #include "console.h"
// #include "elf.h"
// #include "assert.h"
// #include "thread.h"
// #include "process.h"
// #include "memory.h"
// #include "fs.h"
// #include "io.h"
// #include "device.h"
// #include "dev/rtc.h"
// #include "dev/uart.h"
// #include "intr.h"
// #include "dev/virtio.h"
// #include "heap.h"
// #include "string.h"

// #define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE - VIRTIO0_MMIO_BASE)
// extern char _kimg_end[];

// void test_kernel_pipe(void)
// {
//     struct io *wpipe, *rpipe;
//     create_pipe(&wpipe, &rpipe);

//     const char *msg = "Hello from kernel pipe!\n";
//     char buf[64] = {0};

//     long written = iowrite(wpipe, msg, strlen(msg));
//     if (written < 0)
//         kprintf("Write failed: %ld\n", written);
//     else
//         kprintf("Wrote %ld bytes\n", written);

//     long read_bytes = ioread(rpipe, buf, sizeof(buf));
//     if (read_bytes < 0)
//         kprintf("Read failed: %ld\n", read_bytes);
//     else
//     {
//         kprintf("Read %ld bytes (buffer size: %ld): %s\n",
//                 read_bytes, sizeof(buf), buf);
//     }
// }

// void main(void)
// {
//     struct io *blkio;
//     int result;
//     int i;

//     console_init();
//     devmgr_init();
//     intrmgr_init();
//     thrmgr_init();
//     memory_init();
//     procmgr_init();

//     uart_attach((void *)UART0_MMIO_BASE, UART0_INTR_SRCNO + 0);
//     uart_attach((void *)UART1_MMIO_BASE, UART0_INTR_SRCNO + 1);
//     rtc_attach((void *)RTC_MMIO_BASE);

//     for (i = 0; i < 8; i++)
//     {
//         virtio_attach((void *)VIRTIO0_MMIO_BASE + i * VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
//     }

//     result = open_device("vioblk", 0, &blkio);
//     if (result < 0)
//     {
//         kprintf("Error: %d\n", result);
//         panic("Failed to open vioblk\n");
//     }

//     result = fsmount(blkio);
//     if (result < 0)
//     {
//         kprintf("Error: %d\n", result);
//         panic("Failed to mount filesystem\n");
//     }

//     struct io *sysArgPrintio;
//     result = fsopen("shell.elf", &sysArgPrintio);
//     if (result < 0)
//     {
//         kprintf("Error: %d\n", result);
//         panic("Failed to open syswait_test\n");
//     }
//     // // current_process()->iotab[2] = termio;
//     // // process_exec(trekio, 2, (char **)argv);
//     process_exec(sysArgPrintio, 0, NULL);
//     // test_kernel_pipe();
// }

// #include "conf.h"
// #include "console.h"
// #include "elf.h"
// #include "assert.h"
// #include "thread.h"
// #include "process.h"
// #include "memory.h"
// #include "fs.h"
// #include "io.h"
// #include "device.h"
// #include "dev/rtc.h"
// #include "dev/uart.h"
// #include "intr.h"
// #include "dev/virtio.h"
// #include "heap.h"
// #include "string.h"

// void test_kernel_pipe(void);
// void read_func(struct io *io);

// #define UART2_MMIO_BASE 0x10000200UL // PMA

// #define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE - VIRTIO0_MMIO_BASE)
// extern char _kimg_end[];

// #define INIT_NAME "trekfib"
// #define NUM_UARTS 3

// void main(void)
// {
//     struct io *blkio;
//     int result;
//     int i;

//     console_init();
//     devmgr_init();
//     intrmgr_init();
//     thrmgr_init();
//     memory_init();
//     procmgr_init();

//     // uart_attach((void *)UART0_MMIO_BASE, UART0_INTR_SRCNO + 0);
//     // uart_attach((void *)UART1_MMIO_BASE, UART0_INTR_SRCNO + 1);
//     // uart_attach((void *)UART2_MMIO_BASE, UART0_INTR_SRCNO + 2);
//     rtc_attach((void *)RTC_MMIO_BASE);

//     for (i = 0; i < NUM_UARTS; i++) // change for number of UARTs
//         uart_attach((void *)UART_MMIO_BASE(i), UART0_INTR_SRCNO + i);

//     for (i = 0; i < 8; i++)
//     {
//         virtio_attach((void *)VIRTIO0_MMIO_BASE + i * VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
//     }
//     enable_interrupts();

//     result = open_device("vioblk", 0, &blkio);
//     if (result < 0)
//     {
//         kprintf("Error: %d\n", result);
//         panic("Failed to open vioblk\n");
//     }

//     result = fsmount(blkio);
//     if (result < 0)
//     {
//         kprintf("Error: %d\n", result);
//         panic("Failed to mount filesystem\n");
//     }

//     // test_kernel_pipe();
//     // insert testcase below
//     // This test case will run fib and trek simultaneously.

//     struct io *sysArgPrintio;
//     result = fsopen("sysArg_test", &sysArgPrintio);
//     if (result < 0)
//     {
//         kprintf("Error: %d\n", result);
//         panic("Failed to open syswait_test\n");
//     }
//     // current_process()->iotab[2] = termio;
//     // process_exec(trekio, 2, (char **)argv);
//     process_exec(sysArgPrintio, 0, NULL);
//     // test_kernel_pipe();
// }

#include "conf.h"
#include "console.h"
#include "elf.h"
#include "assert.h"
#include "thread.h"
#include "process.h"
#include "memory.h"
#include "fs.h"
#include "io.h"
#include "device.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "intr.h"
#include "dev/virtio.h"
#include "heap.h"
#include "string.h"
// void test_kernel_pipe(void);
// void read_func(struct io *io);

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE - VIRTIO0_MMIO_BASE)
extern char _kimg_end[];

#define INIT_NAME "trekfib"
#define TEST_NAME "sysArg_test"
#define NUM_UARTS 3

void main(void)
{
  struct io *blkio;
  int result;
  int i;

  console_init();
  devmgr_init();
  intrmgr_init();
  thrmgr_init();
  memory_init();
  procmgr_init();

  // uart_attach((void *)UART0_MMIO_BASE, UART0_INTR_SRCNO + 0);
  // uart_attach((void *)UART1_MMIO_BASE, UART0_INTR_SRCNO + 1);
  rtc_attach((void *)RTC_MMIO_BASE);

  for (i = 0; i < NUM_UARTS; i++) // change for number of UARTs
    uart_attach((void *)UART_MMIO_BASE(i), UART0_INTR_SRCNO + i);

  for (i = 0; i < 8; i++)
  {
    virtio_attach((void *)VIRTIO0_MMIO_BASE + i * VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
  }
  enable_interrupts();

  result = open_device("vioblk", 0, &blkio);
  if (result < 0)
  {
    kprintf("Error: %d\n", result);
    panic("Failed to open vioblk\n");
  }

  result = fsmount(blkio);
  if (result < 0)
  {
    kprintf("Error: %d\n", result);
    panic("Failed to mount filesystem\n");
  }

  // insert testcase below
  // This test case will run fib and trek simultaneously.
  // struct io *trekFibio;
  // result = fsopen(INIT_NAME, &trekFibio);
  // if (result < 0)
  // {
  //     kprintf(INIT_NAME ": %s; Unable to open\n");
  //     panic("Failed to open trekfib\n");
  // }
  // result = process_exec(trekFibio, 0, NULL);
  struct io *trekFibio;
  result = fsopen("shell.elf", &trekFibio);
  if (result < 0)
  {
    kprintf(TEST_NAME ": %s; Unable to open\n");
    panic("Failed to open trekfib\n");
  }
  result = process_exec(trekFibio, 0, NULL);
}

// void test_kernel_pipe(void)
// {
//     struct io *wpipe, *rpipe;
//     create_pipe(&wpipe, &rpipe);

//     const char *msg = "Hello from kernel pipe!\n";
//     // char buf[64] = {0};

//     long written = iowrite(wpipe, msg, strlen(msg));
//     if (written < 0)
//         kprintf("Write failed: %ld\n", written);
//     else
//         kprintf("Wrote %ld bytes\n", written);

//     thread_spawn("pipe_thr", (void *)&read_func, rpipe);
//     thread_yield();
// }

// void read_func(struct io *io)
// {
//     // kprintf("got here\n");
//     char buf[64] = {0};
//     long read_bytes = ioread(io, buf, sizeof(buf));
//     if (read_bytes < 0)
//         kprintf("Read failed: %ld\n", read_bytes);
//     else
//     {
//         buf[read_bytes] = '\0';
//         kprintf("Read %ld bytes: %s\n", read_bytes, buf);
//     }
// }
