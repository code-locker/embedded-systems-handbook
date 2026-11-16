/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * memdev_ioctl.h — the ioctl ABI of /dev/memdevN, shared by the driver and
 *                  user space (included by memdev.c and the ../user programs)
 *
 * Concept : An ioctl number is 32 bits built by _IO/_IOR/_IOW/_IOWR:
 *
 *    31 30 29          16 15          8 7           0
 *   [ dir |    size      |    type     |    nr       ]
 *    dir : none / read (driver -> user) / write (user -> driver) / both
 *    size: sizeof(argument type), 14 bits
 *    type: the "magic" byte that identifies this driver's ioctl family
 *    nr  : command number within the family
 *
 *           Direction is named from USER space's point of view: _IOR means
 *           the user READS a value the driver writes (copy_to_user).
 *           The argument structs use only fixed-size __u64/__u32 fields,
 *           no pointers and no `long`, so the layout is identical for a
 *           32-bit program on the 64-bit kernel and .compat_ioctl can be
 *           compat_ptr_ioctl.
 * Read    : ../README.md  (section 3.6 "ioctl design")
 */
#ifndef MEMDEV_IOCTL_H
#define MEMDEV_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * 0xE1 is unassigned in Documentation/userspace-api/ioctl/ioctl-number.rst
 * (check there before picking a magic for a real driver and register it).
 */
#define MEMDEV_IOC_MAGIC 0xE1

struct memdev_stats {
	__u64 size;		/* capacity of the device buffer in bytes */
	__u64 len;		/* bytes of valid data (the "file size") */
	__u64 dev_reads;	/* read() calls on this device, all openers */
	__u64 dev_writes;	/* write() calls on this device, all openers */
	__u64 fd_read_bytes;	/* bytes read through THIS open file */
	__u64 fd_write_bytes;	/* bytes written through THIS open file */
	__u32 openers;		/* files currently open on this device */
	__u32 exclusive;	/* 1 if this minor allows only one opener */
};

/* Zero the buffer and set len = 0. No argument. */
#define MEMDEV_IOC_CLEAR	_IO(MEMDEV_IOC_MAGIC, 0)
/* Capacity in bytes -> *(__u64 *)arg. */
#define MEMDEV_IOC_GET_SIZE	_IOR(MEMDEV_IOC_MAGIC, 1, __u64)
/* Resize to *(__u64 *)arg bytes (1 .. max_size), keeps the data that fits. */
#define MEMDEV_IOC_SET_SIZE	_IOW(MEMDEV_IOC_MAGIC, 2, __u64)
/* Device + per-open counters -> *(struct memdev_stats *)arg. */
#define MEMDEV_IOC_GET_STATS	_IOR(MEMDEV_IOC_MAGIC, 3, struct memdev_stats)

#define MEMDEV_IOC_MAXNR 3

#endif /* MEMDEV_IOCTL_H */
