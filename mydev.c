// SPDX-License-Identifier: GPL-2.0
/*
 * mydev.c - A virtual character device driver (no hardware needed).
 * ==================================================================
 *
 * WHAT THIS MODULE DOES
 *   When loaded with `insmod mydev.ko`, it creates a device file called
 *   /dev/mydev.  Any program can then:
 *     - open()  /dev/mydev
 *     - write() bytes into it   -> bytes are stored in a 1 KB kernel buffer
 *     - read()  bytes from it   -> bytes are copied back out of that buffer
 *     - ioctl() special commands -> clear the buffer / ask its size
 *     - close() it
 *   There is no real hardware: the "device" is just memory inside the kernel.
 *
 * KEY IDEAS FOR A BEGINNER
 *   - A "character device" is a device you talk to as a stream of bytes
 *     (like a keyboard or serial port), as opposed to a "block device"
 *     (like a disk) that is accessed in fixed-size blocks.
 *   - Every device file has a MAJOR number (which driver handles it) and a
 *     MINOR number (which instance of that driver).  Run `ls -l /dev/mydev`
 *     and you will see them printed as "major, minor".
 *   - The kernel finds our functions through a `struct file_operations`
 *     table: "when someone calls read() on this file, call mydev_read()".
 *   - Kernel code may NOT touch user memory directly.  We must use
 *     copy_to_user() / copy_from_user() / put_user(), which check that the
 *     user pointer is valid and handle faults safely.
 *   - Several processes may use the device at the same time, so we protect
 *     the shared buffer with a MUTEX (a lock only one thread can hold).
 *   - We log with printk().  Messages go to the kernel log, which you read
 *     with `sudo dmesg` or `journalctl -k`.
 */

/* ---------------------------------------------------------------------- */
/* Header files                                                           */
/* ---------------------------------------------------------------------- */
#include <linux/module.h>   /* MODULE_LICENSE, module_init, THIS_MODULE ... */
#include <linux/init.h>     /* __init and __exit markers                    */
#include <linux/kernel.h>   /* printk() and KERN_INFO / KERN_ERR levels     */
#include <linux/fs.h>       /* struct file_operations, alloc_chrdev_region  */
#include <linux/cdev.h>     /* struct cdev, cdev_init(), cdev_add()         */
#include <linux/device.h>   /* class_create(), device_create()              */
#include <linux/mutex.h>    /* struct mutex, mutex_lock(), mutex_unlock()   */
#include <linux/uaccess.h>  /* copy_to_user(), copy_from_user(), put_user() */
#include <linux/string.h>   /* memset()                                     */
#include <linux/version.h>  /* LINUX_VERSION_CODE, KERNEL_VERSION()         */

#include "mydev_ioctl.h"    /* OUR shared definitions (buffer size, ioctls) */

/* ---------------------------------------------------------------------- */
/* Constants                                                              */
/* ---------------------------------------------------------------------- */
#define DEVICE_NAME "mydev"        /* name shown in /proc/devices and /dev */
#define CLASS_NAME  "mydev_class"  /* name shown under /sys/class/          */

/* ---------------------------------------------------------------------- */
/* Global (module-wide) variables                                         */
/*   "static" here means: visible only inside this .c file.  That keeps    */
/*   our names from clashing with anything else in the kernel.             */
/* ---------------------------------------------------------------------- */

/* dev_t packs the major and minor numbers into one value. */
static dev_t mydev_number;

/* The kernel's internal object that represents our character device. */
static struct cdev mydev_cdev;

/* A "device class" groups similar devices in /sys/class/.  Creating one
 * lets udev notice our device and automatically create /dev/mydev. */
static struct class *mydev_class;

/* The device object registered under our class (backs /dev/mydev). */
static struct device *mydev_device;

/* THE STORAGE: a 1 KB array that lives in kernel memory. */
static char device_buffer[MYDEV_BUFFER_SIZE];

/* How many bytes of device_buffer currently hold valid data (0..1024). */
static size_t data_length;

/* The lock that protects device_buffer and data_length.
 * DEFINE_MUTEX both declares the variable and initialises it. */
static DEFINE_MUTEX(mydev_lock);

/* How many times the device is currently open (only used for logging).
 * atomic_t can be safely changed by many CPUs without a lock. */
static atomic_t open_count = ATOMIC_INIT(0);

