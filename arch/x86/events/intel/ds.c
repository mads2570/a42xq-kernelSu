// SPDX-License-Identifier: GPL-2.0
#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/slab.h>

#include <asm/cpu_entry_area.h>
#include <asm/perf_event.h>
#include <asm/tlbflush.h>
#include <asm/insn.h>

#include "../perf_event.h"

/* Waste a full page so it can be mapped into the cpu_entry_area */
DEFINE_PER_CPU_PAGE_ALIGNED(struct debug_store, cpu_debug_store);

/* The size of a BTS record in bytes: */
#define BTS_RECORD_SIZE		24

#define PEBS_FIXUP_SIZE		PAGE_SIZE

/*
 * pebs_record_32 for p4 and core not supported

struct pebs_record_32 {
	u32 flags, ip;
	u32 ax, bc, cx, dx;
	u32 si, di, bp, sp;
};

 */

union intel_x86_pebs_dse {
	u64 val;
	struct {
		unsigned int ld_dse:4;
		unsigned int ld_stlb_miss:1;
		unsigned int ld_locked:1;
		unsigned int ld_reserved:26;
	};
	struct {
		unsigned int st_l1d_hit:1;
		unsigned int st_reserved1:3;
		unsigned int st_stlb_miss:1;
		unsigned int st_locked:1;
		unsigned int st_reserved2:26;
	};
};


/*
 * Map PEBS Load Latency Data Source encodings to generic
 * memory data source information
 */
#define P(a, b) PERF_MEM_S(a, b)
#define OP_LH (P(OP, LOAD) | P(LVL, HIT))
#define LEVEL(x) P(LVLNUM, x)
#define REM P(REMOTE, REMOTE)
#define SNOOP_NONE_MISS (P(SNOOP, NONE) | P(SNOOP, MISS))

/* Version for Sandy Bridge and later */
static u64 pebs_data_source[] = {
	P(OP, LOAD) | P(LVL, MISS) | LEVEL(L3) | P(SNOOP, NA),/* 0x00:ukn L3 */
	OP_LH | P(LVL, L1)  | LEVEL(L1) | P(SNOOP, NONE),  /* 0x01: L1 local */
	OP_LH | P(LVL, LFB) | LEVEL(LFB) | P(SNOOP, NONE), /* 0x02: LFB hit */
	OP_LH | P(LVL, L2)  | LEVEL(L2) | P(SNOOP, NONE),  /* 0x03: L2 hit */
	OP_LH | P(LVL, L3)  | LEVEL(L3) | P(SNOOP, NONE),  /* 0x04: L3 hit */
	OP_LH | P(LVL, L3)  | LEVEL(L3) | P(SNOOP, MISS),  /* 0x05: L3 hit, snoop miss */
	OP_LH | P(LVL, L3)  | LEVEL(L3) | P(SNOOP, HIT),   /* 0x06: L3 hit, snoop hit */
	OP_LH | P(LVL, L3)  | LEVEL(L3) | P(SNOOP, HITM),  /* 0x07: L3 hit, snoop hitm */
	OP_LH | P(LVL, REM_CCE1) | REM | LEVEL(L3) | P(SNOOP, HIT),  /* 0x08: L3 miss snoop hit */
	OP_LH | P(LVL, REM_CCE1) | REM | LEVEL(L3) | P(SNOOP, HITM), /* 0x09: L3 miss snoop hitm*/
	OP_LH | P(LVL, LOC_RAM)  | LEVEL(RAM) | P(SNOOP, HIT),       /* 0x0a: L3 miss, shared */
	OP_LH | P(LVL, REM_RAM1) | REM | LEVEL(L3) | P(SNOOP, HIT),  /* 0x0b: L3 miss, shared */
	OP_LH | P(LVL, LOC_RAM)  | LEVEL(RAM) | SNOOP_NONE_MISS,     /* 0x0c: L3 miss, excl */
	OP_LH | P(LVL, REM_RAM1) | LEVEL(RAM) | REM | SNOOP_NONE_MISS, /* 0x0d: L3 miss, excl */
	OP_LH | P(LVL, IO)  | LEVEL(NA) | P(SNOOP, NONE), /* 0x0e: I/O */
	OP_LH | P(LVL, UNC) | LEVEL(NA) | P(SNOOP, NONE), /* 0x0f: uncached */
};

/* Patch up minor differences in the bits */
void __init intel_pmu_pebs_data_source_nhm(void)
{
	pebs_data_source[0x05] = OP_LH | P(LVL, L3) | LEVEL(L3) | P(SNOOP, HIT);
	pebs_data_source[0x06] = OP_LH | P(LVL, L3) | LEVEL(L3) | P(SNOOP, HITM);
	pebs_data_source[0x07] = OP_LH | P(LVL, L3) | LEVEL(L3) | P(SNOOP, HITM);
}

void __init intel_pmu_pebs_data_source_skl(bool pmem)
{
	u64 pmem_or_l4 = pmem ? LEVEL(PMEM) : LEVEL(L4);

	pebs_data_source[0x08] = OP_LH | pmem_or_l4 | P(SNOOP, HIT);
	pebs_data_source[0x09] = OP_LH | pmem_or_l4 | REM | P(SNOOP, HIT);
	pebs_data_source[0x0b] = OP_LH | LEVEL(RAM) | REM | P(SNOOP, NONE);
	pebs_data_source[0x0c] = OP_LH | LEVEL(ANY_CACHE) | REM | P(SNOOPX, FWD);
	pebs_data_source[0x0d] = OP_LH | LEVEL(ANY_CACHE) | REM | P(SNOOP, HITM);
}

static u64 precise_store_data(u64 status)
{
	union intel_x86_pebs_dse dse;
	u64 val = P(OP, STORE) | P(SNOOP, NA) | P(LVL, L1) | P(TLB, L2);

	dse.val = status;

	/*
	 * bit 4: TLB access
	 * 1 = stored missed 2nd level TLB
	 *
	 * so it either hit the walker or the OS
	 * otherwise hit 2nd level TLB
	 */
	if (dse.st_stlb_miss)
		val |= P(TLB, MISS);
	else
		val |= P(TLB, HIT);

	/*
	 * bit 0: hit L1 data cache
	 * if not set, then all we know is that
	 * it missed L1D
	 */
	if (dse.st_l1d_hit)
		val |= P(LVL, HIT);
	else
		val |= P(LVL, MISS);

	/*
	 * bit 5: Locked prefix
	 */
	if (dse.st_locked)
		val |= P(LOCK, LOCKED);

	return val;
}

static u64 precise_datala_hsw(struct perf_event *event, u64 status)
{
	union perf_mem_data_src dse;

	dse.val = PERF_MEM_NA;

	if (event->hw.flags & PERF_X86_EVENT_PEBS_ST_HSW)
		dse.mem_op = PERF_MEM_OP_STORE;
	else if (event->hw.flags & PERF_X86_EVENT_PEBS_LD_HSW)
		dse.mem_op = PERF_MEM_OP_LOAD;

	/*
	 * L1 info only valid for following events:
	 *
	 * MEM_UOPS_RETIRED.STLB_MISS_STORES
	 * MEM_UOPS_RETIRED.LOCK_STORES
	 * MEM_UOPS_RETIRED.SPLIT_STORES
	 * MEM_UOPS_RETIRED.ALL_STORES
	 */
	if (event->hw.flags & PERF_X86_EVENT_PEBS_ST_HSW) {
		if (status & 1)
			dse.mem_lvl = PERF_MEM_LVL_L1 | PERF_MEM_LVL_HIT;
		else
			dse.mem_lvl = PERF_MEM_LVL_L1 | PERF_MEM_LVL_MISS;
	}
	return dse.val;
}

