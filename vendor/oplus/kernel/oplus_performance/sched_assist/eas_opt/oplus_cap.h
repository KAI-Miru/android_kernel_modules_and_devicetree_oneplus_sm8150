// SPDX-License-Identifier: GPL-2.0-only
#ifndef _OPLUS_EAS_CAP_H
#define _OPLUS_EAS_CAP_H

#include <linux/sched.h>

#define OPLUS_CLUSTERS 5
#define OPLUS_CPUS 8

extern int eas_opt_enable;
extern int eas_opt_debug_enable;
extern int force_apply_ocap_enable;
extern int sa_adjust_group_enable;
extern int oplus_cap_multiple[OPLUS_CLUSTERS];
extern int nr_oplus_cap_multiple[OPLUS_CLUSTERS];
extern int util_thresh_percent[OPLUS_CLUSTERS];
extern int util_thresh_cvt[OPLUS_CLUSTERS];
extern unsigned long real_cpu_cap[NR_CPUS];

struct proc_dir_entry;
struct cfs_rq;
struct sched_entity;

int oplus_cap_init(struct proc_dir_entry *dir);
bool oplus_eas_task_skip_cpu(struct task_struct *task, int cpu);
void oplus_eas_place_entity(struct cfs_rq *cfs_rq,
		struct sched_entity *se, int initial);
unsigned long oplus_eas_adjust_capacity(int cpu, unsigned long capacity);
unsigned long oplus_eas_adjust_util(int cpu, unsigned int nr_running,
		unsigned long util, unsigned long max);

#endif
