// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm subsystem sleep-voter reporting for ColorOS.
 *
 * The data layout and proc ABI come from the OnePlus 9R Android 14 kernel.
 * Unlike the donor implementation, this reader validates and snapshots the
 * firmware-owned SMEM before formatting it.  It never writes to SMEM.
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/string.h>

#include "oplus_subsys_sleep_monitor.h"

#define TIMER_MAX_EXPIRY	(1042499ULL * 3600ULL * 19200000ULL)

struct sleepmon_node {
	unsigned int pid;
	bool compact;
};

struct sleepmon_snapshot {
	struct subsys_lprinfo lpr;
	struct subsys_sleepmon voters;
	bool compact;
};

static struct proc_dir_entry *subsys_proc;

static const char * const modechosen_reason[] = {
	"Active_thread", "LPRM_Disabled", "Duration_ISFC",
	"Latency_RSTR", "Clocks_Vote_XO",
};

static struct sleepmon_node nodes[] = {
	{ SLEEPMONITOR_SMEM_ADSP_PID, false },
	{ SLEEPMONITOR_SMEM_ADSP_PID, true },
	{ SLEEPMONITOR_SMEM_CDSP_PID, false },
	{ SLEEPMONITOR_SMEM_CDSP_PID, true },
	{ SLEEPMONITOR_SMEM_SSC_PID, false },
	{ SLEEPMONITOR_SMEM_SSC_PID, true },
};

static const char * const node_names[] = {
	"adsp", "adsp_compact", "cdsp", "cdsp_compact",
	"slpi", "slpi_compact",
};

static u64 ticks_to_us(u64 ticks)
{
	u64 whole;

	if (ticks == U32_MAX || ticks == U64_MAX)
		return ticks;
	whole = div64_u64(ticks, 192);
	return whole * 10 + div64_u64((ticks - whole * 192) * 10, 192);
}

static void seq_fixed(struct seq_file *seq, const char *value, size_t size)
{
	seq_printf(seq, "%.*s", (int)strnlen(value, size), value);
}

static int validate_snapshot(const struct sleepmon_snapshot *snap)
{
	unsigned int i;

	if (!snap->lpr.num_synth_modes ||
	    snap->lpr.num_synth_modes > NUM_SYNTH_MODES ||
	    snap->lpr.num_lpr > NUM_COMPONENT_MODES)
		return -EPROTO;

	for (i = 0; i < snap->lpr.num_lpr; i++)
		if (snap->lpr.lprm[i].num_lprm > NUM_LPRM_MAX)
			return -EPROTO;

	return 0;
}

static int snapshot_smem(struct sleepmon_snapshot *snap, unsigned int pid)
{
	const struct subsys_lprinfo *lpr;
	const struct subsys_sleepmon *voters;
	size_t lpr_size = 0, voters_size = 0;
	int ret;

	lpr = qcom_smem_get(pid, LPRINFO_SMEM_ID, &lpr_size);
	if (IS_ERR(lpr))
		return PTR_ERR(lpr);
	if (!lpr || lpr_size < sizeof(*lpr))
		return -ENODATA;

	voters = qcom_smem_get(pid, SLEEPVOTERS_SMEM_ID, &voters_size);
	if (IS_ERR(voters))
		return PTR_ERR(voters);
	if (!voters || voters_size < sizeof(*voters))
		return -ENODATA;

	memcpy(&snap->lpr, lpr, sizeof(snap->lpr));
	memcpy(&snap->voters, voters, sizeof(snap->voters));
	ret = validate_snapshot(snap);
	if (ret)
		pr_warn_ratelimited("subsys_sleepmon: invalid SMEM layout for pid %u\n",
				    pid);
	return ret;
}

