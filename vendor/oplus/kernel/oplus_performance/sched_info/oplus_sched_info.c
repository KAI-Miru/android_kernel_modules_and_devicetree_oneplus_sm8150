// SPDX-License-Identifier: GPL-2.0-only
/*
 * Android 14 ColorOS CPU-jank control plane for Miru H.40.
 *
 * This is deliberately a 4.14-native backend: it samples the live CPU idle
 * counters and current tasks instead of importing OP9R's SM8250-only masks,
 * task extensions, and netlink reporter.  The exported proc commands retain
 * their Android 14 selector ABI.
 */
#include <linux/bitops.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/tick.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include "osi_base.h"
#include "osi_cpuload.h"
#include "osi_enable.h"
#include "osi_hotthread.h"
#include "osi_loadindicator.h"
#include "osi_onlinecpu.h"
#include "osi_topology.h"
#include "osi_version.h"
#include "tasktrack.h"

#define JANK_INFO_DIR                   "jank_info"
#define JANK_INFO_PROC_NODE             "cpu_jank_info"
#define CLM_PROC_NUMBUF                 32

/*
 * ColorOS encodes one mux selector above bit 7 and its enable state in bit 0.
 * For example, 513 (0x201) enables PEROID_GRAB_BIT and 512 disables it.
 */
#define FUNCTION_BITS                   8
#define ACTIVE_GRAB_BIT                 0
#define PEROID_GRAB_BIT                 1
#define PERSEC_REPORT_SWITCH            2
#define NOTIFY_CPUSET_BIT               3
#define MAX_SWITCH_NUM                  8

#define CLM_MONITOR_PERIOD_US           1000000
#define ACTIVE_GRABTHREAD_DURATION      2000000

struct clm_cpu_sample {
	u64 idle_time_us;
	u64 wall_time_us;
	unsigned int busy_percent;
	pid_t pid;
	pid_t tgid;
	char comm[TASK_COMM_LEN];
};

static struct proc_dir_entry *jank_dir;
static struct proc_dir_entry *cpu_jank_dir;
bool jankinfo_init;
EXPORT_SYMBOL(jankinfo_init);
static DEFINE_MUTEX(clm_lock);
static struct delayed_work clm_monitor_work;
static struct delayed_work grab_hotthread_work;

static unsigned int clm_enable;
static unsigned int fg_freqs_threshold = 75;
static unsigned int clm_highload_all = 98;
static unsigned int clm_highload_grp = 95;
static unsigned int clm_report_threshold = 1;
static unsigned int clm_lowload_grp = 75;
static unsigned int bg_dstat_percent = 30;
static unsigned int clm_mux_switch;
static bool control_array[MAX_SWITCH_NUM];

static struct clm_cpu_sample clm_cpu_samples[NR_CPUS];
static cpumask_t clm_highload_cpus;
static unsigned long clm_sample_generation;

static bool clm_monitor_enabled_locked(void)
{
	return clm_enable || control_array[PEROID_GRAB_BIT];
}

static void clm_capture_cpu_locked(unsigned int cpu, struct cpumask *highload)
{
	struct clm_cpu_sample *sample = &clm_cpu_samples[cpu];
	struct task_struct *task = NULL;
	u64 idle_time;
	u64 wall_time = 0;
	u64 wall_delta;
	u64 idle_delta;
	u64 busy_delta;

	if (!cpu_online(cpu))
		return;

	idle_time = get_cpu_idle_time_us(cpu, &wall_time);
	if (idle_time == (u64)-1 || wall_time == (u64)-1)
		return;

	if (sample->wall_time_us && wall_time > sample->wall_time_us) {
		wall_delta = wall_time - sample->wall_time_us;
		idle_delta = idle_time - sample->idle_time_us;
		busy_delta = wall_delta > idle_delta ? wall_delta - idle_delta : 0;
		sample->busy_percent = min_t(u64,
			(busy_delta * 100) / wall_delta, 100);
		if (sample->busy_percent >= clm_highload_all)
			cpumask_set_cpu(cpu, highload);
	}

	sample->idle_time_us = idle_time;
	sample->wall_time_us = wall_time;

	rcu_read_lock();
	task = cpu_rq(cpu)->curr;
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	if (!task)
		return;

	sample->pid = task->pid;
	sample->tgid = task->tgid;
	strlcpy(sample->comm, task->comm, sizeof(sample->comm));
	put_task_struct(task);
}

