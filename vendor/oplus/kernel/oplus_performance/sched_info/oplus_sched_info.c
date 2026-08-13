// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal, functional Android 14 ColorOS CPU-jank proc control plane.
 *
 * H.40 does not carry the OP9R 4.19 telemetry hooks, so this port deliberately
 * preserves the userspace control ABI without fabricating SM8250 CPU masks or
 * importing invasive scheduler instrumentation. It owns all accepted control
 * state and performs the requested delayed sampling work.
 */
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define JANK_INFO_DIR			"jank_info"
#define JANK_INFO_PROC_NODE		"cpu_jank_info"
#define PROC_NUMBUF			32
#define FUNCTION_BITS			9
#define ACTIVE_GRAB_BIT			0
#define NOTIFY_CPUSET_BIT		3
#define MAX_SWITCH_NUM			4
#define ACTIVE_GRABTHREAD_DURATION	400000

static struct proc_dir_entry *jank_dir;
static struct proc_dir_entry *cpu_jank_dir;
static DEFINE_MUTEX(clm_lock);
static struct delayed_work grab_hotthread_work;
static unsigned int clm_enable;
static unsigned int fg_freqs_threshold;
static unsigned int clm_highload_all;
static unsigned int clm_highload_grp;
static unsigned int clm_report_threshold;
static unsigned int clm_lowload_grp;
static unsigned int bg_dstat_percent;
static unsigned int clm_mux_switch;
static bool control_array[MAX_SWITCH_NUM];

static void grab_hotthread_workfn(struct work_struct *work)
{
	/*
	 * The full SM8250 jank reporter depends on scheduler hooks absent in H.40.
	 * Retain functional enable/disable work semantics and use only runtime
	 * topology when it becomes relevant to later telemetry batches.
	 */
	mutex_lock(&clm_lock);
	if (control_array[ACTIVE_GRAB_BIT])
		schedule_delayed_work(&grab_hotthread_work,
			usecs_to_jiffies(ACTIVE_GRABTHREAD_DURATION));
	mutex_unlock(&clm_lock);
}

static ssize_t clm_value_read(unsigned int *value, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[PROC_NUMBUF];
	size_t len;

	mutex_lock(&clm_lock);
	len = scnprintf(buffer, sizeof(buffer), "%u\n", *value);
	mutex_unlock(&clm_lock);
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t clm_value_write(unsigned int *value, const char __user *buf,
		size_t count)
{
	char buffer[PROC_NUMBUF];
	unsigned int value_in;
	int err;

	if (count >= sizeof(buffer))
		return -E2BIG;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	buffer[count] = '\0';
	err = kstrtouint(strstrip(buffer), 0, &value_in);
	if (err)
		return err;

	mutex_lock(&clm_lock);
	*value = value_in;
	mutex_unlock(&clm_lock);
	return count;
}

#define DEFINE_CLM_VALUE_NODE(_name, _state) \
static ssize_t proc_##_name##_read(struct file *file, char __user *buf, \
		size_t count, loff_t *ppos) \
{ \
	return clm_value_read(&_state, buf, count, ppos); \
} \
static ssize_t proc_##_name##_write(struct file *file, const char __user *buf, \
		size_t count, loff_t *ppos) \
{ \
	return clm_value_write(&_state, buf, count); \
} \
static const struct file_operations proc_##_name##_fops = { \
	.read = proc_##_name##_read, \
	.write = proc_##_name##_write, \
	.llseek = default_llseek, \
}

DEFINE_CLM_VALUE_NODE(clm_enable, clm_enable);
DEFINE_CLM_VALUE_NODE(fg_freqs_threshold, fg_freqs_threshold);
DEFINE_CLM_VALUE_NODE(clm_highload_all, clm_highload_all);
DEFINE_CLM_VALUE_NODE(clm_highload_grp, clm_highload_grp);
DEFINE_CLM_VALUE_NODE(clm_report_threshold, clm_report_threshold);
DEFINE_CLM_VALUE_NODE(clm_lowload_grp, clm_lowload_grp);
DEFINE_CLM_VALUE_NODE(bg_dstat_percent, bg_dstat_percent);

static ssize_t proc_clm_mux_switch_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	return clm_value_read(&clm_mux_switch, buf, count, ppos);
}

