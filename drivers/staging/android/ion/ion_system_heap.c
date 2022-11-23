/*
 * drivers/staging/android/ion/ion_system_heap.c
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (c) 2011-2020, The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <asm/page.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <linux/mmzone.h>
#include <linux/kthread.h>

#include <soc/qcom/secure_buffer.h>
#include "ion_system_heap.h"
#include "ion.h"
#include "ion_system_heap.h"
#include "ion_system_secure_heap.h"
#include "ion_secure_util.h"

#ifdef CONFIG_OPLUS_ION_BOOSTPOOL
#include <linux/proc_fs.h>
#include <linux/sizes.h>
#include "oplus_ion_boost_pool.h"
#endif /* CONFIG_OPLUS_ION_BOOSTPOOL */

#define MIN_ION_POOL_PAGES			51200 /* 200 MB Min ION pool size */
#define MIN_ION_POOL_PAGES_BOOTUP	102400 /* 400 MB each ION pool size during bootup */
static gfp_t high_order_gfp_flags = (GFP_HIGHUSER | __GFP_ZERO | __GFP_NOWARN |
				     __GFP_NORETRY) & ~__GFP_RECLAIM;
static gfp_t low_order_gfp_flags  = GFP_HIGHUSER | __GFP_ZERO;

static struct kmem_cache *ion_page_info_pool;

int order_to_index(unsigned int order)
{
	int i;

	for (i = 0; i < NUM_ORDERS; i++)
		if (order == orders[i])
			return i;
	BUG();
	return -1;
}

static inline unsigned int order_to_size(int order)
{
	return PAGE_SIZE << order;
}

struct pages_mem {
	struct page **pages;
	u32 size;
};

int ion_heap_is_system_heap_type(enum ion_heap_type type)
{
	return type == ((enum ion_heap_type)ION_HEAP_TYPE_SYSTEM);
}

static struct page *alloc_buffer_page(struct ion_system_heap *heap,
				      struct ion_buffer *buffer,
				      unsigned long order,
				      bool *from_pool)
{
	bool cached = ion_buffer_cached(buffer);
	struct page *page;
	struct ion_page_pool *pool;
	int vmid = get_secure_vmid(buffer->flags);
	struct device *dev = heap->heap.priv;

	if (vmid > 0)
		pool = heap->secure_pools[vmid][order_to_index(order)];
	else if (!cached)
		pool = heap->uncached_pools[order_to_index(order)];
	else
		pool = heap->cached_pools[order_to_index(order)];

	page = ion_page_pool_alloc(pool, from_pool);

	if (IS_ERR(page))
		return page;

	if ((MAKE_ION_ALLOC_DMA_READY && vmid <= 0) || !(*from_pool))
		ion_pages_sync_for_device(dev, page, PAGE_SIZE << order,
					  DMA_BIDIRECTIONAL);

	return page;
}

/*
 * For secure pages that need to be freed and not added back to the pool; the
 *  hyp_unassign should be called before calling this function
 */
void free_buffer_page(struct ion_system_heap *heap,
		      struct ion_buffer *buffer, struct page *page,
		      unsigned int order)
{
	bool cached = ion_buffer_cached(buffer);
	int vmid = get_secure_vmid(buffer->flags);

#ifdef CONFIG_OPLUS_ION_BOOSTPOOL
	struct ion_boost_pool *boost_pool = has_boost_pool(heap, buffer);

	if (boost_pool) {
		if (0 == boost_pool_free(boost_pool, page, order)) {
			mod_node_page_state(page_pgdat(page), NR_UNRECLAIMABLE_PAGES,
					    -(1 << order));
			return;
		}
	}
#endif

	if (!(buffer->flags & ION_FLAG_POOL_FORCE_ALLOC)) {
		struct ion_page_pool *pool;

		if (vmid > 0)
			pool = heap->secure_pools[vmid][order_to_index(order)];
		else if (cached)
			pool = heap->cached_pools[order_to_index(order)];
		else
			pool = heap->uncached_pools[order_to_index(order)];

		if (buffer->private_flags & ION_PRIV_FLAG_SHRINKER_FREE)
			ion_page_pool_free_immediate(pool, page);
		else
			ion_page_pool_free(pool, page);

		mod_node_page_state(page_pgdat(page), NR_UNRECLAIMABLE_PAGES,
				    -(1 << pool->order));
	} else {
		__free_pages(page, order);
		mod_node_page_state(page_pgdat(page), NR_UNRECLAIMABLE_PAGES,
				    -(1 << order));
	}
}

