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
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Copyright (c) 2012 by Delphix. All rights reserved.
 * Copyright (c) 2012, Joyent, Inc. All rights reserved.
 */

/*
 * Big Theory Statement for the virtual memory allocator.
 *
 * For a more complete description of the main ideas, see:
 *
 *	Jeff Bonwick and Jonathan Adams,
 *
 *	Magazines and vmem: Extending the Slab Allocator to Many CPUs and
 *	Arbitrary Resources.
 *
 *	Proceedings of the 2001 Usenix Conference.
 *	Available as http://www.usenix.org/event/usenix01/bonwick.html
 *
 *
 * 1. General Concepts
 * -------------------
 *
 * 1.1 Overview
 * ------------
 * We divide the kernel address space into a number of logically distinct
 * pieces, or *arenas*: text, data, heap, stack, and so on.  Within these
 * arenas we often subdivide further; for example, we use heap addresses
 * not only for the kernel heap (kmem_alloc() space), but also for DVMA,
 * bp_mapin(), /dev/kmem, and even some device mappings like the TOD chip.
 * The kernel address space, therefore, is most accurately described as
 * a tree of arenas in which each node of the tree *imports* some subset
 * of its parent.  The virtual memory allocator manages these arenas and
 * supports their natural hierarchical structure.
 *
 * 1.2 Arenas
 * ----------
 * An arena is nothing more than a set of integers.  These integers most
 * commonly represent virtual addresses, but in fact they can represent
 * anything at all.  For example, we could use an arena containing the
 * integers minpid through maxpid to allocate process IDs.  vmem_create()
 * and vmem_destroy() create and destroy vmem arenas.  In order to
 * differentiate between arenas used for adresses and arenas used for
 * identifiers, the VMC_IDENTIFIER flag is passed to vmem_create().  This
 * prevents identifier exhaustion from being diagnosed as general memory
 * failure.
 *
 * 1.3 Spans
 * ---------
 * We represent the integers in an arena as a collection of *spans*, or
 * contiguous ranges of integers.  For example, the kernel heap consists
 * of just one span: [kernelheap, ekernelheap).  Spans can be added to an
 * arena in two ways: explicitly, by vmem_add(), or implicitly, by
 * importing, as described in Section 1.5 below.
 *
 * 1.4 Segments
 * ------------
 * Spans are subdivided into *segments*, each of which is either allocated
 * or free.  A segment, like a span, is a contiguous range of integers.
 * Each allocated segment [addr, addr + size) represents exactly one
 * vmem_alloc(size) that returned addr.  Free segments represent the space
 * between allocated segments.  If two free segments are adjacent, we
 * coalesce them into one larger segment; that is, if segments [a, b) and
 * [b, c) are both free, we merge them into a single segment [a, c).
 * The segments within a span are linked together in increasing-address order
 * so we can easily determine whether coalescing is possible.
 *
 * Segments never cross span boundaries.  When all segments within
 * an imported span become free, we return the span to its source.
 *
 * 1.5 Imported Memory
 * -------------------
 * As mentioned in the overview, some arenas are logical subsets of
 * other arenas.  For example, kmem_va_arena (a virtual address cache
 * that satisfies most kmem_slab_create() requests) is just a subset
 * of heap_arena (the kernel heap) that provides caching for the most
 * common slab sizes.  When kmem_va_arena runs out of virtual memory,
 * it *imports* more from the heap; we say that heap_arena is the
 * *vmem source* for kmem_va_arena.  vmem_create() allows you to
 * specify any existing vmem arena as the source for your new arena.
 * Topologically, since every arena is a child of at most one source,
 * the set of all arenas forms a collection of trees.
 *
 * 1.6 Constrained Allocations
 * ---------------------------
 * Some vmem clients are quite picky about the kind of address they want.
 * For example, the DVMA code may need an address that is at a particular
 * phase with respect to some alignment (to get good cache coloring), or
 * that lies within certain limits (the addressable range of a device),
 * or that doesn't cross some boundary (a DMA counter restriction) --
 * or all of the above.  vmem_xalloc() allows the client to specify any
 * or all of these constraints.
 *
 * 1.7 The Vmem Quantum
 * --------------------
 * Every arena has a notion of 'quantum', specified at vmem_create() time,
 * that defines the arena's minimum unit of currency.  Most commonly the
 * quantum is either 1 or PAGESIZE, but any power of 2 is legal.
 * All vmem allocations are guaranteed to be quantum-aligned.
 *
 * 1.8 Quantum Caching
 * -------------------
 * A vmem arena may be so hot (frequently used) that the scalability of vmem
 * allocation is a significant concern.  We address this by allowing the most
 * common allocation sizes to be serviced by the kernel memory allocator,
 * which provides low-latency per-cpu caching.  The qcache_max argument to
 * vmem_create() specifies the largest allocation size to cache.
 *
 * 1.9 Relationship to Kernel Memory Allocator
 * -------------------------------------------
 * Every kmem cache has a vmem arena as its slab supplier.  The kernel memory
 * allocator uses vmem_alloc() and vmem_free() to create and destroy slabs.
 *
 *
 * 2. Implementation
 * -----------------
 *
 * 2.1 Segment lists and markers
 * -----------------------------
 * The segment structure (vmem_seg_t) contains two doubly-linked lists.
 *
 * The arena list (vs_anext/vs_aprev) links all segments in the arena.
 * In addition to the allocated and free segments, the arena contains
 * special marker segments at span boundaries.  Span markers simplify
 * coalescing and importing logic by making it easy to tell both when
 * we're at a span boundary (so we don't coalesce across it), and when
 * a span is completely free (its neighbors will both be span markers).
 *
 * Imported spans will have vs_import set.
 *
 * The next-of-kin list (vs_knext/vs_kprev) links segments of the same type:
 * (1) for allocated segments, vs_knext is the hash chain linkage;
 * (2) for free segments, vs_knext is the freelist linkage;
 * (3) for span marker segments, vs_knext is the next span marker.
 *
 * 2.2 Allocation hashing
 * ----------------------
 * We maintain a hash table of all allocated segments, hashed by address.
 * This allows vmem_free() to discover the target segment in constant time.
 * vmem_update() periodically resizes hash tables to keep hash chains short.
 *
 * 2.3 Freelist management
 * -----------------------
 * We maintain power-of-2 freelists for free segments, i.e. free segments
 * of size >= 2^n reside in vmp->vm_freelist[n].  To ensure constant-time
 * allocation, vmem_xalloc() looks not in the first freelist that *might*
 * satisfy the allocation, but in the first freelist that *definitely*
 * satisfies the allocation (unless VM_BESTFIT is specified, or all larger
 * freelists are empty).  For example, a 1000-byte allocation will be
 * satisfied not from the 512..1023-byte freelist, whose members *might*
 * contains a 1000-byte segment, but from a 1024-byte or larger freelist,
 * the first member of which will *definitely* satisfy the allocation.
 * This ensures that vmem_xalloc() works in constant time.
 *
 * We maintain a bit map to determine quickly which freelists are non-empty.
 * vmp->vm_freemap & (1 << n) is non-zero iff vmp->vm_freelist[n] is non-empty.
 *
 * The different freelists are linked together into one large freelist,
 * with the freelist heads serving as markers.  Freelist markers simplify
 * the maintenance of vm_freemap by making it easy to tell when we're taking
 * the last member of a freelist (both of its neighbors will be markers).
 *
 * 2.4 Vmem Locking
 * ----------------
 * For simplicity, all arena state is protected by a per-arena lock.
 * For very hot arenas, use quantum caching for scalability.
 *
 * 2.5 Vmem Population
 * -------------------
 * Any internal vmem routine that might need to allocate new segment
 * structures must prepare in advance by calling vmem_populate(), which
 * will preallocate enough vmem_seg_t's to get is through the entire
 * operation without dropping the arena lock.
 *
 * 2.6 Auditing
 * ------------
 * If KMF_AUDIT is set in kmem_flags, we audit vmem allocations as well.
 * Since virtual addresses cannot be scribbled on, there is no equivalent
 * in vmem to redzone checking, deadbeef, or other kmem debugging features.
 * Moreover, we do not audit frees because segment coalescing destroys the
 * association between an address and its segment structure.  Auditing is
 * thus intended primarily to keep track of who's consuming the arena.
 * Debugging support could certainly be extended in the future if it proves
 * necessary, but we do so much live checking via the allocation hash table
 * that even non-DEBUG systems get quite a bit of sanity checking already.
 */

#include <sys/vmem_impl.h>
#include <sys/kmem.h>
#include <sys/kstat.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/atomic.h>
#include <sys/bitmap.h>
#include <sys/sysmacros.h>
#include <sys/cmn_err.h>
#include <sys/debug.h>
#include <sys/types.h>
//#include <sys/panic.h>
#include <stdbool.h>

#define	VMEM_INITIAL		21	/* early vmem arenas */
#define	VMEM_SEG_INITIAL	800
//200 //400	/* early segments */

/*
 * Adding a new span to an arena requires two segment structures: one to
 * represent the span, and one to represent the free segment it contains.
 */
#define	VMEM_SEGS_PER_SPAN_CREATE	2

/*
 * Allocating a piece of an existing segment requires 0-2 segment structures
 * depending on how much of the segment we're allocating.
 *
 * To allocate the entire segment, no new segment structures are needed; we
 * simply move the existing segment structure from the freelist to the
 * allocation hash table.
 *
 * To allocate a piece from the left or right end of the segment, we must
 * split the segment into two pieces (allocated part and remainder), so we
 * need one new segment structure to represent the remainder.
 *
 * To allocate from the middle of a segment, we need two new segment strucures
 * to represent the remainders on either side of the allocated part.
 */
#define	VMEM_SEGS_PER_EXACT_ALLOC	0
#define	VMEM_SEGS_PER_LEFT_ALLOC	1
#define	VMEM_SEGS_PER_RIGHT_ALLOC	1
#define	VMEM_SEGS_PER_MIDDLE_ALLOC	2

/*
 * vmem_populate() preallocates segment structures for vmem to do its work.
 * It must preallocate enough for the worst case, which is when we must import
 * a new span and then allocate from the middle of it.
 */
#define	VMEM_SEGS_PER_ALLOC_MAX		\
(VMEM_SEGS_PER_SPAN_CREATE + VMEM_SEGS_PER_MIDDLE_ALLOC)

/*
 * The segment structures themselves are allocated from vmem_seg_arena, so
 * we have a recursion problem when vmem_seg_arena needs to populate itself.
 * We address this by working out the maximum number of segment structures
 * this act will require, and multiplying by the maximum number of threads
 * that we'll allow to do it simultaneously.
 *
 * The worst-case segment consumption to populate vmem_seg_arena is as
 * follows (depicted as a stack trace to indicate why events are occurring):
 *
 * (In order to lower the fragmentation in the heap_arena, we specify a
 * minimum import size for the vmem_metadata_arena which is the same size
 * as the kmem_va quantum cache allocations.  This causes the worst-case
 * allocation from the vmem_metadata_arena to be 3 segments.)
 *
 * vmem_alloc(vmem_seg_arena)		-> 2 segs (span create + exact alloc)
 *  segkmem_alloc(vmem_metadata_arena)
 *   vmem_alloc(vmem_metadata_arena)	-> 3 segs (span create + left alloc)
 *    vmem_alloc(heap_arena)		-> 1 seg (left alloc)
 *   page_create()
 *   hat_memload()
 *    kmem_cache_alloc()
 *     kmem_slab_create()
 *	vmem_alloc(hat_memload_arena)	-> 2 segs (span create + exact alloc)
 *	 segkmem_alloc(heap_arena)
 *	  vmem_alloc(heap_arena)	-> 1 seg (left alloc)
 *	  page_create()
 *	  hat_memload()		-> (hat layer won't recurse further)
 *
 * The worst-case consumption for each arena is 3 segment structures.
 * Of course, a 3-seg reserve could easily be blown by multiple threads.
 * Therefore, we serialize all allocations from vmem_seg_arena (which is OK
 * because they're rare).  We cannot allow a non-blocking allocation to get
 * tied up behind a blocking allocation, however, so we use separate locks
 * for VM_SLEEP and VM_NOSLEEP allocations.  Similarly, VM_PUSHPAGE allocations
 * must not block behind ordinary VM_SLEEPs.  In addition, if the system is
 * panicking then we must keep enough resources for panic_thread to do its
 * work.  Thus we have at most four threads trying to allocate from
 * vmem_seg_arena, and each thread consumes at most three segment structures,
 * so we must maintain a 12-seg reserve.
 */
#define	VMEM_POPULATE_RESERVE	12

/*
 * vmem_populate() ensures that each arena has VMEM_MINFREE seg structures
 * so that it can satisfy the worst-case allocation *and* participate in
 * worst-case allocation from vmem_seg_arena.
 */
#define	VMEM_MINFREE	(VMEM_POPULATE_RESERVE + VMEM_SEGS_PER_ALLOC_MAX)

static vmem_t vmem0[VMEM_INITIAL];
static vmem_t *vmem_populator[VMEM_INITIAL];
static uint32_t vmem_id;
static uint32_t vmem_populators;
static vmem_seg_t vmem_seg0[VMEM_SEG_INITIAL];
static vmem_seg_t *vmem_segfree;
static kmutex_t vmem_list_lock;
static kmutex_t vmem_segfree_lock;
static kmutex_t vmem_sleep_lock;
static kmutex_t vmem_nosleep_lock;
static kmutex_t vmem_pushpage_lock;
static kmutex_t vmem_panic_lock;
static kmutex_t vmem_xnu_alloc_free_lock;
static kcondvar_t vmem_xnu_alloc_free_cv;
static vmem_t *vmem_list;
static vmem_t *vmem_metadata_arena;
static vmem_t *vmem_seg_arena;
static vmem_t *vmem_hash_arena;
static vmem_t *vmem_vmem_arena;
vmem_t *spl_default_arena; // The bottom-most arena for SPL
static vmem_t *spl_default_arena_parent; // dummy arena as a placeholder
#define VMEM_BUCKETS 13
#define VMEM_BUCKET_LOWBIT 12
#define VMEM_BUCKET_HIBIT 24
static vmem_t *vmem_bucket_arena[VMEM_BUCKETS];
vmem_t *spl_heap_arena;
static void *spl_heap_arena_initial_alloc;
static size_t spl_heap_arena_initial_alloc_size = 0;
#define NUMBER_OF_ARENAS_IN_VMEM_INIT 21
static struct timespec	vmem_update_interval	= {15, 0};	/* vmem_update() every 15 seconds */
uint32_t vmem_mtbf;		/* mean time between failures [default: off] */
size_t vmem_seg_size = sizeof (vmem_seg_t);

// must match with include/sys/vmem_impl.h
static vmem_kstat_t vmem_kstat_template = {
	{ "mem_inuse",		KSTAT_DATA_UINT64 },
	{ "mem_import",		KSTAT_DATA_UINT64 },
	{ "mem_total",		KSTAT_DATA_UINT64 },
	{ "vmem_source",	KSTAT_DATA_UINT32 },
	{ "alloc",		KSTAT_DATA_UINT64 },
	{ "free",		KSTAT_DATA_UINT64 },
	{ "wait",		KSTAT_DATA_UINT64 },
	{ "fail",		KSTAT_DATA_UINT64 },
	{ "lookup",		KSTAT_DATA_UINT64 },
	{ "search",		KSTAT_DATA_UINT64 },
	{ "populate_fail",	KSTAT_DATA_UINT64 },
	{ "contains",		KSTAT_DATA_UINT64 },
	{ "contains_search",	KSTAT_DATA_UINT64 },
	{ "parent_alloc",	KSTAT_DATA_UINT64 },
	{ "parent_free",	KSTAT_DATA_UINT64 },
	{ "threads_waiting",	KSTAT_DATA_UINT64 },
	{ "excess",	KSTAT_DATA_UINT64 },
};


/*
 * Insert/delete from arena list (type 'a') or next-of-kin list (type 'k').
 */
#define	VMEM_INSERT(vprev, vsp, type)					\
{									\
vmem_seg_t *_vnext = (vprev)->vs_##type##next;			\
(vsp)->vs_##type##next = (_vnext);				\
(vsp)->vs_##type##prev = (vprev);				\
(vprev)->vs_##type##next = (vsp);				\
(_vnext)->vs_##type##prev = (vsp);				\
}

#define	VMEM_DELETE(vsp, type)						\
{									\
vmem_seg_t *_vprev = (vsp)->vs_##type##prev;			\
vmem_seg_t *_vnext = (vsp)->vs_##type##next;			\
(_vprev)->vs_##type##next = (_vnext);				\
(_vnext)->vs_##type##prev = (_vprev);				\
}

/// vmem thread block count
uint64_t spl_vmem_threads_waiting = 0;