static u64 load_latency_data(u64 status)
{
	union intel_x86_pebs_dse dse;
	u64 val;

	dse.val = status;

	/*
	 * use the mapping table for bit 0-3
	 */
	val = pebs_data_source[dse.ld_dse];

	/*
	 * Nehalem models do not support TLB, Lock infos
	 */
	if (x86_pmu.pebs_no_tlb) {
		val |= P(TLB, NA) | P(LOCK, NA);
		return val;
	}
	/*
	 * bit 4: TLB access
	 * 0 = did not miss 2nd level TLB
	 * 1 = missed 2nd level TLB
	 */
	if (dse.ld_stlb_miss)
		val |= P(TLB, MISS) | P(TLB, L2);
	else
		val |= P(TLB, HIT) | P(TLB, L1) | P(TLB, L2);

	/*
	 * bit 5: locked prefix
	 */
	if (dse.ld_locked)
		val |= P(LOCK, LOCKED);

	return val;
}

struct pebs_record_core {
	u64 flags, ip;
	u64 ax, bx, cx, dx;
	u64 si, di, bp, sp;
	u64 r8,  r9,  r10, r11;
	u64 r12, r13, r14, r15;
};

struct pebs_record_nhm {
	u64 flags, ip;
	u64 ax, bx, cx, dx;
	u64 si, di, bp, sp;
	u64 r8,  r9,  r10, r11;
	u64 r12, r13, r14, r15;
	u64 status, dla, dse, lat;
};

/*
 * Same as pebs_record_nhm, with two additional fields.
 */
struct pebs_record_hsw {
	u64 flags, ip;
	u64 ax, bx, cx, dx;
	u64 si, di, bp, sp;
	u64 r8,  r9,  r10, r11;
	u64 r12, r13, r14, r15;
	u64 status, dla, dse, lat;
	u64 real_ip, tsx_tuning;
};

union hsw_tsx_tuning {
	struct {
		u32 cycles_last_block     : 32,
		    hle_abort		  : 1,
		    rtm_abort		  : 1,
		    instruction_abort     : 1,
		    non_instruction_abort : 1,
		    retry		  : 1,
		    data_conflict	  : 1,
		    capacity_writes	  : 1,
		    capacity_reads	  : 1;
	};
	u64	    value;
};

#define PEBS_HSW_TSX_FLAGS	0xff00000000ULL

/* Same as HSW, plus TSC */

struct pebs_record_skl {
	u64 flags, ip;
	u64 ax, bx, cx, dx;
	u64 si, di, bp, sp;
	u64 r8,  r9,  r10, r11;
	u64 r12, r13, r14, r15;
	u64 status, dla, dse, lat;
	u64 real_ip, tsx_tuning;
	u64 tsc;
};

void init_debug_store_on_cpu(int cpu)
{
	struct debug_store *ds = per_cpu(cpu_hw_events, cpu).ds;

	if (!ds)
		return;

	wrmsr_on_cpu(cpu, MSR_IA32_DS_AREA,
		     (u32)((u64)(unsigned long)ds),
		     (u32)((u64)(unsigned long)ds >> 32));
}

void fini_debug_store_on_cpu(int cpu)
{
	if (!per_cpu(cpu_hw_events, cpu).ds)
		return;

	wrmsr_on_cpu(cpu, MSR_IA32_DS_AREA, 0, 0);
}

static DEFINE_PER_CPU(void *, insn_buffer);

static void ds_update_cea(void *cea, void *addr, size_t size, pgprot_t prot)
{
	unsigned long start = (unsigned long)cea;
	phys_addr_t pa;
	size_t msz = 0;

	pa = virt_to_phys(addr);

	preempt_disable();
	for (; msz < size; msz += PAGE_SIZE, pa += PAGE_SIZE, cea += PAGE_SIZE)
		cea_set_pte(cea, pa, prot);

	/*
	 * This is a cross-CPU update of the cpu_entry_area, we must shoot down
	 * all TLB entries for it.
	 */
	flush_tlb_kernel_range(start, start + size);
	preempt_enable();
}

static void ds_clear_cea(void *cea, size_t size)
{
	unsigned long start = (unsigned long)cea;
	size_t msz = 0;

	preempt_disable();
	for (; msz < size; msz += PAGE_SIZE, cea += PAGE_SIZE)
		cea_set_pte(cea, 0, PAGE_NONE);

	flush_tlb_kernel_range(start, start + size);
	preempt_enable();
}

static void *dsalloc_pages(size_t size, gfp_t flags, int cpu)
{
	unsigned int order = get_order(size);
	int node = cpu_to_node(cpu);
	struct page *page;

	page = __alloc_pages_node(node, flags | __GFP_ZERO, order);
	return page ? page_address(page) : NULL;
}

static void dsfree_pages(const void *buffer, size_t size)
{
	if (buffer)
		free_pages((unsigned long)buffer, get_order(size));
}

static int alloc_pebs_buffer(int cpu)
{
	struct cpu_hw_events *hwev = per_cpu_ptr(&cpu_hw_events, cpu);
	struct debug_store *ds = hwev->ds;
	size_t bsiz = x86_pmu.pebs_buffer_size;
	int max, node = cpu_to_node(cpu);
	void *buffer, *ibuffer, *cea;

	if (!x86_pmu.pebs)
		return 0;

	buffer = dsalloc_pages(bsiz, GFP_KERNEL, cpu);
	if (unlikely(!buffer))
		return -ENOMEM;

	/*
	 * HSW+ already provides us the eventing ip; no need to allocate this
	 * buffer then.
	 */
	if (x86_pmu.intel_cap.pebs_format < 2) {
		ibuffer = kzalloc_node(PEBS_FIXUP_SIZE, GFP_KERNEL, node);
		if (!ibuffer) {
			dsfree_pages(buffer, bsiz);
			return -ENOMEM;
		}
		per_cpu(insn_buffer, cpu) = ibuffer;
	}
	hwev->ds_pebs_vaddr = buffer;
	/* Update the cpu entry area mapping */
	cea = &get_cpu_entry_area(cpu)->cpu_debug_buffers.pebs_buffer;
	ds->pebs_buffer_base = (unsigned long) cea;
	ds_update_cea(cea, buffer, bsiz, PAGE_KERNEL);
	ds->pebs_index = ds->pebs_buffer_base;
	max = x86_pmu.pebs_record_size * (bsiz / x86_pmu.pebs_record_size);
	ds->pebs_absolute_maximum = ds->pebs_buffer_base + max;
	return 0;
}

static void release_pebs_buffer(int cpu)
{
	struct cpu_hw_events *hwev = per_cpu_ptr(&cpu_hw_events, cpu);
	void *cea;

	if (!x86_pmu.pebs)
		return;

	kfree(per_cpu(insn_buffer, cpu));
	per_cpu(insn_buffer, cpu) = NULL;

	/* Clear the fixmap */
	cea = &get_cpu_entry_area(cpu)->cpu_debug_buffers.pebs_buffer;
	ds_clear_cea(cea, x86_pmu.pebs_buffer_size);
	dsfree_pages(hwev->ds_pebs_vaddr, x86_pmu.pebs_buffer_size);
	hwev->ds_pebs_vaddr = NULL;
}

static int alloc_bts_buffer(int cpu)
{
	struct cpu_hw_events *hwev = per_cpu_ptr(&cpu_hw_events, cpu);
	struct debug_store *ds = hwev->ds;
	void *buffer, *cea;
	int max;

	if (!x86_pmu.bts)
		return 0;

	buffer = dsalloc_pages(BTS_BUFFER_SIZE, GFP_KERNEL | __GFP_NOWARN, cpu);
	if (unlikely(!buffer)) {
		WARN_ONCE(1, "%s: BTS buffer allocation failure\n", __func__);
		return -ENOMEM;
	}
	hwev->ds_bts_vaddr = buffer;
	/* Update the fixmap */
	cea = &get_cpu_entry_area(cpu)->cpu_debug_buffers.bts_buffer;
	ds->bts_buffer_base = (unsigned long) cea;
	ds_update_cea(cea, buffer, BTS_BUFFER_SIZE, PAGE_KERNEL);
	ds->bts_index = ds->bts_buffer_base;
	max = BTS_BUFFER_SIZE / BTS_RECORD_SIZE;
	ds->bts_absolute_maximum = ds->bts_buffer_base +
					max * BTS_RECORD_SIZE;
	ds->bts_interrupt_threshold = ds->bts_absolute_maximum -
					(max / 16) * BTS_RECORD_SIZE;
	return 0;
}

