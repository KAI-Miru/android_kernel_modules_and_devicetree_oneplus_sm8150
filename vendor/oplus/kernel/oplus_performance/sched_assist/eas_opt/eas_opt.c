// SPDX-License-Identifier: GPL-2.0-only
/* Android 14 OnePlus 9R EAS control ABI, adapted for Linux 4.14. */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#include "oplus_cap.h"
#include "oplus_iowait.h"

#define EAS_OPT_INPUT_SIZE 50

int eas_opt_debug_enable;
int eas_opt_enable;
int force_apply_ocap_enable;
EXPORT_SYMBOL(eas_opt_debug_enable);
EXPORT_SYMBOL(eas_opt_enable);
EXPORT_SYMBOL(force_apply_ocap_enable);

static ssize_t eas_opt_enable_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	char input[EAS_OPT_INPUT_SIZE];
	int value;
	int ret;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buffer, count))
		return -EFAULT;
	input[count] = '\0';
	ret = kstrtoint(strstrip(input), 10, &value);
	if (ret)
		return ret;

	switch (value) {
	case 0:
		eas_opt_enable = 0;
		eas_opt_debug_enable = 0;
		force_apply_ocap_enable = 0;
		break;
	case 1:
		eas_opt_enable = 1;
		eas_opt_debug_enable = 0;
		force_apply_ocap_enable = 0;
		break;
	case 2:
		eas_opt_enable = 1;
		eas_opt_debug_enable = 0;
		force_apply_ocap_enable = 1;
		break;
	case 3:
		eas_opt_enable = 1;
		eas_opt_debug_enable = 1;
		force_apply_ocap_enable = 0;
		break;
	case 4:
		eas_opt_enable = 1;
		eas_opt_debug_enable = 1;
		force_apply_ocap_enable = 1;
		break;
	default:
		return -ERANGE;
	}

	return count;
}

static ssize_t eas_opt_enable_read(struct file *file, char __user *buffer,
		size_t count, loff_t *ppos)
{
	char output[EAS_OPT_INPUT_SIZE];
	int length;

	length = scnprintf(output, sizeof(output),
			"eas_enable=%d eas_debug_enable=%d force_apply=%d\n",
			READ_ONCE(eas_opt_enable),
			READ_ONCE(eas_opt_debug_enable),
			READ_ONCE(force_apply_ocap_enable));
	return simple_read_from_buffer(buffer, count, ppos, output, length);
}

static const struct file_operations eas_opt_enable_fops = {
	.owner = THIS_MODULE,
	.read = eas_opt_enable_read,
	.write = eas_opt_enable_write,
	.llseek = default_llseek,
};

static int __init oplus_eas_opt_init(void)
{
	struct proc_dir_entry *directory;

	directory = proc_mkdir("eas_opt", NULL);
	if (!directory)
		return -ENOMEM;
	if (!proc_create("eas_opt_enable", 0666, directory,
			 &eas_opt_enable_fops))
		return -ENOMEM;
	if (oplus_cap_init(directory))
		return -ENOMEM;
	if (oplus_sched_assist_iowait_proc_init(directory))
		return -ENOMEM;

	pr_info("Oplus EAS optimization controls initialized\n");
	return 0;
}
module_init(oplus_eas_opt_init);

MODULE_DESCRIPTION("Oplus Android 14 EAS optimization control ABI");
MODULE_LICENSE("GPL v2");
