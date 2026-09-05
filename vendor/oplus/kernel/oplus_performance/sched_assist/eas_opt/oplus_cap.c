// SPDX-License-Identifier: GPL-2.0-only
/* Android 14 OnePlus 9R EAS capacity policy, adapted for Linux 4.14. */

#include <linux/arch_topology.h>
#include <linux/cgroup.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include <../kernel/sched/sched.h>

#include "../sched_assist_common.h"
#include "oplus_cap.h"

#define EAS_INPUT_SIZE 160
#define EAS_OUTPUT_SIZE 320
#define EAS_GOLD_PLUS_CAPACITY 1024

enum oplus_adjust_group {
	AD_TOP,
	AD_FG,
	AD_BG,
	AD_DF,
	AD_MAX_GROUP,
};

struct oplus_group_adjust_param {
	int util_compensate;
	unsigned long vtime_compensate;
};

struct oplus_groups_adjust {
	int task_compensate;
	u64 adjust_std_vtime_slice;
	struct oplus_group_adjust_param group_param[AD_MAX_GROUP];
};

static const struct oplus_groups_adjust group_adjust_defaults = {
	.task_compensate = 0,
	.adjust_std_vtime_slice = 75,
	.group_param = {
		{ .util_compensate = 0, .vtime_compensate = 200 },
		{ .util_compensate = 0, .vtime_compensate = 150 },
		{ .util_compensate = 0, .vtime_compensate = 0 },
		{ .util_compensate = 0, .vtime_compensate = 50 },
	},
};

static struct oplus_groups_adjust group_adjust = {
	.task_compensate = 0,
	.adjust_std_vtime_slice = 75,
	.group_param = {
		{ .util_compensate = 0, .vtime_compensate = 200 },
		{ .util_compensate = 0, .vtime_compensate = 150 },
		{ .util_compensate = 0, .vtime_compensate = 0 },
		{ .util_compensate = 0, .vtime_compensate = 50 },
	},
};

int sa_adjust_group_enable;
int oplus_cap_multiple[OPLUS_CLUSTERS] = { 100, 100, 100, 100, 100 };
int nr_oplus_cap_multiple[OPLUS_CLUSTERS] = { 100, 100, 100, 100, 100 };
int util_thresh_percent[OPLUS_CLUSTERS] = { 100, 100, 100, 100, 100 };
int util_thresh_cvt[OPLUS_CLUSTERS] = { 1000, 1000, 1000, 1000, 1000 };
unsigned long real_cpu_cap[NR_CPUS];
EXPORT_SYMBOL(sa_adjust_group_enable);
EXPORT_SYMBOL(oplus_cap_multiple);
EXPORT_SYMBOL(nr_oplus_cap_multiple);
EXPORT_SYMBOL(util_thresh_percent);
EXPORT_SYMBOL(util_thresh_cvt);
EXPORT_SYMBOL(real_cpu_cap);

static int oplus_cluster_id(int cpu)
{
	int cluster = cpu_topology[cpu].cluster_id;

	if (cluster < 0 || cluster >= OPLUS_CLUSTERS)
		return -1;
	return cluster;
}

static int oplus_group_id(struct task_struct *task)
{
	struct cgroup_subsys_state *css;
	int id = AD_DF;

	if (!task)
		return id;

	rcu_read_lock();
	css = task_css(task, schedtune_cgrp_id);
	if (css) {
		switch (css->id) {
		case 4:
			id = AD_TOP;
			break;
		case 2:
			id = AD_FG;
			break;
		case 3:
			id = AD_BG;
			break;
		default:
			id = AD_DF;
			break;
		}
	}
	rcu_read_unlock();

	return id;
}

static unsigned long oplus_task_util(struct task_struct *task)
{
	unsigned long util = READ_ONCE(task->se.avg.util_avg);
	struct util_est estimate = READ_ONCE(task->se.avg.util_est);

	return max(util, max(estimate.ewma, estimate.enqueued & 0x7fffffff));
}