static unsigned int sleep_mode_reason(const struct sleepmon_snapshot *snap)
{
	const struct subsys_lprinfo *lpr = &snap->lpr;
	const struct subsys_sleepmon *v = &snap->voters;
	unsigned int reason = 0;
	unsigned int i, j;

	if (v->active_thread.tid)
		reason |= SUBSYSTEM_MODE_REASON_ACTIVE_THREAD;
	for (i = 0; i < lpr->num_lpr; i++)
		for (j = 0; j < lpr->lprm[i].num_lprm; j++)
			if (!v->synth_mode_voters.lprmodes[i].lprmModeEnabled[j] &&
			    lpr->lprm[i].lprmModeSupt[j])
				reason |= SUBSYSTEM_MODE_REASON_LPRM;
	for (i = 0; i < lpr->num_synth_modes; i++) {
		if (v->duration_voters.sleepDuration <
		    v->duration_voters.modeDuration[i])
			reason |= SUBSYSTEM_MODE_REASON_SLEEP_DURATION;
		if (v->latency_voters.latencyBudget <
		    v->latency_voters.modeLatency[i])
			reason |= SUBSYSTEM_MODE_REASON_LATENCY;
	}
	for (i = 0; i < XO_VOTERS_NUM; i++)
		if (strnlen(v->xo_voters.xoclks[i],
			    sizeof(v->xo_voters.xoclks[i]))) {
			reason |= SUBSYSTEM_MODE_REASON_CLOCK;
			break;
		}
	return reason;
}

static void print_mode(struct seq_file *seq,
		       const struct sleepmon_snapshot *snap)
{
	const struct subsys_sleepmon *v = &snap->voters;
	unsigned int count = snap->lpr.num_synth_modes;
	unsigned int reason = sleep_mode_reason(snap);
	unsigned int i;

	if (snap->compact) {
		seq_printf(seq, "sleep_mode_chosen=%u|%u\n",
			   v->synth_mode_voters.modeChosen, count - 1);
		seq_puts(seq, "sleep_mode_reason=");
		for (i = 0; i < ARRAY_SIZE(modechosen_reason); i++)
			if (reason & BIT(i))
				seq_printf(seq, "%s|", modechosen_reason[i]);
		seq_putc(seq, '\n');
		return;
	}

	seq_printf(seq, "Sleep mode chosen: %u from 0 - %u. (0 is deepest sleep level)\n",
		   v->synth_mode_voters.modeChosen, count - 1);
	seq_puts(seq, "| active thread  |      LPRM      | sleep duration |    latency     |      clocks     |\n|");
	for (i = 0; i < ARRAY_SIZE(modechosen_reason); i++)
		seq_printf(seq, "%s|", reason & BIT(i) ? "      FAIL      " :
							      "       OK       ");
	seq_puts(seq, "\n\n");
}

static void print_xo(struct seq_file *seq,
		     const struct sleepmon_snapshot *snap)
{
	const struct sleep_xovoters *xo = &snap->voters.xo_voters;
	unsigned int i;
	bool found = false;

	if (snap->compact)
		seq_puts(seq, "clocks=");
	else
		seq_puts(seq, "[----XO Voters----]\nClocks:\n");
	for (i = 0; i < XO_VOTERS_NUM; i++) {
		if (!strnlen(xo->xoclks[i], sizeof(xo->xoclks[i])))
			continue;
		found = true;
		if (!snap->compact)
			seq_puts(seq, "    ");
		seq_fixed(seq, xo->xoclks[i], sizeof(xo->xoclks[i]));
		seq_puts(seq, snap->compact ? "|" : "\n");
	}
	if (!found && !snap->compact)
		seq_puts(seq, "    No clocks vote XO\n");
	seq_putc(seq, '\n');
}

static void print_active(struct seq_file *seq,
			 const struct sleepmon_snapshot *snap)
{
	const struct sleep_activethread *a = &snap->voters.active_thread;

	if (snap->compact) {
		seq_puts(seq, "active_thread=");
		seq_fixed(seq, a->pname, sizeof(a->pname));
		seq_putc(seq, '|');
		seq_fixed(seq, a->tname, sizeof(a->tname));
		seq_putc(seq, '\n');
		return;
	}
	seq_printf(seq, "[----Active thread----]\nProcessor is %s in sleep context\nProcess: %u(",
		   a->stm ? "running" : "not running", a->pid);
	seq_fixed(seq, a->pname, sizeof(a->pname));
	seq_printf(seq, ")\n    Active Thread: %u(", a->tid);
	seq_fixed(seq, a->tname, sizeof(a->tname));
	seq_puts(seq, ")\n\n");
}

