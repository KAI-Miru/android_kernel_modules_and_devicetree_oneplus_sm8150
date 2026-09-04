// SPDX-License-Identifier: GPL-2.0-only
/*
 * Android 14 CPU-jank task tracking for Miru H.40.
 *
 * OP9R's Android 14 implementation tracks up to four selected PIDs in
 * 128 ms windows.  Its original backend depends on newer vendor hooks and
 * task extensions.  This 4.14-native backend keeps the same proc control
 * plane and windowed state accounting, but registers ordinary scheduler
 * tracepoints only while both conditions are true:
 *
 *   1. userspace enabled task tracking; and
 *   2. at least one PID is being tracked.
 *
 * Therefore the normal phone path has no task-track scheduler callback.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <trace/events/sched.h>

#include "tasktrack.h"

#define TASKTRACK_MAX_TASKS		4
#define TASKTRACK_WINDOW_COUNT		64
#define TASKTRACK_WINDOW_NS		128000000ULL
#define TASKTRACK_EVENT_COUNT		64
#define TASKTRACK_EVENT_THRESHOLD_NS	1000000ULL

/*
 * Keep the OP9R column order.  H.40 can account running, runnable and
 * sleeping time from stable scheduler tracepoints; the vendor-hook specific
 * sub-types intentionally remain zero instead of being fabricated.
 */
enum tasktrack_trace_type {
	TASKTRACK_RUNNING = 0,
	TASKTRACK_RUNNABLE,
	TASKTRACK_SLEEPING,
	TASKTRACK_SLEEPING_INBINDER,
	TASKTRACK_SLEEPING_INFUTEX,
	TASKTRACK_DISKSLEEP,
	TASKTRACK_DISKSLEEP_INIOWAIT,
	TASKTRACK_IRQ,
	TASKTRACK_OTHER,
	TASKTRACK_IN_MINCORE,
	TASKTRACK_TRACE_COUNT,
};

struct tasktrack_entry {
	pid_t pid;
	pid_t tgid;
	char comm[TASK_COMM_LEN];
	u8 state;
	u64 state_since_ns;
	u64 window_id[TASKTRACK_WINDOW_COUNT];
	u64 time[TASKTRACK_WINDOW_COUNT][TASKTRACK_TRACE_COUNT];
};

struct tasktrack_event {
	pid_t pid;
	u64 delta_ns;
	struct timespec64 timestamp;
};

static DEFINE_MUTEX(tasktrack_control_lock);
static DEFINE_MUTEX(tasktrack_event_read_lock);
static DEFINE_RAW_SPINLOCK(tasktrack_lock);
static struct tasktrack_entry tasktrack_entries[TASKTRACK_MAX_TASKS];
static struct tasktrack_entry tasktrack_snapshot[TASKTRACK_MAX_TASKS];
static pid_t tasktrack_fast_pids[TASKTRACK_MAX_TASKS];
static struct tasktrack_event tasktrack_latency_events[TASKTRACK_EVENT_COUNT];
static struct tasktrack_event tasktrack_iowait_events[TASKTRACK_EVENT_COUNT];
static struct tasktrack_event tasktrack_event_snapshot[TASKTRACK_EVENT_COUNT];
static u64 tasktrack_latency_sequence;
static u64 tasktrack_iowait_sequence;
static bool tasktrack_enabled;
static bool tasktrack_hooked;
static bool tasktrack_active;

static u64 tasktrack_window_id(u64 time_ns)
{
	return div64_u64(time_ns, TASKTRACK_WINDOW_NS);
}

static struct tasktrack_entry *tasktrack_find_locked(pid_t pid)
{
	int i;

	for (i = 0; i < TASKTRACK_MAX_TASKS; i++) {
		if (tasktrack_entries[i].pid == pid)
			return &tasktrack_entries[i];
	}

	return NULL;
}

static bool tasktrack_has_entries_locked(void)
{
	int i;

	for (i = 0; i < TASKTRACK_MAX_TASKS; i++) {
		if (tasktrack_entries[i].pid > 0)
			return true;
	}

	return false;
}

/*
 * Tracepoint callbacks run for every context switch while tracking is armed.
 * Keep the common unrelated-task case lock-free; these values are updated
 * together with their entries and never reference dynamically freed storage.
 */
