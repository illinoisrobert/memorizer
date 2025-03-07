/*===-- LICENSE ------------------------------------------------------------===
 * Developed by:
 *
 *    Research Group of Professor Vikram Adve in the Department of Computer
 *    Science The University of Illinois at Urbana-Champaign
 *    http://web.engr.illinois.edu/~vadve/Home.html
 *
 * Copyright (c) 2015, Nathan Dautenhahn
 * Copyright (c) 2024, Board of Trustees of the University of Illinois
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 *===-----------------------------------------------------------------------===
 *
 *       Filename:  kobj_metadata.c
 *
 *    Description:  Metadata tracking for all kobject allocations. Includes
 *		    types for metadata as well as data structure
 *		    implementations.
 *
 *===-----------------------------------------------------------------------===
 */

#include <linux/gfp.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/memorizer.h>

#include "kobj_metadata.h"
#include "memorizer.h"
#include "stats.h"
#include "memalloc.h"

#define ALLOC_CODE_SHIFT 58
#define ALLOC_INDUCED_CODE (_AC(MEM_INDUCED, UL) << ALLOC_CODE_SHIFT)

/* atomic object counter */
static atomic_long_t global_kobj_id = ATOMIC_INIT(0);

/* RW Spinlock for access to table */
DEFINE_RWLOCK(lookup_tbl_rw_lock);

static struct lt_l3_tbl kobj_l3_tbl;
static struct lt_pid_tbl pid_tbl;

/* Emergency Pools for l1 + l2 pages */
#define NUM_EMERGENCY_PAGES 200
struct pages_pool {
	uintptr_t base; /* pointer to array of l1/l2 pages */
	size_t next; /* index of next available */
	size_t entries; /* number of entries to last page */
	size_t pg_size; /* size of object for indexing */
};

//int test_and_set_bit(unsigned long nr, volatile unsigned long *addr);
volatile unsigned long inlt;

/**
 * __klt_enter() - increment recursion counter for entry into memorizer
 *
 * The primary goal of this is to stop recursive handling of events. Memorizer
 * by design tracks two types of events: allocations and accesses. Effectively,
 * while tracking either type we do not want to re-enter and track memorizer
 * events that are sources from within memorizer. Yes this means we may not
 * track legitimate access of some types, but these are caused by memorizer and
 * we want to ignore them.
 */
static inline int __klt_enter(void)
{
	return test_and_set_bit_lock(0, &inlt);
}

static __always_inline void __klt_exit(void)
{
	return clear_bit_unlock(0, &inlt);
}

/**
 * get_pg_from_pool() --- get the next page from the pool
 *
 * @pool: the pool to get the next value
 *
 * desc: this should not care about the type, so the type info is put into the
 * pages_pool struct so that we can do pointer arithmetic to find the next
 * available entry. The pointer is going to be the next index * the size of the
 * object, which is set on initializing the pool.
 *
 */
uintptr_t get_pg_from_pool(struct pages_pool *pool)
{
	pr_info("Getting page from pool (%p). i=%d e=%d\n", (void *)pool->base,
		(int)pool->next, (int)pool->entries);
	if (pool->entries == pool->next)
		return 0;
	/* next * pg_size is the offset in bytes from the base of the pool */
	return (uintptr_t)(pool->base + (pool->next++ * pool->pg_size));
}

struct lt_l1_tbl l1_tbl_pool[NUM_EMERGENCY_PAGES];
struct pages_pool l1_tbl_reserve = {
	.base		= (uintptr_t)&l1_tbl_pool,
	.next		= 0,
	.entries	= NUM_EMERGENCY_PAGES,
	.pg_size	= sizeof(struct lt_l1_tbl),
};

struct lt_l2_tbl l2_tbl_pool[NUM_EMERGENCY_PAGES];
struct pages_pool l2_tbl_reserve = {
	.base		= (uintptr_t)&l2_tbl_pool,
	.next		= 0,
	.entries	= NUM_EMERGENCY_PAGES,
	.pg_size	= sizeof(struct lt_l2_tbl),
};