static unsigned long oplus_cpu_util_without(int cpu, struct task_struct *task)
{
	struct cfs_rq *cfs_rq = &cpu_rq(cpu)->cfs;
	unsigned long util = READ_ONCE(cfs_rq->avg.util_avg);
	unsigned long task_util;

	if (cpu != task_cpu(task) || !READ_ONCE(task->se.avg.last_update_time))
		return max(util, READ_ONCE(cfs_rq->avg.util_est.enqueued));

	task_util = READ_ONCE(task->se.avg.util_avg);
	util -= min(util, task_util);
	if (sched_feat(UTIL_EST)) {
		unsigned long estimated = READ_ONCE(cfs_rq->avg.util_est.enqueued);

		if (task_on_rq_queued(task) || current == task) {
			unsigned long task_est = oplus_task_util(task);

			estimated -= min(estimated, task_est);
		}
		util = max(util, estimated);
	}
	return util;
}

bool oplus_eas_task_skip_cpu(struct task_struct *task, int cpu)
{
	unsigned long capacity;
	unsigned long util;
	unsigned long cpu_util;
	int margin;
	int group;

	if (!READ_ONCE(eas_opt_enable) || !READ_ONCE(sa_adjust_group_enable))
		return false;
	if (arch_scale_cpu_capacity(NULL, cpu) >= EAS_GOLD_PLUS_CAPACITY)
		return false;

	capacity = READ_ONCE(real_cpu_cap[cpu]);
	if (!capacity)
		capacity = arch_scale_cpu_capacity(NULL, cpu);
	util = oplus_task_util(task);
	cpu_util = oplus_cpu_util_without(cpu, task);
	group = oplus_group_id(task);
	margin = READ_ONCE(group_adjust.group_param[group].util_compensate);

	return READ_ONCE(group_adjust.task_compensate) + util > capacity ||
		margin + util + cpu_util > capacity;
}
EXPORT_SYMBOL(oplus_eas_task_skip_cpu);

void oplus_eas_place_entity(struct cfs_rq *cfs_rq,
		struct sched_entity *se, int initial)
{
	struct task_struct *task;
	unsigned long multiplier;
	u64 adjustment;
	int cluster;
	int group;

	if (!READ_ONCE(sa_adjust_group_enable) || initial ||
	    !oplus_entity_is_task(se))
		return;
	cluster = oplus_cluster_id(cpu_of(rq_of(cfs_rq)));
	if (cluster < 0)
		return;
	multiplier = READ_ONCE(oplus_cap_multiple[cluster]);
	if (multiplier <= 100)
		return;

	task = container_of(se, struct task_struct, se);
	if (test_task_ux(task))
		return;
	group = oplus_group_id(task);
	adjustment = READ_ONCE(group_adjust.adjust_std_vtime_slice) *
		READ_ONCE(group_adjust.group_param[group].vtime_compensate) *
		multiplier;
	adjustment = min(adjustment, se->vruntime);
	se->vruntime -= adjustment;
}
EXPORT_SYMBOL(oplus_eas_place_entity);

unsigned long oplus_eas_adjust_capacity(int cpu, unsigned long capacity)
{
	int cluster = oplus_cluster_id(cpu);
	int multiplier;

	WRITE_ONCE(real_cpu_cap[cpu], capacity);
	if (!READ_ONCE(eas_opt_enable) || cluster < 0)
		return capacity;
	multiplier = READ_ONCE(oplus_cap_multiple[cluster]);
	return mult_frac(capacity, multiplier, 100);
}
EXPORT_SYMBOL(oplus_eas_adjust_capacity);

unsigned long oplus_eas_adjust_util(int cpu, unsigned int nr_running,
		unsigned long util, unsigned long max_capacity)
{
	unsigned long threshold;
	unsigned long scaled;
	int cluster = oplus_cluster_id(cpu);

	if (!READ_ONCE(eas_opt_enable) || cluster < 0)
		return util;
	if (READ_ONCE(util_thresh_percent[cluster]) == 100 &&
		READ_ONCE(oplus_cap_multiple[cluster]) == 100)
		return util;

	threshold = max_capacity * READ_ONCE(util_thresh_cvt[cluster]) >> 10;
	if (util <= threshold)
		return util;
	scaled = threshold + mult_frac(nr_running * (util - threshold),
				       READ_ONCE(nr_oplus_cap_multiple[cluster]),
				       1024);
	return min(scaled, max_capacity);
}
EXPORT_SYMBOL(oplus_eas_adjust_util);

