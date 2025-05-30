// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/staging/android/ion/ion.c
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (c) 2011-2020, The Linux Foundation. All rights reserved.
 *
 */

#include <linux/anon_inodes.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/oom.h>
#include <linux/rbtree.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/bitops.h>
#include <linux/msm_dma_iommu_mapping.h>
#define CREATE_TRACE_POINTS
#include <trace/events/ion.h>
#include <soc/qcom/secure_buffer.h>

#include "ion.h"
#include "ion_secure_util.h"

static struct ion_device *internal_dev;
static atomic_long_t total_heap_bytes;

int ion_walk_heaps(int heap_id, enum ion_heap_type type, void *data,
		   int (*f)(struct ion_heap *heap, void *data))
{
	int ret_val = 0;
	struct ion_heap *heap;
	struct ion_device *dev = internal_dev;
	/*
	 * traverse the list of heaps available in this system
	 * and find the heap that is specified.
	 */
	down_write(&dev->lock);
	plist_for_each_entry(heap, &dev->heaps, node) {
		if (ION_HEAP(heap->id) != heap_id ||
		    type != heap->type)
			continue;
		ret_val = f(heap, data);
		break;
	}
	up_write(&dev->lock);
	return ret_val;
}
EXPORT_SYMBOL(ion_walk_heaps);

bool ion_buffer_cached(struct ion_buffer *buffer)
{
	return !!(buffer->flags & ION_FLAG_CACHED);
}

/* this function should only be called while dev->lock is held */
static void ion_buffer_add(struct ion_device *dev,
			   struct ion_buffer *buffer)
{
	struct rb_node **p = &dev->buffers.rb_node;
	struct rb_node *parent = NULL;
	struct ion_buffer *entry;
	struct task_struct *task;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct ion_buffer, node);

		if (buffer < entry) {
			p = &(*p)->rb_left;
		} else if (buffer > entry) {
			p = &(*p)->rb_right;
		} else {
			pr_err("%s: buffer already found.", __func__);
			BUG();
		}
	}
	task = current;
	get_task_comm(buffer->task_comm, task->group_leader);
	get_task_comm(buffer->thread_comm, task);
	buffer->pid = task_pid_nr(task->group_leader);
	buffer->tid = task_pid_nr(task);

	rb_link_node(&buffer->node, parent, p);
	rb_insert_color(&buffer->node, &dev->buffers);
}

static void ion_debug_heap_usage_show(struct ion_heap *heap);

/* this function should only be called while dev->lock is held */
static struct ion_buffer *ion_buffer_create(struct ion_heap *heap,
					    struct ion_device *dev,
					    unsigned long len,
					    unsigned long flags)
{
	struct ion_buffer *buffer;
	struct sg_table *table;
	int ret;
	long nr_alloc_cur, nr_alloc_peak;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	buffer->heap = heap;
	buffer->flags = flags;
	buffer->dev = dev;
	buffer->size = len;

	ret = heap->ops->allocate(heap, buffer, len, flags);

	if (ret) {
		if (!(heap->flags & ION_HEAP_FLAG_DEFER_FREE))
			goto err2;

		if (ret == -EINTR)
			goto err2;

		ion_heap_freelist_drain(heap, 0);
		ret = heap->ops->allocate(heap, buffer, len, flags);
		if (ret)
			goto err2;
	}

	if (!buffer->sg_table) {
		WARN_ONCE(1, "This heap needs to set the sgtable");
		ret = -EINVAL;
		goto err1;
	}

	table = buffer->sg_table;
	INIT_LIST_HEAD(&buffer->attachments);
	INIT_LIST_HEAD(&buffer->vmas);
	mutex_init(&buffer->lock);

	if (IS_ENABLED(CONFIG_ION_FORCE_DMA_SYNC)) {
		int i;
		struct scatterlist *sg;

		/*
		 * this will set up dma addresses for the sglist -- it is not
		 * technically correct as per the dma api -- a specific
		 * device isn't really taking ownership here.  However, in
		 * practice on our systems the only dma_address space is
		 * physical addresses.
		 */
		for_each_sg(table->sgl, sg, table->nents, i) {
			sg_dma_address(sg) = sg_phys(sg);
			sg_dma_len(sg) = sg->length;
		}
	}

	mutex_lock(&dev->buffer_lock);
	ion_buffer_add(dev, buffer);
	mutex_unlock(&dev->buffer_lock);
	nr_alloc_cur = atomic_long_add_return(len, &heap->total_allocated);
	nr_alloc_peak = atomic_long_read(&heap->total_allocated_peak);
	if (nr_alloc_cur > nr_alloc_peak)
		atomic_long_set(&heap->total_allocated_peak, nr_alloc_cur);
	atomic_long_add(len, &total_heap_bytes);

	return buffer;

err1:
	heap->ops->free(buffer);
err2:
	kfree(buffer);
	ion_debug_heap_usage_show(heap);
	return ERR_PTR(ret);
}

void ion_buffer_destroy(struct ion_buffer *buffer)
{
	if (buffer->kmap_cnt > 0) {
		pr_warn_ratelimited("ION client likely missing a call to dma_buf_kunmap or dma_buf_vunmap\n");
		buffer->heap->ops->unmap_kernel(buffer->heap, buffer);
	}
	atomic_long_sub(buffer->size, &buffer->heap->total_allocated);
	buffer->heap->ops->free(buffer);
	kfree(buffer);
}

static void _ion_buffer_destroy(struct ion_buffer *buffer)
{
	struct ion_heap *heap = buffer->heap;
	struct ion_device *dev = buffer->dev;

	msm_dma_buf_freed(buffer);

	mutex_lock(&dev->buffer_lock);
	rb_erase(&buffer->node, &dev->buffers);
	mutex_unlock(&dev->buffer_lock);
	atomic_long_sub(buffer->size, &total_heap_bytes);

	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE)
		ion_heap_freelist_add(heap, buffer);
	else
		ion_buffer_destroy(buffer);
}