/**
 * tbl_get_l1_entry() --- get the l1 entry
 * @addr:	The address to lookup
 *
 * Typical table walk starting from top to bottom.
 *
 * Return: the return value is a pointer to the entry in the table, which means
 * it is a double pointer to the object pointed to by the region. To simplify
 * lookup and setting this returns a double pointer so access to both the entry
 * and the object in the entry can easily be obtained.
 */
static struct memorizer_kobj **tbl_get_l1_entry(uint64_t addr)
{
	struct memorizer_kobj **l1e;
	struct lt_l1_tbl **l2e;
	struct lt_l2_tbl **l3e;

	/* Do the lookup starting from the top */
	l3e = lt_l3_entry(&kobj_l3_tbl, addr);
	if (!*l3e)
		return NULL;
	l2e = lt_l2_entry(*l3e, addr);
	if (!*l2e)
		return NULL;
	l1e = lt_l1_entry(*l2e, addr);
	if (!*l1e)
		return NULL;
	return l1e;
}

/**
 * l1_alloc() --- allocate an l1 table
 */
static struct lt_l1_tbl *l1_alloc(void)
{
	struct lt_l1_tbl *l1_tbl;
	int i = 0;

	l1_tbl = memalloc(sizeof(struct lt_l1_tbl));
	if (!l1_tbl) {
		l1_tbl = (struct lt_l1_tbl *)get_pg_from_pool(&l1_tbl_reserve);
		if (!l1_tbl) {
			/* while in dev we want to print error and panic */
			print_stats((size_t)KERN_CRIT);
			panic("Failed to allocate L1 table for memorizer kobj\n");
		}
	}

	/* Zero out the memory */
	for (i = 0; i < LT_L1_ENTRIES; ++i)
		l1_tbl->kobj_ptrs[i] = 0;

	/* increment stats counter */
	track_l1_alloc();

	return l1_tbl;
}

/**
 * l2_alloc() - alloc level 2 table
 */
static struct lt_l2_tbl *l2_alloc(void)
{
	struct lt_l2_tbl *l2_tbl;
	int i = 0;

	l2_tbl = memalloc(sizeof(struct lt_l2_tbl));
	if (!l2_tbl) {
		l2_tbl = (struct lt_l2_tbl *)get_pg_from_pool(&l2_tbl_reserve);
		if (!l2_tbl) {
			print_stats((size_t)KERN_CRIT);
			panic("Failed to allocate L2 table for memorizer kobj\n");
		}
	}

	/* Zero out the memory */
	for (i = 0; i < LT_L2_ENTRIES; ++i)
		l2_tbl->l1_tbls[i] = 0;

	/* increment stats counter */
	track_l2_alloc();

	return l2_tbl;
}

/**
 * l2_entry_may_alloc() - get the l2 entry and alloc if needed
 * @l2_tbl:	pointer to the l2 table to look into
 * @addr:		Pointer of the addr to index into the table
 *
 * Check if the l1 table exists, if not allocate.
 */
static struct lt_l1_tbl **l2_entry_may_alloc(struct lt_l2_tbl *l2_tbl,
					     uintptr_t addr)
{
	struct lt_l1_tbl **l2e;
	l2e = lt_l2_entry(l2_tbl, addr);
	if (unlikely(!*l2e))
		*l2e = l1_alloc();
	return l2e;
}

/**
 * l3_entry_may_alloc() - get the l3 entry and alloc if needed
 * @addr:		Pointer of the addr to index into the table
 *
 * Check if the l2 table exists, if not allocate.
 */
static struct lt_l2_tbl **l3_entry_may_alloc(uintptr_t addr)
{
	struct lt_l2_tbl **l3e;
	l3e = lt_l3_entry(&kobj_l3_tbl, addr);
	if (unlikely(!*l3e))
		*l3e = l2_alloc();
	return l3e;
}

