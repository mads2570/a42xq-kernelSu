/*
 * Per core/cpu state
 *
 * Used to coordinate shared registers between HT threads or
 * among events on a single PMU.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/nmi.h>

#include <asm/cpufeature.h>
#include <asm/hardirq.h>
#include <asm/intel-family.h>
#include <asm/apic.h>

#include "../perf_event.h"

/*
 * Intel PerfMon, used on Core and later.
 */
static u64 intel_perfmon_event_map[PERF_COUNT_HW_MAX] __read_mostly =
{
	[PERF_COUNT_HW_CPU_CYCLES]		= 0x003c,
	[PERF_COUNT_HW_INSTRUCTIONS]		= 0x00c0,
	[PERF_COUNT_HW_CACHE_REFERENCES]	= 0x4f2e,
	[PERF_COUNT_HW_CACHE_MISSES]		= 0x412e,
	[PERF_COUNT_HW_BRANCH_INSTRUCTIONS]	= 0x00c4,
	[PERF_COUNT_HW_BRANCH_MISSES]		= 0x00c5,
	[PERF_COUNT_HW_BUS_CYCLES]		= 0x013c,
	[PERF_COUNT_HW_REF_CPU_CYCLES]		= 0x0300, /* pseudo-encoding */
};

static struct event_constraint intel_core_event_constraints[] __read_mostly =
{
	INTEL_EVENT_CONSTRAINT(0x11, 0x2), /* FP_ASSIST */
	INTEL_EVENT_CONSTRAINT(0x12, 0x2), /* MUL */
	INTEL_EVENT_CONSTRAINT(0x13, 0x2), /* DIV */
	INTEL_EVENT_CONSTRAINT(0x14, 0x1), /* CYCLES_DIV_BUSY */
	INTEL_EVENT_CONSTRAINT(0x19, 0x2), /* DELAYED_BYPASS */
	INTEL_EVENT_CONSTRAINT(0xc1, 0x1), /* FP_COMP_INSTR_RET */
	EVENT_CONSTRAINT_END
};

static struct event_constraint intel_core2_event_constraints[] __read_mostly =
{
	FIXED_EVENT_CONSTRAINT(0x00c0, 0), /* INST_RETIRED.ANY */
	FIXED_EVENT_CONSTRAINT(0x003c, 1), /* CPU_CLK_UNHALTED.CORE */
	FIXED_EVENT_CONSTRAINT(0x0300, 2), /* CPU_CLK_UNHALTED.REF */
	INTEL_EVENT_CONSTRAINT(0x10, 0x1), /* FP_COMP_OPS_EXE */
	INTEL_EVENT_CONSTRAINT(0x11, 0x2), /* FP_ASSIST */
	INTEL_EVENT_CONSTRAINT(0x12, 0x2), /* MUL */
	INTEL_EVENT_CONSTRAINT(0x13, 0x2), /* DIV */
	INTEL_EVENT_CONSTRAINT(0x14, 0x1), /* CYCLES_DIV_BUSY */
	INTEL_EVENT_CONSTRAINT(0x18, 0x1), /* IDLE_DURING_DIV */
	INTEL_EVENT_CONSTRAINT(0x19, 0x2), /* DELAYED_BYPASS */
	INTEL_EVENT_CONSTRAINT(0xa1, 0x1), /* RS_UOPS_DISPATCH_CYCLES */
	INTEL_EVENT_CONSTRAINT(0xc9, 0x1), /* ITLB_MISS_RETIRED (T30-9) */
	INTEL_EVENT_CONSTRAINT(0xcb, 0x1), /* MEM_LOAD_RETIRED */
	EVENT_CONSTRAINT_END
};

static struct event_constraint intel_nehalem_event_constraints[] __read_mostly =
{
	FIXED_EVENT_CONSTRAINT(0x00c0, 0), /* INST_RETIRED.ANY */
	FIXED_EVENT_CONSTRAINT(0x003c, 1), /* CPU_CLK_UNHALTED.CORE */
	FIXED_EVENT_CONSTRAINT(0x0300, 2), /* CPU_CLK_UNHALTED.REF */
	INTEL_EVENT_CONSTRAINT(0x40, 0x3), /* L1D_CACHE_LD */
	INTEL_EVENT_CONSTRAINT(0x41, 0x3), /* L1D_CACHE_ST */
	INTEL_EVENT_CONSTRAINT(0x42, 0x3), /* L1D_CACHE_LOCK */
	INTEL_EVENT_CONSTRAINT(0x43, 0x3), /* L1D_ALL_REF */
	INTEL_EVENT_CONSTRAINT(0x48, 0x3), /* L1D_PEND_MISS */
	INTEL_EVENT_CONSTRAINT(0x4e, 0x3), /* L1D_PREFETCH */
	INTEL_EVENT_CONSTRAINT(0x51, 0x3), /* L1D */
	INTEL_EVENT_CONSTRAINT(0x63, 0x3), /* CACHE_LOCK_CYCLES */
	EVENT_CONSTRAINT_END
};

static struct extra_reg intel_nehalem_extra_regs[] __read_mostly =
{
	/* must define OFFCORE_RSP_X first, see intel_fixup_er() */
	INTEL_UEVENT_EXTRA_REG(0x01b7, MSR_OFFCORE_RSP_0, 0xffff, RSP_0),
	INTEL_UEVENT_PEBS_LDLAT_EXTRA_REG(0x100b),
	EVENT_EXTRA_END
};

static struct event_constraint intel_westmere_event_constraints[] __read_mostly =
{
	FIXED_EVENT_CONSTRAINT(0x00c0, 0), /* INST_RETIRED.ANY */
	FIXED_EVENT_CONSTRAINT(0x003c, 1), /* CPU_CLK_UNHALTED.CORE */
	FIXED_EVENT_CONSTRAINT(0x0300, 2), /* CPU_CLK_UNHALTED.REF */
	INTEL_EVENT_CONSTRAINT(0x51, 0x3), /* L1D */
	INTEL_EVENT_CONSTRAINT(0x60, 0x1), /* OFFCORE_REQUESTS_OUTSTANDING */
	INTEL_EVENT_CONSTRAINT(0x63, 0x3), /* CACHE_LOCK_CYCLES */
	INTEL_EVENT_CONSTRAINT(0xb3, 0x1), /* SNOOPQ_REQUEST_OUTSTANDING */
	EVENT_CONSTRAINT_END
};

static struct event_constraint intel_snb_event_constraints[] __read_mostly =
{
	FIXED_EVENT_CONSTRAINT(0x00c0, 0), /* INST_RETIRED.ANY */
	FIXED_EVENT_CONSTRAINT(0x003c, 1), /* CPU_CLK_UNHALTED.CORE */
	FIXED_EVENT_CONSTRAINT(0x0300, 2), /* CPU_CLK_UNHALTED.REF */
	INTEL_UEVENT_CONSTRAINT(0x04a3, 0xf), /* CYCLE_ACTIVITY.CYCLES_NO_DISPATCH */
	INTEL_UEVENT_CONSTRAINT(0x05a3, 0xf), /* CYCLE_ACTIVITY.STALLS_L2_PENDING */
	INTEL_UEVENT_CONSTRAINT(0x02a3, 0x4), /* CYCLE_ACTIVITY.CYCLES_L1D_PENDING */
	INTEL_UEVENT_CONSTRAINT(0x06a3, 0x4), /* CYCLE_ACTIVITY.STALLS_L1D_PENDING */
	INTEL_EVENT_CONSTRAINT(0x48, 0x4), /* L1D_PEND_MISS.PENDING */
	INTEL_UEVENT_CONSTRAINT(0x01c0, 0x2), /* INST_RETIRED.PREC_DIST */
	INTEL_EVENT_CONSTRAINT(0xcd, 0x8), /* MEM_TRANS_RETIRED.LOAD_LATENCY */
	INTEL_UEVENT_CONSTRAINT(0x04a3, 0xf), /* CYCLE_ACTIVITY.CYCLES_NO_DISPATCH */
	INTEL_UEVENT_CONSTRAINT(0x02a3, 0x4), /* CYCLE_ACTIVITY.CYCLES_L1D_PENDING */

	/*
	 * When HT is off these events can only run on the bottom 4 counters
	 * When HT is on, they are impacted by the HT bug and require EXCL access
	 */
	INTEL_EXCLEVT_CONSTRAINT(0xd0, 0xf), /* MEM_UOPS_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd1, 0xf), /* MEM_LOAD_UOPS_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd2, 0xf), /* MEM_LOAD_UOPS_LLC_HIT_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd3, 0xf), /* MEM_LOAD_UOPS_LLC_MISS_RETIRED.* */

	EVENT_CONSTRAINT_END
};

static struct event_constraint intel_ivb_event_constraints[] __read_mostly =
{
	FIXED_EVENT_CONSTRAINT(0x00c0, 0), /* INST_RETIRED.ANY */
	FIXED_EVENT_CONSTRAINT(0x003c, 1), /* CPU_CLK_UNHALTED.CORE */
	FIXED_EVENT_CONSTRAINT(0x0300, 2), /* CPU_CLK_UNHALTED.REF */
	INTEL_UEVENT_CONSTRAINT(0x0148, 0x4), /* L1D_PEND_MISS.PENDING */
	INTEL_UEVENT_CONSTRAINT(0x0279, 0xf), /* IDQ.EMTPY */
	INTEL_UEVENT_CONSTRAINT(0x019c, 0xf), /* IDQ_UOPS_NOT_DELIVERED.CORE */
	INTEL_UEVENT_CONSTRAINT(0x02a3, 0xf), /* CYCLE_ACTIVITY.CYCLES_LDM_PENDING */
	INTEL_UEVENT_CONSTRAINT(0x04a3, 0xf), /* CYCLE_ACTIVITY.CYCLES_NO_EXECUTE */
	INTEL_UEVENT_CONSTRAINT(0x05a3, 0xf), /* CYCLE_ACTIVITY.STALLS_L2_PENDING */
	INTEL_UEVENT_CONSTRAINT(0x06a3, 0xf), /* CYCLE_ACTIVITY.STALLS_LDM_PENDING */
	INTEL_UEVENT_CONSTRAINT(0x08a3, 0x4), /* CYCLE_ACTIVITY.CYCLES_L1D_PENDING */
	INTEL_UEVENT_CONSTRAINT(0x0ca3, 0x4), /* CYCLE_ACTIVITY.STALLS_L1D_PENDING */
	INTEL_UEVENT_CONSTRAINT(0x01c0, 0x2), /* INST_RETIRED.PREC_DIST */

	/*
	 * When HT is off these events can only run on the bottom 4 counters
	 * When HT is on, they are impacted by the HT bug and require EXCL access
	 */
	INTEL_EXCLEVT_CONSTRAINT(0xd0, 0xf), /* MEM_UOPS_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd1, 0xf), /* MEM_LOAD_UOPS_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd2, 0xf), /* MEM_LOAD_UOPS_LLC_HIT_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd3, 0xf), /* MEM_LOAD_UOPS_LLC_MISS_RETIRED.* */

	EVENT_CONSTRAINT_END
};

static struct extra_reg intel_westmere_extra_regs[] __read_mostly =
{
	/* must define OFFCORE_RSP_X first, see intel_fixup_er() */
	INTEL_UEVENT_EXTRA_REG(0x01b7, MSR_OFFCORE_RSP_0, 0xffff, RSP_0),
	INTEL_UEVENT_EXTRA_REG(0x01bb, MSR_OFFCORE_RSP_1, 0xffff, RSP_1),
	INTEL_UEVENT_PEBS_LDLAT_EXTRA_REG(0x100b),
	EVENT_EXTRA_END
};

static struct event_constraint intel_v1_event_constraints[] __read_mostly =
{
	EVENT_CONSTRAINT_END
};

static struct event_constraint intel_gen_event_constraints[] __read_mostly =
{
	FIXED_EVENT_CONSTRAINT(0x00c0, 0), /* INST_RETIRED.ANY */
	FIXED_EVENT_CONSTRAINT(0x003c, 1), /* CPU_CLK_UNHALTED.CORE */
	FIXED_EVENT_CONSTRAINT(0x0300, 2), /* CPU_CLK_UNHALTED.REF */
	EVENT_CONSTRAINT_END
};

static struct event_constraint intel_slm_event_constraints[] __read_mostly =
{
	FIXED_EVENT_CONSTRAINT(0x00c0, 0), /* INST_RETIRED.ANY */
	FIXED_EVENT_CONSTRAINT(0x003c, 1), /* CPU_CLK_UNHALTED.CORE */
	FIXED_EVENT_CONSTRAINT(0x0300, 2), /* pseudo CPU_CLK_UNHALTED.REF */
	EVENT_CONSTRAINT_END
};

static struct event_constraint intel_skl_event_constraints[] = {
	FIXED_EVENT_CONSTRAINT(0x00c0, 0),	/* INST_RETIRED.ANY */
	FIXED_EVENT_CONSTRAINT(0x003c, 1),	/* CPU_CLK_UNHALTED.CORE */
	FIXED_EVENT_CONSTRAINT(0x0300, 2),	/* CPU_CLK_UNHALTED.REF */
	INTEL_UEVENT_CONSTRAINT(0x1c0, 0x2),	/* INST_RETIRED.PREC_DIST */

	/*
	 * when HT is off, these can only run on the bottom 4 counters
	 */
	INTEL_EVENT_CONSTRAINT(0xd0, 0xf),	/* MEM_INST_RETIRED.* */
	INTEL_EVENT_CONSTRAINT(0xd1, 0xf),	/* MEM_LOAD_RETIRED.* */
	INTEL_EVENT_CONSTRAINT(0xd2, 0xf),	/* MEM_LOAD_L3_HIT_RETIRED.* */
	INTEL_EVENT_CONSTRAINT(0xcd, 0xf),	/* MEM_TRANS_RETIRED.* */
	INTEL_EVENT_CONSTRAINT(0xc6, 0xf),	/* FRONTEND_RETIRED.* */

	EVENT_CONSTRAINT_END
};

static struct extra_reg intel_knl_extra_regs[] __read_mostly = {
	INTEL_UEVENT_EXTRA_REG(0x01b7, MSR_OFFCORE_RSP_0, 0x799ffbb6e7ull, RSP_0),
	INTEL_UEVENT_EXTRA_REG(0x02b7, MSR_OFFCORE_RSP_1, 0x399ffbffe7ull, RSP_1),
	EVENT_EXTRA_END
};

static struct extra_reg intel_snb_extra_regs[] __read_mostly = {
	/* must define OFFCORE_RSP_X first, see intel_fixup_er() */
	INTEL_UEVENT_EXTRA_REG(0x01b7, MSR_OFFCORE_RSP_0, 0x3f807f8fffull, RSP_0),
	INTEL_UEVENT_EXTRA_REG(0x01bb, MSR_OFFCORE_RSP_1, 0x3f807f8fffull, RSP_1),
	INTEL_UEVENT_PEBS_LDLAT_EXTRA_REG(0x01cd),
	EVENT_EXTRA_END
};

static struct extra_reg intel_snbep_extra_regs[] __read_mostly = {
	/* must define OFFCORE_RSP_X first, see intel_fixup_er() */
	INTEL_UEVENT_EXTRA_REG(0x01b7, MSR_OFFCORE_RSP_0, 0x3fffff8fffull, RSP_0),
	INTEL_UEVENT_EXTRA_REG(0x01bb, MSR_OFFCORE_RSP_1, 0x3fffff8fffull, RSP_1),
	INTEL_UEVENT_PEBS_LDLAT_EXTRA_REG(0x01cd),
	EVENT_EXTRA_END
};

static struct extra_reg intel_skl_extra_regs[] __read_mostly = {
	INTEL_UEVENT_EXTRA_REG(0x01b7, MSR_OFFCORE_RSP_0, 0x3fffff8fffull, RSP_0),
	INTEL_UEVENT_EXTRA_REG(0x01bb, MSR_OFFCORE_RSP_1, 0x3fffff8fffull, RSP_1),
	INTEL_UEVENT_PEBS_LDLAT_EXTRA_REG(0x01cd),
	/*
	 * Note the low 8 bits eventsel code is not a continuous field, containing
	 * some #GPing bits. These are masked out.
	 */
	INTEL_UEVENT_EXTRA_REG(0x01c6, MSR_PEBS_FRONTEND, 0x7fff17, FE),
	EVENT_EXTRA_END
};

EVENT_ATTR_STR(mem-loads,	mem_ld_nhm,	"event=0x0b,umask=0x10,ldlat=3");
EVENT_ATTR_STR(mem-loads,	mem_ld_snb,	"event=0xcd,umask=0x1,ldlat=3");
EVENT_ATTR_STR(mem-stores,	mem_st_snb,	"event=0xcd,umask=0x2");

static struct attribute *nhm_events_attrs[] = {
	EVENT_PTR(mem_ld_nhm),
	NULL,
};

/*
 * topdown events for Intel Core CPUs.
 *
 * The events are all in slots, which is a free slot in a 4 wide
 * pipeline. Some events are already reported in slots, for cycle
 * events we multiply by the pipeline width (4).
 *
 * With Hyper Threading on, topdown metrics are either summed or averaged
 * between the threads of a core: (count_t0 + count_t1).
 *
 * For the average case the metric is always scaled to pipeline width,
 * so we use factor 2 ((count_t0 + count_t1) / 2 * 4)
 */

EVENT_ATTR_STR_HT(topdown-total-slots, td_total_slots,
	"event=0x3c,umask=0x0",			/* cpu_clk_unhalted.thread */
	"event=0x3c,umask=0x0,any=1");		/* cpu_clk_unhalted.thread_any */
EVENT_ATTR_STR_HT(topdown-total-slots.scale, td_total_slots_scale, "4", "2");
EVENT_ATTR_STR(topdown-slots-issued, td_slots_issued,
	"event=0xe,umask=0x1");			/* uops_issued.any */
EVENT_ATTR_STR(topdown-slots-retired, td_slots_retired,
	"event=0xc2,umask=0x2");		/* uops_retired.retire_slots */
EVENT_ATTR_STR(topdown-fetch-bubbles, td_fetch_bubbles,
	"event=0x9c,umask=0x1");		/* idq_uops_not_delivered_core */
EVENT_ATTR_STR_HT(topdown-recovery-bubbles, td_recovery_bubbles,
	"event=0xd,umask=0x3,cmask=1",		/* int_misc.recovery_cycles */
	"event=0xd,umask=0x3,cmask=1,any=1");	/* int_misc.recovery_cycles_any */
EVENT_ATTR_STR_HT(topdown-recovery-bubbles.scale, td_recovery_bubbles_scale,
	"4", "2");

static struct attribute *snb_events_attrs[] = {
	EVENT_PTR(mem_ld_snb),
	EVENT_PTR(mem_st_snb),
	EVENT_PTR(td_slots_issued),
	EVENT_PTR(td_slots_retired),
	EVENT_PTR(td_fetch_bubbles),
	EVENT_PTR(td_total_slots),
	EVENT_PTR(td_total_slots_scale),
	EVENT_PTR(td_recovery_bubbles),
	EVENT_PTR(td_recovery_bubbles_scale),
	NULL,
};

static struct event_constraint intel_hsw_event_constraints[] = {
	FIXED_EVENT_CONSTRAINT(0x00c0, 0), /* INST_RETIRED.ANY */
	FIXED_EVENT_CONSTRAINT(0x003c, 1), /* CPU_CLK_UNHALTED.CORE */
	FIXED_EVENT_CONSTRAINT(0x0300, 2), /* CPU_CLK_UNHALTED.REF */
	INTEL_UEVENT_CONSTRAINT(0x148, 0x4),	/* L1D_PEND_MISS.PENDING */
	INTEL_UEVENT_CONSTRAINT(0x01c0, 0x2), /* INST_RETIRED.PREC_DIST */
	INTEL_EVENT_CONSTRAINT(0xcd, 0x8), /* MEM_TRANS_RETIRED.LOAD_LATENCY */
	/* CYCLE_ACTIVITY.CYCLES_L1D_PENDING */
	INTEL_UEVENT_CONSTRAINT(0x08a3, 0x4),
	/* CYCLE_ACTIVITY.STALLS_L1D_PENDING */
	INTEL_UEVENT_CONSTRAINT(0x0ca3, 0x4),
	/* CYCLE_ACTIVITY.CYCLES_NO_EXECUTE */
	INTEL_UEVENT_CONSTRAINT(0x04a3, 0xf),

