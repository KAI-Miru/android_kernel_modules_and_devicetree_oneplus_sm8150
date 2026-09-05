/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _OPLUS_SCHED_ASSIST_AUDIO_H_
#define _OPLUS_SCHED_ASSIST_AUDIO_H_

struct proc_dir_entry;
struct task_struct;

int oplus_sched_assist_audio_proc_init(struct proc_dir_entry *dir);
void oplus_sched_assist_audio_proc_remove(struct proc_dir_entry *dir);
void oplus_sched_assist_audio_perf_set_status(int status);

/*
 * Called with task_lock(task) held from the IM-flag writer.  It applies the
 * H.40 scheduler's native SA_TYPE_LIGHT boost to IM_FLAG_AUDIO tasks.
 */
void oplus_sched_assist_audio_perf_addIm(struct task_struct *task, int im_flag);

/* Called from enqueue_task_fair() with the runqueue lock held. */
void oplus_sched_assist_audio_enqueue_hook(struct task_struct *task);
bool oplus_sched_assist_audio_perf_check_exit_latency(struct task_struct *task,
						       int cpu);

#endif /* _OPLUS_SCHED_ASSIST_AUDIO_H_ */
