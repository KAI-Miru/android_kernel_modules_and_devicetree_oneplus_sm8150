// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded H.40-native producer for the ColorOS top_hotthread proc ABI.
 *
 * The OP9R implementation adds counters to every task and runs from the
 * scheduler tick.  H.40 instead samples the current task on each online CPU
 * from a delayed work item.  Sampling is armed only by the existing CPU-jank
 * monitor controls, so the default path has no periodic work or scheduler
 * hook.  History and candidate storage are fully static and bounded.
 */
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "osi_base.h"
#include "osi_hotthread.h"

#define HOTTHREAD_WINDOW_COUNT		64
#define HOTTHREAD_VISIBLE_WINDOWS	32
#define HOTTHREAD_CANDIDATE_COUNT	32
#define HOTTHREAD_TOP_COUNT		5
#define HOTTHREAD_WINDOW_NS		128000000ULL
#define HOTTHREAD_SAMPLE_MS		128

struct hotthread_candidate {
	pid_t pid;
	pid_t tgid;
	uid_t uid;
	char comm[TASK_COMM_LEN];
	char leader_comm[TASK_COMM_LEN];
	u32 top_app_count;
	u32 non_topapp_count;
	u32 total_count;
};

struct hotthread_window {
	u64 id;
	struct hotthread_candidate candidates[HOTTHREAD_CANDIDATE_COUNT];
};

struct hotthread_sample {
	pid_t pid;
	pid_t tgid;
	uid_t uid;
	char comm[TASK_COMM_LEN];
	char leader_comm[TASK_COMM_LEN];
	bool top_app;
};

static DEFINE_MUTEX(hotthread_lock);
static struct hotthread_window hotthread_windows[HOTTHREAD_WINDOW_COUNT];
static struct delayed_work hotthread_sample_work;
static bool hotthread_enabled;

static u64 hotthread_window_id(u64 now_ns)
{
	return div64_u64(now_ns, HOTTHREAD_WINDOW_NS);
}

static void hotthread_capture_identity(struct task_struct *task,
		struct hotthread_sample *sample)
{
	const struct cred *cred;
	struct task_struct *leader;

	memset(sample, 0, sizeof(*sample));
	sample->pid = task->pid;
	sample->tgid = task->tgid;
	strlcpy(sample->comm, task->comm, sizeof(sample->comm));

	rcu_read_lock();
	cred = __task_cred(task);
	if (cred)
		sample->uid = __kuid_val(cred->uid);
	leader = rcu_dereference(task->group_leader);
	if (leader)
		strlcpy(sample->leader_comm, leader->comm,
			sizeof(sample->leader_comm));
	rcu_read_unlock();

	if (!sample->leader_comm[0])
		strlcpy(sample->leader_comm, sample->comm,
			sizeof(sample->leader_comm));
	sample->top_app = is_topapp(task);
}

static struct hotthread_candidate *hotthread_find_candidate(
		struct hotthread_window *window, pid_t pid)
{
	int i;

	for (i = 0; i < HOTTHREAD_CANDIDATE_COUNT; i++) {
		if (window->candidates[i].pid == pid)
			return &window->candidates[i];
	}

	return NULL;
}

static struct hotthread_candidate *hotthread_choose_candidate(
		struct hotthread_window *window)
{
	struct hotthread_candidate *lowest = &window->candidates[0];
	int i;

	for (i = 0; i < HOTTHREAD_CANDIDATE_COUNT; i++) {
		if (!window->candidates[i].pid)
			return &window->candidates[i];
		if (window->candidates[i].total_count < lowest->total_count)
			lowest = &window->candidates[i];
	}

	return lowest;
}

static void hotthread_record_sample_locked(const struct hotthread_sample *sample,
		u64 id)
{
	struct hotthread_window *window;
	struct hotthread_candidate *candidate;
	int index = id & (HOTTHREAD_WINDOW_COUNT - 1);

	window = &hotthread_windows[index];
	if (window->id != id) {
		memset(window, 0, sizeof(*window));
		window->id = id;
	}

	candidate = hotthread_find_candidate(window, sample->pid);
	if (!candidate) {
		candidate = hotthread_choose_candidate(window);
		memset(candidate, 0, sizeof(*candidate));
		candidate->pid = sample->pid;
		candidate->tgid = sample->tgid;
		candidate->uid = sample->uid;
		strlcpy(candidate->comm, sample->comm,
			sizeof(candidate->comm));
		strlcpy(candidate->leader_comm, sample->leader_comm,
			sizeof(candidate->leader_comm));
	}

	if (sample->top_app)
		candidate->top_app_count++;
	else
		candidate->non_topapp_count++;
	candidate->total_count++;
}