	/*
	 * When HT is off these events can only run on the bottom 4 counters
	 * When HT is on, they are impacted by the HT bug and require EXCL access
	 */
	INTEL_EXCLEVT_CONSTRAINT(0xd0, 0xf), /* MEM_UOPS_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd1, 0xf), /* MEM_LOAD_UOPS_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd2, 0xf), /* MEM_LOAD_UOPS_LLC_HIT_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd3, 0xf), /* MEM_LOAD_UOPS_LLC_MISS_RETIRED.* */

	EVENT_CONSTRAINT_END
};

static struct event_constraint intel_bdw_event_constraints[] = {
	FIXED_EVENT_CONSTRAINT(0x00c0, 0),	/* INST_RETIRED.ANY */
	FIXED_EVENT_CONSTRAINT(0x003c, 1),	/* CPU_CLK_UNHALTED.CORE */
	FIXED_EVENT_CONSTRAINT(0x0300, 2),	/* CPU_CLK_UNHALTED.REF */
	INTEL_UEVENT_CONSTRAINT(0x148, 0x4),	/* L1D_PEND_MISS.PENDING */
	INTEL_UBIT_EVENT_CONSTRAINT(0x8a3, 0x4),	/* CYCLE_ACTIVITY.CYCLES_L1D_MISS */
	/*
	 * when HT is off, these can only run on the bottom 4 counters
	 */
	INTEL_EVENT_CONSTRAINT(0xd0, 0xf),	/* MEM_INST_RETIRED.* */
	INTEL_EVENT_CONSTRAINT(0xd1, 0xf),	/* MEM_LOAD_RETIRED.* */
	INTEL_EVENT_CONSTRAINT(0xd2, 0xf),	/* MEM_LOAD_L3_HIT_RETIRED.* */
	INTEL_EVENT_CONSTRAINT(0xcd, 0xf),	/* MEM_TRANS_RETIRED.* */
	EVENT_CONSTRAINT_END
};

static u64 intel_pmu_event_map(int hw_event)
{
	return intel_perfmon_event_map[hw_event];
}

/*
 * Notes on the events:
 * - data reads do not include code reads (comparable to earlier tables)
 * - data counts include speculative execution (except L1 write, dtlb, bpu)
 * - remote node access includes remote memory, remote cache, remote mmio.
 * - prefetches are not included in the counts.
 * - icache miss does not include decoded icache
 */

#define SKL_DEMAND_DATA_RD		BIT_ULL(0)
#define SKL_DEMAND_RFO			BIT_ULL(1)
#define SKL_ANY_RESPONSE		BIT_ULL(16)
#define SKL_SUPPLIER_NONE		BIT_ULL(17)
#define SKL_L3_MISS_LOCAL_DRAM		BIT_ULL(26)
#define SKL_L3_MISS_REMOTE_HOP0_DRAM	BIT_ULL(27)
#define SKL_L3_MISS_REMOTE_HOP1_DRAM	BIT_ULL(28)
#define SKL_L3_MISS_REMOTE_HOP2P_DRAM	BIT_ULL(29)
#define SKL_L3_MISS			(SKL_L3_MISS_LOCAL_DRAM| \
					 SKL_L3_MISS_REMOTE_HOP0_DRAM| \
					 SKL_L3_MISS_REMOTE_HOP1_DRAM| \
					 SKL_L3_MISS_REMOTE_HOP2P_DRAM)
#define SKL_SPL_HIT			BIT_ULL(30)
#define SKL_SNOOP_NONE			BIT_ULL(31)
#define SKL_SNOOP_NOT_NEEDED		BIT_ULL(32)
#define SKL_SNOOP_MISS			BIT_ULL(33)
#define SKL_SNOOP_HIT_NO_FWD		BIT_ULL(34)
#define SKL_SNOOP_HIT_WITH_FWD		BIT_ULL(35)
#define SKL_SNOOP_HITM			BIT_ULL(36)
#define SKL_SNOOP_NON_DRAM		BIT_ULL(37)
#define SKL_ANY_SNOOP			(SKL_SPL_HIT|SKL_SNOOP_NONE| \
					 SKL_SNOOP_NOT_NEEDED|SKL_SNOOP_MISS| \
					 SKL_SNOOP_HIT_NO_FWD|SKL_SNOOP_HIT_WITH_FWD| \
					 SKL_SNOOP_HITM|SKL_SNOOP_NON_DRAM)
#define SKL_DEMAND_READ			SKL_DEMAND_DATA_RD
#define SKL_SNOOP_DRAM			(SKL_SNOOP_NONE| \
					 SKL_SNOOP_NOT_NEEDED|SKL_SNOOP_MISS| \
					 SKL_SNOOP_HIT_NO_FWD|SKL_SNOOP_HIT_WITH_FWD| \
					 SKL_SNOOP_HITM|SKL_SPL_HIT)
#define SKL_DEMAND_WRITE		SKL_DEMAND_RFO
#define SKL_LLC_ACCESS			SKL_ANY_RESPONSE
#define SKL_L3_MISS_REMOTE		(SKL_L3_MISS_REMOTE_HOP0_DRAM| \
					 SKL_L3_MISS_REMOTE_HOP1_DRAM| \
					 SKL_L3_MISS_REMOTE_HOP2P_DRAM)

static __initconst const u64 skl_hw_cache_event_ids
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(L1D ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x81d0,	/* MEM_INST_RETIRED.ALL_LOADS */
		[ C(RESULT_MISS)   ] = 0x151,	/* L1D.REPLACEMENT */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x82d0,	/* MEM_INST_RETIRED.ALL_STORES */
		[ C(RESULT_MISS)   ] = 0x0,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(L1I ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x283,	/* ICACHE_64B.MISS */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x1b7,	/* OFFCORE_RESPONSE */
		[ C(RESULT_MISS)   ] = 0x1b7,	/* OFFCORE_RESPONSE */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x1b7,	/* OFFCORE_RESPONSE */
		[ C(RESULT_MISS)   ] = 0x1b7,	/* OFFCORE_RESPONSE */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(DTLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x81d0,	/* MEM_INST_RETIRED.ALL_LOADS */
		[ C(RESULT_MISS)   ] = 0xe08,	/* DTLB_LOAD_MISSES.WALK_COMPLETED */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x82d0,	/* MEM_INST_RETIRED.ALL_STORES */
		[ C(RESULT_MISS)   ] = 0xe49,	/* DTLB_STORE_MISSES.WALK_COMPLETED */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(ITLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x2085,	/* ITLB_MISSES.STLB_HIT */
		[ C(RESULT_MISS)   ] = 0xe85,	/* ITLB_MISSES.WALK_COMPLETED */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(BPU ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0xc4,	/* BR_INST_RETIRED.ALL_BRANCHES */
		[ C(RESULT_MISS)   ] = 0xc5,	/* BR_MISP_RETIRED.ALL_BRANCHES */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(NODE) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x1b7,	/* OFFCORE_RESPONSE */
		[ C(RESULT_MISS)   ] = 0x1b7,	/* OFFCORE_RESPONSE */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x1b7,	/* OFFCORE_RESPONSE */
		[ C(RESULT_MISS)   ] = 0x1b7,	/* OFFCORE_RESPONSE */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
};

static __initconst const u64 skl_hw_cache_extra_regs
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = SKL_DEMAND_READ|
				       SKL_LLC_ACCESS|SKL_ANY_SNOOP,
		[ C(RESULT_MISS)   ] = SKL_DEMAND_READ|
				       SKL_L3_MISS|SKL_ANY_SNOOP|
				       SKL_SUPPLIER_NONE,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = SKL_DEMAND_WRITE|
				       SKL_LLC_ACCESS|SKL_ANY_SNOOP,
		[ C(RESULT_MISS)   ] = SKL_DEMAND_WRITE|
				       SKL_L3_MISS|SKL_ANY_SNOOP|
				       SKL_SUPPLIER_NONE,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(NODE) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = SKL_DEMAND_READ|
				       SKL_L3_MISS_LOCAL_DRAM|SKL_SNOOP_DRAM,
		[ C(RESULT_MISS)   ] = SKL_DEMAND_READ|
				       SKL_L3_MISS_REMOTE|SKL_SNOOP_DRAM,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = SKL_DEMAND_WRITE|
				       SKL_L3_MISS_LOCAL_DRAM|SKL_SNOOP_DRAM,
		[ C(RESULT_MISS)   ] = SKL_DEMAND_WRITE|
				       SKL_L3_MISS_REMOTE|SKL_SNOOP_DRAM,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
};

#define SNB_DMND_DATA_RD	(1ULL << 0)
#define SNB_DMND_RFO		(1ULL << 1)
#define SNB_DMND_IFETCH		(1ULL << 2)
#define SNB_DMND_WB		(1ULL << 3)
#define SNB_PF_DATA_RD		(1ULL << 4)
#define SNB_PF_RFO		(1ULL << 5)
#define SNB_PF_IFETCH		(1ULL << 6)
#define SNB_LLC_DATA_RD		(1ULL << 7)
#define SNB_LLC_RFO		(1ULL << 8)
#define SNB_LLC_IFETCH		(1ULL << 9)
#define SNB_BUS_LOCKS		(1ULL << 10)
#define SNB_STRM_ST		(1ULL << 11)
#define SNB_OTHER		(1ULL << 15)
#define SNB_RESP_ANY		(1ULL << 16)
#define SNB_NO_SUPP		(1ULL << 17)
#define SNB_LLC_HITM		(1ULL << 18)
#define SNB_LLC_HITE		(1ULL << 19)
#define SNB_LLC_HITS		(1ULL << 20)
#define SNB_LLC_HITF		(1ULL << 21)
#define SNB_LOCAL		(1ULL << 22)
#define SNB_REMOTE		(0xffULL << 23)
#define SNB_SNP_NONE		(1ULL << 31)
#define SNB_SNP_NOT_NEEDED	(1ULL << 32)
#define SNB_SNP_MISS		(1ULL << 33)
#define SNB_NO_FWD		(1ULL << 34)
#define SNB_SNP_FWD		(1ULL << 35)
#define SNB_HITM		(1ULL << 36)
#define SNB_NON_DRAM		(1ULL << 37)

#define SNB_DMND_READ		(SNB_DMND_DATA_RD|SNB_LLC_DATA_RD)
#define SNB_DMND_WRITE		(SNB_DMND_RFO|SNB_LLC_RFO)
#define SNB_DMND_PREFETCH	(SNB_PF_DATA_RD|SNB_PF_RFO)

#define SNB_SNP_ANY		(SNB_SNP_NONE|SNB_SNP_NOT_NEEDED| \
				 SNB_SNP_MISS|SNB_NO_FWD|SNB_SNP_FWD| \
				 SNB_HITM)

#define SNB_DRAM_ANY		(SNB_LOCAL|SNB_REMOTE|SNB_SNP_ANY)
#define SNB_DRAM_REMOTE		(SNB_REMOTE|SNB_SNP_ANY)

#define SNB_L3_ACCESS		SNB_RESP_ANY
#define SNB_L3_MISS		(SNB_DRAM_ANY|SNB_NON_DRAM)

static __initconst const u64 snb_hw_cache_extra_regs
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = SNB_DMND_READ|SNB_L3_ACCESS,
		[ C(RESULT_MISS)   ] = SNB_DMND_READ|SNB_L3_MISS,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = SNB_DMND_WRITE|SNB_L3_ACCESS,
		[ C(RESULT_MISS)   ] = SNB_DMND_WRITE|SNB_L3_MISS,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = SNB_DMND_PREFETCH|SNB_L3_ACCESS,
		[ C(RESULT_MISS)   ] = SNB_DMND_PREFETCH|SNB_L3_MISS,
	},
 },
 [ C(NODE) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = SNB_DMND_READ|SNB_DRAM_ANY,
		[ C(RESULT_MISS)   ] = SNB_DMND_READ|SNB_DRAM_REMOTE,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = SNB_DMND_WRITE|SNB_DRAM_ANY,
		[ C(RESULT_MISS)   ] = SNB_DMND_WRITE|SNB_DRAM_REMOTE,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = SNB_DMND_PREFETCH|SNB_DRAM_ANY,
		[ C(RESULT_MISS)   ] = SNB_DMND_PREFETCH|SNB_DRAM_REMOTE,
	},
 },
};

