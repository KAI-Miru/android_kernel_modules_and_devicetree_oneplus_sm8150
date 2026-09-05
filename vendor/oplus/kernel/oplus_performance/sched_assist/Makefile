obj-y += sched_assist_common.o
obj-y += sched_assist_audio.o
obj-y += sched_assist_mutex.o
obj-y += sched_assist_rwsem.o
obj-y += sched_assist_futex.o
obj-y += sched_assist_workqueue.o

# Android 14 OnePlus 9R EAS control plane, adapted to the 4.14 scheduler.
obj-$(CONFIG_OPLUS_FEATURE_EAS_OPT) += eas_opt/eas_opt.o
obj-$(CONFIG_OPLUS_FEATURE_VT_CAP) += eas_opt/oplus_cap.o
obj-$(CONFIG_OPLUS_CPUFREQ_IOWAIT_PROTECT) += eas_opt/oplus_iowait.o


ifeq ($(VERSION).$(PATCHLEVEL), 5.4)
obj-y += sched_assist_slide.o
endif