static bool tasktrack_pid_maybe_tracked(pid_t pid)
{
	int i;

	for (i = 0; i < TASKTRACK_MAX_TASKS; i++) {
		if (READ_ONCE(tasktrack_fast_pids[i]) == pid)
			return true;
	}

	return false;
}

static void tasktrack_add_interval_locked(struct tasktrack_entry *entry,
		u8 type, u64 start_ns, u64 end_ns)
{
	u64 window;
	u64 window_end;
	u64 slice_end;
	int index;

	if (type >= TASKTRACK_TRACE_COUNT || end_ns <= start_ns)
		return;

	while (start_ns < end_ns) {
		window = tasktrack_window_id(start_ns);
		window_end = (window + 1) * TASKTRACK_WINDOW_NS;
		slice_end = min(end_ns, window_end);
		index = window & (TASKTRACK_WINDOW_COUNT - 1);

		if (entry->window_id[index] != window) {
			memset(entry->time[index], 0, sizeof(entry->time[index]));
			entry->window_id[index] = window;
		}

		entry->time[index][type] += slice_end - start_ns;
		start_ns = slice_end;
	}
}

static void tasktrack_refresh_identity_locked(struct tasktrack_entry *entry,
		struct task_struct *task)
{
	entry->tgid = task->tgid;
	memcpy(entry->comm, task->comm, TASK_COMM_LEN);
	entry->comm[TASK_COMM_LEN - 1] = '\0';
}

static void tasktrack_record_event_locked(struct tasktrack_event *events,
		u64 *sequence, pid_t pid, u64 delta_ns)
{
	struct tasktrack_event *event;

	if (delta_ns < TASKTRACK_EVENT_THRESHOLD_NS)
		return;

	event = &events[*sequence & (TASKTRACK_EVENT_COUNT - 1)];
	event->pid = pid;
	event->delta_ns = delta_ns;
	ktime_get_real_ts64(&event->timestamp);
	(*sequence)++;
}

static void tasktrack_record_transition_locked(struct tasktrack_entry *entry,
		u64 now_ns)
{
	u64 delta_ns;

	if (!entry->state_since_ns || now_ns <= entry->state_since_ns)
		return;

	delta_ns = now_ns - entry->state_since_ns;
	if (entry->state == TASKTRACK_RUNNABLE)
		tasktrack_record_event_locked(tasktrack_latency_events,
			&tasktrack_latency_sequence, entry->pid, delta_ns);
	else if (entry->state == TASKTRACK_DISKSLEEP_INIOWAIT)
		tasktrack_record_event_locked(tasktrack_iowait_events,
			&tasktrack_iowait_sequence, entry->pid, delta_ns);
}

static void tasktrack_account_until_locked(struct tasktrack_entry *entry,
		u64 now_ns)
{
	if (entry->state_since_ns && now_ns > entry->state_since_ns)
		tasktrack_add_interval_locked(entry, entry->state,
				entry->state_since_ns, now_ns);

	entry->state_since_ns = now_ns;
}

static void tasktrack_set_state_locked(struct tasktrack_entry *entry,
		struct task_struct *task, u8 state, u64 now_ns)
{
	tasktrack_refresh_identity_locked(entry, task);
	tasktrack_record_transition_locked(entry, now_ns);
	tasktrack_account_until_locked(entry, now_ns);
	entry->state = state;
}

static u8 tasktrack_sleep_state(struct task_struct *task)
{
	long state = READ_ONCE(task->state);

	if (state == TASK_RUNNING)
		return TASKTRACK_RUNNABLE;
	if (state & TASK_UNINTERRUPTIBLE) {
		if (task->in_iowait)
			return TASKTRACK_DISKSLEEP_INIOWAIT;
		return TASKTRACK_DISKSLEEP;
	}

	return TASKTRACK_SLEEPING;
}

static void tasktrack_sched_switch(void *unused, bool preempt,
		struct task_struct *prev, struct task_struct *next)
{
	struct tasktrack_entry *entry;
	unsigned long flags;
	u64 now_ns;
	u8 next_prev_state;
	bool prev_tracked;
	bool next_tracked;

	if (!READ_ONCE(tasktrack_active))
		return;
	prev_tracked = tasktrack_pid_maybe_tracked(prev->pid);
	next_tracked = tasktrack_pid_maybe_tracked(next->pid);
	if (!prev_tracked && !next_tracked)
		return;

	now_ns = ktime_get_ns();
	raw_spin_lock_irqsave(&tasktrack_lock, flags);
	if (!READ_ONCE(tasktrack_active))
		goto out;