static void *ion_buffer_kmap_get(struct ion_buffer *buffer)
{
	void *vaddr;

	if (buffer->kmap_cnt) {
		if (buffer->kmap_cnt == INT_MAX)
			return ERR_PTR(-EOVERFLOW);

		buffer->kmap_cnt++;
		return buffer->vaddr;
	}
	vaddr = buffer->heap->ops->map_kernel(buffer->heap, buffer);
	if (WARN_ONCE(!vaddr,
		      "heap->ops->map_kernel should return ERR_PTR on error"))
		return ERR_PTR(-EINVAL);
	if (IS_ERR(vaddr))
		return vaddr;
	buffer->vaddr = vaddr;
	buffer->kmap_cnt++;
	return vaddr;
}

static void ion_buffer_kmap_put(struct ion_buffer *buffer)
{
	if (buffer->kmap_cnt == 0) {
		pr_warn_ratelimited("ION client likely missing a call to dma_buf_kmap or dma_buf_vmap, pid:%d\n",
				    current->pid);
		return;
	}

	buffer->kmap_cnt--;
	if (!buffer->kmap_cnt) {
		buffer->heap->ops->unmap_kernel(buffer->heap, buffer);
		buffer->vaddr = NULL;
	}
}

static struct sg_table *dup_sg_table(struct sg_table *table)
{
	struct sg_table *new_table;
	int ret, i;
	struct scatterlist *sg, *new_sg;

	new_table = kzalloc(sizeof(*new_table), GFP_KERNEL);
	if (!new_table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(new_table, table->nents, GFP_KERNEL);
	if (ret) {
		kfree(new_table);
		return ERR_PTR(-ENOMEM);
	}

	new_sg = new_table->sgl;
	for_each_sg(table->sgl, sg, table->nents, i) {
		memcpy(new_sg, sg, sizeof(*sg));
		sg_dma_address(new_sg) = 0;
		sg_dma_len(new_sg) = 0;
		new_sg = sg_next(new_sg);
	}

	return new_table;
}

static void free_duped_table(struct sg_table *table)
{
	sg_free_table(table);
	kfree(table);
}

struct ion_dma_buf_attachment {
	struct device *dev;
	struct sg_table *table;
	struct list_head list;
	bool dma_mapped;
};

static int ion_dma_buf_attach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attachment)
{
	struct ion_dma_buf_attachment *a;
	struct sg_table *table;
	struct ion_buffer *buffer = dmabuf->priv;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	table = dup_sg_table(buffer->sg_table);
	if (IS_ERR(table)) {
		kfree(a);
		return -ENOMEM;
	}

	a->table = table;
	a->dev = attachment->dev;
	a->dma_mapped = false;
	INIT_LIST_HEAD(&a->list);

	attachment->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void ion_dma_buf_detatch(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attachment)
{
	struct ion_dma_buf_attachment *a = attachment->priv;
	struct ion_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	list_del(&a->list);
	mutex_unlock(&buffer->lock);
	free_duped_table(a->table);

	kfree(a);
}

static struct sg_table *ion_map_dma_buf(struct dma_buf_attachment *attachment,
					enum dma_data_direction direction)
{
	struct ion_dma_buf_attachment *a = attachment->priv;
	struct sg_table *table;
	int count, map_attrs;
	struct ion_buffer *buffer = attachment->dmabuf->priv;

	table = a->table;

	map_attrs = attachment->dma_map_attrs;
	if (!(buffer->flags & ION_FLAG_CACHED) ||
	    !hlos_accessible_buffer(buffer))
		map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	mutex_lock(&buffer->lock);
	if (map_attrs & DMA_ATTR_SKIP_CPU_SYNC)
		trace_ion_dma_map_cmo_skip(attachment->dev,
					   attachment->dmabuf->buf_name,
					   ion_buffer_cached(buffer),
					   hlos_accessible_buffer(buffer),
					   attachment->dma_map_attrs,
					   direction);
	else
		trace_ion_dma_map_cmo_apply(attachment->dev,
					    attachment->dmabuf->buf_name,
					    ion_buffer_cached(buffer),
					    hlos_accessible_buffer(buffer),
					    attachment->dma_map_attrs,
					    direction);

	if (map_attrs & DMA_ATTR_DELAYED_UNMAP) {
		count = msm_dma_map_sg_attrs(attachment->dev, table->sgl,
					     table->nents, direction,
					     attachment->dmabuf, map_attrs);
	} else {
		count = dma_map_sg_attrs(attachment->dev, table->sgl,
					 table->nents, direction,
					 map_attrs);
	}

	if (count <= 0) {
		mutex_unlock(&buffer->lock);
		return ERR_PTR(-ENOMEM);
	}

	a->dma_mapped = true;
	mutex_unlock(&buffer->lock);
	return table;
}

static void ion_unmap_dma_buf(struct dma_buf_attachment *attachment,
			      struct sg_table *table,
			      enum dma_data_direction direction)
{
	int map_attrs;
	struct ion_buffer *buffer = attachment->dmabuf->priv;
	struct ion_dma_buf_attachment *a = attachment->priv;

	map_attrs = attachment->dma_map_attrs;
	if (!(buffer->flags & ION_FLAG_CACHED) ||
	    !hlos_accessible_buffer(buffer))
		map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	mutex_lock(&buffer->lock);
	if (map_attrs & DMA_ATTR_SKIP_CPU_SYNC)
		trace_ion_dma_unmap_cmo_skip(attachment->dev,
					     attachment->dmabuf->buf_name,
					     ion_buffer_cached(buffer),
					     hlos_accessible_buffer(buffer),
					     attachment->dma_map_attrs,
					     direction);
	else
		trace_ion_dma_unmap_cmo_apply(attachment->dev,
					      attachment->dmabuf->buf_name,
					      ion_buffer_cached(buffer),
					      hlos_accessible_buffer(buffer),
					      attachment->dma_map_attrs,
					      direction);

	if (map_attrs & DMA_ATTR_DELAYED_UNMAP)
		msm_dma_unmap_sg_attrs(attachment->dev, table->sgl,
				       table->nents, direction,
				       attachment->dmabuf,
				       map_attrs);
	else
		dma_unmap_sg_attrs(attachment->dev, table->sgl, table->nents,
				   direction, map_attrs);
	a->dma_mapped = false;
	mutex_unlock(&buffer->lock);
}

void ion_pages_sync_for_device(struct device *dev, struct page *page,
			       size_t size, enum dma_data_direction dir)
{
	struct scatterlist sg;

	sg_init_table(&sg, 1);
	sg_set_page(&sg, page, size, 0);
	/*
	 * This is not correct - sg_dma_address needs a dma_addr_t that is valid
	 * for the targeted device, but this works on the currently targeted
	 * hardware.
	 */
	sg_dma_address(&sg) = page_to_phys(page);
	dma_sync_sg_for_device(dev, &sg, 1, dir);
}

static void ion_vm_open(struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = vma->vm_private_data;
	struct ion_vma_list *vma_list;

	vma_list = kmalloc(sizeof(*vma_list), GFP_KERNEL);
	if (!vma_list)
		return;
	vma_list->vma = vma;
	mutex_lock(&buffer->lock);
	list_add(&vma_list->list, &buffer->vmas);
	mutex_unlock(&buffer->lock);
}

static void ion_vm_close(struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = vma->vm_private_data;
	struct ion_vma_list *vma_list, *tmp;

	mutex_lock(&buffer->lock);
	list_for_each_entry_safe(vma_list, tmp, &buffer->vmas, list) {
		if (vma_list->vma != vma)
			continue;
		list_del(&vma_list->list);
		kfree(vma_list);
		break;
	}
	mutex_unlock(&buffer->lock);
}

static const struct vm_operations_struct ion_vma_ops = {
	.open = ion_vm_open,
	.close = ion_vm_close,
};

static int ion_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = dmabuf->priv;
	int ret = 0;

	if (!buffer->heap->ops->map_user) {
		pr_err("%s: this heap does not define a method for mapping to userspace\n",
		       __func__);
		return -EINVAL;
	}

	if (!(buffer->flags & ION_FLAG_CACHED))
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	vma->vm_private_data = buffer;
	vma->vm_ops = &ion_vma_ops;
	ion_vm_open(vma);

	mutex_lock(&buffer->lock);
	/* now map it to userspace */
	ret = buffer->heap->ops->map_user(buffer->heap, buffer, vma);
	mutex_unlock(&buffer->lock);

	if (ret)
		pr_err("%s: failure mapping buffer to userspace\n",
		       __func__);

	return ret;
}