static __initconst const u64 snb_hw_cache_event_ids
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(L1D) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0xf1d0, /* MEM_UOP_RETIRED.LOADS        */
		[ C(RESULT_MISS)   ] = 0x0151, /* L1D.REPLACEMENT              */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0xf2d0, /* MEM_UOP_RETIRED.STORES       */
		[ C(RESULT_MISS)   ] = 0x0851, /* L1D.ALL_M_REPLACEMENT        */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x024e, /* HW_PRE_REQ.DL1_MISS          */
	},
 },
 [ C(L1I ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0280, /* ICACHE.MISSES */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		/* OFFCORE_RESPONSE.ANY_DATA.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		/* OFFCORE_RESPONSE.ANY_DATA.ANY_LLC_MISS */
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	[ C(OP_WRITE) ] = {
		/* OFFCORE_RESPONSE.ANY_RFO.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		/* OFFCORE_RESPONSE.ANY_RFO.ANY_LLC_MISS */
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	[ C(OP_PREFETCH) ] = {
		/* OFFCORE_RESPONSE.PREFETCH.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		/* OFFCORE_RESPONSE.PREFETCH.ANY_LLC_MISS */
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
 },
 [ C(DTLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x81d0, /* MEM_UOP_RETIRED.ALL_LOADS */
		[ C(RESULT_MISS)   ] = 0x0108, /* DTLB_LOAD_MISSES.CAUSES_A_WALK */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x82d0, /* MEM_UOP_RETIRED.ALL_STORES */
		[ C(RESULT_MISS)   ] = 0x0149, /* DTLB_STORE_MISSES.MISS_CAUSES_A_WALK */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(ITLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x1085, /* ITLB_MISSES.STLB_HIT         */
		[ C(RESULT_MISS)   ] = 0x0185, /* ITLB_MISSES.CAUSES_A_WALK    */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(BPU ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x00c4, /* BR_INST_RETIRED.ALL_BRANCHES */
		[ C(RESULT_MISS)   ] = 0x00c5, /* BR_MISP_RETIRED.ALL_BRANCHES */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(NODE) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x01b7,
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x01b7,
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x01b7,
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
 },

};

/*
 * Notes on the events:
 * - data reads do not include code reads (comparable to earlier tables)
 * - data counts include speculative execution (except L1 write, dtlb, bpu)
 * - remote node access includes remote memory, remote cache, remote mmio.
 * - prefetches are not included in the counts because they are not
 *   reliably counted.
 */

#define HSW_DEMAND_DATA_RD		BIT_ULL(0)
#define HSW_DEMAND_RFO			BIT_ULL(1)
#define HSW_ANY_RESPONSE		BIT_ULL(16)
#define HSW_SUPPLIER_NONE		BIT_ULL(17)
#define HSW_L3_MISS_LOCAL_DRAM		BIT_ULL(22)
#define HSW_L3_MISS_REMOTE_HOP0		BIT_ULL(27)
#define HSW_L3_MISS_REMOTE_HOP1		BIT_ULL(28)
#define HSW_L3_MISS_REMOTE_HOP2P	BIT_ULL(29)
#define HSW_L3_MISS			(HSW_L3_MISS_LOCAL_DRAM| \
					 HSW_L3_MISS_REMOTE_HOP0|HSW_L3_MISS_REMOTE_HOP1| \
					 HSW_L3_MISS_REMOTE_HOP2P)
#define HSW_SNOOP_NONE			BIT_ULL(31)
#define HSW_SNOOP_NOT_NEEDED		BIT_ULL(32)
#define HSW_SNOOP_MISS			BIT_ULL(33)
#define HSW_SNOOP_HIT_NO_FWD		BIT_ULL(34)
#define HSW_SNOOP_HIT_WITH_FWD		BIT_ULL(35)
#define HSW_SNOOP_HITM			BIT_ULL(36)
#define HSW_SNOOP_NON_DRAM		BIT_ULL(37)
#define HSW_ANY_SNOOP			(HSW_SNOOP_NONE| \
					 HSW_SNOOP_NOT_NEEDED|HSW_SNOOP_MISS| \
					 HSW_SNOOP_HIT_NO_FWD|HSW_SNOOP_HIT_WITH_FWD| \
					 HSW_SNOOP_HITM|HSW_SNOOP_NON_DRAM)
#define HSW_SNOOP_DRAM			(HSW_ANY_SNOOP & ~HSW_SNOOP_NON_DRAM)
#define HSW_DEMAND_READ			HSW_DEMAND_DATA_RD
#define HSW_DEMAND_WRITE		HSW_DEMAND_RFO
#define HSW_L3_MISS_REMOTE		(HSW_L3_MISS_REMOTE_HOP0|\
					 HSW_L3_MISS_REMOTE_HOP1|HSW_L3_MISS_REMOTE_HOP2P)
#define HSW_LLC_ACCESS			HSW_ANY_RESPONSE

#define BDW_L3_MISS_LOCAL		BIT(26)
#define BDW_L3_MISS			(BDW_L3_MISS_LOCAL| \
					 HSW_L3_MISS_REMOTE_HOP0|HSW_L3_MISS_REMOTE_HOP1| \
					 HSW_L3_MISS_REMOTE_HOP2P)


static __initconst const u64 hsw_hw_cache_event_ids
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(L1D ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x81d0,	/* MEM_UOPS_RETIRED.ALL_LOADS */
		[ C(RESULT_MISS)   ] = 0x151,	/* L1D.REPLACEMENT */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x82d0,	/* MEM_UOPS_RETIRED.ALL_STORES */
		[ C(RESULT_MISS)   ] = 0x0,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(L1I ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x280,	/* ICACHE.MISSES */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x1b7,	/* OFFCORE_RESPONSE */
		[ C(RESULT_MISS)   ] = 0x1b7,	/* OFFCORE_RESPONSE */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x1b7,	/* OFFCORE_RESPONSE */
		[ C(RESULT_MISS)   ] = 0x1b7,	/* OFFCORE_RESPONSE */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(DTLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x81d0,	/* MEM_UOPS_RETIRED.ALL_LOADS */
		[ C(RESULT_MISS)   ] = 0x108,	/* DTLB_LOAD_MISSES.MISS_CAUSES_A_WALK */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x82d0,	/* MEM_UOPS_RETIRED.ALL_STORES */
		[ C(RESULT_MISS)   ] = 0x149,	/* DTLB_STORE_MISSES.MISS_CAUSES_A_WALK */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(ITLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x6085,	/* ITLB_MISSES.STLB_HIT */
		[ C(RESULT_MISS)   ] = 0x185,	/* ITLB_MISSES.MISS_CAUSES_A_WALK */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(BPU ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0xc4,	/* BR_INST_RETIRED.ALL_BRANCHES */
		[ C(RESULT_MISS)   ] = 0xc5,	/* BR_MISP_RETIRED.ALL_BRANCHES */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(NODE) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x1b7,	/* OFFCORE_RESPONSE */
		[ C(RESULT_MISS)   ] = 0x1b7,	/* OFFCORE_RESPONSE */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x1b7,	/* OFFCORE_RESPONSE */
		[ C(RESULT_MISS)   ] = 0x1b7,	/* OFFCORE_RESPONSE */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
};

static __initconst const u64 hsw_hw_cache_extra_regs
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = HSW_DEMAND_READ|
				       HSW_LLC_ACCESS,
		[ C(RESULT_MISS)   ] = HSW_DEMAND_READ|
				       HSW_L3_MISS|HSW_ANY_SNOOP,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = HSW_DEMAND_WRITE|
				       HSW_LLC_ACCESS,
		[ C(RESULT_MISS)   ] = HSW_DEMAND_WRITE|
				       HSW_L3_MISS|HSW_ANY_SNOOP,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(NODE) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = HSW_DEMAND_READ|
				       HSW_L3_MISS_LOCAL_DRAM|
				       HSW_SNOOP_DRAM,
		[ C(RESULT_MISS)   ] = HSW_DEMAND_READ|
				       HSW_L3_MISS_REMOTE|
				       HSW_SNOOP_DRAM,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = HSW_DEMAND_WRITE|
				       HSW_L3_MISS_LOCAL_DRAM|
				       HSW_SNOOP_DRAM,
		[ C(RESULT_MISS)   ] = HSW_DEMAND_WRITE|
				       HSW_L3_MISS_REMOTE|
				       HSW_SNOOP_DRAM,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
};

static __initconst const u64 westmere_hw_cache_event_ids
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(L1D) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x010b, /* MEM_INST_RETIRED.LOADS       */
		[ C(RESULT_MISS)   ] = 0x0151, /* L1D.REPL                     */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x020b, /* MEM_INST_RETURED.STORES      */
		[ C(RESULT_MISS)   ] = 0x0251, /* L1D.M_REPL                   */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x014e, /* L1D_PREFETCH.REQUESTS        */
		[ C(RESULT_MISS)   ] = 0x024e, /* L1D_PREFETCH.MISS            */
	},
 },
 [ C(L1I ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x0380, /* L1I.READS                    */
		[ C(RESULT_MISS)   ] = 0x0280, /* L1I.MISSES                   */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		/* OFFCORE_RESPONSE.ANY_DATA.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		/* OFFCORE_RESPONSE.ANY_DATA.ANY_LLC_MISS */
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	/*
	 * Use RFO, not WRITEBACK, because a write miss would typically occur
	 * on RFO.
	 */
	[ C(OP_WRITE) ] = {
		/* OFFCORE_RESPONSE.ANY_RFO.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		/* OFFCORE_RESPONSE.ANY_RFO.ANY_LLC_MISS */
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	[ C(OP_PREFETCH) ] = {
		/* OFFCORE_RESPONSE.PREFETCH.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		/* OFFCORE_RESPONSE.PREFETCH.ANY_LLC_MISS */
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
 },
 [ C(DTLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x010b, /* MEM_INST_RETIRED.LOADS       */
		[ C(RESULT_MISS)   ] = 0x0108, /* DTLB_LOAD_MISSES.ANY         */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x020b, /* MEM_INST_RETURED.STORES      */
		[ C(RESULT_MISS)   ] = 0x010c, /* MEM_STORE_RETIRED.DTLB_MISS  */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(ITLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x01c0, /* INST_RETIRED.ANY_P           */
		[ C(RESULT_MISS)   ] = 0x0185, /* ITLB_MISSES.ANY              */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(BPU ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x00c4, /* BR_INST_RETIRED.ALL_BRANCHES */
		[ C(RESULT_MISS)   ] = 0x03e8, /* BPU_CLEARS.ANY               */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(NODE) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x01b7,
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x01b7,
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x01b7,
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
 },
};

/*
 * Nehalem/Westmere MSR_OFFCORE_RESPONSE bits;
 * See IA32 SDM Vol 3B 30.6.1.3
 */

#define NHM_DMND_DATA_RD	(1 << 0)
#define NHM_DMND_RFO		(1 << 1)
#define NHM_DMND_IFETCH		(1 << 2)
#define NHM_DMND_WB		(1 << 3)
#define NHM_PF_DATA_RD		(1 << 4)
#define NHM_PF_DATA_RFO		(1 << 5)
#define NHM_PF_IFETCH		(1 << 6)
#define NHM_OFFCORE_OTHER	(1 << 7)
#define NHM_UNCORE_HIT		(1 << 8)
#define NHM_OTHER_CORE_HIT_SNP	(1 << 9)
#define NHM_OTHER_CORE_HITM	(1 << 10)
        			/* reserved */
#define NHM_REMOTE_CACHE_FWD	(1 << 12)
#define NHM_REMOTE_DRAM		(1 << 13)
#define NHM_LOCAL_DRAM		(1 << 14)
#define NHM_NON_DRAM		(1 << 15)

#define NHM_LOCAL		(NHM_LOCAL_DRAM|NHM_REMOTE_CACHE_FWD)
#define NHM_REMOTE		(NHM_REMOTE_DRAM)

#define NHM_DMND_READ		(NHM_DMND_DATA_RD)
#define NHM_DMND_WRITE		(NHM_DMND_RFO|NHM_DMND_WB)
#define NHM_DMND_PREFETCH	(NHM_PF_DATA_RD|NHM_PF_DATA_RFO)

#define NHM_L3_HIT	(NHM_UNCORE_HIT|NHM_OTHER_CORE_HIT_SNP|NHM_OTHER_CORE_HITM)
#define NHM_L3_MISS	(NHM_NON_DRAM|NHM_LOCAL_DRAM|NHM_REMOTE_DRAM|NHM_REMOTE_CACHE_FWD)
#define NHM_L3_ACCESS	(NHM_L3_HIT|NHM_L3_MISS)

static __initconst const u64 nehalem_hw_cache_extra_regs
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = NHM_DMND_READ|NHM_L3_ACCESS,
		[ C(RESULT_MISS)   ] = NHM_DMND_READ|NHM_L3_MISS,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = NHM_DMND_WRITE|NHM_L3_ACCESS,
		[ C(RESULT_MISS)   ] = NHM_DMND_WRITE|NHM_L3_MISS,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = NHM_DMND_PREFETCH|NHM_L3_ACCESS,
		[ C(RESULT_MISS)   ] = NHM_DMND_PREFETCH|NHM_L3_MISS,
	},
 },
 [ C(NODE) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = NHM_DMND_READ|NHM_LOCAL|NHM_REMOTE,
		[ C(RESULT_MISS)   ] = NHM_DMND_READ|NHM_REMOTE,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = NHM_DMND_WRITE|NHM_LOCAL|NHM_REMOTE,
		[ C(RESULT_MISS)   ] = NHM_DMND_WRITE|NHM_REMOTE,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = NHM_DMND_PREFETCH|NHM_LOCAL|NHM_REMOTE,
		[ C(RESULT_MISS)   ] = NHM_DMND_PREFETCH|NHM_REMOTE,
	},
 },
};

static __initconst const u64 nehalem_hw_cache_event_ids
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(L1D) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x010b, /* MEM_INST_RETIRED.LOADS       */
		[ C(RESULT_MISS)   ] = 0x0151, /* L1D.REPL                     */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x020b, /* MEM_INST_RETURED.STORES      */
		[ C(RESULT_MISS)   ] = 0x0251, /* L1D.M_REPL                   */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x014e, /* L1D_PREFETCH.REQUESTS        */
		[ C(RESULT_MISS)   ] = 0x024e, /* L1D_PREFETCH.MISS            */
	},
 },
 [ C(L1I ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x0380, /* L1I.READS                    */
		[ C(RESULT_MISS)   ] = 0x0280, /* L1I.MISSES                   */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		/* OFFCORE_RESPONSE.ANY_DATA.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		/* OFFCORE_RESPONSE.ANY_DATA.ANY_LLC_MISS */
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	/*
	 * Use RFO, not WRITEBACK, because a write miss would typically occur
	 * on RFO.
	 */
	[ C(OP_WRITE) ] = {
		/* OFFCORE_RESPONSE.ANY_RFO.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		/* OFFCORE_RESPONSE.ANY_RFO.ANY_LLC_MISS */
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	[ C(OP_PREFETCH) ] = {
		/* OFFCORE_RESPONSE.PREFETCH.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		/* OFFCORE_RESPONSE.PREFETCH.ANY_LLC_MISS */
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
 },
 [ C(DTLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x0f40, /* L1D_CACHE_LD.MESI   (alias)  */
		[ C(RESULT_MISS)   ] = 0x0108, /* DTLB_LOAD_MISSES.ANY         */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x0f41, /* L1D_CACHE_ST.MESI   (alias)  */
		[ C(RESULT_MISS)   ] = 0x010c, /* MEM_STORE_RETIRED.DTLB_MISS  */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0x0,
	},
 },
 [ C(ITLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x01c0, /* INST_RETIRED.ANY_P           */
		[ C(RESULT_MISS)   ] = 0x20c8, /* ITLB_MISS_RETIRED            */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(BPU ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x00c4, /* BR_INST_RETIRED.ALL_BRANCHES */
		[ C(RESULT_MISS)   ] = 0x03e8, /* BPU_CLEARS.ANY               */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(NODE) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x01b7,
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x01b7,
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x01b7,
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
 },
};

static __initconst const u64 core2_hw_cache_event_ids
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(L1D) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x0f40, /* L1D_CACHE_LD.MESI          */
		[ C(RESULT_MISS)   ] = 0x0140, /* L1D_CACHE_LD.I_STATE       */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x0f41, /* L1D_CACHE_ST.MESI          */
		[ C(RESULT_MISS)   ] = 0x0141, /* L1D_CACHE_ST.I_STATE       */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x104e, /* L1D_PREFETCH.REQUESTS      */
		[ C(RESULT_MISS)   ] = 0,
	},
 },
 [ C(L1I ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x0080, /* L1I.READS                  */
		[ C(RESULT_MISS)   ] = 0x0081, /* L1I.MISSES                 */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0,
	},
 },
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x4f29, /* L2_LD.MESI                 */
		[ C(RESULT_MISS)   ] = 0x4129, /* L2_LD.ISTATE               */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x4f2A, /* L2_ST.MESI                 */
		[ C(RESULT_MISS)   ] = 0x412A, /* L2_ST.ISTATE               */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0,
	},
 },
 [ C(DTLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x0f40, /* L1D_CACHE_LD.MESI  (alias) */
		[ C(RESULT_MISS)   ] = 0x0208, /* DTLB_MISSES.MISS_LD        */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x0f41, /* L1D_CACHE_ST.MESI  (alias) */
		[ C(RESULT_MISS)   ] = 0x0808, /* DTLB_MISSES.MISS_ST        */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0,
	},
 },
 [ C(ITLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x00c0, /* INST_RETIRED.ANY_P         */
		[ C(RESULT_MISS)   ] = 0x1282, /* ITLBMISSES                 */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(BPU ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x00c4, /* BR_INST_RETIRED.ANY        */
		[ C(RESULT_MISS)   ] = 0x00c5, /* BP_INST_RETIRED.MISPRED    */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
};

static __initconst const u64 atom_hw_cache_event_ids
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(L1D) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x2140, /* L1D_CACHE.LD               */
		[ C(RESULT_MISS)   ] = 0,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x2240, /* L1D_CACHE.ST               */
		[ C(RESULT_MISS)   ] = 0,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0x0,
		[ C(RESULT_MISS)   ] = 0,
	},
 },
 [ C(L1I ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x0380, /* L1I.READS                  */
		[ C(RESULT_MISS)   ] = 0x0280, /* L1I.MISSES                 */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0,
	},
 },
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x4f29, /* L2_LD.MESI                 */
		[ C(RESULT_MISS)   ] = 0x4129, /* L2_LD.ISTATE               */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x4f2A, /* L2_ST.MESI                 */
		[ C(RESULT_MISS)   ] = 0x412A, /* L2_ST.ISTATE               */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0,
	},
 },
 [ C(DTLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x2140, /* L1D_CACHE_LD.MESI  (alias) */
		[ C(RESULT_MISS)   ] = 0x0508, /* DTLB_MISSES.MISS_LD        */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0x2240, /* L1D_CACHE_ST.MESI  (alias) */
		[ C(RESULT_MISS)   ] = 0x0608, /* DTLB_MISSES.MISS_ST        */
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0,
	},
 },
 [ C(ITLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x00c0, /* INST_RETIRED.ANY_P         */
		[ C(RESULT_MISS)   ] = 0x0282, /* ITLB.MISSES                */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(BPU ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x00c4, /* BR_INST_RETIRED.ANY        */
		[ C(RESULT_MISS)   ] = 0x00c5, /* BP_INST_RETIRED.MISPRED    */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
};

EVENT_ATTR_STR(topdown-total-slots, td_total_slots_slm, "event=0x3c");
EVENT_ATTR_STR(topdown-total-slots.scale, td_total_slots_scale_slm, "2");
/* no_alloc_cycles.not_delivered */
EVENT_ATTR_STR(topdown-fetch-bubbles, td_fetch_bubbles_slm,
	       "event=0xca,umask=0x50");
EVENT_ATTR_STR(topdown-fetch-bubbles.scale, td_fetch_bubbles_scale_slm, "2");
/* uops_retired.all */
EVENT_ATTR_STR(topdown-slots-issued, td_slots_issued_slm,
	       "event=0xc2,umask=0x10");
/* uops_retired.all */
EVENT_ATTR_STR(topdown-slots-retired, td_slots_retired_slm,
	       "event=0xc2,umask=0x10");

static struct attribute *slm_events_attrs[] = {
	EVENT_PTR(td_total_slots_slm),
	EVENT_PTR(td_total_slots_scale_slm),
	EVENT_PTR(td_fetch_bubbles_slm),
	EVENT_PTR(td_fetch_bubbles_scale_slm),
	EVENT_PTR(td_slots_issued_slm),
	EVENT_PTR(td_slots_retired_slm),
	NULL
};

static struct extra_reg intel_slm_extra_regs[] __read_mostly =
{
	/* must define OFFCORE_RSP_X first, see intel_fixup_er() */
	INTEL_UEVENT_EXTRA_REG(0x01b7, MSR_OFFCORE_RSP_0, 0x768005ffffull, RSP_0),
	INTEL_UEVENT_EXTRA_REG(0x02b7, MSR_OFFCORE_RSP_1, 0x368005ffffull, RSP_1),
	EVENT_EXTRA_END
};

#define SLM_DMND_READ		SNB_DMND_DATA_RD
#define SLM_DMND_WRITE		SNB_DMND_RFO
#define SLM_DMND_PREFETCH	(SNB_PF_DATA_RD|SNB_PF_RFO)

#define SLM_SNP_ANY		(SNB_SNP_NONE|SNB_SNP_MISS|SNB_NO_FWD|SNB_HITM)
#define SLM_LLC_ACCESS		SNB_RESP_ANY
#define SLM_LLC_MISS		(SLM_SNP_ANY|SNB_NON_DRAM)

static __initconst const u64 slm_hw_cache_extra_regs
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = SLM_DMND_READ|SLM_LLC_ACCESS,
		[ C(RESULT_MISS)   ] = 0,
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = SLM_DMND_WRITE|SLM_LLC_ACCESS,
		[ C(RESULT_MISS)   ] = SLM_DMND_WRITE|SLM_LLC_MISS,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = SLM_DMND_PREFETCH|SLM_LLC_ACCESS,
		[ C(RESULT_MISS)   ] = SLM_DMND_PREFETCH|SLM_LLC_MISS,
	},
 },
};

static __initconst const u64 slm_hw_cache_event_ids
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
 [ C(L1D) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0x0104, /* LD_DCU_MISS */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0,
	},
 },
 [ C(L1I ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x0380, /* ICACHE.ACCESSES */
		[ C(RESULT_MISS)   ] = 0x0280, /* ICACGE.MISSES */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0,
	},
 },
 [ C(LL  ) ] = {
	[ C(OP_READ) ] = {
		/* OFFCORE_RESPONSE.ANY_DATA.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		[ C(RESULT_MISS)   ] = 0,
	},
	[ C(OP_WRITE) ] = {
		/* OFFCORE_RESPONSE.ANY_RFO.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		/* OFFCORE_RESPONSE.ANY_RFO.ANY_LLC_MISS */
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
	[ C(OP_PREFETCH) ] = {
		/* OFFCORE_RESPONSE.PREFETCH.LOCAL_CACHE */
		[ C(RESULT_ACCESS) ] = 0x01b7,
		/* OFFCORE_RESPONSE.PREFETCH.ANY_LLC_MISS */
		[ C(RESULT_MISS)   ] = 0x01b7,
	},
 },
 [ C(DTLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0x0804, /* LD_DTLB_MISS */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = 0,
		[ C(RESULT_MISS)   ] = 0,
	},
 },
 [ C(ITLB) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x00c0, /* INST_RETIRED.ANY_P */
		[ C(RESULT_MISS)   ] = 0x40205, /* PAGE_WALKS.I_SIDE_WALKS */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
 [ C(BPU ) ] = {
	[ C(OP_READ) ] = {
		[ C(RESULT_ACCESS) ] = 0x00c4, /* BR_INST_RETIRED.ANY */
		[ C(RESULT_MISS)   ] = 0x00c5, /* BP_INST_RETIRED.MISPRED */
	},
	[ C(OP_WRITE) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
	[ C(OP_PREFETCH) ] = {
		[ C(RESULT_ACCESS) ] = -1,
		[ C(RESULT_MISS)   ] = -1,
	},
 },
};

EVENT_ATTR_STR(topdown-total-slots, td_total_slots_glm, "event=0x3c");
EVENT_ATTR_STR(topdown-total-slots.scale, td_total_slots_scale_glm, "3");
/* UOPS_NOT_DELIVERED.ANY */
EVENT_ATTR_STR(topdown-fetch-bubbles, td_fetch_bubbles_glm, "event=0x9c");
/* ISSUE_SLOTS_NOT_CONSUMED.RECOVERY */
EVENT_ATTR_STR(topdown-recovery-bubbles, td_recovery_bubbles_glm, "event=0xca,umask=0x02");
/* UOPS_RETIRED.ANY */
EVENT_ATTR_STR(topdown-slots-retired, td_slots_retired_glm, "event=0xc2");
/* UOPS_ISSUED.ANY */
EVENT_ATTR_STR(topdown-slots-issued, td_slots_issued_glm, "event=0x0e");

static struct attribute *glm_events_attrs[] = {
	EVENT_PTR(td_total_slots_glm),
	EVENT_PTR(td_total_slots_scale_glm),
	EVENT_PTR(td_fetch_bubbles_glm),
	EVENT_PTR(td_recovery_bubbles_glm),
	EVENT_PTR(td_slots_issued_glm),
	EVENT_PTR(td_slots_retired_glm),
	NULL
};

static struct extra_reg intel_glm_extra_regs[] __read_mostly = {
	/* must define OFFCORE_RSP_X first, see intel_fixup_er() */
	INTEL_UEVENT_EXTRA_REG(0x01b7, MSR_OFFCORE_RSP_0, 0x760005ffbfull, RSP_0),
	INTEL_UEVENT_EXTRA_REG(0x02b7, MSR_OFFCORE_RSP_1, 0x360005ffbfull, RSP_1),
	EVENT_EXTRA_END
};

#define GLM_DEMAND_DATA_RD		BIT_ULL(0)
#define GLM_DEMAND_RFO			BIT_ULL(1)
#define GLM_ANY_RESPONSE		BIT_ULL(16)
#define GLM_SNP_NONE_OR_MISS		BIT_ULL(33)
#define GLM_DEMAND_READ			GLM_DEMAND_DATA_RD
#define GLM_DEMAND_WRITE		GLM_DEMAND_RFO
#define GLM_DEMAND_PREFETCH		(SNB_PF_DATA_RD|SNB_PF_RFO)
#define GLM_LLC_ACCESS			GLM_ANY_RESPONSE
#define GLM_SNP_ANY			(GLM_SNP_NONE_OR_MISS|SNB_NO_FWD|SNB_HITM)
#define GLM_LLC_MISS			(GLM_SNP_ANY|SNB_NON_DRAM)

static __initconst const u64 glm_hw_cache_event_ids
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	[C(L1D)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x81d0,	/* MEM_UOPS_RETIRED.ALL_LOADS */
			[C(RESULT_MISS)]	= 0x0,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= 0x82d0,	/* MEM_UOPS_RETIRED.ALL_STORES */
			[C(RESULT_MISS)]	= 0x0,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= 0x0,
			[C(RESULT_MISS)]	= 0x0,
		},
	},
	[C(L1I)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x0380,	/* ICACHE.ACCESSES */
			[C(RESULT_MISS)]	= 0x0280,	/* ICACHE.MISSES */
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= 0x0,
			[C(RESULT_MISS)]	= 0x0,
		},
	},
	[C(LL)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x1b7,	/* OFFCORE_RESPONSE */
			[C(RESULT_MISS)]	= 0x1b7,	/* OFFCORE_RESPONSE */
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= 0x1b7,	/* OFFCORE_RESPONSE */
			[C(RESULT_MISS)]	= 0x1b7,	/* OFFCORE_RESPONSE */
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= 0x1b7,	/* OFFCORE_RESPONSE */
			[C(RESULT_MISS)]	= 0x1b7,	/* OFFCORE_RESPONSE */
		},
	},
	[C(DTLB)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x81d0,	/* MEM_UOPS_RETIRED.ALL_LOADS */
			[C(RESULT_MISS)]	= 0x0,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= 0x82d0,	/* MEM_UOPS_RETIRED.ALL_STORES */
			[C(RESULT_MISS)]	= 0x0,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= 0x0,
			[C(RESULT_MISS)]	= 0x0,
		},
	},
	[C(ITLB)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x00c0,	/* INST_RETIRED.ANY_P */
			[C(RESULT_MISS)]	= 0x0481,	/* ITLB.MISS */
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
	},
	[C(BPU)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x00c4,	/* BR_INST_RETIRED.ALL_BRANCHES */
			[C(RESULT_MISS)]	= 0x00c5,	/* BR_MISP_RETIRED.ALL_BRANCHES */
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
	},
};

