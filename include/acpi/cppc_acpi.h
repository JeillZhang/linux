/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CPPC (Collaborative Processor Performance Control) methods used
 * by CPUfreq drivers.
 *
 * (C) Copyright 2014, 2015 Linaro Ltd.
 * Author: Ashwin Chaugule <ashwin.chaugule@linaro.org>
 */

#ifndef _CPPC_ACPI_H
#define _CPPC_ACPI_H

#include <linux/acpi.h>
#include <linux/cpufreq.h>
#include <linux/types.h>

#include <acpi/pcc.h>
#include <acpi/processor.h>

/* CPPCv2 and CPPCv3 support */
#define CPPC_V2_REV	2
#define CPPC_V3_REV	3
#define CPPC_V2_NUM_ENT	21
#define CPPC_V3_NUM_ENT	23

#define PCC_CMD_COMPLETE_MASK	(1 << 0)
#define PCC_ERROR_MASK		(1 << 2)

#define MAX_CPC_REG_ENT 21

/* CPPC specific PCC commands. */
#define	CMD_READ 0
#define	CMD_WRITE 1

#define CPPC_AUTO_ACT_WINDOW_SIG_BIT_SIZE	(7)
#define CPPC_AUTO_ACT_WINDOW_EXP_BIT_SIZE	(3)
#define CPPC_AUTO_ACT_WINDOW_MAX_SIG	((1 << CPPC_AUTO_ACT_WINDOW_SIG_BIT_SIZE) - 1)
#define CPPC_AUTO_ACT_WINDOW_MAX_EXP	((1 << CPPC_AUTO_ACT_WINDOW_EXP_BIT_SIZE) - 1)
/* CPPC_AUTO_ACT_WINDOW_MAX_SIG is 127, so 128 and 129 will decay to 127 when writing */
#define CPPC_AUTO_ACT_WINDOW_SIG_CARRY_THRESH 129

#define CPPC_ENERGY_PERF_MAX	(0xFF)

/* Each register has the folowing format. */
struct cpc_reg {
	u8 descriptor;
	u16 length;
	u8 space_id;
	u8 bit_width;
	u8 bit_offset;
	u8 access_width;
	u64 address;
} __packed;

/*
 * Each entry in the CPC table is either
 * of type ACPI_TYPE_BUFFER or
 * ACPI_TYPE_INTEGER.
 */
struct cpc_register_resource {
	acpi_object_type type;
	u64 __iomem *sys_mem_vaddr;
	union {
		struct cpc_reg reg;
		u64 int_value;
	} cpc_entry;
};

/* Container to hold the CPC details for each CPU */
struct cpc_desc {
	int num_entries;
	int version;
	int cpu_id;
	int write_cmd_status;
	int write_cmd_id;
	/* Lock used for RMW operations in cpc_write() */
	raw_spinlock_t rmw_lock;
	struct cpc_register_resource cpc_regs[MAX_CPC_REG_ENT];
	struct acpi_psd_package domain_info;
	struct kobject kobj;
};

/* These are indexes into the per-cpu cpc_regs[]. Order is important. */
enum cppc_regs {
	HIGHEST_PERF,
	NOMINAL_PERF,
	LOW_NON_LINEAR_PERF,
	LOWEST_PERF,
	GUARANTEED_PERF,
	DESIRED_PERF,
	MIN_PERF,
	MAX_PERF,
	PERF_REDUC_TOLERANCE,
	TIME_WINDOW,
	CTR_WRAP_TIME,
	REFERENCE_CTR,
	DELIVERED_CTR,
	PERF_LIMITED,
	ENABLE,
	AUTO_SEL_ENABLE,
	AUTO_ACT_WINDOW,
	ENERGY_PERF,
	REFERENCE_PERF,
	LOWEST_FREQ,
	NOMINAL_FREQ,
};

/*
 * Categorization of registers as described
 * in the ACPI v.5.1 spec.
 * XXX: Only filling up ones which are used by governors
 * today.
 */
struct cppc_perf_caps {
	u32 guaranteed_perf;
	u32 highest_perf;
	u32 nominal_perf;
	u32 lowest_perf;
	u32 lowest_nonlinear_perf;
	u32 lowest_freq;
	u32 nominal_freq;
	u32 energy_perf;
	bool auto_sel;
};

struct cppc_perf_ctrls {
	u32 max_perf;
	u32 min_perf;
	u32 desired_perf;
	u32 energy_perf;
};

struct cppc_perf_fb_ctrs {
	u64 reference;
	u64 delivered;
	u64 reference_perf;
	u64 wraparound_time;
};

/* Per CPU container for runtime CPPC management. */
struct cppc_cpudata {
	struct cppc_perf_caps perf_caps;
	struct cppc_perf_ctrls perf_ctrls;
	struct cppc_perf_fb_ctrs perf_fb_ctrs;
	unsigned int shared_type;
	cpumask_var_t shared_cpu_map;
};