static void ion_dma_buf_release(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = dmabuf->priv;

	_ion_buffer_destroy(buffer);
	kfree(dmabuf->exp_name);
}

static void *ion_dma_buf_vmap(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = dmabuf->priv;
	void *vaddr = ERR_PTR(-EINVAL);

	if (buffer->heap->ops->map_kernel) {
		mutex_lock(&buffer->lock);
		vaddr = ion_buffer_kmap_get(buffer);
		mutex_unlock(&buffer->lock);
	} else {
		pr_warn_ratelimited("heap %s doesn't support map_kernel\n",
				    buffer->heap->name);
	}

	return vaddr;
}

static void ion_dma_buf_vunmap(struct dma_buf *dmabuf, void *vaddr)
{
	struct ion_buffer *buffer = dmabuf->priv;

	if (buffer->heap->ops->map_kernel) {
		mutex_lock(&buffer->lock);
		ion_buffer_kmap_put(buffer);
		mutex_unlock(&buffer->lock);
	}
}

static void *ion_dma_buf_kmap(struct dma_buf *dmabuf, unsigned long offset)
{
	/*
	 * TODO: Once clients remove their hacks where they assume kmap(ed)
	 * addresses are virtually contiguous implement this properly
	 */
	void *vaddr = ion_dma_buf_vmap(dmabuf);

	if (IS_ERR(vaddr))
		return vaddr;

	return vaddr + offset * PAGE_SIZE;
}

static void ion_dma_buf_kunmap(struct dma_buf *dmabuf, unsigned long offset,
			       void *ptr)
{
	/*
	 * TODO: Once clients remove their hacks where they assume kmap(ed)
	 * addresses are virtually contiguous implement this properly
	 */
	ion_dma_buf_vunmap(dmabuf, ptr);
}

static int ion_sgl_sync_range(struct device *dev, struct scatterlist *sgl,
			      unsigned int nents, unsigned long offset,
			      unsigned long length,
			      enum dma_data_direction dir, bool for_cpu)
{
	int i;
	struct scatterlist *sg;
	unsigned int len = 0;
	dma_addr_t sg_dma_addr;

	for_each_sg(sgl, sg, nents, i) {
		if (sg_dma_len(sg) == 0)
			break;

		if (i > 0) {
			pr_warn_ratelimited("Partial cmo only supported with 1 segment\n"
				"is dma_set_max_seg_size being set on dev:%s\n",
				dev_name(dev));
			return -EINVAL;
		}
	}

	for_each_sg(sgl, sg, nents, i) {
		unsigned int sg_offset, sg_left, size = 0;

		if (i == 0)
			sg_dma_addr = sg_dma_address(sg);

		len += sg->length;
		if (len <= offset) {
			sg_dma_addr += sg->length;
			continue;
		}

		sg_left = len - offset;
		sg_offset = sg->length - sg_left;

		size = (length < sg_left) ? length : sg_left;
		if (for_cpu)
			dma_sync_single_range_for_cpu(dev, sg_dma_addr,
						      sg_offset, size, dir);
		else
			dma_sync_single_range_for_device(dev, sg_dma_addr,
							 sg_offset, size, dir);

		offset += size;
		length -= size;
		sg_dma_addr += sg->length;

		if (length == 0)
			break;
	}

	return 0;
}

static int ion_sgl_sync_mapped(struct device *dev, struct scatterlist *sgl,
			       unsigned int nents, struct list_head *vmas,
			       enum dma_data_direction dir, bool for_cpu)
{
	struct ion_vma_list *vma_list;
	int ret = 0;