static void release_bts_buffer(int cpu)
{
	struct cpu_hw_events *hwev = per_cpu_ptr(&cpu_hw_events, cpu);
	void *cea;

	if (!x86_pmu.bts)
		return;

	/* Clear the fixmap */
	cea = &get_cpu_entry_area(cpu)->cpu_debug_buffers.bts_buffer;
	ds_clear_cea(cea, BTS_BUFFER_SIZE);
	dsfree_pages(hwev->ds_bts_vaddr, BTS_BUFFER_SIZE);
	hwev->ds_bts_vaddr = NULL;
}

static int alloc_ds_buffer(int cpu)
{
	struct debug_store *ds = &get_cpu_entry_area(cpu)->cpu_debug_store;

	memset(ds, 0, sizeof(*ds));
	per_cpu(cpu_hw_events, cpu).ds = ds;
	return 0;
}

static void release_ds_buffer(int cpu)
{
	per_cpu(cpu_hw_events, cpu).ds = NULL;
}

void release_ds_buffers(void)
{
	int cpu;

	if (!x86_pmu.bts && !x86_pmu.pebs)
		return;

	for_each_possible_cpu(cpu)
		release_ds_buffer(cpu);

	for_each_possible_cpu(cpu) {
		/*
		 * Again, ignore errors from offline CPUs, they will no longer
		 * observe cpu_hw_events.ds and not program the DS_AREA when
		 * they come up.
		 */
		fini_debug_store_on_cpu(cpu);
	}

	for_each_possible_cpu(cpu) {
		release_pebs_buffer(cpu);
		release_bts_buffer(cpu);
	}
}

void reserve_ds_buffers(void)
{
	int bts_err = 0, pebs_err = 0;
	int cpu;

	x86_pmu.bts_active = 0;
	x86_pmu.pebs_active = 0;

	if (!x86_pmu.bts && !x86_pmu.pebs)
		return;

	if (!x86_pmu.bts)
		bts_err = 1;

	if (!x86_pmu.pebs)
		pebs_err = 1;

	for_each_possible_cpu(cpu) {
		if (alloc_ds_buffer(cpu)) {
			bts_err = 1;
			pebs_err = 1;
		}

		if (!bts_err && alloc_bts_buffer(cpu))
			bts_err = 1;

		if (!pebs_err && alloc_pebs_buffer(cpu))
			pebs_err = 1;

		if (bts_err && pebs_err)
			break;
	}

	if (bts_err) {
		for_each_possible_cpu(cpu)
			release_bts_buffer(cpu);
	}

	if (pebs_err) {
		for_each_possible_cpu(cpu)
			release_pebs_buffer(cpu);
	}

	if (bts_err && pebs_err) {
		for_each_possible_cpu(cpu)
			release_ds_buffer(cpu);
	} else {
		if (x86_pmu.bts && !bts_err)
			x86_pmu.bts_active = 1;

		if (x86_pmu.pebs && !pebs_err)
			x86_pmu.pebs_active = 1;

		for_each_possible_cpu(cpu) {
			/*
			 * Ignores wrmsr_on_cpu() errors for offline CPUs they
			 * will get this call through intel_pmu_cpu_starting().
			 */
			init_debug_store_on_cpu(cpu);
		}
	}
}

/*
 * BTS
 */

struct event_constraint bts_constraint =
	EVENT_CONSTRAINT(0, 1ULL << INTEL_PMC_IDX_FIXED_BTS, 0);

void intel_pmu_enable_bts(u64 config)
{
	unsigned long debugctlmsr;

	debugctlmsr = get_debugctlmsr();

	debugctlmsr |= DEBUGCTLMSR_TR;
	debugctlmsr |= DEBUGCTLMSR_BTS;
	if (config & ARCH_PERFMON_EVENTSEL_INT)
		debugctlmsr |= DEBUGCTLMSR_BTINT;

	if (!(config & ARCH_PERFMON_EVENTSEL_OS))
		debugctlmsr |= DEBUGCTLMSR_BTS_OFF_OS;

	if (!(config & ARCH_PERFMON_EVENTSEL_USR))
		debugctlmsr |= DEBUGCTLMSR_BTS_OFF_USR;

	update_debugctlmsr(debugctlmsr);
}

void intel_pmu_disable_bts(void)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	unsigned long debugctlmsr;

	if (!cpuc->ds)
		return;

	debugctlmsr = get_debugctlmsr();

	debugctlmsr &=
		~(DEBUGCTLMSR_TR | DEBUGCTLMSR_BTS | DEBUGCTLMSR_BTINT |
		  DEBUGCTLMSR_BTS_OFF_OS | DEBUGCTLMSR_BTS_OFF_USR);

	update_debugctlmsr(debugctlmsr);
}

int intel_pmu_drain_bts_buffer(void)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	struct debug_store *ds = cpuc->ds;
	struct bts_record {
		u64	from;
		u64	to;
		u64	flags;
	};
	struct perf_event *event = cpuc->events[INTEL_PMC_IDX_FIXED_BTS];
	struct bts_record *at, *base, *top;
	struct perf_output_handle handle;
	struct perf_event_header header;
	struct perf_sample_data data;
	unsigned long skip = 0;
	struct pt_regs regs;

	if (!event)
		return 0;

	if (!x86_pmu.bts_active)
		return 0;

	base = (struct bts_record *)(unsigned long)ds->bts_buffer_base;
	top  = (struct bts_record *)(unsigned long)ds->bts_index;

	if (top <= base)
		return 0;

	memset(&regs, 0, sizeof(regs));

	ds->bts_index = ds->bts_buffer_base;

	perf_sample_data_init(&data, 0, event->hw.last_period);

	/*
	 * BTS leaks kernel addresses in branches across the cpl boundary,
	 * such as traps or system calls, so unless the user is asking for
	 * kernel tracing (and right now it's not possible), we'd need to
	 * filter them out. But first we need to count how many of those we
	 * have in the current batch. This is an extra O(n) pass, however,
	 * it's much faster than the other one especially considering that
	 * n <= 2560 (BTS_BUFFER_SIZE / BTS_RECORD_SIZE * 15/16; see the
	 * alloc_bts_buffer()).
	 */
	for (at = base; at < top; at++) {
		/*
		 * Note that right now *this* BTS code only works if
		 * attr::exclude_kernel is set, but let's keep this extra
		 * check here in case that changes.
		 */
		if (event->attr.exclude_kernel &&
		    (kernel_ip(at->from) || kernel_ip(at->to)))
			skip++;
	}

	/*
	 * Prepare a generic sample, i.e. fill in the invariant fields.
	 * We will overwrite the from and to address before we output
	 * the sample.
	 */
	rcu_read_lock();
	perf_prepare_sample(&header, &data, event, &regs);

	if (perf_output_begin(&handle, event, header.size *
			      (top - base - skip)))
		goto unlock;

	for (at = base; at < top; at++) {
		/* Filter out any records that contain kernel addresses. */
		if (event->attr.exclude_kernel &&
		    (kernel_ip(at->from) || kernel_ip(at->to)))
			continue;

		data.ip		= at->from;
		data.addr	= at->to;

		perf_output_sample(&handle, &header, &data, event);
	}

	perf_output_end(&handle);

	/* There's new data available. */
	event->hw.interrupts++;
	event->pending_kill = POLL_IN;
unlock:
	rcu_read_unlock();
	return 1;
}

static inline void intel_pmu_drain_pebs_buffer(void)
{
	struct pt_regs regs;

	x86_pmu.drain_pebs(&regs);
}

/*
 * PEBS
 */
