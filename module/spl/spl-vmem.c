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

#define	VMEM_INITIAL		13	/* early vmem arenas */
#define	VMEM_SEG_INITIAL	400
//200	/* early segments */

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
static kmutex_t vmem_flush_free_lock;
static vmem_t *vmem_list;
static vmem_t *vmem_metadata_arena;
static vmem_t *vmem_seg_arena;
static vmem_t *vmem_hash_arena;
static vmem_t *vmem_vmem_arena;
static vmem_t *free_arena;
vmem_t *spl_root_arena; // The bottom-most arena for SPL
static vmem_t *spl_root_arena_parent;
static void *spl_root_initial_reserve_import;
static  uint64_t spl_root_initial_reserve_import_size;
static vmem_t *spl_large_reserve_arena;
static void *spl_large_reserve_initial_allocation;
static size_t spl_large_reserve_initial_allocation_size;
#define NUMBER_OF_ARENAS_IN_VMEM_INIT 10
static vmem_t *heap_parent;
static struct timespec	vmem_update_interval	= {15, 0};	/* vmem_update() every 15 seconds */
static struct timespec  spl_root_refill_interval = {0, MSEC2NSEC(10)};   // spl_root_refill() every 10 ms
static struct timespec  spl_root_refill_interval_long = {0, MSEC2NSEC(100)};
static struct timespec  vmem_vacuum_thread_interval = {30, 0};
uint32_t vmem_mtbf;		/* mean time between failures [default: off] */
size_t vmem_seg_size = sizeof (vmem_seg_t);

uint64_t vmem_free_memory_recycled = 0;
uint64_t vmem_free_memory_released = 0;

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
	{ "populate_wait",	KSTAT_DATA_UINT64 },
	{ "populate_fail",	KSTAT_DATA_UINT64 },
	{ "contains",		KSTAT_DATA_UINT64 },
	{ "contains_search",	KSTAT_DATA_UINT64 },
	{ "parent_alloc",	KSTAT_DATA_UINT64 },
	{ "parent_xalloc",	KSTAT_DATA_UINT64 },
	{ "parent_free",	KSTAT_DATA_UINT64 },
	{ "nextfit_xalloc",	KSTAT_DATA_UINT64 },
	{ "nextfit_root_wait",	KSTAT_DATA_UINT64 },
};

// Warning, we know its 10 from inside of vmem_init() - marcroized
static void *global_vmem_reap[NUMBER_OF_ARENAS_IN_VMEM_INIT] = {NULL};


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

static uint64_t spl_minalloc = 1024ULL*1024ULL;
// number of allocations > minalloc
uint64_t spl_vmem_large_allocs = 0;

// stats for vmem_add_or_return_memory_if_space()
uint64_t spl_vmem_fallthrough_imported_bytes = 0;
uint64_t spl_vmem_fallthrough_returned_bytes = 0;
uint64_t spl_vmem_add_or_return_calls = 0;
uint64_t spl_vmem_add_or_return_fails = 0;

// stats for spl_root_allocator()
uint64_t spl_root_allocator_bytes_asked = 0;
uint64_t spl_root_allocator_calls = 0;
uint64_t spl_root_allocator_cv_timedwaits = 0;
uint64_t spl_root_allocator_wait_expired = 0;
uint64_t spl_vmem_large_bytes_unneeded = 0;
uint64_t spl_vmem_nonwait_satisfied_large = 0;
uint64_t spl_vmem_nonwait_then_large = 0;
uint64_t spl_vmem_nonwait_unsatisfied = 0;
uint64_t spl_vmem_nonwait_unsatisfied_bytes = 0;
uint64_t spl_vmem_nonwait_unsatisfied_large = 0;
uint64_t spl_vmem_nonwait_unsatisfied_large_bytes = 0;
uint64_t spl_vmem_nonwait_with_allocation = 0;
uint64_t spl_vmem_spl_root_allocator_outer = 0;
uint64_t spl_vmem_total_large_bytes_alloc = 0;
uint64_t spl_vmem_total_large_bytes_asked = 0;
uint64_t spl_vmem_wait_satisfied_large = 0;
uint64_t spl_vmem_wait_then_large = 0;
uint64_t spl_vmem_wait_with_allocation = 0;
uint64_t spl_vmem_wait_without_allocation = 0;
uint64_t spl_root_allocator_pressure_short_circuit = 0;
uint64_t spl_root_allocator_large_reserve = 0;
uint64_t spl_root_allocator_small_reserve = 0;


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
		vmp->vm_kstat.vk_search.value.ui64++;
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
				vmp->vm_kstat.vk_nextfit_xalloc.value.ui64++;
				return (vmem_xalloc(vmp, size, vmp->vm_quantum,
					0, 0, NULL, NULL, vmflag & (VM_KMFLAGS | VM_NEXTFIT)));
			}
			vmp->vm_kstat.vk_wait.value.ui64++;
			printf("SPL: %s: waiting for %lu sized alloc after full circle of  %s, other threads waiting = %llu.\n",
			    __func__, size, vmp->vm_name, spl_vmem_threads_waiting);
			atomic_inc_64(&spl_vmem_threads_waiting);
			cv_wait(&vmp->vm_cv, &vmp->vm_lock);
			if (spl_vmem_threads_waiting > 0)
				atomic_dec_64(&spl_vmem_threads_waiting);
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
		flist = lowbit(P2ALIGN(vmp->vm_freemap, 1UL << hb));
	
	return (flist);
}

static void *vmem_add_as_import(vmem_t *, void *, size_t, int);

/*
 * allocate the next largest power-of-two above size, if there is space to do so
 *    and return(NULL)
 * otherwise, allocate size exactly, if there is space to do so
 * otherwise, return NULL
 *
 * overallocating might be wasteful, but it makes negligible the odds of
 * vmem_canalloc_nomutex(vmp, size) going false
 */