	list_for_each_entry(vma_list, vmas, list) {
		struct vm_area_struct *vma = vma_list->vma;

		ret = ion_sgl_sync_range(dev, sgl, nents,
					 vma->vm_pgoff * PAGE_SIZE,
					 vma->vm_end - vma->vm_start, dir,
					 for_cpu);
		if (ret)
			break;
	}

	return ret;
}

static int __ion_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					  enum dma_data_direction direction,
					  bool sync_only_mapped)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_dma_buf_attachment *a;
	int ret = 0;

	if (!hlos_accessible_buffer(buffer)) {
		trace_ion_begin_cpu_access_cmo_skip(NULL, dmabuf->buf_name,
						    ion_buffer_cached(buffer),
						    false, direction,
						    sync_only_mapped);
		ret = -EPERM;
		goto out;
	}

	if (!(buffer->flags & ION_FLAG_CACHED)) {
		trace_ion_begin_cpu_access_cmo_skip(NULL, dmabuf->buf_name,
						    false, true, direction,
						    sync_only_mapped);
		goto out;
	}

	mutex_lock(&buffer->lock);

	if (IS_ENABLED(CONFIG_ION_FORCE_DMA_SYNC)) {
		struct device *dev = buffer->heap->priv;
		struct sg_table *table = buffer->sg_table;

		if (sync_only_mapped)
			ret = ion_sgl_sync_mapped(dev, table->sgl,
						  table->nents, &buffer->vmas,
						  direction, true);
		else
			dma_sync_sg_for_cpu(dev, table->sgl,
					    table->nents, direction);

		if (!ret)
			trace_ion_begin_cpu_access_cmo_apply(dev,
							     dmabuf->buf_name,
							     true, true,
							     direction,
							     sync_only_mapped);
		else
			trace_ion_begin_cpu_access_cmo_skip(dev,
							    dmabuf->buf_name,
							    true, true,
							    direction,
							    sync_only_mapped);
		mutex_unlock(&buffer->lock);
		goto out;
	}

	list_for_each_entry(a, &buffer->attachments, list) {
		int tmp = 0;

		if (!a->dma_mapped) {
			trace_ion_begin_cpu_access_notmapped(a->dev,
							     dmabuf->buf_name,
							     true, true,
							     direction,
							     sync_only_mapped);
			continue;
		}

		if (sync_only_mapped)
			tmp = ion_sgl_sync_mapped(a->dev, a->table->sgl,
						  a->table->nents,
						  &buffer->vmas,
						  direction, true);
		else
			dma_sync_sg_for_cpu(a->dev, a->table->sgl,
					    a->table->nents, direction);

		if (!tmp) {
			trace_ion_begin_cpu_access_cmo_apply(a->dev,
							     dmabuf->buf_name,
							     true, true,
							     direction,
							     sync_only_mapped);
		} else {
			trace_ion_begin_cpu_access_cmo_skip(a->dev,
							    dmabuf->buf_name,
							    true, true,
							    direction,
							    sync_only_mapped);
			ret = tmp;
		}

	}
	mutex_unlock(&buffer->lock);
out:
	return ret;
}

static int __ion_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction,
					bool sync_only_mapped)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_dma_buf_attachment *a;
	int ret = 0;

	if (!hlos_accessible_buffer(buffer)) {
		trace_ion_end_cpu_access_cmo_skip(NULL, dmabuf->buf_name,
						  ion_buffer_cached(buffer),
						  false, direction,
						  sync_only_mapped);
		ret = -EPERM;
		goto out;
	}

	if (!(buffer->flags & ION_FLAG_CACHED)) {
		trace_ion_end_cpu_access_cmo_skip(NULL, dmabuf->buf_name, false,
						  true, direction,
						  sync_only_mapped);
		goto out;
	}

	mutex_lock(&buffer->lock);
	if (IS_ENABLED(CONFIG_ION_FORCE_DMA_SYNC)) {
		struct device *dev = buffer->heap->priv;
		struct sg_table *table = buffer->sg_table;

		if (sync_only_mapped)
			ret = ion_sgl_sync_mapped(dev, table->sgl,
						  table->nents, &buffer->vmas,
						  direction, false);
		else
			dma_sync_sg_for_device(dev, table->sgl,
					       table->nents, direction);

		if (!ret)
			trace_ion_end_cpu_access_cmo_apply(dev,
							   dmabuf->buf_name,
							   true, true,
							   direction,
							   sync_only_mapped);
		else
			trace_ion_end_cpu_access_cmo_skip(dev, dmabuf->buf_name,
							  true, true, direction,
							  sync_only_mapped);
		mutex_unlock(&buffer->lock);
		goto out;
	}

	list_for_each_entry(a, &buffer->attachments, list) {
		int tmp = 0;

		if (!a->dma_mapped) {
			trace_ion_end_cpu_access_notmapped(a->dev,
							   dmabuf->buf_name,
							   true, true,
							   direction,
							   sync_only_mapped);
			continue;
		}

		if (sync_only_mapped)
			tmp = ion_sgl_sync_mapped(a->dev, a->table->sgl,
						  a->table->nents,
						  &buffer->vmas, direction,
						  false);
		else
			dma_sync_sg_for_device(a->dev, a->table->sgl,
					       a->table->nents, direction);

		if (!tmp) {
			trace_ion_end_cpu_access_cmo_apply(a->dev,
							   dmabuf->buf_name,
							   true, true,
							   direction,
							   sync_only_mapped);
		} else {
			trace_ion_end_cpu_access_cmo_skip(a->dev,
							  dmabuf->buf_name,
							  true, true, direction,
							  sync_only_mapped);
			ret = tmp;
		}
	}
	mutex_unlock(&buffer->lock);

out:
	return ret;
}

static int ion_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction)
{
	return __ion_dma_buf_begin_cpu_access(dmabuf, direction, false);
}

static int ion_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction direction)
{
	return __ion_dma_buf_end_cpu_access(dmabuf, direction, false);
}

static int ion_dma_buf_begin_cpu_access_umapped(struct dma_buf *dmabuf,
						enum dma_data_direction dir)
{
	return __ion_dma_buf_begin_cpu_access(dmabuf, dir, true);
}