struct event_constraint intel_core2_pebs_event_constraints[] = {
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x00c0, 0x1), /* INST_RETIRED.ANY */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0xfec1, 0x1), /* X87_OPS_RETIRED.ANY */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x00c5, 0x1), /* BR_INST_RETIRED.MISPRED */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x1fc7, 0x1), /* SIMD_INST_RETURED.ANY */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xcb, 0x1),    /* MEM_LOAD_RETIRED.* */
	/* INST_RETIRED.ANY_P, inv=1, cmask=16 (cycles:p). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108000c0, 0x01),
	EVENT_CONSTRAINT_END
};

struct event_constraint intel_atom_pebs_event_constraints[] = {
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x00c0, 0x1), /* INST_RETIRED.ANY */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x00c5, 0x1), /* MISPREDICTED_BRANCH_RETIRED */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xcb, 0x1),    /* MEM_LOAD_RETIRED.* */
	/* INST_RETIRED.ANY_P, inv=1, cmask=16 (cycles:p). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108000c0, 0x01),
	/* Allow all events as PEBS with no flags */
	INTEL_ALL_EVENT_CONSTRAINT(0, 0x1),
	EVENT_CONSTRAINT_END
};

struct event_constraint intel_slm_pebs_event_constraints[] = {
	/* INST_RETIRED.ANY_P, inv=1, cmask=16 (cycles:p). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108000c0, 0x1),
	/* Allow all events as PEBS with no flags */
	INTEL_ALL_EVENT_CONSTRAINT(0, 0x1),
	EVENT_CONSTRAINT_END
};

struct event_constraint intel_glm_pebs_event_constraints[] = {
	/* Allow all events as PEBS with no flags */
	INTEL_ALL_EVENT_CONSTRAINT(0, 0x1),
	EVENT_CONSTRAINT_END
};

struct event_constraint intel_nehalem_pebs_event_constraints[] = {
	INTEL_PLD_CONSTRAINT(0x100b, 0xf),      /* MEM_INST_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT(0x0f, 0xf),    /* MEM_UNCORE_RETIRED.* */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x010c, 0xf), /* MEM_STORE_RETIRED.DTLB_MISS */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xc0, 0xf),    /* INST_RETIRED.ANY */
	INTEL_EVENT_CONSTRAINT(0xc2, 0xf),    /* UOPS_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xc4, 0xf),    /* BR_INST_RETIRED.* */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x02c5, 0xf), /* BR_MISP_RETIRED.NEAR_CALL */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xc7, 0xf),    /* SSEX_UOPS_RETIRED.* */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x20c8, 0xf), /* ITLB_MISS_RETIRED */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xcb, 0xf),    /* MEM_LOAD_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xf7, 0xf),    /* FP_ASSIST.* */
	/* INST_RETIRED.ANY_P, inv=1, cmask=16 (cycles:p). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108000c0, 0x0f),
	EVENT_CONSTRAINT_END
};

struct event_constraint intel_westmere_pebs_event_constraints[] = {
	INTEL_PLD_CONSTRAINT(0x100b, 0xf),      /* MEM_INST_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT(0x0f, 0xf),    /* MEM_UNCORE_RETIRED.* */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x010c, 0xf), /* MEM_STORE_RETIRED.DTLB_MISS */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xc0, 0xf),    /* INSTR_RETIRED.* */
	INTEL_EVENT_CONSTRAINT(0xc2, 0xf),    /* UOPS_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xc4, 0xf),    /* BR_INST_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xc5, 0xf),    /* BR_MISP_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xc7, 0xf),    /* SSEX_UOPS_RETIRED.* */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x20c8, 0xf), /* ITLB_MISS_RETIRED */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xcb, 0xf),    /* MEM_LOAD_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT(0xf7, 0xf),    /* FP_ASSIST.* */
	/* INST_RETIRED.ANY_P, inv=1, cmask=16 (cycles:p). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108000c0, 0x0f),
	EVENT_CONSTRAINT_END
};

struct event_constraint intel_snb_pebs_event_constraints[] = {
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x01c0, 0x2), /* INST_RETIRED.PRECDIST */
	INTEL_PLD_CONSTRAINT(0x01cd, 0x8),    /* MEM_TRANS_RETIRED.LAT_ABOVE_THR */
	INTEL_PST_CONSTRAINT(0x02cd, 0x8),    /* MEM_TRANS_RETIRED.PRECISE_STORES */
	/* UOPS_RETIRED.ALL, inv=1, cmask=16 (cycles:p). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108001c2, 0xf),
        INTEL_EXCLEVT_CONSTRAINT(0xd0, 0xf),    /* MEM_UOP_RETIRED.* */
        INTEL_EXCLEVT_CONSTRAINT(0xd1, 0xf),    /* MEM_LOAD_UOPS_RETIRED.* */
        INTEL_EXCLEVT_CONSTRAINT(0xd2, 0xf),    /* MEM_LOAD_UOPS_LLC_HIT_RETIRED.* */
        INTEL_EXCLEVT_CONSTRAINT(0xd3, 0xf),    /* MEM_LOAD_UOPS_LLC_MISS_RETIRED.* */
	/* Allow all events as PEBS with no flags */
	INTEL_ALL_EVENT_CONSTRAINT(0, 0xf),
	EVENT_CONSTRAINT_END
};

struct event_constraint intel_ivb_pebs_event_constraints[] = {
        INTEL_FLAGS_UEVENT_CONSTRAINT(0x01c0, 0x2), /* INST_RETIRED.PRECDIST */
        INTEL_PLD_CONSTRAINT(0x01cd, 0x8),    /* MEM_TRANS_RETIRED.LAT_ABOVE_THR */
	INTEL_PST_CONSTRAINT(0x02cd, 0x8),    /* MEM_TRANS_RETIRED.PRECISE_STORES */
	/* UOPS_RETIRED.ALL, inv=1, cmask=16 (cycles:p). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108001c2, 0xf),
	/* INST_RETIRED.PREC_DIST, inv=1, cmask=16 (cycles:ppp). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108001c0, 0x2),
	INTEL_EXCLEVT_CONSTRAINT(0xd0, 0xf),    /* MEM_UOP_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd1, 0xf),    /* MEM_LOAD_UOPS_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd2, 0xf),    /* MEM_LOAD_UOPS_LLC_HIT_RETIRED.* */
	INTEL_EXCLEVT_CONSTRAINT(0xd3, 0xf),    /* MEM_LOAD_UOPS_LLC_MISS_RETIRED.* */
	/* Allow all events as PEBS with no flags */
	INTEL_ALL_EVENT_CONSTRAINT(0, 0xf),
        EVENT_CONSTRAINT_END
};

struct event_constraint intel_hsw_pebs_event_constraints[] = {
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x01c0, 0x2), /* INST_RETIRED.PRECDIST */
	INTEL_PLD_CONSTRAINT(0x01cd, 0xf),    /* MEM_TRANS_RETIRED.* */
	/* UOPS_RETIRED.ALL, inv=1, cmask=16 (cycles:p). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108001c2, 0xf),
	/* INST_RETIRED.PREC_DIST, inv=1, cmask=16 (cycles:ppp). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108001c0, 0x2),
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_NA(0x01c2, 0xf), /* UOPS_RETIRED.ALL */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_XLD(0x11d0, 0xf), /* MEM_UOPS_RETIRED.STLB_MISS_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_XLD(0x21d0, 0xf), /* MEM_UOPS_RETIRED.LOCK_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_XLD(0x41d0, 0xf), /* MEM_UOPS_RETIRED.SPLIT_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_XLD(0x81d0, 0xf), /* MEM_UOPS_RETIRED.ALL_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_XST(0x12d0, 0xf), /* MEM_UOPS_RETIRED.STLB_MISS_STORES */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_XST(0x42d0, 0xf), /* MEM_UOPS_RETIRED.SPLIT_STORES */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_XST(0x82d0, 0xf), /* MEM_UOPS_RETIRED.ALL_STORES */
	INTEL_FLAGS_EVENT_CONSTRAINT_DATALA_XLD(0xd1, 0xf),    /* MEM_LOAD_UOPS_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT_DATALA_XLD(0xd2, 0xf),    /* MEM_LOAD_UOPS_L3_HIT_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT_DATALA_XLD(0xd3, 0xf),    /* MEM_LOAD_UOPS_L3_MISS_RETIRED.* */
	/* Allow all events as PEBS with no flags */
	INTEL_ALL_EVENT_CONSTRAINT(0, 0xf),
	EVENT_CONSTRAINT_END
};

