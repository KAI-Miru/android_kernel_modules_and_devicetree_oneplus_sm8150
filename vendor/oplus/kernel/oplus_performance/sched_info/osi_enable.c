// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2022 Oplus. All rights reserved. */

#include <linux/kernel.h>

#include "osi_base.h"
#include "osi_enable.h"

unsigned int cpu_jank_info_enable;

static ssize_t proc_jank_info_enable_read(struct file *file, char __user *buf,
					  size_t count, loff_t *ppos)
{
	char buffer[PROC_NUMBUF];
	size_t len;

	len = scnprintf(buffer, sizeof(buffer), "%u\n",
			READ_ONCE(cpu_jank_info_enable));
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t proc_jank_info_enable_write(struct file *file,
					   const char __user *buf,
					   size_t count, loff_t *ppos)
{
	char buffer[PROC_NUMBUF];
	unsigned int enable;
	int err;

	if (*ppos != 0 || !count)
		return -EINVAL;
	if (count >= sizeof(buffer))
		return -E2BIG;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	buffer[count] = '\0';

	err = kstrtouint(strstrip(buffer), 0, &enable);
	if (err)
		return err;
	WRITE_ONCE(cpu_jank_info_enable, enable);

	*ppos += count;
	return count;
}

static const struct file_operations proc_jank_info_enable_operations = {
	.read = proc_jank_info_enable_read,
	.write = proc_jank_info_enable_write,
	.llseek = default_llseek,
};

struct proc_dir_entry *jank_enable_proc_init(struct proc_dir_entry *pde)
{
	return proc_create("enable", S_IRUGO | S_IWUGO, pde,
			   &proc_jank_info_enable_operations);
}

void jank_enable_proc_deinit(struct proc_dir_entry *pde)
{
	remove_proc_entry("enable", pde);
}
