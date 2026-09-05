// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2020 Oplus. All rights reserved. */

#define pr_fmt(fmt) "oplus_ua: " fmt

#include <linux/bitops.h>
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#include "frame_group.h"
#include "touch_ioctl.h"
#include "ua_ioctl_common.h"

#define UA_PROC_NODE "oplus_cpu"
#define UA_CTRL_MODE 0777

static struct proc_dir_entry *ua_common_proc;
static unsigned long ua_status_bits;

static int set_ua_status(void __user *uarg)
{
	struct ua_info_data info;

	if (!uarg)
		return -EINVAL;

	if (copy_from_user(&info, uarg, sizeof(info)))
		return -EFAULT;

	if (info.status >= 0 && info.status < UA_MAX_STATUS)
		set_bit(info.status, &ua_status_bits);

	return 0;
}

static int get_util_info(void __user *uarg)
{
	struct ua_info_data info;

	if (!uarg)
		return -EINVAL;

	if (copy_from_user(&info, uarg, sizeof(info)))
		return -EFAULT;

	info.frame_prev_util_scale = 0;
	info.frame_curr_util_scale = 0;
	fbg_get_prev_util(&info.frame_prev_util_scale);
	fbg_get_curr_util(&info.frame_curr_util_scale);

	if (copy_to_user(uarg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static long ua_ctrl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	if (_IOC_TYPE(cmd) != CPU_CTRL_MAGIC ||
	    _IOC_NR(cmd) >= CPU_CTRL_CMD_MAX)
		return -EINVAL;

	switch (cmd) {
	case CPU_CTRL_SET_UA_STATUS:
		return set_ua_status(uarg);
	case CPU_CTRL_GET_UTIL_INFO:
		return get_util_info(uarg);
	default:
		return -EINVAL;
	}
}

#ifdef CONFIG_COMPAT
static long ua_ctrl_compat_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	return ua_ctrl_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations ua_ctrl_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = ua_ctrl_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ua_ctrl_compat_ioctl,
#endif
	.llseek = no_llseek,
};

int ua_ioctl_init(void)
{
	struct proc_dir_entry *entry;
	int ret;

	ua_common_proc = proc_mkdir(UA_PROC_NODE, NULL);
	if (!ua_common_proc)
		return -ENOMEM;

	entry = proc_create("ua_ctrl", UA_CTRL_MODE, ua_common_proc,
			    &ua_ctrl_fops);
	if (!entry) {
		ret = -ENOMEM;
		goto remove_ua_dir;
	}

	ret = touch_ioctl_init();
	if (ret)
		goto remove_ua_ctrl;

	return 0;

remove_ua_ctrl:
	remove_proc_entry("ua_ctrl", ua_common_proc);
remove_ua_dir:
	remove_proc_entry(UA_PROC_NODE, NULL);
	ua_common_proc = NULL;
	return ret;
}

void ua_ioctl_exit(void)
{
	touch_ioctl_exit();
	if (!ua_common_proc)
		return;

	remove_proc_entry("ua_ctrl", ua_common_proc);
	remove_proc_entry(UA_PROC_NODE, NULL);
	ua_common_proc = NULL;
}