#ifdef CONFIG_ACPI_CPPC_LIB
extern int cppc_get_desired_perf(int cpunum, u64 *desired_perf);
extern int cppc_get_nominal_perf(int cpunum, u64 *nominal_perf);
extern int cppc_get_highest_perf(int cpunum, u64 *highest_perf);
extern int cppc_get_perf_ctrs(int cpu, struct cppc_perf_fb_ctrs *perf_fb_ctrs);
extern int cppc_set_perf(int cpu, struct cppc_perf_ctrls *perf_ctrls);
extern int cppc_set_enable(int cpu, bool enable);
extern int cppc_get_perf_caps(int cpu, struct cppc_perf_caps *caps);
extern bool cppc_perf_ctrs_in_pcc(void);
extern unsigned int cppc_perf_to_khz(struct cppc_perf_caps *caps, unsigned int perf);
extern unsigned int cppc_khz_to_perf(struct cppc_perf_caps *caps, unsigned int freq);
extern bool acpi_cpc_valid(void);
extern bool cppc_allow_fast_switch(void);
extern int acpi_get_psd_map(unsigned int cpu, struct cppc_cpudata *cpu_data);
extern unsigned int cppc_get_transition_latency(int cpu);
extern bool cpc_ffh_supported(void);
extern bool cpc_supported_by_cpu(void);
extern int cpc_read_ffh(int cpunum, struct cpc_reg *reg, u64 *val);
extern int cpc_write_ffh(int cpunum, struct cpc_reg *reg, u64 val);
extern int cppc_get_epp_perf(int cpunum, u64 *epp_perf);
extern int cppc_set_epp_perf(int cpu, struct cppc_perf_ctrls *perf_ctrls, bool enable);
extern int cppc_set_epp(int cpu, u64 epp_val);
extern int cppc_get_auto_act_window(int cpu, u64 *auto_act_window);
extern int cppc_set_auto_act_window(int cpu, u64 auto_act_window);
extern int cppc_get_auto_sel(int cpu, bool *enable);
extern int cppc_set_auto_sel(int cpu, bool enable);
extern int amd_get_highest_perf(unsigned int cpu, u32 *highest_perf);
extern int amd_get_boost_ratio_numerator(unsigned int cpu, u64 *numerator);
extern int amd_detect_prefcore(bool *detected);
#else /* !CONFIG_ACPI_CPPC_LIB */
static inline int cppc_get_desired_perf(int cpunum, u64 *desired_perf)
{
	return -EOPNOTSUPP;
}
static inline int cppc_get_nominal_perf(int cpunum, u64 *nominal_perf)
{
	return -EOPNOTSUPP;
}
static inline int cppc_get_highest_perf(int cpunum, u64 *highest_perf)
{
	return -EOPNOTSUPP;
}
static inline int cppc_get_perf_ctrs(int cpu, struct cppc_perf_fb_ctrs *perf_fb_ctrs)
{
	return -EOPNOTSUPP;
}
static inline int cppc_set_perf(int cpu, struct cppc_perf_ctrls *perf_ctrls)
{
	return -EOPNOTSUPP;
}
static inline int cppc_set_enable(int cpu, bool enable)
{
	return -EOPNOTSUPP;
}
static inline int cppc_get_perf_caps(int cpu, struct cppc_perf_caps *caps)
{
	return -EOPNOTSUPP;
}
static inline bool cppc_perf_ctrs_in_pcc(void)
{
	return false;
}
static inline bool acpi_cpc_valid(void)
{
	return false;
}
static inline bool cppc_allow_fast_switch(void)
{
	return false;
}
static inline unsigned int cppc_get_transition_latency(int cpu)
{
	return CPUFREQ_ETERNAL;
}
static inline bool cpc_ffh_supported(void)
{
	return false;
}
static inline int cpc_read_ffh(int cpunum, struct cpc_reg *reg, u64 *val)
{
	return -EOPNOTSUPP;
}
static inline int cpc_write_ffh(int cpunum, struct cpc_reg *reg, u64 val)
{
	return -EOPNOTSUPP;
}
static inline int cppc_set_epp_perf(int cpu, struct cppc_perf_ctrls *perf_ctrls, bool enable)
{
	return -EOPNOTSUPP;
}
static inline int cppc_get_epp_perf(int cpunum, u64 *epp_perf)
{
	return -EOPNOTSUPP;
}
static inline int cppc_set_epp(int cpu, u64 epp_val)
{
	return -EOPNOTSUPP;
}
static inline int cppc_get_auto_act_window(int cpu, u64 *auto_act_window)
{
	return -EOPNOTSUPP;
}
static inline int cppc_set_auto_act_window(int cpu, u64 auto_act_window)
{
	return -EOPNOTSUPP;
}
static inline int cppc_get_auto_sel(int cpu, bool *enable)
{
	return -EOPNOTSUPP;
}
static inline int cppc_set_auto_sel(int cpu, bool enable)
{
	return -EOPNOTSUPP;
}
static inline int amd_get_highest_perf(unsigned int cpu, u32 *highest_perf)
{
	return -ENODEV;
}
static inline int amd_get_boost_ratio_numerator(unsigned int cpu, u64 *numerator)
{
	return -EOPNOTSUPP;
}
static inline int amd_detect_prefcore(bool *detected)
{
	return -ENODEV;
}
#endif /* !CONFIG_ACPI_CPPC_LIB */

#endif /* _CPPC_ACPI_H*/