static void clm_capture_all_locked(void)
{
	unsigned int cpu;
	cpumask_t highload;

	cpumask_clear(&highload);
	for_each_online_cpu(cpu)
		clm_capture_cpu_locked(cpu, &highload);

	cpumask_copy(&clm_highload_cpus, &highload);
	clm_sample_generation++;

	/*
	 * OP9R forwards this event through an SM8250-specific netlink stack.
	 * H.40 reports the same real detection event through the native kernel
	 * log when the corresponding mux selector is enabled.
	 */
	if (control_array[PERSEC_REPORT_SWITCH] && !cpumask_empty(&highload) &&
	    cpumask_weight(&highload) >= max(1U, clm_report_threshold))
		pr_info_ratelimited("oplus_jank: sample=%lu highload_cpus=%*pbl\n",
			clm_sample_generation, cpumask_pr_args(&highload));
}

static void clm_monitor_workfn(struct work_struct *work)
{
	mutex_lock(&clm_lock);
	if (clm_monitor_enabled_locked()) {
		clm_capture_all_locked();
		schedule_delayed_work(&clm_monitor_work,
			usecs_to_jiffies(CLM_MONITOR_PERIOD_US));
	}
	mutex_unlock(&clm_lock);
}

static void grab_hotthread_workfn(struct work_struct *work)
{
	mutex_lock(&clm_lock);
	if (control_array[ACTIVE_GRAB_BIT]) {
		/*
		 * Capture current tasks as the H.40 equivalent of OP9R's active
		 * hot-thread grab; no SM8250 cpuset mask is assumed.
		 */
		clm_capture_all_locked();
		schedule_delayed_work(&grab_hotthread_work,
			usecs_to_jiffies(ACTIVE_GRABTHREAD_DURATION));
	}
	mutex_unlock(&clm_lock);
}

static ssize_t clm_value_read(unsigned int *value, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[CLM_PROC_NUMBUF];
	size_t len;

	mutex_lock(&clm_lock);
	len = scnprintf(buffer, sizeof(buffer), "%u\n", *value);
	mutex_unlock(&clm_lock);
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t clm_value_write(unsigned int *value, const char __user *buf,
		size_t count)
{
	char buffer[CLM_PROC_NUMBUF];
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

static ssize_t proc_clm_enable_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	return clm_value_read(&clm_enable, buf, count, ppos);
}

static ssize_t proc_clm_enable_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	ssize_t ret;
	bool active_enabled;
	bool monitor_enabled;

	ret = clm_value_write(&clm_enable, buf, count);
	if (ret < 0)
		return ret;

	mutex_lock(&clm_lock);
	monitor_enabled = clm_monitor_enabled_locked();
	active_enabled = control_array[ACTIVE_GRAB_BIT];
	if (monitor_enabled)
		schedule_delayed_work(&clm_monitor_work,
			usecs_to_jiffies(CLM_MONITOR_PERIOD_US));
	osi_hotthread_set_enabled(monitor_enabled || active_enabled);
	mutex_unlock(&clm_lock);

	if (!monitor_enabled)
		cancel_delayed_work_sync(&clm_monitor_work);
	return ret;
}

static const struct file_operations proc_clm_enable_fops = {
	.read = proc_clm_enable_read,
	.write = proc_clm_enable_write,
	.llseek = default_llseek,
};

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
	char buffer[CLM_PROC_NUMBUF];
	unsigned int state;
	unsigned int function_bits;
	unsigned int index;
	bool active_enabled;
	bool monitor_enabled;
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
	if (!function_bits || ffs(function_bits) > MAX_SWITCH_NUM)
		goto done;

	/* Match the donor's first-set-bit selector interpretation. */
	index = ffs(function_bits) - 1;
	control_array[index] = !!(state & 1);

	if (index == NOTIFY_CPUSET_BIT && control_array[index]) {
		/*
		 * The donor queues an SM8250 cpuset/netlink notification.  Capture
		 * fresh topology-independent H.40 data instead, then consume this
		 * one-shot selector exactly as the donor consumes its notification.
		 */
		clm_capture_all_locked();
		control_array[index] = false;
	}

	active_enabled = control_array[ACTIVE_GRAB_BIT];
	monitor_enabled = clm_monitor_enabled_locked();
	if (active_enabled)
		schedule_delayed_work(&grab_hotthread_work,
			usecs_to_jiffies(ACTIVE_GRABTHREAD_DURATION));
	if (monitor_enabled)
		schedule_delayed_work(&clm_monitor_work,
			usecs_to_jiffies(CLM_MONITOR_PERIOD_US));
	osi_hotthread_set_enabled(monitor_enabled || active_enabled);
	mutex_unlock(&clm_lock);

	if (!active_enabled)
		cancel_delayed_work_sync(&grab_hotthread_work);
	if (!monitor_enabled)
		cancel_delayed_work_sync(&clm_monitor_work);
	return count;

