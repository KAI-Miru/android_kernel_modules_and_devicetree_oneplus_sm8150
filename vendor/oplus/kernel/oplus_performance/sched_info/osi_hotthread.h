/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __OPLUS_CPU_JANK_HOTTHREAD_H__
#define __OPLUS_CPU_JANK_HOTTHREAD_H__

#include <linux/proc_fs.h>
#include <linux/types.h>

int osi_hotthread_proc_init(struct proc_dir_entry *parent);
void osi_hotthread_proc_deinit(struct proc_dir_entry *parent);
void osi_hotthread_set_enabled(bool enabled);

#endif /* __OPLUS_CPU_JANK_HOTTHREAD_H__ */