static void *
vmem_add_or_return_memory_if_space(vmem_t *vmp, size_t size, int vmflags)
{

	//MUTEX NOT HELD ON ENTRY

	atomic_inc_64(&spl_vmem_add_or_return_calls);

	extern volatile unsigned int vm_page_free_count;
	extern volatile unsigned int vm_page_free_min;
	extern volatile unsigned int vm_page_speculative_count;
	extern volatile unsigned int vm_page_free_wanted;

	uint64_t bytes_free = (uint64_t)vm_page_free_count * (uint64_t)PAGESIZE;
	uint64_t bytes_min = (uint64_t)vm_page_free_min * (uint64_t)PAGESIZE;
	uint64_t bytes_spec = (uint64_t)vm_page_speculative_count * (uint64_t)PAGESIZE;

	uint64_t useful_free = bytes_free + (bytes_spec/2) - bytes_min;

	// next power of two higher than size

	uint64_t r = 0;
	uint64_t s = size;
	while (s >>= 1)
		r++;

	uint64_t size_wanted = 1 << (r+1);

	extern void *osif_malloc(uint64_t);
	const uint64_t minalloc = spl_minalloc;

	mutex_enter(&vmem_flush_free_lock);

	void *ret = NULL;

	if (!vm_page_free_wanted && size > minalloc && useful_free > size_wanted) {
		void *p1 = osif_malloc(size);
		void *p2 = osif_malloc(size);
		if (p1 != NULL) {
			vmem_add_as_import(vmp, p1, size, vmflags);
			atomic_add_64(&spl_vmem_fallthrough_imported_bytes, size);
			ret = NULL;
		}
		if (p2 != NULL) {
			atomic_add_64(&spl_vmem_fallthrough_returned_bytes, size);
			ret = p2;
		}
	} else if (!vm_page_free_wanted && useful_free > size) {
		void *p = osif_malloc(size);
		if (p != NULL)
			atomic_add_64(&spl_vmem_fallthrough_returned_bytes, size);
		ret = p;
	} else
		ret = NULL;

	if (ret == NULL)
		atomic_inc_64(&spl_vmem_add_or_return_fails);

	mutex_exit(&vmem_flush_free_lock);

	return(ret);
}

static inline int
vmem_canalloc_nomutex(vmem_t *vmp, size_t size)
{
	ASSERT(!MUTEX_HELD(&vmp->vm_lock));
	mutex_enter(&vmp->vm_lock);
	int r = vmem_canalloc(vmp, size);
	mutex_exit(&vmp->vm_lock);
	return (r);
}

/*
 * Top-level allocator for spl_root, replacing segkmem_alloc
 *
 * we do a cv_timedwait loop for a brief period, and if
 * vmem_canalloc_nomutex(vmp, size), return NULL
 * ("Our import failed, but another thread added sufficient free memory...")
 *
 * if we time out of the loop and have sufficient memory, then
 * we just osif_malloc(size) and return that
 *
 * Finally, we can return NULL and expect vmem_canalloc (not _nomutex!) will fail and hit
 * the cv_wait in vmem_xalloc (assuming not VM_NOSLEEP, VM_ABORT or VM_PANIC)
 */

static uint64_t vmem_flush_free_to_root();

static void *
spl_try_large_reserve_alloc(size_t size, int vmflags)
{
	// MUTEX_NOT_HELD !
	ASSERT(!MUTEX_HELD(&spl_large_reserve_arena->vm_lock));

	if (vmem_canalloc_nomutex(spl_large_reserve_arena, size))
		return (vmem_alloc(spl_large_reserve_arena, size, vmflags));
	else
		return (NULL);
}