struct event_constraint intel_bdw_pebs_event_constraints[] = {
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x01c0, 0x2), /* INST_RETIRED.PRECDIST */
	INTEL_PLD_CONSTRAINT(0x01cd, 0xf),    /* MEM_TRANS_RETIRED.* */
	/* UOPS_RETIRED.ALL, inv=1, cmask=16 (cycles:p). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108001c2, 0xf),
	/* INST_RETIRED.PREC_DIST, inv=1, cmask=16 (cycles:ppp). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108001c0, 0x2),
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_NA(0x01c2, 0xf), /* UOPS_RETIRED.ALL */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_LD(0x11d0, 0xf), /* MEM_UOPS_RETIRED.STLB_MISS_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_LD(0x21d0, 0xf), /* MEM_UOPS_RETIRED.LOCK_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_LD(0x41d0, 0xf), /* MEM_UOPS_RETIRED.SPLIT_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_LD(0x81d0, 0xf), /* MEM_UOPS_RETIRED.ALL_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_ST(0x12d0, 0xf), /* MEM_UOPS_RETIRED.STLB_MISS_STORES */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_ST(0x42d0, 0xf), /* MEM_UOPS_RETIRED.SPLIT_STORES */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_ST(0x82d0, 0xf), /* MEM_UOPS_RETIRED.ALL_STORES */
	INTEL_FLAGS_EVENT_CONSTRAINT_DATALA_LD(0xd1, 0xf),    /* MEM_LOAD_UOPS_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT_DATALA_LD(0xd2, 0xf),    /* MEM_LOAD_UOPS_L3_HIT_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT_DATALA_LD(0xd3, 0xf),    /* MEM_LOAD_UOPS_L3_MISS_RETIRED.* */
	/* Allow all events as PEBS with no flags */
	INTEL_ALL_EVENT_CONSTRAINT(0, 0xf),
	EVENT_CONSTRAINT_END
};


struct event_constraint intel_skl_pebs_event_constraints[] = {
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x1c0, 0x2),	/* INST_RETIRED.PREC_DIST */
	/* INST_RETIRED.PREC_DIST, inv=1, cmask=16 (cycles:ppp). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108001c0, 0x2),
	/* INST_RETIRED.TOTAL_CYCLES_PS (inv=1, cmask=16) (cycles:p). */
	INTEL_FLAGS_UEVENT_CONSTRAINT(0x108000c0, 0x0f),
	INTEL_PLD_CONSTRAINT(0x1cd, 0xf),		      /* MEM_TRANS_RETIRED.* */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_LD(0x11d0, 0xf), /* MEM_INST_RETIRED.STLB_MISS_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_ST(0x12d0, 0xf), /* MEM_INST_RETIRED.STLB_MISS_STORES */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_LD(0x21d0, 0xf), /* MEM_INST_RETIRED.LOCK_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_ST(0x22d0, 0xf), /* MEM_INST_RETIRED.LOCK_STORES */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_LD(0x41d0, 0xf), /* MEM_INST_RETIRED.SPLIT_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_ST(0x42d0, 0xf), /* MEM_INST_RETIRED.SPLIT_STORES */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_LD(0x81d0, 0xf), /* MEM_INST_RETIRED.ALL_LOADS */
	INTEL_FLAGS_UEVENT_CONSTRAINT_DATALA_ST(0x82d0, 0xf), /* MEM_INST_RETIRED.ALL_STORES */
	INTEL_FLAGS_EVENT_CONSTRAINT_DATALA_LD(0xd1, 0xf),    /* MEM_LOAD_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT_DATALA_LD(0xd2, 0xf),    /* MEM_LOAD_L3_HIT_RETIRED.* */
	INTEL_FLAGS_EVENT_CONSTRAINT_DATALA_LD(0xd3, 0xf),    /* MEM_LOAD_L3_MISS_RETIRED.* */
	/* Allow all events as PEBS with no flags */
	INTEL_ALL_EVENT_CONSTRAINT(0, 0xf),
	EVENT_CONSTRAINT_END
};

struct event_constraint *intel_pebs_constraints(struct perf_event *event)
{
	struct event_constraint *c;

	if (!event->attr.precise_ip)
		return NULL;

	if (x86_pmu.pebs_constraints) {
		for_each_event_constraint(c, x86_pmu.pebs_constraints) {
			if ((event->hw.config & c->cmask) == c->code) {
				event->hw.flags |= c->flags;
				return c;
			}
		}
	}

	/*
	 * Extended PEBS support
	 * Makes the PEBS code search the normal constraints.
	 */
	if (x86_pmu.flags & PMU_FL_PEBS_ALL)
		return NULL;

	return &emptyconstraint;
}

/*
 * We need the sched_task callback even for per-cpu events when we use
 * the large interrupt threshold, such that we can provide PID and TID
 * to PEBS samples.
 */
static inline bool pebs_needs_sched_cb(struct cpu_hw_events *cpuc)
{
	return cpuc->n_pebs && (cpuc->n_pebs == cpuc->n_large_pebs);
}

void intel_pmu_pebs_sched_task(struct perf_event_context *ctx, bool sched_in)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);

	if (!sched_in && pebs_needs_sched_cb(cpuc))
		intel_pmu_drain_pebs_buffer();
}

static inline void pebs_update_threshold(struct cpu_hw_events *cpuc)
{
	struct debug_store *ds = cpuc->ds;
	u64 threshold;
	int reserved;

	if (x86_pmu.flags & PMU_FL_PEBS_ALL)
		reserved = x86_pmu.max_pebs_events + x86_pmu.num_counters_fixed;
	else
		reserved = x86_pmu.max_pebs_events;

	if (cpuc->n_pebs == cpuc->n_large_pebs) {
		threshold = ds->pebs_absolute_maximum -
			reserved * x86_pmu.pebs_record_size;
	} else {
		threshold = ds->pebs_buffer_base + x86_pmu.pebs_record_size;
	}

	ds->pebs_interrupt_threshold = threshold;
}

static void
pebs_update_state(bool needed_cb, struct cpu_hw_events *cpuc, struct pmu *pmu)
{
	/*
	 * Make sure we get updated with the first PEBS
	 * event. It will trigger also during removal, but
	 * that does not hurt:
	 */
	bool update = cpuc->n_pebs == 1;

	if (needed_cb != pebs_needs_sched_cb(cpuc)) {
		if (!needed_cb)
			perf_sched_cb_inc(pmu);
		else
			perf_sched_cb_dec(pmu);

		update = true;
	}

	if (update)
		pebs_update_threshold(cpuc);
}

void intel_pmu_pebs_add(struct perf_event *event)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	struct hw_perf_event *hwc = &event->hw;
	bool needed_cb = pebs_needs_sched_cb(cpuc);

	cpuc->n_pebs++;
	if (hwc->flags & PERF_X86_EVENT_LARGE_PEBS)
		cpuc->n_large_pebs++;

	pebs_update_state(needed_cb, cpuc, event->ctx->pmu);
}