static int ion_dma_buf_end_cpu_access_umapped(struct dma_buf *dmabuf,
					      enum dma_data_direction dir)
{
	return __ion_dma_buf_end_cpu_access(dmabuf, dir, true);
}

static int ion_dma_buf_begin_cpu_access_partial(struct dma_buf *dmabuf,
						enum dma_data_direction dir,
						unsigned int offset,
						unsigned int len)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_dma_buf_attachment *a;
	int ret = 0;

	if (!hlos_accessible_buffer(buffer)) {
		trace_ion_begin_cpu_access_cmo_skip(NULL, dmabuf->buf_name,
						    ion_buffer_cached(buffer),
						    false, dir,
						    false);
		ret = -EPERM;
		goto out;
	}

	if (!(buffer->flags & ION_FLAG_CACHED)) {
		trace_ion_begin_cpu_access_cmo_skip(NULL, dmabuf->buf_name,
						    false, true, dir,
						    false);
		goto out;
	}

	mutex_lock(&buffer->lock);
	if (IS_ENABLED(CONFIG_ION_FORCE_DMA_SYNC)) {
		struct device *dev = buffer->heap->priv;
		struct sg_table *table = buffer->sg_table;

		ret = ion_sgl_sync_range(dev, table->sgl, table->nents,
					 offset, len, dir, true);

		if (!ret)
			trace_ion_begin_cpu_access_cmo_apply(dev,
							     dmabuf->buf_name,
							     true, true, dir,
							     false);
		else
			trace_ion_begin_cpu_access_cmo_skip(dev,
							    dmabuf->buf_name,
							    true, true, dir,
							    false);
		mutex_unlock(&buffer->lock);
		goto out;
	}

	list_for_each_entry(a, &buffer->attachments, list) {
		int tmp = 0;

		if (!a->dma_mapped) {
			trace_ion_begin_cpu_access_notmapped(a->dev,
							     dmabuf->buf_name,
							     true, true,
							     dir,
							     false);
			continue;
		}

		tmp = ion_sgl_sync_range(a->dev, a->table->sgl, a->table->nents,
					 offset, len, dir, true);

		if (!tmp) {
			trace_ion_begin_cpu_access_cmo_apply(a->dev,
							     dmabuf->buf_name,
							     true, true, dir,
							     false);
		} else {
			trace_ion_begin_cpu_access_cmo_skip(a->dev,
							    dmabuf->buf_name,
							    true, true, dir,
							    false);
			ret = tmp;
		}
	}
	mutex_unlock(&buffer->lock);

out:
	return ret;
}

static int ion_dma_buf_end_cpu_access_partial(struct dma_buf *dmabuf,
					      enum dma_data_direction direction,
					      unsigned int offset,
					      unsigned int len)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_dma_buf_attachment *a;
	int ret = 0;

	if (!hlos_accessible_buffer(buffer)) {
		trace_ion_end_cpu_access_cmo_skip(NULL, dmabuf->buf_name,
						  ion_buffer_cached(buffer),
						  false, direction,
						  false);
		ret = -EPERM;
		goto out;
	}

	if (!(buffer->flags & ION_FLAG_CACHED)) {
		trace_ion_end_cpu_access_cmo_skip(NULL, dmabuf->buf_name, false,
						  true, direction,
						  false);
		goto out;
	}

	mutex_lock(&buffer->lock);
	if (IS_ENABLED(CONFIG_ION_FORCE_DMA_SYNC)) {
		struct device *dev = buffer->heap->priv;
		struct sg_table *table = buffer->sg_table;

		ret = ion_sgl_sync_range(dev, table->sgl, table->nents,
					 offset, len, direction, false);

		if (!ret)
			trace_ion_end_cpu_access_cmo_apply(dev,
							   dmabuf->buf_name,
							   true, true,
							   direction, false);
		else
			trace_ion_end_cpu_access_cmo_skip(dev, dmabuf->buf_name,
							  true, true,
							  direction, false);

		mutex_unlock(&buffer->lock);
		goto out;
	}

	list_for_each_entry(a, &buffer->attachments, list) {
		int tmp = 0;

		if (!a->dma_mapped) {
			trace_ion_end_cpu_access_notmapped(a->dev,
							   dmabuf->buf_name,
							   true, true,
							   direction,
							   false);
			continue;
		}

		tmp = ion_sgl_sync_range(a->dev, a->table->sgl, a->table->nents,
					 offset, len, direction, false);

		if (!tmp) {
			trace_ion_end_cpu_access_cmo_apply(a->dev,
							   dmabuf->buf_name,
							   true, true,
							   direction, false);

		} else {
			trace_ion_end_cpu_access_cmo_skip(a->dev,
							  dmabuf->buf_name,
							  true, true, direction,
							  false);
			ret = tmp;
		}
	}
	mutex_unlock(&buffer->lock);

out:
	return ret;
}

static int ion_dma_buf_get_flags(struct dma_buf *dmabuf,
				 unsigned long *flags)
{
	struct ion_buffer *buffer = dmabuf->priv;
	*flags = buffer->flags;

	return 0;
}

static const struct dma_buf_ops dma_buf_ops = {
	.map_dma_buf = ion_map_dma_buf,
	.unmap_dma_buf = ion_unmap_dma_buf,
	.mmap = ion_mmap,
	.release = ion_dma_buf_release,
	.attach = ion_dma_buf_attach,
	.detach = ion_dma_buf_detatch,
	.begin_cpu_access = ion_dma_buf_begin_cpu_access,
	.end_cpu_access = ion_dma_buf_end_cpu_access,
	.begin_cpu_access_umapped = ion_dma_buf_begin_cpu_access_umapped,
	.end_cpu_access_umapped = ion_dma_buf_end_cpu_access_umapped,
	.begin_cpu_access_partial = ion_dma_buf_begin_cpu_access_partial,
	.end_cpu_access_partial = ion_dma_buf_end_cpu_access_partial,
	.map = ion_dma_buf_kmap,
	.unmap = ion_dma_buf_kunmap,
	.vmap = ion_dma_buf_vmap,
	.vunmap = ion_dma_buf_vunmap,
	.get_flags = ion_dma_buf_get_flags,
};

