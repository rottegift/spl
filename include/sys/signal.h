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
 * Copyright 2010 Sun Microsystems, Inc. All rights reserved.
 * Use is subject to license terms.
 */

/* Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T */
/* All Rights Reserved */

/*
 * University Copyright- Copyright (c) 1982, 1986, 1988
 * The Regents of the University of California
 * All Rights Reserved
 *
 * University Acknowledgment- Portions of this document are derived from
 * software developed by the University of California, Berkeley, and its
 * contributors.
 */


#ifndef _SPL_SIGNAL_H
#define _SPL_SIGNAL_H

#include <osx/sched.h>
#include <sys/vm.h>
#include <sys/proc.h>
#include_next <sys/signal.h>

#define	FORREAL		0	/* Usual side-effects */
#define	JUSTLOOKING	1	/* Don't stop the process */

struct proc;

extern int
thread_issignal(struct proc *, thread_t, sigset_t);

/* The "why" argument indicates the allowable side-effects of the call:
 *
 * FORREAL:  Extract the next pending signal from p_sig into p_cursig;
 * stop the process if a stop has been requested or if a traced signal
 * is pending.
 *
 * JUSTLOOKING:  Don't stop the process, just indicate whether or not
 * a signal might be pending (FORREAL is needed to tell for sure).
 */
#define threadmask (sigmask(SIGILL)|sigmask(SIGTRAP)|\
                    sigmask(SIGIOT)|sigmask(SIGEMT)|\
                    sigmask(SIGFPE)|sigmask(SIGBUS)|\
                    sigmask(SIGSEGV)|sigmask(SIGSYS)|\
                    sigmask(SIGPIPE)|sigmask(SIGKILL)|\
                    sigmask(SIGTERM)|sigmask(SIGINT))

static __inline__ int
issig(int why)
{
	return (thread_issignal(current_proc(), current_thread(),
							threadmask));
}

#endif /* SPL_SIGNAL_H */
