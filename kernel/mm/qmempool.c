/*
 * qmempool - a quick queue based mempool
 *
 * Copyright (C) 2014, Red Hat, Inc., Jesper Dangaard Brouer
 *  for licensing details see kernel-base/COPYING
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/qmempool.h>

/* Global settings */

/* Due to hotplug CPU support, we need access to all qmempools
 * in-order to cleanup elements in localq for the CPU going offline.
 *
 * FIXME: NOT IMPLEMENTED this is just a prototype
#ifdef CONFIG_HOTPLUG_CPU
static LIST_HEAD(qmempool_list);
static DEFINE_SPINLOCK(qmempool_list_lock);
#endif
 */

/* True if x is a power of 2 */
#define POWEROF2(x) (((x) & ((x) - 1)) == 0)

/* Creating and destroying the qmempool
 */

void qmempool_destroy(struct qmempool *pool)
{
	void *elem = NULL;
	int j;

	if (pool->percpu) {
		for_each_possible_cpu(j) {
			struct qmempool_percpu *cpu =
				per_cpu_ptr(pool->percpu, j);

			while (ring_queue_dequeue(cpu->localq,
						  (void **)&elem) >= 0)
				kmem_cache_free(pool->kmem, elem);
			BUG_ON(!ring_queue_empty(cpu->localq));
			ring_queue_free(cpu->localq);
		}
		free_percpu(pool->percpu);
	}

	if (pool->sharedq) {
		while (ring_queue_dequeue(pool->sharedq, (void **)&elem) >= 0)
			kmem_cache_free(pool->kmem, elem);
		BUG_ON(!ring_queue_empty(pool->sharedq));
		ring_queue_free(pool->sharedq);
	}

	kfree(pool);
}
EXPORT_SYMBOL(qmempool_destroy);

struct qmempool*
qmempool_create(uint32_t localq_sz, uint32_t sharedq_sz, uint32_t prealloc,
		struct kmem_cache *kmem, gfp_t gfp_mask)
{
	struct qmempool* pool;
	int i, j, res;
	void *elem;

	/* Validate constraints, e.g. due to bulking */
	if (localq_sz < QMEMPOOL_BULK) {
		/* FIXME: perhaps <= because ring_queue 16 can only
		 * contain 15 elems */
		pr_err("%s() localq size(%d) too small for bulking\n",
		       __func__, localq_sz);
		return NULL;
	}
	if (sharedq_sz <= (QMEMPOOL_BULK * QMEMPOOL_REFILL_MULTIPLIER)) {
		/* Minimum sharedq size is 64 due to refill and return
		 * bulking needs sufficient space.  Due to ring_queue
		 * implementation size can only contain is its size
		 * minus 1, thus size 32 can only contain 31 elems
		 * which is to small for REFILL_MULTIPLIER(2) * BULK(16)
		 */
		pr_err("%s() sharedq size(%d) too small for bulk refill\n",
		       __func__, sharedq_sz);
		return NULL;
	}
	if (!POWEROF2(localq_sz) || !POWEROF2(sharedq_sz)) {
		pr_err("%s() queue sizes (%d/%d) must be power-of-2\n",
		       __func__, localq_sz, sharedq_sz);
		return NULL;
	}
	if (prealloc >= sharedq_sz) {
		/* ring_queue limit is its size minus 1 */
		pr_err("%s() prealloc(%d) req >= sharedq size(%d)\n",
		       __func__, prealloc, sharedq_sz);
		return NULL;
	}
	if ((prealloc % QMEMPOOL_BULK) != 0) {
		pr_warn("%s() prealloc(%d) should be div by BULK size(%d)\n",
			__func__, prealloc, QMEMPOOL_BULK);
	}
	if (!kmem) {
		pr_err("%s() kmem_cache is a NULL ptr\n",  __func__);
		return NULL;
	}

	pool = kzalloc(sizeof(*pool), gfp_mask);
	if (!pool)
		return NULL;
	pool->kmem     = kmem;
	pool->gfp_mask = gfp_mask;

	/* MPMC (Multi-Producer-Multi-Consumer) ring queue */
	pool->sharedq = ring_queue_create(sharedq_sz, 0);
	if (pool->sharedq == NULL) {
		pr_err("%s() failed to create shared queue(%d)\n",
		       __func__, sharedq_sz);
		qmempool_destroy(pool);
		return NULL;
	}

	pool->prealloc = prealloc;
	for (i = 0; i < prealloc; i++) {
		elem = kmem_cache_alloc(pool->kmem, gfp_mask);
		if (!elem) {
			pr_err("%s() kmem_cache out of memory?!\n",  __func__);
			qmempool_destroy(pool);
			return NULL;
		}
		res = ring_queue_enqueue(pool->sharedq, elem);
		BUG_ON(res < 0);
	}

	pool->percpu = alloc_percpu(struct qmempool_percpu);
	if (pool->percpu == NULL) {
		pr_err("%s() failed to alloc percpu\n", __func__);
		qmempool_destroy(pool);
		return NULL;
	}

	/* SPSC (Single-Consumer-Single-Producer) ring queue per CPU */
	for_each_possible_cpu(j) {
		struct qmempool_percpu *cpu = per_cpu_ptr(pool->percpu, j);

		cpu->localq = ring_queue_create(localq_sz,
						RING_F_SP_ENQ|RING_F_SC_DEQ);
		if (cpu->localq == NULL) {
			pr_err("%s() failed alloc localq on cpu:%d\n",
			       __func__, j);
			qmempool_destroy(pool);
			return NULL;
		}
		cpu->refill_cnt = 0;
		cpu->full_cnt = 0;
		cpu->owner_cpu = -1;
	}

	return pool;
}
EXPORT_SYMBOL(qmempool_create);