static __initconst const u64 glm_hw_cache_extra_regs
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	[C(LL)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= GLM_DEMAND_READ|
						  GLM_LLC_ACCESS,
			[C(RESULT_MISS)]	= GLM_DEMAND_READ|
						  GLM_LLC_MISS,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= GLM_DEMAND_WRITE|
						  GLM_LLC_ACCESS,
			[C(RESULT_MISS)]	= GLM_DEMAND_WRITE|
						  GLM_LLC_MISS,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= GLM_DEMAND_PREFETCH|
						  GLM_LLC_ACCESS,
			[C(RESULT_MISS)]	= GLM_DEMAND_PREFETCH|
						  GLM_LLC_MISS,
		},
	},
};

static __initconst const u64 glp_hw_cache_event_ids
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	[C(L1D)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x81d0,	/* MEM_UOPS_RETIRED.ALL_LOADS */
			[C(RESULT_MISS)]	= 0x0,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= 0x82d0,	/* MEM_UOPS_RETIRED.ALL_STORES */
			[C(RESULT_MISS)]	= 0x0,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= 0x0,
			[C(RESULT_MISS)]	= 0x0,
		},
	},
	[C(L1I)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x0380,	/* ICACHE.ACCESSES */
			[C(RESULT_MISS)]	= 0x0280,	/* ICACHE.MISSES */
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= 0x0,
			[C(RESULT_MISS)]	= 0x0,
		},
	},
	[C(LL)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x1b7,	/* OFFCORE_RESPONSE */
			[C(RESULT_MISS)]	= 0x1b7,	/* OFFCORE_RESPONSE */
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= 0x1b7,	/* OFFCORE_RESPONSE */
			[C(RESULT_MISS)]	= 0x1b7,	/* OFFCORE_RESPONSE */
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= 0x0,
			[C(RESULT_MISS)]	= 0x0,
		},
	},
	[C(DTLB)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x81d0,	/* MEM_UOPS_RETIRED.ALL_LOADS */
			[C(RESULT_MISS)]	= 0xe08,	/* DTLB_LOAD_MISSES.WALK_COMPLETED */
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= 0x82d0,	/* MEM_UOPS_RETIRED.ALL_STORES */
			[C(RESULT_MISS)]	= 0xe49,	/* DTLB_STORE_MISSES.WALK_COMPLETED */
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= 0x0,
			[C(RESULT_MISS)]	= 0x0,
		},
	},
	[C(ITLB)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x00c0,	/* INST_RETIRED.ANY_P */
			[C(RESULT_MISS)]	= 0x0481,	/* ITLB.MISS */
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
	},
	[C(BPU)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= 0x00c4,	/* BR_INST_RETIRED.ALL_BRANCHES */
			[C(RESULT_MISS)]	= 0x00c5,	/* BR_MISP_RETIRED.ALL_BRANCHES */
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
	},
};

static __initconst const u64 glp_hw_cache_extra_regs
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	[C(LL)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= GLM_DEMAND_READ|
						  GLM_LLC_ACCESS,
			[C(RESULT_MISS)]	= GLM_DEMAND_READ|
						  GLM_LLC_MISS,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= GLM_DEMAND_WRITE|
						  GLM_LLC_ACCESS,
			[C(RESULT_MISS)]	= GLM_DEMAND_WRITE|
						  GLM_LLC_MISS,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= 0x0,
			[C(RESULT_MISS)]	= 0x0,
		},
	},
};

#define KNL_OT_L2_HITE		BIT_ULL(19) /* Other Tile L2 Hit */
#define KNL_OT_L2_HITF		BIT_ULL(20) /* Other Tile L2 Hit */
#define KNL_MCDRAM_LOCAL	BIT_ULL(21)
#define KNL_MCDRAM_FAR		BIT_ULL(22)
#define KNL_DDR_LOCAL		BIT_ULL(23)
#define KNL_DDR_FAR		BIT_ULL(24)
#define KNL_DRAM_ANY		(KNL_MCDRAM_LOCAL | KNL_MCDRAM_FAR | \
				    KNL_DDR_LOCAL | KNL_DDR_FAR)
#define KNL_L2_READ		SLM_DMND_READ
#define KNL_L2_WRITE		SLM_DMND_WRITE
#define KNL_L2_PREFETCH		SLM_DMND_PREFETCH
#define KNL_L2_ACCESS		SLM_LLC_ACCESS
#define KNL_L2_MISS		(KNL_OT_L2_HITE | KNL_OT_L2_HITF | \
				   KNL_DRAM_ANY | SNB_SNP_ANY | \
						  SNB_NON_DRAM)

static __initconst const u64 knl_hw_cache_extra_regs
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX] = {
	[C(LL)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)] = KNL_L2_READ | KNL_L2_ACCESS,
			[C(RESULT_MISS)]   = 0,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)] = KNL_L2_WRITE | KNL_L2_ACCESS,
			[C(RESULT_MISS)]   = KNL_L2_WRITE | KNL_L2_MISS,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)] = KNL_L2_PREFETCH | KNL_L2_ACCESS,
			[C(RESULT_MISS)]   = KNL_L2_PREFETCH | KNL_L2_MISS,
		},
	},
};

/*
 * Used from PMIs where the LBRs are already disabled.
 *
 * This function could be called consecutively. It is required to remain in
 * disabled state if called consecutively.
 *
 * During consecutive calls, the same disable value will be written to related
 * registers, so the PMU state remains unchanged.
 *
 * intel_bts events don't coexist with intel PMU's BTS events because of
 * x86_add_exclusive(x86_lbr_exclusive_lbr); there's no need to keep them
 * disabled around intel PMU's event batching etc, only inside the PMI handler.
 */
static void __intel_pmu_disable_all(void)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);

	wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL, 0);

	if (test_bit(INTEL_PMC_IDX_FIXED_BTS, cpuc->active_mask))
		intel_pmu_disable_bts();

	intel_pmu_pebs_disable_all();
}

static void intel_pmu_disable_all(void)
{
	__intel_pmu_disable_all();
	intel_pmu_lbr_disable_all();
}

static void __intel_pmu_enable_all(int added, bool pmi)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);

	intel_pmu_pebs_enable_all();
	intel_pmu_lbr_enable_all(pmi);
	wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL,
			x86_pmu.intel_ctrl & ~cpuc->intel_ctrl_guest_mask);

	if (test_bit(INTEL_PMC_IDX_FIXED_BTS, cpuc->active_mask)) {
		struct perf_event *event =
			cpuc->events[INTEL_PMC_IDX_FIXED_BTS];

		if (WARN_ON_ONCE(!event))
			return;

		intel_pmu_enable_bts(event->hw.config);
	}
}

static void intel_pmu_enable_all(int added)
{
	__intel_pmu_enable_all(added, false);
}

/*
 * Workaround for:
 *   Intel Errata AAK100 (model 26)
 *   Intel Errata AAP53  (model 30)
 *   Intel Errata BD53   (model 44)
 *
 * The official story:
 *   These chips need to be 'reset' when adding counters by programming the
 *   magic three (non-counting) events 0x4300B5, 0x4300D2, and 0x4300B1 either
 *   in sequence on the same PMC or on different PMCs.
 *
 * In practise it appears some of these events do in fact count, and
 * we need to programm all 4 events.
 */
static void intel_pmu_nhm_workaround(void)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	static const unsigned long nhm_magic[4] = {
		0x4300B5,
		0x4300D2,
		0x4300B1,
		0x4300B1
	};
	struct perf_event *event;
	int i;

	/*
	 * The Errata requires below steps:
	 * 1) Clear MSR_IA32_PEBS_ENABLE and MSR_CORE_PERF_GLOBAL_CTRL;
	 * 2) Configure 4 PERFEVTSELx with the magic events and clear
	 *    the corresponding PMCx;
	 * 3) set bit0~bit3 of MSR_CORE_PERF_GLOBAL_CTRL;
	 * 4) Clear MSR_CORE_PERF_GLOBAL_CTRL;
	 * 5) Clear 4 pairs of ERFEVTSELx and PMCx;
	 */

	/*
	 * The real steps we choose are a little different from above.
	 * A) To reduce MSR operations, we don't run step 1) as they
	 *    are already cleared before this function is called;
	 * B) Call x86_perf_event_update to save PMCx before configuring
	 *    PERFEVTSELx with magic number;
	 * C) With step 5), we do clear only when the PERFEVTSELx is
	 *    not used currently.
	 * D) Call x86_perf_event_set_period to restore PMCx;
	 */

	/* We always operate 4 pairs of PERF Counters */
	for (i = 0; i < 4; i++) {
		event = cpuc->events[i];
		if (event)
			x86_perf_event_update(event);
	}

	for (i = 0; i < 4; i++) {
		wrmsrl(MSR_ARCH_PERFMON_EVENTSEL0 + i, nhm_magic[i]);
		wrmsrl(MSR_ARCH_PERFMON_PERFCTR0 + i, 0x0);
	}

	wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL, 0xf);
	wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL, 0x0);

	for (i = 0; i < 4; i++) {
		event = cpuc->events[i];

		if (event) {
			x86_perf_event_set_period(event);
			__x86_pmu_enable_event(&event->hw,
					ARCH_PERFMON_EVENTSEL_ENABLE);
		} else
			wrmsrl(MSR_ARCH_PERFMON_EVENTSEL0 + i, 0x0);
	}
}

static void intel_pmu_nhm_enable_all(int added)
{
	if (added)
		intel_pmu_nhm_workaround();
	intel_pmu_enable_all(added);
}

static void intel_set_tfa(struct cpu_hw_events *cpuc, bool on)
{
	u64 val = on ? MSR_TFA_RTM_FORCE_ABORT : 0;

	if (cpuc->tfa_shadow != val) {
		cpuc->tfa_shadow = val;
		wrmsrl(MSR_TSX_FORCE_ABORT, val);
	}
}

static void intel_tfa_commit_scheduling(struct cpu_hw_events *cpuc, int idx, int cntr)
{
	/*
	 * We're going to use PMC3, make sure TFA is set before we touch it.
	 */
	if (cntr == 3 && !cpuc->is_fake)
		intel_set_tfa(cpuc, true);
}

static void intel_tfa_pmu_enable_all(int added)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);

	/*
	 * If we find PMC3 is no longer used when we enable the PMU, we can
	 * clear TFA.
	 */
	if (!test_bit(3, cpuc->active_mask))
		intel_set_tfa(cpuc, false);

	intel_pmu_enable_all(added);
}

static inline u64 intel_pmu_get_status(void)
{
	u64 status;

	rdmsrl(MSR_CORE_PERF_GLOBAL_STATUS, status);

	return status;
}

static inline void intel_pmu_ack_status(u64 ack)
{
	wrmsrl(MSR_CORE_PERF_GLOBAL_OVF_CTRL, ack);
}

static void intel_pmu_disable_fixed(struct hw_perf_event *hwc)
{
	int idx = hwc->idx - INTEL_PMC_IDX_FIXED;
	u64 ctrl_val, mask;

	mask = 0xfULL << (idx * 4);

	rdmsrl(hwc->config_base, ctrl_val);
	ctrl_val &= ~mask;
	wrmsrl(hwc->config_base, ctrl_val);
}

static inline bool event_is_checkpointed(struct perf_event *event)
{
	return (event->hw.config & HSW_IN_TX_CHECKPOINTED) != 0;
}

static void intel_pmu_disable_event(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);

	if (unlikely(hwc->idx == INTEL_PMC_IDX_FIXED_BTS)) {
		intel_pmu_disable_bts();
		intel_pmu_drain_bts_buffer();
		return;
	}

	cpuc->intel_ctrl_guest_mask &= ~(1ull << hwc->idx);
	cpuc->intel_ctrl_host_mask &= ~(1ull << hwc->idx);
	cpuc->intel_cp_status &= ~(1ull << hwc->idx);

	if (unlikely(hwc->config_base == MSR_ARCH_PERFMON_FIXED_CTR_CTRL))
		intel_pmu_disable_fixed(hwc);
	else
		x86_pmu_disable_event(event);

	/*
	 * Needs to be called after x86_pmu_disable_event,
	 * so we don't trigger the event without PEBS bit set.
	 */
	if (unlikely(event->attr.precise_ip))
		intel_pmu_pebs_disable(event);
}

static void intel_pmu_del_event(struct perf_event *event)
{
	if (needs_branch_stack(event))
		intel_pmu_lbr_del(event);
	if (event->attr.precise_ip)
		intel_pmu_pebs_del(event);
}

static void intel_pmu_read_event(struct perf_event *event)
{
	if (event->hw.flags & PERF_X86_EVENT_AUTO_RELOAD)
		intel_pmu_auto_reload_read(event);
	else
		x86_perf_event_update(event);
}

static void intel_pmu_enable_fixed(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx - INTEL_PMC_IDX_FIXED;
	u64 ctrl_val, mask, bits = 0;

	/*
	 * Enable IRQ generation (0x8), if not PEBS,
	 * and enable ring-3 counting (0x2) and ring-0 counting (0x1)
	 * if requested:
	 */
	if (!event->attr.precise_ip)
		bits |= 0x8;
	if (hwc->config & ARCH_PERFMON_EVENTSEL_USR)
		bits |= 0x2;
	if (hwc->config & ARCH_PERFMON_EVENTSEL_OS)
		bits |= 0x1;

	/*
	 * ANY bit is supported in v3 and up
	 */
	if (x86_pmu.version > 2 && hwc->config & ARCH_PERFMON_EVENTSEL_ANY)
		bits |= 0x4;

	bits <<= (idx * 4);
	mask = 0xfULL << (idx * 4);

	rdmsrl(hwc->config_base, ctrl_val);
	ctrl_val &= ~mask;
	ctrl_val |= bits;
	wrmsrl(hwc->config_base, ctrl_val);
}

static void intel_pmu_enable_event(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);

	if (unlikely(hwc->idx == INTEL_PMC_IDX_FIXED_BTS)) {
		if (!__this_cpu_read(cpu_hw_events.enabled))
			return;

		intel_pmu_enable_bts(hwc->config);
		return;
	}

	if (event->attr.exclude_host)
		cpuc->intel_ctrl_guest_mask |= (1ull << hwc->idx);
	if (event->attr.exclude_guest)
		cpuc->intel_ctrl_host_mask |= (1ull << hwc->idx);

	if (unlikely(event_is_checkpointed(event)))
		cpuc->intel_cp_status |= (1ull << hwc->idx);

	if (unlikely(event->attr.precise_ip))
		intel_pmu_pebs_enable(event);

	if (unlikely(hwc->config_base == MSR_ARCH_PERFMON_FIXED_CTR_CTRL)) {
		intel_pmu_enable_fixed(event);
		return;
	}

	__x86_pmu_enable_event(hwc, ARCH_PERFMON_EVENTSEL_ENABLE);
}

static void intel_pmu_add_event(struct perf_event *event)
{
	if (event->attr.precise_ip)
		intel_pmu_pebs_add(event);
	if (needs_branch_stack(event))
		intel_pmu_lbr_add(event);
}

/*
 * Save and restart an expired event. Called by NMI contexts,
 * so it has to be careful about preempting normal event ops:
 */
int intel_pmu_save_and_restart(struct perf_event *event)
{
	x86_perf_event_update(event);
	/*
	 * For a checkpointed counter always reset back to 0.  This
	 * avoids a situation where the counter overflows, aborts the
	 * transaction and is then set back to shortly before the
	 * overflow, and overflows and aborts again.
	 */
	if (unlikely(event_is_checkpointed(event))) {
		/* No race with NMIs because the counter should not be armed */
		wrmsrl(event->hw.event_base, 0);
		local64_set(&event->hw.prev_count, 0);
	}
	return x86_perf_event_set_period(event);
}

static void intel_pmu_reset(void)
{
	struct debug_store *ds = __this_cpu_read(cpu_hw_events.ds);
	unsigned long flags;
	int idx;

	if (!x86_pmu.num_counters)
		return;

	local_irq_save(flags);

	pr_info("clearing PMU state on CPU#%d\n", smp_processor_id());

	for (idx = 0; idx < x86_pmu.num_counters; idx++) {
		wrmsrl_safe(x86_pmu_config_addr(idx), 0ull);
		wrmsrl_safe(x86_pmu_event_addr(idx),  0ull);
	}
	for (idx = 0; idx < x86_pmu.num_counters_fixed; idx++)
		wrmsrl_safe(MSR_ARCH_PERFMON_FIXED_CTR0 + idx, 0ull);

	if (ds)
		ds->bts_index = ds->bts_buffer_base;

	/* Ack all overflows and disable fixed counters */
	if (x86_pmu.version >= 2) {
		intel_pmu_ack_status(intel_pmu_get_status());
		wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL, 0);
	}

	/* Reset LBRs and LBR freezing */
	if (x86_pmu.lbr_nr) {
		update_debugctlmsr(get_debugctlmsr() &
			~(DEBUGCTLMSR_FREEZE_LBRS_ON_PMI|DEBUGCTLMSR_LBR));
	}

	local_irq_restore(flags);
}

/*
 * This handler is triggered by the local APIC, so the APIC IRQ handling
 * rules apply:
 */
static int intel_pmu_handle_irq(struct pt_regs *regs)
{
	struct perf_sample_data data;
	struct cpu_hw_events *cpuc;
	int bit, loops;
	u64 status;
	int handled;
	int pmu_enabled;

	cpuc = this_cpu_ptr(&cpu_hw_events);

	/*
	 * Save the PMU state.
	 * It needs to be restored when leaving the handler.
	 */
	pmu_enabled = cpuc->enabled;
	/*
	 * No known reason to not always do late ACK,
	 * but just in case do it opt-in.
	 */
	if (!x86_pmu.late_ack)
		apic_write(APIC_LVTPC, APIC_DM_NMI);
	intel_bts_disable_local();
	cpuc->enabled = 0;
	__intel_pmu_disable_all();
	handled = intel_pmu_drain_bts_buffer();
	handled += intel_bts_interrupt();
	status = intel_pmu_get_status();
	if (!status)
		goto done;

	loops = 0;
again:
	intel_pmu_lbr_read();
	intel_pmu_ack_status(status);
	if (++loops > 100) {
		static bool warned = false;
		if (!warned) {
			WARN(1, "perfevents: irq loop stuck!\n");
			perf_event_print_debug();
			warned = true;
		}
		intel_pmu_reset();
		goto done;
	}

	inc_irq_stat(apic_perf_irqs);


	/*
	 * Ignore a range of extra bits in status that do not indicate
	 * overflow by themselves.
	 */
	status &= ~(GLOBAL_STATUS_COND_CHG |
		    GLOBAL_STATUS_ASIF |
		    GLOBAL_STATUS_LBRS_FROZEN);
	if (!status)
		goto done;
	/*
	 * In case multiple PEBS events are sampled at the same time,
	 * it is possible to have GLOBAL_STATUS bit 62 set indicating
	 * PEBS buffer overflow and also seeing at most 3 PEBS counters
	 * having their bits set in the status register. This is a sign
	 * that there was at least one PEBS record pending at the time
	 * of the PMU interrupt. PEBS counters must only be processed
	 * via the drain_pebs() calls and not via the regular sample
	 * processing loop coming after that the function, otherwise
	 * phony regular samples may be generated in the sampling buffer
	 * not marked with the EXACT tag. Another possibility is to have
	 * one PEBS event and at least one non-PEBS event whic hoverflows
	 * while PEBS has armed. In this case, bit 62 of GLOBAL_STATUS will
	 * not be set, yet the overflow status bit for the PEBS counter will
	 * be on Skylake.
	 *
	 * To avoid this problem, we systematically ignore the PEBS-enabled
	 * counters from the GLOBAL_STATUS mask and we always process PEBS
	 * events via drain_pebs().
	 */
	if (x86_pmu.flags & PMU_FL_PEBS_ALL)
		status &= ~cpuc->pebs_enabled;
	else
		status &= ~(cpuc->pebs_enabled & PEBS_COUNTER_MASK);

	/*
	 * PEBS overflow sets bit 62 in the global status register
	 */
	if (__test_and_clear_bit(62, (unsigned long *)&status)) {
		handled++;
		x86_pmu.drain_pebs(regs);
		status &= x86_pmu.intel_ctrl | GLOBAL_STATUS_TRACE_TOPAPMI;
	}

	/*
	 * Intel PT
	 */
	if (__test_and_clear_bit(55, (unsigned long *)&status)) {
		handled++;
		intel_pt_interrupt();
	}

	/*
	 * Checkpointed counters can lead to 'spurious' PMIs because the
	 * rollback caused by the PMI will have cleared the overflow status
	 * bit. Therefore always force probe these counters.
	 */
	status |= cpuc->intel_cp_status;

	for_each_set_bit(bit, (unsigned long *)&status, X86_PMC_IDX_MAX) {
		struct perf_event *event = cpuc->events[bit];

		handled++;

		if (!test_bit(bit, cpuc->active_mask))
			continue;

		if (!intel_pmu_save_and_restart(event))
			continue;

		perf_sample_data_init(&data, 0, event->hw.last_period);

		if (has_branch_stack(event))
			data.br_stack = &cpuc->lbr_stack;

		if (perf_event_overflow(event, &data, regs))
			x86_pmu_stop(event, 0);
	}

	/*
	 * Repeat if there is more work to be done:
	 */
	status = intel_pmu_get_status();
	if (status)
		goto again;

done:
	/* Only restore PMU state when it's active. See x86_pmu_disable(). */
	cpuc->enabled = pmu_enabled;
	if (pmu_enabled)
		__intel_pmu_enable_all(0, true);
	intel_bts_enable_local();

	/*
	 * Only unmask the NMI after the overflow counters
	 * have been reset. This avoids spurious NMIs on
	 * Haswell CPUs.
	 */
	if (x86_pmu.late_ack)
		apic_write(APIC_LVTPC, APIC_DM_NMI);
	return handled;
}