// number of allocations > minalloc
uint64_t spl_bucket_non_pow2_allocs = 0;

// allocator kstats
uint64_t spl_vmem_unconditional_allocs = 0;
uint64_t spl_vmem_unconditional_alloc_bytes = 0;
uint64_t spl_vmem_conditional_allocs = 0;
uint64_t spl_vmem_conditional_alloc_bytes = 0;
uint64_t spl_vmem_conditional_alloc_deny = 0;
uint64_t spl_vmem_conditional_alloc_deny_bytes = 0;

// bucket allocator kstat
uint64_t spl_xat_success = 0;
uint64_t spl_xat_late_success = 0;
uint64_t spl_xat_late_success_nosleep = 0;
uint64_t spl_xat_pressured = 0;
uint64_t spl_xat_bailed = 0;
uint64_t spl_xat_lastalloc = 0;
uint64_t spl_xat_lastfree = 0;
uint64_t spl_xat_forced = 0;
uint64_t spl_xat_memory_appeared = 0;
uint64_t spl_xat_sleep = 0;
uint64_t spl_xat_late_deny = 0;
uint64_t spl_xat_no_waiters = 0;

uint64_t spl_vba_memory_appeared = 0;
uint64_t spl_vba_parent_memory_appeared = 0;
uint64_t spl_vba_parent_memory_blocked = 0;
uint64_t spl_vba_cv_timeout = 0;
uint64_t spl_vba_loop_timeout = 0;
uint64_t spl_vba_cv_timeout_blocked = 0;
uint64_t spl_vba_loop_timeout_blocked = 0;
uint64_t spl_vba_sleep = 0;
uint64_t spl_vba_loop_entries = 0;

// bucket minimum span size tunables
uint64_t spl_bucket_tunable_large_span = 0;
uint64_t spl_bucket_tunable_small_span = 0;

extern void spl_free_set_emergency_pressure(int64_t p);
extern uint64_t segkmem_total_mem_allocated;
extern uint64_t total_memory;

/*
 * Get a vmem_seg_t from the global segfree list.
 */
static vmem_seg_t *
vmem_getseg_global(void)
{
	vmem_seg_t *vsp;

	mutex_enter(&vmem_segfree_lock);
	if ((vsp = vmem_segfree) != NULL)
		vmem_segfree = vsp->vs_knext;
	mutex_exit(&vmem_segfree_lock);

	return (vsp);
}

/*
 * Put a vmem_seg_t on the global segfree list.
 */
static void
vmem_putseg_global(vmem_seg_t *vsp)
{
	mutex_enter(&vmem_segfree_lock);
	vsp->vs_knext = vmem_segfree;
	vmem_segfree = vsp;
	mutex_exit(&vmem_segfree_lock);
}

/*
 * Get a vmem_seg_t from vmp's segfree list.
 */
static vmem_seg_t *
vmem_getseg(vmem_t *vmp)
{
	vmem_seg_t *vsp;

	ASSERT(vmp->vm_nsegfree > 0);

	vsp = vmp->vm_segfree;
	vmp->vm_segfree = vsp->vs_knext;
	vmp->vm_nsegfree--;

	return (vsp);
}

/*
 * Put a vmem_seg_t on vmp's segfree list.
 */
static void
vmem_putseg(vmem_t *vmp, vmem_seg_t *vsp)
{
	vsp->vs_knext = vmp->vm_segfree;
	vmp->vm_segfree = vsp;
	vmp->vm_nsegfree++;
}

/*
 * Add vsp to the appropriate freelist.
 */
static void
vmem_freelist_insert(vmem_t *vmp, vmem_seg_t *vsp)
{
	vmem_seg_t *vprev;

	ASSERT(*VMEM_HASH(vmp, vsp->vs_start) != vsp);

	vprev = (vmem_seg_t *)&vmp->vm_freelist[highbit(VS_SIZE(vsp)) - 1];
	vsp->vs_type = VMEM_FREE;
	vmp->vm_freemap |= VS_SIZE(vprev);
	VMEM_INSERT(vprev, vsp, k);

	cv_broadcast(&vmp->vm_cv);
}

/*
 * Take vsp from the freelist.
 */
static void
vmem_freelist_delete(vmem_t *vmp, vmem_seg_t *vsp)
{
	ASSERT(*VMEM_HASH(vmp, vsp->vs_start) != vsp);
	ASSERT(vsp->vs_type == VMEM_FREE);

	if (vsp->vs_knext->vs_start == 0 && vsp->vs_kprev->vs_start == 0) {
		/*
		 * The segments on both sides of 'vsp' are freelist heads,
		 * so taking vsp leaves the freelist at vsp->vs_kprev empty.
		 */
		ASSERT(vmp->vm_freemap & VS_SIZE(vsp->vs_kprev));
		vmp->vm_freemap ^= VS_SIZE(vsp->vs_kprev);
	}
	VMEM_DELETE(vsp, k);
}

/*
 * Add vsp to the allocated-segment hash table and update kstats.
 */
static void
vmem_hash_insert(vmem_t *vmp, vmem_seg_t *vsp)
{
	vmem_seg_t **bucket;

	vsp->vs_type = VMEM_ALLOC;
	bucket = VMEM_HASH(vmp, vsp->vs_start);
	vsp->vs_knext = *bucket;
	*bucket = vsp;

	if (vmem_seg_size == sizeof (vmem_seg_t)) {
		//		vsp->vs_depth = (uint8_t)getpcstack(vsp->vs_stack,
		//											VMEM_STACK_DEPTH);
		//		vsp->vs_thread = curthread;

		vsp->vs_depth = 0;
		vsp->vs_thread = 0;
		vsp->vs_timestamp = gethrtime();
	} else {
		vsp->vs_depth = 0;
	}

	vmp->vm_kstat.vk_alloc.value.ui64++;
	vmp->vm_kstat.vk_mem_inuse.value.ui64 += VS_SIZE(vsp);
}

/*
 * Remove vsp from the allocated-segment hash table and update kstats.
 */
static vmem_seg_t *
vmem_hash_delete(vmem_t *vmp, uintptr_t addr, size_t size)
{
	vmem_seg_t *vsp, **prev_vspp;

	prev_vspp = VMEM_HASH(vmp, addr);
	while ((vsp = *prev_vspp) != NULL) {
		if (vsp->vs_start == addr) {
			*prev_vspp = vsp->vs_knext;
			break;
		}
		vmp->vm_kstat.vk_lookup.value.ui64++;
		prev_vspp = &vsp->vs_knext;
	}

	if (vsp == NULL)
		panic("vmem_hash_delete(%p, %lx, %lu): bad free (name: %s, addr, size)",
		    (void *)vmp, addr, size, vmp->vm_name);
	if (VS_SIZE(vsp) != size)
		panic("vmem_hash_delete(%p, %lx, %lu): wrong size (expect %lu)",
			  (void *)vmp, addr, size, VS_SIZE(vsp));

	vmp->vm_kstat.vk_free.value.ui64++;
	vmp->vm_kstat.vk_mem_inuse.value.ui64 -= size;

	return (vsp);
}

/*
 * Create a segment spanning the range [start, end) and add it to the arena.
 */
static vmem_seg_t *
vmem_seg_create(vmem_t *vmp, vmem_seg_t *vprev, uintptr_t start, uintptr_t end)
{
	vmem_seg_t *newseg = vmem_getseg(vmp);

	newseg->vs_start = start;
	newseg->vs_end = end;
	newseg->vs_type = 0;
	newseg->vs_import = 0;

	VMEM_INSERT(vprev, newseg, a);

	return (newseg);
}

/*
 * Remove segment vsp from the arena.
 */
static void
vmem_seg_destroy(vmem_t *vmp, vmem_seg_t *vsp)
{
	ASSERT(vsp->vs_type != VMEM_ROTOR);
	VMEM_DELETE(vsp, a);

	vmem_putseg(vmp, vsp);
}

/*
 * Add the span [vaddr, vaddr + size) to vmp and update kstats.
 */
static vmem_seg_t *
vmem_span_create(vmem_t *vmp, void *vaddr, size_t size, uint8_t import)
{
	vmem_seg_t *newseg, *span;
	uintptr_t start = (uintptr_t)vaddr;
	uintptr_t end = start + size;

	ASSERT(MUTEX_HELD(&vmp->vm_lock));

	if ((start | end) & (vmp->vm_quantum - 1))
		panic("vmem_span_create(%p, %p, %lu): misaligned (%s)",
		    (void *)vmp, vaddr, size, vmp->vm_name);

	span = vmem_seg_create(vmp, vmp->vm_seg0.vs_aprev, start, end);
	span->vs_type = VMEM_SPAN;
	span->vs_import = import;
	VMEM_INSERT(vmp->vm_seg0.vs_kprev, span, k);

	newseg = vmem_seg_create(vmp, span, start, end);
	vmem_freelist_insert(vmp, newseg);

	if (import)
		vmp->vm_kstat.vk_mem_import.value.ui64 += size;
	vmp->vm_kstat.vk_mem_total.value.ui64 += size;

	return (newseg);
}

/*
 * Remove span vsp from vmp and update kstats.
 */
static void
vmem_span_destroy(vmem_t *vmp, vmem_seg_t *vsp)
{
	vmem_seg_t *span = vsp->vs_aprev;
	size_t size = VS_SIZE(vsp);

	ASSERT(MUTEX_HELD(&vmp->vm_lock));
	ASSERT(span->vs_type == VMEM_SPAN);

	if (span->vs_import)
		vmp->vm_kstat.vk_mem_import.value.ui64 -= size;
	vmp->vm_kstat.vk_mem_total.value.ui64 -= size;

	VMEM_DELETE(span, k);

	vmem_seg_destroy(vmp, vsp);
	vmem_seg_destroy(vmp, span);
}

/*
 * Allocate the subrange [addr, addr + size) from segment vsp.
 * If there are leftovers on either side, place them on the freelist.
 * Returns a pointer to the segment representing [addr, addr + size).
 */
static vmem_seg_t *
vmem_seg_alloc(vmem_t *vmp, vmem_seg_t *vsp, uintptr_t addr, size_t size)
{
	uintptr_t vs_start = vsp->vs_start;
	uintptr_t vs_end = vsp->vs_end;
	size_t vs_size = vs_end - vs_start;
	size_t realsize = P2ROUNDUP(size, vmp->vm_quantum);
	uintptr_t addr_end = addr + realsize;

	ASSERT(P2PHASE(vs_start, vmp->vm_quantum) == 0);
	ASSERT(P2PHASE(addr, vmp->vm_quantum) == 0);
	ASSERT(vsp->vs_type == VMEM_FREE);
	ASSERT(addr >= vs_start && addr_end - 1 <= vs_end - 1);
	ASSERT(addr - 1 <= addr_end - 1);

	/*
	 * If we're allocating from the start of the segment, and the
	 * remainder will be on the same freelist, we can save quite
	 * a bit of work.
	 */
	if (P2SAMEHIGHBIT(vs_size, vs_size - realsize) && addr == vs_start) {
		ASSERT(highbit(vs_size) == highbit(vs_size - realsize));
		vsp->vs_start = addr_end;
		vsp = vmem_seg_create(vmp, vsp->vs_aprev, addr, addr + size);
		vmem_hash_insert(vmp, vsp);
		return (vsp);
	}

	vmem_freelist_delete(vmp, vsp);

	if (vs_end != addr_end)
		vmem_freelist_insert(vmp,
							 vmem_seg_create(vmp, vsp, addr_end, vs_end));

	if (vs_start != addr)
		vmem_freelist_insert(vmp,
							 vmem_seg_create(vmp, vsp->vs_aprev, vs_start, addr));

	vsp->vs_start = addr;
	vsp->vs_end = addr + size;

	vmem_hash_insert(vmp, vsp);
	return (vsp);
}

/*
 * Returns 1 if we are populating, 0 otherwise.
 * Call it if we want to prevent recursion from HAT.
 */
int
vmem_is_populator()
{
	return (mutex_owner(&vmem_sleep_lock) == curthread ||
			mutex_owner(&vmem_nosleep_lock) == curthread ||
			mutex_owner(&vmem_pushpage_lock) == curthread ||
			mutex_owner(&vmem_panic_lock) == curthread);
}

/*
 * Populate vmp's segfree list with VMEM_MINFREE vmem_seg_t structures.
 */
static int
vmem_populate(vmem_t *vmp, int vmflag)
{
	char *p;
	vmem_seg_t *vsp;
	ssize_t nseg;
	size_t size;
	kmutex_t *lp;
	int i;

	while (vmp->vm_nsegfree < VMEM_MINFREE &&
	    (vsp = vmem_getseg_global()) != NULL)
		vmem_putseg(vmp, vsp);

	if (vmp->vm_nsegfree >= VMEM_MINFREE)
		return (1);

	/*
	 * If we're already populating, tap the reserve.
	 */
	if (vmem_is_populator()) {
		ASSERT(vmp->vm_cflags & VMC_POPULATOR);
		return (1);
	}

	mutex_exit(&vmp->vm_lock);

	//	if (panic_thread == curthread)
	//		lp = &vmem_panic_lock;
	//	else

	if (vmflag & VM_NOSLEEP)
		lp = &vmem_nosleep_lock;
	else if (vmflag & VM_PUSHPAGE)
		lp = &vmem_pushpage_lock;
	else
		lp = &vmem_sleep_lock;

	mutex_enter(lp);

	nseg = VMEM_MINFREE + vmem_populators * VMEM_POPULATE_RESERVE;
	size = P2ROUNDUP(nseg * vmem_seg_size, vmem_seg_arena->vm_quantum);
	nseg = size / vmem_seg_size;

	/*
	 * The following vmem_alloc() may need to populate vmem_seg_arena
	 * and all the things it imports from.  When doing so, it will tap
	 * each arena's reserve to prevent recursion (see the block comment
	 * above the definition of VMEM_POPULATE_RESERVE).
	 */
	p = vmem_alloc(vmem_seg_arena, size, vmflag & VM_KMFLAGS);
	if (p == NULL) {
		mutex_exit(lp);
		mutex_enter(&vmp->vm_lock);
		vmp->vm_kstat.vk_populate_fail.value.ui64++;
		return (0);
	}

	/*
	 * Restock the arenas that may have been depleted during population.
	 */
	for (i = 0; i < vmem_populators; i++) {
		mutex_enter(&vmem_populator[i]->vm_lock);
		while (vmem_populator[i]->vm_nsegfree < VMEM_POPULATE_RESERVE)
			vmem_putseg(vmem_populator[i],
						(vmem_seg_t *)(p + --nseg * vmem_seg_size));
		mutex_exit(&vmem_populator[i]->vm_lock);
	}

	mutex_exit(lp);
	mutex_enter(&vmp->vm_lock);

	/*
	 * Now take our own segments.
	 */
	ASSERT(nseg >= VMEM_MINFREE);
	while (vmp->vm_nsegfree < VMEM_MINFREE)
		vmem_putseg(vmp, (vmem_seg_t *)(p + --nseg * vmem_seg_size));

	/*
	 * Give the remainder to charity.
	 */
	while (nseg > 0)
		vmem_putseg_global((vmem_seg_t *)(p + --nseg * vmem_seg_size));

	return (1);
}

/*
 * Advance a walker from its previous position to 'afterme'.
 * Note: may drop and reacquire vmp->vm_lock.
 */
static void
vmem_advance(vmem_t *vmp, vmem_seg_t *walker, vmem_seg_t *afterme)
{
	vmem_seg_t *vprev = walker->vs_aprev;
	vmem_seg_t *vnext = walker->vs_anext;
	vmem_seg_t *vsp = NULL;

	VMEM_DELETE(walker, a);

	if (afterme != NULL)
		VMEM_INSERT(afterme, walker, a);

	/*
	 * The walker segment's presence may have prevented its neighbors
	 * from coalescing.  If so, coalesce them now.
	 */
	if (vprev->vs_type == VMEM_FREE) {
		if (vnext->vs_type == VMEM_FREE) {
			ASSERT(vprev->vs_end == vnext->vs_start);
			vmem_freelist_delete(vmp, vnext);
			vmem_freelist_delete(vmp, vprev);
			vprev->vs_end = vnext->vs_end;
			vmem_freelist_insert(vmp, vprev);
			vmem_seg_destroy(vmp, vnext);
		}
		vsp = vprev;
	} else if (vnext->vs_type == VMEM_FREE) {
		vsp = vnext;
	}

	/*
	 * vsp could represent a complete imported span,
	 * in which case we must return it to the source.
	 */
	if (vsp != NULL && vsp->vs_aprev->vs_import &&
		vmp->vm_source_free != NULL &&
		vsp->vs_aprev->vs_type == VMEM_SPAN &&
		vsp->vs_anext->vs_type == VMEM_SPAN) {
		void *vaddr = (void *)vsp->vs_start;
		size_t size = VS_SIZE(vsp);
		ASSERT(size == VS_SIZE(vsp->vs_aprev));
		vmem_freelist_delete(vmp, vsp);
		vmem_span_destroy(vmp, vsp);
		vmp->vm_kstat.vk_parent_free.value.ui64++;
		mutex_exit(&vmp->vm_lock);
		vmp->vm_source_free(vmp->vm_source, vaddr, size);
		mutex_enter(&vmp->vm_lock);
	}
}