/* Element handling
 */

/* Debug hack to ease profiling functions when not inlined */
#ifdef QMEMPOOL_DEBUG_PROFILING
noinline void* qmempool_alloc(struct qmempool *pool, gfp_t gfp_mask)
{
       return __qmempool_alloc_node(pool, gfp_mask, 0);
}
EXPORT_SYMBOL(qmempool_alloc);

noinline void* qmempool_alloc_node(struct qmempool *pool, gfp_t gfp_mask, int node)
{
       return __qmempool_alloc_node(pool, gfp_mask, 0);
}
EXPORT_SYMBOL(qmempool_alloc_node);

noinline void qmempool_free(struct qmempool *pool, void *elem)
{
	return __qmempool_free(pool, elem);
}
EXPORT_SYMBOL(qmempool_free);
#endif

/* Assumed called with local_bh_disable() or preempt_disable() */
void * __qmempool_alloc_from_sharedq(struct qmempool *pool, gfp_t gfp_mask,
				   struct ring_queue *localq)
{
	void *elems[QMEMPOOL_BULK]; /* on stack variable */
	void *elem;
	int res;

	/* This function is called when the localq runs out-of
	 * elements.  Thus, the localq needs refilling */

	/* Costs atomic "cmpxchg", but amortize cost by bulk dequeue */
	res = ring_queue_mc_dequeue_bulk(pool->sharedq, elems, QMEMPOOL_BULK);
	// FIXME: should be "_burst" variant if e.g. only 15 elements is left
	if (likely(res == 0)) {/* Success */
		/* FIXME: Consider prefetching data part of elements
		 * here, it should be an optimal place to hide memory
		 * prefetching.  Especially given the localq is known
		 * to be an empty FIFO which guarantees the order objs
		 * are accessed in.
		 */
		elem = elems[0]; /* extract one element */
		/* Refill localq */
		res = ring_queue_sp_enqueue_bulk(localq,
						 &elems[1],
						 QMEMPOOL_BULK-1);
		/* FIXME: localq should be empty, thus enqueue should
		 * succeed, but races can exists. This MUST be fixed,
		 * for now add an ASSERT, then users will notice this
		 * problem need handling/implemeting the hard-way ;-)
		 */
		BUG_ON(res < 0);

		return elem;
	}
	return NULL;
}
EXPORT_SYMBOL(__qmempool_alloc_from_sharedq);

void * __qmempool_alloc_from_slab(struct qmempool *pool, gfp_t gfp_mask)
{
	void *elems[QMEMPOOL_BULK]; /* on stack variable */
	void *elem;
	int res, i, j;
	/* This function is called when sharedq runs-out of elements.
	 * Thus, sharedq needs to be refilled from slab.
	 */

	/* TODO: Consider prefetching this elem */
	elem = kmem_cache_alloc(pool->kmem, gfp_mask);
	/* Note: This one extra alloc will "unalign" the
	 * number of elements in localq from BULK(16) setting.
	 */
	if (elem == NULL) {
		/* slab is depleted, no reason to call below allocs */
		pr_err("%s() slab is depleted returning NULL\n", __func__);
		WARN_ON(!elem);
		return NULL;
	}

	for (i = 0; i < QMEMPOOL_REFILL_MULTIPLIER; i++) {
		for (j = 0; j < QMEMPOOL_BULK; j++) {
			elems[j] =
				kmem_cache_alloc(pool->kmem, gfp_mask);
			// Handle if slab gives us NULL elem!
			if (elems[j] == NULL) {
				pr_err("%s() ARGH - slab returned NULL",
				       __func__);
				res = ring_queue_mp_enqueue_bulk(
					pool->sharedq, elems, j-1);
				BUG_ON(res < 0); //FIXME handle
				return elem;
			}
		}
		res = ring_queue_mp_enqueue_bulk(
			pool->sharedq, elems, QMEMPOOL_BULK);
		/* FIXME: There is a theoretical chance that multiple
		 * CPU enter here, refilling sharedq at the same time,
		 * thus we must handle "full" situation, for now die
		 * hard so some will need to fix this.
		 */
		BUG_ON(res < 0); /* sharedq should have room */
	}

	/* What about refilling localq ???  (guess it will happen on
	 * next cycle, but it will cost an extra cmpxchg).  One
	 * additional small cost for refilling localq here, would be
	 * that we have to call this_cpu_ptr() again as the CPU could
	 * have changed.
	 */

	return elem;
	//TODO: Add stat counter for this event
}
EXPORT_SYMBOL(__qmempool_alloc_from_slab);

bool __qmempool_free_to_slab(struct qmempool *pool, void **elems)
{
	int res, i, j;
	/* Called when sharedq is full, thus make room */

	/* free these elements for real */
	for (i = 0; i < QMEMPOOL_BULK; i++) {
		kmem_cache_free(pool->kmem, elems[i]);
	}

        //Q: is it wise to dealloc 48 elems to slab in one go?

	/* make enough room in sharedq for next round */
	for (i = 0; i < QMEMPOOL_REFILL_MULTIPLIER; i++) {
		res = ring_queue_mc_dequeue_bulk(
			pool->sharedq, elems, QMEMPOOL_BULK);
		BUG_ON(res < 0); /* could race, but sharedq should be full */
		for (j = 0; j < QMEMPOOL_BULK; j++) {
			kmem_cache_free(pool->kmem, elems[j]);
		}
	}
	return true;
}
EXPORT_SYMBOL(__qmempool_free_to_slab);

MODULE_DESCRIPTION("Quick queue based mempool (qmempool)");
MODULE_AUTHOR("Jesper Dangaard Brouer <netoptimizer@brouer.com>");
MODULE_LICENSE("GPL");