static int alloc_largest_available(struct page_info *info,
				   struct ion_system_heap *heap,
				   struct ion_buffer *buffer,
				   unsigned long size,
				   unsigned int max_order)
{
	struct page *page;
	int i;
	bool from_pool;

	for (i = 0; i < NUM_ORDERS; i++) {
		if (size < order_to_size(orders[i]))
			continue;
		if (max_order < orders[i])
			continue;
		from_pool = !(buffer->flags & ION_FLAG_POOL_FORCE_ALLOC);
		page = alloc_buffer_page(heap, buffer, orders[i], &from_pool);
		if (IS_ERR(page))
			continue;

		info->page = page;
		info->order = orders[i];
		info->from_pool = from_pool;
		INIT_LIST_HEAD(&info->list);
		return 0;
	}

	return -ENOMEM;
}

static int alloc_from_pool_preferred(struct page_info *info,
		struct ion_system_heap *heap, struct ion_buffer *buffer,
		unsigned long size, unsigned int max_order)
{
	struct page *page;
	int i;

	if (buffer->flags & ION_FLAG_POOL_FORCE_ALLOC)
		goto force_alloc;

	for (i = 0; i < NUM_ORDERS; i++) {
		if (size < order_to_size(orders[i]))
			continue;
		if (max_order < orders[i])
			continue;

		page = alloc_from_secure_pool_order(heap, buffer, orders[i]);
		if (IS_ERR(page))
			continue;

		info->page = page;
		info->order = orders[i];
		info->from_pool = true;
		INIT_LIST_HEAD(&info->list);
		return 0;
	}

	page = split_page_from_secure_pool(heap, buffer);
	if (!IS_ERR(page)) {
		info->page = page;
		info->order = 0;
		info->from_pool = true;
		INIT_LIST_HEAD(&info->list);
		return 0;
	}

force_alloc:
	return alloc_largest_available(info, heap, buffer, size, max_order);
}

static unsigned int process_info(struct page_info *info,
				 struct scatterlist *sg,
				 struct scatterlist *sg_sync,
				 struct pages_mem *data, unsigned int i)
{
	struct page *page = info->page;
	unsigned int j;

	if (sg_sync) {
		sg_set_page(sg_sync, page, (1 << info->order) * PAGE_SIZE, 0);
		sg_dma_address(sg_sync) = page_to_phys(page);
	}
	sg_set_page(sg, page, (1 << info->order) * PAGE_SIZE, 0);
	/*
	 * This is not correct - sg_dma_address needs a dma_addr_t
	 * that is valid for the the targeted device, but this works
	 * on the currently targeted hardware.
	 */
	sg_dma_address(sg) = page_to_phys(page);
	if (data) {
		for (j = 0; j < (1 << info->order); ++j)
			data->pages[i++] = nth_page(page, j);
	}
	list_del(&info->list);
	return i;
}

static void free_info(struct page_info *info, struct page_info *info_onstack,
		      size_t onstack_len)
{
	if (info < info_onstack || info > &info_onstack[onstack_len - 1])
		kmem_cache_free(ion_page_info_pool, info);
}

static int ion_heap_alloc_pages_mem(struct pages_mem *pages_mem)
{
	struct page **pages;
	unsigned int page_tbl_size;

	page_tbl_size = sizeof(struct page *) * (pages_mem->size >> PAGE_SHIFT);
	if (page_tbl_size > SZ_8K) {
		/*
		 * Do fallback to ensure we have a balance between
		 * performance and availability.
		 */
		pages = kmalloc(page_tbl_size,
				__GFP_COMP | __GFP_NORETRY |
				__GFP_NOWARN);
		if (!pages)
			pages = vmalloc(page_tbl_size);
	} else {
		pages = kmalloc(page_tbl_size, GFP_KERNEL);
	}

	if (!pages)
		return -ENOMEM;

	pages_mem->pages = pages;
	return 0;
}

