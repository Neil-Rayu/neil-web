/*! @file syscall.c
    @brief system call handlers
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "scnum.h"
#include "process.h"
#include "memory.h"
#include "io.h"
#include "device.h"
#include "fs.h"
#include "intr.h"
#include "timer.h"
#include "error.h"
#include "thread.h"
#include "process.h"
#include "ktfs.h"
#include "dev/fbuf.h"
// #define ENULLIO 239

// EXPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame *tfr); // called from excp.c

// INTERNAL FUNCTION DECLARATIONS
//

static int64_t syscall(const struct trap_frame *tfr);

static int sysexit(void);
static int sysexec(int fd, int argc, char **argv);
static int sysfork(const struct trap_frame *tfr);
static int syswait(int tid);
static int sysprint(const char *msg);
static int sysusleep(unsigned long us);

static int sysdevopen(int fd, const char *name, int instno);
static int sysfsopen(int fd, const char *name);

static int sysclose(int fd);
static long sysread(int fd, void *buf, size_t bufsz);
static long syswrite(int fd, const void *buf, size_t len);
static int sysioctl(int fd, int cmd, void *arg);
static int syspipe(int *wfdptr, int *rfdptr);
static int sysiodup(int oldfd, int newfd);

static int sysfscreate(const char *name);
static int sysfsdelete(const char *name);
// EXPORTED FUNCTION DEFINITIONS
//

void handle_syscall(struct trap_frame *tfr)
{
    tfr->sepc += 4;
    tfr->a0 = syscall(tfr);
}

// INTERNAL FUNCTION DEFINITIONS
//

int64_t syscall(const struct trap_frame *tfr)
{
    switch (tfr->a7)
    {
    case SYSCALL_EXIT:
        return sysexit();
        break;
    case SYSCALL_EXEC:
        return sysexec((int)tfr->a0, (int)tfr->a1, (char **)tfr->a2);
        break;
    case SYSCALL_WAIT:
        return syswait((int)tfr->a0);
        break;
    case SYSCALL_PRINT:
        return sysprint((const char *)tfr->a0);
        break;
    case SYSCALL_USLEEP:
        return sysusleep((unsigned long)tfr->a0);
        break;
    case SYSCALL_DEVOPEN:
        return sysdevopen((int)tfr->a0, (const char *)tfr->a1, (int)tfr->a2);
        break;
    case SYSCALL_FSOPEN:
        return sysfsopen((int)tfr->a0, (const char *)tfr->a1);
        break;
    case SYSCALL_CLOSE:
        return sysclose((int)tfr->a0);
        break;
    case SYSCALL_READ:
        return sysread((int)tfr->a0, (void *)tfr->a1, (size_t)tfr->a2);
        break;
    case SYSCALL_WRITE:
        return syswrite((int)tfr->a0, (void *)tfr->a1, (size_t)tfr->a2);
        break;
    case SYSCALL_IOCTL:
        return sysioctl((int)tfr->a0, (int)tfr->a1, (void *)tfr->a2);
        break;
    case SYSCALL_FSCREATE:
        return sysfscreate((const char *)tfr->a0);
        break;
    case SYSCALL_FSDELETE:
        return sysfsdelete((const char *)tfr->a0);
        break;
    case SYSCALL_FORK:
        return sysfork(tfr);
        break;
    case SYSCALL_PIPE:
        return syspipe((int *)tfr->a0, (int *)tfr->a1);
        break;
    case SYSCALL_IODUP:
        return sysiodup((int)tfr->a0, (int)tfr->a1);
        break;
    default:
        break;
    }
    return -ENOTSUP;
}

int sysexit(void)
{
    process_exit();
}

int sysexec(int fd, int argc, char **argv)
{
    // kprintf("Exec #\n");
    if (fd < 0 || fd > PROCESS_IOMAX)
    {
        return -EBADFD;
    }
    if (current_process()->iotab[fd] != NULL)
    {
        process_exec(current_process()->iotab[fd], argc, argv);
        sysclose(fd);
    }
    return 0;
}

int sysfork(const struct trap_frame *tfr)
{
    return process_fork(tfr);
}

int syswait(int tid)
{
    if (0 <= tid)
    {
        return thread_join(tid);
    }
    else
    {
        return -EINVAL;
    }
}

// Prints to console.
// Validates that msg string is valid (NULL-terminated)
// and then prints it to the console in the following format: <thread_name:thread_num> msg
// Parameters
// msg	string msg in userspace
// Returns
// 0 on sucess else error from validate_vstr
int sysprint(const char *msg)
{
    kprintf("Thread <%s:%d> says: %s\n", thread_name(running_thread()), running_thread(), msg);
    return 0;
}

// return fd number if sucessful else return error on invalid file descriptor or empty file descriptor
// Is it supposed to copy into same process
int sysiodup(int oldfd, int newfd)
{
    if (newfd >= 0 && oldfd >= 0 && newfd < PROCESS_IOMAX && oldfd < PROCESS_IOMAX)
    {

        if (current_process()->iotab[newfd] == NULL && current_process()->iotab[oldfd] != NULL)
        {
            current_process()->iotab[newfd] = ioaddref(current_process()->iotab[oldfd]);
            return newfd;
        }
        // else if(current_process()->iotab[newfd] != NULL && current_process()->iotab[oldfd] != NULL){
        //     char buf[1];
        //     if(ioread(current_process()->iotab[newfd], buf, 1) == (-ENULLIO)){
        //         ioclose(current_process()->iotab[newfd]);
        //         current_process()->iotab[newfd] = ioaddref(current_process()->iotab[oldfd]);
        //         return newfd;
        //     }
        // }
        else
        {
            current_process()->iotab[newfd] = NULL;
            return newfd;
        }
    }
    else if (newfd < 0 && oldfd >= 0 && oldfd < PROCESS_IOMAX)
    {
        for (int i = 0; i < PROCESS_IOMAX; i++)
        {
            if (current_process()->iotab[i] == NULL)
            {
                current_process()->iotab[i] = ioaddref(current_process()->iotab[oldfd]);
                return i;
            }
        }
    }
    return -EBADFD;
}

int sysusleep(unsigned long us)
{
    sleep_us(us);
    return 0;
}

int sysdevopen(int fd, const char *name, int instno)
{
    if (fd >= 0)
    {
        if (fd < PROCESS_IOMAX && current_process()->iotab[fd] == NULL)
        {
            int ret = open_device(name, instno, &(current_process()->iotab[fd]));
            if (ret < 0)
            {
                return ret;
            }
            return fd;
        }
        else
        {
            return -EBADFD;
        }
    }
    else
    {
        for (int i = 0; i < PROCESS_IOMAX; i++)
        {
            if (current_process()->iotab[i] == NULL)
            {
                int ret = open_device(name, instno, &(current_process()->iotab[i]));
                if (ret < 0)
                {
                    return ret;
                }
                return i;
            }
        }
        return -EMFILE;
    }

    return 0;
}

int sysfsopen(int fd, const char *name)
{
    // kprintf("\nName: %s and FD: %d", name, fd);
    if (fd >= 0)
    {
        if (fd < PROCESS_IOMAX && current_process()->iotab[fd] == NULL)
        {
            int ret = fsopen(name, &current_process()->iotab[fd]);
            if (ret < 0)
            {
                return ret;
            }
            return fd;
        }
        else
        {
            return -EBADFD;
        }
    }
    else
    {
        for (int i = (PROCESS_IOMAX-1); i >= 0; i++)
        {
            if (current_process()->iotab[i] == NULL)
            {
                int ret = fsopen(name, &current_process()->iotab[i]);
                if (ret < 0)
                {
                    return ret;
                }
                // kprintf("\nName: %s and FD: %d", name, i);
                return i;
            }
        }
        return -EMFILE;
    }

    return 0;
}

int sysclose(int fd)
{
    // kprintf("\nName: %p and FD: %d", current_process()->iotab[fd], fd);
    if (fd >= 0 && fd < PROCESS_IOMAX)
    {
        if (current_process()->iotab[fd] != NULL)
        {
            ioclose(current_process()->iotab[fd]);
            current_process()->iotab[fd] = NULL;
            return 0;
        }
        return -EBADFD;
    }
    else
    {
        return -EBADFD;
    }

    return 0;
}

long sysread(int fd, void *buf, size_t bufsz)
{
    if (fd >= 0)
    {
        long ret = ioread(current_process()->iotab[fd], buf, bufsz);
        if (ret > bufsz)
        {
            return -EINVAL;
        }
        return ret;
    }

    return -EBADFD;
}

long syswrite(int fd, const void *buf, size_t len)
{
    if (fd >= 0)
    {
        // kprintf("ERROR FD: %d\n", fd);

        // assert(current_process()->iotab[fd] != NULL);
        long ret = iowrite(current_process()->iotab[fd], buf, len);
        if (ret > len)
        {
            return -EINVAL;
        }
        return ret;
    }
    return -EBADFD;
}

int sysioctl(int fd, int cmd, void *arg)
{
    if (fd >= 0)
    {
        return ioctl(current_process()->iotab[fd], cmd, arg);
    }
    return -EBADFD;
}

int syspipe(int *wfdptr, int *rfdptr)
{

    if (*wfdptr >= 0 && *rfdptr >= 0)
    {
        if (*wfdptr == *rfdptr)
        {
            return -EBADFD;
        }
        if (*wfdptr < PROCESS_IOMAX && *rfdptr < PROCESS_IOMAX)
        {
            // handle case
            if (current_process()->iotab[*wfdptr] == NULL && current_process()->iotab[*rfdptr] == NULL)
            {
                // How do we open writer and reader device
                create_pipe(&(current_process()->iotab[*wfdptr]), &(current_process()->iotab[*rfdptr]));
                return 0;
            }
            else
            {
                return -EBADFD;
            }
        }
        else
        {
            return -EBADFD;
        }
    }
    else
    {
        for (int i = 0; i < PROCESS_IOMAX; i++)
        {
            if (current_process()->iotab[i] == NULL)
            {
                for (int j = 0; j < PROCESS_IOMAX; j++)
                {
                    if (current_process()->iotab[j] == NULL)
                    {
                        if (i == j)
                        {
                            continue;
                        }
                        // handle case
                        *wfdptr = i;
                        *rfdptr = j;
                        create_pipe(&(current_process()->iotab[*wfdptr]), &(current_process()->iotab[*rfdptr]));
                        return 0;
                    }
                }
                return -EBADFD;
            }
        }
        return -EMFILE;
    }

    return 0;
}

int sysfscreate(const char *name)
{
    return fscreate(name);
}

int sysfsdelete(const char *name)
{
    return fsdelete(name);
}
