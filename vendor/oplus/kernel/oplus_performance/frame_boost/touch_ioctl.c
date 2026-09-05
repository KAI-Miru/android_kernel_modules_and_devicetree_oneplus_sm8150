// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2020 Oplus. All rights reserved. */

#include <linux/fs.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "touch_ioctl.h"

#define TOUCHBOOST_PROC_NODE "oplus_touch_boost"

static struct proc_dir_entry *touch_boost_proc;
static DEFINE_SPINLOCK(touch_event_lock);
static int current_touch_event;
static bool touch_handler_registered;

static void touchboost_input_event(struct input_handle *handle,
				   unsigned int type, unsigned int code, int value)
{
	unsigned long flags;

	if (type != EV_KEY || code != BTN_TOUCH || (value != 0 && value != 1))
		return;

	spin_lock_irqsave(&touch_event_lock, flags);
	current_touch_event = value;
	spin_unlock_irqrestore(&touch_event_lock, flags);
}

static int touchboost_input_connect(struct input_handler *handler,
				    struct input_dev *dev,
				    const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "touch_boost_cpufreq";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void touchboost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static bool touchboost_input_match(struct input_handler *handler, struct input_dev *dev)
{
	static const char touchpanel_name[] = "touchpanel";

	return dev->name &&
		!strncmp(touchpanel_name, dev->name, strlen(touchpanel_name));
}

static const struct input_device_id touchboost_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler touchboost_input_handler = {
	.event = touchboost_input_event,
	.match = touchboost_input_match,
	.connect = touchboost_input_connect,
	.disconnect = touchboost_input_disconnect,
	.name = "touch_boost",
	.id_table = touchboost_ids,
};

static int touch_info_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int event;

	spin_lock_irqsave(&touch_event_lock, flags);
	event = current_touch_event;
	spin_unlock_irqrestore(&touch_event_lock, flags);

	seq_printf(m, "%d\n", event);
	return 0;
}

static int touch_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, touch_info_show, NULL);
}

static const struct file_operations touch_info_fops = {
	.owner = THIS_MODULE,
	.open = touch_info_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int touch_ioctl_init(void)
{
	struct proc_dir_entry *entry;
	int ret;

	touch_boost_proc = proc_mkdir(TOUCHBOOST_PROC_NODE, NULL);
	if (!touch_boost_proc)
		return -ENOMEM;

	entry = proc_create("touch_info", 0444, touch_boost_proc,
			    &touch_info_fops);
	if (!entry) {
		ret = -ENOMEM;
		goto remove_touch_dir;
	}

	ret = input_register_handler(&touchboost_input_handler);
	if (ret)
		goto remove_touch_info;
	touch_handler_registered = true;

	return 0;

remove_touch_info:
	remove_proc_entry("touch_info", touch_boost_proc);
remove_touch_dir:
	remove_proc_entry(TOUCHBOOST_PROC_NODE, NULL);
	touch_boost_proc = NULL;
	return ret;
}

void touch_ioctl_exit(void)
{
	if (touch_handler_registered) {
		input_unregister_handler(&touchboost_input_handler);
		touch_handler_registered = false;
	}

	if (!touch_boost_proc)
		return;

	remove_proc_entry("touch_info", touch_boost_proc);
	remove_proc_entry(TOUCHBOOST_PROC_NODE, NULL);
	touch_boost_proc = NULL;
}