static void ion_heap_free_pages_mem(struct pages_mem *pages_mem)
{
	kvfree(pages_mem->pages);
}

static int ion_system_heap_allocate(struct ion_heap *heap,
				    struct ion_buffer *buffer,
				    unsigned long size,
				    unsigned long flags)
{
	struct ion_system_heap *sys_heap = container_of(heap,
							struct ion_system_heap,
							heap);
	struct sg_table *table;
	struct sg_table table_sync = {0};
	struct scatterlist *sg;
	struct scatterlist *sg_sync;
	int ret = -ENOMEM;
	struct list_head pages;
	struct list_head pages_from_pool;
	struct page_info *info, *tmp_info;
	int i = 0;
	unsigned int nents_sync = 0;
	unsigned long size_remaining = PAGE_ALIGN(size);
	unsigned int max_order = orders[0];
	struct pages_mem data;
	unsigned int sz;
	int vmid = get_secure_vmid(buffer->flags);
	struct page_info info_onstack[SZ_4K / sizeof(struct page_info)];

#ifdef CONFIG_OPLUS_ION_BOOSTPOOL
	struct ion_boost_pool *boost_pool = has_boost_pool(sys_heap, buffer);
#ifdef BOOSTPOOL_DEBUG
	int boostpool_order[3] = {0};
	unsigned long alloc_start = jiffies;
#endif /* BOOSTPOOL_DEBUG */
#endif /* CONFIG_OPLUS_ION_BOOSTPOOL */

	if (size / PAGE_SIZE > totalram_pages / 2)
		return -ENOMEM;

	if (ion_heap_is_system_heap_type(buffer->heap->type) &&
	    is_secure_vmid_valid(vmid)) {
		pr_info("%s: System heap doesn't support secure allocations\n",
			__func__);
		return -EINVAL;
	}

	data.size = 0;
	INIT_LIST_HEAD(&pages);
	INIT_LIST_HEAD(&pages_from_pool);

#ifdef CONFIG_OPLUS_ION_BOOSTPOOL
	if (size < SZ_1M)
		boost_pool = NULL;

	if (boost_pool) {
		unsigned int alloc_sz = 0;
		while (size_remaining > 0) {

#ifdef OPLUS_FEATURE_UIFIRST
			current->static_ux = 2;
#endif /* OPLUS_FEATURE_UIFIRST */
			info = boost_pool_allocate(boost_pool,
						   size_remaining,
						   max_order);
#ifdef OPLUS_FEATURE_UIFIRST
			current->static_ux = 0;
#endif /* OPLUS_FEATURE_UIFIRST */
			if (!info)
				break;

			sz = (1 << info->order) * PAGE_SIZE;
			alloc_sz += sz;
#ifdef BOOSTPOOL_DEBUG
			boostpool_order[order_to_index(info->order)] += 1;
#endif /* BOOSTPOOL_DEBUG */

			list_add_tail(&info->list, &pages_from_pool);

			mod_node_page_state(page_pgdat(info->page),
					    NR_UNRECLAIMABLE_PAGES,
					    (1 << (info->order)));

			size_remaining -= sz;
			max_order = info->order;
			i++;
		}
		max_order = orders[0];

		boost_pool_dec_high(boost_pool, alloc_sz >> PAGE_SHIFT);
#ifdef BOOSTPOOL_DEBUG
		if (size_remaining != 0) {
			pr_info("boostpool %s alloc failed. alloc_sz: %d size: %d orders(%d, %d, %d) %d ms\n",
				boost_pool->name, alloc_sz, (int) size,
				boostpool_order[0], boostpool_order[1],
				boostpool_order[2],
				jiffies_to_msecs(jiffies - alloc_start));
		}
#endif /* BOOSTPOOL_DEBUG */
	}
#endif /* CONFIG_OPLUS_ION_BOOSTPOOL */

	while (size_remaining > 0) {
		if (i >= ARRAY_SIZE(info_onstack)) {
			info = kmem_cache_alloc(ion_page_info_pool, GFP_KERNEL);
			if (!info)
				goto err;
		} else {
			info = &info_onstack[i];
		}

		if (is_secure_vmid_valid(vmid))
			ret = alloc_from_pool_preferred(info,
					sys_heap, buffer, size_remaining,
					max_order);
		else
			ret = alloc_largest_available(info,
					sys_heap, buffer, size_remaining,
					max_order);

		if (ret) {
			free_info(info, info_onstack, ARRAY_SIZE(info_onstack));
			goto err;
		}

		sz = (1 << info->order) * PAGE_SIZE;

		mod_node_page_state(
				page_pgdat(info->page), NR_UNRECLAIMABLE_PAGES,
				(1 << (info->order)));

		if (info->from_pool) {
			list_add_tail(&info->list, &pages_from_pool);
		} else {
			list_add_tail(&info->list, &pages);
			data.size += sz;
			++nents_sync;
		}
		size_remaining -= sz;
		max_order = info->order;
		i++;
	}

	ret = ion_heap_alloc_pages_mem(&data);

	if (ret)
		goto err;

	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table) {
		ret = -ENOMEM;
		goto err_free_data_pages;
	}

	ret = sg_alloc_table(table, i, GFP_KERNEL);
	if (ret)
		goto err1;

	if (nents_sync) {
		ret = sg_alloc_table(&table_sync, nents_sync, GFP_KERNEL);
		if (ret)
			goto err_free_sg;
	}

	i = 0;
	sg = table->sgl;
	sg_sync = table_sync.sgl;

	/*
	 * We now have two separate lists. One list contains pages from the
	 * pool and the other pages from buddy. We want to merge these
	 * together while preserving the ordering of the pages (higher order
	 * first).
	 */
	do {
		info = list_first_entry_or_null(&pages, struct page_info, list);
		tmp_info = list_first_entry_or_null(&pages_from_pool,
						    struct page_info, list);
		if (info && tmp_info) {
			if (info->order >= tmp_info->order) {
				i = process_info(info, sg, sg_sync, &data, i);
				free_info(info, info_onstack,
					  ARRAY_SIZE(info_onstack));
				sg_sync = sg_next(sg_sync);
			} else {
				i = process_info(tmp_info, sg, 0, 0, i);
				free_info(tmp_info, info_onstack,
					  ARRAY_SIZE(info_onstack));
			}
		} else if (info) {
			i = process_info(info, sg, sg_sync, &data, i);
			free_info(info, info_onstack, ARRAY_SIZE(info_onstack));
			sg_sync = sg_next(sg_sync);
		} else if (tmp_info) {
			i = process_info(tmp_info, sg, 0, 0, i);
			free_info(tmp_info, info_onstack,
				  ARRAY_SIZE(info_onstack));
		}
		sg = sg_next(sg);

	} while (sg);

	if (nents_sync) {
		if (vmid > 0) {
			ret = ion_hyp_assign_sg(&table_sync, &vmid, 1, true);
			if (ret)
				goto err_free_sg2;
		}
	}

	buffer->sg_table = table;
	if (nents_sync)
		sg_free_table(&table_sync);
	ion_heap_free_pages_mem(&data);