static struct event_constraint *
intel_bts_constraints(struct perf_event *event)
{
	if (unlikely(intel_pmu_has_bts(event)))
		return &bts_constraint;

	return NULL;
}

static int intel_alt_er(int idx, u64 config)
{
	int alt_idx = idx;

	if (!(x86_pmu.flags & PMU_FL_HAS_RSP_1))
		return idx;

	if (idx == EXTRA_REG_RSP_0)
		alt_idx = EXTRA_REG_RSP_1;

	if (idx == EXTRA_REG_RSP_1)
		alt_idx = EXTRA_REG_RSP_0;

	if (config & ~x86_pmu.extra_regs[alt_idx].valid_mask)
		return idx;

	return alt_idx;
}

static void intel_fixup_er(struct perf_event *event, int idx)
{
	event->hw.extra_reg.idx = idx;

	if (idx == EXTRA_REG_RSP_0) {
		event->hw.config &= ~INTEL_ARCH_EVENT_MASK;
		event->hw.config |= x86_pmu.extra_regs[EXTRA_REG_RSP_0].event;
		event->hw.extra_reg.reg = MSR_OFFCORE_RSP_0;
	} else if (idx == EXTRA_REG_RSP_1) {
		event->hw.config &= ~INTEL_ARCH_EVENT_MASK;
		event->hw.config |= x86_pmu.extra_regs[EXTRA_REG_RSP_1].event;
		event->hw.extra_reg.reg = MSR_OFFCORE_RSP_1;
	}
}

/*
 * manage allocation of shared extra msr for certain events
 *
 * sharing can be:
 * per-cpu: to be shared between the various events on a single PMU
 * per-core: per-cpu + shared by HT threads
 */
static struct event_constraint *
__intel_shared_reg_get_constraints(struct cpu_hw_events *cpuc,
				   struct perf_event *event,
				   struct hw_perf_event_extra *reg)
{
	struct event_constraint *c = &emptyconstraint;
	struct er_account *era;
	unsigned long flags;
	int idx = reg->idx;

	/*
	 * reg->alloc can be set due to existing state, so for fake cpuc we
	 * need to ignore this, otherwise we might fail to allocate proper fake
	 * state for this extra reg constraint. Also see the comment below.
	 */
	if (reg->alloc && !cpuc->is_fake)
		return NULL; /* call x86_get_event_constraint() */

again:
	era = &cpuc->shared_regs->regs[idx];
	/*
	 * we use spin_lock_irqsave() to avoid lockdep issues when
	 * passing a fake cpuc
	 */
	raw_spin_lock_irqsave(&era->lock, flags);

	if (!atomic_read(&era->ref) || era->config == reg->config) {

		/*
		 * If its a fake cpuc -- as per validate_{group,event}() we
		 * shouldn't touch event state and we can avoid doing so
		 * since both will only call get_event_constraints() once
		 * on each event, this avoids the need for reg->alloc.
		 *
		 * Not doing the ER fixup will only result in era->reg being
		 * wrong, but since we won't actually try and program hardware
		 * this isn't a problem either.
		 */
		if (!cpuc->is_fake) {
			if (idx != reg->idx)
				intel_fixup_er(event, idx);

			/*
			 * x86_schedule_events() can call get_event_constraints()
			 * multiple times on events in the case of incremental
			 * scheduling(). reg->alloc ensures we only do the ER
			 * allocation once.
			 */
			reg->alloc = 1;
		}

		/* lock in msr value */
		era->config = reg->config;
		era->reg = reg->reg;

		/* one more user */
		atomic_inc(&era->ref);

		/*
		 * need to call x86_get_event_constraint()
		 * to check if associated event has constraints
		 */
		c = NULL;
	} else {
		idx = intel_alt_er(idx, reg->config);
		if (idx != reg->idx) {
			raw_spin_unlock_irqrestore(&era->lock, flags);
			goto again;
		}
	}
	raw_spin_unlock_irqrestore(&era->lock, flags);

	return c;
}

static void
__intel_shared_reg_put_constraints(struct cpu_hw_events *cpuc,
				   struct hw_perf_event_extra *reg)
{
	struct er_account *era;

	/*
	 * Only put constraint if extra reg was actually allocated. Also takes
	 * care of event which do not use an extra shared reg.
	 *
	 * Also, if this is a fake cpuc we shouldn't touch any event state
	 * (reg->alloc) and we don't care about leaving inconsistent cpuc state
	 * either since it'll be thrown out.
	 */
	if (!reg->alloc || cpuc->is_fake)
		return;

	era = &cpuc->shared_regs->regs[reg->idx];

	/* one fewer user */
	atomic_dec(&era->ref);

	/* allocate again next time */
	reg->alloc = 0;
}

static struct event_constraint *
intel_shared_regs_constraints(struct cpu_hw_events *cpuc,
			      struct perf_event *event)
{
	struct event_constraint *c = NULL, *d;
	struct hw_perf_event_extra *xreg, *breg;

	xreg = &event->hw.extra_reg;
	if (xreg->idx != EXTRA_REG_NONE) {
		c = __intel_shared_reg_get_constraints(cpuc, event, xreg);
		if (c == &emptyconstraint)
			return c;
	}
	breg = &event->hw.branch_reg;
	if (breg->idx != EXTRA_REG_NONE) {
		d = __intel_shared_reg_get_constraints(cpuc, event, breg);
		if (d == &emptyconstraint) {
			__intel_shared_reg_put_constraints(cpuc, xreg);
			c = d;
		}
	}
	return c;
}

struct event_constraint *
x86_get_event_constraints(struct cpu_hw_events *cpuc, int idx,
			  struct perf_event *event)
{
	struct event_constraint *c;

	if (x86_pmu.event_constraints) {
		for_each_event_constraint(c, x86_pmu.event_constraints) {
			if ((event->hw.config & c->cmask) == c->code) {
				event->hw.flags |= c->flags;
				return c;
			}
		}
	}

	return &unconstrained;
}

static struct event_constraint *
__intel_get_event_constraints(struct cpu_hw_events *cpuc, int idx,
			    struct perf_event *event)
{
	struct event_constraint *c;

	c = intel_bts_constraints(event);
	if (c)
		return c;

	c = intel_shared_regs_constraints(cpuc, event);
	if (c)
		return c;

	c = intel_pebs_constraints(event);
	if (c)
		return c;

	return x86_get_event_constraints(cpuc, idx, event);
}

static void
intel_start_scheduling(struct cpu_hw_events *cpuc)
{
	struct intel_excl_cntrs *excl_cntrs = cpuc->excl_cntrs;
	struct intel_excl_states *xl;
	int tid = cpuc->excl_thread_id;

	/*
	 * nothing needed if in group validation mode
	 */
	if (cpuc->is_fake || !is_ht_workaround_enabled())
		return;

	/*
	 * no exclusion needed
	 */
	if (WARN_ON_ONCE(!excl_cntrs))
		return;

	xl = &excl_cntrs->states[tid];

	xl->sched_started = true;
	/*
	 * lock shared state until we are done scheduling
	 * in stop_event_scheduling()
	 * makes scheduling appear as a transaction
	 */
	raw_spin_lock(&excl_cntrs->lock);
}

static void intel_commit_scheduling(struct cpu_hw_events *cpuc, int idx, int cntr)
{
	struct intel_excl_cntrs *excl_cntrs = cpuc->excl_cntrs;
	struct event_constraint *c = cpuc->event_constraint[idx];
	struct intel_excl_states *xl;
	int tid = cpuc->excl_thread_id;

	if (cpuc->is_fake || !is_ht_workaround_enabled())
		return;

	if (WARN_ON_ONCE(!excl_cntrs))
		return;

	if (!(c->flags & PERF_X86_EVENT_DYNAMIC))
		return;

	xl = &excl_cntrs->states[tid];

	lockdep_assert_held(&excl_cntrs->lock);

	if (c->flags & PERF_X86_EVENT_EXCL)
		xl->state[cntr] = INTEL_EXCL_EXCLUSIVE;
	else
		xl->state[cntr] = INTEL_EXCL_SHARED;
}

static void
intel_stop_scheduling(struct cpu_hw_events *cpuc)
{
	struct intel_excl_cntrs *excl_cntrs = cpuc->excl_cntrs;
	struct intel_excl_states *xl;
	int tid = cpuc->excl_thread_id;

	/*
	 * nothing needed if in group validation mode
	 */
	if (cpuc->is_fake || !is_ht_workaround_enabled())
		return;
	/*
	 * no exclusion needed
	 */
	if (WARN_ON_ONCE(!excl_cntrs))
		return;

	xl = &excl_cntrs->states[tid];

	xl->sched_started = false;
	/*
	 * release shared state lock (acquired in intel_start_scheduling())
	 */
	raw_spin_unlock(&excl_cntrs->lock);
}

static struct event_constraint *
dyn_constraint(struct cpu_hw_events *cpuc, struct event_constraint *c, int idx)
{
	WARN_ON_ONCE(!cpuc->constraint_list);

	if (!(c->flags & PERF_X86_EVENT_DYNAMIC)) {
		struct event_constraint *cx;

		/*
		 * grab pre-allocated constraint entry
		 */
		cx = &cpuc->constraint_list[idx];

		/*
		 * initialize dynamic constraint
		 * with static constraint
		 */
		*cx = *c;

		/*
		 * mark constraint as dynamic
		 */
		cx->flags |= PERF_X86_EVENT_DYNAMIC;
		c = cx;
	}

	return c;
}

static struct event_constraint *
intel_get_excl_constraints(struct cpu_hw_events *cpuc, struct perf_event *event,
			   int idx, struct event_constraint *c)
{
	struct intel_excl_cntrs *excl_cntrs = cpuc->excl_cntrs;
	struct intel_excl_states *xlo;
	int tid = cpuc->excl_thread_id;
	int is_excl, i;

	/*
	 * validating a group does not require
	 * enforcing cross-thread  exclusion
	 */
	if (cpuc->is_fake || !is_ht_workaround_enabled())
		return c;

	/*
	 * no exclusion needed
	 */
	if (WARN_ON_ONCE(!excl_cntrs))
		return c;

	/*
	 * because we modify the constraint, we need
	 * to make a copy. Static constraints come
	 * from static const tables.
	 *
	 * only needed when constraint has not yet
	 * been cloned (marked dynamic)
	 */
	c = dyn_constraint(cpuc, c, idx);

	/*
	 * From here on, the constraint is dynamic.
	 * Either it was just allocated above, or it
	 * was allocated during a earlier invocation
	 * of this function
	 */

	/*
	 * state of sibling HT
	 */
	xlo = &excl_cntrs->states[tid ^ 1];

	/*
	 * event requires exclusive counter access
	 * across HT threads
	 */
	is_excl = c->flags & PERF_X86_EVENT_EXCL;
	if (is_excl && !(event->hw.flags & PERF_X86_EVENT_EXCL_ACCT)) {
		event->hw.flags |= PERF_X86_EVENT_EXCL_ACCT;
		if (!cpuc->n_excl++)
			WRITE_ONCE(excl_cntrs->has_exclusive[tid], 1);
	}

	/*
	 * Modify static constraint with current dynamic
	 * state of thread
	 *
	 * EXCLUSIVE: sibling counter measuring exclusive event
	 * SHARED   : sibling counter measuring non-exclusive event
	 * UNUSED   : sibling counter unused
	 */
	for_each_set_bit(i, c->idxmsk, X86_PMC_IDX_MAX) {
		/*
		 * exclusive event in sibling counter
		 * our corresponding counter cannot be used
		 * regardless of our event
		 */
		if (xlo->state[i] == INTEL_EXCL_EXCLUSIVE)
			__clear_bit(i, c->idxmsk);
		/*
		 * if measuring an exclusive event, sibling
		 * measuring non-exclusive, then counter cannot
		 * be used
		 */
		if (is_excl && xlo->state[i] == INTEL_EXCL_SHARED)
			__clear_bit(i, c->idxmsk);
	}

	/*
	 * recompute actual bit weight for scheduling algorithm
	 */
	c->weight = hweight64(c->idxmsk64);

	/*
	 * if we return an empty mask, then switch
	 * back to static empty constraint to avoid
	 * the cost of freeing later on
	 */
	if (c->weight == 0)
		c = &emptyconstraint;

	return c;
}

static struct event_constraint *
intel_get_event_constraints(struct cpu_hw_events *cpuc, int idx,
			    struct perf_event *event)
{
	struct event_constraint *c1 = NULL;
	struct event_constraint *c2;

	if (idx >= 0) /* fake does < 0 */
		c1 = cpuc->event_constraint[idx];

	/*
	 * first time only
	 * - static constraint: no change across incremental scheduling calls
	 * - dynamic constraint: handled by intel_get_excl_constraints()
	 */
	c2 = __intel_get_event_constraints(cpuc, idx, event);
	if (c1 && (c1->flags & PERF_X86_EVENT_DYNAMIC)) {
		bitmap_copy(c1->idxmsk, c2->idxmsk, X86_PMC_IDX_MAX);
		c1->weight = c2->weight;
		c2 = c1;
	}

	if (cpuc->excl_cntrs)
		return intel_get_excl_constraints(cpuc, event, idx, c2);

	return c2;
}

static void intel_put_excl_constraints(struct cpu_hw_events *cpuc,
		struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct intel_excl_cntrs *excl_cntrs = cpuc->excl_cntrs;
	int tid = cpuc->excl_thread_id;
	struct intel_excl_states *xl;

	/*
	 * nothing needed if in group validation mode
	 */
	if (cpuc->is_fake)
		return;

	if (WARN_ON_ONCE(!excl_cntrs))
		return;

	if (hwc->flags & PERF_X86_EVENT_EXCL_ACCT) {
		hwc->flags &= ~PERF_X86_EVENT_EXCL_ACCT;
		if (!--cpuc->n_excl)
			WRITE_ONCE(excl_cntrs->has_exclusive[tid], 0);
	}

	/*
	 * If event was actually assigned, then mark the counter state as
	 * unused now.
	 */
	if (hwc->idx >= 0) {
		xl = &excl_cntrs->states[tid];

		/*
		 * put_constraint may be called from x86_schedule_events()
		 * which already has the lock held so here make locking
		 * conditional.
		 */
		if (!xl->sched_started)
			raw_spin_lock(&excl_cntrs->lock);

		xl->state[hwc->idx] = INTEL_EXCL_UNUSED;

		if (!xl->sched_started)
			raw_spin_unlock(&excl_cntrs->lock);
	}
}

static void
intel_put_shared_regs_event_constraints(struct cpu_hw_events *cpuc,
					struct perf_event *event)
{
	struct hw_perf_event_extra *reg;

	reg = &event->hw.extra_reg;
	if (reg->idx != EXTRA_REG_NONE)
		__intel_shared_reg_put_constraints(cpuc, reg);

	reg = &event->hw.branch_reg;
	if (reg->idx != EXTRA_REG_NONE)
		__intel_shared_reg_put_constraints(cpuc, reg);
}

static void intel_put_event_constraints(struct cpu_hw_events *cpuc,
					struct perf_event *event)
{
	intel_put_shared_regs_event_constraints(cpuc, event);

	/*
	 * is PMU has exclusive counter restrictions, then
	 * all events are subject to and must call the
	 * put_excl_constraints() routine
	 */
	if (cpuc->excl_cntrs)
		intel_put_excl_constraints(cpuc, event);
}

static void intel_pebs_aliases_core2(struct perf_event *event)
{
	if ((event->hw.config & X86_RAW_EVENT_MASK) == 0x003c) {
		/*
		 * Use an alternative encoding for CPU_CLK_UNHALTED.THREAD_P
		 * (0x003c) so that we can use it with PEBS.
		 *
		 * The regular CPU_CLK_UNHALTED.THREAD_P event (0x003c) isn't
		 * PEBS capable. However we can use INST_RETIRED.ANY_P
		 * (0x00c0), which is a PEBS capable event, to get the same
		 * count.
		 *
		 * INST_RETIRED.ANY_P counts the number of cycles that retires
		 * CNTMASK instructions. By setting CNTMASK to a value (16)
		 * larger than the maximum number of instructions that can be
		 * retired per cycle (4) and then inverting the condition, we
		 * count all cycles that retire 16 or less instructions, which
		 * is every cycle.
		 *
		 * Thereby we gain a PEBS capable cycle counter.
		 */
		u64 alt_config = X86_CONFIG(.event=0xc0, .inv=1, .cmask=16);

		alt_config |= (event->hw.config & ~X86_RAW_EVENT_MASK);
		event->hw.config = alt_config;
	}
}

static void intel_pebs_aliases_snb(struct perf_event *event)
{
	if ((event->hw.config & X86_RAW_EVENT_MASK) == 0x003c) {
		/*
		 * Use an alternative encoding for CPU_CLK_UNHALTED.THREAD_P
		 * (0x003c) so that we can use it with PEBS.
		 *
		 * The regular CPU_CLK_UNHALTED.THREAD_P event (0x003c) isn't
		 * PEBS capable. However we can use UOPS_RETIRED.ALL
		 * (0x01c2), which is a PEBS capable event, to get the same
		 * count.
		 *
		 * UOPS_RETIRED.ALL counts the number of cycles that retires
		 * CNTMASK micro-ops. By setting CNTMASK to a value (16)
		 * larger than the maximum number of micro-ops that can be
		 * retired per cycle (4) and then inverting the condition, we
		 * count all cycles that retire 16 or less micro-ops, which
		 * is every cycle.
		 *
		 * Thereby we gain a PEBS capable cycle counter.
		 */
		u64 alt_config = X86_CONFIG(.event=0xc2, .umask=0x01, .inv=1, .cmask=16);

		alt_config |= (event->hw.config & ~X86_RAW_EVENT_MASK);
		event->hw.config = alt_config;
	}
}

static void intel_pebs_aliases_precdist(struct perf_event *event)
{
	if ((event->hw.config & X86_RAW_EVENT_MASK) == 0x003c) {
		/*
		 * Use an alternative encoding for CPU_CLK_UNHALTED.THREAD_P
		 * (0x003c) so that we can use it with PEBS.
		 *
		 * The regular CPU_CLK_UNHALTED.THREAD_P event (0x003c) isn't
		 * PEBS capable. However we can use INST_RETIRED.PREC_DIST
		 * (0x01c0), which is a PEBS capable event, to get the same
		 * count.
		 *
		 * The PREC_DIST event has special support to minimize sample
		 * shadowing effects. One drawback is that it can be
		 * only programmed on counter 1, but that seems like an
		 * acceptable trade off.
		 */
		u64 alt_config = X86_CONFIG(.event=0xc0, .umask=0x01, .inv=1, .cmask=16);

		alt_config |= (event->hw.config & ~X86_RAW_EVENT_MASK);
		event->hw.config = alt_config;
	}
}

static void intel_pebs_aliases_ivb(struct perf_event *event)
{
	if (event->attr.precise_ip < 3)
		return intel_pebs_aliases_snb(event);
	return intel_pebs_aliases_precdist(event);
}

static void intel_pebs_aliases_skl(struct perf_event *event)
{
	if (event->attr.precise_ip < 3)
		return intel_pebs_aliases_core2(event);
	return intel_pebs_aliases_precdist(event);
}

static unsigned long intel_pmu_large_pebs_flags(struct perf_event *event)
{
	unsigned long flags = x86_pmu.large_pebs_flags;

	if (event->attr.use_clockid)
		flags &= ~PERF_SAMPLE_TIME;
	if (!event->attr.exclude_kernel)
		flags &= ~PERF_SAMPLE_REGS_USER;
	if (event->attr.sample_regs_user & ~PEBS_GP_REGS)
		flags &= ~(PERF_SAMPLE_REGS_USER | PERF_SAMPLE_REGS_INTR);
	return flags;
}