void intel_pmu_pebs_enable(struct perf_event *event)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	struct hw_perf_event *hwc = &event->hw;
	struct debug_store *ds = cpuc->ds;

	hwc->config &= ~ARCH_PERFMON_EVENTSEL_INT;

	cpuc->pebs_enabled |= 1ULL << hwc->idx;

	if (event->hw.flags & PERF_X86_EVENT_PEBS_LDLAT)
		cpuc->pebs_enabled |= 1ULL << (hwc->idx + 32);
	else if (event->hw.flags & PERF_X86_EVENT_PEBS_ST)
		cpuc->pebs_enabled |= 1ULL << 63;

	/*
	 * Use auto-reload if possible to save a MSR write in the PMI.
	 * This must be done in pmu::start(), because PERF_EVENT_IOC_PERIOD.
	 */
	if (hwc->flags & PERF_X86_EVENT_AUTO_RELOAD) {
		unsigned int idx = hwc->idx;

		if (idx >= INTEL_PMC_IDX_FIXED)
			idx = MAX_PEBS_EVENTS + (idx - INTEL_PMC_IDX_FIXED);
		ds->pebs_event_reset[idx] =
			(u64)(-hwc->sample_period) & x86_pmu.cntval_mask;
	} else {
		ds->pebs_event_reset[hwc->idx] = 0;
	}
}

void intel_pmu_pebs_del(struct perf_event *event)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	struct hw_perf_event *hwc = &event->hw;
	bool needed_cb = pebs_needs_sched_cb(cpuc);

	cpuc->n_pebs--;
	if (hwc->flags & PERF_X86_EVENT_LARGE_PEBS)
		cpuc->n_large_pebs--;

	pebs_update_state(needed_cb, cpuc, event->ctx->pmu);
}

void intel_pmu_pebs_disable(struct perf_event *event)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	struct hw_perf_event *hwc = &event->hw;

	if (cpuc->n_pebs == cpuc->n_large_pebs)
		intel_pmu_drain_pebs_buffer();

	cpuc->pebs_enabled &= ~(1ULL << hwc->idx);

	if (event->hw.flags & PERF_X86_EVENT_PEBS_LDLAT)
		cpuc->pebs_enabled &= ~(1ULL << (hwc->idx + 32));
	else if (event->hw.flags & PERF_X86_EVENT_PEBS_ST)
		cpuc->pebs_enabled &= ~(1ULL << 63);

	if (cpuc->enabled)
		wrmsrl(MSR_IA32_PEBS_ENABLE, cpuc->pebs_enabled);

	hwc->config |= ARCH_PERFMON_EVENTSEL_INT;
}

void intel_pmu_pebs_enable_all(void)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);

	if (cpuc->pebs_enabled)
		wrmsrl(MSR_IA32_PEBS_ENABLE, cpuc->pebs_enabled);
}

void intel_pmu_pebs_disable_all(void)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);

	if (cpuc->pebs_enabled)
		wrmsrl(MSR_IA32_PEBS_ENABLE, 0);
}

static int intel_pmu_pebs_fixup_ip(struct pt_regs *regs)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	unsigned long from = cpuc->lbr_entries[0].from;
	unsigned long old_to, to = cpuc->lbr_entries[0].to;
	unsigned long ip = regs->ip;
	int is_64bit = 0;
	void *kaddr;
	int size;

	/*
	 * We don't need to fixup if the PEBS assist is fault like
	 */
	if (!x86_pmu.intel_cap.pebs_trap)
		return 1;

	/*
	 * No LBR entry, no basic block, no rewinding
	 */
	if (!cpuc->lbr_stack.nr || !from || !to)
		return 0;

	/*
	 * Basic blocks should never cross user/kernel boundaries
	 */
	if (kernel_ip(ip) != kernel_ip(to))
		return 0;

	/*
	 * unsigned math, either ip is before the start (impossible) or
	 * the basic block is larger than 1 page (sanity)
	 */
	if ((ip - to) > PEBS_FIXUP_SIZE)
		return 0;

	/*
	 * We sampled a branch insn, rewind using the LBR stack
	 */
	if (ip == to) {
		set_linear_ip(regs, from);
		return 1;
	}

	size = ip - to;
	if (!kernel_ip(ip)) {
		int bytes;
		u8 *buf = this_cpu_read(insn_buffer);

		/* 'size' must fit our buffer, see above */
		bytes = copy_from_user_nmi(buf, (void __user *)to, size);
		if (bytes != 0)
			return 0;

		kaddr = buf;
	} else {
		kaddr = (void *)to;
	}

	do {
		struct insn insn;

		old_to = to;

#ifdef CONFIG_X86_64
		is_64bit = kernel_ip(to) || !test_thread_flag(TIF_IA32);
#endif
		insn_init(&insn, kaddr, size, is_64bit);
		insn_get_length(&insn);
		/*
		 * Make sure there was not a problem decoding the
		 * instruction and getting the length.  This is
		 * doubly important because we have an infinite
		 * loop if insn.length=0.
		 */
		if (!insn.length)
			break;

		to += insn.length;
		kaddr += insn.length;
		size -= insn.length;
	} while (to < ip);

	if (to == ip) {
		set_linear_ip(regs, old_to);
		return 1;
	}

	/*
	 * Even though we decoded the basic block, the instruction stream
	 * never matched the given IP, either the TO or the IP got corrupted.
	 */
	return 0;
}

static inline u64 intel_hsw_weight(struct pebs_record_skl *pebs)
{
	if (pebs->tsx_tuning) {
		union hsw_tsx_tuning tsx = { .value = pebs->tsx_tuning };
		return tsx.cycles_last_block;
	}
	return 0;
}

static inline u64 intel_hsw_transaction(struct pebs_record_skl *pebs)
{
	u64 txn = (pebs->tsx_tuning & PEBS_HSW_TSX_FLAGS) >> 32;

	/* For RTM XABORTs also log the abort code from AX */
	if ((txn & PERF_TXN_TRANSACTION) && (pebs->ax & 1))
		txn |= ((pebs->ax >> 24) & 0xff) << PERF_TXN_ABORT_SHIFT;
	return txn;
}

static void setup_pebs_sample_data(struct perf_event *event,
				   struct pt_regs *iregs, void *__pebs,
				   struct perf_sample_data *data,
				   struct pt_regs *regs)
{
#define PERF_X86_EVENT_PEBS_HSW_PREC \
		(PERF_X86_EVENT_PEBS_ST_HSW | \
		 PERF_X86_EVENT_PEBS_LD_HSW | \
		 PERF_X86_EVENT_PEBS_NA_HSW)
	/*
	 * We cast to the biggest pebs_record but are careful not to
	 * unconditionally access the 'extra' entries.
	 */
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	struct pebs_record_skl *pebs = __pebs;
	u64 sample_type;
	int fll, fst, dsrc;
	int fl = event->hw.flags;

	if (pebs == NULL)
		return;

	sample_type = event->attr.sample_type;
	dsrc = sample_type & PERF_SAMPLE_DATA_SRC;

	fll = fl & PERF_X86_EVENT_PEBS_LDLAT;
	fst = fl & (PERF_X86_EVENT_PEBS_ST | PERF_X86_EVENT_PEBS_HSW_PREC);

	perf_sample_data_init(data, 0, event->hw.last_period);

	data->period = event->hw.last_period;

	/*
	 * Use latency for weight (only avail with PEBS-LL)
	 */
	if (fll && (sample_type & PERF_SAMPLE_WEIGHT))
		data->weight = pebs->lat;

	/*
	 * data.data_src encodes the data source
	 */
	if (dsrc) {
		u64 val = PERF_MEM_NA;
		if (fll)
			val = load_latency_data(pebs->dse);
		else if (fst && (fl & PERF_X86_EVENT_PEBS_HSW_PREC))
			val = precise_datala_hsw(event, pebs->dse);
		else if (fst)
			val = precise_store_data(pebs->dse);
		data->data_src.val = val;
	}

	/*
	 * We must however always use iregs for the unwinder to stay sane; the
	 * record BP,SP,IP can point into thin air when the record is from a
	 * previous PMI context or an (I)RET happend between the record and
	 * PMI.
	 */
	if (sample_type & PERF_SAMPLE_CALLCHAIN)
		data->callchain = perf_callchain(event, iregs);

	/*
	 * We use the interrupt regs as a base because the PEBS record does not
	 * contain a full regs set, specifically it seems to lack segment
	 * descriptors, which get used by things like user_mode().
	 *
	 * In the simple case fix up only the IP for PERF_SAMPLE_IP.
	 */
	*regs = *iregs;

