// SPDX-License-Identifier: GPL-2.0-only
/*
 * Android 14 ColorOS AudioBoost support for the H.40 sched_assist backend.
 *
 * The 4.19 donor's RT-pull and cpuidle hooks do not exist in H.40.  This
 * implementation retains the parts that have real 4.14 scheduler hooks:
 * IM_FLAG_AUDIO maps to SA_TYPE_LIGHT and audio/status lowers timer slack on
 * every fair-class enqueue.
 */
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "sched_assist_audio.h"
#include "sched_assist_common.h"

#define AUDIO_PROC_NUMBUF 32
#define AUDIO_DEBUG_BUFSZ 1024
#define AUDIO_TIMER_SLACK_NS 50000
#define AUDIO_DEBUG_MAX_PIDS 128

struct audio_pid_node {
	pid_t pid;
	struct list_head list;
};

static int sa_audio_perf_enable = 1;
static int sa_audio_perf_status;
static int sa_audio_debug_enable;
static struct proc_dir_entry *audio_dir;
static LIST_HEAD(audio_debug_pids);
static DEFINE_SPINLOCK(audio_debug_lock);
static unsigned int audio_debug_pid_count;

static bool audio_perf_enabled(void)
{
	return READ_ONCE(sysctl_sched_assist_enabled) &&
		READ_ONCE(sa_audio_perf_enable);
}

static bool audio_perf_status_on(void)
{
	return audio_perf_enabled() && READ_ONCE(sa_audio_perf_status);
}

static bool task_is_audio(const struct task_struct *task)
{
	return task && READ_ONCE(task->ux_im_flag) == IM_FLAG_AUDIO;
}

static void audio_debug_add_pid(pid_t pid)
{
	struct audio_pid_node *node;
	struct audio_pid_node *entry;
	unsigned long flags;
	bool exists = false;

	node = kzalloc(sizeof(*node), GFP_ATOMIC);
	if (!node)
		return;
	node->pid = pid;

	spin_lock_irqsave(&audio_debug_lock, flags);
	list_for_each_entry(entry, &audio_debug_pids, list) {
		if (entry->pid == pid) {
			exists = true;
			break;
		}
	}
	if (!exists && audio_debug_pid_count < AUDIO_DEBUG_MAX_PIDS) {
		list_add_tail(&node->list, &audio_debug_pids);
		audio_debug_pid_count++;
		node = NULL;
	}
	spin_unlock_irqrestore(&audio_debug_lock, flags);
	kfree(node);
}

static void audio_debug_remove_pid(pid_t pid)
{
	struct audio_pid_node *node;
	struct audio_pid_node *tmp;
	struct audio_pid_node *remove = NULL;
	unsigned long flags;

	spin_lock_irqsave(&audio_debug_lock, flags);
	list_for_each_entry_safe(node, tmp, &audio_debug_pids, list) {
		if (node->pid == pid) {
			list_del(&node->list);
			audio_debug_pid_count--;
			remove = node;
			break;
		}
	}
	spin_unlock_irqrestore(&audio_debug_lock, flags);
	kfree(remove);
}

static void audio_debug_clear_pids(void)
{
	LIST_HEAD(dead);
	struct audio_pid_node *node;
	struct audio_pid_node *tmp;
	unsigned long flags;

	spin_lock_irqsave(&audio_debug_lock, flags);
	list_splice_init(&audio_debug_pids, &dead);
	audio_debug_pid_count = 0;
	spin_unlock_irqrestore(&audio_debug_lock, flags);

	list_for_each_entry_safe(node, tmp, &dead, list) {
		list_del(&node->list);
		kfree(node);
	}
}

static size_t audio_debug_show(char *buffer, size_t size)
{
	pid_t pids[AUDIO_DEBUG_MAX_PIDS];
	struct audio_pid_node *node;
	struct task_struct *task;
	unsigned long flags;
	unsigned int nr = 0;
	unsigned int i;
	size_t len = 0;

	spin_lock_irqsave(&audio_debug_lock, flags);
	list_for_each_entry(node, &audio_debug_pids, list) {
		if (nr == ARRAY_SIZE(pids))
			break;
		pids[nr++] = node->pid;
	}
	spin_unlock_irqrestore(&audio_debug_lock, flags);

	for (i = 0; i < nr; i++) {
		task = NULL;
		rcu_read_lock();
		task = find_task_by_vpid(pids[i]);
		if (task)
			get_task_struct(task);
		rcu_read_unlock();
		if (!task)
			continue;
		if (!task_is_audio(task)) {
			put_task_struct(task);
			continue;
		}

		len += scnprintf(buffer + len, size - len,
			"pid=%d comm=%s tgid=%d\n",
			task->pid, task->comm, task->tgid);
		put_task_struct(task);
		if (len >= size - 1)
			break;
	}
	return len;
}