/**
 * tbl_get_l1_entry_may_alloc() --- get the l1 entry
 * @addr:	The address to lookup
 *
 * Typical table walk starting from top to bottom.
 *
 * Return: the return value is a pointer to the entry in the table, which means
 * it is a double pointer to the object pointed to by the region. To simplify
 * lookup and setting this returns a double pointer so access to both the entry
 * and the object in the entry can easily be obtained.
 */
struct memorizer_kobj **tbl_get_l1_entry_may_alloc(uint64_t addr)
{
	struct memorizer_kobj **l1e;
	struct lt_l1_tbl **l2e;
	struct lt_l2_tbl **l3e;

	/* Do the lookup starting from the top */
	l3e = l3_entry_may_alloc( addr);
	if (!*l3e)
		return NULL;
	l2e = l2_entry_may_alloc(*l3e, addr);
	if (!*l2e)
		return NULL;
	l1e = lt_l1_entry(*l2e, addr);
	return l1e;
}
/**
 *
 */
static bool is_tracked_obj(uintptr_t l1entry)
{
	return ((uint64_t)l1entry >> ALLOC_CODE_SHIFT) != (uint64_t)MEM_INDUCED;
}

/**
 * is_induced_obj() -
 *
 * Args:
 *   @addr: the virtual address to check
 *
 * Description:
 *	Return the code that is stored in the upper 5 bits of the pointer value.
 *	This is stored when we detect that we've had an induced allocation. A
 *	normally tracked allocation will have the value 0 and thus evaluate to
 *	false.
 */
bool is_induced_obj(uintptr_t addr)
{
	struct memorizer_kobj **l1e = tbl_get_l1_entry(addr);
	if (!l1e)
		return false;
	return ((uint64_t)*l1e >> ALLOC_CODE_SHIFT) == (uint64_t)MEM_INDUCED;
}

/**
 * lt_check_kobj() - search for @kobj in the tree
 * @kobj - value to search for.
 */
bool lt_check_kobj(const char *id, struct memorizer_kobj *kobj)
{
	int i3, i2, i1;
	for (i3 = 0; i3 < LT_L3_ENTRIES; i3++) {
		struct lt_l2_tbl *l2_tbl = kobj_l3_tbl.l2_tbls[i3];
		if (!l2_tbl)
			continue;
		for (i2 = 0; i2 < LT_L2_ENTRIES; i2++) {
			struct lt_l1_tbl *l1_tbl = l2_tbl->l1_tbls[i2];
			if (!l1_tbl)
				continue;
			for (i1 = 0; i1 < LT_L1_ENTRIES; i1++) {
				struct memorizer_kobj *p =
					l1_tbl->kobj_ptrs[i1];
				if (p == kobj) {
					pr_info("%s: kobj=%p i3=%d i2=%d i1=%d\n",
						id, kobj, i3, i2, i1);
					return true;
				}
			}
		}
	}
	return false;
}

/**
 * lt_remove_kobj() --- remove object from the table
 * @addr: pointer to the beginning of the object
 *
 * This code assumes that it will only ever get a remove from the beginning of
 * the kobj. TODO: check the beginning of the kobj to make sure.
 *
 * Return: the kobject at the location that was removed.
 */
struct memorizer_kobj *lt_remove_kobj(uintptr_t addr)
{
	struct memorizer_kobj **l1e, *kobj = NULL;

	/*
	 * Get the l1 entry for the addr, if there is no entry then we not only
	 * haven't tracked the object, but we also haven't allocated a l1 page
	 * for the particular address
	 */
	l1e = tbl_get_l1_entry(addr);
	if (!l1e)
		return NULL;