	entry = tasktrack_find_locked(prev->pid);
	if (entry) {
		next_prev_state = tasktrack_sleep_state(prev);
		tasktrack_set_state_locked(entry, prev, next_prev_state, now_ns);
	}

	entry = tasktrack_find_locked(next->pid);
	if (entry)
		tasktrack_set_state_locked(entry, next, TASKTRACK_RUNNING, now_ns);
out:
	raw_spin_unlock_irqrestore(&tasktrack_lock, flags);
}

static void tasktrack_sched_waking(void *unused, struct task_struct *task)
{
	struct tasktrack_entry *entry;
	unsigned long flags;
	u64 now_ns;

	if (!READ_ONCE(tasktrack_active))
		return;
	if (!tasktrack_pid_maybe_tracked(task->pid))
		return;

	now_ns = ktime_get_ns();
	raw_spin_lock_irqsave(&tasktrack_lock, flags);
	if (!READ_ONCE(tasktrack_active))
		goto out;

	entry = tasktrack_find_locked(task->pid);
	if (entry)
		tasktrack_set_state_locked(entry, task, TASKTRACK_RUNNABLE, now_ns);
out:
	raw_spin_unlock_irqrestore(&tasktrack_lock, flags);
}

static void tasktrack_stop_hooks_locked(void)
{
	if (!tasktrack_hooked) {
		WRITE_ONCE(tasktrack_active, false);
		return;
	}

	WRITE_ONCE(tasktrack_active, false);
	unregister_trace_sched_waking(tasktrack_sched_waking, NULL);
	unregister_trace_sched_switch(tasktrack_sched_switch, NULL);
	tasktrack_hooked = false;
}

static int tasktrack_update_hooks_locked(void)
{
	unsigned long flags;
	bool needed;
	int ret;

	raw_spin_lock_irqsave(&tasktrack_lock, flags);
	needed = tasktrack_enabled && tasktrack_has_entries_locked();
	raw_spin_unlock_irqrestore(&tasktrack_lock, flags);

	if (!needed) {
		tasktrack_stop_hooks_locked();
		return 0;
	}

	if (tasktrack_hooked) {
		WRITE_ONCE(tasktrack_active, true);
		return 0;
	}

	ret = register_trace_sched_switch(tasktrack_sched_switch, NULL);
	if (ret)
		return ret;

	ret = register_trace_sched_waking(tasktrack_sched_waking, NULL);
	if (ret) {
		unregister_trace_sched_switch(tasktrack_sched_switch, NULL);
		return ret;
	}

	tasktrack_hooked = true;
	WRITE_ONCE(tasktrack_active, true);
	return 0;
}

static void tasktrack_clear_entries(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&tasktrack_lock, flags);
	memset(tasktrack_entries, 0, sizeof(tasktrack_entries));
	memset(tasktrack_fast_pids, 0, sizeof(tasktrack_fast_pids));
	memset(tasktrack_latency_events, 0, sizeof(tasktrack_latency_events));
	memset(tasktrack_iowait_events, 0, sizeof(tasktrack_iowait_events));
	tasktrack_latency_sequence = 0;
	tasktrack_iowait_sequence = 0;
	raw_spin_unlock_irqrestore(&tasktrack_lock, flags);
}

static int tasktrack_add_pid_locked(pid_t pid, u64 now_ns)
{
	struct tasktrack_entry *entry;
	int i;

	if (tasktrack_find_locked(pid))
		return 0;

	for (i = 0; i < TASKTRACK_MAX_TASKS; i++) {
		entry = &tasktrack_entries[i];
		if (entry->pid)
			continue;

		memset(entry, 0, sizeof(*entry));
		entry->pid = pid;
		WRITE_ONCE(tasktrack_fast_pids[i], pid);
		entry->tgid = pid;
		strncpy(entry->comm, "pending", TASK_COMM_LEN - 1);
		entry->state = TASKTRACK_OTHER;
		entry->state_since_ns = now_ns;
		return 0;
	}

	return -ENOSPC;
}

static void tasktrack_remove_pid_locked(pid_t pid)
{
	int i;

	for (i = 0; i < TASKTRACK_MAX_TASKS; i++) {
		if (tasktrack_entries[i].pid != pid)
			continue;

		WRITE_ONCE(tasktrack_fast_pids[i], 0);
		memset(&tasktrack_entries[i], 0,
				sizeof(tasktrack_entries[i]));
		return;
	}
}