/*
 * VM_NEXTFIT allocations deliberately cycle through all virtual addresses
 * in an arena, so that we avoid reusing addresses for as long as possible.
 * This helps to catch used-after-freed bugs.  It's also the perfect policy
 * for allocating things like process IDs, where we want to cycle through
 * all values in order.
 */
#define dprintf if (0) printf
static void *
vmem_nextfit_alloc(vmem_t *vmp, size_t size, int vmflag)
{
	vmem_seg_t *vsp, *rotor;
	uintptr_t addr;
	size_t realsize = P2ROUNDUP(size, vmp->vm_quantum);
	size_t vs_size;

	mutex_enter(&vmp->vm_lock);

	if (vmp->vm_nsegfree < VMEM_MINFREE && !vmem_populate(vmp, vmflag)) {
		mutex_exit(&vmp->vm_lock);
		return (NULL);
	}

	/*
	 * The common case is that the segment right after the rotor is free,
	 * and large enough that extracting 'size' bytes won't change which
	 * freelist it's on.  In this case we can avoid a *lot* of work.
	 * Instead of the normal vmem_seg_alloc(), we just advance the start
	 * address of the victim segment.  Instead of moving the rotor, we
	 * create the new segment structure *behind the rotor*, which has
	 * the same effect.  And finally, we know we don't have to coalesce
	 * the rotor's neighbors because the new segment lies between them.
	 */
	rotor = &vmp->vm_rotor;
	vsp = rotor->vs_anext;
	if (vsp->vs_type == VMEM_FREE && (vs_size = VS_SIZE(vsp)) > realsize &&
		P2SAMEHIGHBIT(vs_size, vs_size - realsize)) {
		ASSERT(highbit(vs_size) == highbit(vs_size - realsize));
		addr = vsp->vs_start;
		vsp->vs_start = addr + realsize;
		vmem_hash_insert(vmp,
						 vmem_seg_create(vmp, rotor->vs_aprev, addr, addr + size));
		mutex_exit(&vmp->vm_lock);
		return ((void *)addr);
	}

	/*
	 * Starting at the rotor, look for a segment large enough to
	 * satisfy the allocation.
	 */
	for (;;) {
		atomic_inc_64(&vmp->vm_kstat.vk_search.value.ui64);
		if (vsp->vs_type == VMEM_FREE && VS_SIZE(vsp) >= size)
			break;
		vsp = vsp->vs_anext;
		if (vsp == rotor) {
			/*
			 * We've come full circle.  One possibility is that the
			 * there's actually enough space, but the rotor itself
			 * is preventing the allocation from succeeding because
			 * it's sitting between two free segments.  Therefore,
			 * we advance the rotor and see if that liberates a
			 * suitable segment.
			 */
			vmem_advance(vmp, rotor, rotor->vs_anext);
			vsp = rotor->vs_aprev;
			if (vsp->vs_type == VMEM_FREE && VS_SIZE(vsp) >= size)
				break;
			/*
			 * If there's a lower arena we can import from, or it's
			 * a VM_NOSLEEP allocation, let vmem_xalloc() handle it.
			 * Otherwise, wait until another thread frees something.
			 */
			if (vmp->vm_source_alloc != NULL ||
			    (vmflag & VM_NOSLEEP)) {
				mutex_exit(&vmp->vm_lock);
				return (vmem_xalloc(vmp, size, vmp->vm_quantum,
					0, 0, NULL, NULL, vmflag & (VM_KMFLAGS | VM_NEXTFIT)));
			}
			atomic_inc_64(&vmp->vm_kstat.vk_wait.value.ui64);
			atomic_inc_64(&vmp->vm_kstat.vk_threads_waiting.value.ui64);
			atomic_inc_64(&spl_vmem_threads_waiting);
			if (spl_vmem_threads_waiting > 1)
				printf("SPL: %s: waiting for %lu sized alloc after full circle of  %s, "
				    "waiting threads %llu, total threads waiting = %llu.\n",
				    __func__, size, vmp->vm_name,
				    vmp->vm_kstat.vk_threads_waiting.value.ui64,
				    spl_vmem_threads_waiting);
			cv_wait(&vmp->vm_cv, &vmp->vm_lock);
			atomic_dec_64(&spl_vmem_threads_waiting);
			atomic_dec_64(&vmp->vm_kstat.vk_threads_waiting.value.ui64);
			vsp = rotor->vs_anext;
		}
	}

	/*
	 * We found a segment.  Extract enough space to satisfy the allocation.
	 */
	addr = vsp->vs_start;
	vsp = vmem_seg_alloc(vmp, vsp, addr, size);
	ASSERT(vsp->vs_type == VMEM_ALLOC &&
	    vsp->vs_start == addr && vsp->vs_end == addr + size);

	/*
	 * Advance the rotor to right after the newly-allocated segment.
	 * That's where the next VM_NEXTFIT allocation will begin searching.
	 */
	vmem_advance(vmp, rotor, vsp);
	mutex_exit(&vmp->vm_lock);
	return ((void *)addr);
}

/*
 * Checks if vmp is guaranteed to have a size-byte buffer somewhere on its
 * freelist.  If size is not a power-of-2, it can return a false-negative.
 *
 * Used to decide if a newly imported span is superfluous after re-acquiring
 * the arena lock.
 */
static int
vmem_canalloc(vmem_t *vmp, size_t size)
{
	int hb;
	int flist = 0;
	ASSERT(MUTEX_HELD(&vmp->vm_lock));

	if ((size & (size - 1)) == 0)
		flist = lowbit(P2ALIGN(vmp->vm_freemap, size));
	else if ((hb = highbit(size)) < VMEM_FREELISTS)
		flist = lowbit(P2ALIGN(vmp->vm_freemap, 1ULL << hb));

	return (flist);
}

// Convenience functions for use when gauging
// allocation ability when not holding the lock.
// These are unreliable because vmp->vm_freemap is
// liable to change immediately after being examined.
int
vmem_canalloc_lock(vmem_t *vmp, size_t size)
{
	mutex_enter(&vmp->vm_lock);
	int i = vmem_canalloc(vmp, size);
	mutex_exit(&vmp->vm_lock);
	return (i);
}

int
vmem_canalloc_atomic(vmem_t *vmp, size_t size)
{
	int hb;
	int flist = 0;

	ulong_t freemap = __c11_atomic_load((_Atomic ulong_t *)&vmp->vm_freemap, __ATOMIC_SEQ_CST);

	if (ISP2(size))
		flist = lowbit(P2ALIGN(freemap, size));
	else if ((hb = highbit(size)) < VMEM_FREELISTS)
		flist = lowbit(P2ALIGN(freemap, 1ULL << hb));

	return (flist);
}

static inline uint64_t
spl_vmem_xnu_useful_bytes_free(void)
{
	extern volatile unsigned int vm_page_free_wanted;
	extern volatile unsigned int vm_page_free_count;
	extern volatile unsigned int vm_page_free_min;

	if (vm_page_free_wanted > 0)
		return (0);

	uint64_t bytes_free = (uint64_t)vm_page_free_count * (uint64_t)PAGESIZE;
	uint64_t bytes_min = (uint64_t)vm_page_free_min * (uint64_t)PAGESIZE;

	if (bytes_free <= bytes_min)
		return (0);

	uint64_t useful_free = bytes_free - bytes_min;

	return (useful_free);
}

static void *
spl_vmem_malloc_unconditionally_unlocked(size_t size)
{
	extern void *osif_malloc(uint64_t);
	atomic_inc_64(&spl_vmem_unconditional_allocs);
	atomic_add_64(&spl_vmem_unconditional_alloc_bytes, size);
	return(osif_malloc(size));
}

static void *
spl_vmem_malloc_unconditionally(size_t size)
{
	mutex_enter(&vmem_xnu_alloc_free_lock);
	void *m = spl_vmem_malloc_unconditionally_unlocked(size);
	cv_broadcast(&vmem_xnu_alloc_free_cv);
	mutex_exit(&vmem_xnu_alloc_free_lock);
	return (m);
}

static void *
spl_vmem_malloc_if_no_pressure(size_t size)
{
	// The mutex serializes concurrent callers, providing time for
	// the variables in spl_vmem_xnu_useful_bytes_free() to be updated.
	mutex_enter(&vmem_xnu_alloc_free_lock);
	if (spl_vmem_xnu_useful_bytes_free() > (MAX(size,16ULL*1024ULL*1024ULL))) {
		extern void *osif_malloc(uint64_t);
		void *p = osif_malloc(size);
		if (p != NULL) {
			atomic_inc_64(&spl_vmem_conditional_allocs);
			atomic_add_64(&spl_vmem_conditional_alloc_bytes, size);
			cv_broadcast(&vmem_xnu_alloc_free_cv);
		}
		mutex_exit(&vmem_xnu_alloc_free_lock);
		return (p);
	} else {
		mutex_exit(&vmem_xnu_alloc_free_lock);
		atomic_inc_64(&spl_vmem_conditional_alloc_deny);
		atomic_add_64(&spl_vmem_conditional_alloc_deny_bytes, size);
		return (NULL);
	}
}

/*
 * Allocate size bytes at offset phase from an align boundary such that the
 * resulting segment [addr, addr + size) is a subset of [minaddr, maxaddr)
 * that does not straddle a nocross-aligned boundary.
 */
void *
vmem_xalloc(vmem_t *vmp, size_t size, size_t align_arg, size_t phase,
			size_t nocross, void *minaddr, void *maxaddr, int vmflag)
{
	vmem_seg_t *vsp;
	vmem_seg_t *vbest = NULL;
	uintptr_t addr, taddr, start, end;
	uintptr_t align = (align_arg != 0) ? align_arg : vmp->vm_quantum;
	void *vaddr, *xvaddr = NULL;
	size_t xsize;
	int hb, flist, resv;
	uint32_t mtbf;

	if ((align | phase | nocross) & (vmp->vm_quantum - 1))
		panic("vmem_xalloc(%p, %lu, %lu, %lu, %lu, %p, %p, %x): "
			  "parameters not vm_quantum aligned",
			  (void *)vmp, size, align_arg, phase, nocross,
			  minaddr, maxaddr, vmflag);

	if (nocross != 0 &&
		(align > nocross || P2ROUNDUP(phase + size, align) > nocross))
		panic("vmem_xalloc(%p, %lu, %lu, %lu, %lu, %p, %p, %x): "
			  "overconstrained allocation",
			  (void *)vmp, size, align_arg, phase, nocross,
			  minaddr, maxaddr, vmflag);

	if (phase >= align || (align & (align - 1)) != 0 ||
		(nocross & (nocross - 1)) != 0)
		panic("vmem_xalloc(%p, %lu, %lu, %lu, %lu, %p, %p, %x): "
			  "parameters inconsistent or invalid",
			  (void *)vmp, size, align_arg, phase, nocross,
			  minaddr, maxaddr, vmflag);

	if ((mtbf = vmem_mtbf | vmp->vm_mtbf) != 0 && gethrtime() % mtbf == 0 &&
		(vmflag & (VM_NOSLEEP | VM_PANIC)) == VM_NOSLEEP)
		return (NULL);

	mutex_enter(&vmp->vm_lock);
	for (;;) {
		if (vmp->vm_nsegfree < VMEM_MINFREE &&
			!vmem_populate(vmp, vmflag))
			break;
	do_alloc:
		/*
		 * highbit() returns the highest bit + 1, which is exactly
		 * what we want: we want to search the first freelist whose
		 * members are *definitely* large enough to satisfy our
		 * allocation.  However, there are certain cases in which we
		 * want to look at the next-smallest freelist (which *might*
		 * be able to satisfy the allocation):
		 *
		 * (1)	The size is exactly a power of 2, in which case
		 *	the smaller freelist is always big enough;
		 *
		 * (2)	All other freelists are empty;
		 *
		 * (3)	We're in the highest possible freelist, which is
		 *	always empty (e.g. the 4GB freelist on 32-bit systems);
		 *
		 * (4)	We're doing a best-fit or first-fit allocation.
		 */
		if ((size & (size - 1)) == 0) {
			flist = lowbit(P2ALIGN(vmp->vm_freemap, size));
		} else {
			hb = highbit(size);
			if ((vmp->vm_freemap >> hb) == 0 ||
				hb == VMEM_FREELISTS ||
				(vmflag & (VM_BESTFIT | VM_FIRSTFIT)))
				hb--;
			flist = lowbit(P2ALIGN(vmp->vm_freemap, 1UL << hb));
		}

		for (vbest = NULL, vsp = (flist == 0) ? NULL :
			 vmp->vm_freelist[flist - 1].vs_knext;
			 vsp != NULL; vsp = vsp->vs_knext) {
			atomic_inc_64(&vmp->vm_kstat.vk_search.value.ui64);
			if (vsp->vs_start == 0) {
				/*
				 * We're moving up to a larger freelist,
				 * so if we've already found a candidate,
				 * the fit can't possibly get any better.
				 */
				if (vbest != NULL)
					break;
				/*
				 * Find the next non-empty freelist.
				 */
				flist = lowbit(P2ALIGN(vmp->vm_freemap,
									   VS_SIZE(vsp)));
				if (flist-- == 0)
					break;
				vsp = (vmem_seg_t *)&vmp->vm_freelist[flist];
				ASSERT(vsp->vs_knext->vs_type == VMEM_FREE);
				continue;
			}
			if (vsp->vs_end - 1 < (uintptr_t)minaddr)
				continue;
			if (vsp->vs_start > (uintptr_t)maxaddr - 1)
				continue;
			start = MAX(vsp->vs_start, (uintptr_t)minaddr);
			end = MIN(vsp->vs_end - 1, (uintptr_t)maxaddr - 1) + 1;
			taddr = P2PHASEUP(start, align, phase);
			if (P2BOUNDARY(taddr, size, nocross))
				taddr +=
				P2ROUNDUP(P2NPHASE(taddr, nocross), align);
			if ((taddr - start) + size > end - start ||
				(vbest != NULL && VS_SIZE(vsp) >= VS_SIZE(vbest)))
				continue;
			vbest = vsp;
			addr = taddr;
			if (!(vmflag & VM_BESTFIT) || VS_SIZE(vbest) == size)
				break;
		}
		if (vbest != NULL)
			break;
		ASSERT(xvaddr == NULL);
		if (size == 0)
			panic("vmem_xalloc(): size == 0");
		if (vmp->vm_source_alloc != NULL && nocross == 0 &&
			minaddr == NULL && maxaddr == NULL) {
			size_t aneeded, asize;
			size_t aquantum = MAX(vmp->vm_quantum,
								  vmp->vm_source->vm_quantum);
			size_t aphase = phase;
			if ((align > aquantum) &&
				!(vmp->vm_cflags & VMC_XALIGN)) {
				aphase = (P2PHASE(phase, aquantum) != 0) ?
				align - vmp->vm_quantum : align - aquantum;
				ASSERT(aphase >= phase);
			}
			aneeded = MAX(size + aphase, vmp->vm_min_import);
			asize = P2ROUNDUP(aneeded, aquantum);

			if (asize < size) {
				/*
				 * The rounding induced overflow; return NULL
				 * if we are permitted to fail the allocation
				 * (and explicitly panic if we aren't).
				 */
				if ((vmflag & VM_NOSLEEP) &&
					!(vmflag & VM_PANIC)) {
					mutex_exit(&vmp->vm_lock);
					return (NULL);
				}

				panic("vmem_xalloc(): size overflow");
			}

			/*
			 * Determine how many segment structures we'll consume.
			 * The calculation must be precise because if we're
			 * here on behalf of vmem_populate(), we are taking
			 * segments from a very limited reserve.
			 */
			if (size == asize && !(vmp->vm_cflags & VMC_XALLOC))
				resv = VMEM_SEGS_PER_SPAN_CREATE +
				VMEM_SEGS_PER_EXACT_ALLOC;
			else if (phase == 0 &&
					 align <= vmp->vm_source->vm_quantum)
				resv = VMEM_SEGS_PER_SPAN_CREATE +
				VMEM_SEGS_PER_LEFT_ALLOC;
			else
				resv = VMEM_SEGS_PER_ALLOC_MAX;

			ASSERT(vmp->vm_nsegfree >= resv);
			vmp->vm_nsegfree -= resv;	/* reserve our segs */
			mutex_exit(&vmp->vm_lock);
			if (vmp->vm_cflags & VMC_XALLOC) {
				//size_t oasize = asize;
				vaddr = ((vmem_ximport_t *)
						 vmp->vm_source_alloc)(vmp->vm_source,
											   &asize, align, vmflag & VM_KMFLAGS);
				//ASSERT(asize >= oasize);
				ASSERT(P2PHASE(asize,
							   vmp->vm_source->vm_quantum) == 0);
				ASSERT(!(vmp->vm_cflags & VMC_XALIGN) ||
				    IS_P2ALIGNED(vaddr, align));
			} else {
				atomic_inc_64(&vmp->vm_kstat.vk_parent_alloc.value.ui64);
				vaddr = vmp->vm_source_alloc(vmp->vm_source,
				    asize, vmflag & (VM_KMFLAGS | VM_NEXTFIT));
			}
			mutex_enter(&vmp->vm_lock);
			vmp->vm_nsegfree += resv;	/* claim reservation */
			aneeded = size + align - vmp->vm_quantum;
			aneeded = P2ROUNDUP(aneeded, vmp->vm_quantum);
			if (vaddr != NULL) {
				/*
				 * Since we dropped the vmem lock while
				 * calling the import function, other
				 * threads could have imported space
				 * and made our import unnecessary.  In
				 * order to save space, we return
				 * excess imports immediately.
				 */
				// but if there are threads waiting below,
				// do not return the excess import, rather
				// wake those threads up so they can use it.
				if (asize > aneeded &&
					vmp->vm_source_free != NULL &&
				        vmp->vm_kstat.vk_threads_waiting.value.ui64 == 0 &&
					vmem_canalloc(vmp, aneeded)) {
					ASSERT(resv >=
					    VMEM_SEGS_PER_MIDDLE_ALLOC);
					xvaddr = vaddr;
					xsize = asize;
					goto do_alloc;
				} else if (vmp->vm_kstat.vk_threads_waiting.value.ui64 > 0) {
					vmp->vm_kstat.vk_excess.value.ui64++;
					cv_broadcast(&vmp->vm_cv);
				}
				vbest = vmem_span_create(vmp, vaddr, asize, 1);
				addr = P2PHASEUP(vbest->vs_start, align, phase);
				break;
			} else if (vmem_canalloc(vmp, aneeded)) {
				/*
				 * Our import failed, but another thread
				 * added sufficient free memory to the arena
				 * to satisfy our request.  Go back and
				 * grab it.
				 */
				ASSERT(resv >= VMEM_SEGS_PER_MIDDLE_ALLOC);
				goto do_alloc;
			}
		}

		/*
		 * If the requestor chooses to fail the allocation attempt
		 * rather than reap wait and retry - get out of the loop.
		 */
		if (vmflag & VM_ABORT)
			break;
		mutex_exit(&vmp->vm_lock);

		if (vmp->vm_cflags & VMC_IDENTIFIER)
			kmem_reap_idspace();
		else
			kmem_reap();

		mutex_enter(&vmp->vm_lock);
		if (vmflag & VM_NOSLEEP)
			break;
		atomic_inc_64(&vmp->vm_kstat.vk_wait.value.ui64);
		atomic_inc_64(&vmp->vm_kstat.vk_threads_waiting.value.ui64);
		atomic_inc_64(&spl_vmem_threads_waiting);
		if (spl_vmem_threads_waiting > 0) {
			printf("SPL: %s: vmem waiting for %lu sized alloc for %s, "
			    "waiting threads %llu, total threads waiting = %llu\n",
			    __func__, size, vmp->vm_name,
			    vmp->vm_kstat.vk_threads_waiting.value.ui64,
			    spl_vmem_threads_waiting);
			extern int64_t spl_free_set_and_wait_pressure(int64_t, boolean_t, clock_t);
			extern int64_t spl_free_manual_pressure_wrapper(void);
			mutex_exit(&vmp->vm_lock);
			spl_free_set_pressure(0); // release other waiting threads
			int64_t target_pressure = size * spl_vmem_threads_waiting;
			int64_t delivered_pressure = spl_free_set_and_wait_pressure(target_pressure,
			    TRUE, USEC2NSEC(500));
			printf("SPL: %s: pressure %lld targeted, %lld delivered\n",
			    __func__, target_pressure, delivered_pressure);
			mutex_enter(&vmp->vm_lock);
		}
		cv_wait(&vmp->vm_cv, &vmp->vm_lock);
		atomic_dec_64(&spl_vmem_threads_waiting);
		atomic_dec_64(&vmp->vm_kstat.vk_threads_waiting.value.ui64);
	}
	if (vbest != NULL) {
		ASSERT(vbest->vs_type == VMEM_FREE);
		ASSERT(vbest->vs_knext != vbest);
		/* re-position to end of buffer */
		if (vmflag & VM_ENDALLOC) {
			addr += ((vbest->vs_end - (addr + size)) / align) *
			align;
		}
		(void) vmem_seg_alloc(vmp, vbest, addr, size);
		mutex_exit(&vmp->vm_lock);
		if (xvaddr) {
			atomic_inc_64(&vmp->vm_kstat.vk_parent_free.value.ui64);
			vmp->vm_source_free(vmp->vm_source, xvaddr, xsize);
		}
		ASSERT(P2PHASE(addr, align) == phase);
		ASSERT(!P2BOUNDARY(addr, size, nocross));
		ASSERT(addr >= (uintptr_t)minaddr);
		ASSERT(addr + size - 1 <= (uintptr_t)maxaddr - 1);
		return ((void *)addr);
	}
	vmp->vm_kstat.vk_fail.value.ui64++;
	mutex_exit(&vmp->vm_lock);
	if (vmflag & VM_PANIC)
		panic("vmem_xalloc(%p, %lu, %lu, %lu, %lu, %p, %p, %x): "
			  "cannot satisfy mandatory allocation",
			  (void *)vmp, size, align_arg, phase, nocross,
			  minaddr, maxaddr, vmflag);
	ASSERT(xvaddr == NULL);
	return (NULL);
}

