/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 *
 * Copyright (C) 2008 MacZFS
 * Copyright (C) 2013 Jorgen Lundman <lundman@lundman.net>
 *
 */

#ifndef _SPL_TYPES_H
#define	_SPL_TYPES_H

#define likely(x)      __builtin_expect(!!(x), 1)
#define unlikely(x)    __builtin_expect(!!(x), 0)

#include_next <sys/types.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <libkern/libkern.h>


#include <sys/stropts.h>



#if 0
#ifndef HAVE_UINTPTR_T
typedef unsigned long			uintptr_t;
#endif
#endif

#ifndef ULLONG_MAX
#define ULLONG_MAX			(~0ULL)
#endif

#ifndef LLONG_MAX
#define LLONG_MAX			((long long)(~0ULL>>1))
#endif

#if 0
typedef unsigned long			intptr_t;
typedef unsigned long long		u_offset_t;
typedef struct task_struct		kthread_t;
typedef struct task_struct		proc_t;
typedef struct vmem { }			vmem_t;
typedef struct timespec			timestruc_t; /* definition per SVr4 */
typedef struct timespec			timespec_t;
typedef u_longlong_t			len_t;
typedef longlong_t			diskaddr_t;
typedef ushort_t			o_mode_t;
typedef uint_t				major_t;
typedef ulong_t				pfn_t;
typedef long				spgcnt_t;
typedef short				index_t;
typedef int				id_t;

extern proc_t p0;
typedef enum { B_FALSE=0, B_TRUE=1 }	boolean_t;
#endif

enum { B_FALSE=0, B_TRUE=1 };
typedef short				pri_t;
typedef unsigned long			ulong_t;
typedef unsigned long long		u_longlong_t;
typedef unsigned long long		rlim64_t;
typedef unsigned long long		loff_t;
typedef long long			longlong_t;
typedef unsigned char			uchar_t;
typedef unsigned int			uint_t;
typedef unsigned short			ushort_t;
typedef void *spinlock_t;
typedef long long			offset_t;
typedef struct timespec			timestruc_t; /* definition per SVr4 */
typedef struct timespec			timespec_t;
typedef ulong_t				pgcnt_t;
typedef unsigned int umode_t ;
#define   NODEV32 (dev32_t)(-1)
typedef   uint32_t        dev32_t;
typedef uint_t				minor_t;

#define EBADE EBADMACHO

#include  <sys/fcntl.h>
#define FCREAT          O_CREAT
#define FTRUNC          O_TRUNC
#define FEXCL           O_EXCL
#define FNOCTTY         O_NOCTTY
//#define       FASYNC          O_SYNC
#define FNOFOLLOW       O_NOFOLLOW

#define FSYNC           0x10    /* file (data+inode) integrity while writing */
#define FDSYNC          0x40    /* file data only integrity while writing */
#define FRSYNC          0x8000  /* sync read operations at same level of */
                                /* integrity as specified for writes by */
                                /* FSYNC and FDSYNC flags */
#define FOFFMAX         0x2000  /* large file */

#define EXPORT_SYMBOL(X)
#define module_param(X,Y,Z)
#define MODULE_PARM_DESC(X,Y)

#ifdef __GNUC__
#define member_type(type, member) __typeof__ (((type *)0)->member)
#else
#define member_type(type, member) void
#endif

#define container_of(ptr, type, member) ((type *)(                      \
      (char *)(member_type(type, member) *){ ptr } - offsetof(type, member)))

#endif	/* _SPL_TYPES_H */