int tasktrack_init(void)
{
	mutex_lock(&tasktrack_control_lock);
	tasktrack_enabled = false;
	tasktrack_hooked = false;
	WRITE_ONCE(tasktrack_active, false);
	tasktrack_clear_entries();
	mutex_unlock(&tasktrack_control_lock);

	return 0;
}

void tasktrack_deinit(void)
{
	mutex_lock(&tasktrack_control_lock);
	tasktrack_enabled = false;
	tasktrack_stop_hooks_locked();
	tasktrack_clear_entries();
	mutex_unlock(&tasktrack_control_lock);
}

static void tasktrack_take_snapshot_locked(u64 now_ns)
{
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&tasktrack_lock, flags);
	memcpy(tasktrack_snapshot, tasktrack_entries,
			sizeof(tasktrack_snapshot));
	for (i = 0; i < TASKTRACK_MAX_TASKS; i++) {
		if (tasktrack_snapshot[i].pid)
			tasktrack_account_until_locked(&tasktrack_snapshot[i], now_ns);
	}
	raw_spin_unlock_irqrestore(&tasktrack_lock, flags);
}

static int tasktrack_proc_show(struct seq_file *m, void *v)
{
	struct tasktrack_entry *entry;
	u64 now_ns = ktime_get_ns();
	u64 current_window;
	u64 window;
	u64 value;
	bool enabled;
	int age;
	int i;
	int type;
	int index;

	mutex_lock(&tasktrack_control_lock);
	enabled = tasktrack_enabled;
	tasktrack_take_snapshot_locked(now_ns);
	mutex_unlock(&tasktrack_control_lock);

	if (!enabled) {
		seq_puts(m, "Please enable this feature first!!!\n");
		return 0;
	}

	current_window = tasktrack_window_id(now_ns);
	for (i = 0; i < TASKTRACK_MAX_TASKS; i++) {
		entry = &tasktrack_snapshot[i];
		if (!entry->pid)
			continue;

		seq_printf(m, "PID: %d (%s)\n", entry->pid, entry->comm);
		for (age = 0; age < TASKTRACK_WINDOW_COUNT; age++) {
			if (current_window < age) {
				for (type = 0; type < TASKTRACK_TRACE_COUNT; type++)
					seq_puts(m, type == TASKTRACK_TRACE_COUNT - 1 ?
						 "0\n" : "0 ");
				continue;
			}

			window = current_window - age;
			index = window & (TASKTRACK_WINDOW_COUNT - 1);
			for (type = 0; type < TASKTRACK_TRACE_COUNT; type++) {
				value = entry->window_id[index] == window ?
					entry->time[index][type] : 0;
				seq_printf(m, "%llu%s",
					(unsigned long long)div64_u64(value, 1000000ULL),
					type == TASKTRACK_TRACE_COUNT - 1 ? "\n" : " ");
			}
		}
	}

	return 0;
}

static int tasktrack_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, tasktrack_proc_show, inode->i_private);
}

static ssize_t tasktrack_proc_write(struct file *file,
		const char __user *buf, size_t count, loff_t *offset)
{
	char buffer[256];
	char *token;
	char *cursor;
	size_t input_count = count;
	unsigned long flags;
	u64 now_ns;
	int value;
	int ret = 0;
	bool seen = false;

	if (!count)
		return 0;

	if (count >= sizeof(buffer))
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	buffer[count] = '\0';

	mutex_lock(&tasktrack_control_lock);
	raw_spin_lock_irqsave(&tasktrack_lock, flags);
	now_ns = ktime_get_ns();
	cursor = buffer;
	while ((token = strsep(&cursor, " \t\r\n")) != NULL) {
		token = strim(token);
		if (!*token)
			continue;

		if (kstrtoint(token, 10, &value)) {
			ret = -EINVAL;
			break;
		}
		if (!value || value == -2147483647 - 1) {
			ret = -EINVAL;
			break;
		}

		seen = true;
		if (value < 0)
			tasktrack_remove_pid_locked(-value);
		else {
			ret = tasktrack_add_pid_locked(value, now_ns);
			if (ret)
				break;
		}
	}
	raw_spin_unlock_irqrestore(&tasktrack_lock, flags);

	if (!ret && !seen)
		ret = -EINVAL;
	if (!ret)
		ret = tasktrack_update_hooks_locked();
	mutex_unlock(&tasktrack_control_lock);

	return ret ? ret : input_count;
}

