/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _OPLUS_SUBSYS_SLEEP_MONITOR_H
#define _OPLUS_SUBSYS_SLEEP_MONITOR_H

#define SLEEPMONITOR_SMEM_ADSP_PID	2
#define SLEEPMONITOR_SMEM_SSC_PID	3
#define SLEEPMONITOR_SMEM_CDSP_PID	5

#define LPRINFO_SMEM_ID		590
#define SLEEPVOTERS_SMEM_ID	591

#define NUM_COMPONENT_MODES	15
#define NUM_LPRM_MAX		5
#define NUM_SYNTH_MODES		6
#define XO_VOTERS_NUM		4
#define NUM_CLIENT_NODES	2
#define NUM_NPA_NODES		3

#define SUBSYSTEM_MODE_REASON_ACTIVE_THREAD	BIT(0)
#define SUBSYSTEM_MODE_REASON_LPRM		BIT(1)
#define SUBSYSTEM_MODE_REASON_SLEEP_DURATION	BIT(2)
#define SUBSYSTEM_MODE_REASON_LATENCY		BIT(3)
#define SUBSYSTEM_MODE_REASON_CLOCK		BIT(4)

struct sleep_xovoters {
	char xoclks[XO_VOTERS_NUM][32];
};

struct sleep_latencyvoters {
	u32 normalLatency;
	char normalLatencyReqClient[32];
	u32 islandLatency;
	u32 islands;
	u32 latencyBudget;
	u32 modeLatency[NUM_SYNTH_MODES];
};

struct sleep_durationvoters {
	u64 normalTimerExpiryAbs;
	u64 normalTimerExpiry;
	u32 normalTimerTid;
	char normalTimerTName[32];
	u64 islandTimerExpiryAbs;
	u64 islandTimerExpiry;
	u32 islandId;
	u64 minTimerExpiry;
	u64 softDuration;
	u64 sleepDuration;
	u32 modeDuration[NUM_SYNTH_MODES];
};

struct sleep_lprm_info {
	u8 num_lprm;
	bool lprmModeSupt[NUM_LPRM_MAX];
};

struct subsys_lprinfo {
	u8 num_synth_modes;
	u8 num_lpr;
	char lpr[NUM_COMPONENT_MODES][16];
	struct sleep_lprm_info lprm[NUM_COMPONENT_MODES];
};

struct sleep_npavoters {
	u8 lpr_index;
	char resourceName[32];
	char clientName[NUM_CLIENT_NODES][32];
};

struct sleep_lprmvoters {
	bool lprmModeEnabled[NUM_LPRM_MAX];
};

struct sleep_synthvoters {
	u8 modeChosen;
	struct sleep_lprmvoters lprmodes[NUM_COMPONENT_MODES];
	struct sleep_npavoters lprnpavoters[NUM_NPA_NODES];
};

struct sleep_activethread {
	bool stm;
	u32 pid;
	u32 tid;
	char pname[32];
	char tname[32];
};

struct sleep_modereason {
	u32 reason;
};

struct subsys_sleepmon {
	struct sleep_xovoters xo_voters;
	struct sleep_latencyvoters latency_voters;
	struct sleep_durationvoters duration_voters;
	struct sleep_synthvoters synth_mode_voters;
	struct sleep_activethread active_thread;
	struct sleep_modereason modeskip_reason[NUM_SYNTH_MODES];
};

#endif