#ifdef CONFIG_OPLUS_ION_BOOSTPOOL
	if (boost_pool)
		boost_pool_wakeup_process(boost_pool);
#endif /* CONFIG_OPLUS_ION_BOOSTPOOL */
	return 0;

err_free_sg2:
	/* We failed to zero buffers. Bypass pool */
	buffer->private_flags |= ION_PRIV_FLAG_SHRINKER_FREE;

	if (vmid > 0)
		if (ion_hyp_unassign_sg(table, &vmid, 1, true, false))
			goto err_free_table_sync;

	for_each_sg(table->sgl, sg, table->nents, i)
		free_buffer_page(sys_heap, buffer, sg_page(sg),
				 get_order(sg->length));
err_free_table_sync:
	if (nents_sync)
		sg_free_table(&table_sync);
err_free_sg:
	sg_free_table(table);
err1:
	kfree(table);
err_free_data_pages:
	ion_heap_free_pages_mem(&data);
err:
	list_for_each_entry_safe(info, tmp_info, &pages, list) {
		free_buffer_page(sys_heap, buffer, info->page, info->order);
		free_info(info, info_onstack, ARRAY_SIZE(info_onstack));
	}
	list_for_each_entry_safe(info, tmp_info, &pages_from_pool, list) {
		free_buffer_page(sys_heap, buffer, info->page, info->order);
		free_info(info, info_onstack, ARRAY_SIZE(info_onstack));
	}
	return ret;
}