static int oplus_parse_values(char *input, int *values, int maximum)
{
	char *token;
	int count = 0;
	int ret;

	while ((token = strsep(&input, " \t\n")) != NULL) {
		if (!*token)
			continue;
		if (count >= maximum)
			return -E2BIG;
		ret = kstrtoint(token, 10, &values[count]);
		if (ret)
			return ret;
		count++;
	}
	return count;
}

static int oplus_copy_values(const char __user *buffer, size_t count,
		int *values, int maximum)
{
	char input[EAS_INPUT_SIZE];

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buffer, count))
		return -EFAULT;
	input[count] = '\0';
	return oplus_parse_values(input, values, maximum);
}

static ssize_t group_adjust_enable_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	int values[1];
	int parsed = oplus_copy_values(buffer, count, values, ARRAY_SIZE(values));

	if (parsed != 1)
		return parsed < 0 ? parsed : -EINVAL;
	if (values[0] < 0 || values[0] > 1)
		return -ERANGE;
	WRITE_ONCE(sa_adjust_group_enable, values[0]);
	return count;
}

static ssize_t group_adjust_enable_read(struct file *file,
		char __user *buffer, size_t count, loff_t *ppos)
{
	char output[64];
	int length = scnprintf(output, sizeof(output),
			"sa_adjust_group_enable=%d\n",
			READ_ONCE(sa_adjust_group_enable));

	return simple_read_from_buffer(buffer, count, ppos, output, length);
}

static ssize_t oplus_cap_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *ppos)
{
	int values[OPLUS_CLUSTERS];
	int parsed;
	int index;

	parsed = oplus_copy_values(buffer, count, values, ARRAY_SIZE(values));
	if (parsed < 0)
		return parsed;
	if (!parsed)
		return -EINVAL;
	if (values[0] == -1) {
		for (index = 0; index < OPLUS_CLUSTERS; index++)
			oplus_cap_multiple[index] = 100;
	} else {
		for (index = 0; index < parsed; index++) {
			if (values[index] < 1 || values[index] > 400)
				return -ERANGE;
		}
		for (index = 0; index < parsed; index++)
			WRITE_ONCE(oplus_cap_multiple[index], values[index]);
	}
	for (index = 0; index < OPLUS_CLUSTERS; index++)
		nr_oplus_cap_multiple[index] = mult_frac(
			oplus_cap_multiple[OPLUS_CLUSTERS - 1], 1024,
			oplus_cap_multiple[index]);
	return count;
}

static ssize_t oplus_cap_read(struct file *file, char __user *buffer,
		size_t count, loff_t *ppos)
{
	char output[EAS_OUTPUT_SIZE];
	int length = 0;
	int index;

	for (index = 0; index < OPLUS_CLUSTERS; index++)
		length += scnprintf(output + length, sizeof(output) - length,
				    "cluster%d:%d\n", index,
				    READ_ONCE(oplus_cap_multiple[index]));
	for (index = 0; index < min_t(int, OPLUS_CPUS, NR_CPUS); index++)
		length += scnprintf(output + length, sizeof(output) - length,
				    "cpu%d:%lu\n", index,
				    READ_ONCE(real_cpu_cap[index]));
	return simple_read_from_buffer(buffer, count, ppos, output, length);
}

static ssize_t util_thresh_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *ppos)
{
	int values[OPLUS_CLUSTERS];
	int parsed;
	int index;

	parsed = oplus_copy_values(buffer, count, values, ARRAY_SIZE(values));
	if (parsed < 0)
		return parsed;
	if (!parsed)
		return -EINVAL;
	if (values[0] == -1) {
		for (index = 0; index < OPLUS_CLUSTERS; index++) {
			util_thresh_percent[index] = 100;
			util_thresh_cvt[index] = 1000;
		}
		return count;
	}
	for (index = 0; index < parsed; index++) {
		if (values[index] < 0 || values[index] > 100)
			return -ERANGE;
	}
	for (index = 0; index < parsed; index++) {
		WRITE_ONCE(util_thresh_percent[index], values[index]);
		WRITE_ONCE(util_thresh_cvt[index], values[index] * 10);
	}
	return count;
}