static int intel_pmu_bts_config(struct perf_event *event)
{
	struct perf_event_attr *attr = &event->attr;

	if (unlikely(intel_pmu_has_bts(event))) {
		/* BTS is not supported by this architecture. */
		if (!x86_pmu.bts_active)
			return -EOPNOTSUPP;

		/* BTS is currently only allowed for user-mode. */
		if (!attr->exclude_kernel)
			return -EOPNOTSUPP;

		/* BTS is not allowed for precise events. */
		if (attr->precise_ip)
			return -EOPNOTSUPP;

		/* disallow bts if conflicting events are present */
		if (x86_add_exclusive(x86_lbr_exclusive_lbr))
			return -EBUSY;

		event->destroy = hw_perf_lbr_event_destroy;
	}

	return 0;
}

static int core_pmu_hw_config(struct perf_event *event)
{
	int ret = x86_pmu_hw_config(event);

	if (ret)
		return ret;

	return intel_pmu_bts_config(event);
}

static int intel_pmu_hw_config(struct perf_event *event)
{
	int ret = x86_pmu_hw_config(event);

	if (ret)
		return ret;

	ret = intel_pmu_bts_config(event);
	if (ret)
		return ret;

	if (event->attr.precise_ip) {
		if (!(event->attr.freq || (event->attr.wakeup_events && !event->attr.watermark))) {
			event->hw.flags |= PERF_X86_EVENT_AUTO_RELOAD;
			if (!(event->attr.sample_type &
			      ~intel_pmu_large_pebs_flags(event)))
				event->hw.flags |= PERF_X86_EVENT_LARGE_PEBS;
		}
		if (x86_pmu.pebs_aliases)
			x86_pmu.pebs_aliases(event);

		if (event->attr.sample_type & PERF_SAMPLE_CALLCHAIN)
			event->attr.sample_type |= __PERF_SAMPLE_CALLCHAIN_EARLY;
	}

	if (needs_branch_stack(event)) {
		ret = intel_pmu_setup_lbr_filter(event);
		if (ret)
			return ret;

		/*
		 * BTS is set up earlier in this path, so don't account twice
		 */
		if (!unlikely(intel_pmu_has_bts(event))) {
			/* disallow lbr if conflicting events are present */
			if (x86_add_exclusive(x86_lbr_exclusive_lbr))
				return -EBUSY;

			event->destroy = hw_perf_lbr_event_destroy;
		}
	}

	if (event->attr.type != PERF_TYPE_RAW)
		return 0;

	if (!(event->attr.config & ARCH_PERFMON_EVENTSEL_ANY))
		return 0;

	if (x86_pmu.version < 3)
		return -EINVAL;

	ret = perf_allow_cpu(&event->attr);
	if (ret)
		return ret;

	event->hw.config |= ARCH_PERFMON_EVENTSEL_ANY;

	return 0;
}

struct perf_guest_switch_msr *perf_guest_get_msrs(int *nr)
{
	if (x86_pmu.guest_get_msrs)
		return x86_pmu.guest_get_msrs(nr);
	*nr = 0;
	return NULL;
}
EXPORT_SYMBOL_GPL(perf_guest_get_msrs);

static struct perf_guest_switch_msr *intel_guest_get_msrs(int *nr)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	struct perf_guest_switch_msr *arr = cpuc->guest_switch_msrs;

	arr[0].msr = MSR_CORE_PERF_GLOBAL_CTRL;
	arr[0].host = x86_pmu.intel_ctrl & ~cpuc->intel_ctrl_guest_mask;
	arr[0].guest = x86_pmu.intel_ctrl & ~cpuc->intel_ctrl_host_mask;
	/*
	 * If PMU counter has PEBS enabled it is not enough to disable counter
	 * on a guest entry since PEBS memory write can overshoot guest entry
	 * and corrupt guest memory. Disabling PEBS solves the problem.
	 */
	arr[1].msr = MSR_IA32_PEBS_ENABLE;
	arr[1].host = cpuc->pebs_enabled;
	arr[1].guest = 0;

	*nr = 2;
	return arr;
}

static struct perf_guest_switch_msr *core_guest_get_msrs(int *nr)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	struct perf_guest_switch_msr *arr = cpuc->guest_switch_msrs;
	int idx;

	for (idx = 0; idx < x86_pmu.num_counters; idx++)  {
		struct perf_event *event = cpuc->events[idx];

		arr[idx].msr = x86_pmu_config_addr(idx);
		arr[idx].host = arr[idx].guest = 0;

		if (!test_bit(idx, cpuc->active_mask))
			continue;

		arr[idx].host = arr[idx].guest =
			event->hw.config | ARCH_PERFMON_EVENTSEL_ENABLE;

		if (event->attr.exclude_host)
			arr[idx].host &= ~ARCH_PERFMON_EVENTSEL_ENABLE;
		else if (event->attr.exclude_guest)
			arr[idx].guest &= ~ARCH_PERFMON_EVENTSEL_ENABLE;
	}

	*nr = x86_pmu.num_counters;
	return arr;
}

static void core_pmu_enable_event(struct perf_event *event)
{
	if (!event->attr.exclude_host)
		x86_pmu_enable_event(event);
}

static void core_pmu_enable_all(int added)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	int idx;

	for (idx = 0; idx < x86_pmu.num_counters; idx++) {
		struct hw_perf_event *hwc = &cpuc->events[idx]->hw;

		if (!test_bit(idx, cpuc->active_mask) ||
				cpuc->events[idx]->attr.exclude_host)
			continue;

		__x86_pmu_enable_event(hwc, ARCH_PERFMON_EVENTSEL_ENABLE);
	}
}

static int hsw_hw_config(struct perf_event *event)
{
	int ret = intel_pmu_hw_config(event);

	if (ret)
		return ret;
	if (!boot_cpu_has(X86_FEATURE_RTM) && !boot_cpu_has(X86_FEATURE_HLE))
		return 0;
	event->hw.config |= event->attr.config & (HSW_IN_TX|HSW_IN_TX_CHECKPOINTED);

	/*
	 * IN_TX/IN_TX-CP filters are not supported by the Haswell PMU with
	 * PEBS or in ANY thread mode. Since the results are non-sensical forbid
	 * this combination.
	 */
	if ((event->hw.config & (HSW_IN_TX|HSW_IN_TX_CHECKPOINTED)) &&
	     ((event->hw.config & ARCH_PERFMON_EVENTSEL_ANY) ||
	      event->attr.precise_ip > 0))
		return -EOPNOTSUPP;

	if (event_is_checkpointed(event)) {
		/*
		 * Sampling of checkpointed events can cause situations where
		 * the CPU constantly aborts because of a overflow, which is
		 * then checkpointed back and ignored. Forbid checkpointing
		 * for sampling.
		 *
		 * But still allow a long sampling period, so that perf stat
		 * from KVM works.
		 */
		if (event->attr.sample_period > 0 &&
		    event->attr.sample_period < 0x7fffffff)
			return -EOPNOTSUPP;
	}
	return 0;
}

static struct event_constraint counter0_constraint =
			INTEL_ALL_EVENT_CONSTRAINT(0, 0x1);

static struct event_constraint counter2_constraint =
			EVENT_CONSTRAINT(0, 0x4, 0);

static struct event_constraint *
hsw_get_event_constraints(struct cpu_hw_events *cpuc, int idx,
			  struct perf_event *event)
{
	struct event_constraint *c;

	c = intel_get_event_constraints(cpuc, idx, event);

	/* Handle special quirk on in_tx_checkpointed only in counter 2 */
	if (event->hw.config & HSW_IN_TX_CHECKPOINTED) {
		if (c->idxmsk64 & (1U << 2))
			return &counter2_constraint;
		return &emptyconstraint;
	}

	return c;
}

static struct event_constraint *
glp_get_event_constraints(struct cpu_hw_events *cpuc, int idx,
			  struct perf_event *event)
{
	struct event_constraint *c;

	/* :ppp means to do reduced skid PEBS which is PMC0 only. */
	if (event->attr.precise_ip == 3)
		return &counter0_constraint;

	c = intel_get_event_constraints(cpuc, idx, event);

	return c;
}

static bool allow_tsx_force_abort = true;

static struct event_constraint *
tfa_get_event_constraints(struct cpu_hw_events *cpuc, int idx,
			  struct perf_event *event)
{
	struct event_constraint *c = hsw_get_event_constraints(cpuc, idx, event);

	/*
	 * Without TFA we must not use PMC3.
	 */
	if (!allow_tsx_force_abort && test_bit(3, c->idxmsk) && idx >= 0) {
		c = dyn_constraint(cpuc, c, idx);
		c->idxmsk64 &= ~(1ULL << 3);
		c->weight--;
	}

	return c;
}

/*
 * Broadwell:
 *
 * The INST_RETIRED.ALL period always needs to have lowest 6 bits cleared
 * (BDM55) and it must not use a period smaller than 100 (BDM11). We combine
 * the two to enforce a minimum period of 128 (the smallest value that has bits
 * 0-5 cleared and >= 100).
 *
 * Because of how the code in x86_perf_event_set_period() works, the truncation
 * of the lower 6 bits is 'harmless' as we'll occasionally add a longer period
 * to make up for the 'lost' events due to carrying the 'error' in period_left.
 *
 * Therefore the effective (average) period matches the requested period,
 * despite coarser hardware granularity.
 */
static u64 bdw_limit_period(struct perf_event *event, u64 left)
{
	if ((event->hw.config & INTEL_ARCH_EVENT_MASK) ==
			X86_CONFIG(.event=0xc0, .umask=0x01)) {
		if (left < 128)
			left = 128;
		left &= ~0x3fULL;
	}
	return left;
}

static u64 nhm_limit_period(struct perf_event *event, u64 left)
{
	return max(left, 32ULL);
}

PMU_FORMAT_ATTR(event,	"config:0-7"	);
PMU_FORMAT_ATTR(umask,	"config:8-15"	);
PMU_FORMAT_ATTR(edge,	"config:18"	);
PMU_FORMAT_ATTR(pc,	"config:19"	);
PMU_FORMAT_ATTR(any,	"config:21"	); /* v3 + */
PMU_FORMAT_ATTR(inv,	"config:23"	);
PMU_FORMAT_ATTR(cmask,	"config:24-31"	);
PMU_FORMAT_ATTR(in_tx,  "config:32");
PMU_FORMAT_ATTR(in_tx_cp, "config:33");

static struct attribute *intel_arch_formats_attr[] = {
	&format_attr_event.attr,
	&format_attr_umask.attr,
	&format_attr_edge.attr,
	&format_attr_pc.attr,
	&format_attr_inv.attr,
	&format_attr_cmask.attr,
	NULL,
};

ssize_t intel_event_sysfs_show(char *page, u64 config)
{
	u64 event = (config & ARCH_PERFMON_EVENTSEL_EVENT);

	return x86_event_sysfs_show(page, config, event);
}

static struct intel_shared_regs *allocate_shared_regs(int cpu)
{
	struct intel_shared_regs *regs;
	int i;

	regs = kzalloc_node(sizeof(struct intel_shared_regs),
			    GFP_KERNEL, cpu_to_node(cpu));
	if (regs) {
		/*
		 * initialize the locks to keep lockdep happy
		 */
		for (i = 0; i < EXTRA_REG_MAX; i++)
			raw_spin_lock_init(&regs->regs[i].lock);

		regs->core_id = -1;
	}
	return regs;
}

static struct intel_excl_cntrs *allocate_excl_cntrs(int cpu)
{
	struct intel_excl_cntrs *c;

	c = kzalloc_node(sizeof(struct intel_excl_cntrs),
			 GFP_KERNEL, cpu_to_node(cpu));
	if (c) {
		raw_spin_lock_init(&c->lock);
		c->core_id = -1;
	}
	return c;
}


int intel_cpuc_prepare(struct cpu_hw_events *cpuc, int cpu)
{
	if (x86_pmu.extra_regs || x86_pmu.lbr_sel_map) {
		cpuc->shared_regs = allocate_shared_regs(cpu);
		if (!cpuc->shared_regs)
			goto err;
	}

	if (x86_pmu.flags & (PMU_FL_EXCL_CNTRS | PMU_FL_TFA)) {
		size_t sz = X86_PMC_IDX_MAX * sizeof(struct event_constraint);

		cpuc->constraint_list = kzalloc_node(sz, GFP_KERNEL, cpu_to_node(cpu));
		if (!cpuc->constraint_list)
			goto err_shared_regs;
	}

	if (x86_pmu.flags & PMU_FL_EXCL_CNTRS) {
		cpuc->excl_cntrs = allocate_excl_cntrs(cpu);
		if (!cpuc->excl_cntrs)
			goto err_constraint_list;

		cpuc->excl_thread_id = 0;
	}

	return 0;

err_constraint_list:
	kfree(cpuc->constraint_list);
	cpuc->constraint_list = NULL;

err_shared_regs:
	kfree(cpuc->shared_regs);
	cpuc->shared_regs = NULL;

err:
	return -ENOMEM;
}

static int intel_pmu_cpu_prepare(int cpu)
{
	return intel_cpuc_prepare(&per_cpu(cpu_hw_events, cpu), cpu);
}

static void flip_smm_bit(void *data)
{
	unsigned long set = *(unsigned long *)data;

	if (set > 0) {
		msr_set_bit(MSR_IA32_DEBUGCTLMSR,
			    DEBUGCTLMSR_FREEZE_IN_SMM_BIT);
	} else {
		msr_clear_bit(MSR_IA32_DEBUGCTLMSR,
			      DEBUGCTLMSR_FREEZE_IN_SMM_BIT);
	}
}

static void intel_pmu_cpu_starting(int cpu)
{
	struct cpu_hw_events *cpuc = &per_cpu(cpu_hw_events, cpu);
	int core_id = topology_core_id(cpu);
	int i;

	init_debug_store_on_cpu(cpu);
	/*
	 * Deal with CPUs that don't clear their LBRs on power-up.
	 */
	intel_pmu_lbr_reset();

	cpuc->lbr_sel = NULL;

	if (x86_pmu.flags & PMU_FL_TFA) {
		WARN_ON_ONCE(cpuc->tfa_shadow);
		cpuc->tfa_shadow = ~0ULL;
		intel_set_tfa(cpuc, false);
	}

	if (x86_pmu.version > 1)
		flip_smm_bit(&x86_pmu.attr_freeze_on_smi);

	if (!cpuc->shared_regs)
		return;

	if (!(x86_pmu.flags & PMU_FL_NO_HT_SHARING)) {
		for_each_cpu(i, topology_sibling_cpumask(cpu)) {
			struct intel_shared_regs *pc;

			pc = per_cpu(cpu_hw_events, i).shared_regs;
			if (pc && pc->core_id == core_id) {
				cpuc->kfree_on_online[0] = cpuc->shared_regs;
				cpuc->shared_regs = pc;
				break;
			}
		}
		cpuc->shared_regs->core_id = core_id;
		cpuc->shared_regs->refcnt++;
	}

	if (x86_pmu.lbr_sel_map)
		cpuc->lbr_sel = &cpuc->shared_regs->regs[EXTRA_REG_LBR];

	if (x86_pmu.flags & PMU_FL_EXCL_CNTRS) {
		for_each_cpu(i, topology_sibling_cpumask(cpu)) {
			struct cpu_hw_events *sibling;
			struct intel_excl_cntrs *c;

			sibling = &per_cpu(cpu_hw_events, i);
			c = sibling->excl_cntrs;
			if (c && c->core_id == core_id) {
				cpuc->kfree_on_online[1] = cpuc->excl_cntrs;
				cpuc->excl_cntrs = c;
				if (!sibling->excl_thread_id)
					cpuc->excl_thread_id = 1;
				break;
			}
		}
		cpuc->excl_cntrs->core_id = core_id;
		cpuc->excl_cntrs->refcnt++;
	}
}

static void free_excl_cntrs(struct cpu_hw_events *cpuc)
{
	struct intel_excl_cntrs *c;

	c = cpuc->excl_cntrs;
	if (c) {
		if (c->core_id == -1 || --c->refcnt == 0)
			kfree(c);
		cpuc->excl_cntrs = NULL;
	}

	kfree(cpuc->constraint_list);
	cpuc->constraint_list = NULL;
}

static void intel_pmu_cpu_dying(int cpu)
{
	fini_debug_store_on_cpu(cpu);
}

void intel_cpuc_finish(struct cpu_hw_events *cpuc)
{
	struct intel_shared_regs *pc;

	pc = cpuc->shared_regs;
	if (pc) {
		if (pc->core_id == -1 || --pc->refcnt == 0)
			kfree(pc);
		cpuc->shared_regs = NULL;
	}

	free_excl_cntrs(cpuc);
}

static void intel_pmu_cpu_dead(int cpu)
{
	intel_cpuc_finish(&per_cpu(cpu_hw_events, cpu));
}

static void intel_pmu_sched_task(struct perf_event_context *ctx,
				 bool sched_in)
{
	intel_pmu_pebs_sched_task(ctx, sched_in);
	intel_pmu_lbr_sched_task(ctx, sched_in);
}

static int intel_pmu_check_period(struct perf_event *event, u64 value)
{
	return intel_pmu_has_bts_period(event, value) ? -EINVAL : 0;
}

PMU_FORMAT_ATTR(offcore_rsp, "config1:0-63");

PMU_FORMAT_ATTR(ldlat, "config1:0-15");

PMU_FORMAT_ATTR(frontend, "config1:0-23");

static struct attribute *intel_arch3_formats_attr[] = {
	&format_attr_event.attr,
	&format_attr_umask.attr,
	&format_attr_edge.attr,
	&format_attr_pc.attr,
	&format_attr_any.attr,
	&format_attr_inv.attr,
	&format_attr_cmask.attr,
	NULL,
};

static struct attribute *hsw_format_attr[] = {
	&format_attr_in_tx.attr,
	&format_attr_in_tx_cp.attr,
	&format_attr_offcore_rsp.attr,
	&format_attr_ldlat.attr,
	NULL
};

static struct attribute *nhm_format_attr[] = {
	&format_attr_offcore_rsp.attr,
	&format_attr_ldlat.attr,
	NULL
};

static struct attribute *slm_format_attr[] = {
	&format_attr_offcore_rsp.attr,
	NULL
};

static struct attribute *skl_format_attr[] = {
	&format_attr_frontend.attr,
	NULL,
};

static __initconst const struct x86_pmu core_pmu = {
	.name			= "core",
	.handle_irq		= x86_pmu_handle_irq,
	.disable_all		= x86_pmu_disable_all,
	.enable_all		= core_pmu_enable_all,
	.enable			= core_pmu_enable_event,
	.disable		= x86_pmu_disable_event,
	.hw_config		= core_pmu_hw_config,
	.schedule_events	= x86_schedule_events,
	.eventsel		= MSR_ARCH_PERFMON_EVENTSEL0,
	.perfctr		= MSR_ARCH_PERFMON_PERFCTR0,
	.event_map		= intel_pmu_event_map,
	.max_events		= ARRAY_SIZE(intel_perfmon_event_map),
	.apic			= 1,
	.large_pebs_flags	= LARGE_PEBS_FLAGS,

	/*
	 * Intel PMCs cannot be accessed sanely above 32-bit width,
	 * so we install an artificial 1<<31 period regardless of
	 * the generic event period:
	 */
	.max_period		= (1ULL<<31) - 1,
	.get_event_constraints	= intel_get_event_constraints,
	.put_event_constraints	= intel_put_event_constraints,
	.event_constraints	= intel_core_event_constraints,
	.guest_get_msrs		= core_guest_get_msrs,
	.format_attrs		= intel_arch_formats_attr,
	.events_sysfs_show	= intel_event_sysfs_show,

	/*
	 * Virtual (or funny metal) CPU can define x86_pmu.extra_regs
	 * together with PMU version 1 and thus be using core_pmu with
	 * shared_regs. We need following callbacks here to allocate
	 * it properly.
	 */
	.cpu_prepare		= intel_pmu_cpu_prepare,
	.cpu_starting		= intel_pmu_cpu_starting,
	.cpu_dying		= intel_pmu_cpu_dying,
	.cpu_dead		= intel_pmu_cpu_dead,

	.check_period		= intel_pmu_check_period,
};

static struct attribute *intel_pmu_attrs[];