void ion_system_heap_free(struct ion_buffer *buffer)
{
	struct ion_heap *heap = buffer->heap;
	struct ion_system_heap *sys_heap = container_of(heap,
							struct ion_system_heap,
							heap);
	struct sg_table *table = buffer->sg_table;
	struct scatterlist *sg;
	int i;
	int vmid = get_secure_vmid(buffer->flags);

	if (!(buffer->private_flags & ION_PRIV_FLAG_SHRINKER_FREE) &&
	    !(buffer->flags & ION_FLAG_POOL_FORCE_ALLOC)) {
		if (vmid < 0)
			ion_heap_buffer_zero(buffer);
	} else if (vmid > 0) {
		if (ion_hyp_unassign_sg(table, &vmid, 1, true, false))
			return;
	}

	for_each_sg(table->sgl, sg, table->nents, i)
		free_buffer_page(sys_heap, buffer, sg_page(sg),
				 get_order(sg->length));
	sg_free_table(table);
	kfree(table);
}

static int ion_system_heap_shrink(struct ion_heap *heap, gfp_t gfp_mask,
				 int nr_to_scan)
{
	struct ion_system_heap *sys_heap;
	int nr_total = 0;
	int i, j, nr_freed = 0;
	int only_scan = 0;
	struct ion_page_pool *pool;
#ifdef CONFIG_OPLUS_ION_BOOSTPOOL
	struct ion_boost_pool *boost_pool;
#endif /* CONFIG_OPLUS_ION_BOOSTPOOL */

	sys_heap = container_of(heap, struct ion_system_heap, heap);

	if (!nr_to_scan)
		only_scan = 1;

	for (i = 0; i < NUM_ORDERS; i++) {
		nr_freed = 0;

		/*
		 * Keep minimum 200 MB in the pool so that camera launch latency would remain
		 * low under heavy memory pressure also.
		 */
		if (global_zone_page_state(NR_IONCACHE_PAGES) < MIN_ION_POOL_PAGES)
			break;

#ifdef CONFIG_OPLUS_ION_BOOSTPOOL
		if (sys_heap->uncached_boost_pool) {
			boost_pool = sys_heap->uncached_boost_pool;
			nr_freed += boost_pool_shrink(boost_pool,
							boost_pool->pools[i],
							gfp_mask,
							nr_to_scan);
		}
		if (sys_heap->gr_pool) {
			boost_pool = sys_heap->gr_pool;
			nr_freed += boost_pool_shrink(boost_pool,
						      boost_pool->pools[i],
						      gfp_mask,
						      nr_to_scan);
		}
		if (sys_heap->cam_pool) {
			boost_pool = sys_heap->cam_pool;
			nr_freed += boost_pool_shrink(boost_pool,
						      boost_pool->pools[i],
						      gfp_mask,
						      nr_to_scan);
		}
#endif /* CONFIG_OPLUS_ION_BOOSTPOOL */

		for (j = 0; j < VMID_LAST; j++) {
			if (is_secure_vmid_valid(j))
				nr_freed += ion_secure_page_pool_shrink(
						sys_heap, j, i, nr_to_scan);
		}

		pool = sys_heap->uncached_pools[i];
		nr_freed += ion_page_pool_shrink(pool, gfp_mask, nr_to_scan);

		pool = sys_heap->cached_pools[i];
		nr_freed += ion_page_pool_shrink(pool, gfp_mask, nr_to_scan);
		nr_total += nr_freed;

		if (!only_scan) {
			nr_to_scan -= nr_freed;
			/* shrink completed */
			if (nr_to_scan <= 0)
				break;
		}
	}

	return nr_total;
}

static struct ion_heap_ops system_heap_ops = {
	.allocate = ion_system_heap_allocate,
	.free = ion_system_heap_free,
	.map_kernel = ion_heap_map_kernel,
	.unmap_kernel = ion_heap_unmap_kernel,
	.map_user = ion_heap_map_user,
	.shrink = ion_system_heap_shrink,
};

