obj-y += sched_assist_common.o
obj-y += sched_assist_audio.o
obj-y += sched_assist_futex.o
obj-y += sched_assist_workqueue.o

# Android 14 OnePlus 9R locking strategy. Keep the legacy futex helpers for
# the H.40 UI-first call sites; the donor's newer futex hook sites are also
# intentionally inactive in the published Kona kernel.
ifeq ($(CONFIG_OPLUS_LOCKING_STRATEGY),y)
obj-y += oplus_locking_strategy.o
oplus_locking_strategy-y += sync/locking_main.o
oplus_locking_strategy-y += sync/futex.o
oplus_locking_strategy-y += sync/sysfs.o
oplus_locking_strategy-y += sync/mutex.o
oplus_locking_strategy-y += sync/rwsem.o
oplus_locking_strategy-$(CONFIG_OPLUS_LOCKING_MONITOR) += sync/kern_lock_stat.o
else
obj-y += sched_assist_mutex.o
obj-y += sched_assist_rwsem.o
endif

# Android 14 OnePlus 9R EAS control plane, adapted to the 4.14 scheduler.
obj-$(CONFIG_OPLUS_FEATURE_EAS_OPT) += eas_opt/eas_opt.o
obj-$(CONFIG_OPLUS_FEATURE_VT_CAP) += eas_opt/oplus_cap.o
obj-$(CONFIG_OPLUS_CPUFREQ_IOWAIT_PROTECT) += eas_opt/oplus_iowait.o


ifeq ($(VERSION).$(PATCHLEVEL), 5.4)
obj-y += sched_assist_slide.o
endif