static __initconst const struct x86_pmu intel_pmu = {
	.name			= "Intel",
	.handle_irq		= intel_pmu_handle_irq,
	.disable_all		= intel_pmu_disable_all,
	.enable_all		= intel_pmu_enable_all,
	.enable			= intel_pmu_enable_event,
	.disable		= intel_pmu_disable_event,
	.add			= intel_pmu_add_event,
	.del			= intel_pmu_del_event,
	.read			= intel_pmu_read_event,
	.hw_config		= intel_pmu_hw_config,
	.schedule_events	= x86_schedule_events,
	.eventsel		= MSR_ARCH_PERFMON_EVENTSEL0,
	.perfctr		= MSR_ARCH_PERFMON_PERFCTR0,
	.event_map		= intel_pmu_event_map,
	.max_events		= ARRAY_SIZE(intel_perfmon_event_map),
	.apic			= 1,
	.large_pebs_flags	= LARGE_PEBS_FLAGS,
	/*
	 * Intel PMCs cannot be accessed sanely above 32 bit width,
	 * so we install an artificial 1<<31 period regardless of
	 * the generic event period:
	 */
	.max_period		= (1ULL << 31) - 1,
	.get_event_constraints	= intel_get_event_constraints,
	.put_event_constraints	= intel_put_event_constraints,
	.pebs_aliases		= intel_pebs_aliases_core2,

	.format_attrs		= intel_arch3_formats_attr,
	.events_sysfs_show	= intel_event_sysfs_show,

	.attrs			= intel_pmu_attrs,

	.cpu_prepare		= intel_pmu_cpu_prepare,
	.cpu_starting		= intel_pmu_cpu_starting,
	.cpu_dying		= intel_pmu_cpu_dying,
	.cpu_dead		= intel_pmu_cpu_dead,

	.guest_get_msrs		= intel_guest_get_msrs,
	.sched_task		= intel_pmu_sched_task,

	.check_period		= intel_pmu_check_period,
};

static __init void intel_clovertown_quirk(void)
{
	/*
	 * PEBS is unreliable due to:
	 *
	 *   AJ67  - PEBS may experience CPL leaks
	 *   AJ68  - PEBS PMI may be delayed by one event
	 *   AJ69  - GLOBAL_STATUS[62] will only be set when DEBUGCTL[12]
	 *   AJ106 - FREEZE_LBRS_ON_PMI doesn't work in combination with PEBS
	 *
	 * AJ67 could be worked around by restricting the OS/USR flags.
	 * AJ69 could be worked around by setting PMU_FREEZE_ON_PMI.
	 *
	 * AJ106 could possibly be worked around by not allowing LBR
	 *       usage from PEBS, including the fixup.
	 * AJ68  could possibly be worked around by always programming
	 *	 a pebs_event_reset[0] value and coping with the lost events.
	 *
	 * But taken together it might just make sense to not enable PEBS on
	 * these chips.
	 */
	pr_warn("PEBS disabled due to CPU errata\n");
	x86_pmu.pebs = 0;
	x86_pmu.pebs_constraints = NULL;
}

static int intel_snb_pebs_broken(int cpu)
{
	u32 rev = UINT_MAX; /* default to broken for unknown models */

	switch (cpu_data(cpu).x86_model) {
	case INTEL_FAM6_SANDYBRIDGE:
		rev = 0x28;
		break;

	case INTEL_FAM6_SANDYBRIDGE_X:
		switch (cpu_data(cpu).x86_stepping) {
		case 6: rev = 0x618; break;
		case 7: rev = 0x70c; break;
		}
	}

	return (cpu_data(cpu).microcode < rev);
}

static void intel_snb_check_microcode(void)
{
	int pebs_broken = 0;
	int cpu;

	for_each_online_cpu(cpu) {
		if ((pebs_broken = intel_snb_pebs_broken(cpu)))
			break;
	}

	if (pebs_broken == x86_pmu.pebs_broken)
		return;

	/*
	 * Serialized by the microcode lock..
	 */
	if (x86_pmu.pebs_broken) {
		pr_info("PEBS enabled due to microcode update\n");
		x86_pmu.pebs_broken = 0;
	} else {
		pr_info("PEBS disabled due to CPU errata, please upgrade microcode\n");
		x86_pmu.pebs_broken = 1;
	}
}

static bool is_lbr_from(unsigned long msr)
{
	unsigned long lbr_from_nr = x86_pmu.lbr_from + x86_pmu.lbr_nr;

	return x86_pmu.lbr_from <= msr && msr < lbr_from_nr;
}

/*
 * Under certain circumstances, access certain MSR may cause #GP.
 * The function tests if the input MSR can be safely accessed.
 */
static bool check_msr(unsigned long msr, u64 mask)
{
	u64 val_old, val_new, val_tmp;

	/*
	 * Read the current value, change it and read it back to see if it
	 * matches, this is needed to detect certain hardware emulators
	 * (qemu/kvm) that don't trap on the MSR access and always return 0s.
	 */
	if (rdmsrl_safe(msr, &val_old))
		return false;

	/*
	 * Only change the bits which can be updated by wrmsrl.
	 */
	val_tmp = val_old ^ mask;

	if (is_lbr_from(msr))
		val_tmp = lbr_from_signext_quirk_wr(val_tmp);

	if (wrmsrl_safe(msr, val_tmp) ||
	    rdmsrl_safe(msr, &val_new))
		return false;

	/*
	 * Quirk only affects validation in wrmsr(), so wrmsrl()'s value
	 * should equal rdmsrl()'s even with the quirk.
	 */
	if (val_new != val_tmp)
		return false;

	if (is_lbr_from(msr))
		val_old = lbr_from_signext_quirk_wr(val_old);

	/* Here it's sure that the MSR can be safely accessed.
	 * Restore the old value and return.
	 */
	wrmsrl(msr, val_old);

	return true;
}

static __init void intel_sandybridge_quirk(void)
{
	x86_pmu.check_microcode = intel_snb_check_microcode;
	cpus_read_lock();
	intel_snb_check_microcode();
	cpus_read_unlock();
}

static const struct { int id; char *name; } intel_arch_events_map[] __initconst = {
	{ PERF_COUNT_HW_CPU_CYCLES, "cpu cycles" },
	{ PERF_COUNT_HW_INSTRUCTIONS, "instructions" },
	{ PERF_COUNT_HW_BUS_CYCLES, "bus cycles" },
	{ PERF_COUNT_HW_CACHE_REFERENCES, "cache references" },
	{ PERF_COUNT_HW_CACHE_MISSES, "cache misses" },
	{ PERF_COUNT_HW_BRANCH_INSTRUCTIONS, "branch instructions" },
	{ PERF_COUNT_HW_BRANCH_MISSES, "branch misses" },
};

static __init void intel_arch_events_quirk(void)
{
	int bit;

	/* disable event that reported as not presend by cpuid */
	for_each_set_bit(bit, x86_pmu.events_mask, ARRAY_SIZE(intel_arch_events_map)) {
		intel_perfmon_event_map[intel_arch_events_map[bit].id] = 0;
		pr_warn("CPUID marked event: \'%s\' unavailable\n",
			intel_arch_events_map[bit].name);
	}
}

static __init void intel_nehalem_quirk(void)
{
	union cpuid10_ebx ebx;

	ebx.full = x86_pmu.events_maskl;
	if (ebx.split.no_branch_misses_retired) {
		/*
		 * Erratum AAJ80 detected, we work it around by using
		 * the BR_MISP_EXEC.ANY event. This will over-count
		 * branch-misses, but it's still much better than the
		 * architectural event which is often completely bogus:
		 */
		intel_perfmon_event_map[PERF_COUNT_HW_BRANCH_MISSES] = 0x7f89;
		ebx.split.no_branch_misses_retired = 0;
		x86_pmu.events_maskl = ebx.full;
		pr_info("CPU erratum AAJ80 worked around\n");
	}
}

/*
 * enable software workaround for errata:
 * SNB: BJ122
 * IVB: BV98
 * HSW: HSD29
 *
 * Only needed when HT is enabled. However detecting
 * if HT is enabled is difficult (model specific). So instead,
 * we enable the workaround in the early boot, and verify if
 * it is needed in a later initcall phase once we have valid
 * topology information to check if HT is actually enabled
 */
static __init void intel_ht_bug(void)
{
	x86_pmu.flags |= PMU_FL_EXCL_CNTRS | PMU_FL_EXCL_ENABLED;

	x86_pmu.start_scheduling = intel_start_scheduling;
	x86_pmu.commit_scheduling = intel_commit_scheduling;
	x86_pmu.stop_scheduling = intel_stop_scheduling;
}

EVENT_ATTR_STR(mem-loads,	mem_ld_hsw,	"event=0xcd,umask=0x1,ldlat=3");
EVENT_ATTR_STR(mem-stores,	mem_st_hsw,	"event=0xd0,umask=0x82")

/* Haswell special events */
EVENT_ATTR_STR(tx-start,	tx_start,	"event=0xc9,umask=0x1");
EVENT_ATTR_STR(tx-commit,	tx_commit,	"event=0xc9,umask=0x2");
EVENT_ATTR_STR(tx-abort,	tx_abort,	"event=0xc9,umask=0x4");
EVENT_ATTR_STR(tx-capacity,	tx_capacity,	"event=0x54,umask=0x2");
EVENT_ATTR_STR(tx-conflict,	tx_conflict,	"event=0x54,umask=0x1");
EVENT_ATTR_STR(el-start,	el_start,	"event=0xc8,umask=0x1");
EVENT_ATTR_STR(el-commit,	el_commit,	"event=0xc8,umask=0x2");
EVENT_ATTR_STR(el-abort,	el_abort,	"event=0xc8,umask=0x4");
EVENT_ATTR_STR(el-capacity,	el_capacity,	"event=0x54,umask=0x2");
EVENT_ATTR_STR(el-conflict,	el_conflict,	"event=0x54,umask=0x1");
EVENT_ATTR_STR(cycles-t,	cycles_t,	"event=0x3c,in_tx=1");
EVENT_ATTR_STR(cycles-ct,	cycles_ct,	"event=0x3c,in_tx=1,in_tx_cp=1");

static struct attribute *hsw_events_attrs[] = {
	EVENT_PTR(mem_ld_hsw),
	EVENT_PTR(mem_st_hsw),
	EVENT_PTR(td_slots_issued),
	EVENT_PTR(td_slots_retired),
	EVENT_PTR(td_fetch_bubbles),
	EVENT_PTR(td_total_slots),
	EVENT_PTR(td_total_slots_scale),
	EVENT_PTR(td_recovery_bubbles),
	EVENT_PTR(td_recovery_bubbles_scale),
	NULL
};

static struct attribute *hsw_tsx_events_attrs[] = {
	EVENT_PTR(tx_start),
	EVENT_PTR(tx_commit),
	EVENT_PTR(tx_abort),
	EVENT_PTR(tx_capacity),
	EVENT_PTR(tx_conflict),
	EVENT_PTR(el_start),
	EVENT_PTR(el_commit),
	EVENT_PTR(el_abort),
	EVENT_PTR(el_capacity),
	EVENT_PTR(el_conflict),
	EVENT_PTR(cycles_t),
	EVENT_PTR(cycles_ct),
	NULL
};

static __init struct attribute **get_hsw_events_attrs(void)
{
	return boot_cpu_has(X86_FEATURE_RTM) ?
		merge_attr(hsw_events_attrs, hsw_tsx_events_attrs) :
		hsw_events_attrs;
}

static ssize_t freeze_on_smi_show(struct device *cdev,
				  struct device_attribute *attr,
				  char *buf)
{
	return sprintf(buf, "%lu\n", x86_pmu.attr_freeze_on_smi);
}

static DEFINE_MUTEX(freeze_on_smi_mutex);

static ssize_t freeze_on_smi_store(struct device *cdev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned long val;
	ssize_t ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	if (val > 1)
		return -EINVAL;

	mutex_lock(&freeze_on_smi_mutex);

	if (x86_pmu.attr_freeze_on_smi == val)
		goto done;

	x86_pmu.attr_freeze_on_smi = val;

	get_online_cpus();
	on_each_cpu(flip_smm_bit, &val, 1);
	put_online_cpus();
done:
	mutex_unlock(&freeze_on_smi_mutex);

	return count;
}

static DEVICE_ATTR_RW(freeze_on_smi);

static ssize_t branches_show(struct device *cdev,
			     struct device_attribute *attr,
			     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", x86_pmu.lbr_nr);
}

static DEVICE_ATTR_RO(branches);

static struct attribute *lbr_attrs[] = {
	&dev_attr_branches.attr,
	NULL
};

static char pmu_name_str[30];

static ssize_t pmu_name_show(struct device *cdev,
			     struct device_attribute *attr,
			     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", pmu_name_str);
}

static DEVICE_ATTR_RO(pmu_name);

static struct attribute *intel_pmu_caps_attrs[] = {
       &dev_attr_pmu_name.attr,
       NULL
};

static DEVICE_BOOL_ATTR(allow_tsx_force_abort, 0644, allow_tsx_force_abort);

static struct attribute *intel_pmu_attrs[] = {
	&dev_attr_freeze_on_smi.attr,
	NULL, /* &dev_attr_allow_tsx_force_abort.attr.attr */
	NULL,
};