static void ion_system_heap_destroy_pools(struct ion_page_pool **pools)
{
	int i;

	for (i = 0; i < NUM_ORDERS; i++)
		if (pools[i]) {
			ion_page_pool_destroy(pools[i]);
			pools[i] = NULL;
		}
}

/**
 * ion_system_heap_create_pools - Creates pools for all orders
 *
 * If this fails you don't need to destroy any pools. It's all or
 * nothing. If it succeeds you'll eventually need to use
 * ion_system_heap_destroy_pools to destroy the pools.
 */
int ion_system_heap_create_pools(struct ion_page_pool **pools,
				 bool cached, bool boost_flag)
{
	int i;
	for (i = 0; i < NUM_ORDERS; i++) {
		struct ion_page_pool *pool;
		gfp_t gfp_flags = low_order_gfp_flags;

		if (orders[i])
			gfp_flags = high_order_gfp_flags;
		pool = ion_page_pool_create(gfp_flags, orders[i], cached);
		pool->boost_flag = boost_flag;
		if (!pool)
			goto err_create_pool;
		pools[i] = pool;
	}
	return 0;
err_create_pool:
	ion_system_heap_destroy_pools(pools);
	return -ENOMEM;
}

#ifdef CONFIG_OPLUS_ION_BOOSTPOOL
static void init_once(void *foo)
{
	;
}
#endif /* CONFIG_OPLUS_ION_BOOSTPOOL */

static int fill_page_pool(struct device *dev, struct ion_page_pool *pool)
{
	struct page *page;

	if (NULL == pool) {
		pr_err("%s: pool is NULL!\n", __func__);
		return -ENOENT;
	}

	page = ion_page_pool_alloc_pages(pool);
	if (NULL == page)
		return -ENOMEM;

	ion_pages_sync_for_device(dev, page,
				  PAGE_SIZE << pool->order,
				  DMA_BIDIRECTIONAL);

	ion_page_pool_free(pool, page);

	return 0;
}

static int fill_pool_kworkthread(void *p)
{
	int i;
	struct ion_system_heap * sh;
	sh = (struct ion_system_heap *) p;

	pr_info("boot time ION pool filling started\n");

	for (i = 0; i < NUM_ORDERS; i++) {
		while (global_zone_page_state(NR_IONCACHE_PAGES) <
				MIN_ION_POOL_PAGES_BOOTUP) {
			if (fill_page_pool(sh->heap.priv, sh->cached_pools[i]) < 0)
				break;
		}
	}

	for (i = 0; i < NUM_ORDERS; i++) {
		while (global_zone_page_state(NR_IONCACHE_PAGES) <
				(2 * MIN_ION_POOL_PAGES_BOOTUP)) {
			if (fill_page_pool(sh->heap.priv, sh->uncached_pools[i]) < 0)
				break;
		}
	}

	pr_info("boot time ION pool filling ended\n");
	return 0;
}

struct ion_heap *ion_system_heap_create(struct ion_platform_heap *data)
{
	struct ion_system_heap *heap;
	int i;
	struct task_struct *tsk;

#ifdef CONFIG_OPLUS_ION_BOOSTPOOL
	struct proc_dir_entry *boost_root_dir;
#endif /* CONFIG_OPLUS_ION_BOOSTPOOL */

	ion_page_info_pool = KMEM_CACHE(page_info, 0);
	if (!ion_page_info_pool)
		return ERR_PTR(-ENOMEM);

	heap = kzalloc(sizeof(*heap), GFP_KERNEL);
	if (!heap)
		goto destroy_page_info_pool;

	heap->heap.ops = &system_heap_ops;
	heap->heap.type = ION_HEAP_TYPE_SYSTEM;
	heap->heap.flags = ION_HEAP_FLAG_DEFER_FREE;

	for (i = 0; i < VMID_LAST; i++)
		if (is_secure_vmid_valid(i))
			if (ion_system_heap_create_pools(
					heap->secure_pools[i], false, false))
				goto destroy_secure_pools;

	if (ion_system_heap_create_pools(heap->uncached_pools, false, false))
		goto destroy_secure_pools;

