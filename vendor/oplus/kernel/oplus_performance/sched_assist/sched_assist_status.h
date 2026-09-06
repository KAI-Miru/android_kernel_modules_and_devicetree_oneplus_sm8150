/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#ifndef _OPLUS_SCHED_STATUS_H_
#define _OPLUS_SCHED_STATUS_H_

enum task_stats_type {
	TST_SLEEP = 0,
	TST_RUNNABLE,
	TST_EXEC,
	TST_SCHED_TYPE_TATOL,
};

#define TASK_INFO_SAMPLE 4

struct task_info {
	u64 sa_info[TST_SCHED_TYPE_TATOL][TASK_INFO_SAMPLE];
	bool im_small;
};

#endif /* _OPLUS_SCHED_STATUS_H_ */
