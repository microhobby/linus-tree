// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2019, Microsoft Corporation.
 *
 * Author:
 *   Iouri Tarassov <iourit@microsoft.com>
 *
 * Dxgkrnl Graphics Driver
 * Helper functions
 *
 */

#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

#include "dxgkrnl.h"
#include "misc.h"

#undef pr_fmt
#define pr_fmt(fmt)	"dxgk:err: " fmt

u16 *wcsncpy(u16 *dest, const u16 *src, size_t n)
{
	int i;

	for (i = 0; i < n; i++) {
		dest[i] = src[i];
		if (src[i] == 0) {
			i++;
			break;
		}
	}
	dest[i - 1] = 0;
	return dest;
}

int dxg_copy_from_user(void *to, const void __user *from, unsigned long len)
{
	int ret = copy_from_user(to, from, len);

	if (ret) {
		pr_err("copy_from_user failed: %p %p %lx\n", to, from, len);
		return -EINVAL;
	}
	return 0;
}

int dxg_copy_to_user(void __user *to, const void *from, unsigned long len)
{
	int ret = copy_to_user(to, from, len);

	if (ret) {
		pr_err("copy_to_user failed: %p %p %lx\n", to, from, len);
		return -EINVAL;
	}
	return 0;
}

#ifdef CONFIG_DXGKRNL_DEBUG

static atomic_t dxg_memory[DXGMEM_LAST];

void dxgmem_addalloc(struct dxgprocess *process, enum dxgk_memory_tag tag)
{
	if (process)
		atomic_inc(&process->dxg_memory[tag]);
	else
		atomic_inc(&dxg_memory[tag]);
}

void dxgmem_remalloc(struct dxgprocess *process, enum dxgk_memory_tag tag)
{
	if (process) {
		if (atomic_read(&process->dxg_memory[tag]) == 0) {
			pr_err("%s process error: %d\n",
					__func__, tag);
			dump_stack();
		} else {
			atomic_dec(&process->dxg_memory[tag]);
		}
	} else {
		if (atomic_read(&dxg_memory[tag]) == 0) {
			pr_err("%s error: %d\n", __func__, tag);
			dump_stack();
		} else {
			atomic_dec(&dxg_memory[tag]);
		}
	}
}

void dxgmem_check(struct dxgprocess *process, enum dxgk_memory_tag ignore_tag)
{
	int i;
	int v;

	for (i = 0; i < DXGMEM_LAST; i++) {
		if (process)
			v = atomic_read(&process->dxg_memory[i]);
		else
			v = atomic_read(&dxg_memory[i]);
		if (i != ignore_tag && v != 0)
			pr_err("memory leak: %2d %3d\n", i, v);
	}
}

void dxglockorder_acquire(enum dxgk_lockorder order)
{
	struct dxgthreadinfo *info = dxglockorder_get_thread();
	int index = info->current_lock_index;

	dev_dbg(dxgglobaldev, "%s %p %d %d",
		__func__, info->thread, index, order);
	if (index == DXGK_MAX_LOCK_DEPTH) {
		pr_err("Lock depth exceeded");
		DXGKRNL_ASSERT(0);
	}
	if (index != 0) {
		struct dxgk_lockinfo *lock_info = &info->lock_info[index - 1];

		if (lock_info->lock_order <= order) {
			pr_err("Invalid lock order: %d %d %d", index,
				   lock_info->lock_order, order);
			DXGKRNL_ASSERT(0);
		}
	}
	info->lock_info[index].lock_order = order;
	info->current_lock_index++;
	dxglockorder_put_thread(info);
}

void dxglockorder_release(enum dxgk_lockorder order)
{
	struct dxgthreadinfo *info = dxglockorder_get_thread();
	int index;
	int i;

	info->current_lock_index--;
	index = info->current_lock_index;
	dev_dbg(dxgglobaldev, "dxglockorder release %p %d %d",
		info->thread, index, order);
	if (index < 0) {
		pr_err("Lock depth underflow");
		DXGKRNL_ASSERT(0);
	}
	for (i = index; i >= 0; i--) {
		if (info->lock_info[i].lock_order == order) {
			memmove(&info->lock_info[i], &info->lock_info[index],
				(index - i) * sizeof(info->lock_info[0]));
			break;
		}
	}
	if (i < 0) {
		pr_err("Failed to find lock order to release");
		DXGKRNL_ASSERT(0);
	}
	memset(&info->lock_info[index], 0, sizeof(info->lock_info[0]));
	dxglockorder_put_thread(info);
}

struct dxgthreadinfo *dxglockorder_get_thread(void)
{
	struct dxgthreadinfo *info = NULL;
	struct dxgthreadinfo *entry = NULL;
	struct task_struct *thread = current;

	mutex_lock(&dxgglobal->thread_info_mutex);
	list_for_each_entry(entry, &dxgglobal->thread_info_list_head,
			    thread_info_list_entry) {
		if (entry->thread == thread) {
			info = entry;
			dev_dbg(dxgglobaldev, "dxglockorder found thread %p %d",
				thread, info->refcount + 1);
			break;
		}
	}
	if (info == NULL) {
		info = kzalloc(sizeof(struct dxgthreadinfo), GFP_ATOMIC);
		if (info) {
			dev_dbg(dxgglobaldev, "dxglockorder new thread %p",
				thread);
			dxgmem_addalloc(NULL, DXGMEM_THREADINFO);
			info->thread = thread;
			list_add(&info->thread_info_list_entry,
				 &dxgglobal->thread_info_list_head);
		}
	}
	if (info)
		info->refcount++;
	mutex_unlock(&dxgglobal->thread_info_mutex);
	return info;
}

void dxglockorder_put_thread(struct dxgthreadinfo *info)
{
	if (info) {
		dev_dbg(dxgglobaldev, "dxglockorder remove thread %p %d",
			info->thread, info->refcount);
		if (info->refcount <= 0) {
			pr_err("Invalid refcount for thread info: %p %d",
				   info, info->refcount);
			DXGKRNL_ASSERT(0);
			return;
		}
		info->refcount--;
		if (info->refcount == 0) {
			dev_dbg(dxgglobaldev, "dxglockorder remove thread %p",
				info->thread);
			if (info->current_lock_index != 0) {
				pr_err("A lock is not released: %d %d",
				   info->current_lock_index,
				   info->lock_info[
				   info->current_lock_index].lock_order);
				DXGKRNL_ASSERT(0);
			}

			if (!info->lock_held) {
				mutex_lock(&dxgglobal->thread_info_mutex);
				list_del(&info->thread_info_list_entry);
				mutex_unlock(&dxgglobal->thread_info_mutex);
			}

			kfree(info);
			dxgmem_remalloc(NULL, DXGMEM_THREADINFO);
		}
	}
}

void dxglockorder_check_empty(struct dxgthreadinfo *info)
{
	if (info->refcount != 1) {
		pr_err("A lock is not released");
		DXGKRNL_ASSERT(0);
	}
}

#endif /* CONFIG_DXGKRNL_DEBUG */