struct dma_buf *ion_alloc_dmabuf(size_t len, unsigned int heap_id_mask,
				 unsigned int flags)
{
	struct ion_device *dev = internal_dev;
	struct ion_buffer *buffer = NULL;
	struct ion_heap *heap;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	char task_comm[TASK_COMM_LEN];

	pr_debug("%s: len %zu heap_id_mask %u flags %x\n", __func__,
		 len, heap_id_mask, flags);
	/*
	 * traverse the list of heaps available in this system in priority
	 * order.  If the heap type is supported by the client, and matches the
	 * request of the caller allocate from it.  Repeat until allocate has
	 * succeeded or all heaps have been tried
	 */
	len = PAGE_ALIGN(len);

	if (!len)
		return ERR_PTR(-EINVAL);

	down_read(&dev->lock);
	plist_for_each_entry(heap, &dev->heaps, node) {
		/* if the caller didn't specify this heap id */
		if (!((1 << heap->id) & heap_id_mask))
			continue;
		tracing_mark_begin("%s(%s, %zu, 0x%x, 0x%x)", "ion_alloc",
				   heap->name, len, heap_id_mask, flags);
		buffer = ion_buffer_create(heap, dev, len, flags);
		tracing_mark_end();
		if (!IS_ERR(buffer) || PTR_ERR(buffer) == -EINTR)
			break;
	}
	up_read(&dev->lock);

	if (!buffer)
		return ERR_PTR(-ENODEV);

	if (IS_ERR(buffer)) {
		pr_err("%s ion alloc failed len: 0x%zx mask=0x%x flags=0x%x error%ld\n",
		       __func__, len, heap_id_mask, flags, PTR_ERR(buffer));
		return ERR_CAST(buffer);
	}

	get_task_comm(task_comm, current->group_leader);

	exp_info.ops = &dma_buf_ops;
	exp_info.size = buffer->size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buffer;
	exp_info.exp_name = kasprintf(GFP_KERNEL, "%s-%s-%d-%s", KBUILD_MODNAME,
				      heap->name, current->tgid, task_comm);

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		_ion_buffer_destroy(buffer);
		kfree(exp_info.exp_name);
	}

	return dmabuf;
}

struct dma_buf *ion_alloc(size_t len, unsigned int heap_id_mask,
			  unsigned int flags)
{
	struct ion_device *dev = internal_dev;
	struct ion_heap *heap;
	bool type_valid = false;

	pr_debug("%s: len %zu heap_id_mask %u flags %x\n", __func__,
		 len, heap_id_mask, flags);
	/*
	 * traverse the list of heaps available in this system in priority
	 * order.  Check the heap type is supported.
	 */

	down_read(&dev->lock);
	plist_for_each_entry(heap, &dev->heaps, node) {
		/* if the caller didn't specify this heap id */
		if (!((1 << heap->id) & heap_id_mask))
			continue;
		if (heap->type == ION_HEAP_TYPE_SYSTEM ||
		    heap->type == ION_HEAP_TYPE_CARVEOUT ||
		    heap->type == (enum ion_heap_type)ION_HEAP_TYPE_HYP_CMA ||
		    heap->type == (enum ion_heap_type)ION_HEAP_TYPE_RBIN ||
		    heap->type ==
			(enum ion_heap_type)ION_HEAP_TYPE_SYSTEM_SECURE) {
			type_valid = true;
		} else {
			pr_warn("%s: heap type not supported, type:%d\n",
				__func__, heap->type);
		}
		break;
	}
	up_read(&dev->lock);

	if (!type_valid)
		return ERR_PTR(-EINVAL);

	return ion_alloc_dmabuf(len, heap_id_mask, flags);
}
EXPORT_SYMBOL(ion_alloc);

int ion_alloc_fd(size_t len, unsigned int heap_id_mask, unsigned int flags)
{
	int fd;
	struct dma_buf *dmabuf;

	dmabuf = ion_alloc_dmabuf(len, heap_id_mask, flags);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dmabuf);

	return fd;
}

#define MAX_ION_ACC_PROCESS	16	/* should be smaller than bitmap */
struct ion_size_account {
	char task_comm[TASK_COMM_LEN];
	pid_t pid;
	size_t size;
};
static struct ion_size_account ion_size_acc[MAX_ION_ACC_PROCESS];
static int ion_dbg_idx_new;
static int ion_dbg_idx_last;

static inline int __ion_account_add_buf_locked(struct ion_buffer *buffer)
{
	int i;

	if (ion_dbg_idx_new &&
			(ion_size_acc[ion_dbg_idx_last].pid == buffer->pid)) {
		ion_size_acc[ion_dbg_idx_last].size += buffer->size;
		return 0;
	}
	for (i = 0; i < ion_dbg_idx_new; i++) {
		if (ion_size_acc[i].pid == buffer->pid) {
			ion_size_acc[i].size += buffer->size;
			ion_dbg_idx_last = i;
			return 0;
		}
	}
	if (ion_dbg_idx_new == MAX_ION_ACC_PROCESS) {
		pr_warn_once("out of ion_size_account idx\n");
		return -1;
	}
	ion_size_acc[ion_dbg_idx_new].pid = buffer->pid;
	ion_size_acc[ion_dbg_idx_new].size = buffer->size;
	strncpy(ion_size_acc[ion_dbg_idx_new].task_comm, buffer->task_comm,
		TASK_COMM_LEN);
	ion_dbg_idx_last = ion_dbg_idx_new++;
	return 0;
}

static inline void __ion_account_print_locked(void)
{
	int i, heaviest_idx;
	size_t heaviest_size = 0;
	size_t total = 0;

	if (!ion_dbg_idx_new)
		return;
	pr_info("ion_size: accounted by thread group\n");
	pr_info("ion_size: %16s( %3s ) %8s\n", "task_comm", "pid", "size(kb)");
	for (i = 0; i < ion_dbg_idx_new; i++) {
		pr_info("[%d]       %16s(%5u) %8zu\n", i, ion_size_acc[i].task_comm,
			ion_size_acc[i].pid, ion_size_acc[i].size / SZ_1K);
		if (heaviest_size < ion_size_acc[i].size) {
			heaviest_size = ion_size_acc[i].size ;
			heaviest_idx = i;
		}
		total += ion_size_acc[i].size;
	}
	if (heaviest_size)
		pr_info("heaviest_task_ion:%s(%5u) size:%zuKB, total:%zuKB/%luKB\n",
			ion_size_acc[heaviest_idx].task_comm,
			ion_size_acc[heaviest_idx].pid, heaviest_size / SZ_1K,
			total / SZ_1K, totalram_pages << (PAGE_SHIFT - 10));
}