static void print_latency(struct seq_file *seq,
			  const struct sleepmon_snapshot *snap)
{
	const struct sleep_latencyvoters *l = &snap->voters.latency_voters;
	unsigned int i;

	seq_puts(seq, snap->compact ? "latency_mode=" :
		 "[----Latency vote----]\nmode latency (us)=");
	for (i = 0; i < snap->lpr.num_synth_modes; i++)
		seq_printf(seq, "%llu|",
			   (unsigned long long)ticks_to_us(l->modeLatency[i]));
	seq_putc(seq, '\n');
	if (l->islandLatency == U32_MAX || !l->islandLatency ||
	    l->normalLatency <= l->islandLatency) {
		seq_printf(seq, snap->compact ? "latency_normal=%u(" :
			   "Normal latency: %u, voter: ", l->normalLatency);
		seq_fixed(seq, l->normalLatencyReqClient,
			  sizeof(l->normalLatencyReqClient));
		seq_puts(seq, snap->compact ? ")\n" : "\n");
	} else {
		seq_printf(seq, snap->compact ? "latency_island=%llu(%u)\n" :
			   "Island latency: %llu, islands: %u\n",
			   (unsigned long long)ticks_to_us(l->islandLatency),
			   l->islands);
	}
	if (!snap->compact)
		seq_printf(seq, "Sleep latency budget: %llu\n\n",
			   (unsigned long long)ticks_to_us(l->latencyBudget));
}

static void print_duration(struct seq_file *seq,
			   const struct sleepmon_snapshot *snap)
{
	const struct sleep_durationvoters *d = &snap->voters.duration_voters;
	unsigned int i;

	seq_puts(seq, snap->compact ? "duration_mode=" :
		 "[----Sleep duration vote----]\nmode duration (us)=");
	for (i = 0; i < snap->lpr.num_synth_modes; i++)
		seq_printf(seq, "%llu|",
			   (unsigned long long)ticks_to_us(d->modeDuration[i]));
	seq_putc(seq, '\n');

	if (snap->compact) {
		if (d->sleepDuration == d->normalTimerExpiry) {
			seq_printf(seq, "duration_normal=%llu(",
				   (unsigned long long)ticks_to_us(d->normalTimerExpiry));
			seq_fixed(seq, d->normalTimerTName,
				  sizeof(d->normalTimerTName));
			seq_puts(seq, ")\n");
		} else if (d->sleepDuration == d->islandTimerExpiry) {
			seq_printf(seq, "duration_island=%llu(%u)\n",
				   (unsigned long long)ticks_to_us(d->islandTimerExpiry),
				   d->islandId);
		} else if (d->minTimerExpiry > d->softDuration) {
			seq_printf(seq, "duration_soft=%llu\n",
				   (unsigned long long)ticks_to_us(d->softDuration));
		}
		return;
	}

	if (d->normalTimerExpiryAbs < TIMER_MAX_EXPIRY) {
		seq_printf(seq, "Normal timer: relative = %llu, abs = %llu\n    voter: %u(",
			   (unsigned long long)ticks_to_us(d->normalTimerExpiry),
			   (unsigned long long)ticks_to_us(d->normalTimerExpiryAbs),
			   d->normalTimerTid);
		seq_fixed(seq, d->normalTimerTName, sizeof(d->normalTimerTName));
		seq_puts(seq, ")\n");
	} else {
		seq_puts(seq, "Normal timer: relative = -1, abs = -1\n    voter: 0(NA)\n");
	}
	seq_printf(seq, "Sleep Duration: %llu\n\n",
		   (unsigned long long)ticks_to_us(d->sleepDuration));
}