/*
 * Free the segment [vaddr, vaddr + size), where vaddr was a constrained
 * allocation.  vmem_xalloc() and vmem_xfree() must always be paired because
 * both routines bypass the quantum caches.
 */
void
vmem_xfree(vmem_t *vmp, void *vaddr, size_t size)
{
	vmem_seg_t *vsp, *vnext, *vprev;

	mutex_enter(&vmp->vm_lock);

	vsp = vmem_hash_delete(vmp, (uintptr_t)vaddr, size);
	vsp->vs_end = P2ROUNDUP(vsp->vs_end, vmp->vm_quantum);

	/*
	 * Attempt to coalesce with the next segment.
	 */
	vnext = vsp->vs_anext;
	if (vnext->vs_type == VMEM_FREE) {
		ASSERT(vsp->vs_end == vnext->vs_start);
		vmem_freelist_delete(vmp, vnext);
		vsp->vs_end = vnext->vs_end;
		vmem_seg_destroy(vmp, vnext);
	}

	/*
	 * Attempt to coalesce with the previous segment.
	 */
	vprev = vsp->vs_aprev;
	if (vprev->vs_type == VMEM_FREE) {
		ASSERT(vprev->vs_end == vsp->vs_start);
		vmem_freelist_delete(vmp, vprev);
		vprev->vs_end = vsp->vs_end;
		vmem_seg_destroy(vmp, vsp);
		vsp = vprev;
	}

	/*
	 * If the entire span is free, return it to the source.
	 */
	if (vsp->vs_aprev->vs_import && vmp->vm_source_free != NULL &&
		vsp->vs_aprev->vs_type == VMEM_SPAN &&
		vsp->vs_anext->vs_type == VMEM_SPAN) {
		vaddr = (void *)vsp->vs_start;
		size = VS_SIZE(vsp);
		ASSERT(size == VS_SIZE(vsp->vs_aprev));
		vmem_span_destroy(vmp, vsp);
		vmp->vm_kstat.vk_parent_free.value.ui64++;
		mutex_exit(&vmp->vm_lock);
		vmp->vm_source_free(vmp->vm_source, vaddr, size);
	} else {
		vmem_freelist_insert(vmp, vsp);
		mutex_exit(&vmp->vm_lock);
	}
}

/*
 * Allocate size bytes from arena vmp.  Returns the allocated address
 * on success, NULL on failure.  vmflag specifies VM_SLEEP or VM_NOSLEEP,
 * and may also specify best-fit, first-fit, or next-fit allocation policy
 * instead of the default instant-fit policy.  VM_SLEEP allocations are
 * guaranteed to succeed.
 */
void *
vmem_alloc(vmem_t *vmp, size_t size, int vmflag)
{
	vmem_seg_t *vsp;
	uintptr_t addr;
	int hb;
	int flist = 0;
	uint32_t mtbf;

	if (size - 1 < vmp->vm_qcache_max)
		return (kmem_cache_alloc(vmp->vm_qcache[(size - 1) >>
												vmp->vm_qshift], vmflag & VM_KMFLAGS));

	if ((mtbf = vmem_mtbf | vmp->vm_mtbf) != 0 && gethrtime() % mtbf == 0 &&
		(vmflag & (VM_NOSLEEP | VM_PANIC)) == VM_NOSLEEP)
		return (NULL);

	if (vmflag & VM_NEXTFIT)
		return (vmem_nextfit_alloc(vmp, size, vmflag));

	if (vmflag & (VM_BESTFIT | VM_FIRSTFIT))
		return (vmem_xalloc(vmp, size, vmp->vm_quantum, 0, 0,
							NULL, NULL, vmflag));
	if (vmp->vm_cflags & VM_NEXTFIT)
		return (vmem_nextfit_alloc(vmp, size, vmflag));

	/*
	 * Unconstrained instant-fit allocation from the segment list.
	 */
	mutex_enter(&vmp->vm_lock);

	if (vmp->vm_nsegfree >= VMEM_MINFREE || vmem_populate(vmp, vmflag)) {
		if ((size & (size - 1)) == 0)
			flist = lowbit(P2ALIGN(vmp->vm_freemap, size));
		else if ((hb = highbit(size)) < VMEM_FREELISTS)
			flist = lowbit(P2ALIGN(vmp->vm_freemap, 1UL << hb));
	}

	if (flist-- == 0) {
		mutex_exit(&vmp->vm_lock);
		return (vmem_xalloc(vmp, size, vmp->vm_quantum,
							0, 0, NULL, NULL, vmflag));
	}

	ASSERT(size <= (1UL << flist));
	vsp = vmp->vm_freelist[flist].vs_knext;
	addr = vsp->vs_start;
	if (vmflag & VM_ENDALLOC) {
		addr += vsp->vs_end - (addr + size);
	}
	(void) vmem_seg_alloc(vmp, vsp, addr, size);
	mutex_exit(&vmp->vm_lock);
	return ((void *)addr);
}

/*
 * Free the segment [vaddr, vaddr + size).
 */
void
vmem_free(vmem_t *vmp, void *vaddr, size_t size)
{
	if (size - 1 < vmp->vm_qcache_max)
		kmem_cache_free(vmp->vm_qcache[(size - 1) >> vmp->vm_qshift],
		    vaddr);
	else
		vmem_xfree(vmp, vaddr, size);
}

/*
 * Determine whether arena vmp contains the segment [vaddr, vaddr + size).
 */
int
vmem_contains(vmem_t *vmp, void *vaddr, size_t size)
{
	uintptr_t start = (uintptr_t)vaddr;
	uintptr_t end = start + size;
	vmem_seg_t *vsp;
	vmem_seg_t *seg0 = &vmp->vm_seg0;

	mutex_enter(&vmp->vm_lock);
	vmp->vm_kstat.vk_contains.value.ui64++;
	for (vsp = seg0->vs_knext; vsp != seg0; vsp = vsp->vs_knext) {
		vmp->vm_kstat.vk_contains_search.value.ui64++;
		ASSERT(vsp->vs_type == VMEM_SPAN);
		if (start >= vsp->vs_start && end - 1 <= vsp->vs_end - 1)
			break;
	}
	mutex_exit(&vmp->vm_lock);
	return (vsp != seg0);
}

/*
 * Add the span [vaddr, vaddr + size) to arena vmp.
 */
void *
vmem_add(vmem_t *vmp, void *vaddr, size_t size, int vmflag)
{
	if (vaddr == NULL || size == 0)
		panic("vmem_add(%p, %p, %lu): bad arguments",
			  (void *)vmp, vaddr, size);

	ASSERT(!vmem_contains(vmp, vaddr, size));

	mutex_enter(&vmp->vm_lock);
	if (vmem_populate(vmp, vmflag))
		(void) vmem_span_create(vmp, vaddr, size, 0);
	else
		vaddr = NULL;
	mutex_exit(&vmp->vm_lock);
	return (vaddr);
}

/*
 * Walk the vmp arena, applying func to each segment matching typemask.
 * If VMEM_REENTRANT is specified, the arena lock is dropped across each
 * call to func(); otherwise, it is held for the duration of vmem_walk()
 * to ensure a consistent snapshot.  Note that VMEM_REENTRANT callbacks
 * are *not* necessarily consistent, so they may only be used when a hint
 * is adequate.
 */
void
vmem_walk(vmem_t *vmp, int typemask,
		  void (*func)(void *, void *, size_t), void *arg)
{
	vmem_seg_t *vsp;
	vmem_seg_t *seg0 = &vmp->vm_seg0;
	vmem_seg_t walker;

	if (typemask & VMEM_WALKER)
		return;

	bzero(&walker, sizeof (walker));
	walker.vs_type = VMEM_WALKER;

	mutex_enter(&vmp->vm_lock);
	VMEM_INSERT(seg0, &walker, a);
	for (vsp = seg0->vs_anext; vsp != seg0; vsp = vsp->vs_anext) {
		if (vsp->vs_type & typemask) {
			void *start = (void *)vsp->vs_start;
			size_t size = VS_SIZE(vsp);
			if (typemask & VMEM_REENTRANT) {
				vmem_advance(vmp, &walker, vsp);
				mutex_exit(&vmp->vm_lock);
				func(arg, start, size);
				mutex_enter(&vmp->vm_lock);
				vsp = &walker;
			} else {
				func(arg, start, size);
			}
		}
	}
	vmem_advance(vmp, &walker, NULL);
	mutex_exit(&vmp->vm_lock);
}

/*
 * Return the total amount of memory whose type matches typemask.  Thus:
 *
 *	typemask VMEM_ALLOC yields total memory allocated (in use).
 *	typemask VMEM_FREE yields total memory free (available).
 *	typemask (VMEM_ALLOC | VMEM_FREE) yields total arena size.
 */
size_t
vmem_size(vmem_t *vmp, int typemask)
{
	int64_t size = 0;

	if (typemask & VMEM_ALLOC)
		size += (int64_t)vmp->vm_kstat.vk_mem_inuse.value.ui64;
	if (typemask & VMEM_FREE)
		size += (int64_t)vmp->vm_kstat.vk_mem_total.value.ui64 -
		    (int64_t)vmp->vm_kstat.vk_mem_inuse.value.ui64;
	if (size < 0)
		size = 0;

	return ((size_t)size);
}

size_t
vmem_size_locked(vmem_t *vmp, int typemask)
{
	boolean_t m = (mutex_owner(&vmp->vm_lock) == curthread);

	if (!m)
		mutex_enter(&vmp->vm_lock);
	size_t s = vmem_size(vmp, typemask);
	if (!m)
		mutex_exit(&vmp->vm_lock);
	return (s);
}

size_t
vmem_size_semi_atomic(vmem_t *vmp, int typemask)
{
	int64_t size = 0;
	uint64_t inuse = 0;
	uint64_t total = 0;

	__sync_swap(&total, vmp->vm_kstat.vk_mem_total.value.ui64);
	__sync_swap(&inuse, vmp->vm_kstat.vk_mem_inuse.value.ui64);

	int64_t inuse_signed = (int64_t)inuse;
	int64_t total_signed = (int64_t)total;

	if (typemask & VMEM_ALLOC)
		size += inuse_signed;
	if (typemask & VMEM_FREE)
		size += total_signed - inuse_signed;

	if (size < 0)
		size = 0;

	return ((size_t) size);
}

size_t
spl_vmem_size(vmem_t *vmp, int typemask)
{
	return(vmem_size_locked(vmp, typemask));
}

/*
 * Create an arena called name whose initial span is [base, base + size).
 * The arena's natural unit of currency is quantum, so vmem_alloc()
 * guarantees quantum-aligned results.  The arena may import new spans
 * by invoking afunc() on source, and may return those spans by invoking
 * ffunc() on source.  To make small allocations fast and scalable,
 * the arena offers high-performance caching for each integer multiple
 * of quantum up to qcache_max.
 */