/* ====================================================================== */
/* File operations                                                        */
/*   Each function below is called by the kernel when a user program      */
/*   performs the matching system call on /dev/mydev.                      */
/* ====================================================================== */

/*
 * mydev_open - called for every open("/dev/mydev", ...) system call.
 * @inode: the file-system object for /dev/mydev (holds major/minor).
 * @filp:  a NEW "open file" object created for this particular open().
 *         Each open() gets its own filp with its own file position.
 *
 * Returns 0 for success (a negative error code would make open() fail).
 */
static int mydev_open(struct inode *inode, struct file *filp)
{
	/* atomic_inc_return adds 1 and gives back the new value. */
	int count = atomic_inc_return(&open_count);

	/* KERN_INFO is the log level ("informational").  %d prints an int. */
	printk(KERN_INFO "mydev: device opened (open handles now: %d)\n", count);
	return 0;
}

/*
 * mydev_release - called when the LAST reference to an open file goes
 * away, i.e. when the program calls close() (or exits).
 */
static int mydev_release(struct inode *inode, struct file *filp)
{
	int count = atomic_dec_return(&open_count);  /* subtract 1 */

	printk(KERN_INFO "mydev: device closed (open handles now: %d)\n", count);
	return 0;
}

/*
 * mydev_read - called for read(fd, buf, count).
 * @filp:  the open file.
 * @ubuf:  pointer into the USER program's memory where data must go.
 *         The "__user" tag reminds us (and static checkers) that we must
 *         never dereference it directly.
 * @count: how many bytes the user asked for.
 * @ppos:  pointer to this open file's current position (offset).
 *
 * Returns: number of bytes copied, 0 for "end of file", or a negative
 *          error code such as -EFAULT.
 */
static ssize_t mydev_read(struct file *filp, char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	ssize_t ret;          /* value we will return */
	size_t available;     /* bytes left between position and end of data */

	/* Take the lock.  The "_interruptible" version lets the user kill the
	 * process with Ctrl+C while it waits.  It returns non-zero if a signal
	 * interrupted the wait; -ERESTARTSYS tells the kernel to retry or
	 * report EINTR to the program. */
	if (mutex_lock_interruptible(&mydev_lock))
		return -ERESTARTSYS;

	/* A negative position is never valid. */
	if (*ppos < 0) {
		ret = -EINVAL;
		goto out;
	}

	/* If the position is at (or past) the end of the stored data there is
	 * nothing more to read.  Returning 0 means "end of file", which is how
	 * programs like `cat` know to stop reading. */
	if (*ppos >= data_length) {
		ret = 0;
		goto out;
	}

	/* Never hand out more bytes than actually exist. */
	available = data_length - *ppos;
	if (count > available)
		count = available;

	/* Copy from kernel memory -> user memory.  copy_to_user returns the
	 * number of bytes it could NOT copy, so non-zero means the user gave
	 * us a bad pointer: report "bad address". */
	if (copy_to_user(ubuf, device_buffer + *ppos, count)) {
		ret = -EFAULT;
		goto out;
	}

	/* Move this file's position forward so the next read() continues
	 * where this one stopped. */
	*ppos += count;
	ret = count;

	printk(KERN_INFO "mydev: read %zu bytes (new position %lld)\n",
	       count, (long long)*ppos);

out:
	/* ALWAYS release the lock on every exit path - the "goto out" pattern
	 * makes that easy to guarantee. */
	mutex_unlock(&mydev_lock);
	return ret;
}

/*
 * mydev_write - called for write(fd, buf, count).
 *
 * Behaviour (kept simple on purpose):
 *   - Data is written starting at the file's current position.
 *   - After the write, the stored data ENDS where this write ended.
 *     So writing "hi" at position 0 after "hello" leaves just "hi"
 *     (no leftover "llo").  Consecutive writes on the same open file
 *     still append, because the position keeps moving forward.
 *   - If the buffer is full we store what fits and return that smaller
 *     number (a "partial write").  If nothing fits we return -ENOSPC
 *     ("No space left on device").
 */