	if (ion_system_heap_create_pools(heap->cached_pools, true, false))
		goto destroy_uncached_pools;

#ifdef CONFIG_OPLUS_ION_BOOSTPOOL
	boost_root_dir = proc_mkdir("boost_pool", NULL);
	if (!IS_ERR_OR_NULL(boost_root_dir)) {
		unsigned long cam_sz = 32 * 256, uncached_sz = 32 * 256;

		if (totalram_pages > ((SZ_2G << 1) >> PAGE_SHIFT)) {
			cam_sz = 192 * 256;
			uncached_sz = 64 * 256;
		}
		/* on low memory target, we should not set 128Mib on camera pool. */
		/* TODO set by total ram pages */
		heap->cam_pool = boost_pool_create(heap, ION_FLAG_CAMERA_BUFFER,
						   cam_sz,
						   boost_root_dir, "camera", ION_FLAG_CACHED);
		if (!heap->cam_pool)
			pr_err("%s: create boost_pool camera failed!\n",
			       __func__);
		heap->uncached_boost_pool = boost_pool_create(heap, 0,
						uncached_sz, boost_root_dir, "ion_boost_pool_uncached", 0);
		if (!heap->uncached_boost_pool)
			pr_err("%s: create boost_pool ion_uncached failed!\n", __func__);
		boost_ion_info_cachep = kmem_cache_create("boost_ion_info_cachep",
						sizeof(struct page_info), 0,0, init_once);
		if (boost_ion_info_cachep != NULL)
			create_kmemcache_ion_info_success = true;
		else
			pr_err("boost_ion_info_cachep create failed\n");
	}
#endif /* CONFIG_OPLUS_ION_BOOSTPOOL */
	mutex_init(&heap->split_page_mutex);

	return &heap->heap;

destroy_uncached_pools:
	ion_system_heap_destroy_pools(heap->uncached_pools);
destroy_secure_pools:
	for (i = 0; i < VMID_LAST; i++) {
		if (heap->secure_pools[i])
			ion_system_heap_destroy_pools(heap->secure_pools[i]);
	}
	kfree(heap);
destroy_page_info_pool:
	kmem_cache_destroy(ion_page_info_pool);
	return ERR_PTR(-ENOMEM);
}

static int ion_system_contig_heap_allocate(struct ion_heap *heap,
					   struct ion_buffer *buffer,
					   unsigned long len,
					   unsigned long flags)
{
	int order = get_order(len);
	struct page *page;
	struct sg_table *table;
	unsigned long i;
	int ret;

	page = alloc_pages(low_order_gfp_flags | __GFP_NOWARN, order);
	if (!page)
		return -ENOMEM;

	split_page(page, order);

	len = PAGE_ALIGN(len);
	for (i = len >> PAGE_SHIFT; i < (1 << order); i++)
		__free_page(page + i);

	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table) {
		ret = -ENOMEM;
		goto free_pages;
	}

	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret)
		goto free_table;

	sg_set_page(table->sgl, page, len, 0);

	buffer->sg_table = table;

	ion_pages_sync_for_device(NULL, page, len, DMA_BIDIRECTIONAL);

	return 0;

free_table:
	kfree(table);
free_pages:
	for (i = 0; i < len >> PAGE_SHIFT; i++)
		__free_page(page + i);

	return ret;
}

static void ion_system_contig_heap_free(struct ion_buffer *buffer)
{
	struct sg_table *table = buffer->sg_table;
	struct page *page = sg_page(table->sgl);
	unsigned long pages = PAGE_ALIGN(buffer->size) >> PAGE_SHIFT;
	unsigned long i;

	for (i = 0; i < pages; i++)
		__free_page(page + i);
	sg_free_table(table);
	kfree(table);
}

static struct ion_heap_ops kmalloc_ops = {
	.allocate = ion_system_contig_heap_allocate,
	.free = ion_system_contig_heap_free,
	.map_kernel = ion_heap_map_kernel,
	.unmap_kernel = ion_heap_unmap_kernel,
	.map_user = ion_heap_map_user,
};

struct ion_heap *ion_system_contig_heap_create(struct ion_platform_heap *unused)
{
	struct ion_heap *heap;

	heap = kzalloc(sizeof(struct ion_heap), GFP_KERNEL);
	if (!heap)
		return ERR_PTR(-ENOMEM);
	heap->ops = &kmalloc_ops;
	heap->type = ION_HEAP_TYPE_SYSTEM_CONTIG;
	return heap;
}
