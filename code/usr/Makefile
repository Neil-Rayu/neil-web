# Based on xv6-riscv Makefile; see LICENSE.

TOOLPREFIX=riscv64-unknown-elf-
QEMU=qemu-system-riscv64

CC=$(TOOLPREFIX)gcc
AS=$(TOOLPREFIX)as
LD=$(TOOLPREFIX)ld
OBJCOPY=$(TOOLPREFIX)objcopy
OBJDUMP=$(TOOLPREFIX)objdump

UMODE := 1

ULIB_OBJS = \
	io.o \
	string.o \
	syscall.o \
	heap.o

ULIB_LD = no_umode.ld

ifeq ($(UMODE), 1)
	ULIB_OBJS += start.o
	ULIB_LD = umode.ld
endif

ALL_TARGETS = \
	hello 

CFLAGS = -Wall -fno-omit-frame-pointer -ggdb3 -gdwarf-2
CFLAGS += -mcmodel=medany -fno-pie -no-pie -march=rv64g -mabi=lp64d
CFLAGS += -ffreestanding -fno-common -nostdlib -mno-relax
CFLAGS += -fno-asynchronous-unwind-tables
CFLAGS += -I.

ifeq ($(UMODE), 1)
	CFLAGS += -DUMODE
endif

all: $(ALL_TARGETS)

hello: $(ULIB_OBJS) hello.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

reader: $(ULIB_OBJS) reader.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

writer: $(ULIB_OBJS) writer.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

fib: $(ULIB_OBJS) fib.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

trekfib: $(ULIB_OBJS) trekfib.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

sysArg_test: $(ULIB_OBJS) sysArg_test.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

bin: 
	mkdir $@

clean:
	rm -rf *.o *.elf *.asm
