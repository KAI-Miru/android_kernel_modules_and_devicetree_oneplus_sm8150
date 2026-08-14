/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Android 14 CPU-jank task tracking ABI for Miru H.40.
 *
 * Keep this interface intentionally small: the implementation owns all
 * scheduler tracepoint lifetime and only attaches them while userspace has
 * explicitly enabled tracking for at least one PID.
 */
#ifndef _MIRU_CPU_JANK_TASKTRACK_H
#define _MIRU_CPU_JANK_TASKTRACK_H

#include <linux/proc_fs.h>

int tasktrack_init(void);
void tasktrack_deinit(void);
int tasktrack_proc_init(struct proc_dir_entry *parent);
void tasktrack_proc_deinit(struct proc_dir_entry *parent);

#endif /* _MIRU_CPU_JANK_TASKTRACK_H */
