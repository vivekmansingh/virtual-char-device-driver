/*
 * mydev_ioctl.h - Definitions SHARED by the kernel module and the user app.
 * =========================================================================
 *
 * Why a shared header?
 *   The kernel module (mydev.c) and the user program (app.cpp) are compiled
 *   completely separately: one runs inside the kernel, the other is a normal
 *   program.  They still have to agree on a few "contract" values:
 *     - how big the kernel buffer is, and
 *     - which numbers mean which ioctl command.
 *   Putting these in ONE header that both files #include guarantees that the
 *   two sides can never disagree.
 *
 * This file is written so it compiles in three situations:
 *   1. inside the kernel   (the build system defines __KERNEL__)
 *   2. in a C user program
 *   3. in a C++ user program (app.cpp)
 */

/* "Include guard": if this header is included twice in the same file,
 * the second copy is skipped, so nothing gets defined twice. */
#ifndef MYDEV_IOCTL_H
#define MYDEV_IOCTL_H

/* The _IO / _IOR macros used below live in different headers depending on
 * whether we are compiling kernel code or user code. */
#ifdef __KERNEL__
#include <linux/ioctl.h>   /* kernel-side definition of _IO, _IOR, ...      */
#else
#include <sys/ioctl.h>     /* user-side: also declares the ioctl() function */
#endif

/* Size of the storage buffer that lives inside the kernel: 1 KB. */
#define MYDEV_BUFFER_SIZE 1024

/*
 * ioctl command numbers
 * ---------------------
 * An ioctl ("input/output control") command is just a 32-bit number.  Linux
 * recommends building it with helper macros that pack 4 fields together:
 *
 *     direction | size of the argument | "magic" type byte | command number
 *
 *   _IO(type, nr)         -> command with NO data argument
 *   _IOR(type, nr, T)     -> command where the kernel WRITES a T back to the
 *                            user ("R" = the user Reads the result)
 *   _IOW(type, nr, T)     -> command where the user passes a T into kernel
 *
 * The "magic" byte is a letter that identifies OUR driver, so a command meant
 * for some other driver is very unlikely to be mistaken for one of ours.
 */
#define MYDEV_IOC_MAGIC 'm'

/* Erase the buffer.  Takes no argument. */
#define MYDEV_IOC_CLEAR     _IO(MYDEV_IOC_MAGIC, 0)

/* Ask for the buffer CAPACITY (always 1024).  Kernel writes an int back. */
#define MYDEV_IOC_GET_SIZE  _IOR(MYDEV_IOC_MAGIC, 1, int)

/* Ask how many bytes are CURRENTLY stored.  Kernel writes an int back. */
#define MYDEV_IOC_GET_LEN   _IOR(MYDEV_IOC_MAGIC, 2, int)

#endif /* MYDEV_IOCTL_H */