static vmem_t *
vmem_create_common(const char *name, void *base, size_t size, size_t quantum,
				   void *(*afunc)(vmem_t *, size_t, int),
				   void (*ffunc)(vmem_t *, void *, size_t),
				   vmem_t *source, size_t qcache_max, int vmflag)
{
	int i;
	size_t nqcache;
	vmem_t *vmp, *cur, **vmpp;
	vmem_seg_t *vsp;
	vmem_freelist_t *vfp;
	uint32_t id = atomic_inc_32_nv(&vmem_id);

	if (vmem_vmem_arena != NULL) {
		vmp = vmem_alloc(vmem_vmem_arena, sizeof (vmem_t),
						 vmflag & VM_KMFLAGS);
	} else {
		ASSERT(id <= VMEM_INITIAL);
		vmp = &vmem0[id - 1];
	}

	/* An identifier arena must inherit from another identifier arena */
	ASSERT(source == NULL || ((source->vm_cflags & VMC_IDENTIFIER) ==
							  (vmflag & VMC_IDENTIFIER)));

	if (vmp == NULL)
		return (NULL);
	bzero(vmp, sizeof (vmem_t));

	(void) snprintf(vmp->vm_name, VMEM_NAMELEN, "%s", name);
	mutex_init(&vmp->vm_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&vmp->vm_cv, NULL, CV_DEFAULT, NULL);
	vmp->vm_cflags = vmflag;
	vmflag &= VM_KMFLAGS;

	vmp->vm_quantum = quantum;
	vmp->vm_qshift = highbit(quantum) - 1;
	nqcache = MIN(qcache_max >> vmp->vm_qshift, VMEM_NQCACHE_MAX);

	for (i = 0; i <= VMEM_FREELISTS; i++) {
		vfp = &vmp->vm_freelist[i];
		vfp->vs_end = 1UL << i;
		vfp->vs_knext = (vmem_seg_t *)(vfp + 1);
		vfp->vs_kprev = (vmem_seg_t *)(vfp - 1);
	}

	vmp->vm_freelist[0].vs_kprev = NULL;
	vmp->vm_freelist[VMEM_FREELISTS].vs_knext = NULL;
	vmp->vm_freelist[VMEM_FREELISTS].vs_end = 0;
	vmp->vm_hash_table = vmp->vm_hash0;
	vmp->vm_hash_mask = VMEM_HASH_INITIAL - 1;
	vmp->vm_hash_shift = highbit(vmp->vm_hash_mask);

	vsp = &vmp->vm_seg0;
	vsp->vs_anext = vsp;
	vsp->vs_aprev = vsp;
	vsp->vs_knext = vsp;
	vsp->vs_kprev = vsp;
	vsp->vs_type = VMEM_SPAN;

	vsp = &vmp->vm_rotor;
	vsp->vs_type = VMEM_ROTOR;
	VMEM_INSERT(&vmp->vm_seg0, vsp, a);

	bcopy(&vmem_kstat_template, &vmp->vm_kstat, sizeof (vmem_kstat_t));

	vmp->vm_id = id;
	if (source != NULL)
		vmp->vm_kstat.vk_source_id.value.ui32 = source->vm_id;
	vmp->vm_source = source;
	vmp->vm_source_alloc = afunc;
	vmp->vm_source_free = ffunc;

	/*
	 * Some arenas (like vmem_metadata and kmem_metadata) cannot
	 * use quantum caching to lower fragmentation.  Instead, we
	 * increase their imports, giving a similar effect.
	 */
	if (vmp->vm_cflags & VMC_NO_QCACHE) {
		if (qcache_max > VMEM_NQCACHE_MAX && ISP2(qcache_max)) {
			vmp->vm_min_import = qcache_max;
		} else {
			vmp->vm_min_import =
			    VMEM_QCACHE_SLABSIZE(nqcache << vmp->vm_qshift);
		}
		nqcache = 0;
	}

	if (nqcache != 0) {
		ASSERT(!(vmflag & VM_NOSLEEP));
		vmp->vm_qcache_max = nqcache << vmp->vm_qshift;
		for (i = 0; i < nqcache; i++) {
			char buf[VMEM_NAMELEN + 21];
			(void) snprintf(buf, VMEM_NAMELEN + 20, "%s_%lu", vmp->vm_name,
							(i + 1) * quantum);
			vmp->vm_qcache[i] = kmem_cache_create(buf,
												  (i + 1) * quantum, quantum, NULL, NULL, NULL,
												  NULL, vmp, KMC_QCACHE | KMC_NOTOUCH);
		}
	}

	if ((vmp->vm_ksp = kstat_create("vmem", vmp->vm_id, vmp->vm_name,
									"vmem", KSTAT_TYPE_NAMED, sizeof (vmem_kstat_t) /
									sizeof (kstat_named_t), KSTAT_FLAG_VIRTUAL)) != NULL) {
		vmp->vm_ksp->ks_data = &vmp->vm_kstat;
		kstat_install(vmp->vm_ksp);
	}

	mutex_enter(&vmem_list_lock);
	vmpp = &vmem_list;
	while ((cur = *vmpp) != NULL)
		vmpp = &cur->vm_next;
	*vmpp = vmp;
	mutex_exit(&vmem_list_lock);

	if (vmp->vm_cflags & VMC_POPULATOR) {
		ASSERT(vmem_populators < VMEM_INITIAL);
		vmem_populator[atomic_inc_32_nv(&vmem_populators) - 1] = vmp;
		mutex_enter(&vmp->vm_lock);
		(void) vmem_populate(vmp, vmflag | VM_PANIC);
		mutex_exit(&vmp->vm_lock);
	}

	if ((base || size) && vmem_add(vmp, base, size, vmflag) == NULL) {
		vmem_destroy(vmp);
		return (NULL);
	}

	return (vmp);
}

vmem_t *
vmem_xcreate(const char *name, void *base, size_t size, size_t quantum,
    vmem_ximport_t *afunc, vmem_free_t *ffunc, vmem_t *source,
    size_t qcache_max, int vmflag)
{
	ASSERT(!(vmflag & (VMC_POPULATOR | VMC_XALLOC)));
	vmflag &= ~(VMC_POPULATOR | VMC_XALLOC);

	return (vmem_create_common(name, base, size, quantum,
							   (vmem_alloc_t *)afunc, ffunc, source, qcache_max,
							   vmflag | VMC_XALLOC));
}

vmem_t *
vmem_create(const char *name, void *base, size_t size, size_t quantum,
			vmem_alloc_t *afunc, vmem_free_t *ffunc, vmem_t *source,
			size_t qcache_max, int vmflag)
{
	ASSERT(!(vmflag & (VMC_XALLOC | VMC_XALIGN)));
	vmflag &= ~(VMC_XALLOC | VMC_XALIGN);

	return (vmem_create_common(name, base, size, quantum,
							   afunc, ffunc, source, qcache_max, vmflag));
}

/*
 * Destroy arena vmp.
 */
void
vmem_destroy(vmem_t *vmp)
{
	vmem_t *cur, **vmpp;
	vmem_seg_t *seg0 = &vmp->vm_seg0;
	vmem_seg_t *vsp, *anext;
	size_t leaked;

	mutex_enter(&vmem_list_lock);
	vmpp = &vmem_list;
	while ((cur = *vmpp) != vmp)
		vmpp = &cur->vm_next;
	*vmpp = vmp->vm_next;
	mutex_exit(&vmem_list_lock);

	leaked = vmem_size(vmp, VMEM_ALLOC);
	if (leaked != 0)
		printf( "SPL: vmem_destroy('%s'): leaked %lu %s\n",
			   vmp->vm_name, leaked, (vmp->vm_cflags & VMC_IDENTIFIER) ?
			   "identifiers" : "bytes");

	if (vmp->vm_hash_table != vmp->vm_hash0)
		vmem_free(vmem_hash_arena, vmp->vm_hash_table,
		    (vmp->vm_hash_mask + 1) * sizeof (void *));

	/*
	 * Give back the segment structures for anything that's left in the
	 * arena, e.g. the primary spans and their free segments.
	 */
	VMEM_DELETE(&vmp->vm_rotor, a);
	for (vsp = seg0->vs_anext; vsp != seg0; vsp = anext) {
		anext = vsp->vs_anext;
		vmem_putseg_global(vsp);
	}

	while (vmp->vm_nsegfree > 0)
		vmem_putseg_global(vmem_getseg(vmp));

	kstat_delete(vmp->vm_ksp);

	mutex_destroy(&vmp->vm_lock);
	cv_destroy(&vmp->vm_cv);
	vmem_free(vmem_vmem_arena, vmp, sizeof (vmem_t));
}


/*
 * Destroy arena vmp.
 */
void
vmem_destroy_internal(vmem_t *vmp)
{
	vmem_t *cur, **vmpp;
	vmem_seg_t *seg0 = &vmp->vm_seg0;
	vmem_seg_t *vsp, *anext;
	size_t leaked;

	mutex_enter(&vmem_list_lock);
	vmpp = &vmem_list;
	while ((cur = *vmpp) != vmp)
		vmpp = &cur->vm_next;
	*vmpp = vmp->vm_next;
	mutex_exit(&vmem_list_lock);

	leaked = vmem_size(vmp, VMEM_ALLOC);
	if (leaked != 0)
		printf("SPL: vmem_destroy('%s'): leaked %lu %s\n",
			   vmp->vm_name, leaked, (vmp->vm_cflags & VMC_IDENTIFIER) ?
			   "identifiers" : "bytes");

	if (vmp->vm_hash_table != vmp->vm_hash0)
	  if(vmem_hash_arena != NULL)
		vmem_free(vmem_hash_arena, vmp->vm_hash_table,
		    (vmp->vm_hash_mask + 1) * sizeof (void *));

	/*
	 * Give back the segment structures for anything that's left in the
	 * arena, e.g. the primary spans and their free segments.
	 */
	VMEM_DELETE(&vmp->vm_rotor, a);
	for (vsp = seg0->vs_anext; vsp != seg0; vsp = anext) {
		anext = vsp->vs_anext;
		vmem_putseg_global(vsp);
	}

	while (vmp->vm_nsegfree > 0)
		vmem_putseg_global(vmem_getseg(vmp));

         if (!(vmp->vm_cflags & VMC_IDENTIFIER) && vmem_size(vmp, VMEM_ALLOC) != 0)
	   printf("SPL: vmem_destroy('%s'): STILL %lu bytes at kstat_delete() time\n",
		  vmp->vm_name, vmem_size(vmp, VMEM_ALLOC));

	kstat_delete(vmp->vm_ksp);

	mutex_destroy(&vmp->vm_lock);
	cv_destroy(&vmp->vm_cv);

	// Alas, to free, requires access to "vmem_vmem_arena" the very thing
	// we release first.
	//vmem_free(vmem_vmem_arena, vmp, sizeof (vmem_t));
}

/*
 * Only shrink vmem hashtable if it is 1<<vmem_rescale_minshift times (8x)
 * larger than necessary.
 */
int vmem_rescale_minshift = 3;

/*
 * Resize vmp's hash table to keep the average lookup depth near 1.0.
 */
static void
vmem_hash_rescale(vmem_t *vmp)
{
	vmem_seg_t **old_table, **new_table, *vsp;
	size_t old_size, new_size, h, nseg;

	nseg = (size_t)(vmp->vm_kstat.vk_alloc.value.ui64 -
	    vmp->vm_kstat.vk_free.value.ui64);

	new_size = MAX(VMEM_HASH_INITIAL, 1 << (highbit(3 * nseg + 4) - 2));
	old_size = vmp->vm_hash_mask + 1;

	if ((old_size >> vmem_rescale_minshift) <= new_size &&
	    new_size <= (old_size << 1))
		return;

	new_table = vmem_alloc(vmem_hash_arena, new_size * sizeof (void *),
						   VM_NOSLEEP);
	if (new_table == NULL)
		return;
	bzero(new_table, new_size * sizeof (void *));

	mutex_enter(&vmp->vm_lock);

	old_size = vmp->vm_hash_mask + 1;
	old_table = vmp->vm_hash_table;

	vmp->vm_hash_mask = new_size - 1;
	vmp->vm_hash_table = new_table;
	vmp->vm_hash_shift = highbit(vmp->vm_hash_mask);

	for (h = 0; h < old_size; h++) {
		vsp = old_table[h];
		while (vsp != NULL) {
			uintptr_t addr = vsp->vs_start;
			vmem_seg_t *next_vsp = vsp->vs_knext;
			vmem_seg_t **hash_bucket = VMEM_HASH(vmp, addr);
			vsp->vs_knext = *hash_bucket;
			*hash_bucket = vsp;
			vsp = next_vsp;
		}
	}

	mutex_exit(&vmp->vm_lock);

	if (old_table != vmp->vm_hash0)
		vmem_free(vmem_hash_arena, old_table,
		    old_size * sizeof (void *));
}

/*
 * Perform periodic maintenance on all vmem arenas.
 */

void
vmem_update(void *dummy)
{
	vmem_t *vmp;

	mutex_enter(&vmem_list_lock);
	for (vmp = vmem_list; vmp != NULL; vmp = vmp->vm_next) {
		/*
		 * If threads are waiting for resources, wake them up
		 * periodically so they can issue another kmem_reap()
		 * to reclaim resources cached by the slab allocator.
		 */
		cv_broadcast(&vmp->vm_cv);

		/*
		 * Rescale the hash table to keep the hash chains short.
		 */
		vmem_hash_rescale(vmp);
	}
	mutex_exit(&vmem_list_lock);

	(void) bsd_timeout(vmem_update, dummy, &vmem_update_interval);
}

void
vmem_qcache_reap(vmem_t *vmp)
{
	int i;

	/*
	 * Reap any quantum caches that may be part of this vmem.
	 */
	for (i = 0; i < VMEM_NQCACHE_MAX; i++)
		if (vmp->vm_qcache[i])
			kmem_cache_reap_now(vmp->vm_qcache[i]);
}

/* given a size, return the appropriate vmem_bucket_arena[] entry */

static inline uint16_t
vmem_bucket_number(size_t size)
{
	// For VMEM_BUCKET_HIBIT == 12,
        // vmem_bucket_arena[n] holds allocations from 2^[n+11]+1 to  2^[n+12],
        // so for [n] = 0, 2049-4096, for [n]=5 65537-131072, for [n]=7 (256k+1)-512k

        // set hb: 512k == 19, 256k+1 == 19, 256k == 18, ...
        const int hb = highbit(size-1);

        int bucket = hb - VMEM_BUCKET_LOWBIT;

        // very large allocations go into the 16 MiB bucket
        if (hb > VMEM_BUCKET_HIBIT)
                bucket = VMEM_BUCKET_HIBIT - VMEM_BUCKET_LOWBIT;

        // very small allocations go into the 4 kiB bucket
        if (bucket < 0)
                bucket = 0;

	return (bucket);
}

static inline vmem_t *
vmem_bucket_arena_by_size(size_t size)
{
	uint16_t bucket = vmem_bucket_number(size);

        return(vmem_bucket_arena[bucket]);
}

static inline void
vmem_bucket_wake_all_waiters(void)
{
	for (int i = VMEM_BUCKET_LOWBIT; i < VMEM_BUCKET_HIBIT; i++) {
		const int bucket = i - VMEM_BUCKET_LOWBIT;
		vmem_t *bvmp = vmem_bucket_arena[bucket];
		cv_broadcast(&bvmp->vm_cv);
	}
	cv_broadcast(&spl_heap_arena->vm_cv);
}

/*
 * return NULL in the typical case, but force an allocation if we have been
 * stuck waiting for an allocation for a long time
 */
