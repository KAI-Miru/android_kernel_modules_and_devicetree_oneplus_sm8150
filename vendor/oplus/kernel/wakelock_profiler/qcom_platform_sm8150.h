/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 *
 * SM8150 adaptation for the OnePlus 9R Android 14 wakelock profiler.
 */

#ifndef __QCOM_PLATFORM_SM8150_H__
#define __QCOM_PLATFORM_SM8150_H__

#include "oplus_wakelock_profiler.h"

/* H.40 uses the QPNP RTC action name rather than the newer PM8xxx name. */
#define IRQ_NAME_SM8150_RTCALARM "qpnp_rtc_alarm"

static struct wakeup_count_desc_t wc_powerkey_sm8150 = {
	.module_name = "powerkey",
	.module_mask = WS_CNT_POWERKEY,
	.ws_number = 1,
	.ws_desc[0] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_POWERKEY,
	},
};

static struct wakeup_count_desc_t wc_rtc_alarm_sm8150 = {
	.module_name = "rtc_alarm",
	.module_mask = WS_CNT_RTCALARM,
	.ws_number = 1,
	.ws_desc[0] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_SM8150_RTCALARM,
	},
};

static struct wakeup_count_desc_t wc_alarm_sm8150 = {
	.module_name = "alarm",
	.module_mask = WS_CNT_ALARM,
	.ws_number = 1,
	.ws_desc[0] = {
		.prop = IRQ_PROP_EXCHANGE,
		.name = IRQ_NAME_ALARM,
	},
};

/*
 * Guacamole and the non-5G OnePlus 7-series projects use the integrated
 * SM8150 modem. Hotdogg (project 19861) uses SDX55 over MHI. Keeping both
 * sets of observed IRQ action names is safe: the profiler only increments an
 * entry when that exact action is reported as pending after resume.
 */
static struct wakeup_count_desc_t wc_modem_sm8150 = {
	.module_name = "modem",
	.module_mask = WS_CNT_MODEM,
	.ws_number = 7,
	.ws_desc[0] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_MODEM_GLINK,
	},
	.ws_desc[1] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_MODEM_IPA,
	},
	.ws_desc[2] = {
		.prop = IRQ_PROP_EXCHANGE,
		.name = IRQ_NAME_MODEM_QMI,
	},
	.ws_desc[3] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_MODEM_MODEM,
	},
	.ws_desc[4] = {
		.prop = IRQ_PROP_REAL,
		.name = "mhi",
	},
	.ws_desc[5] = {
		.prop = IRQ_PROP_REAL,
		.name = "mdm status",
	},
	.ws_desc[6] = {
		.prop = IRQ_PROP_REAL,
		.name = "mdm errfatal",
	},
};

static struct wakeup_count_desc_t wc_wlan_sm8150 = {
	.module_name = "wlan",
	.module_mask = WS_CNT_WLAN,
	.ws_number = 3,
	.ws_desc[0] = {
		.prop = IRQ_PROP_EXCHANGE,
		.name = IRQ_NAME_WLAN_MSI,
	},
	.ws_desc[1] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_WLAN_IPCC_DATA,
	},
	.ws_desc[2] = {
		.prop = IRQ_PROP_REAL,
		.name = "wlan",
	},
};

static struct wakeup_count_desc_t wc_adsp_sm8150 = {
	.module_name = "adsp",
	.module_mask = WS_CNT_ADSP,
	.ws_number = 2,
	.ws_desc[0] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_ADSP,
	},
	.ws_desc[1] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_ADSP_GLINK,
	},
};

static struct wakeup_count_desc_t wc_cdsp_sm8150 = {
	.module_name = "cdsp",
	.module_mask = WS_CNT_CDSP,
	.ws_number = 2,
	.ws_desc[0] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_CDSP,
	},
	.ws_desc[1] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_CDSP_GLINK,
	},
};

static struct wakeup_count_desc_t wc_slpi_sm8150 = {
	.module_name = "slpi",
	.module_mask = WS_CNT_SLPI,
	.ws_number = 2,
	.ws_desc[0] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_SLPI,
	},
	.ws_desc[1] = {
		.prop = IRQ_PROP_REAL,
		.name = IRQ_NAME_SLPI_GLINK,
	},
};

static struct wakeup_count_desc_t wc_glink_sm8150 = {
	.module_name = "glink",
	.module_mask = WS_CNT_GLINK,
	.ws_number = 1,
	.ws_desc[0] = {
		.prop = IRQ_PROP_DUMMY_STATICS,
		.name = IRQ_NAME_GLINK,
	},
};

static struct wakeup_count_desc_t wc_abort_sm8150 = {
	.module_name = "abort",
	.module_mask = WS_CNT_ABORT,
	.ws_number = 1,
	.ws_desc[0] = {
		.prop = IRQ_PROP_DUMMY_STATICS,
		.name = IRQ_NAME_ABORT,
	},
};

static struct wakeup_count_desc_t wc_sum_sm8150 = {
	.module_name = "wakeup_sum",
	.module_mask = WS_CNT_SUM,
	.ws_number = 1,
	.ws_desc[0] = {
		.prop = IRQ_PROP_DUMMY_STATICS,
		.name = IRQ_NAME_WAKE_SUM,
	},
};

static struct wakeup_count_desc_t * const all_modules_sm8150[] = {
	&wc_powerkey_sm8150,
	&wc_rtc_alarm_sm8150,
	&wc_alarm_sm8150,
	&wc_modem_sm8150,
	&wc_wlan_sm8150,
	&wc_adsp_sm8150,
	&wc_cdsp_sm8150,
	&wc_slpi_sm8150,
	&wc_glink_sm8150,
	&wc_abort_sm8150,
	&wc_sum_sm8150,
	NULL,
};

static const char * const modem_report_exchange_sm8150[][MODEM_REPORT_NUMBER] = {
	{ IRQ_NAME_MODEM_IPA, IRQ_NAME_MODEM_QMI },
	{ NULL, NULL },
};

#endif /* __QCOM_PLATFORM_SM8150_H__ */