	/* Setup the return: if it is an induced object then no kobj exists */
	/* the code is in the most significant bits so shift and compare */
	if (is_tracked_obj((uintptr_t)*l1e)) {
		kobj = *l1e;
		if(verbose_warnings_enabled.value) {
			WARN(kobj->va_ptr != addr, "kobj(%p)->va_ptr(%p) != addr(%p); kobj->state(%d)\n",
				kobj, (void*)kobj->va_ptr, (void*)addr, kobj->state);
		} else {
			// TODO robadams@illinois.edu - figure out why this happens and delete this warning
			if(kobj->va_ptr != addr) {
				pr_warn("kobj(%p)->va_ptr(%p) != addr(%p); kobj->state(%d)\n",
					kobj, (void*)kobj->va_ptr, (void*)addr, kobj->state);
			}
		}
		
		if (kobj->state != KOBJ_STATE_ALLOCATED) {
			pr_err("kobj(%p)->state(%d) != KOBJ_STATE_ALLOCATED\n",
			       kobj, kobj->state);
			pr_err("kobj->va_ptr==%p\n", (void *)kobj->va_ptr);
			pr_err("addr=%p\n", (void *)addr);
			BUG();
		}
		klt_zero(kobj);
	} 

	return kobj;
}

inline struct memorizer_kobj *lt_get_kobj(uintptr_t addr)
{
	struct memorizer_kobj **l1e = tbl_get_l1_entry(addr);
	if (l1e && is_tracked_obj((uintptr_t)*l1e))
		return *l1e;
	return NULL;
}

/*
 * handle_overalpping_insert() -- hanlde the overlapping insert case
 * @addr:		the virtual address that is currently not vacant
 * @prev_addr:          `addr` from the immediately preceeding allocation
 * @new_kobj:           The l1 entry that will be inserted
 * @obj:                the current l1 entry 
 *
 * If a successful klt_insert is followed immediately by another klt_insert
 * of the same address, assume we have nested allocators (e.g.
 * `alloc_pages_exact` calls `__get_free_pages`.) Update the old and new
 * li entries appropriately.
 *
 * Otherwise, there are some missing free's, and it isn't clear what is causing them;
 * however, if we assume objects are allocated before use then the most recent
 * allocation will be viable for any writes to these regions so we remove the
 * previous entry and set up its free times with a special code denoting it was
 * evicted from the table in an erroneous fasion.
 */
static void noinline handle_overlapping_insert(uintptr_t addr,
					       uintptr_t prev_addr,
					       struct memorizer_kobj *new_kobj)
{
	unsigned long flags;
	struct memorizer_kobj *obj = lt_get_kobj(addr);

	/*
	 * If there is no current obj, or the current object is already
	 * marked free, there is no resolution required.
	 *
	 * If `free_index` is already set, that probably means
	 * that this is the Nth address of an allocation, and we
	 * updated `obj` when we saw the first address of this
	 * allocation.
	 */
	if ((!obj) || (!is_tracked_obj((uintptr_t)obj)))
		return;
	if (obj->free_index)
		return;
	if (obj->state != KOBJ_STATE_ALLOCATED) {
		pr_err("kobj(%p)->state(%x) != KOBJ_STATE_ALLOCATED\n", obj,
		       obj->state);
		BUG();
	}

	// klt_for_each_addr(obj->va_ptr, obj->va_ptr+obj->size, l1_i, l1e) {
	// 	if(*l1e == obj)  // paranoia
	// 		*l1e = 0;
	// }
	klt_zero(obj);

	/*
	 * If the value to be written is not an object, take care
	 * not to dereference it.
	 */
	if ((!new_kobj) || (!is_tracked_obj((uintptr_t)new_kobj))) {
		write_lock_irqsave(&obj->rwlock, flags);
		list_del(&obj->object_list);
		obj->free_index = get_index();
		obj->free_ip = MEM_INDUCED | 0xdeadbeef00000000;
		obj->state = KOBJ_STATE_FREED;
		list_add(&obj->object_list, &memorizer_object_freed_list);
		write_unlock_irqrestore(&obj->rwlock, flags);
		return;
	}

	/*
	 * TODO robadams@illinois.edu
	if(obj->alloc_type == new_kobj->alloc_type) {
		pr_info("memorizer: va_ptr re-allocated:\n  %p %p %p %s\n  %p %p %p %s\n",
			(void*)obj->va_ptr,
			(void*)obj->alloc_ip, (void*)obj->free_ip,
			alloc_type_str(obj->alloc_type),
			(void*)new_kobj->va_ptr,
			(void*)new_kobj->alloc_ip, (void*)new_kobj->free_ip,
			alloc_type_str(new_kobj->alloc_type));
	}
	*/