	/*
	 * Initialize regs_>flags from PEBS,
	 * Clear exact bit (which uses x86 EFLAGS Reserved bit 3),
	 * i.e., do not rely on it being zero:
	 */
	regs->flags = pebs->flags & ~PERF_EFLAGS_EXACT;

	if (sample_type & PERF_SAMPLE_REGS_INTR) {
		regs->ax = pebs->ax;
		regs->bx = pebs->bx;
		regs->cx = pebs->cx;
		regs->dx = pebs->dx;
		regs->si = pebs->si;
		regs->di = pebs->di;

		regs->bp = pebs->bp;
		regs->sp = pebs->sp;

#ifndef CONFIG_X86_32
		regs->r8 = pebs->r8;
		regs->r9 = pebs->r9;
		regs->r10 = pebs->r10;
		regs->r11 = pebs->r11;
		regs->r12 = pebs->r12;
		regs->r13 = pebs->r13;
		regs->r14 = pebs->r14;
		regs->r15 = pebs->r15;
#endif
	}

	if (event->attr.precise_ip > 1) {
		/*
		 * Haswell and later processors have an 'eventing IP'
		 * (real IP) which fixes the off-by-1 skid in hardware.
		 * Use it when precise_ip >= 2 :
		 */
		if (x86_pmu.intel_cap.pebs_format >= 2) {
			set_linear_ip(regs, pebs->real_ip);
			regs->flags |= PERF_EFLAGS_EXACT;
		} else {
			/* Otherwise, use PEBS off-by-1 IP: */
			set_linear_ip(regs, pebs->ip);

			/*
			 * With precise_ip >= 2, try to fix up the off-by-1 IP
			 * using the LBR. If successful, the fixup function
			 * corrects regs->ip and calls set_linear_ip() on regs:
			 */
			if (intel_pmu_pebs_fixup_ip(regs))
				regs->flags |= PERF_EFLAGS_EXACT;
		}
	} else {
		/*
		 * When precise_ip == 1, return the PEBS off-by-1 IP,
		 * no fixup attempted:
		 */
		set_linear_ip(regs, pebs->ip);
	}


	if ((sample_type & (PERF_SAMPLE_ADDR | PERF_SAMPLE_PHYS_ADDR)) &&
	    x86_pmu.intel_cap.pebs_format >= 1)
		data->addr = pebs->dla;

	if (x86_pmu.intel_cap.pebs_format >= 2) {
		/* Only set the TSX weight when no memory weight. */
		if ((sample_type & PERF_SAMPLE_WEIGHT) && !fll)
			data->weight = intel_hsw_weight(pebs);

		if (sample_type & PERF_SAMPLE_TRANSACTION)
			data->txn = intel_hsw_transaction(pebs);
	}

	/*
	 * v3 supplies an accurate time stamp, so we use that
	 * for the time stamp.
	 *
	 * We can only do this for the default trace clock.
	 */
	if (x86_pmu.intel_cap.pebs_format >= 3 &&
		event->attr.use_clockid == 0)
		data->time = native_sched_clock_from_tsc(pebs->tsc);

	if (has_branch_stack(event))
		data->br_stack = &cpuc->lbr_stack;
}

static inline void *
get_next_pebs_record_by_bit(void *base, void *top, int bit)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	void *at;
	u64 pebs_status;

	/*
	 * fmt0 does not have a status bitfield (does not use
	 * perf_record_nhm format)
	 */
	if (x86_pmu.intel_cap.pebs_format < 1)
		return base;

	if (base == NULL)
		return NULL;

	for (at = base; at < top; at += x86_pmu.pebs_record_size) {
		struct pebs_record_nhm *p = at;

		if (test_bit(bit, (unsigned long *)&p->status)) {
			/* PEBS v3 has accurate status bits */
			if (x86_pmu.intel_cap.pebs_format >= 3)
				return at;

			if (p->status == (1 << bit))
				return at;

			/* clear non-PEBS bit and re-check */
			pebs_status = p->status & cpuc->pebs_enabled;
			pebs_status &= PEBS_COUNTER_MASK;
			if (pebs_status == (1 << bit))
				return at;
		}
	}
	return NULL;
}

void intel_pmu_auto_reload_read(struct perf_event *event)
{
	WARN_ON(!(event->hw.flags & PERF_X86_EVENT_AUTO_RELOAD));

	perf_pmu_disable(event->pmu);
	intel_pmu_drain_pebs_buffer();
	perf_pmu_enable(event->pmu);
}

/*
 * Special variant of intel_pmu_save_and_restart() for auto-reload.
 */
static int
intel_pmu_save_and_restart_reload(struct perf_event *event, int count)
{
	struct hw_perf_event *hwc = &event->hw;
	int shift = 64 - x86_pmu.cntval_bits;
	u64 period = hwc->sample_period;
	u64 prev_raw_count, new_raw_count;
	s64 new, old;

	WARN_ON(!period);

	/*
	 * drain_pebs() only happens when the PMU is disabled.
	 */
	WARN_ON(this_cpu_read(cpu_hw_events.enabled));

	prev_raw_count = local64_read(&hwc->prev_count);
	rdpmcl(hwc->event_base_rdpmc, new_raw_count);
	local64_set(&hwc->prev_count, new_raw_count);

	/*
	 * Since the counter increments a negative counter value and
	 * overflows on the sign switch, giving the interval:
	 *
	 *   [-period, 0]
	 *
	 * the difference between two consequtive reads is:
	 *
	 *   A) value2 - value1;
	 *      when no overflows have happened in between,
	 *
	 *   B) (0 - value1) + (value2 - (-period));
	 *      when one overflow happened in between,
	 *
	 *   C) (0 - value1) + (n - 1) * (period) + (value2 - (-period));
	 *      when @n overflows happened in between.
	 *
	 * Here A) is the obvious difference, B) is the extension to the
	 * discrete interval, where the first term is to the top of the
	 * interval and the second term is from the bottom of the next
	 * interval and C) the extension to multiple intervals, where the
	 * middle term is the whole intervals covered.
	 *
	 * An equivalent of C, by reduction, is:
	 *
	 *   value2 - value1 + n * period
	 */
	new = ((s64)(new_raw_count << shift) >> shift);
	old = ((s64)(prev_raw_count << shift) >> shift);
	local64_add(new - old + count * period, &event->count);

	local64_set(&hwc->period_left, -new);

	perf_event_update_userpage(event);

	return 0;
}

static void __intel_pmu_pebs_event(struct perf_event *event,
				   struct pt_regs *iregs,
				   void *base, void *top,
				   int bit, int count)
{
	struct hw_perf_event *hwc = &event->hw;
	struct perf_sample_data data;
	struct pt_regs regs;
	void *at = get_next_pebs_record_by_bit(base, top, bit);

	if (hwc->flags & PERF_X86_EVENT_AUTO_RELOAD) {
		/*
		 * Now, auto-reload is only enabled in fixed period mode.
		 * The reload value is always hwc->sample_period.
		 * May need to change it, if auto-reload is enabled in
		 * freq mode later.
		 */
		intel_pmu_save_and_restart_reload(event, count);
	} else if (!intel_pmu_save_and_restart(event))
		return;

	while (count > 1) {
		setup_pebs_sample_data(event, iregs, at, &data, &regs);
		perf_event_output(event, &data, &regs);
		at += x86_pmu.pebs_record_size;
		at = get_next_pebs_record_by_bit(at, top, bit);
		count--;
	}

	setup_pebs_sample_data(event, iregs, at, &data, &regs);

	/*
	 * All but the last records are processed.
	 * The last one is left to be able to call the overflow handler.
	 */
	if (perf_event_overflow(event, &data, &regs)) {
		x86_pmu_stop(event, 0);
		return;
	}

}

