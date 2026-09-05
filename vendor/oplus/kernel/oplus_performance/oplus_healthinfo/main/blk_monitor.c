// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 Oplus. All rights reserved.
 *
 * Android 14 OnePlus 9R block monitor, adapted to the H.40 4.14 request
 * clock so request timestamps and completion timestamps share one domain.
 */

#include <soc/oplus/oplus_healthinfo.h>
#include <linux/blkdev.h>
#include <linux/kernel.h>
#include <linux/sched/clock.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <trace/events/block.h>

struct blk_wait_para q2i_wait_para = {
	.wait_h_ms = 500,
	.wait_l_ms = 200,
};

struct blk_wait_para i2d_wait_para = {
	.wait_h_ms = 500,
	.wait_l_ms = 200,
};

struct blk_wait_para q2c_wait_para = {
	.wait_h_ms = 500,
	.wait_l_ms = 200,
};

struct blk_wait_para d2c_wait_para = {
	.wait_h_ms = 500,
	.wait_l_ms = 200,
};

extern bool ohm_blkmon_ctrl;
extern bool ohm_blkmon_logon;
extern bool ohm_blkmon_trig;
extern void ohm_action_trig(int type);

static void blk_wait_monitor(struct blk_wait_para *para, struct request *rq,
			     u64 wait_ms, const char *mark)
{
	struct long_wait_record *record;
	struct timespec64 ts;
	u32 index;

	if (unlikely(!READ_ONCE(ohm_blkmon_ctrl)))
		return;

	if (unlikely(wait_ms > para->wait_stat.max_ms))
		para->wait_stat.max_ms = wait_ms;

	if (unlikely(wait_ms >= para->wait_h_ms)) {
		para->wait_stat.high_cnt++;
		if (ohm_blkmon_logon)
			ohm_debug("[blk / %s] long, wait %llu ms!\n", mark,
				  wait_ms);
		if (ohm_blkmon_trig)
			ohm_action_trig(OHM_BLK_MON);
	} else if (unlikely(wait_ms >= para->wait_l_ms)) {
		para->wait_stat.low_cnt++;
	}

	if (unlikely(wait_ms >= para->wait_l_ms)) {
		index = (u32)atomic_inc_return(&para->lwr_index);
		record = &para->last_n_lwr[index & LWR_MASK];
		record->pid = rq->rq_disk ? (u32)disk_devt(rq->rq_disk) : 0;
		record->priv = blk_rq_bytes(rq);
		ktime_get_real_ts64(&ts);
		record->timestamp = (u64)ts.tv_sec;
		record->timestamp_ns = (u64)ts.tv_nsec;
		record->ms = wait_ms;
	}
}

static u64 blkmon_elapsed_ms(u64 now, u64 then)
{
	if (!then || now < then)
		return 0;
	return (now - then) >> 20;
}

static void blkmon_add_trace_rq_insert(void *ignore, struct request_queue *q,
				       struct request *rq)
{
	u64 now = sched_clock();

	blk_wait_monitor(&q2i_wait_para, rq,
			 blkmon_elapsed_ms(now, rq->start_time_ns), "Q2I");
	rq->io_start_time_ns = now;
}

static void blkmon_add_trace_rq_issue(void *ignore, struct request_queue *q,
				      struct request *rq)
{
	u64 now = sched_clock();

	if (rq->io_start_time_ns)
		blk_wait_monitor(&i2d_wait_para, rq,
				 blkmon_elapsed_ms(now, rq->io_start_time_ns),
				 "I2D");
	rq->io_start_time_ns = now;
}

static void blkmon_add_trace_rq_complete(void *ignore, struct request *rq,
					 int error, unsigned int nr_bytes)
{
	u64 now = sched_clock();

	blk_wait_monitor(&q2c_wait_para, rq,
			 blkmon_elapsed_ms(now, rq->start_time_ns), "Q2C");
	if (rq->io_start_time_ns)
		blk_wait_monitor(&d2c_wait_para, rq,
				 blkmon_elapsed_ms(now, rq->io_start_time_ns),
				 "D2C");
}

void blkmon_init(void)
{
	WARN_ON(register_trace_block_rq_insert(blkmon_add_trace_rq_insert, NULL));
	WARN_ON(register_trace_block_rq_issue(blkmon_add_trace_rq_issue, NULL));
	WARN_ON(register_trace_block_rq_complete(blkmon_add_trace_rq_complete,
						  NULL));
}

module_param_named(q2c_wait_high_ms, q2c_wait_para.wait_h_ms, int,
		   S_IRUGO | S_IWUSR);
module_param_named(q2c_wait_low_ms, q2c_wait_para.wait_l_ms, int,
		   S_IRUGO | S_IWUSR);
module_param_named(q2i_wait_high_ms, q2i_wait_para.wait_h_ms, int,
		   S_IRUGO | S_IWUSR);
module_param_named(q2i_wait_low_ms, q2i_wait_para.wait_l_ms, int,
		   S_IRUGO | S_IWUSR);
module_param_named(i2d_wait_high_ms, i2d_wait_para.wait_h_ms, int,
		   S_IRUGO | S_IWUSR);
module_param_named(i2d_wait_low_ms, i2d_wait_para.wait_l_ms, int,
		   S_IRUGO | S_IWUSR);
module_param_named(d2c_wait_high_ms, d2c_wait_para.wait_h_ms, int,
		   S_IRUGO | S_IWUSR);
module_param_named(d2c_wait_low_ms, d2c_wait_para.wait_l_ms, int,
		   S_IRUGO | S_IWUSR);