bool ion_account_print_usage(void)
{
	struct rb_node *n;
	struct ion_buffer *buffer;
	unsigned int system_heap_id;
	struct ion_device *dev = internal_dev;
	bool locked;

	if (!dev)
		return false;
	system_heap_id = get_ion_system_heap_id();
	if (IS_ERR(ERR_PTR(system_heap_id)))
		return false;
	locked = mutex_trylock(&dev->buffer_lock);
	if (!locked)
		return false;
	ion_dbg_idx_new = 0;
	ion_dbg_idx_last = -1;
	for (n = rb_first(&dev->buffers); n; n = rb_next(n)) {
		buffer = rb_entry(n, struct ion_buffer, node);
		if (buffer->heap->id == system_heap_id)
			__ion_account_add_buf_locked(buffer);
	}
	__ion_account_print_locked();
	mutex_unlock(&dev->buffer_lock);

	return true;
}

int ion_query_heaps(struct ion_heap_query *query)
{
	struct ion_device *dev = internal_dev;
	struct ion_heap_data __user *buffer = u64_to_user_ptr(query->heaps);
	int ret = -EINVAL, cnt = 0, max_cnt;
	struct ion_heap *heap;
	struct ion_heap_data hdata;

	memset(&hdata, 0, sizeof(hdata));

	down_read(&dev->lock);
	if (!buffer) {
		query->cnt = dev->heap_cnt;
		ret = 0;
		goto out;
	}

	if (query->cnt <= 0)
		goto out;

	max_cnt = query->cnt;

	plist_for_each_entry(heap, &dev->heaps, node) {
		strlcpy(hdata.name, heap->name, sizeof(hdata.name));
		hdata.name[sizeof(hdata.name) - 1] = '\0';
		hdata.type = heap->type;
		hdata.heap_id = heap->id;

		if (copy_to_user(&buffer[cnt], &hdata, sizeof(hdata))) {
			ret = -EFAULT;
			goto out;
		}

		cnt++;
		if (cnt >= max_cnt)
			break;
	}

	query->cnt = cnt;
	ret = 0;
out:
	up_read(&dev->lock);
	return ret;
}

static const struct file_operations ion_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = ion_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ion_ioctl,
#endif
};

static void __ion_debug_heap_usage_show(struct ion_heap *heap)
{
	struct ion_device *dev = heap->dev;
	struct rb_node *n;
	size_t total_size = 0;

	pr_info("ion heap: %s %u\n", heap->name, heap->id);
	pr_info("%16s %16s %16s\n", "task", "pid", "size");
	mutex_lock(&dev->buffer_lock);
	for (n = rb_first(&dev->buffers); n; n = rb_next(n)) {
		struct ion_buffer *buffer = rb_entry(n, struct ion_buffer,
						     node);
		if (buffer->heap->id != heap->id)
			continue;
		total_size += buffer->size;
		pr_info("%16s %16u (%16s %16u) %16zu\n", buffer->task_comm,
			buffer->pid, buffer->thread_comm, buffer->tid,
			buffer->size);
	}
	mutex_unlock(&dev->buffer_lock);
	pr_info("%16s %16zu\n", "total ", total_size);
	pr_info("%16.s %16lu\n", "peak allocated",
		atomic_long_read(&heap->total_allocated_peak));
}

static void ion_debug_heap_usage_show(struct ion_heap *heap)
{
	static DEFINE_RATELIMIT_STATE(show_heap_usage, HZ * 10, 1);

	/* supports only for some heaps */
	if (heap->type != ION_HEAP_TYPE_CARVEOUT &&
	    heap->type != ION_HEAP_TYPE_DMA &&
	    heap->type != ION_HEAP_TYPE_SECURE_DMA &&
	    heap->type != ION_HEAP_TYPE_HYP_CMA &&
	    heap->type != ION_HEAP_TYPE_SECURE_CARVEOUT)
		return;

	if (heap->id == ION_CAMERA_HEAP_ID)
		return;

	if (!__ratelimit(&show_heap_usage))
		return;

	__ion_debug_heap_usage_show(heap);
}

static void ion_debug_heap_usage_show_force(struct ion_heap *heap)
{
	static DEFINE_RATELIMIT_STATE(show_heap_usage_force, HZ * 10, 1);

	if (!__ratelimit(&show_heap_usage_force))
		return;

	__ion_debug_heap_usage_show(heap);
}

static int ion_oom_notify(struct notifier_block *nb,
			  unsigned long action, void *data)
{
	struct ion_heap *heap;

	/* print ion system_heap */
	heap = get_ion_heap(ION_SYSTEM_HEAP_ID);
	ion_debug_heap_usage_show_force(heap);

	return NOTIFY_DONE;
}

static struct notifier_block ion_oom_notifier = {
	.notifier_call = ion_oom_notify,
};