static inline void *
xnu_allocate_throttled_bail(uint64_t now_ticks, vmem_t *calling_vmp, size_t size, int vmflags)
{

	// caller will be a bucket arena looking for memory
	// vmp will be spl_default_arena_parent, which is just a placeholder.

	// we can allocate up to this much in a short time period
	const int32_t recent_threshold = 16ULL*1024ULL*1024ULL;

	// count of bytes allocated in the last second
	static volatile _Atomic int32_t recently_allocated = 0;

	static volatile _Atomic uint64_t last_forced_ticks = 0;

	// (Concurrent) allocations that are in xnu_alloc_throttled_bail().
	static volatile _Atomic int32_t alloc_bytes_in_flight = 0;

	alloc_bytes_in_flight += size;

	// if we have a long queue on this arena's cv_wait in vmem_xalloc()
	// then we run the risk of stalling the whole I/O subsystem.  Force
	// a single allocation here if so, then bounce the others back.

	const int64_t unstall_threshold = 8;

	if (calling_vmp->vm_kstat.vk_threads_waiting.value.ui64 >= unstall_threshold) {
		spl_free_set_emergency_pressure((int64_t)size * unstall_threshold);
		static volatile _Atomic uint32_t unstall_waiters = 0;
		static volatile _Atomic bool another_processing = false;
		unstall_waiters++;
		// note that the unstall waiters can be from different arenas
		for (int iter = 0; unstall_waiters > 1; iter++) {
			// wait on the arena cv
			mutex_enter(&calling_vmp->vm_lock);
			(void) cv_timedwait_hires(&calling_vmp->vm_cv, &calling_vmp->vm_lock,
			    USEC2NSEC(500UL * MAX(unstall_waiters,1UL)), 0, 0);
			// we are likely to have been awakened by a cv_broadcast
			if (calling_vmp->vm_kstat.vk_threads_waiting.value.ui64 < unstall_threshold)
				break;
			if (iter >= 10 && another_processing == false) {
				another_processing = true;
				break;
			}
			mutex_exit(&calling_vmp->vm_lock);
			spl_free_set_emergency_pressure((int64_t)size);
		}
		// still have the mutex
		if (calling_vmp->vm_kstat.vk_threads_waiting.value.ui64 >= unstall_threshold) {
			// we're the only waiter
			void *force_m = spl_vmem_malloc_unconditionally(size);
			atomic_inc_64(&spl_xat_forced);
			last_forced_ticks = now_ticks;
			recently_allocated += size;
			unstall_waiters--;
			another_processing = false;
			mutex_exit(&calling_vmp->vm_lock);
			alloc_bytes_in_flight -= size;
			return (force_m);
		} else {
			// we exited the for loop and the cv_wait queue is now
			// relatively short, so bounce back to vmem_xalloc(), but
			// wake up a single other waiter, which might be in the for
			// loop above, or might be in the xalloc cv_wait().
			// the last one out of the for loop for this arena will
			// almost certainly wake up a waiter in xalloc, so the queue
			// will continue to try to be drained.
			unstall_waiters--;
			another_processing = false;
			cv_signal(&calling_vmp->vm_cv);
			mutex_exit(&calling_vmp->vm_lock);
			if (recently_allocated >= recent_threshold) {
				alloc_bytes_in_flight -= size;
				return (NULL);
			} else
				now_ticks = zfs_lbolt();
		}
	}

	// if this arena has  a queue at the cv_wait in vmem_xalloc() that
	// is this long, a forced allocation will be efficient.
	const uint64_t max_arena_waiters = 4;

	// if (concurrently) we are dealing more than the threshold of bytes,
	// deny this thread entry unless we have a long queue at the arena cv_wait

	if (alloc_bytes_in_flight > recent_threshold) {
		if (calling_vmp->vm_kstat.vk_threads_waiting.value.ui64 < max_arena_waiters) {
			alloc_bytes_in_flight -= size;
                        return (NULL);
		}
	}

	const uint64_t one_second = hz;
	const uint64_t ten_second = one_second * 10ULL;
	const uint64_t pushpage_after = ten_second;
	const uint64_t non_pushpage_after = 3ULL * ten_second;

	// wait at least one second from previous force
	// unless we haven't allocated over the recent_threshold yet

	if (now_ticks < last_forced_ticks + one_second) {
		if (recently_allocated >= recent_threshold) {
			alloc_bytes_in_flight -= size;
			return (NULL);
		} else {
			// it has been a second, clear out recently_allocated
			recently_allocated = 0;
		}
	}

	const uint64_t last_success_seconds = spl_xat_lastalloc;
	const uint64_t last_success_ticks = last_success_seconds * 60ULL;

	// if XAT has done any successful allocation in the past
	// second, don't allocate now unless we have a long queue
	// at the arena's cv_wait, because it's likely that XAT will
	// shortly be able to do another allocation that will
	// satisfy this one.

	if (now_ticks < last_success_ticks + one_second) {
		if (calling_vmp->vm_kstat.vk_threads_waiting.value.ui64 < max_arena_waiters) {
			alloc_bytes_in_flight -= size;
			return (NULL);
		}
	}

	const uint64_t lastfree_seconds = spl_xat_lastfree;
	const uint64_t lastfree_ticks = lastfree_seconds * 60ULL;

	// if we have freed in the past second, then bail out
	// unless there is already a big queue at the cv_wait.
	// recent freeing implies memory thrashing, which we
	// do not want to exacerbate, or newly available memory,
	// which XAT can allocate non-forcibly soon enough.

	if (now_ticks < lastfree_ticks + one_second) {
		if (calling_vmp->vm_kstat.vk_threads_waiting.value.ui64 < max_arena_waiters) {
			alloc_bytes_in_flight -= size;
			return (NULL);
		}
	}

	// if now is after the appropriate threshold below, allocate

	if ((vmflags & VM_PUSHPAGE) &&
	    (now_ticks > last_success_ticks + pushpage_after ||
		recently_allocated < recent_threshold)) {
		last_forced_ticks = now_ticks;
		recently_allocated += size;
		void *push_m = spl_vmem_malloc_unconditionally(size);
		alloc_bytes_in_flight -= size;
		atomic_inc_64(&spl_xat_forced);
		return (push_m);
	} else if (now_ticks > last_success_ticks + non_pushpage_after ||
	    recently_allocated < recent_threshold) {
		last_forced_ticks = now_ticks;
		recently_allocated += size;
		void *norm_m = spl_vmem_malloc_unconditionally(size);
		alloc_bytes_in_flight -= size;
		atomic_inc_64(&spl_xat_forced);
		return (norm_m);
	}

	// now is not after the threshold, bail
	alloc_bytes_in_flight -= size;
	return (NULL);
}

static void *
xnu_alloc_throttled(vmem_t *null_vmp, size_t size, int vmflag)
{

	// the caller is one of the bucket arenas.
	// null_vmp will be spl_default_arena_parent, which is just a placeholder.

	vmem_t *bvmp = vmem_bucket_arena_by_size(size);

	uint64_t now = zfs_lbolt();

	void *m = spl_vmem_malloc_if_no_pressure(size);

	if (m != NULL) {
		atomic_inc_64(&spl_xat_success);
		if (now > hz)
			atomic_swap_64(&spl_xat_lastalloc,  now / hz);
		// wake up waiters on all the arena condvars
		// since there is apparently no memory shortage.
		vmem_bucket_wake_all_waiters();
		return (m);
	} else {
		spl_free_set_emergency_pressure((int64_t)size);
	}

	if (vmflag & VM_PANIC) {
		// force an allocation now to avoid a panic
		atomic_swap_64(&spl_xat_lastalloc,  now / hz);
		spl_free_set_emergency_pressure(4LL * (int64_t)size);
		void *p = spl_vmem_malloc_unconditionally(size);
		// p cannot be NULL (unconditional kernel malloc always works or panics)
		// therefore: success, wake all waiters on alloc|free condvar
		// wake up arena waiters to let them know there is memory
		// available in the arena; let waiters on other bucket arenas
		// continue sleeping.
		cv_broadcast(&bvmp->vm_cv);
		return (p);
	}

	if (vmflag & VM_NOSLEEP) {
		spl_free_set_emergency_pressure(2LL * (int64_t)size);
		kpreempt(KPREEMPT_SYNC); /* cheating a bit, but not really waiting */
		void *p = spl_vmem_malloc_if_no_pressure(size);
		if (p != NULL) {
			atomic_inc_64(&spl_xat_late_success_nosleep);
			cv_broadcast(&bvmp->vm_cv);
			if (now > hz)
				atomic_swap_64(&spl_xat_lastalloc,  now / hz);
		}
		// if p == NULL, then there will be an increment in the fail kstat
		return (p);
	}

	/*
	 * Loop for a while trying to satisfy VM_SLEEP allocations.
	 *
	 * If we are able to allocate memory, then return the pointer.
	 *
	 * We return NULL if some other thread's activity has caused
	 * sufficient memory to appear in this arena that we can satisfy
	 * the allocation.
	 *
	 * We call xnu_allocate_throttle_bail() after a few milliseconds of waiting;
	 * it will either return a pointer to newly allocated memory or NULL.  We
	 * return the result.
	 *
	 */

	static volatile _Atomic uint32_t waiters = 0;

	waiters++;

	if (waiters == 1)
		atomic_inc_64(&spl_xat_no_waiters);

	for (int iter = 0; ; iter++) {
		clock_t wait_time = USEC2NSEC(500UL * MAX(waiters,1UL));
		mutex_enter(&bvmp->vm_lock);
		spl_xat_sleep++;
		(void) cv_timedwait_hires(&bvmp->vm_cv, &bvmp->vm_lock,
		    wait_time, 0, 0);
		// still under mutex
		now = zfs_lbolt();
		if (vmem_canalloc(bvmp, size)) {
			atomic_inc_64(&spl_xat_memory_appeared);
			// Memory has appeared thanks to another thread.
			// Wake up anything waiting on the arena cv in vmem_xalloc().
			waiters--;
			// Once we return, vmem_xalloc() will immediately
			// enter the mutex, and will quickly "goto do_alloc;".
			mutex_exit(&bvmp->vm_lock);
			return(NULL);
		} else {
			mutex_exit(&bvmp->vm_lock);
		}
		// We may be here because of a broadcast to &vmp->vm_cv,
		// causing xnu to schedule all the sleepers in priority-weighted FIFO
		// order.  Because of the mutex_exit(), the sections below here may
		// be entered concurrently.

		// spl_vmem_malloc_if_no_pressure does a mutex, so avoid calling it
		// unless there is a chance it will succeed.
		if (spl_vmem_xnu_useful_bytes_free() > (MAX(size,16ULL*1024ULL*1024ULL))) {
			void *a = spl_vmem_malloc_if_no_pressure(size);
			if (a != NULL) {
				atomic_inc_64(&spl_xat_late_success);
				atomic_swap_64(&spl_xat_lastalloc,  now / hz);
				waiters--;
				// Wake up all waiters on the bucket arena locks,
				// since the system apparently has memory again.
				vmem_bucket_wake_all_waiters();
				return (a);
			} else {
				// Probably vm_page_free_count changed while we were
				// in the mutex queue in spl_vmem_malloc_if_no_pressure().
				// There is therefore no point in doing the bail-out check
				// below, so go back to the top of the for loop.
				atomic_inc_64(&spl_xat_late_deny);
				continue;
			}
		}
		if ((iter >= 20 &&
			iter >= 20 * (int)waiters &&
			iter >= 40 * (int)bvmp->vm_kstat.vk_threads_waiting.value.ui64 &&
			iter >= 30 * (int)spl_vmem_threads_waiting) ||
		    bvmp->vm_kstat.vk_threads_waiting.value.ui64 >= 8) {
			// if there is a large cv_wait() queue already, or
			// after ~ 10 milliseconds (if no other waiters),
			// bail out and let vmem_xalloc() deal with it,
			// most likely by putting this thread into its cv_wait().
			//
			// We are VM_SLEEP here, so if there are many waiters,
			// it is OK to loop them a bit longer in hopes
			// that another thread frees to vmp or another thread
			// successfully imports memory into vmp.
			//
			// Any or all of waiters the arena and global _threads_waiting
			// may have changed during the cv_timedwait_hires at the top
			// of the for loop, including going to zero.
			//
			atomic_inc_64(&spl_xat_bailed);
			void *b = xnu_allocate_throttled_bail(now, bvmp, size, vmflag);
			if (b != NULL) {
				atomic_swap_64(&spl_xat_lastalloc, now / hz);
				// wake up waiters on the arena lock,
				// since they now have memory they can use.
				cv_broadcast(&bvmp->vm_cv);
			} else {
				const int64_t pressure =
				    size * (1LL + bvmp->vm_kstat.vk_threads_waiting.value.ui64);
				spl_free_set_pressure(pressure);
			}
			// open turnstile after having bailed, rather than before
			waiters--;
			return (b);
		} else if (iter == 20 || ((iter % 8) == 0 && iter > 20)) {
			spl_free_set_emergency_pressure(size * (iter / 8));
			atomic_inc_64(&spl_xat_pressured);
		}
	}
}

static void
xnu_free_throttled(vmem_t *vmp, void *vaddr, size_t size)
{
	extern void osif_free(void *, uint64_t);

	// Serialize behind a (short) spin-sleep delay, giving
	// xnu time to do freelist management and
	// PT teardowns

	// In the usual case there is only one thread in this function,
	// so we proceed waitlessly to osif_free().

	// When there are multiple threads here, we delay the 2nd and later.

	// Explict race:
	// The osif_free() is not protected by the vmem_xnu_alloc_free_lock
	// mutex; that is just used for implementing the delay.   Consequently,
	// the waiters on the same lock in spl_vmem_malloc_if_no_pressure may
	// falsely see too small a value for vm_page_free_count.   We don't
	// care in part because xnu performs poorly when doing
	// free-then-allocate anwyay.

	// a_waiters gauges the loop exit checking and sleep duration;
	// it is a count of the number of threads trying to do work
	// in this function.
	static volatile _Atomic uint32_t a_waiters = 0;

	// is_freeing protects the osif_free() call; see comment below
	static volatile _Atomic bool is_freeing = false;

	a_waiters++; // generates "lock incl ..."
	for (uint32_t iter = 0; a_waiters > 1; iter++) {
		// If are growing old in this loop, then see if
		// anyone else is still in osif_free.  If not, we can exit.
		if (iter > a_waiters && is_freeing == false) {
			is_freeing = true;
			break;
		}
		// There is a queue waiting for the mutex, sleep.
		mutex_enter(&vmem_xnu_alloc_free_lock);
		clock_t wait_time = USEC2NSEC(90UL + (10UL * MAX(a_waiters,1UL)));
		(void) cv_timedwait_hires(&vmem_xnu_alloc_free_cv,
		    &vmem_xnu_alloc_free_lock,
		    wait_time, 0, 0);
		// We may have been awakened by a cv_broadcast or by
		// a timeout.   If we are now the only waiter, we will
		// simply exit the loop after the mutex drop.
		// However, if we are in a gang of awakened waiters,
		// then only one may exit, and this is decided by xnu's
		// wake-and-get-mutex order, which is hignly fair.
		mutex_exit(&vmem_xnu_alloc_free_lock);
	}
	// If there is more than one thread in this function, osif_free() is
	// protected by is_freeing.   Release it after the osif_free()
	// call has been made and the lastfree bookkeeping has been done.
	osif_free(vaddr, size);
	uint64_t now = zfs_lbolt();
	atomic_swap_64(&spl_xat_lastfree,  now / hz);
	is_freeing = false;
	a_waiters--;
	// Schedule all other threads in this function.
	cv_broadcast(&vmem_xnu_alloc_free_cv);
	// since we just gave back xnu enough to satisfy an allocation
	// in at least the smaller buckets, let's wake up anyone in
	// the cv_wait() in vmem_xalloc([bucket_#], ...)
	vmem_bucket_wake_all_waiters();
}

// return 0 if the bit was unset before the atomic OR.
static inline bool
vba_atomic_lock_bucket(volatile _Atomic uint16_t *bbap, uint16_t bucket_bit)
{

	// We use a test-and-set of the appropriate bit
	// in buckets_busy_allocating; if it was not set,
	// then break out of the loop.
	//
	// This compiles into an orl, cmpxchgw instruction pair.
	// the return from __c11_atomic_fetch_or() is the
	// previous value of buckets_busy_allocating.

	uint16_t prev = __c11_atomic_fetch_or(bbap, bucket_bit, __ATOMIC_SEQ_CST);
	if (prev & bucket_bit)
		return (false); // we did not acquire the bit lock here
	else
		return (true); // we turned the bit from 0 to 1
}