static ssize_t mydev_write(struct file *filp, const char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	ssize_t ret;
	size_t space;         /* free bytes between position and buffer end */

	if (mutex_lock_interruptible(&mydev_lock))
		return -ERESTARTSYS;

	if (*ppos < 0) {
		ret = -EINVAL;
		goto out;
	}

	/* Position already at the very end of the 1 KB buffer: no room. */
	if (*ppos >= MYDEV_BUFFER_SIZE) {
		ret = -ENOSPC;
		goto out;
	}

	/* Trim the request so we never write past the end of the array.
	 * Writing past the end would corrupt other kernel memory! */
	space = MYDEV_BUFFER_SIZE - *ppos;
	if (count > space)
		count = space;

	/* Copy from user memory -> kernel memory (safe, checked copy). */
	if (copy_from_user(device_buffer + *ppos, ubuf, count)) {
		ret = -EFAULT;
		goto out;
	}

	*ppos += count;          /* advance the file position          */
	data_length = *ppos;     /* stored data now ends right here     */
	ret = count;             /* tell the caller how much we stored  */

	printk(KERN_INFO "mydev: wrote %zu bytes (buffer now holds %zu/%d)\n",
	       count, data_length, MYDEV_BUFFER_SIZE);

out:
	mutex_unlock(&mydev_lock);
	return ret;
}

/*
 * mydev_llseek - called for lseek(fd, offset, whence).
 * Lets a program move its file position, e.g. back to 0 before reading.
 *   SEEK_SET: position = offset
 *   SEEK_CUR: position = current position + offset
 *   SEEK_END: position = end of stored data + offset
 */
static loff_t mydev_llseek(struct file *filp, loff_t offset, int whence)
{
	loff_t new_pos;

	mutex_lock(&mydev_lock);   /* we read data_length, so lock */

	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = filp->f_pos + offset;
		break;
	case SEEK_END:
		new_pos = (loff_t)data_length + offset;
		break;
	default:                   /* unknown "whence" value */
		new_pos = -EINVAL;
		goto out;
	}

	/* Keep the position inside the buffer: 0 .. 1024. */
	if (new_pos < 0 || new_pos > MYDEV_BUFFER_SIZE) {
		new_pos = -EINVAL;
		goto out;
	}

	filp->f_pos = new_pos;     /* store the new position in the open file */

out:
	mutex_unlock(&mydev_lock);
	return new_pos;            /* new position, or negative error code */
}

/*
 * mydev_ioctl - called for ioctl(fd, cmd, arg).
 * ioctl is the "everything else" system call: operations that are not
 * simply reading or writing bytes.
 * @cmd: which command (one of the MYDEV_IOC_* numbers from our header).
 * @arg: an extra value; for the GET commands it is a USER pointer to an
 *       int where we should store the answer.
 */
static long mydev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int value;      /* answer to send back for the GET commands */
	long ret = 0;   /* 0 = success */

	/* Reject commands that do not carry OUR magic byte.  -ENOTTY is the
	 * traditional "this device does not support that ioctl" error. */
	if (_IOC_TYPE(cmd) != MYDEV_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case MYDEV_IOC_CLEAR:
		/* Erase the buffer: fill with zeros and mark it empty. */
		mutex_lock(&mydev_lock);
		memset(device_buffer, 0, MYDEV_BUFFER_SIZE);
		data_length = 0;
		mutex_unlock(&mydev_lock);
		printk(KERN_INFO "mydev: ioctl CLEAR - buffer erased\n");
		break;

	case MYDEV_IOC_GET_SIZE:
		/* Capacity is a constant, so no lock is needed. */
		value = MYDEV_BUFFER_SIZE;
		/* put_user copies ONE simple value to a user pointer safely.
		 * (int __user *)arg turns the plain number back into a
		 * pointer-to-int in user space. */
		if (put_user(value, (int __user *)arg))
			ret = -EFAULT;
		printk(KERN_INFO "mydev: ioctl GET_SIZE -> %d\n", value);
		break;

	case MYDEV_IOC_GET_LEN:
		mutex_lock(&mydev_lock);
		value = (int)data_length;   /* read under the lock */
		mutex_unlock(&mydev_lock);
		if (put_user(value, (int __user *)arg))
			ret = -EFAULT;
		printk(KERN_INFO "mydev: ioctl GET_LEN -> %d\n", value);
		break;

	default:
		ret = -ENOTTY;   /* our magic byte, but an unknown command */
		break;
	}

	return ret;
}

/*
 * The "jump table" that connects system calls to our functions.
 * Fields we do not set stay NULL, and the kernel uses a default or
 * returns an error for them.
 */
static const struct file_operations mydev_fops = {
	.owner          = THIS_MODULE,   /* prevents unloading while in use */
	.open           = mydev_open,
	.release        = mydev_release,
	.read           = mydev_read,
	.write          = mydev_write,
	.llseek         = mydev_llseek,
	.unlocked_ioctl = mydev_ioctl,   /* the modern ioctl entry point     */
	.compat_ioctl   = compat_ptr_ioctl, /* lets 32-bit apps call it too  */
};