static int ion_debug_heap_show(struct seq_file *s, void *unused)
{
	struct ion_heap *heap = s->private;
	struct ion_device *dev = heap->dev;
	struct rb_node *n;
	size_t total_size = 0;

	seq_printf(s, "%16s %16s %16s\n", "client", "pid", "size");

	seq_puts(s, "----------------------------------------------------\n");
	mutex_lock(&dev->buffer_lock);
	for (n = rb_first(&dev->buffers); n; n = rb_next(n)) {
		struct ion_buffer *buffer = rb_entry(n, struct ion_buffer,
						     node);
		if (buffer->heap->id != heap->id)
			continue;
		total_size += buffer->size;
		seq_printf(s, "%16s %16u (%16s %16u) %16zu\n",
			   buffer->task_comm, buffer->pid,
			   buffer->thread_comm, buffer->tid,
			   buffer->size);
	}
	mutex_unlock(&dev->buffer_lock);
	seq_puts(s, "----------------------------------------------------\n");
	seq_printf(s, "%16s %16zu\n", "total ", total_size);
	seq_printf(s, "%16.s %16lu\n", "peak allocated",
		   atomic_long_read(&heap->total_allocated_peak));
	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE)
		seq_printf(s, "%16s %16zu\n", "deferred free",
			   heap->free_list_size);
	seq_puts(s, "----------------------------------------------------\n");

	if (heap->debug_show)
		heap->debug_show(heap, s, unused);

	return 0;
}

static int ion_debug_heap_open(struct inode *inode, struct file *file)
{
	return single_open(file, ion_debug_heap_show, inode->i_private);
}

static const struct file_operations debug_heap_fops = {
	.open = ion_debug_heap_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int debug_shrink_set(void *data, u64 val)
{
	struct ion_heap *heap = data;
	struct shrink_control sc;
	int objs;

	sc.gfp_mask = GFP_HIGHUSER;
	sc.nr_to_scan = val;

	if (!val) {
		objs = heap->shrinker.count_objects(&heap->shrinker, &sc);
		sc.nr_to_scan = objs;
	}

	heap->shrinker.scan_objects(&heap->shrinker, &sc);
	return 0;
}

static int debug_shrink_get(void *data, u64 *val)
{
	struct ion_heap *heap = data;
	struct shrink_control sc;
	int objs;

	sc.gfp_mask = GFP_HIGHUSER;
	sc.nr_to_scan = 0;

	objs = heap->shrinker.count_objects(&heap->shrinker, &sc);
	*val = objs;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_shrink_fops, debug_shrink_get,
			debug_shrink_set, "%llu\n");

void ion_device_add_heap(struct ion_device *dev, struct ion_heap *heap)
{
	int ret;

	if (!heap->ops->allocate || !heap->ops->free)
		pr_err("%s: can not add heap with invalid ops struct.\n",
		       __func__);

	spin_lock_init(&heap->free_lock);
	heap->free_list_size = 0;

	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE)
		ion_heap_init_deferred_free(heap);

	if ((heap->flags & ION_HEAP_FLAG_DEFER_FREE) || heap->ops->shrink) {
		ret = ion_heap_init_shrinker(heap);
		if (ret)
			pr_err("%s: Failed to register shrinker\n", __func__);
	}

	heap->dev = dev;
	down_write(&dev->lock);
	/*
	 * use negative heap->id to reverse the priority -- when traversing
	 * the list later attempt higher id numbers first
	 */
	plist_node_init(&heap->node, -heap->id);
	plist_add(&heap->node, &dev->heaps);
	debugfs_create_file(heap->name, 0664,
			    dev->heaps_debug_root, heap,
			    &debug_heap_fops);

	if (heap->shrinker.count_objects && heap->shrinker.scan_objects) {
		char debug_name[64];

		snprintf(debug_name, 64, "%s_shrink", heap->name);
		debugfs_create_file(debug_name, 0644, dev->heaps_debug_root,
				    heap, &debug_shrink_fops);
	}

	dev->heap_cnt++;
	up_write(&dev->lock);
}
EXPORT_SYMBOL(ion_device_add_heap);

static ssize_t
total_heaps_kb_show(struct kobject *kobj, struct kobj_attribute *attr,
		    char *buf)
{
	u64 size_in_bytes = atomic_long_read(&total_heap_bytes);

	return sprintf(buf, "%llu\n", div_u64(size_in_bytes, 1024));
}

static ssize_t
total_pools_kb_show(struct kobject *kobj, struct kobj_attribute *attr,
		    char *buf)
{
	u64 size_in_bytes = ion_page_pool_nr_pages() * PAGE_SIZE;

	return sprintf(buf, "%llu\n", div_u64(size_in_bytes, 1024));
}

static struct kobj_attribute total_heaps_kb_attr =
	__ATTR_RO(total_heaps_kb);

static struct kobj_attribute total_pools_kb_attr =
	__ATTR_RO(total_pools_kb);

static struct attribute *ion_device_attrs[] = {
	&total_heaps_kb_attr.attr,
	&total_pools_kb_attr.attr,
	NULL,
};

ATTRIBUTE_GROUPS(ion_device);

static int ion_init_sysfs(void)
{
	struct kobject *ion_kobj;
	int ret;

	ion_kobj = kobject_create_and_add("ion", kernel_kobj);
	if (!ion_kobj)
		return -ENOMEM;

	ret = sysfs_create_groups(ion_kobj, ion_device_groups);
	if (ret) {
		kobject_put(ion_kobj);
		return ret;
	}

	return 0;
}

struct ion_device *ion_device_create(void)
{
	struct ion_device *idev;
	int ret;

	idev = kzalloc(sizeof(*idev), GFP_KERNEL);
	if (!idev)
		return ERR_PTR(-ENOMEM);

	idev->dev.minor = MISC_DYNAMIC_MINOR;
	idev->dev.name = "ion";
	idev->dev.fops = &ion_fops;
	idev->dev.parent = NULL;
	ret = misc_register(&idev->dev);
	if (ret) {
		pr_err("ion: failed to register misc device.\n");
		goto err_reg;
	}

	ret = ion_init_sysfs();
	if (ret) {
		pr_err("ion: failed to add sysfs attributes.\n");
		goto err_sysfs;
	}

	idev->debug_root = debugfs_create_dir("ion", NULL);
	idev->heaps_debug_root = debugfs_create_dir("heaps", idev->debug_root);
	WARN_ON(register_oom_notifier(&ion_oom_notifier));
	idev->buffers = RB_ROOT;
	mutex_init(&idev->buffer_lock);
	init_rwsem(&idev->lock);
	plist_head_init(&idev->heaps);
	internal_dev = idev;
	return idev;

err_sysfs:
	misc_deregister(&idev->dev);
err_reg:
	kfree(idev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(ion_device_create);
