// SPDX-License-Identifier: GPL-2.0-only
#ifndef _OPLUS_EAS_IOWAIT_H
#define _OPLUS_EAS_IOWAIT_H

extern unsigned int sysctl_iowait_reset_ticks;
extern unsigned int sysctl_iowait_apply_ticks;
extern unsigned int sysctl_oplus_iowait_boost_enabled;
extern unsigned int sysctl_oplus_iowait_skip_min_enabled;

struct proc_dir_entry;
int oplus_sched_assist_iowait_proc_init(struct proc_dir_entry *dir);

#endif