static void intel_pmu_drain_pebs_core(struct pt_regs *iregs)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	struct debug_store *ds = cpuc->ds;
	struct perf_event *event = cpuc->events[0]; /* PMC0 only */
	struct pebs_record_core *at, *top;
	int n;

	if (!x86_pmu.pebs_active)
		return;

	at  = (struct pebs_record_core *)(unsigned long)ds->pebs_buffer_base;
	top = (struct pebs_record_core *)(unsigned long)ds->pebs_index;

	/*
	 * Whatever else happens, drain the thing
	 */
	ds->pebs_index = ds->pebs_buffer_base;

	if (!test_bit(0, cpuc->active_mask))
		return;

	WARN_ON_ONCE(!event);

	if (!event->attr.precise_ip)
		return;

	n = top - at;
	if (n <= 0) {
		if (event->hw.flags & PERF_X86_EVENT_AUTO_RELOAD)
			intel_pmu_save_and_restart_reload(event, 0);
		return;
	}

	__intel_pmu_pebs_event(event, iregs, at, top, 0, n);
}

static void intel_pmu_drain_pebs_nhm(struct pt_regs *iregs)
{
	struct cpu_hw_events *cpuc = this_cpu_ptr(&cpu_hw_events);
	struct debug_store *ds = cpuc->ds;
	struct perf_event *event;
	void *base, *at, *top;
	short counts[INTEL_PMC_IDX_FIXED + MAX_FIXED_PEBS_EVENTS] = {};
	short error[INTEL_PMC_IDX_FIXED + MAX_FIXED_PEBS_EVENTS] = {};
	int bit, i, size;
	u64 mask;

	if (!x86_pmu.pebs_active)
		return;

	base = (struct pebs_record_nhm *)(unsigned long)ds->pebs_buffer_base;
	top = (struct pebs_record_nhm *)(unsigned long)ds->pebs_index;

	ds->pebs_index = ds->pebs_buffer_base;

	mask = (1ULL << x86_pmu.max_pebs_events) - 1;
	size = x86_pmu.max_pebs_events;
	if (x86_pmu.flags & PMU_FL_PEBS_ALL) {
		mask |= ((1ULL << x86_pmu.num_counters_fixed) - 1) << INTEL_PMC_IDX_FIXED;
		size = INTEL_PMC_IDX_FIXED + x86_pmu.num_counters_fixed;
	}

	if (unlikely(base >= top)) {
		/*
		 * The drain_pebs() could be called twice in a short period
		 * for auto-reload event in pmu::read(). There are no
		 * overflows have happened in between.
		 * It needs to call intel_pmu_save_and_restart_reload() to
		 * update the event->count for this case.
		 */
		for_each_set_bit(bit, (unsigned long *)&cpuc->pebs_enabled,
				 size) {
			event = cpuc->events[bit];
			if (event->hw.flags & PERF_X86_EVENT_AUTO_RELOAD)
				intel_pmu_save_and_restart_reload(event, 0);
		}
		return;
	}

	for (at = base; at < top; at += x86_pmu.pebs_record_size) {
		struct pebs_record_nhm *p = at;
		u64 pebs_status;

		pebs_status = p->status & cpuc->pebs_enabled;
		pebs_status &= mask;

		/* PEBS v3 has more accurate status bits */
		if (x86_pmu.intel_cap.pebs_format >= 3) {
			for_each_set_bit(bit, (unsigned long *)&pebs_status,
					 size)
				counts[bit]++;

			continue;
		}

		/*
		 * On some CPUs the PEBS status can be zero when PEBS is
		 * racing with clearing of GLOBAL_STATUS.
		 *
		 * Normally we would drop that record, but in the
		 * case when there is only a single active PEBS event
		 * we can assume it's for that event.
		 */
		if (!pebs_status && cpuc->pebs_enabled &&
			!(cpuc->pebs_enabled & (cpuc->pebs_enabled-1)))
			pebs_status = p->status = cpuc->pebs_enabled;

		bit = find_first_bit((unsigned long *)&pebs_status,
					x86_pmu.max_pebs_events);
		if (bit >= x86_pmu.max_pebs_events)
			continue;

		/*
		 * The PEBS hardware does not deal well with the situation
		 * when events happen near to each other and multiple bits
		 * are set. But it should happen rarely.
		 *
		 * If these events include one PEBS and multiple non-PEBS
		 * events, it doesn't impact PEBS record. The record will
		 * be handled normally. (slow path)
		 *
		 * If these events include two or more PEBS events, the
		 * records for the events can be collapsed into a single
		 * one, and it's not possible to reconstruct all events
		 * that caused the PEBS record. It's called collision.
		 * If collision happened, the record will be dropped.
		 */
		if (p->status != (1ULL << bit)) {
			for_each_set_bit(i, (unsigned long *)&pebs_status,
					 x86_pmu.max_pebs_events)
				error[i]++;
			continue;
		}

		counts[bit]++;
	}

	for (bit = 0; bit < size; bit++) {
		if ((counts[bit] == 0) && (error[bit] == 0))
			continue;

		event = cpuc->events[bit];
		if (WARN_ON_ONCE(!event))
			continue;

		if (WARN_ON_ONCE(!event->attr.precise_ip))
			continue;

		/* log dropped samples number */
		if (error[bit]) {
			perf_log_lost_samples(event, error[bit]);

			if (perf_event_account_interrupt(event))
				x86_pmu_stop(event, 0);
		}

		if (counts[bit]) {
			__intel_pmu_pebs_event(event, iregs, base,
					       top, bit, counts[bit]);
		}
	}
}

/*
 * BTS, PEBS probe and setup
 */

void __init intel_ds_init(void)
{
	/*
	 * No support for 32bit formats
	 */
	if (!boot_cpu_has(X86_FEATURE_DTES64))
		return;

	x86_pmu.bts  = boot_cpu_has(X86_FEATURE_BTS);
	x86_pmu.pebs = boot_cpu_has(X86_FEATURE_PEBS);
	x86_pmu.pebs_buffer_size = PEBS_BUFFER_SIZE;
	if (x86_pmu.pebs) {
		char pebs_type = x86_pmu.intel_cap.pebs_trap ?  '+' : '-';
		int format = x86_pmu.intel_cap.pebs_format;

		switch (format) {
		case 0:
			pr_cont("PEBS fmt0%c, ", pebs_type);
			x86_pmu.pebs_record_size = sizeof(struct pebs_record_core);
			/*
			 * Using >PAGE_SIZE buffers makes the WRMSR to
			 * PERF_GLOBAL_CTRL in intel_pmu_enable_all()
			 * mysteriously hang on Core2.
			 *
			 * As a workaround, we don't do this.
			 */
			x86_pmu.pebs_buffer_size = PAGE_SIZE;
			x86_pmu.drain_pebs = intel_pmu_drain_pebs_core;
			break;

		case 1:
			pr_cont("PEBS fmt1%c, ", pebs_type);
			x86_pmu.pebs_record_size = sizeof(struct pebs_record_nhm);
			x86_pmu.drain_pebs = intel_pmu_drain_pebs_nhm;
			break;

		case 2:
			pr_cont("PEBS fmt2%c, ", pebs_type);
			x86_pmu.pebs_record_size = sizeof(struct pebs_record_hsw);
			x86_pmu.drain_pebs = intel_pmu_drain_pebs_nhm;
			break;

		case 3:
			pr_cont("PEBS fmt3%c, ", pebs_type);
			x86_pmu.pebs_record_size =
						sizeof(struct pebs_record_skl);
			x86_pmu.drain_pebs = intel_pmu_drain_pebs_nhm;
			x86_pmu.large_pebs_flags |= PERF_SAMPLE_TIME;
			break;

		default:
			pr_cont("no PEBS fmt%d%c, ", format, pebs_type);
			x86_pmu.pebs = 0;
		}
	}
}

void perf_restore_debug_store(void)
{
	struct debug_store *ds = __this_cpu_read(cpu_hw_events.ds);

	if (!x86_pmu.bts && !x86_pmu.pebs)
		return;

	wrmsrl(MSR_IA32_DS_AREA, (unsigned long)ds);
}