static void hotthread_sample_current_tasks(void)
{
	struct hotthread_sample sample;
	struct task_struct *task;
	u64 id = hotthread_window_id(ktime_get_ns());
	int cpu;

	for_each_online_cpu(cpu) {
		task = NULL;
		rcu_read_lock();
		task = cpu_rq(cpu)->curr;
		if (task && task->pid > 0)
			get_task_struct(task);
		else
			task = NULL;
		rcu_read_unlock();

		if (!task)
			continue;

		hotthread_capture_identity(task, &sample);
		put_task_struct(task);

		mutex_lock(&hotthread_lock);
		if (hotthread_enabled)
			hotthread_record_sample_locked(&sample, id);
		mutex_unlock(&hotthread_lock);
	}
}

static void hotthread_sample_workfn(struct work_struct *work)
{
	if (!READ_ONCE(hotthread_enabled))
		return;

	hotthread_sample_current_tasks();
	if (READ_ONCE(hotthread_enabled))
		schedule_delayed_work(&hotthread_sample_work,
			msecs_to_jiffies(HOTTHREAD_SAMPLE_MS));
}

void osi_hotthread_set_enabled(bool enabled)
{
	bool changed;

	mutex_lock(&hotthread_lock);
	changed = hotthread_enabled != enabled;
	if (changed && enabled)
		memset(hotthread_windows, 0, sizeof(hotthread_windows));
	hotthread_enabled = enabled;
	mutex_unlock(&hotthread_lock);

	if (!changed)
		return;
	if (enabled)
		schedule_delayed_work(&hotthread_sample_work, 0);
	else
		cancel_delayed_work_sync(&hotthread_sample_work);
}

static int hotthread_best_candidate(struct hotthread_window *window,
		bool selected[HOTTHREAD_CANDIDATE_COUNT])
{
	u32 best_count = 0;
	int best = -1;
	int i;

	for (i = 0; i < HOTTHREAD_CANDIDATE_COUNT; i++) {
		if (selected[i] || !window->candidates[i].pid)
			continue;
		if (window->candidates[i].total_count > best_count) {
			best_count = window->candidates[i].total_count;
			best = i;
		}
	}

	return best;
}

static int proc_top_hotthread_show(struct seq_file *m, void *v)
{
	struct hotthread_candidate *candidate;
	struct hotthread_window *window;
	bool selected[HOTTHREAD_CANDIDATE_COUNT];
	u64 now_id = hotthread_window_id(ktime_get_ns());
	u64 id;
	int best;
	int age;
	int rank;

	mutex_lock(&hotthread_lock);
	for (age = 0; age < HOTTHREAD_VISIBLE_WINDOWS; age++) {
		id = now_id - age;
		window = &hotthread_windows[id & (HOTTHREAD_WINDOW_COUNT - 1)];
		memset(selected, 0, sizeof(selected));

		if (window->id == id) {
			for (rank = 0; rank < HOTTHREAD_TOP_COUNT; rank++) {
				best = hotthread_best_candidate(window, selected);
				if (best < 0)
					break;
				selected[best] = true;
				candidate = &window->candidates[best];
				if (rank)
					seq_puts(m, "  ");
				seq_printf(m, "%u$%d$%s$%d$%s$%u$%u",
					candidate->uid, candidate->tgid,
					candidate->leader_comm, candidate->pid,
					candidate->comm,
					candidate->top_app_count,
					candidate->non_topapp_count);
			}
		}
		seq_putc(m, '\n');
	}
	mutex_unlock(&hotthread_lock);

	return 0;
}

static int proc_top_hotthread_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_top_hotthread_show, inode->i_private);
}

static const struct file_operations proc_top_hotthread_fops = {
	.open = proc_top_hotthread_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int osi_hotthread_proc_init(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *entry;

	mutex_lock(&hotthread_lock);
	hotthread_enabled = false;
	memset(hotthread_windows, 0, sizeof(hotthread_windows));
	mutex_unlock(&hotthread_lock);
	INIT_DELAYED_WORK(&hotthread_sample_work, hotthread_sample_workfn);

	entry = proc_create("top_hotthread", 0444, parent,
			&proc_top_hotthread_fops);
	return entry ? 0 : -ENOMEM;
}

void osi_hotthread_proc_deinit(struct proc_dir_entry *parent)
{
	osi_hotthread_set_enabled(false);
	remove_proc_entry("top_hotthread", parent);
}