	write_lock_irqsave(&obj->rwlock, flags);
	list_del(&obj->object_list);
	obj->free_index = new_kobj->alloc_index;
	obj->state = KOBJ_STATE_FREED;

	/* 
	 * DANGER! Magic numbers ahead.
	 */
	if (addr == prev_addr) {
		/* This is a nested allocation */
		obj->free_ip = new_kobj->alloc_type | 0xfeed00000000;
	} else {
		/* The reason for this duplicate alloc is unknown */
		obj->free_ip = new_kobj->alloc_type | 0xdeadbeef00000000;
	}
	// TEMP robadams@illinois.edu
	// write_unlock_irqrestore(&obj->rwlock, flags);

	// write_lock_irqsave(&obj->rwlock, flags);
	list_add(&obj->object_list, &memorizer_object_freed_list);
	write_unlock_irqrestore(&obj->rwlock, flags);
}

/**
 * lt_insert_kobj() - insert kobject into the lookup table
 * @kobj:	pointer to the kobj to insert
 *
 * For each virtual address in the range of the kobj allocation set the l1 table
 * entry mapping for the virtual address to the kobj pointer. The function
 * starts by getting the l2 table from the global l3 table. If it doesn't exist
 * then allocates the table. The same goes for looking up the l1 table for the
 * given addr. Once the particular l1 table is obtained for the start addr of the
 * object, iterate through the table setting each entry of the object to the
 * given kobj pointer.
 */
static int __klt_insert(uintptr_t ptr, size_t size, uintptr_t metadata)
{
	struct memorizer_kobj **l1e;
	uint64_t l1_i = 0;
	uintptr_t addr;
	static uintptr_t prev_addr = 0;

	if (metadata && is_tracked_obj(metadata)) {
		BUG_ON(((struct memorizer_kobj *)metadata)->state !=
		       KOBJ_STATE_ALLOCATED);
	}

	klt_for_each_addr(ptr, ptr+size, l1_i, l1e) {
		/* get the pointer to the l1_entry for this addr byte */
		// struct memorizer_kobj **l1e = lt_l1_entry(*l2e, addr);
		/* If it is not null then we are double allocating */
		if (*l1e)
			handle_overlapping_insert(
				addr, prev_addr,
				(struct memorizer_kobj *)metadata);

		/* insert object pointer in the table for byte addr */
		*l1e = (struct memorizer_kobj *)metadata;
	}
	prev_addr = ptr;
	return 0;
}


/**
 * We create a unique label for each induced allocated object so that we can
 * easily free. We insert a 5 bit code for the type with the MSB as 0 to make
 * sure we don't have a false positive with a real address. We then make the 59
 * least significatn bits a unique identifier for this obj. By inserting this
 * way the free just finds all matching entries in the table.
 */
size_t d = 0;
int lt_insert_induced(void *ptr, size_t size)
{
	uintptr_t label = ((uintptr_t)MEM_INDUCED << ALLOC_CODE_SHIFT) |
			  atomic_long_inc_return(&global_kobj_id);
	__klt_insert((uintptr_t)ptr, size, label);
	return 1;
}

int lt_insert_kobj(struct memorizer_kobj *kobj)
{
	return __klt_insert(kobj->va_ptr, kobj->size, (uintptr_t)kobj);
}

void plt_insert(struct pid_obj pobj)
{
	// Insert into the PID Table based on the Key of the Object
	pid_tbl.pid_obj_list[pobj.key] = pobj;
}

void __init lt_init(void)
{
	/* Zero the page dir contents */
	memset(&kobj_l3_tbl, 0, sizeof(kobj_l3_tbl));
	// Zero Out the Contents of the PID Table
	memset(&pid_tbl, 0, sizeof(pid_tbl));
	/* track that we statically allocated an l3 */
	track_l3_alloc();
}