static void *
vmem_bucket_alloc(vmem_t *null_vmp, size_t size, const int vmflags)
{

	// caller is spl_heap_arena looking for memory.
	// null_vmp will be spl_default_arena_parent, and so is just a placeholder.

	vmem_t *calling_arena = spl_heap_arena;

	if (!ISP2(size))
		atomic_inc_64(&spl_bucket_non_pow2_allocs);

	vmem_t *bvmp = vmem_bucket_arena_by_size(size);

	// there are 13 buckets, so use a 16-bit scalar to hold
	// a set of bits, where each bit corresponds to an in-progress
	// vmem_alloc(bucket, ...) below.

	static volatile _Atomic uint16_t buckets_busy_allocating = 0;
	const uint16_t bucket_bit = (uint16_t)1 << vmem_bucket_number(size);

	static volatile _Atomic uint32_t waiters = 0;

	// spin-sleep: if we would need to go to the xnu allocator.
	//
	// We want to avoid a burst of allocs from bucket_heap's children
	// successively hitting a low-memory condition, or alternatively
	// each successfully importing memory from xnu when they can share
	// a single import.
	//
	// We also want to take advantage of any memory that becomes available
	// in bucket_heap.
	//
	// If there is more than one thread in this function (~ few percent)
	// then the subsequent threads are put into the loop below.   They
	// can escape the loop if they are [1]non-waiting allocations, or
	// [2]when there is uncontended free memory in the calling bucket,
	// [3]if they become the only waiting thread, or
	// [4]if the cv_timedwait_hires returns -1 (which represents EWOULDBLOCK
	// from msleep() which gets it from _sleep()'s THREAD_TIMED_OUT)
	// allocating in the bucket, or if this thread has (highly improbably!) spent
	// a quarter of a second in the loop.

	if (waiters++ > 1) {
		atomic_inc_64(&spl_vba_loop_entries);
	}

	for (uint64_t loop_timeout = zfs_lbolt() + (hz/4), timedout = 0; waiters > 1; ) {
		// non-waiting allocations should proceeed to vmem_alloc() immediately
		if (vmflags & (VM_NOSLEEP | VM_PANIC | VM_ABORT)) {
			break;
		}
		if (vmem_canalloc_atomic(bvmp, size)) {
			// We can probably vmem_alloc(bvmp, size, vmflags).
			// At worst case it will give us a NULL and we will
			// end up on the vmp's cv_wait.
			//
			// We can have threads with different bvmp
			// taking this exit, and will proceed concurrently.
			//
			// However, we should protect against a burst of
			// callers hitting the same bvmp before the allocation
			// results are reflected in vmem_canalloc_atomic(bvmp, ...)
			if (vba_atomic_lock_bucket(&buckets_busy_allocating, bucket_bit)) {
				atomic_inc_64(&spl_vba_parent_memory_appeared);
				break;
				// The vmem_alloc() should return extremely quickly from
				// an INSTANTFIT allocation that canalloc predicts will succeed.
			} else {
				// another thread is trying to use the free memory in the
				// bucket_## arena; there might still be free memory there after
				// its allocation is completed, and there might be excess in the
				// bucket_heap arena, so stick around in this loop.
				atomic_inc_64(&spl_vba_parent_memory_blocked);
			}
		}
		if (timedout > 0) {
			if (vba_atomic_lock_bucket(&buckets_busy_allocating, bucket_bit)) {
				if (timedout & 1)
					atomic_inc_64(&spl_vba_cv_timeout);
				if (timedout & 2 || zfs_lbolt() >= loop_timeout)
					atomic_inc_64(&spl_vba_loop_timeout);
				break;
			} else {
				if (timedout & 1)
					atomic_inc_64(&spl_vba_cv_timeout_blocked);
				if (timedout & 2)
					atomic_inc_64(&spl_vba_loop_timeout_blocked);
			}
		}
		// The bucket is already allocating, or the bucket needs
		// more memory to satisfy vmem_allocat(bvmp, size, VM_NOSLEEP), or
		// we want to give the bucket some time to acquire more memory.
		//
		// substitute for the vmp arena's cv_wait in vmem_xalloc()
		// (vmp is the bucket_heap AKA spl_heap_arena)
		mutex_enter(&calling_arena->vm_lock);
		spl_vba_sleep++;
		clock_t wait_time = MSEC2NSEC(30);
		if ((vmflags & VM_PUSHPAGE) == 0) {
			wait_time *= 2; // lower-priority allocs to back of queue
		}
		int ret = cv_timedwait_hires(&calling_arena->vm_cv, &calling_arena->vm_lock,
		    wait_time, 0, 0);
		// We almost certainly have exited because of a signal/broadcast,
		// but maybe just timed out.  Either way, recheck memory.
		if (vmem_canalloc(calling_arena, size)) {
			// Another thread may has caused memory to become
			// available in our own arena; when we have returned to
			// vmem_xalloc(), it will almost certainly do a "goto do_alloc;",
			// and at worst we end up in the arena cv_wait()
			// with kstat.vmem.vmem.bucket_heap.wait incremented.
			// However, we don't want to wake up other sleepers
			// on vmp->vm_cv, as we don't know at this point whether
			// they would make any useful progress.
			spl_vba_memory_appeared++; // under mutex and before lock decw, so normal inc.
			waiters--;
			mutex_exit(&calling_arena->vm_lock);
			return(NULL);
		}
		mutex_exit(&calling_arena->vm_lock);
		if (ret == -1) {
			timedout |= 1;
		} else if (timedout == 0) {
			if (zfs_lbolt() >= loop_timeout)
				timedout |= 2;
		}
	}

	/*
	 * Turn on the exclusion bit in buckets_busy_allocating, to
	 * prevent multiple threads from calling vmem_alloc() on the
	 * same bucket arena concurrently rather than serially.
	 *
	 * This principally reduces the liklihood of asking xnu for
	 * more memory when other memory is or becomes available.
	 *
	 * This exclusion only applies to VM_SLEEP allocations;
	 * others (VM_PANIC, VM_NOSLEEP, VM_ABORT) will go to
	 * vmem_alloc() concurrently with any other threads.
	 *
	 * Since we aren't doing a test-and-set operation like above,
	 * we can just use |= and &= below and get correct atomic
	 * results, instead of using:
	 *
	 * __c11_atomic_fetch_or(&buckets_busy_allocating,
	 * bucket_bit, __ATOMIC_SEQ_CST);
	 * with the &= down below being written as
	 * __c11_atomic_fetch_and(&buckets_busy_allocating,
	 * ~bucket_bit, __ATOMIC_SEQ_CST);
	 *
	 * and this makes a difference with no optimization either
	 * compiling the whole file or with __attribute((optnone))
	 * in front of the function decl.   In particular, the non-
	 * optimized version that uses the builtin __c11_atomic_fetch_{and,or}
	 * preserves the C program order in the machine language output,
	 * inersting cmpxchgws, while all optimized versions, and the
	 * non-optimized version using the plainly-written version, reorder
	 * the "orw regr, memory" and "andw register, memory" (these are atomic
	 * RMW operations in x86-64 when the memory is naturally aligned) so that
	 * the strong memory model x86-64 promise that later loads see the
	 * results of earlier stores.
	 *
	 * clang+llvm simply are good at optimizing _Atomics and
	 * the optimized code differs only in line numbers and
	 * among all three approaches (as plainly written, using
	 * the __c11_atomic_fetch_{or,and} with sequential consistency,
	 * or when compiling with at least -O optimization so an
	 * atomic_or_16(&buckets_busy_allocating) built with GCC intrinsics
	 * is actually inlined rather than a function call).
	 *
	 */

	// in case we left the loop by being the only waiter, stop the
	// next thread arriving from leaving the for loop because
	// vmem_canalloc(bvmp, that_thread's_size) is true.

	buckets_busy_allocating |= bucket_bit;

	// There is memory in this bucket, or there are no other waiters,
	// or we aren't a VM_SLEEP allocation,  or we iterated out of the for loop.
	//
	// vmem_alloc() and vmem_xalloc() do their own mutex serializing
	// on bvmp->vm_lock, so we don't have to here.
	//
	// vmem_alloc may take some time to return (especially for VM_SLEEP
	// allocations where we did not take the vm_canalloc(bvmp...) break out
	// of the for loop).  Therefore, if we didn't enter the for loop at all
	// because waiters was 0 when we entered this function,
	// subsequent callers will enter the for loop.

	void *m = vmem_alloc(bvmp, size, vmflags);

	// allow another vmem_canalloc() through for this bucket
	// by atomically turning off the appropriate bit

	/*
	 * Except clang+llvm DTRT because of _Atomic, could be written as:
	 *__c11_atomic_fetch_and(&buckets_busy_allocating,
	 *~bucket_bit, __ATOMIC_SEQ_CST);
	 *
	 * On processors with more relaxed memory models, it might be
	 * more efficient to do so with release semantics here, and
	 * in the atomic |= above, with acquire semantics in the bit tests,
	 * but on the other hand it may be hard to do better than clang+llvm.
	 */

	buckets_busy_allocating &= ~bucket_bit;

	// if we got an allocation, wake up the arena cv waiters
	// to let them try to exit the for(;;) loop above and
	// exit the cv_wait() in vmem_xalloc(vmp, ...)

	if (m != NULL) {
		cv_broadcast(&calling_arena->vm_cv);
	}

	waiters--;
	return (m);
}

static void
vmem_bucket_free(vmem_t *null_vmp, void *vaddr, size_t size)
{
	vmem_t *calling_arena = spl_heap_arena;

	vmem_free(vmem_bucket_arena_by_size(size), vaddr, size);

	// wake up arena waiters to let them try an alloc
	cv_broadcast(&calling_arena->vm_cv);
}

static inline int64_t
vmem_bucket_arena_free(uint16_t bucket)
{
	VERIFY(bucket < VMEM_BUCKETS);
	return((int64_t)vmem_size_semi_atomic(vmem_bucket_arena[bucket], VMEM_FREE));
}

static inline int64_t
vmem_bucket_arena_used(int bucket)
{
	VERIFY(bucket < VMEM_BUCKETS);
	return((int64_t)vmem_size_semi_atomic(vmem_bucket_arena[bucket], VMEM_ALLOC));
}


int64_t
vmem_buckets_size(int typemask)
{
	int64_t total_size = 0;

	for (int i = 0; i < VMEM_BUCKETS; i++) {
		int64_t u = vmem_bucket_arena_used(i);
		int64_t f = vmem_bucket_arena_free(i);
		if (typemask & VMEM_ALLOC)
			total_size += u;
		if (typemask & VMEM_FREE)
			total_size += f;
	}
	if (total_size < 0)
		total_size = 0;

	return((size_t) total_size);
}

static uint64_t
spl_validate_bucket_span_size(uint64_t val)
{
	if (!ISP2(val)) {
		printf("SPL: %s: WARNING %llu is not a power of two, not changing.\n",
		    __func__, val);
		return (0);
	}
	if (val < 128ULL*1024ULL || val > 16ULL*1024ULL*1024ULL) {
		printf("SPL: %s: WARNING %llu is out of range [128k - 16M], not changing.\n",
		    __func__, val);
		return (0);
	}
	return (val);
}

static inline void
spl_modify_bucket_span_size(int bucket, uint64_t size)
{
	vmem_t *bvmp = vmem_bucket_arena[bucket];

	mutex_enter(&bvmp->vm_lock);
	bvmp->vm_min_import = size;
	mutex_exit(&bvmp->vm_lock);
}

static inline void
spl_modify_bucket_array()
{
	for (int i = VMEM_BUCKET_LOWBIT; i < VMEM_BUCKET_HIBIT; i++) {
		// i = 12, bucket = 0, contains allocs from 8192 to 16383 bytes,
		// and should never ask xnu for < 16384 bytes, so as to avoid
		// asking xnu for a non-power-of-two size.
		const int bucket = i - VMEM_BUCKET_LOWBIT;
		const uint32_t bucket_alloc_minimum_size = 1UL << (uint32_t)i;
		const uint32_t bucket_parent_alloc_minimum_size = bucket_alloc_minimum_size * 2UL;

		switch(i) {
			// see vmem_init() below for details
		case 16:
		case 17:
			spl_modify_bucket_span_size(bucket,
			    MAX(spl_bucket_tunable_small_span, bucket_parent_alloc_minimum_size));
			break;
		default:
			spl_modify_bucket_span_size(bucket,
			    MAX(spl_bucket_tunable_large_span, bucket_parent_alloc_minimum_size));
			break;
		}
	}
}

static inline void
spl_printf_bucket_span_sizes(void)
{
	// this doesn't have to be super-exact
	printf("SPL: %s: ", __func__);
	for (int i = VMEM_BUCKET_LOWBIT; i < VMEM_BUCKET_HIBIT; i++) {
		int bnum = i - VMEM_BUCKET_LOWBIT;
		vmem_t *bvmp = vmem_bucket_arena[bnum];
		printf("%llu ", (uint64_t)bvmp->vm_min_import);
	}
	printf("\n");
}

static inline void
spl_set_bucket_spans(uint64_t l, uint64_t s)
{
	if (spl_validate_bucket_span_size(l) &&
	    spl_validate_bucket_span_size(s)) {
		atomic_swap_64(&spl_bucket_tunable_large_span, l);
		atomic_swap_64(&spl_bucket_tunable_small_span, s);
		spl_modify_bucket_array();
	}
}

void
spl_set_bucket_tunable_large_span(uint64_t size)
{
	uint64_t s = 0;

	mutex_enter(&vmem_xnu_alloc_free_lock);
	atomic_swap_64(&s, spl_bucket_tunable_small_span);
	spl_set_bucket_spans(size, s);
	mutex_exit(&vmem_xnu_alloc_free_lock);

	spl_printf_bucket_span_sizes();
}

void
spl_set_bucket_tunable_small_span(uint64_t size)
{
	uint64_t l = 0;

	mutex_enter(&vmem_xnu_alloc_free_lock);
	atomic_swap_64(&l, spl_bucket_tunable_large_span);
	spl_set_bucket_spans(l, size);
	mutex_exit(&vmem_xnu_alloc_free_lock);

	spl_printf_bucket_span_sizes();
}

static void *
spl_vmem_default_alloc(vmem_t *vmp, size_t size, int vmflags)
{
	extern void *osif_malloc(uint64_t);
	return(osif_malloc(size));
}

static void
spl_vmem_default_free(vmem_t *vmp, void *vaddr, size_t size)
{
	extern void osif_free(void *, uint64_t);
	osif_free(vaddr, size);
}

