// SPDX-License-Identifier: GPL-2.0-only
/* Android 14 OnePlus 9R schedutil I/O-wait control ABI. */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#include "oplus_iowait.h"

#define IOWAIT_INPUT_SIZE 16
#define IOWAIT_OUTPUT_SIZE 64

unsigned int sysctl_iowait_reset_ticks = 1;
unsigned int sysctl_iowait_apply_ticks;
unsigned int sysctl_oplus_iowait_boost_enabled;
unsigned int sysctl_oplus_iowait_skip_min_enabled = 1;
EXPORT_SYMBOL(sysctl_iowait_reset_ticks);
EXPORT_SYMBOL(sysctl_iowait_apply_ticks);
EXPORT_SYMBOL(sysctl_oplus_iowait_boost_enabled);
EXPORT_SYMBOL(sysctl_oplus_iowait_skip_min_enabled);

struct oplus_iowait_node {
	unsigned int *value;
	const char *label;
	unsigned int maximum;
};

static ssize_t oplus_iowait_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	const struct oplus_iowait_node *node = PDE_DATA(file_inode(file));
	char input[IOWAIT_INPUT_SIZE];
	unsigned int value;
	int ret;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buffer, count))
		return -EFAULT;
	input[count] = '\0';
	ret = kstrtouint(strstrip(input), 10, &value);
	if (ret)
		return ret;
	if (value > node->maximum)
		return -ERANGE;

	WRITE_ONCE(*node->value, value);
	return count;
}

static ssize_t oplus_iowait_read(struct file *file, char __user *buffer,
		size_t count, loff_t *ppos)
{
	const struct oplus_iowait_node *node = PDE_DATA(file_inode(file));
	char output[IOWAIT_OUTPUT_SIZE];
	int length;

	length = scnprintf(output, sizeof(output), "%s=%u\n", node->label,
			 READ_ONCE(*node->value));
	return simple_read_from_buffer(buffer, count, ppos, output, length);
}

static const struct file_operations oplus_iowait_fops = {
	.owner = THIS_MODULE,
	.read = oplus_iowait_read,
	.write = oplus_iowait_write,
	.llseek = default_llseek,
};

static const struct oplus_iowait_node oplus_iowait_nodes[] = {
	{
		.value = &sysctl_iowait_reset_ticks,
		.label = "iowait_reset_ticks",
		.maximum = 1000,
	},
	{
		.value = &sysctl_iowait_apply_ticks,
		.label = "iowait_apply_ticks",
		.maximum = 1000,
	},
	{
		.value = &sysctl_oplus_iowait_boost_enabled,
		.label = "iowait_boost_freq_enabled",
		.maximum = 1,
	},
	{
		.value = &sysctl_oplus_iowait_skip_min_enabled,
		.label = "iowait_skip_min_enabled",
		.maximum = 1,
	},
};

static const char * const oplus_iowait_names[] = {
	"iowait_reset_ticks",
	"iowait_apply_ticks",
	"oplus_iowait_boost_enabled",
	"oplus_iowait_skip_min_enabled",
};

int oplus_sched_assist_iowait_proc_init(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *directory;
	int index;

	directory = proc_mkdir("iowait_config", parent);
	if (!directory)
		return -ENOMEM;

	for (index = 0; index < ARRAY_SIZE(oplus_iowait_nodes); index++) {
		if (!proc_create_data(oplus_iowait_names[index], 0666, directory,
				      &oplus_iowait_fops,
				      (void *)&oplus_iowait_nodes[index]))
			return -ENOMEM;
	}

	return 0;
}

MODULE_DESCRIPTION("Oplus schedutil I/O-wait control ABI");
MODULE_LICENSE("GPL v2");