static void *
spl_root_allocator(vmem_t *vmp, size_t size, int vmflags)
{
	// MUTEX NOT HELD ON ENTRY
	ASSERT(!MUTEX_HELD(&vmp->vm_lock));

	// xalloc invokes this with vmp->vm_source in the first argument
	// so it will be spl_root_arena_parent

	if (vmp == spl_root_arena_parent)
		vmp = spl_root_arena;
	else
		printf("SPL: %s WOAH! vmp is %s!\n",
		    __func__, vmp->vm_name);

	ASSERT(!MUTEX_HELD(&vmp->vm_lock));

	if (mutex_owner(&vmp->vm_lock) == curthread)
		printf("SPL: %s ERROR! I am holding the mutex lock for %s!\n",
		    __func__, vmp->vm_name);

	atomic_inc_64(&spl_root_allocator_calls);

	// try shortcut for all large allocations
	if (size > spl_minalloc) { // minalloc
		void *p = spl_try_large_reserve_alloc(size, vmflags);
		if (p != NULL) {
			atomic_inc_64(&spl_root_allocator_large_reserve);
			return(p);
		} else
			atomic_add_64(&spl_vmem_total_large_bytes_asked, size);
	}

	// if we are under memory pressure then check if we have space in large_reserve_alloc
	// and use it if there is.

	extern volatile unsigned int vm_page_free_wanted;
	if (vm_page_free_wanted) {
		void *p = spl_try_large_reserve_alloc(size, vmflags);
		if (p != NULL) {
			atomic_inc_64(&spl_root_allocator_pressure_short_circuit);
			return(p);
		}
	}

	// if we are under less heavy memory pressure AND
	// (we have ample large_reserve_alloc space OR if we are a small allocation)

	extern volatile unsigned int vm_page_free_count;
	extern volatile unsigned int vm_page_speculative_count;
	extern volatile unsigned int vm_page_free_min;
	// mimic logic in vmem_add_or_return_memory_if_space()
	unsigned int useful_pages_free = vm_page_free_count +
	    (vm_page_speculative_count/2) - vm_page_free_min;

	if (size / PAGESIZE >= useful_pages_free || size < spl_minalloc) { // minalloc
		unsigned int threshold = (unsigned int)(spl_root_initial_reserve_import_size / PAGESIZE) / 2;
		unsigned int nosleep_threshold = threshold + (threshold/2);
		unsigned int reserve_free = vmem_size(spl_large_reserve_arena, VMEM_FREE) / PAGESIZE;
		if (reserve_free >= threshold ||
		    ((vmflags & VM_NOSLEEP) && reserve_free >= nosleep_threshold)) {
			void *p = spl_try_large_reserve_alloc(size, vmflags);
			if (p != NULL) {
				if (size / PAGESIZE >= useful_pages_free)
					atomic_inc_64(&spl_root_allocator_pressure_short_circuit);
				if (size < spl_minalloc) // not else, as can be both
					atomic_inc_64(&spl_root_allocator_small_reserve);
				return(p);
			}
		}
	}

	atomic_add_64(&spl_root_allocator_bytes_asked, size);

	uint32_t pass = 0;
	uint64_t minalloc = spl_minalloc;

	while (1) {

		pass++;

		if (vmem_size(free_arena, VMEM_FREE) > 0 && !vmem_canalloc_nomutex(vmp, size))
			(void) vmem_flush_free_to_root();

		if (!(vmflags & (VM_NOSLEEP | VM_ABORT))) {

			atomic_inc_64(&spl_vmem_threads_waiting);

			uint64_t timenow = zfs_lbolt();
			const uint64_t five_seconds = 5ULL * (uint64_t)hz;
			const uint64_t half_second = 5ULL * (uint64_t)hz;
			const uint64_t sixty_seconds = 60ULL * (uint64_t)hz;
			uint64_t expire_after = timenow;

			if (pass == 1 && size > minalloc)
				expire_after += half_second;
			else if (size > minalloc)
				expire_after += five_seconds;
			else
				expire_after += sixty_seconds; // lots of time for the refill and vacuum threads

			for (uint64_t t = timenow; t < expire_after; t = zfs_lbolt()) {
				mutex_enter(&vmp->vm_lock);
				atomic_inc_64(&spl_root_allocator_cv_timedwaits);
				(void) cv_timedwait(&vmp->vm_cv, &vmp->vm_lock, expire_after);
				// we hold the mutex after cv_timedwait
				if (vmem_canalloc(vmp, size)) {
					if (spl_vmem_threads_waiting > 0)
						atomic_dec_64(&spl_vmem_threads_waiting);
					mutex_exit(&vmp->vm_lock);
					atomic_inc_64(&spl_vmem_wait_without_allocation);
					if (size > spl_minalloc) {
						atomic_inc_64(&spl_vmem_wait_satisfied_large);
						atomic_add_64(&spl_vmem_large_bytes_unneeded, size);
					}
					return (NULL);
				}
				mutex_exit(&vmp->vm_lock);
			}

			atomic_inc_64(&spl_root_allocator_wait_expired);

			if (spl_vmem_threads_waiting > 0)
				atomic_dec_64(&spl_vmem_threads_waiting);

			uint64_t time_elapsed = (zfs_lbolt() - timenow)/hz;
			if (size <= minalloc && !vmem_canalloc_nomutex(vmp, size))
				printf("SPL: WOAH %s timed out waiting for %llu sized alloc for %s after %llu seconds!\n",
				    __func__, (uint64_t)size, vmp->vm_name, time_elapsed);
		}

		if (vmem_canalloc_nomutex(vmp, size)) {
			if (size > minalloc)
				atomic_add_64(&spl_vmem_large_bytes_unneeded, size);
			if (vmflags & (VM_NOSLEEP | VM_ABORT))
				atomic_inc_64(&spl_vmem_nonwait_satisfied_large);
			else
				atomic_inc_64(&spl_vmem_wait_satisfied_large);
			return (NULL);
		}

		void *p = vmem_add_or_return_memory_if_space(vmp, size, vmflags);

		if (p != NULL) {
			if (vmflags & (VM_NOSLEEP | VM_ABORT)) {
				atomic_inc_64(&spl_vmem_nonwait_with_allocation);
				if (size > minalloc)
					atomic_add_64(&spl_vmem_nonwait_then_large, size);
			} else {
				atomic_inc_64(&spl_vmem_wait_with_allocation);
				if (size > minalloc)
					atomic_add_64(&spl_vmem_wait_then_large, size);
			}
			if (size > spl_minalloc)
				atomic_add_64(&spl_vmem_total_large_bytes_alloc, size);
			return (p);
		} else if (vmem_canalloc_nomutex(vmp, size)) {
			if (size > minalloc)
				atomic_add_64(&spl_vmem_large_bytes_unneeded, size);
			if (vmflags & (VM_NOSLEEP | VM_ABORT))
				atomic_inc_64(&spl_vmem_nonwait_satisfied_large);
			else
				atomic_inc_64(&spl_vmem_wait_satisfied_large);
			return (NULL);
		} else if (vmflags & (VM_ABORT | VM_NOSLEEP)) {
			atomic_inc_64(&spl_vmem_nonwait_unsatisfied);
			atomic_add_64(&spl_vmem_nonwait_unsatisfied_bytes, size);
			if (size > spl_minalloc) {
				atomic_inc_64(&spl_vmem_nonwait_unsatisfied_large);
				atomic_add_64(&spl_vmem_nonwait_unsatisfied_large_bytes, size);
			}
			return (NULL);
		} else {
			printf("SPL: %s still waiting for %llu for %s, flags = %u, pass = %u\n",
			    __func__, (uint64_t)size, vmp->vm_name, vmflags, pass);
			atomic_inc_64(&spl_vmem_spl_root_allocator_outer);
		}
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

	if (size > spl_minalloc)
		spl_vmem_large_allocs++;
	
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
			vmp->vm_kstat.vk_search.value.ui64++;
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
				vmp->vm_kstat.vk_parent_xalloc.value.ui64++;
				vaddr = ((vmem_ximport_t *)
						 vmp->vm_source_alloc)(vmp->vm_source,
											   &asize, align, vmflag & VM_KMFLAGS);
				//ASSERT(asize >= oasize);
				ASSERT(P2PHASE(asize,
							   vmp->vm_source->vm_quantum) == 0);
				ASSERT(!(vmp->vm_cflags & VMC_XALIGN) ||
				    IS_P2ALIGNED(vaddr, align));
			} else {
				vmp->vm_kstat.vk_parent_alloc.value.ui64++;
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
				if (asize > aneeded &&
					vmp->vm_source_free != NULL &&
					vmem_canalloc(vmp, aneeded)) {
					ASSERT(resv >=
					    VMEM_SEGS_PER_MIDDLE_ALLOC);
					xvaddr = vaddr;
					xsize = asize;
					goto do_alloc;
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
		vmp->vm_kstat.vk_wait.value.ui64++;
		if (size != spl_minalloc && spl_vmem_threads_waiting != 0) {
			printf("SPL: %s: vmem waiting for %lu sized alloc for %s, other threads waiting = %llu\n",
			    __func__, size, vmp->vm_name, spl_vmem_threads_waiting);
		}
		atomic_inc_64(&spl_vmem_threads_waiting);
		cv_wait(&vmp->vm_cv, &vmp->vm_lock);
		if (spl_vmem_threads_waiting > 0)
			atomic_dec_64(&spl_vmem_threads_waiting);
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
			vmp->vm_kstat.vk_parent_free.value.ui64++;
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

	if (size > spl_minalloc)
		spl_vmem_large_allocs++;
	
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

static void *
vmem_add_as_import(vmem_t *vmp, void *vaddr, size_t size, int vmflag)
{
	if (vaddr == NULL || size == 0)
		panic("vmem_add_as_import(%p, %p, %lu): bad arguments",
		    (void *)vmp, vaddr, size);

	ASSERT(!vmem_contains(vmp, vaddr, size));

	mutex_enter(&vmp->vm_lock);
	if (vmem_populate(vmp, vmflag))
		(void) vmem_span_create(vmp, vaddr, size, 1);
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
	uint64_t size = 0;
	
	if (typemask & VMEM_ALLOC)
		size += vmp->vm_kstat.vk_mem_inuse.value.ui64;
	if (typemask & VMEM_FREE)
		size += vmp->vm_kstat.vk_mem_total.value.ui64 -
		vmp->vm_kstat.vk_mem_inuse.value.ui64;
	return ((size_t)size);
}

size_t
spl_vmem_size(vmem_t *vmp, int typemask)
{
	return(vmem_size(vmp, typemask));
}

size_t
spl_free_arena_size(void)
{
	return(vmem_size(free_arena, VMEM_ALLOC | VMEM_FREE));
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
		vmp->vm_min_import =
		VMEM_QCACHE_SLABSIZE(nqcache << vmp->vm_qshift);
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
static uint64_t zfs_arc_c_min = 0;
static uint64_t spl_target_arc_c_min = 0;
static uint64_t original_zfs_arc_min = 0;

uint64_t
spl_arc_c_min_update(uint64_t cur_arc_c_min)
{
	atomic_swap_64(&zfs_arc_c_min, cur_arc_c_min);
	if (original_zfs_arc_min &&
	    spl_target_arc_c_min &&
	    spl_target_arc_c_min != cur_arc_c_min)
		return (spl_target_arc_c_min);
	else
		return (cur_arc_c_min);
}

uint64_t
spl_zfs_arc_min_set(uint64_t val)
{
	printf("SPL: %s, original_zfs_arc_min %llu -> %llu\n",
	    __func__, original_zfs_arc_min, val);
	atomic_swap_64(&original_zfs_arc_min, val);
	atomic_swap_64(&spl_target_arc_c_min, val);
	return (val);
}

static inline void
deflate_arc_c_min(uint64_t howmuch)
{
	if (spl_target_arc_c_min > howmuch + (1024ULL * 1024ULL * 1024ULL))
		atomic_sub_64(&spl_target_arc_c_min, howmuch);
}


static inline void
reset_arc_c_min(void)
{
	if(zfs_arc_c_min != original_zfs_arc_min)
		atomic_swap_64(&zfs_arc_c_min, original_zfs_arc_min);
}

static inline void
increment_arc_c_min(uint64_t howmuch)
{
	if (original_zfs_arc_min &&
	    spl_target_arc_c_min + howmuch >= original_zfs_arc_min)
		reset_arc_c_min();
	else if (original_zfs_arc_min)
		atomic_add_64(&spl_target_arc_c_min, howmuch);
}

void vmem_vacuum_free_arena(void);

static uint64_t vmem_add_a_gibibyte_to_spl_root_arena();

void
vmem_vacuum_thread(void *dummy)
{
	vmem_vacuum_free_arena();
	bsd_timeout(vmem_vacuum_thread, dummy, &vmem_vacuum_thread_interval);
}

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

	atomic_swap_64(&spl_vmem_threads_waiting, 0ULL);

	(void) bsd_timeout(vmem_update, dummy, &vmem_update_interval);
}

/*
 * refill thread: regularly pull in memory, if needed and possible
 */
void
spl_root_refill(void *dummy)
{

	boolean_t wait = false;
	const uint64_t onegig = 1024ULL*1024ULL*1024ULL;

	// amount pulled in by refill + initial amount allocated in vmem_init +
	// anything not recycled or released yet
	uint64_t sys_mem_in_use = spl_root_arena->vm_kstat.vk_mem_import.value.ui64 +
	    spl_large_reserve_arena->vm_kstat.vk_mem_total.value.ui64 +
	    free_arena->vm_kstat.vk_mem_import.value.ui64;

	uint64_t root_free = (uint64_t)vmem_size(spl_root_arena, VMEM_FREE);

	extern volatile unsigned int vm_page_free_wanted;
	extern volatile unsigned int vm_page_free_count;
	extern volatile unsigned int vm_page_speculative_count;
	extern volatile unsigned int vm_page_free_min;

	extern uint64_t real_total_memory;

	// grab memory if there are waiting threads
	// or if we there is lots of free xnu memory and < 1 GiB free in spl_root_arena
	// or if there is less than 1/8 of real_physmem in spl_root_arena

	uint32_t useful_free_pages = vm_page_free_count +
	    (vm_page_speculative_count/2) - vm_page_free_min;

	const uint32_t lots_free_pages = (onegig/PAGESIZE) + vm_page_free_min;
	const uint32_t threshold_pages = (onegig/PAGESIZE) * 100ULL / 95ULL;

	if (spl_vmem_threads_waiting > 0 ||
	    (root_free < onegig && !vm_page_free_wanted &&
		    useful_free_pages > lots_free_pages) ||
	    sys_mem_in_use < real_total_memory/8ULL) {
		const uint32_t minalloc = (uint32_t)spl_minalloc;

		uint64_t pages = vmem_add_a_gibibyte_to_spl_root_arena();

		wait = true;

		if (pages == 0) {
			spl_free_set_emergency_pressure(32LL * (int64_t)minalloc);
		} else if (pages < threshold_pages) {
			spl_free_set_emergency_pressure((int64_t)minalloc);
		} else if (pages >= threshold_pages) {
			// we can inflate rapidly
			wait = false;
		}
	}
	if (wait)
		bsd_timeout(spl_root_refill, dummy, &spl_root_refill_interval_long);
	else
		bsd_timeout(spl_root_refill, dummy, &spl_root_refill_interval);
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

static uint64_t vmem_vacuum_spl_root_arena();

/*
 * vmem_add_a_gibibyte_to_spl_root_arena()
 *
 * if we cannot recycle memory sitting in free_memory arena,
 * then we do our best (given instantaneous behaviour of xnu vm  memory)
 * to add minalloc-sized spans to spl_root_arena as imported spans
 * (so they can be freed dynamically and also appear in the import kstat)
 * until we reach 1 GiB.
 *
 * minalloc == 1GiB is fine for any system where the most common
 * recordsize/volblocksize <= 1 MiB.
 *
 * larger allocations will either (if possible) come directly from the huge span
 * added to spl_root early in vmem_init(), or via a call out from vmem_xalloc()
 * to code that if necessary adds a largeer allocation
 *
 * should minalloc be different? dynamic? run-time tunable?
 *
 */

static uint64_t
vmem_add_a_gibibyte_to_spl_root_arena()
{
	const uint64_t gibibyte = 1024ULL*1024ULL*1024ULL;
	const uint64_t pages = (gibibyte/PAGESIZE);
	const uint64_t minalloc = spl_minalloc;
	const uint64_t pages_per_alloc = minalloc/PAGESIZE;
	uint64_t allocs = (gibibyte/minalloc);

	extern void *osif_malloc(uint64_t);
	extern volatile unsigned int vm_page_free_wanted;
	extern volatile unsigned int vm_page_free_count;
	extern volatile unsigned int vm_page_free_min;

	// vacuum if we are fragmented (or have ample free in root)
	// fragmentation metric: 25% free space
	size_t rtotal = vmem_size(spl_root_arena, VMEM_ALLOC | VMEM_FREE);
	size_t rfree = vmem_size(spl_root_arena, VMEM_FREE);
	size_t rused = vmem_size(spl_root_arena, VMEM_ALLOC);

	if (rtotal > (2 * gibibyte) && rfree > (rtotal / 4))
		vmem_vacuum_spl_root_arena();
	else if (rtotal / 2 > rused)
		vmem_vacuum_spl_root_arena();

	// in critically low memory, just return anything in free_arena to XNU, as
	// below we can fight for XNU memory with other non-spl allocators.
	if (vm_page_free_wanted > 0 || vm_page_free_count <= vm_page_free_min) {
		vmem_vacuum_free_arena();
	}

	uint64_t recovered = vmem_flush_free_to_root();

	// if there is sufficient space, we can return
	// waiters have been awakened by the cv_broadcast
	// in vmem_freelist_insert<-vmem_span_create<-vmem_add_as_import
	// if they have all stopped waiting, then we can return what
	// was recovered by recycling free_arena memory

	if (recovered > minalloc &&
	    spl_vmem_threads_waiting == 0) {
		return(recovered/PAGESIZE);
	}

	if (recovered >= gibibyte) {
		return (recovered/PAGESIZE);
	}

	rtotal = vmem_size(spl_root_arena, VMEM_ALLOC | VMEM_FREE);
	rfree = vmem_size(spl_root_arena, VMEM_FREE);
	rused = vmem_size(spl_root_arena, VMEM_ALLOC);

	if (recovered > minalloc && // recovered is also < 1GiB
	    rtotal > (2ULL * gibibyte) &&
	    (rfree * 100ULL / rtotal) >= 16ULL) {
		// we will need less memory from XNU
		uint64_t minallocs_recovered = recovered / minalloc;
		if (allocs > minallocs_recovered)
			allocs -= minallocs_recovered; // won't underflow because of previous if->return
		else {
			return (recovered / PAGESIZE);
		}
	}

	if (recovered > minalloc) {
		if (vm_page_free_wanted > 0 || vm_page_free_count < (4 * vm_page_free_min))
			return (recovered/PAGESIZE);
	}

	for (uint64_t i = 0; i < allocs; i++) {
		void *a = osif_malloc(minalloc);
		if (a == NULL) {
			printf("SPL: WOAH! %s bailing out after only %llu pages for %s.\n",
			    __func__, i * pages_per_alloc, spl_root_arena->vm_name);
			spl_root_arena->vm_kstat.vk_fail.value.ui64++;
			return (i/pages_per_alloc);
		} else {
		        vmem_add_as_import(spl_root_arena, a, minalloc, VM_NOSLEEP);
			spl_root_arena->vm_kstat.vk_parent_alloc.value.ui64++;
		}
		if (vm_page_free_wanted > 0 || vm_page_free_count < (2 * vm_page_free_min)) {
			if (recovered == 0 && i == 0) {
				spl_root_arena->vm_kstat.vk_fail.value.ui64++;
				dprintf("SPL: %s NO MEMORY, bailing out, %s.\n",
				    __func__, spl_root_arena->vm_name);
			}
			return (i/pages_per_alloc);
		}
	}
	return (pages);
}

/*
 * Prepare vmem for use.
 */

static void
spl_segkmem_free_if_not_big(vmem_t *vmp, void *inaddr, size_t size)
{
	if (vmem_contains(spl_large_reserve_arena, inaddr, size)) {
		bzero(inaddr, size);
		vmem_free(spl_large_reserve_arena, inaddr, size);
		mutex_enter(&spl_large_reserve_arena->vm_lock);
		cv_broadcast(&spl_large_reserve_arena->vm_cv);
		mutex_exit(&spl_large_reserve_arena->vm_lock);
	} else {
		extern void segkmem_free(vmem_t *, void *, size_t);
		segkmem_free(vmp, inaddr, size);
	}
}

static void
vmem_free_transfer(vmem_t *source, void *inaddr, size_t size, vmem_t *destination)
{
	bzero(inaddr, size);
	vmem_add_as_import(destination, inaddr, size, VM_SLEEP);
}

static void
spl_root_arena_free_to_free_arena(vmem_t *vmp, void *inaddr, size_t size)
{
	if (vmem_contains(spl_large_reserve_arena, inaddr, size)) {
		bzero(inaddr, size);
		vmem_free(spl_large_reserve_arena, inaddr, size);
		mutex_enter(&spl_large_reserve_arena->vm_lock);
		cv_broadcast(&spl_large_reserve_arena->vm_cv);
		mutex_exit(&spl_large_reserve_arena->vm_lock);
	} else {
		vmem_free_transfer(vmp, inaddr, size, free_arena);
	}
}

static void
free_arena_free_to_spl_root(vmem_t *vmp, void *inaddr, size_t size)
{
	vmem_free_transfer(vmp, inaddr, size, spl_root_arena);
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
	mutex_init(&vmem_flush_free_lock, "vmem_flush_free_lock", MUTEX_DEFAULT, NULL);
	
	while (--nseg >= 0)
		vmem_putseg_global(&vmem_seg0[nseg]);
	
	//	printf("SPL: vmem_init-a: npop=%u\n", vmem_populators);
	
	/*
	 * On OSX we ultimately have to use the OS allocator
	 * as the source and sink of memory as it is allocated
	 * and freed.
	 *
	 * By adding the heap_parent arena that acts as a source
	 * for the actual heap, the heap accumulates allocations
	 * and statistics just as it would on Illumos, while
	 * using the heap_parent as a "source". The heap parent
	 * arena never internally allocates memory as
	 * heap_alloc (segkmem_alloc) and heap_free (segkmem_free)
	 * go direct to the OS.
	 */


	spl_root_arena_parent = vmem_create("spl_root_arena_parent",  // id 0
	    NULL, 0, heap_quantum, NULL, NULL, NULL, 0, VM_SLEEP);

#ifdef _KERNEL
	// first add 1/4 of real_total_memory in one alloc
	// TUNE ME!
	// this needs to be small enough not to be annoying, since
	// it will never be released back to the operating system,
	// but where it's large it will reduce freelist and tlb work
	// significantly, AND it will also offer enough space that
	// the (> minalloc) large path in vmem_xalloc will not be taken

	const uint64_t half_gibibyte = 512LL*1024ULL*1024ULL;
	extern uint64_t real_total_memory;
	spl_large_reserve_initial_allocation_size =
	    MAX(real_total_memory / 64, half_gibibyte);

	printf("SPL: %s: doing initial large allocation of %llu bytes\n",
	    __func__, (uint64_t)spl_large_reserve_initial_allocation_size);

	extern void *osif_malloc(uint64_t);
	spl_large_reserve_initial_allocation = osif_malloc(spl_large_reserve_initial_allocation_size);

	if (spl_large_reserve_initial_allocation == NULL) {
		panic("SPL: %s unable to make initial spl_large_reserve_arena allocation of %llu bytes\n",
		    __func__, (uint64_t)spl_large_reserve_initial_allocation_size);
	}

	spl_large_reserve_arena = vmem_create("spl_large_reserve_arena",  // id 1
	    spl_large_reserve_initial_allocation, spl_large_reserve_initial_allocation_size, heap_quantum,
	    NULL, NULL, spl_root_arena_parent, 0, VM_SLEEP | VMC_POPULATOR);

	printf("SPL: %s created spl_large_reserve_arena with %llu bytes.\n",
	    __func__, (uint64_t)spl_large_reserve_initial_allocation_size);

	spl_root_initial_reserve_import_size = half_gibibyte/16;
	spl_root_initial_reserve_import = vmem_alloc(spl_large_reserve_arena,
	    spl_root_initial_reserve_import_size, VM_PANIC | VM_NOSLEEP);

	if (spl_root_initial_reserve_import == NULL)
		panic("vmem_init: unable to vmem_alloc %llu bytes!", spl_root_initial_reserve_import_size);

	spl_root_arena = vmem_create("spl_root_arena", // id 2
	    spl_root_initial_reserve_import, spl_root_initial_reserve_import_size, heap_quantum,
	    spl_root_allocator, spl_root_arena_free_to_free_arena, spl_root_arena_parent, 0,
	    VM_SLEEP | VMC_POPULATOR);

	printf("SPL: %s created spl_root_arena.\n", __func__);

	extern void segkmem_free(vmem_t *, void *, size_t);
	free_arena = vmem_create("free_arena", // id 3
	    NULL, 0,
	    PAGESIZE, NULL, segkmem_free, NULL, 0, VM_SLEEP);

#else
	spl_large_reserve_arena = vmem_create("spl_large_reserve_arena", // id 1 (userland)
	    NULL, 0, heap_quantum, NULL, NULL, 0, VM_SLEEP);

	spl_root_arena = vmem_create("spl_root_arena", // id 2 (userland)
	    NULL, 0, heap_quantum,
	    segkmem_alloc, segkmem_free, spl_root_arena_parent, 0, VM_SLEEP | VMC_POPULATOR);

	free_arena = vmem_create("spl_free_arena",  // id 3 (userland)
	    NULL, 0, heap_quantum, NULL, NULL, 0, VM_SLEEP);
#endif

	heap_parent = vmem_create("heap_parent",  // id 4
							  NULL, 0, heap_quantum,
							  vmem_alloc, vmem_free, spl_root_arena, 0,
							  VM_SLEEP);
	
	heap = vmem_create(heap_name,  // id 5
					   NULL, 0, heap_quantum,
					   vmem_alloc, vmem_free, heap_parent, 0,
					   VM_SLEEP);
	
	
	// Root all the low bandwidth metadata arenas off heap_parent,
	// which is essentially the OS heap. This may give us some
	// chance of cleaning up in an orderly manner.
	vmem_metadata_arena = vmem_create("vmem_metadata", // id 6
									  NULL, 0, heap_quantum,
									  vmem_alloc, vmem_free, heap_parent, 8 * PAGESIZE,
									  VM_SLEEP | VMC_POPULATOR | VMC_NO_QCACHE);
	
	vmem_seg_arena = vmem_create("vmem_seg", // id 7
								 NULL, 0, heap_quantum,
								 vmem_alloc, vmem_free, vmem_metadata_arena, 0,
								 VM_SLEEP | VMC_POPULATOR);
	
	vmem_hash_arena = vmem_create("vmem_hash", // id 8
								  NULL, 0, 8,
								  vmem_alloc, vmem_free, vmem_metadata_arena, 0,
								  VM_SLEEP);
	
	vmem_vmem_arena = vmem_create("vmem_vmem", // id 9
								  vmem0, sizeof (vmem0), 1,
								  vmem_alloc, vmem_free, vmem_metadata_arena, 0,
								  VM_SLEEP);
	
	// 10 (0-based) vmem_create before this line. - macroized NUMBER_OF_ARENAS_IN_VMEM_INIT
	for (id = 0; id < vmem_id; id++) {
		global_vmem_reap[id] = vmem_xalloc(vmem_vmem_arena, sizeof (vmem_t),
										   1, 0, 0, &vmem0[id], &vmem0[id + 1],
										   VM_NOSLEEP | VM_BESTFIT | VM_PANIC);
	}

	printf("SPL: starting vmem_update() thread\n");
	vmem_update(NULL);
	printf("SPL: starting vmem_vacuum_thread() thread\n");
	vmem_vacuum_thread(NULL);
	
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

void
vmem_fini(vmem_t *heap)
{
	uint32_t id;
	struct free_slab *fs;
	uint64_t total;

	bsd_untimeout(spl_root_refill, NULL);
	bsd_untimeout(vmem_update, NULL);
	bsd_untimeout(vmem_vacuum_thread, NULL);
	
	/* Create a list of slabs to free by walking the list of allocs */
	list_create(&freelist, sizeof (struct free_slab),
				offsetof(struct free_slab, next));
	
	/* Walk to list of allocations */
	vmem_walk(vmem_seg_arena, VMEM_ALLOC,
			  vmem_fini_freelist, vmem_seg_arena);
	
	vmem_walk(vmem_hash_arena, VMEM_ALLOC,
			  vmem_fini_freelist, vmem_hash_arena);
	
	vmem_walk(heap, VMEM_ALLOC,
			  vmem_fini_freelist, heap);
	
	vmem_walk(heap_parent, VMEM_ALLOC,
			  vmem_fini_freelist, heap_parent);

	vmem_walk(spl_root_arena, VMEM_ALLOC,
	    vmem_fini_freelist, spl_root_arena);

	for (id = 0; id < NUMBER_OF_ARENAS_IN_VMEM_INIT; id++) {// From vmem_init, 10 vmem_create macroized
		vmem_xfree(vmem_vmem_arena, global_vmem_reap[id], sizeof (vmem_t));
	}
	
	/* Now release the list of allocs to built above */
	total = 0;
	while((fs = list_head(&freelist))) {
		total+=fs->slabsize;
		list_remove(&freelist, fs);
		extern void segkmem_free(vmem_t *, void *, size_t);
		segkmem_free(fs->vmp, fs->slab, fs->slabsize);
		FREE(fs, M_TEMP);
	}
	printf("SPL: Released %llu bytes from arenas\n", total);
	list_destroy(&freelist);

#ifdef _KERNEL
	extern void osif_free(void *, uint64_t);
	printf("SPL: %s freeing spl_large_reserve_initial_allocation of %llu bytes.\n",
	    __func__, (uint64_t)spl_large_reserve_initial_allocation_size);
	osif_free(spl_large_reserve_initial_allocation, spl_large_reserve_initial_allocation_size);
#endif
	
#if 0 // Don't release, panics
	mutex_destroy(&vmem_flush_free_lock);
	mutex_destroy(&vmem_panic_lock);
	mutex_destroy(&vmem_pushpage_lock);
	mutex_destroy(&vmem_nosleep_lock);
	mutex_destroy(&vmem_sleep_lock);
	mutex_destroy(&vmem_segfree_lock);
	mutex_destroy(&vmem_list_lock);
#endif
}

static void
vmem_vacuum_freelist(void *vmp, void *start, size_t size)
{
	//vmem_xfree(vmp, start, size);
}

void
vmem_vacuum_free_arena(void)
{
	if (free_arena == NULL)
		return;

	uint64_t start_total = free_arena->vm_kstat.vk_mem_total.value.ui64;

	if (start_total == 0)
		return;

	mutex_enter(&vmem_flush_free_lock);

	extern void segkmem_free(vmem_t *, void *, size_t);

	if (free_arena->vm_source_free != segkmem_free) {
		printf("SPL: %s ERROR: free_arena->vm_source_free should be %p, is %p\n",
		    __func__, segkmem_free, free_arena->vm_source_free);
		mutex_exit(&vmem_flush_free_lock);
		return;
	}

	vmem_walk(free_arena, VMEM_FREE | VMEM_REENTRANT, vmem_vacuum_freelist, free_arena);

	uint64_t end_total = free_arena->vm_kstat.vk_mem_total.value.ui64;
	uint64_t difference;
	if (end_total > start_total) {
		difference = end_total - start_total;
		printf("SPL: %s WOAH!, free_arena grew by %llu.\n",
		    __func__, difference);
	} else if (start_total > end_total) {
		difference = start_total - end_total;
		printf("SPL: %s released %llu bytes from free_arena.\n",
		    __func__, difference);
		vmem_free_memory_released += difference;
	}
	mutex_exit(&vmem_flush_free_lock);
}

static uint64_t
vmem_flush_free_to_root()
{
	uint64_t start_total = free_arena->vm_kstat.vk_mem_total.value.ui64;

	if (start_total == 0)
		return(0);

	mutex_enter(&vmem_flush_free_lock);

	dprintf("SPL: %s starting, current free_arena == %llu\n",
	    __func__, start_total);

	mutex_enter(&spl_root_arena->vm_lock);
	mutex_enter(&free_arena->vm_lock);

	spl_root_arena->vm_source_free = NULL;

	free_arena->vm_source_free = free_arena_free_to_spl_root;

	mutex_exit(&free_arena->vm_lock);
	mutex_exit(&spl_root_arena->vm_lock);

	dprintf("SPL: %s walking\n", __func__);
	vmem_walk(free_arena, VMEM_FREE | VMEM_REENTRANT, vmem_vacuum_freelist, free_arena);

	uint64_t end_total = free_arena->vm_kstat.vk_mem_total.value.ui64;

	dprintf("SPL: %s walked, current free arena == %llu\n",
	    __func__, end_total);

	mutex_enter(&free_arena->vm_lock);
	mutex_enter(&spl_root_arena->vm_lock);

	extern void segkmem_free(vmem_t *, void *, size_t);
	free_arena->vm_source_free = segkmem_free;

	spl_root_arena->vm_source_free = spl_root_arena_free_to_free_arena;

	mutex_exit(&spl_root_arena->vm_lock);
	mutex_exit(&free_arena->vm_lock);

	uint64_t difference;
	if (end_total > start_total) {
		difference = end_total - start_total;
		printf("SPL: %s WOAH!, free_arena grew by %llu.\n",
		    __func__, difference);
	} else if (start_total > end_total) {
		difference = start_total - end_total;
		dprintf("SPL: %s flushed  %llu bytes from free_arena back into spl_root_arena.\n",
		    __func__, difference);
		vmem_free_memory_recycled += difference;
		mutex_exit(&vmem_flush_free_lock);
		return (difference);
	}
	mutex_exit(&vmem_flush_free_lock);
	return (0);
}

static uint64_t
vmem_vacuum_spl_root_arena()
{
	uint64_t start_total = spl_root_arena->vm_kstat.vk_mem_total.value.ui64;

	if (start_total == 0)
		return(0);

	mutex_enter(&vmem_flush_free_lock);

	dprintf("SPL: %s starting, current spl_root_arena == %llu\n",
	    __func__, start_total);

	mutex_enter(&spl_root_arena->vm_lock);

	spl_root_arena->vm_source_free = spl_segkmem_free_if_not_big;

	mutex_exit(&spl_root_arena->vm_lock);

	dprintf("SPL: %s walking\n", __func__);
	vmem_walk(free_arena, VMEM_FREE | VMEM_REENTRANT, vmem_vacuum_freelist, spl_root_arena);

	uint64_t end_total = spl_root_arena->vm_kstat.vk_mem_total.value.ui64;

	dprintf("SPL: %s walked, current free arena == %llu\n",
	    __func__, end_total);

	mutex_enter(&free_arena->vm_lock);
	mutex_enter(&spl_root_arena->vm_lock);

	extern void segkmem_free(vmem_t *, void *, size_t);
	free_arena->vm_source_free = segkmem_free;

	spl_root_arena->vm_source_free = spl_root_arena_free_to_free_arena;

	mutex_exit(&spl_root_arena->vm_lock);
	mutex_exit(&free_arena->vm_lock);

	uint64_t difference;
	if (end_total > start_total) {
		difference = end_total - start_total;
		printf("SPL: %s WOAH!, spl_root_arena grew by %llu.\n",
		    __func__, difference);
	} else if (start_total > end_total) {
		difference = start_total - end_total;
		dprintf("SPL: %s released  %llu bytes from spl_root_arena.\n",
		    __func__, difference);
		vmem_free_memory_released += difference;
		mutex_exit(&vmem_flush_free_lock);
		return (difference);
	}
	mutex_exit(&vmem_flush_free_lock);
	return (0);
}