static ssize_t util_thresh_read(struct file *file, char __user *buffer,
		size_t count, loff_t *ppos)
{
	char output[EAS_OUTPUT_SIZE];
	int length = 0;
	int index;

	for (index = 0; index < OPLUS_CLUSTERS; index++)
		length += scnprintf(output + length, sizeof(output) - length,
				    "%d:util_thresh_percent=%d\n", index,
				    READ_ONCE(util_thresh_percent[index]));
	return simple_read_from_buffer(buffer, count, ppos, output, length);
}

static void set_group_adjust_value(int index, int value)
{
	if (index == AD_MAX_GROUP * 2)
		group_adjust.task_compensate = value;
	else if (index == AD_MAX_GROUP * 2 + 1)
		group_adjust.adjust_std_vtime_slice = value;
	else if (index & 1)
		group_adjust.group_param[index >> 1].util_compensate = value;
	else
		group_adjust.group_param[index >> 1].vtime_compensate = value;
}

static ssize_t group_adjust_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	int values[AD_MAX_GROUP * 2 + 2];
	int parsed;
	int index;

	parsed = oplus_copy_values(buffer, count, values, ARRAY_SIZE(values));
	if (parsed < 0)
		return parsed;
	if (!parsed)
		return -EINVAL;
	if (values[0] == -1) {
		group_adjust = group_adjust_defaults;
		return count;
	}
	for (index = 0; index < parsed; index++) {
		if (values[index] < 0 || values[index] > 1000000)
			return -ERANGE;
	}
	for (index = 0; index < parsed; index++)
		set_group_adjust_value(index, values[index]);
	return count;
}

static ssize_t group_adjust_read(struct file *file, char __user *buffer,
		size_t count, loff_t *ppos)
{
	static const char * const names[] = { "top", "fg", "bg", "def" };
	char output[EAS_OUTPUT_SIZE];
	int length = 0;
	int index;

	for (index = 0; index < AD_MAX_GROUP; index++)
		length += scnprintf(output + length, sizeof(output) - length,
			"group_adjust_%s:%lu %d\n", names[index],
			READ_ONCE(group_adjust.group_param[index].vtime_compensate),
			READ_ONCE(group_adjust.group_param[index].util_compensate));
	length += scnprintf(output + length, sizeof(output) - length,
			"task_util_thresh:%d\nadjust_std_vtime_slice:%llu\n",
			READ_ONCE(group_adjust.task_compensate),
			(unsigned long long)
			READ_ONCE(group_adjust.adjust_std_vtime_slice));
	return simple_read_from_buffer(buffer, count, ppos, output, length);
}

static const struct file_operations group_adjust_enable_fops = {
	.owner = THIS_MODULE,
	.read = group_adjust_enable_read,
	.write = group_adjust_enable_write,
	.llseek = default_llseek,
};
static const struct file_operations oplus_cap_fops = {
	.owner = THIS_MODULE,
	.read = oplus_cap_read,
	.write = oplus_cap_write,
	.llseek = default_llseek,
};
static const struct file_operations util_thresh_fops = {
	.owner = THIS_MODULE,
	.read = util_thresh_read,
	.write = util_thresh_write,
	.llseek = default_llseek,
};
static const struct file_operations group_adjust_fops = {
	.owner = THIS_MODULE,
	.read = group_adjust_read,
	.write = group_adjust_write,
	.llseek = default_llseek,
};

int oplus_cap_init(struct proc_dir_entry *directory)
{
	int cpu;

	if (!proc_create("group_adjust_enable", 0666, directory,
			 &group_adjust_enable_fops) ||
	    !proc_create("oplus_cap_multiple", 0666, directory,
			 &oplus_cap_fops) ||
	    !proc_create("group_adjust", 0666, directory,
			 &group_adjust_fops) ||
	    !proc_create("util_thresh_percent", 0666, directory,
			 &util_thresh_fops))
		return -ENOMEM;

	for_each_possible_cpu(cpu)
		real_cpu_cap[cpu] = arch_scale_cpu_capacity(NULL, cpu);
	return 0;
}

MODULE_DESCRIPTION("Oplus Android 14 virtual capacity policy");
MODULE_LICENSE("GPL v2");
