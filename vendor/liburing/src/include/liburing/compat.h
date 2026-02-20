/* SPDX-License-Identifier: MIT */
#ifndef LIBURING_COMPAT_H
#define LIBURING_COMPAT_H

/*
 * Compatibility header for liburing 2.8
 * Generated for static musl build
 */

/* Include linux headers for io_uring types */
#include <linux/blkdev.h>  /* BLOCK_URING_CMD_DISCARD (if available) */
#include <linux/openat2.h> /* struct open_how */

/* We're building on a modern kernel with full io_uring support */
#define CONFIG_HAVE_KERNEL_RWF_T 1
#define CONFIG_HAVE_KERNEL_TIMESPEC 1
#define CONFIG_HAVE_OPEN_HOW 1
#define CONFIG_HAVE_STATX 1
#define CONFIG_HAVE_GLIBC_STATX 0
#define CONFIG_HAVE_CXX 1
#define CONFIG_HAVE_UCONTEXT 1
#define CONFIG_HAVE_STRINGOP_OVERFLOW 0
#define CONFIG_HAVE_ARRAY_BOUNDS 0
#define CONFIG_HAVE_NVME_URING 1
#define CONFIG_HAVE_FANOTIFY 1
#define CONFIG_HAVE_FUTEXV 1

/* Fallback for BLOCK_URING_CMD_DISCARD if not defined in kernel headers */
#ifndef BLOCK_URING_CMD_DISCARD
#  define BLOCK_URING_CMD_DISCARD 0
#endif

#endif