static void print_lprm(struct seq_file *seq,
		       const struct sleepmon_snapshot *snap)
{
	const struct subsys_lprinfo *lpr = &snap->lpr;
	const struct sleep_synthvoters *s = &snap->voters.synth_mode_voters;
	unsigned int i, j, n, client;
	bool any = false, disabled;

	if (!snap->compact)
		seq_puts(seq, "[----LPRM vote----]\n");
	for (i = 0; i < lpr->num_lpr; i++) {
		disabled = false;
		for (j = 0; j < lpr->lprm[i].num_lprm; j++)
			if (lpr->lprm[i].lprmModeSupt[j] &&
			    !s->lprmodes[i].lprmModeEnabled[j])
				disabled = true;
		if (!disabled)
			continue;
		for (n = 0; n < NUM_NPA_NODES; n++) {
			if (!s->lprnpavoters[n].lpr_index ||
			    s->lprnpavoters[n].lpr_index - 1 != i)
				continue;
			any = true;
			seq_puts(seq, snap->compact ? "resource=" : "resource: ");
			seq_fixed(seq, s->lprnpavoters[n].resourceName,
				  sizeof(s->lprnpavoters[n].resourceName));
			seq_putc(seq, '(');
			for (client = 0; client < NUM_CLIENT_NODES; client++) {
				const char *name = s->lprnpavoters[n].clientName[client];

				if (!strnlen(name, sizeof(s->lprnpavoters[n].clientName[client])))
					continue;
				seq_fixed(seq, name,
					  sizeof(s->lprnpavoters[n].clientName[client]));
				seq_putc(seq, '|');
			}
			seq_puts(seq, ")\n");
		}
	}
	if (!any)
		seq_puts(seq, "resource=null\n");
}

static int sleepmon_show(struct seq_file *seq, void *unused)
{
	const struct sleepmon_snapshot *snap = seq->private;

	print_mode(seq, snap);
	if (!snap->compact)
		seq_puts(seq, "Details below:\n\n");
	print_xo(seq, snap);
	print_active(seq, snap);
	print_latency(seq, snap);
	print_duration(seq, snap);
	print_lprm(seq, snap);
	return 0;
}

static int sleepmon_open(struct inode *inode, struct file *file)
{
	const struct sleepmon_node *node = PDE_DATA(inode);
	struct sleepmon_snapshot *snap;
	int ret;

	snap = kzalloc(sizeof(*snap), GFP_KERNEL);
	if (!snap)
		return -ENOMEM;
	snap->compact = node->compact;
	ret = snapshot_smem(snap, node->pid);
	if (ret) {
		kfree(snap);
		return ret;
	}
	ret = single_open(file, sleepmon_show, snap);
	if (ret)
		kfree(snap);
	return ret;
}

static int sleepmon_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;

	kfree(seq->private);
	return single_release(inode, file);
}

static const struct file_operations sleepmon_fops = {
	.owner = THIS_MODULE,
	.open = sleepmon_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = sleepmon_release,
};

static int __init subsys_sleepmon_init(void)
{
	unsigned int i;

	subsys_proc = proc_mkdir("subsys_sleepmon", NULL);
	if (!subsys_proc)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(nodes); i++)
		if (!proc_create_data(node_names[i], 0444, subsys_proc,
				      &sleepmon_fops, &nodes[i])) {
			remove_proc_subtree("subsys_sleepmon", NULL);
			subsys_proc = NULL;
			return -ENOMEM;
		}
	return 0;
}
module_init(subsys_sleepmon_init);

static void __exit subsys_sleepmon_exit(void)
{
	remove_proc_subtree("subsys_sleepmon", NULL);
	subsys_proc = NULL;
}
module_exit(subsys_sleepmon_exit);

MODULE_AUTHOR("OnePlus/Oplus; Miru hardening");
MODULE_DESCRIPTION("Qualcomm subsystem sleep-voter monitor");
MODULE_LICENSE("GPL v2");