/* ====================================================================== */
/* Module load / unload                                                   */
/* ====================================================================== */

/*
 * mydev_init - runs once, when the module is loaded (insmod).
 * "__init" lets the kernel free this function's memory after loading.
 *
 * Steps:
 *   1. Ask the kernel for a free major/minor number.
 *   2. Register our file_operations under that number (cdev).
 *   3. Create a device class in /sys/class/.
 *   4. Create the device -> udev sees it and makes /dev/mydev.
 * If any step fails we undo the earlier steps in reverse order.
 */
static int __init mydev_init(void)
{
	int ret;

	/* Step 1: dynamically allocate 1 device number, starting at minor 0. */
	ret = alloc_chrdev_region(&mydev_number, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		printk(KERN_ERR "mydev: failed to allocate device number (%d)\n", ret);
		return ret;
	}

	/* Step 2: link the cdev to our file_operations, then make it live. */
	cdev_init(&mydev_cdev, &mydev_fops);
	mydev_cdev.owner = THIS_MODULE;
	ret = cdev_add(&mydev_cdev, mydev_number, 1);
	if (ret < 0) {
		printk(KERN_ERR "mydev: cdev_add failed (%d)\n", ret);
		goto err_unregister_region;
	}

	/* Step 3: create the class.  Kernel 6.4 removed the first argument
	 * (THIS_MODULE), so we pick the right form for the kernel we are
	 * built against.  Ubuntu 24.04 ships kernel 6.8 -> one argument. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	mydev_class = class_create(CLASS_NAME);
#else
	mydev_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
	/* These functions return an "error pointer" on failure, not NULL.
	 * IS_ERR() detects that; PTR_ERR() extracts the error code. */
	if (IS_ERR(mydev_class)) {
		ret = PTR_ERR(mydev_class);
		printk(KERN_ERR "mydev: class_create failed (%d)\n", ret);
		goto err_cdev_del;
	}

	/* Step 4: create the device.  This sends a "uevent" to user space;
	 * udev reacts by creating the file /dev/mydev (and applying any
	 * permission rule we installed, see mydev.sh). */
	mydev_device = device_create(mydev_class, NULL, mydev_number, NULL,
				     DEVICE_NAME);
	if (IS_ERR(mydev_device)) {
		ret = PTR_ERR(mydev_device);
		printk(KERN_ERR "mydev: device_create failed (%d)\n", ret);
		goto err_class_destroy;
	}

	/* Start with an empty buffer. */
	data_length = 0;

	/* MAJOR()/MINOR() split a dev_t back into its two numbers. */
	printk(KERN_INFO "mydev: loaded - /dev/%s ready (major %d, minor %d, buffer %d bytes)\n",
	       DEVICE_NAME, MAJOR(mydev_number), MINOR(mydev_number),
	       MYDEV_BUFFER_SIZE);
	return 0;   /* success: module stays loaded */

	/* Error unwinding: each label undoes one step, falling through to
	 * undo all the steps that came before it. */
err_class_destroy:
	class_destroy(mydev_class);
err_cdev_del:
	cdev_del(&mydev_cdev);
err_unregister_region:
	unregister_chrdev_region(mydev_number, 1);
	return ret;  /* non-zero: insmod reports the error, module not loaded */
}

/*
 * mydev_exit - runs once, when the module is unloaded (rmmod).
 * Undo everything mydev_init() did, in REVERSE order.
 */
static void __exit mydev_exit(void)
{
	device_destroy(mydev_class, mydev_number);  /* removes /dev/mydev   */
	class_destroy(mydev_class);                 /* removes /sys/class/.. */
	cdev_del(&mydev_cdev);                      /* unhooks our fops      */
	unregister_chrdev_region(mydev_number, 1);  /* frees major/minor     */

	printk(KERN_INFO "mydev: unloaded - goodbye\n");
}

/* Tell the kernel which functions to run on load and unload. */
module_init(mydev_init);
module_exit(mydev_exit);

/* Module metadata - visible with `modinfo mydev.ko`.
 * The GPL licence is required to use many kernel helper functions. */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Virtual character device with a 1 KB buffer, mutex and ioctl");
MODULE_VERSION("1.0");