__init int intel_pmu_init(void)
{
	struct attribute **extra_attr = NULL;
	struct attribute **to_free = NULL;
	union cpuid10_edx edx;
	union cpuid10_eax eax;
	union cpuid10_ebx ebx;
	struct event_constraint *c;
	unsigned int unused;
	struct extra_reg *er;
	int version, i;
	char *name;

	if (!cpu_has(&boot_cpu_data, X86_FEATURE_ARCH_PERFMON)) {
		switch (boot_cpu_data.x86) {
		case 0x6:
			return p6_pmu_init();
		case 0xb:
			return knc_pmu_init();
		case 0xf:
			return p4_pmu_init();
		}
		return -ENODEV;
	}

	/*
	 * Check whether the Architectural PerfMon supports
	 * Branch Misses Retired hw_event or not.
	 */
	cpuid(10, &eax.full, &ebx.full, &unused, &edx.full);
	if (eax.split.mask_length < ARCH_PERFMON_EVENTS_COUNT)
		return -ENODEV;

	version = eax.split.version_id;
	if (version < 2)
		x86_pmu = core_pmu;
	else
		x86_pmu = intel_pmu;

	x86_pmu.version			= version;
	x86_pmu.num_counters		= eax.split.num_counters;
	x86_pmu.cntval_bits		= eax.split.bit_width;
	x86_pmu.cntval_mask		= (1ULL << eax.split.bit_width) - 1;

	x86_pmu.events_maskl		= ebx.full;
	x86_pmu.events_mask_len		= eax.split.mask_length;

	x86_pmu.max_pebs_events		= min_t(unsigned, MAX_PEBS_EVENTS, x86_pmu.num_counters);

	/*
	 * Quirk: v2 perfmon does not report fixed-purpose events, so
	 * assume at least 3 events, when not running in a hypervisor:
	 */
	if (version > 1) {
		int assume = 3 * !boot_cpu_has(X86_FEATURE_HYPERVISOR);

		x86_pmu.num_counters_fixed =
			max((int)edx.split.num_counters_fixed, assume);
	}

	if (boot_cpu_has(X86_FEATURE_PDCM)) {
		u64 capabilities;

		rdmsrl(MSR_IA32_PERF_CAPABILITIES, capabilities);
		x86_pmu.intel_cap.capabilities = capabilities;
	}

	intel_ds_init();

	x86_add_quirk(intel_arch_events_quirk); /* Install first, so it runs last */

	/*
	 * Install the hw-cache-events table:
	 */
	switch (boot_cpu_data.x86_model) {
	case INTEL_FAM6_CORE_YONAH:
		pr_cont("Core events, ");
		name = "core";
		break;

	case INTEL_FAM6_CORE2_MEROM:
		x86_add_quirk(intel_clovertown_quirk);
	case INTEL_FAM6_CORE2_MEROM_L:
	case INTEL_FAM6_CORE2_PENRYN:
	case INTEL_FAM6_CORE2_DUNNINGTON:
		memcpy(hw_cache_event_ids, core2_hw_cache_event_ids,
		       sizeof(hw_cache_event_ids));

		intel_pmu_lbr_init_core();

		x86_pmu.event_constraints = intel_core2_event_constraints;
		x86_pmu.pebs_constraints = intel_core2_pebs_event_constraints;
		pr_cont("Core2 events, ");
		name = "core2";
		break;

	case INTEL_FAM6_NEHALEM:
	case INTEL_FAM6_NEHALEM_EP:
	case INTEL_FAM6_NEHALEM_EX:
		memcpy(hw_cache_event_ids, nehalem_hw_cache_event_ids,
		       sizeof(hw_cache_event_ids));
		memcpy(hw_cache_extra_regs, nehalem_hw_cache_extra_regs,
		       sizeof(hw_cache_extra_regs));

		intel_pmu_lbr_init_nhm();

		x86_pmu.event_constraints = intel_nehalem_event_constraints;
		x86_pmu.pebs_constraints = intel_nehalem_pebs_event_constraints;
		x86_pmu.enable_all = intel_pmu_nhm_enable_all;
		x86_pmu.extra_regs = intel_nehalem_extra_regs;
		x86_pmu.limit_period = nhm_limit_period;

		x86_pmu.cpu_events = nhm_events_attrs;

		/* UOPS_ISSUED.STALLED_CYCLES */
		intel_perfmon_event_map[PERF_COUNT_HW_STALLED_CYCLES_FRONTEND] =
			X86_CONFIG(.event=0x0e, .umask=0x01, .inv=1, .cmask=1);
		/* UOPS_EXECUTED.CORE_ACTIVE_CYCLES,c=1,i=1 */
		intel_perfmon_event_map[PERF_COUNT_HW_STALLED_CYCLES_BACKEND] =
			X86_CONFIG(.event=0xb1, .umask=0x3f, .inv=1, .cmask=1);

		intel_pmu_pebs_data_source_nhm();
		x86_add_quirk(intel_nehalem_quirk);
		x86_pmu.pebs_no_tlb = 1;
		extra_attr = nhm_format_attr;

		pr_cont("Nehalem events, ");
		name = "nehalem";
		break;

	case INTEL_FAM6_ATOM_BONNELL:
	case INTEL_FAM6_ATOM_BONNELL_MID:
	case INTEL_FAM6_ATOM_SALTWELL:
	case INTEL_FAM6_ATOM_SALTWELL_MID:
	case INTEL_FAM6_ATOM_SALTWELL_TABLET:
		memcpy(hw_cache_event_ids, atom_hw_cache_event_ids,
		       sizeof(hw_cache_event_ids));

		intel_pmu_lbr_init_atom();

		x86_pmu.event_constraints = intel_gen_event_constraints;
		x86_pmu.pebs_constraints = intel_atom_pebs_event_constraints;
		x86_pmu.pebs_aliases = intel_pebs_aliases_core2;
		pr_cont("Atom events, ");
		name = "bonnell";
		break;

	case INTEL_FAM6_ATOM_SILVERMONT:
	case INTEL_FAM6_ATOM_SILVERMONT_X:
	case INTEL_FAM6_ATOM_SILVERMONT_MID:
	case INTEL_FAM6_ATOM_AIRMONT:
	case INTEL_FAM6_ATOM_AIRMONT_MID:
		memcpy(hw_cache_event_ids, slm_hw_cache_event_ids,
			sizeof(hw_cache_event_ids));
		memcpy(hw_cache_extra_regs, slm_hw_cache_extra_regs,
		       sizeof(hw_cache_extra_regs));

		intel_pmu_lbr_init_slm();

		x86_pmu.event_constraints = intel_slm_event_constraints;
		x86_pmu.pebs_constraints = intel_slm_pebs_event_constraints;
		x86_pmu.extra_regs = intel_slm_extra_regs;
		x86_pmu.flags |= PMU_FL_HAS_RSP_1;
		x86_pmu.cpu_events = slm_events_attrs;
		extra_attr = slm_format_attr;
		pr_cont("Silvermont events, ");
		name = "silvermont";
		break;

	case INTEL_FAM6_ATOM_GOLDMONT:
	case INTEL_FAM6_ATOM_GOLDMONT_X:
		memcpy(hw_cache_event_ids, glm_hw_cache_event_ids,
		       sizeof(hw_cache_event_ids));
		memcpy(hw_cache_extra_regs, glm_hw_cache_extra_regs,
		       sizeof(hw_cache_extra_regs));

		intel_pmu_lbr_init_skl();

		x86_pmu.event_constraints = intel_slm_event_constraints;
		x86_pmu.pebs_constraints = intel_glm_pebs_event_constraints;
		x86_pmu.extra_regs = intel_glm_extra_regs;
		/*
		 * It's recommended to use CPU_CLK_UNHALTED.CORE_P + NPEBS
		 * for precise cycles.
		 * :pp is identical to :ppp
		 */
		x86_pmu.pebs_aliases = NULL;
		x86_pmu.pebs_prec_dist = true;
		x86_pmu.lbr_pt_coexist = true;
		x86_pmu.flags |= PMU_FL_HAS_RSP_1;
		x86_pmu.cpu_events = glm_events_attrs;
		extra_attr = slm_format_attr;
		pr_cont("Goldmont events, ");
		name = "goldmont";
		break;

	case INTEL_FAM6_ATOM_GOLDMONT_PLUS:
		memcpy(hw_cache_event_ids, glp_hw_cache_event_ids,
		       sizeof(hw_cache_event_ids));
		memcpy(hw_cache_extra_regs, glp_hw_cache_extra_regs,
		       sizeof(hw_cache_extra_regs));

		intel_pmu_lbr_init_skl();

		x86_pmu.event_constraints = intel_slm_event_constraints;
		x86_pmu.extra_regs = intel_glm_extra_regs;
		/*
		 * It's recommended to use CPU_CLK_UNHALTED.CORE_P + NPEBS
		 * for precise cycles.
		 */
		x86_pmu.pebs_aliases = NULL;
		x86_pmu.pebs_prec_dist = true;
		x86_pmu.lbr_pt_coexist = true;
		x86_pmu.flags |= PMU_FL_HAS_RSP_1;
		x86_pmu.flags |= PMU_FL_PEBS_ALL;
		x86_pmu.get_event_constraints = glp_get_event_constraints;
		x86_pmu.cpu_events = glm_events_attrs;
		/* Goldmont Plus has 4-wide pipeline */
		event_attr_td_total_slots_scale_glm.event_str = "4";
		extra_attr = slm_format_attr;
		pr_cont("Goldmont plus events, ");
		name = "goldmont_plus";
		break;

	case INTEL_FAM6_WESTMERE:
	case INTEL_FAM6_WESTMERE_EP:
	case INTEL_FAM6_WESTMERE_EX:
		memcpy(hw_cache_event_ids, westmere_hw_cache_event_ids,
		       sizeof(hw_cache_event_ids));
		memcpy(hw_cache_extra_regs, nehalem_hw_cache_extra_regs,
		       sizeof(hw_cache_extra_regs));

		intel_pmu_lbr_init_nhm();

		x86_pmu.event_constraints = intel_westmere_event_constraints;
		x86_pmu.enable_all = intel_pmu_nhm_enable_all;
		x86_pmu.pebs_constraints = intel_westmere_pebs_event_constraints;
		x86_pmu.extra_regs = intel_westmere_extra_regs;
		x86_pmu.flags |= PMU_FL_HAS_RSP_1;

		x86_pmu.cpu_events = nhm_events_attrs;

		/* UOPS_ISSUED.STALLED_CYCLES */
		intel_perfmon_event_map[PERF_COUNT_HW_STALLED_CYCLES_FRONTEND] =
			X86_CONFIG(.event=0x0e, .umask=0x01, .inv=1, .cmask=1);
		/* UOPS_EXECUTED.CORE_ACTIVE_CYCLES,c=1,i=1 */
		intel_perfmon_event_map[PERF_COUNT_HW_STALLED_CYCLES_BACKEND] =
			X86_CONFIG(.event=0xb1, .umask=0x3f, .inv=1, .cmask=1);

		intel_pmu_pebs_data_source_nhm();
		extra_attr = nhm_format_attr;
		pr_cont("Westmere events, ");
		name = "westmere";
		break;

	case INTEL_FAM6_SANDYBRIDGE:
	case INTEL_FAM6_SANDYBRIDGE_X:
		x86_add_quirk(intel_sandybridge_quirk);
		x86_add_quirk(intel_ht_bug);
		memcpy(hw_cache_event_ids, snb_hw_cache_event_ids,
		       sizeof(hw_cache_event_ids));
		memcpy(hw_cache_extra_regs, snb_hw_cache_extra_regs,
		       sizeof(hw_cache_extra_regs));

		intel_pmu_lbr_init_snb();

		x86_pmu.event_constraints = intel_snb_event_constraints;
		x86_pmu.pebs_constraints = intel_snb_pebs_event_constraints;
		x86_pmu.pebs_aliases = intel_pebs_aliases_snb;
		if (boot_cpu_data.x86_model == INTEL_FAM6_SANDYBRIDGE_X)
			x86_pmu.extra_regs = intel_snbep_extra_regs;
		else
			x86_pmu.extra_regs = intel_snb_extra_regs;


		/* all extra regs are per-cpu when HT is on */
		x86_pmu.flags |= PMU_FL_HAS_RSP_1;
		x86_pmu.flags |= PMU_FL_NO_HT_SHARING;

		x86_pmu.cpu_events = snb_events_attrs;

		/* UOPS_ISSUED.ANY,c=1,i=1 to count stall cycles */
		intel_perfmon_event_map[PERF_COUNT_HW_STALLED_CYCLES_FRONTEND] =
			X86_CONFIG(.event=0x0e, .umask=0x01, .inv=1, .cmask=1);
		/* UOPS_DISPATCHED.THREAD,c=1,i=1 to count stall cycles*/
		intel_perfmon_event_map[PERF_COUNT_HW_STALLED_CYCLES_BACKEND] =
			X86_CONFIG(.event=0xb1, .umask=0x01, .inv=1, .cmask=1);

		extra_attr = nhm_format_attr;

		pr_cont("SandyBridge events, ");
		name = "sandybridge";
		break;

	case INTEL_FAM6_IVYBRIDGE:
	case INTEL_FAM6_IVYBRIDGE_X:
		x86_add_quirk(intel_ht_bug);
		memcpy(hw_cache_event_ids, snb_hw_cache_event_ids,
		       sizeof(hw_cache_event_ids));
		/* dTLB-load-misses on IVB is different than SNB */
		hw_cache_event_ids[C(DTLB)][C(OP_READ)][C(RESULT_MISS)] = 0x8108; /* DTLB_LOAD_MISSES.DEMAND_LD_MISS_CAUSES_A_WALK */

		memcpy(hw_cache_extra_regs, snb_hw_cache_extra_regs,
		       sizeof(hw_cache_extra_regs));

		intel_pmu_lbr_init_snb();

		x86_pmu.event_constraints = intel_ivb_event_constraints;
		x86_pmu.pebs_constraints = intel_ivb_pebs_event_constraints;
		x86_pmu.pebs_aliases = intel_pebs_aliases_ivb;
		x86_pmu.pebs_prec_dist = true;
		if (boot_cpu_data.x86_model == INTEL_FAM6_IVYBRIDGE_X)
			x86_pmu.extra_regs = intel_snbep_extra_regs;
		else
			x86_pmu.extra_regs = intel_snb_extra_regs;
		/* all extra regs are per-cpu when HT is on */
		x86_pmu.flags |= PMU_FL_HAS_RSP_1;
		x86_pmu.flags |= PMU_FL_NO_HT_SHARING;

		x86_pmu.cpu_events = snb_events_attrs;

		/* UOPS_ISSUED.ANY,c=1,i=1 to count stall cycles */
		intel_perfmon_event_map[PERF_COUNT_HW_STALLED_CYCLES_FRONTEND] =
			X86_CONFIG(.event=0x0e, .umask=0x01, .inv=1, .cmask=1);

		extra_attr = nhm_format_attr;

		pr_cont("IvyBridge events, ");
		name = "ivybridge";
		break;


	case INTEL_FAM6_HASWELL_CORE:
	case INTEL_FAM6_HASWELL_X:
	case INTEL_FAM6_HASWELL_ULT:
	case INTEL_FAM6_HASWELL_GT3E:
		x86_add_quirk(intel_ht_bug);
		x86_pmu.late_ack = true;
		memcpy(hw_cache_event_ids, hsw_hw_cache_event_ids, sizeof(hw_cache_event_ids));
		memcpy(hw_cache_extra_regs, hsw_hw_cache_extra_regs, sizeof(hw_cache_extra_regs));

		intel_pmu_lbr_init_hsw();

		x86_pmu.event_constraints = intel_hsw_event_constraints;
		x86_pmu.pebs_constraints = intel_hsw_pebs_event_constraints;
		x86_pmu.extra_regs = intel_snbep_extra_regs;
		x86_pmu.pebs_aliases = intel_pebs_aliases_ivb;
		x86_pmu.pebs_prec_dist = true;
		/* all extra regs are per-cpu when HT is on */
		x86_pmu.flags |= PMU_FL_HAS_RSP_1;
		x86_pmu.flags |= PMU_FL_NO_HT_SHARING;

		x86_pmu.hw_config = hsw_hw_config;
		x86_pmu.get_event_constraints = hsw_get_event_constraints;
		x86_pmu.cpu_events = get_hsw_events_attrs();
		x86_pmu.lbr_double_abort = true;
		extra_attr = boot_cpu_has(X86_FEATURE_RTM) ?
			hsw_format_attr : nhm_format_attr;
		pr_cont("Haswell events, ");
		name = "haswell";
		break;

	case INTEL_FAM6_BROADWELL_CORE:
	case INTEL_FAM6_BROADWELL_XEON_D:
	case INTEL_FAM6_BROADWELL_GT3E:
	case INTEL_FAM6_BROADWELL_X:
		x86_pmu.late_ack = true;
		memcpy(hw_cache_event_ids, hsw_hw_cache_event_ids, sizeof(hw_cache_event_ids));
		memcpy(hw_cache_extra_regs, hsw_hw_cache_extra_regs, sizeof(hw_cache_extra_regs));

		/* L3_MISS_LOCAL_DRAM is BIT(26) in Broadwell */
		hw_cache_extra_regs[C(LL)][C(OP_READ)][C(RESULT_MISS)] = HSW_DEMAND_READ |
									 BDW_L3_MISS|HSW_SNOOP_DRAM;
		hw_cache_extra_regs[C(LL)][C(OP_WRITE)][C(RESULT_MISS)] = HSW_DEMAND_WRITE|BDW_L3_MISS|
									  HSW_SNOOP_DRAM;
		hw_cache_extra_regs[C(NODE)][C(OP_READ)][C(RESULT_ACCESS)] = HSW_DEMAND_READ|
									     BDW_L3_MISS_LOCAL|HSW_SNOOP_DRAM;
		hw_cache_extra_regs[C(NODE)][C(OP_WRITE)][C(RESULT_ACCESS)] = HSW_DEMAND_WRITE|
									      BDW_L3_MISS_LOCAL|HSW_SNOOP_DRAM;

		intel_pmu_lbr_init_hsw();

		x86_pmu.event_constraints = intel_bdw_event_constraints;
		x86_pmu.pebs_constraints = intel_bdw_pebs_event_constraints;
		x86_pmu.extra_regs = intel_snbep_extra_regs;
		x86_pmu.pebs_aliases = intel_pebs_aliases_ivb;
		x86_pmu.pebs_prec_dist = true;
		/* all extra regs are per-cpu when HT is on */
		x86_pmu.flags |= PMU_FL_HAS_RSP_1;
		x86_pmu.flags |= PMU_FL_NO_HT_SHARING;

		x86_pmu.hw_config = hsw_hw_config;
		x86_pmu.get_event_constraints = hsw_get_event_constraints;
		x86_pmu.cpu_events = get_hsw_events_attrs();
		x86_pmu.limit_period = bdw_limit_period;
		extra_attr = boot_cpu_has(X86_FEATURE_RTM) ?
			hsw_format_attr : nhm_format_attr;
		pr_cont("Broadwell events, ");
		name = "broadwell";
		break;

	case INTEL_FAM6_XEON_PHI_KNL:
	case INTEL_FAM6_XEON_PHI_KNM:
		memcpy(hw_cache_event_ids,
		       slm_hw_cache_event_ids, sizeof(hw_cache_event_ids));
		memcpy(hw_cache_extra_regs,
		       knl_hw_cache_extra_regs, sizeof(hw_cache_extra_regs));
		intel_pmu_lbr_init_knl();

		x86_pmu.event_constraints = intel_slm_event_constraints;
		x86_pmu.pebs_constraints = intel_slm_pebs_event_constraints;
		x86_pmu.extra_regs = intel_knl_extra_regs;

		/* all extra regs are per-cpu when HT is on */
		x86_pmu.flags |= PMU_FL_HAS_RSP_1;
		x86_pmu.flags |= PMU_FL_NO_HT_SHARING;
		extra_attr = slm_format_attr;
		pr_cont("Knights Landing/Mill events, ");
		name = "knights-landing";
		break;

	case INTEL_FAM6_SKYLAKE_MOBILE:
	case INTEL_FAM6_SKYLAKE_DESKTOP:
	case INTEL_FAM6_SKYLAKE_X:
	case INTEL_FAM6_KABYLAKE_MOBILE:
	case INTEL_FAM6_KABYLAKE_DESKTOP:
		x86_pmu.late_ack = true;
		memcpy(hw_cache_event_ids, skl_hw_cache_event_ids, sizeof(hw_cache_event_ids));
		memcpy(hw_cache_extra_regs, skl_hw_cache_extra_regs, sizeof(hw_cache_extra_regs));
		intel_pmu_lbr_init_skl();

		/* INT_MISC.RECOVERY_CYCLES has umask 1 in Skylake */
		event_attr_td_recovery_bubbles.event_str_noht =
			"event=0xd,umask=0x1,cmask=1";
		event_attr_td_recovery_bubbles.event_str_ht =
			"event=0xd,umask=0x1,cmask=1,any=1";

		x86_pmu.event_constraints = intel_skl_event_constraints;
		x86_pmu.pebs_constraints = intel_skl_pebs_event_constraints;
		x86_pmu.extra_regs = intel_skl_extra_regs;
		x86_pmu.pebs_aliases = intel_pebs_aliases_skl;
		x86_pmu.pebs_prec_dist = true;
		/* all extra regs are per-cpu when HT is on */
		x86_pmu.flags |= PMU_FL_HAS_RSP_1;
		x86_pmu.flags |= PMU_FL_NO_HT_SHARING;

		x86_pmu.hw_config = hsw_hw_config;
		x86_pmu.get_event_constraints = hsw_get_event_constraints;
		extra_attr = boot_cpu_has(X86_FEATURE_RTM) ?
			hsw_format_attr : nhm_format_attr;
		extra_attr = merge_attr(extra_attr, skl_format_attr);
		to_free = extra_attr;
		x86_pmu.cpu_events = get_hsw_events_attrs();
		intel_pmu_pebs_data_source_skl(
			boot_cpu_data.x86_model == INTEL_FAM6_SKYLAKE_X);

		if (boot_cpu_has(X86_FEATURE_TSX_FORCE_ABORT)) {
			x86_pmu.flags |= PMU_FL_TFA;
			x86_pmu.get_event_constraints = tfa_get_event_constraints;
			x86_pmu.enable_all = intel_tfa_pmu_enable_all;
			x86_pmu.commit_scheduling = intel_tfa_commit_scheduling;
			intel_pmu_attrs[1] = &dev_attr_allow_tsx_force_abort.attr.attr;
		}

		pr_cont("Skylake events, ");
		name = "skylake";
		break;

	default:
		switch (x86_pmu.version) {
		case 1:
			x86_pmu.event_constraints = intel_v1_event_constraints;
			pr_cont("generic architected perfmon v1, ");
			name = "generic_arch_v1";
			break;
		default:
			/*
			 * default constraints for v2 and up
			 */
			x86_pmu.event_constraints = intel_gen_event_constraints;
			pr_cont("generic architected perfmon, ");
			name = "generic_arch_v2+";
			break;
		}
	}

	snprintf(pmu_name_str, sizeof pmu_name_str, "%s", name);

	if (version >= 2 && extra_attr) {
		x86_pmu.format_attrs = merge_attr(intel_arch3_formats_attr,
						  extra_attr);
		WARN_ON(!x86_pmu.format_attrs);
	}

	if (x86_pmu.num_counters > INTEL_PMC_MAX_GENERIC) {
		WARN(1, KERN_ERR "hw perf events %d > max(%d), clipping!",
		     x86_pmu.num_counters, INTEL_PMC_MAX_GENERIC);
		x86_pmu.num_counters = INTEL_PMC_MAX_GENERIC;
	}
	x86_pmu.intel_ctrl = (1ULL << x86_pmu.num_counters) - 1;

	if (x86_pmu.num_counters_fixed > INTEL_PMC_MAX_FIXED) {
		WARN(1, KERN_ERR "hw perf events fixed %d > max(%d), clipping!",
		     x86_pmu.num_counters_fixed, INTEL_PMC_MAX_FIXED);
		x86_pmu.num_counters_fixed = INTEL_PMC_MAX_FIXED;
	}

	x86_pmu.intel_ctrl |=
		((1LL << x86_pmu.num_counters_fixed)-1) << INTEL_PMC_IDX_FIXED;

	if (x86_pmu.event_constraints) {
		/*
		 * event on fixed counter2 (REF_CYCLES) only works on this
		 * counter, so do not extend mask to generic counters
		 */
		for_each_event_constraint(c, x86_pmu.event_constraints) {
			if (c->cmask == FIXED_EVENT_FLAGS
			    && c->idxmsk64 != INTEL_PMC_MSK_FIXED_REF_CYCLES) {
				c->idxmsk64 |= (1ULL << x86_pmu.num_counters) - 1;
			}
			c->idxmsk64 &=
				~(~0ULL << (INTEL_PMC_IDX_FIXED + x86_pmu.num_counters_fixed));
			c->weight = hweight64(c->idxmsk64);
		}
	}

	/*
	 * Access LBR MSR may cause #GP under certain circumstances.
	 * E.g. KVM doesn't support LBR MSR
	 * Check all LBT MSR here.
	 * Disable LBR access if any LBR MSRs can not be accessed.
	 */
	if (x86_pmu.lbr_nr && !check_msr(x86_pmu.lbr_tos, 0x3UL))
		x86_pmu.lbr_nr = 0;
	for (i = 0; i < x86_pmu.lbr_nr; i++) {
		if (!(check_msr(x86_pmu.lbr_from + i, 0xffffUL) &&
		      check_msr(x86_pmu.lbr_to + i, 0xffffUL)))
			x86_pmu.lbr_nr = 0;
	}

	x86_pmu.caps_attrs = intel_pmu_caps_attrs;

	if (x86_pmu.lbr_nr) {
		x86_pmu.caps_attrs = merge_attr(x86_pmu.caps_attrs, lbr_attrs);
		pr_cont("%d-deep LBR, ", x86_pmu.lbr_nr);
	}

	/*
	 * Access extra MSR may cause #GP under certain circumstances.
	 * E.g. KVM doesn't support offcore event
	 * Check all extra_regs here.
	 */
	if (x86_pmu.extra_regs) {
		for (er = x86_pmu.extra_regs; er->msr; er++) {
			er->extra_msr_access = check_msr(er->msr, 0x11UL);
			/* Disable LBR select mapping */
			if ((er->idx == EXTRA_REG_LBR) && !er->extra_msr_access)
				x86_pmu.lbr_sel_map = NULL;
		}
	}

	/* Support full width counters using alternative MSR range */
	if (x86_pmu.intel_cap.full_width_write) {
		x86_pmu.max_period = x86_pmu.cntval_mask >> 1;
		x86_pmu.perfctr = MSR_IA32_PMC0;
		pr_cont("full-width counters, ");
	}

	kfree(to_free);
	return 0;
}

/*
 * HT bug: phase 2 init
 * Called once we have valid topology information to check
 * whether or not HT is enabled
 * If HT is off, then we disable the workaround
 */
static __init int fixup_ht_bug(void)
{
	int c;
	/*
	 * problem not present on this CPU model, nothing to do
	 */
	if (!(x86_pmu.flags & PMU_FL_EXCL_ENABLED))
		return 0;

	if (topology_max_smt_threads() > 1) {
		pr_info("PMU erratum BJ122, BV98, HSD29 worked around, HT is on\n");
		return 0;
	}

	cpus_read_lock();

	hardlockup_detector_perf_stop();

	x86_pmu.flags &= ~(PMU_FL_EXCL_CNTRS | PMU_FL_EXCL_ENABLED);

	x86_pmu.start_scheduling = NULL;
	x86_pmu.commit_scheduling = NULL;
	x86_pmu.stop_scheduling = NULL;

	hardlockup_detector_perf_restart();

	for_each_online_cpu(c)
		free_excl_cntrs(&per_cpu(cpu_hw_events, c));

	cpus_read_unlock();
	pr_info("PMU erratum BJ122, BV98, HSD29 workaround disabled, HT off\n");
	return 0;
}
subsys_initcall(fixup_ht_bug)