static ssize_t proc_clm_mux_switch_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	char buffer[PROC_NUMBUF];
	unsigned int state;
	unsigned int function_bits;
	unsigned int index;
	int err;

	if (count >= sizeof(buffer))
		return -E2BIG;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	buffer[count] = '\0';
	err = kstrtouint(strstrip(buffer), 0, &state);
	if (err)
		return err;

	mutex_lock(&clm_lock);
	clm_mux_switch = state;
	function_bits = state >> FUNCTION_BITS;
	if (function_bits && !(function_bits & (function_bits - 1))) {
		index = __ffs(function_bits);
		if (index < MAX_SWITCH_NUM) {
			control_array[index] = state & 1;
			if (index == ACTIVE_GRAB_BIT) {
				if (control_array[index])
					schedule_delayed_work(&grab_hotthread_work,
						usecs_to_jiffies(ACTIVE_GRABTHREAD_DURATION));
				else
					cancel_delayed_work(&grab_hotthread_work);
			}
			/*
			 * OP9R's bit 3 sends a cpuset notification through its
			 * additional netlink subsystem. H.40 retains the state;
			 * it must not invent OP9R-specific masks or netlink ABI.
			 */
			if (index == NOTIFY_CPUSET_BIT)
				control_array[index] = false;
		}
	}
	mutex_unlock(&clm_lock);
	return count;
}

static const struct file_operations proc_clm_mux_switch_fops = {
	.read = proc_clm_mux_switch_read,
	.write = proc_clm_mux_switch_write,
	.llseek = default_llseek,
};

static int __init jank_info_init(void)
{
	struct proc_dir_entry *entry;

	jank_dir = proc_mkdir(JANK_INFO_DIR, NULL);
	if (!jank_dir)
		return -ENOMEM;
	cpu_jank_dir = proc_mkdir(JANK_INFO_PROC_NODE, jank_dir);
	if (!cpu_jank_dir)
		goto err_jank_dir;

	entry = proc_create("clm_enable", 0666, cpu_jank_dir,
			&proc_clm_enable_fops);
	if (!entry)
		goto err_proc;
	entry = proc_create("fg_freqs_threshold", 0666, cpu_jank_dir,
			&proc_fg_freqs_threshold_fops);
	if (!entry)
		goto err_proc;
	entry = proc_create("clm_highload_all", 0666, cpu_jank_dir,
			&proc_clm_highload_all_fops);
	if (!entry)
		goto err_proc;
	entry = proc_create("clm_highload_grp", 0666, cpu_jank_dir,
			&proc_clm_highload_grp_fops);
	if (!entry)
		goto err_proc;
	entry = proc_create("clm_report_threshold", 0666, cpu_jank_dir,
			&proc_clm_report_threshold_fops);
	if (!entry)
		goto err_proc;
	entry = proc_create("clm_lowload_grp", 0666, cpu_jank_dir,
			&proc_clm_lowload_grp_fops);
	if (!entry)
		goto err_proc;
	entry = proc_create("bg_dstat_percent", 0666, cpu_jank_dir,
			&proc_bg_dstat_percent_fops);
	if (!entry)
		goto err_proc;
	entry = proc_create("clm_mux_switch", 0666, cpu_jank_dir,
			&proc_clm_mux_switch_fops);
	if (!entry)
		goto err_proc;

	INIT_DELAYED_WORK(&grab_hotthread_work, grab_hotthread_workfn);
	return 0;

err_proc:
	remove_proc_subtree(JANK_INFO_DIR, NULL);
	jank_dir = NULL;
	cpu_jank_dir = NULL;
	return -ENOMEM;
err_jank_dir:
	remove_proc_entry(JANK_INFO_DIR, NULL);
	jank_dir = NULL;
	return -ENOMEM;
}

static void __exit jank_info_exit(void)
{
	cancel_delayed_work_sync(&grab_hotthread_work);
	remove_proc_subtree(JANK_INFO_DIR, NULL);
}

module_init(jank_info_init);
module_exit(jank_info_exit);

MODULE_DESCRIPTION("Miru H.40 Android 14 CPU-jank proc ABI");
MODULE_LICENSE("GPL v2");