static int tasktrack_enable_show(struct seq_file *m, void *v)
{
	bool enabled;

	mutex_lock(&tasktrack_control_lock);
	enabled = tasktrack_enabled;
	mutex_unlock(&tasktrack_control_lock);
	seq_printf(m, "%u\n", enabled ? 1 : 0);

	return 0;
}

static int tasktrack_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, tasktrack_enable_show, inode->i_private);
}

static ssize_t tasktrack_enable_write(struct file *file,
		const char __user *buf, size_t count, loff_t *offset)
{
	char buffer[32];
	size_t input_count = count;
	int value;
	int ret;

	if (!count)
		return 0;

	if (count >= sizeof(buffer))
		return -EINVAL;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;
	buffer[count] = '\0';
	if (kstrtoint(strim(buffer), 10, &value))
		return -EINVAL;

	mutex_lock(&tasktrack_control_lock);
	tasktrack_enabled = !!value;
	ret = tasktrack_update_hooks_locked();
	if (!tasktrack_enabled)
		tasktrack_clear_entries();
	mutex_unlock(&tasktrack_control_lock);

	return ret ? ret : input_count;
}

static const struct file_operations tasktrack_proc_fops = {
	.open = tasktrack_proc_open,
	.read = seq_read,
	.write = tasktrack_proc_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations tasktrack_enable_fops = {
	.open = tasktrack_enable_open,
	.read = seq_read,
	.write = tasktrack_enable_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int tasktrack_event_proc_show(struct seq_file *m,
		struct tasktrack_event *events)
{
	struct tasktrack_event *event;
	unsigned long flags;
	int i;

	mutex_lock(&tasktrack_event_read_lock);
	raw_spin_lock_irqsave(&tasktrack_lock, flags);
	memcpy(tasktrack_event_snapshot, events,
		sizeof(tasktrack_event_snapshot));
	raw_spin_unlock_irqrestore(&tasktrack_lock, flags);

	for (i = 0; i < TASKTRACK_EVENT_COUNT; i++) {
		event = &tasktrack_event_snapshot[i];
		seq_printf(m, "%d,%llu,%llu.%lu\n", event->pid,
			(unsigned long long)event->delta_ns,
			(unsigned long long)event->timestamp.tv_sec,
			(unsigned long)event->timestamp.tv_nsec);
	}
	seq_putc(m, '\n');
	mutex_unlock(&tasktrack_event_read_lock);

	return 0;
}

static int tasktrack_latency_proc_show(struct seq_file *m, void *v)
{
	return tasktrack_event_proc_show(m, tasktrack_latency_events);
}

static int tasktrack_iowait_proc_show(struct seq_file *m, void *v)
{
	return tasktrack_event_proc_show(m, tasktrack_iowait_events);
}

static int tasktrack_latency_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, tasktrack_latency_proc_show, inode->i_private);
}

static int tasktrack_iowait_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, tasktrack_iowait_proc_show, inode->i_private);
}

static const struct file_operations tasktrack_latency_fops = {
	.open = tasktrack_latency_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations tasktrack_iowait_fops = {
	.open = tasktrack_iowait_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int tasktrack_proc_init(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *entry;

	entry = proc_create("task_track", 0666, parent, &tasktrack_proc_fops);
	if (!entry)
		return -ENOMEM;

	entry = proc_create("task_track_enable", 0666, parent,
			&tasktrack_enable_fops);
	if (!entry) {
		remove_proc_entry("task_track", parent);
		return -ENOMEM;
	}

	entry = proc_create("sched_latency", 0444, parent,
			&tasktrack_latency_fops);
	if (!entry)
		goto err_latency;

	entry = proc_create("sched_iowait", 0444, parent,
			&tasktrack_iowait_fops);
	if (!entry)
		goto err_iowait;

	return 0;

err_iowait:
	remove_proc_entry("sched_latency", parent);
err_latency:
	remove_proc_entry("task_track_enable", parent);
	remove_proc_entry("task_track", parent);
	return -ENOMEM;
}

void tasktrack_proc_deinit(struct proc_dir_entry *parent)
{
	remove_proc_entry("sched_iowait", parent);
	remove_proc_entry("sched_latency", parent);
	remove_proc_entry("task_track_enable", parent);
	remove_proc_entry("task_track", parent);
}