static ssize_t audio_value_write(int *value, const char __user *buf,
		size_t count)
{
	char buffer[AUDIO_PROC_NUMBUF];
	int input;
	int err;

	if (count >= sizeof(buffer))
		return -E2BIG;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	buffer[count] = '\0';
	err = kstrtoint(strstrip(buffer), 10, &input);
	if (err)
		return err;

	WRITE_ONCE(*value, input);
	return count;
}

static ssize_t proc_audio_enable_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[AUDIO_PROC_NUMBUF];
	size_t len;

	len = scnprintf(buffer, sizeof(buffer), "enabled=%d\n",
		READ_ONCE(sa_audio_perf_enable));
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t proc_audio_enable_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	return audio_value_write(&sa_audio_perf_enable, buf, count);
}

static const struct file_operations proc_audio_enable_fops = {
	.read = proc_audio_enable_read,
	.write = proc_audio_enable_write,
	.llseek = default_llseek,
};

static ssize_t proc_audio_debug_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[AUDIO_DEBUG_BUFSZ];
	size_t len;

	if (!READ_ONCE(sa_audio_debug_enable))
		len = scnprintf(buffer, sizeof(buffer), "debug=%d\n",
			READ_ONCE(sa_audio_debug_enable));
	else
		len = audio_debug_show(buffer, sizeof(buffer));

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t proc_audio_debug_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	int old_value;
	ssize_t ret;

	old_value = READ_ONCE(sa_audio_debug_enable);
	ret = audio_value_write(&sa_audio_debug_enable, buf, count);
	if (ret < 0)
		return ret;
	if (old_value && !READ_ONCE(sa_audio_debug_enable))
		audio_debug_clear_pids();
	return ret;
}

static const struct file_operations proc_audio_debug_fops = {
	.read = proc_audio_debug_read,
	.write = proc_audio_debug_write,
	.llseek = default_llseek,
};

static ssize_t proc_audio_status_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[AUDIO_PROC_NUMBUF];
	size_t len;

	len = scnprintf(buffer, sizeof(buffer), "status=%d\n",
		READ_ONCE(sa_audio_perf_status));
	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t proc_audio_status_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	char buffer[AUDIO_PROC_NUMBUF];
	int status;
	int err;

	if (count >= sizeof(buffer))
		return -E2BIG;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	buffer[count] = '\0';
	err = kstrtoint(strstrip(buffer), 10, &status);
	if (err)
		return err;

	oplus_sched_assist_audio_perf_set_status(status);
	return count;
}

static const struct file_operations proc_audio_status_fops = {
	.read = proc_audio_status_read,
	.write = proc_audio_status_write,
	.llseek = default_llseek,
};

int oplus_sched_assist_audio_proc_init(struct proc_dir_entry *dir)
{
	struct proc_dir_entry *entry;

	audio_dir = proc_mkdir("audio", dir);
	if (!audio_dir)
		return -ENOMEM;

	entry = proc_create("enable", 0666, audio_dir, &proc_audio_enable_fops);
	if (!entry)
		goto err_audio;
	entry = proc_create("debug", 0666, audio_dir, &proc_audio_debug_fops);
	if (!entry)
		goto err_audio;
	entry = proc_create("status", 0666, audio_dir, &proc_audio_status_fops);
	if (!entry)
		goto err_audio;

	return 0;

err_audio:
	remove_proc_subtree("audio", dir);
	audio_dir = NULL;
	return -ENOMEM;
}

void oplus_sched_assist_audio_proc_remove(struct proc_dir_entry *dir)
{
	audio_debug_clear_pids();
	if (audio_dir)
		remove_proc_subtree("audio", dir);
	audio_dir = NULL;
}

static void set_audio_sched_boost_locked(struct task_struct *task, bool enable)
{
	if (enable)
		task->ux_state |= SA_TYPE_LIGHT;
	else
		task->ux_state &= ~SA_TYPE_LIGHT;

	if (!READ_ONCE(sa_audio_debug_enable))
		return;
	if (enable)
		audio_debug_add_pid(task->pid);
	else
		audio_debug_remove_pid(task->pid);
}

void oplus_sched_assist_audio_perf_addIm(struct task_struct *task, int im_flag)
{
	if (!audio_perf_enabled())
		return;

	if (task_is_audio(task) && im_flag != IM_FLAG_AUDIO)
		set_audio_sched_boost_locked(task, false);
	else if (im_flag == IM_FLAG_AUDIO)
		set_audio_sched_boost_locked(task, true);
}

void oplus_sched_assist_audio_enqueue_hook(struct task_struct *task)
{
	if (!audio_perf_status_on() || !task_is_audio(task))
		return;

	if (task->timer_slack_ns > AUDIO_TIMER_SLACK_NS)
		task->timer_slack_ns = AUDIO_TIMER_SLACK_NS;
}

void oplus_sched_assist_audio_perf_set_status(int status)
{
	WRITE_ONCE(sa_audio_perf_status, !!status);
}
