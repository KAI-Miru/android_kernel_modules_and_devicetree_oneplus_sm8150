/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2022 Oplus. All rights reserved. */

#ifndef __OPLUS_CPU_JANK_ENABLE_H__
#define __OPLUS_CPU_JANK_ENABLE_H__

#include <linux/proc_fs.h>

extern unsigned int cpu_jank_info_enable;

struct proc_dir_entry *jank_enable_proc_init(struct proc_dir_entry *pde);
void jank_enable_proc_deinit(struct proc_dir_entry *pde);

#endif /* __OPLUS_CPU_JANK_ENABLE_H__ */