vmem_t *
vmem_init(const char *heap_name,
		  void *heap_start, size_t heap_size, size_t heap_quantum,
		  void *(*heap_alloc)(vmem_t *, size_t, int),
		  void (*heap_free)(vmem_t *, void *, size_t))
{
	uint32_t id;
	int nseg = VMEM_SEG_INITIAL;
	vmem_t *heap;

	// XNU mutexes need initialisation
	mutex_init(&vmem_list_lock, "vmem_list_lock", MUTEX_DEFAULT, NULL);
	mutex_init(&vmem_segfree_lock, "vmem_segfree_lock", MUTEX_DEFAULT, NULL);
	mutex_init(&vmem_sleep_lock, "vmem_sleep_lock", MUTEX_DEFAULT, NULL);
	mutex_init(&vmem_nosleep_lock, "vmem_nosleep_lock", MUTEX_DEFAULT, NULL);
	mutex_init(&vmem_pushpage_lock, "vmem_pushpage_lock", MUTEX_DEFAULT, NULL);
	mutex_init(&vmem_panic_lock, "vmem_panic_lock", MUTEX_DEFAULT, NULL);

	mutex_init(&vmem_xnu_alloc_free_lock, "vmem_xnu_alloc_free_lock", MUTEX_DEFAULT, NULL);
	cv_init(&vmem_xnu_alloc_free_cv, "vmem_xnu_alloc_free_cv", CV_DEFAULT, NULL);

	while (--nseg >= 0)
		vmem_putseg_global(&vmem_seg0[nseg]);

	/*
	 * On OSX we ultimately have to use the OS allocator
	 * as the ource and sink of memory as it is allocated
	 * and freed.
	 *
	 * The spl_root_arena_parent is needed in order to provide a
	 * base arena with an always-NULL afunc and ffunc in order to
	 * end the searches done by vmem_[x]alloc and vm_xfree; it
	 * serves no other purpose; its stats will always be zero.
	 *
	 */

	spl_default_arena_parent = vmem_create("spl_default_arena_parent",  // id 0
	    NULL, 0, heap_quantum, NULL, NULL, NULL, 0, VM_SLEEP);

	// illumos/openzfs has a gigantic pile of memory that it can use for its first arena;
	// o3x is not so lucky, so we start with this

	static char initial_default_block[16ULL*1024ULL*1024ULL] __attribute__((aligned(4096))) = { 0 };

	// The default arena is very low-bandwidth; it supplies the initial large
	// allocation for the heap arena below, and it serves as the parent of the
	// vmem_metadata arena.   It will typically do only 2 or 3 parent_alloc calls
	// (to spl_vmem_default_alloc) in total.

	spl_default_arena = vmem_create("spl_default_arena", // id 1
	    initial_default_block, 16ULL*1024ULL*1024ULL,
	    heap_quantum, spl_vmem_default_alloc, spl_vmem_default_free,
	    spl_default_arena_parent, 16ULL*1024ULL*1024ULL, VM_SLEEP | VMC_POPULATOR | VMC_NO_QCACHE);

	// The bucket arenas satisfy allocations & frees from the bucket heap
	// that are dispatched to the bucket whose power-of-two label is the
	// smallest allocation that vmem_bucket_allocate will ask for.
	//
	// The bucket arenas in turn exchange memory with XNU's allocator/freer in
	// large spans (> 1MiB).
	//
	// Segregating by size constrains internal fragmentation within the bucket and
	// provides kstat.vmem visiblity and span-size policy to be applied to particular
	// buckets (notably the 128k one).
	//
	// For VMEM_BUCKET_HIBIT == 12,
	// vmem_bucket_arena[n] holds allocations from 2^[n+11]+1 to  2^[n+12],
	// so for [n] = 0, 2049-4096, for [n]=5 65537-131072, for [n]=7 (256k+1)-512k
	//
	// so "kstat.vmvm.vmem.bucket_1048576" should be read as the bucket arena containing
	// allocations 1 MiB and smaller, but larger than 512 kiB.

	// create arenas for the VMEM_BUCKETS, id 2 - id 14
	for (int32_t i = VMEM_BUCKET_LOWBIT; i <= VMEM_BUCKET_HIBIT; i++) {
		const uint64_t bucket_largest_size = (1ULL << (uint64_t)i);
		char *buf = vmem_alloc(spl_default_arena, VMEM_NAMELEN + 21, VM_SLEEP);
		(void) snprintf(buf, VMEM_NAMELEN + 20, "%s_%llu",
		    "bucket", bucket_largest_size);
		printf("SPL: %s creating arena %s (i == %d)\n", __func__, buf, i);
		extern uint64_t real_total_memory;
		if (real_total_memory > 0) {
			// adjust minimum bucket span size for memory size
			// we do not want to be smaller than 1 MiB
			const uint64_t k = 1024ULL;
			const uint64_t m = 1024ULL* k;
			const uint64_t b = MAX(real_total_memory / (k * 16ULL), m);
			const uint64_t s = MAX(b / 2ULL, m);
			spl_bucket_tunable_large_span = MIN(b, 16ULL * m);
			spl_bucket_tunable_small_span = s;
			printf("SPL: %s: real_total_memory %llu, large spans %llu, small spans %llu\n",
			    __func__, real_total_memory,
			    spl_bucket_tunable_large_span, spl_bucket_tunable_small_span);
		}
		size_t minimum_allocsize = 0;
		switch (i) {
		case 16:
		case 17:
			// The 128k bucket will generally be the most fragmented of all
			// because it is one step larger than the maximum qcache size
			// and is a popular size for the zio data and metadata arenas,
			// with a small amount of va heap use.
			//
			// These static factors and the dynamism of a busy zfs system lead
			// to interleaving 128k allocations with very different lifespans
			// into the same xnu allocation, and at 16MiB this tends to
			// leads to substantial (~ 1/2 GiB) waste.   That does not seem
			// like much memory for most sytems, but it tends to represent holding
			// ~ 4x more memory than the ideal case of 0 waste, thus the division by
			// four of minimum_allocate compared to the 16 MiB default.   Additionally,
			// this interleaving makes it harder to shrink the overall SPL memory
			// use when memory is cruicially low.
			//
			// The trade off here is contributing to the fragmentation of
			// the xnu freelist, so the step-down should not be even smaller.
			//
			// The 64k bucket is typically very low bandwidth and often holds just
			// one or two spans thanks to qcaching so does not need to be large.
			minimum_allocsize = MAX(spl_bucket_tunable_small_span,
			    bucket_largest_size * 4);
			break;
		default:
			// 16 MiB has proven to be a decent choice, with surprisingly
			// little waste.   The greatest waste has in practice been in
			// the 128k bucket (see above), the 256k bucket under unusual
			// circumstances (although this is much  smaller as a percentage,
			// and in particular less than 1/2, so a step change to 8 MiB would
			// not be very worthwhile), the 64k bucket (see above), and the
			// 2 MiB bucket (which will typically occupy only one span anyway).
			minimum_allocsize = MAX(spl_bucket_tunable_large_span,
			    bucket_largest_size * 4);
			break;
		}
		printf("SPL: %s setting bucket %d (%d) to size %llu\n",
		    __func__, i, (int)(1 << i), (uint64_t)minimum_allocsize);
		vmem_bucket_arena[i - VMEM_BUCKET_LOWBIT] =
		    vmem_create(buf, NULL, 0, heap_quantum,
			xnu_alloc_throttled, xnu_free_throttled, spl_default_arena_parent,
			minimum_allocsize, VM_SLEEP | VMC_POPULATOR | VMC_NO_QCACHE);
	}

	// spl_heap_arena, the bucket heap, is the primary interface to the vmem system

	// all arenas not rooted to vmem_metadata will be rooted to spl_heap arena.

	spl_heap_arena = vmem_create("bucket_heap", // id 15
	    NULL, 0, heap_quantum,
	    vmem_bucket_alloc, vmem_bucket_free, spl_default_arena_parent, 0,
	    VM_SLEEP);

	// add a fixed-sized allocation to spl_heap_arena; this reduces the
	// need to talk to the bucket arenas by a substantial margin
	// (kstat.vmem.vmem.bucket_heap.{alloc+free} is much greater than
	// kstat.vmem.vmem.bucket_heap.parent_{alloc+free}, and improves with
	// increasing initial fixed allocation size.

	const size_t mib = 1024ULL * 1024ULL;
	const size_t gib = 1024ULL * mib;
	size_t resv_size = 128ULL * mib;
	extern uint64_t real_total_memory;

	if (real_total_memory >= 4ULL * gib)
		resv_size = 256ULL * mib;
	if (real_total_memory >= 8ULL * gib)
		resv_size = 512ULL * mib;
	if (real_total_memory >= 16ULL * gib)
		resv_size = gib;

	printf("SPL: %s adding fixed allocation of %llu to the bucket_heap\n",
	    __func__, (uint64_t)resv_size);

	spl_heap_arena_initial_alloc =  vmem_add(spl_heap_arena,
	    vmem_alloc(spl_default_arena, resv_size, VM_SLEEP),
	    resv_size, VM_SLEEP);

	spl_heap_arena_initial_alloc_size = resv_size;

	// kstat.vmem.vmem.heap : kmem_cache_alloc() and similar calls
	// to handle in-memory datastructures other than arc and zio buffers.

	heap = vmem_create(heap_name,  // id 16
	    NULL, 0, heap_quantum,
	    vmem_alloc, vmem_free, spl_heap_arena, 0,
	    VM_SLEEP);

	// Root all the low bandwidth metadata arenas to the default arena.
	// The vmem_metadata allocations will all be 32 kiB or larger,
	// and the total allocation will generally cap off around 24 MiB.

	vmem_metadata_arena = vmem_create("vmem_metadata", // id 17
	    NULL, 0, heap_quantum, vmem_alloc, vmem_free, spl_default_arena,
	    8 * PAGESIZE, VM_SLEEP | VMC_POPULATOR | VMC_NO_QCACHE);

	vmem_seg_arena = vmem_create("vmem_seg", // id 18
								 NULL, 0, heap_quantum,
								 vmem_alloc, vmem_free, vmem_metadata_arena, 0,
								 VM_SLEEP | VMC_POPULATOR);

	vmem_hash_arena = vmem_create("vmem_hash", // id 19
								  NULL, 0, 8,
								  vmem_alloc, vmem_free, vmem_metadata_arena, 0,
								  VM_SLEEP);

	vmem_vmem_arena = vmem_create("vmem_vmem", // id 20
								  vmem0, sizeof (vmem0), 1,
								  vmem_alloc, vmem_free, vmem_metadata_arena, 0,
								  VM_SLEEP);

	// 21 (0-based) vmem_create before this line. - macroized NUMBER_OF_ARENAS_IN_VMEM_INIT
	for (id = 0; id < vmem_id; id++) {
	  (void) vmem_xalloc(vmem_vmem_arena, sizeof (vmem_t),
			     1, 0, 0, &vmem0[id], &vmem0[id + 1],
			     VM_NOSLEEP | VM_BESTFIT | VM_PANIC);
	}

	printf("SPL: starting vmem_update() thread\n");
	vmem_update(NULL);

	return (heap);
}

struct free_slab {
	vmem_t *vmp;
	size_t slabsize;
	void *slab;
	list_node_t next;
};
static list_t freelist;

static void vmem_fini_freelist(void *vmp, void *start, size_t size)
{
	struct free_slab *fs;

	MALLOC(fs, struct free_slab *, sizeof(struct free_slab), M_TEMP, M_WAITOK);
	fs->vmp = vmp;
	fs->slabsize = size;
	fs->slab = start;
	list_link_init(&fs->next);
	list_insert_tail(&freelist, fs);
}

static void vmem_fini_void(void *vmp, void *start, size_t size)
{
	return;
}

void
vmem_fini(vmem_t *heap)
{
	struct free_slab *fs;
	uint64_t total;

	bsd_untimeout(vmem_update, NULL);

	printf("SPL: %s: stopped vmem_update.  Creating list and walking arenas.\n", __func__);

	/* Create a list of slabs to free by walking the list of allocs */
	list_create(&freelist, sizeof (struct free_slab),
				offsetof(struct free_slab, next));

	/* Walk to list of allocations */

	// walking with VMEM_REENTRANT causes segment consolidation and freeing of spans
	// the freelist contains a list of segments that are still allocated
	// at the time of the walk; unfortunately the lists cannot be exact without
	// complex multiple passes, locking,  and a more complex vmem_fini_freelist().
	//
	// Walking withoutu VMEM_REENTRANT can produce a nearly-exact list of unfreed
	// spans, which Illumos would then free directly after the list is complete.
	//
	// Unfortunately in O3X, that lack of exactness can lead to a panic
	// caused by attempting to free to xnu memory that we already freed to xnu.
	// Fortunately, we can get a sense of what would have been destroyed
	// after the (non-reentrant) walking, and we printf that at the end of this function.

	// Walk all still-alive arenas from leaves to the root

	vmem_walk(heap, VMEM_ALLOC | VMEM_REENTRANT, vmem_fini_void, heap);

	vmem_walk(heap, VMEM_ALLOC, vmem_fini_freelist, heap);

	printf("\nSPL: %s destroying heap\n", __func__);
 	vmem_destroy(heap); // PARENT: spl_heap_arena

	printf("SPL: %s: walking spl_heap_arena, aka bucket_heap (pass 1)\n", __func__);

	vmem_walk(spl_heap_arena, VMEM_ALLOC | VMEM_REENTRANT, vmem_fini_void, spl_heap_arena);

	printf("SPL: %s: calling vmem_xfree(spl_default_arena, ptr, %llu);\n",
	    __func__, (uint64_t)spl_heap_arena_initial_alloc_size);

	// forcibly remove the initial alloc from spl_heap_arena arena, whether
	// or not it is empty.  below this point, any activity on spl_default_arena
	// other than a non-reentrant(!) walk and a destroy is unsafe (UAF or MAF).

	// However, all the children of spl_heap_arena should now be destroyed.

	vmem_xfree(spl_default_arena, spl_heap_arena_initial_alloc,
	    spl_heap_arena_initial_alloc_size);

	printf("SPL: %s: walking spl_heap_arena, aka bucket_heap (pass 2)\n", __func__);

	vmem_walk(spl_heap_arena, VMEM_ALLOC, vmem_fini_freelist, spl_heap_arena);

	printf("SPL: %s: walking bucket arenas...\n", __func__);

	for (int i = VMEM_BUCKET_LOWBIT; i <= VMEM_BUCKET_HIBIT; i++) {
		const int bucket = i - VMEM_BUCKET_LOWBIT;
		vmem_walk(vmem_bucket_arena[bucket], VMEM_ALLOC | VMEM_REENTRANT,
		    vmem_fini_void, vmem_bucket_arena[bucket]);

		vmem_walk(vmem_bucket_arena[bucket], VMEM_ALLOC,
		    vmem_fini_freelist, vmem_bucket_arena[bucket]);
	}

	printf("SPL: %s: walking vmem metadata-related arenas...\n", __func__);

	vmem_walk(vmem_vmem_arena, VMEM_ALLOC | VMEM_REENTRANT,
	    vmem_fini_void, vmem_vmem_arena);

	vmem_walk(vmem_vmem_arena, VMEM_ALLOC,
	    vmem_fini_freelist, vmem_vmem_arena);

	// We should not do VMEM_REENTRANT on vmem_seg_arena or vmem_hash_arena or below
	// to avoid causing work in vmem_seg_arena and vmem_hash_arena.

	vmem_walk(vmem_seg_arena, VMEM_ALLOC,
			  vmem_fini_freelist, vmem_seg_arena);

	vmem_walk(vmem_hash_arena, VMEM_ALLOC,
			  vmem_fini_freelist, vmem_hash_arena);

	vmem_walk(vmem_metadata_arena, VMEM_ALLOC,
	    vmem_fini_freelist, vmem_metadata_arena);

	printf("SPL: %s walking the root arena (spl_default_arena)...\n", __func__);

	vmem_walk(spl_default_arena, VMEM_ALLOC,
	    vmem_fini_freelist, spl_default_arena);

	printf("SPL: %s destroying bucket heap\n", __func__);
	vmem_destroy(spl_heap_arena); // PARENT: spl_default_arena_parent (but depends on buckets)

	printf("SPL: %s destroying spl_bucket_arenas...", __func__);
	for (int32_t i = VMEM_BUCKET_LOWBIT; i <= VMEM_BUCKET_HIBIT; i++) {
		vmem_t *vmpt = vmem_bucket_arena[i - VMEM_BUCKET_LOWBIT];
		printf(" %llu", (1ULL << i));
		vmem_destroy(vmpt); // parent: spl_default_arena_parent
	}
	printf("\n");

	// destroying the vmem_vmem arena and any arena afterwards
	// requires the use of vmem_destroy_internal(), which does
	// not talk to vmem_vmem_arena like vmem_destroy() does.
	printf("SPL: %s destroying vmem_vmem_arena\n", __func__);
	vmem_destroy_internal(vmem_vmem_arena); // parent: vmem_metadata_arena

	// destroying the seg arena means we must no longer
	// talk to vmem_populate()
	printf("SPL: %s destroying vmem_seg_arena\n", __func__);
	vmem_destroy_internal(vmem_seg_arena); // parent: vmem_metadata_arena

        // vmem_hash_arena may be freed-to in vmem_destroy_internal()
	// so it should be just before the vmem_metadata_arena.
	printf("SPL: %s destroying vmem_hash_arena\n", __func__);
	vmem_destroy_internal(vmem_hash_arena); // parent: vmem_metadata_arena
	vmem_hash_arena = NULL;

	// XXX: if we panic on unload below here due to destroyed mutex, vmem_init()
	//      will need some reworking (e.g. have vmem_metadata_arena talk directly
	//      to xnu), or alternatively a vmem_destroy_internal_internal()
	//      function that does not touch vmem_hash_arena will need writing.

	printf("SPL: %s destroying vmem_metadata_arena\n", __func__);
	vmem_destroy_internal(vmem_metadata_arena); // parent: spl_default_arena

	printf("\nSPL: %s destroying spl_default_arena\n", __func__);
	vmem_destroy_internal(spl_default_arena); // parent: spl_default_arena_parent

	printf("SPL: %s destroying spl_default_arena_parent\n", __func__);
	vmem_destroy_internal(spl_default_arena_parent); // parent: NONE

	printf("SPL: arenas removed, now try destroying mutexes... ");

	printf("vmem_xnu_alloc_free_lock ");
	mutex_destroy(&vmem_xnu_alloc_free_lock);
	printf("vmem_panic_lock ");
	mutex_destroy(&vmem_panic_lock);
	printf("vmem_pushpage_lock ");
	mutex_destroy(&vmem_pushpage_lock);
	printf("vmem_nosleep_lock ");
	mutex_destroy(&vmem_nosleep_lock);
	printf("vmem_sleep_lock ");
	mutex_destroy(&vmem_sleep_lock);
	printf("vmem_segfree_lock ");
	mutex_destroy(&vmem_segfree_lock);
	printf("vmem_list_lock ");
	mutex_destroy(&vmem_list_lock);

	printf("\nSPL: %s: walking list of live slabs at time of call to %s\n",
	       __func__, __func__);

	// annoyingly, some of these should be returned to xnu, but
	// we have no idea which have already been freed to xnu, and
	// freeing a second time results in a panic.

	/* Now release the list of allocs to built above */
	total = 0;
	uint64_t total_count = 0;
	while((fs = list_head(&freelist))) {
	        total_count++;
		total+=fs->slabsize;
		list_remove(&freelist, fs);
		//extern void segkmem_free(vmem_t *, void *, size_t);
		//segkmem_free(fs->vmp, fs->slab, fs->slabsize);
		FREE(fs, M_TEMP);
	}
	printf("SPL: WOULD HAVE released %llu bytes (%llu spans) from arenas\n",
	       total, total_count);
	list_destroy(&freelist);
	printf("SPL: %s: Brief delay for readability...\n", __func__);
	delay(hz);
	printf("SPL: %s: done!\n", __func__);
}
