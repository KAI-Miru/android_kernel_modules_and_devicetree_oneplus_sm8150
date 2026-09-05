// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 KAI-Miru. All rights reserved. */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <../../mm/slab.h>

#define ATHENA_ALLOCATOR_BUFFER_SIZE	512
#define ATHENA_SWAPPINESS_BUFFER_SIZE	128
#define ATHENA_SWAPPINESS_MAX		200

extern int direct_vm_swappiness;

static unsigned long athena_kmalloc_cache_bytes(struct kmem_cache *cache)
{
	struct kmem_cache *child;
	struct slabinfo info = { };
	struct slabinfo child_info;

	get_slabinfo(cache, &info);
	for_each_memcg_cache(child, cache) {
		memset(&child_info, 0, sizeof(child_info));
		get_slabinfo(child, &child_info);
		info.num_objs += child_info.num_objs;
	}

	return info.num_objs * cache->object_size;
}

static ssize_t athena_kmalloc_used_read(struct file *file,
		char __user *buffer, size_t count, loff_t *ppos)
{
	char output[ATHENA_ALLOCATOR_BUFFER_SIZE];
	size_t length = 0;
	int index, type;

	mutex_lock(&slab_mutex);
	for (index = 0; index <= KMALLOC_SHIFT_HIGH; index++) {
		unsigned long bytes = 0;
		unsigned int size = kmalloc_size(index);

		if (!size)
			continue;

		for (type = KMALLOC_NORMAL; type <= KMALLOC_RECLAIM; type++) {
			struct kmem_cache *cache = kmalloc_caches[type][index];

			if (cache)
				bytes += athena_kmalloc_cache_bytes(cache);
		}

		if (!bytes)
			continue;

		length += scnprintf(output + length, sizeof(output) - length,
				"%-8u %lu\n", size, bytes >> 10);
		if (length >= sizeof(output) - 1)
			break;
	}
	mutex_unlock(&slab_mutex);

	return simple_read_from_buffer(buffer, count, ppos, output, length);
}

static ssize_t athena_vmalloc_used_read(struct file *file,
		char __user *buffer, size_t count, loff_t *ppos)
{
	char output[32];
	size_t length;

	length = scnprintf(output, sizeof(output), "%lu\n",
			vmalloc_nr_pages() << 2);
	return simple_read_from_buffer(buffer, count, ppos, output, length);
}

static ssize_t athena_swappiness_read(struct file *file,
		char __user *buffer, size_t count, loff_t *ppos)
{
	char output[ATHENA_SWAPPINESS_BUFFER_SIZE];
	size_t length;

	length = scnprintf(output, sizeof(output),
			"vm_swappiness: %d\n"
			"direct_swappiness: %d\n"
			"swapd_swappiness: %d\n",
			READ_ONCE(vm_swappiness),
			READ_ONCE(direct_vm_swappiness),
			READ_ONCE(vm_swappiness));

	return simple_read_from_buffer(buffer, count, ppos, output, length);
}

static bool athena_update_swappiness(const char *input, const char *key,
		int *target)
{
	int value;
	size_t key_length = strlen(key);

	if (strncmp(input, key, key_length))
		return false;
	if (kstrtoint(input + key_length, 0, &value))
		return false;
	if (value < 0 || value > ATHENA_SWAPPINESS_MAX)
		return false;

	WRITE_ONCE(*target, value);
	return true;
}

static ssize_t athena_swappiness_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	char input[ATHENA_SWAPPINESS_BUFFER_SIZE];
	char *value;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buffer, count))
		return -EFAULT;

	input[count] = '\0';
	value = strstrip(input);
	if (athena_update_swappiness(value, "vm_swappiness=", &vm_swappiness) ||
	    athena_update_swappiness(value, "direct_swappiness=",
				       &direct_vm_swappiness) ||
	    athena_update_swappiness(value, "swapd_swappiness=", &vm_swappiness))
		return count;

	return -EINVAL;
}

static const struct file_operations athena_kmalloc_used_fops = {
	.owner = THIS_MODULE,
	.read = athena_kmalloc_used_read,
};

static const struct file_operations athena_vmalloc_used_fops = {
	.owner = THIS_MODULE,
	.read = athena_vmalloc_used_read,
};

static const struct file_operations athena_swappiness_fops = {
	.owner = THIS_MODULE,
	.read = athena_swappiness_read,
	.write = athena_swappiness_write,
};

int __init create_athena_memory_abi(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *kmalloc_entry = NULL;
	struct proc_dir_entry *vmalloc_entry = NULL;
	struct proc_dir_entry *swappiness_entry;

#ifndef CONFIG_KMALLOC_DEBUG
	kmalloc_entry = proc_create("kmalloc_used", 0444, parent,
				    &athena_kmalloc_used_fops);
	if (!kmalloc_entry)
		goto error;
#endif

#ifndef CONFIG_VMALLOC_DEBUG
	vmalloc_entry = proc_create("vmalloc_used", 0444, parent,
				    &athena_vmalloc_used_fops);
	if (!vmalloc_entry)
		goto error;
#endif

	swappiness_entry = proc_create("swappiness_para", 0666, parent,
				       &athena_swappiness_fops);
	if (!swappiness_entry)
		goto error;

	return 0;

error:
	if (vmalloc_entry)
		proc_remove(vmalloc_entry);
	if (kmalloc_entry)
		proc_remove(kmalloc_entry);
	return -ENOMEM;
}