done:
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
	bool base_proc_ready = false;
	bool enable_proc_ready = false;
	bool tasktrack_proc_ready = false;
	bool hotthread_proc_ready = false;
	int ret;

	cluster_init();
	jank_cpuload_init();
	jank_onlinecpu_reset();

	INIT_DELAYED_WORK(&clm_monitor_work, clm_monitor_workfn);
	INIT_DELAYED_WORK(&grab_hotthread_work, grab_hotthread_workfn);

	ret = tasktrack_init();
	if (ret)
		return ret;

	jank_dir = proc_mkdir(JANK_INFO_DIR, NULL);
	if (!jank_dir)
		return -ENOMEM;
	cpu_jank_dir = proc_mkdir(JANK_INFO_PROC_NODE, jank_dir);
	if (!cpu_jank_dir)
		goto err_jank_dir;
	ret = osi_hotthread_proc_init(cpu_jank_dir);
	if (ret)
		goto err_proc;
	hotthread_proc_ready = true;
	ret = osi_base_proc_init(cpu_jank_dir);
	if (ret)
		goto err_proc;
	base_proc_ready = true;
	entry = jank_enable_proc_init(cpu_jank_dir);
	if (!entry)
		goto err_proc;
	enable_proc_ready = true;

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
	ret = tasktrack_proc_init(cpu_jank_dir);
	if (ret)
		goto err_proc;
	tasktrack_proc_ready = true;
	entry = jank_load_indicator_proc_init(cpu_jank_dir);
	if (!entry)
		goto err_proc;
	entry = jank_cpuload_proc_init(cpu_jank_dir);
	if (!entry)
		goto err_load_indicator;
	entry = jank_version_proc_init(cpu_jank_dir);
	if (!entry)
		goto err_cpuload;
	jankinfo_init = true;

	return 0;

err_cpuload:
	jank_cpuload_proc_deinit(cpu_jank_dir);
err_load_indicator:
	jank_load_indicator_proc_deinit(cpu_jank_dir);

err_proc:
	if (enable_proc_ready)
		jank_enable_proc_deinit(cpu_jank_dir);
	if (base_proc_ready)
		osi_base_proc_deinit(cpu_jank_dir);
	if (hotthread_proc_ready)
		osi_hotthread_proc_deinit(cpu_jank_dir);
	if (tasktrack_proc_ready)
		tasktrack_proc_deinit(cpu_jank_dir);
	tasktrack_deinit();
	remove_proc_subtree(JANK_INFO_DIR, NULL);
	jank_dir = NULL;
	cpu_jank_dir = NULL;
	return -ENOMEM;
err_jank_dir:
	tasktrack_deinit();
	remove_proc_entry(JANK_INFO_DIR, NULL);
	jank_dir = NULL;
	return -ENOMEM;
}

static void __exit jank_info_exit(void)
{
	jankinfo_init = false;
	jank_version_proc_deinit(cpu_jank_dir);
	jank_cpuload_proc_deinit(cpu_jank_dir);
	jank_load_indicator_proc_deinit(cpu_jank_dir);
	jank_enable_proc_deinit(cpu_jank_dir);
	osi_base_proc_deinit(cpu_jank_dir);
	osi_hotthread_proc_deinit(cpu_jank_dir);
	tasktrack_proc_deinit(cpu_jank_dir);
	tasktrack_deinit();
	cancel_delayed_work_sync(&grab_hotthread_work);
	cancel_delayed_work_sync(&clm_monitor_work);
	remove_proc_subtree(JANK_INFO_DIR, NULL);
}

module_init(jank_info_init);
module_exit(jank_info_exit);

MODULE_DESCRIPTION("Miru H.40 Android 14 CPU-jank proc ABI");
MODULE_LICENSE("GPL v2");
