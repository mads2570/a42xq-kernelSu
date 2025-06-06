/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Copyright IBM Corp. 2007
 *
 * Authors: Hollis Blanchard <hollisb@us.ibm.com>
 */

#ifndef __POWERPC_KVM_HOST_H__
#define __POWERPC_KVM_HOST_H__

#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/kvm_types.h>
#include <linux/threads.h>
#include <linux/spinlock.h>
#include <linux/kvm_para.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <asm/kvm_asm.h>
#include <asm/processor.h>
#include <asm/page.h>
#include <asm/cacheflush.h>
#include <asm/hvcall.h>
#include <asm/mce.h>

#define KVM_MAX_VCPUS		NR_CPUS
#define KVM_MAX_VCORES		NR_CPUS
#define KVM_USER_MEM_SLOTS	512

#include <asm/cputhreads.h>

#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
#include <asm/kvm_book3s_asm.h>		/* for MAX_SMT_THREADS */
#define KVM_MAX_VCPU_ID		(MAX_SMT_THREADS * KVM_MAX_VCORES)

#else
#define KVM_MAX_VCPU_ID		KVM_MAX_VCPUS
#endif /* CONFIG_KVM_BOOK3S_HV_POSSIBLE */

#define __KVM_HAVE_ARCH_INTC_INITIALIZED

#define KVM_HALT_POLL_NS_DEFAULT 10000	/* 10 us */

/* These values are internal and can be increased later */
#define KVM_NR_IRQCHIPS          1
#define KVM_IRQCHIP_NUM_PINS     256

/* PPC-specific vcpu->requests bit members */
#define KVM_REQ_WATCHDOG	KVM_ARCH_REQ(0)
#define KVM_REQ_EPR_EXIT	KVM_ARCH_REQ(1)

#include <linux/mmu_notifier.h>

#define KVM_ARCH_WANT_MMU_NOTIFIER

extern int kvm_unmap_hva_range(struct kvm *kvm,
			       unsigned long start, unsigned long end,
			       bool blockable);
extern int kvm_age_hva(struct kvm *kvm, unsigned long start, unsigned long end);
extern int kvm_test_age_hva(struct kvm *kvm, unsigned long hva);
extern void kvm_set_spte_hva(struct kvm *kvm, unsigned long hva, pte_t pte);

#define HPTEG_CACHE_NUM			(1 << 15)
#define HPTEG_HASH_BITS_PTE		13
#define HPTEG_HASH_BITS_PTE_LONG	12
#define HPTEG_HASH_BITS_VPTE		13
#define HPTEG_HASH_BITS_VPTE_LONG	5
#define HPTEG_HASH_BITS_VPTE_64K	11
#define HPTEG_HASH_NUM_PTE		(1 << HPTEG_HASH_BITS_PTE)
#define HPTEG_HASH_NUM_PTE_LONG		(1 << HPTEG_HASH_BITS_PTE_LONG)
#define HPTEG_HASH_NUM_VPTE		(1 << HPTEG_HASH_BITS_VPTE)
#define HPTEG_HASH_NUM_VPTE_LONG	(1 << HPTEG_HASH_BITS_VPTE_LONG)
#define HPTEG_HASH_NUM_VPTE_64K		(1 << HPTEG_HASH_BITS_VPTE_64K)

/* Physical Address Mask - allowed range of real mode RAM access */
#define KVM_PAM			0x0fffffffffffffffULL

struct lppaca;
struct slb_shadow;
struct dtl_entry;

struct kvmppc_vcpu_book3s;
struct kvmppc_book3s_shadow_vcpu;

struct kvm_vm_stat {
	ulong remote_tlb_flush;
};

struct kvm_vcpu_stat {
	u64 sum_exits;
	u64 mmio_exits;
	u64 signal_exits;
	u64 light_exits;
	/* Account for special types of light exits: */
	u64 itlb_real_miss_exits;
	u64 itlb_virt_miss_exits;
	u64 dtlb_real_miss_exits;
	u64 dtlb_virt_miss_exits;
	u64 syscall_exits;
	u64 isi_exits;
	u64 dsi_exits;
	u64 emulated_inst_exits;
	u64 dec_exits;
	u64 ext_intr_exits;
	u64 halt_poll_success_ns;
	u64 halt_poll_fail_ns;
	u64 halt_wait_ns;
	u64 halt_successful_poll;
	u64 halt_attempted_poll;
	u64 halt_successful_wait;
	u64 halt_poll_invalid;
	u64 halt_wakeup;
	u64 dbell_exits;
	u64 gdbell_exits;
	u64 ld;
	u64 st;
#ifdef CONFIG_PPC_BOOK3S
	u64 pf_storage;
	u64 pf_instruc;
	u64 sp_storage;
	u64 sp_instruc;
	u64 queue_intr;
	u64 ld_slow;
	u64 st_slow;
#endif
	u64 pthru_all;
	u64 pthru_host;
	u64 pthru_bad_aff;
};

enum kvm_exit_types {
	MMIO_EXITS,
	SIGNAL_EXITS,
	ITLB_REAL_MISS_EXITS,
	ITLB_VIRT_MISS_EXITS,
	DTLB_REAL_MISS_EXITS,
	DTLB_VIRT_MISS_EXITS,
	SYSCALL_EXITS,
	ISI_EXITS,
	DSI_EXITS,
	EMULATED_INST_EXITS,
	EMULATED_MTMSRWE_EXITS,
	EMULATED_WRTEE_EXITS,
	EMULATED_MTSPR_EXITS,
	EMULATED_MFSPR_EXITS,
	EMULATED_MTMSR_EXITS,
	EMULATED_MFMSR_EXITS,
	EMULATED_TLBSX_EXITS,
	EMULATED_TLBWE_EXITS,
	EMULATED_RFI_EXITS,
	EMULATED_RFCI_EXITS,
	EMULATED_RFDI_EXITS,
	DEC_EXITS,
	EXT_INTR_EXITS,
	HALT_WAKEUP,
	USR_PR_INST,
	FP_UNAVAIL,
	DEBUG_EXITS,
	TIMEINGUEST,
	DBELL_EXITS,
	GDBELL_EXITS,
	__NUMBER_OF_KVM_EXIT_TYPES
};

/* allow access to big endian 32bit upper/lower parts and 64bit var */
struct kvmppc_exit_timing {
	union {
		u64 tv64;
		struct {
			u32 tbu, tbl;
		} tv32;
	};
};

struct kvmppc_pginfo {
	unsigned long pfn;
	atomic_t refcnt;
};

struct kvmppc_spapr_tce_iommu_table {
	struct rcu_head rcu;
	struct list_head next;
	struct iommu_table *tbl;
	struct kref kref;
};

struct kvmppc_spapr_tce_table {
	struct list_head list;
	struct kvm *kvm;
	u64 liobn;
	struct rcu_head rcu;
	u32 page_shift;
	u64 offset;		/* in pages */
	u64 size;		/* window size in pages */
	struct list_head iommu_tables;
	struct page *pages[0];
};

/* XICS components, defined in book3s_xics.c */
struct kvmppc_xics;
struct kvmppc_icp;
extern struct kvm_device_ops kvm_xics_ops;

/* XIVE components, defined in book3s_xive.c */
struct kvmppc_xive;
struct kvmppc_xive_vcpu;
extern struct kvm_device_ops kvm_xive_ops;

struct kvmppc_passthru_irqmap;

/*
 * The reverse mapping array has one entry for each HPTE,
 * which stores the guest's view of the second word of the HPTE
 * (including the guest physical address of the mapping),
 * plus forward and backward pointers in a doubly-linked ring
 * of HPTEs that map the same host page.  The pointers in this
 * ring are 32-bit HPTE indexes, to save space.
 */
struct revmap_entry {
	unsigned long guest_rpte;
	unsigned int forw, back;
};

/*
 * We use the top bit of each memslot->arch.rmap entry as a lock bit,
 * and bit 32 as a present flag.  The bottom 32 bits are the
 * index in the guest HPT of a HPTE that points to the page.
 */
#define KVMPPC_RMAP_LOCK_BIT	63
#define KVMPPC_RMAP_RC_SHIFT	32
#define KVMPPC_RMAP_REFERENCED	(HPTE_R_R << KVMPPC_RMAP_RC_SHIFT)
#define KVMPPC_RMAP_PRESENT	0x100000000ul
#define KVMPPC_RMAP_INDEX	0xfffffffful

struct kvm_arch_memory_slot {
#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	unsigned long *rmap;
#endif /* CONFIG_KVM_BOOK3S_HV_POSSIBLE */
};

struct kvm_hpt_info {
	/* Host virtual (linear mapping) address of guest HPT */
	unsigned long virt;
	/* Array of reverse mapping entries for each guest HPTE */
	struct revmap_entry *rev;
	/* Guest HPT size is 2**(order) bytes */
	u32 order;
	/* 1 if HPT allocated with CMA, 0 otherwise */
	int cma;
};

struct kvm_resize_hpt;

struct kvm_arch {
	unsigned int lpid;
	unsigned int smt_mode;		/* # vcpus per virtual core */
	unsigned int emul_smt_mode;	/* emualted SMT mode, on P9 */
#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	unsigned int tlb_sets;
	struct kvm_hpt_info hpt;
	atomic64_t mmio_update;
	unsigned int host_lpid;
	unsigned long host_lpcr;
	unsigned long sdr1;
	unsigned long host_sdr1;
	unsigned long lpcr;
	unsigned long vrma_slb_v;
	int mmu_ready;
	atomic_t vcpus_running;
	u32 online_vcores;
	atomic_t hpte_mod_interest;
	cpumask_t need_tlb_flush;
	cpumask_t cpu_in_guest;
	u8 radix;
	u8 fwnmi_enabled;
	bool threads_indep;
	pgd_t *pgtable;
	u64 process_table;
	struct dentry *debugfs_dir;
	struct dentry *htab_dentry;
	struct kvm_resize_hpt *resize_hpt; /* protected by kvm->lock */
#endif /* CONFIG_KVM_BOOK3S_HV_POSSIBLE */
#ifdef CONFIG_KVM_BOOK3S_PR_POSSIBLE
	struct mutex hpt_mutex;
#endif
#ifdef CONFIG_PPC_BOOK3S_64
	struct list_head spapr_tce_tables;
	struct list_head rtas_tokens;
	struct mutex rtas_token_lock;
	DECLARE_BITMAP(enabled_hcalls, MAX_HCALL_OPCODE/4 + 1);
#endif
#ifdef CONFIG_KVM_MPIC
	struct openpic *mpic;
#endif
#ifdef CONFIG_KVM_XICS
	struct kvmppc_xics *xics;
	struct kvmppc_xive *xive;
	struct kvmppc_passthru_irqmap *pimap;
#endif
	struct kvmppc_ops *kvm_ops;
#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	/* This array can grow quite large, keep it at the end */
	struct kvmppc_vcore *vcores[KVM_MAX_VCORES];
#endif
};

#define VCORE_ENTRY_MAP(vc)	((vc)->entry_exit_map & 0xff)
#define VCORE_EXIT_MAP(vc)	((vc)->entry_exit_map >> 8)
#define VCORE_IS_EXITING(vc)	(VCORE_EXIT_MAP(vc) != 0)

/* This bit is used when a vcore exit is triggered from outside the vcore */
#define VCORE_EXIT_REQ		0x10000

/*
 * Values for vcore_state.
 * Note that these are arranged such that lower values
 * (< VCORE_SLEEPING) don't require stolen time accounting
 * on load/unload, and higher values do.
 */
#define VCORE_INACTIVE	0
#define VCORE_PREEMPT	1
#define VCORE_PIGGYBACK	2
#define VCORE_SLEEPING	3
#define VCORE_RUNNING	4
#define VCORE_EXITING	5
#define VCORE_POLLING	6

/*
 * Struct used to manage memory for a virtual processor area
 * registered by a PAPR guest.  There are three types of area
 * that a guest can register.
 */
struct kvmppc_vpa {
	unsigned long gpa;	/* Current guest phys addr */
	void *pinned_addr;	/* Address in kernel linear mapping */
	void *pinned_end;	/* End of region */
	unsigned long next_gpa;	/* Guest phys addr for update */
	unsigned long len;	/* Number of bytes required */
	u8 update_pending;	/* 1 => update pinned_addr from next_gpa */
	bool dirty;		/* true => area has been modified by kernel */
};

struct kvmppc_pte {
	ulong eaddr;
	u64 vpage;
	ulong raddr;
	bool may_read		: 1;
	bool may_write		: 1;
	bool may_execute	: 1;
	unsigned long wimg;
	u8 page_size;		/* MMU_PAGE_xxx */
};

struct kvmppc_mmu {
	/* book3s_64 only */
	void (*slbmte)(struct kvm_vcpu *vcpu, u64 rb, u64 rs);
	u64  (*slbmfee)(struct kvm_vcpu *vcpu, u64 slb_nr);
	u64  (*slbmfev)(struct kvm_vcpu *vcpu, u64 slb_nr);
	void (*slbie)(struct kvm_vcpu *vcpu, u64 slb_nr);
	void (*slbia)(struct kvm_vcpu *vcpu);
	/* book3s */
	void (*mtsrin)(struct kvm_vcpu *vcpu, u32 srnum, ulong value);
	u32  (*mfsrin)(struct kvm_vcpu *vcpu, u32 srnum);
	int  (*xlate)(struct kvm_vcpu *vcpu, gva_t eaddr,
		      struct kvmppc_pte *pte, bool data, bool iswrite);
	void (*reset_msr)(struct kvm_vcpu *vcpu);
	void (*tlbie)(struct kvm_vcpu *vcpu, ulong addr, bool large);
	int  (*esid_to_vsid)(struct kvm_vcpu *vcpu, ulong esid, u64 *vsid);
	u64  (*ea_to_vp)(struct kvm_vcpu *vcpu, gva_t eaddr, bool data);
	bool (*is_dcbz32)(struct kvm_vcpu *vcpu);
};

struct kvmppc_slb {
	u64 esid;
	u64 vsid;
	u64 orige;
	u64 origv;
	bool valid	: 1;
	bool Ks		: 1;
	bool Kp		: 1;
	bool nx		: 1;
	bool large	: 1;	/* PTEs are 16MB */
	bool tb		: 1;	/* 1TB segment */
	bool class	: 1;
	u8 base_page_size;	/* MMU_PAGE_xxx */
};

/* Struct used to accumulate timing information in HV real mode code */
struct kvmhv_tb_accumulator {
	u64	seqcount;	/* used to synchronize access, also count * 2 */
	u64	tb_total;	/* total time in timebase ticks */
	u64	tb_min;		/* min time */
	u64	tb_max;		/* max time */
};

#ifdef CONFIG_PPC_BOOK3S_64
struct kvmppc_irq_map {
	u32	r_hwirq;
	u32	v_hwirq;
	struct irq_desc *desc;
};

#define	KVMPPC_PIRQ_MAPPED	1024
struct kvmppc_passthru_irqmap {
	int n_mapped;
	struct kvmppc_irq_map mapped[KVMPPC_PIRQ_MAPPED];
};
#endif

# ifdef CONFIG_PPC_FSL_BOOK3E
#define KVMPPC_BOOKE_IAC_NUM	2
#define KVMPPC_BOOKE_DAC_NUM	2
# else
#define KVMPPC_BOOKE_IAC_NUM	4
#define KVMPPC_BOOKE_DAC_NUM	2
# endif
#define KVMPPC_BOOKE_MAX_IAC	4
#define KVMPPC_BOOKE_MAX_DAC	2

/* KVMPPC_EPR_USER takes precedence over KVMPPC_EPR_KERNEL */
#define KVMPPC_EPR_NONE		0 /* EPR not supported */
#define KVMPPC_EPR_USER		1 /* exit to userspace to fill EPR */
#define KVMPPC_EPR_KERNEL	2 /* in-kernel irqchip */

#define KVMPPC_IRQ_DEFAULT	0
#define KVMPPC_IRQ_MPIC		1
#define KVMPPC_IRQ_XICS		2 /* Includes a XIVE option */

#define MMIO_HPTE_CACHE_SIZE	4

struct mmio_hpte_cache_entry {
	unsigned long hpte_v;
	unsigned long hpte_r;
	unsigned long rpte;
	unsigned long pte_index;
	unsigned long eaddr;
	unsigned long slb_v;
	long mmio_update;
	unsigned int slb_base_pshift;
};

struct mmio_hpte_cache {
	struct mmio_hpte_cache_entry entry[MMIO_HPTE_CACHE_SIZE];
	unsigned int index;
};

#define KVMPPC_VSX_COPY_NONE		0
#define KVMPPC_VSX_COPY_WORD		1
#define KVMPPC_VSX_COPY_DWORD		2
#define KVMPPC_VSX_COPY_DWORD_LOAD_DUMP	3
#define KVMPPC_VSX_COPY_WORD_LOAD_DUMP	4

#define KVMPPC_VMX_COPY_BYTE		8
#define KVMPPC_VMX_COPY_HWORD		9
#define KVMPPC_VMX_COPY_WORD		10
#define KVMPPC_VMX_COPY_DWORD		11

struct openpic;

/* W0 and W1 of a XIVE thread management context */
union xive_tma_w01 {
	struct {
		u8	nsr;
		u8	cppr;
		u8	ipb;
		u8	lsmfb;
		u8	ack;
		u8	inc;
		u8	age;
		u8	pipr;
	};
	__be64 w01;
};

struct kvm_vcpu_arch {
	ulong host_stack;
	u32 host_pid;
#ifdef CONFIG_PPC_BOOK3S
	struct kvmppc_slb slb[64];
	int slb_max;		/* 1 + index of last valid entry in slb[] */
	int slb_nr;		/* total number of entries in SLB */
	struct kvmppc_mmu mmu;
	struct kvmppc_vcpu_book3s *book3s;
#endif
#ifdef CONFIG_PPC_BOOK3S_32
	struct kvmppc_book3s_shadow_vcpu *shadow_vcpu;
#endif

	struct pt_regs regs;

	struct thread_fp_state fp;

#ifdef CONFIG_SPE
	ulong evr[32];
	ulong spefscr;
	ulong host_spefscr;
	u64 acc;
#endif
#ifdef CONFIG_ALTIVEC
	struct thread_vr_state vr;
#endif

#ifdef CONFIG_KVM_BOOKE_HV
	u32 host_mas4;
	u32 host_mas6;
	u32 shadow_epcr;
	u32 shadow_msrp;
	u32 eplc;
	u32 epsc;
	u32 oldpir;
#endif

#if defined(CONFIG_BOOKE)
#if defined(CONFIG_KVM_BOOKE_HV) || defined(CONFIG_64BIT)
	u32 epcr;
#endif
#endif

#ifdef CONFIG_PPC_BOOK3S
	/* For Gekko paired singles */
	u32 qpr[32];
#endif

#ifdef CONFIG_PPC_BOOK3S
	ulong tar;
#endif

#ifdef CONFIG_PPC_BOOK3S
	ulong hflags;
	ulong guest_owned_ext;
	ulong purr;
	ulong spurr;
	ulong ic;
	ulong dscr;
	ulong amr;
	ulong uamor;
	ulong iamr;
	u32 ctrl;
	u32 dabrx;
	ulong dabr;
	ulong dawr;
	ulong dawrx;
	ulong ciabr;
	ulong cfar;
	ulong ppr;
	u32 pspb;
	ulong fscr;
	ulong shadow_fscr;
	ulong ebbhr;
	ulong ebbrr;
	ulong bescr;
	ulong csigr;
	ulong tacr;
	ulong tcscr;
	ulong acop;
	ulong wort;
	ulong tid;
	ulong psscr;
	ulong hfscr;
	ulong shadow_srr1;
#endif
	u32 vrsave; /* also USPRG0 */
	u32 mmucr;
	/* shadow_msr is unused for BookE HV */
	ulong shadow_msr;
	ulong csrr0;
	ulong csrr1;
	ulong dsrr0;
	ulong dsrr1;
	ulong mcsrr0;
	ulong mcsrr1;
	ulong mcsr;
	ulong dec;
#ifdef CONFIG_BOOKE
	u32 decar;
#endif
	/* Time base value when we entered the guest */
	u64 entry_tb;
	u64 entry_vtb;
	u64 entry_ic;
	u32 tcr;
	ulong tsr; /* we need to perform set/clr_bits() which requires ulong */
	u32 ivor[64];
	ulong ivpr;
	u32 pvr;

	u32 shadow_pid;
	u32 shadow_pid1;
	u32 pid;
	u32 swap_pid;

	u32 ccr0;
	u32 ccr1;
	u32 dbsr;

	u64 mmcr[5];
	u32 pmc[8];
	u32 spmc[2];
	u64 siar;
	u64 sdar;
	u64 sier;
#ifdef CONFIG_PPC_TRANSACTIONAL_MEM
	u64 tfhar;
	u64 texasr;
	u64 tfiar;
	u64 orig_texasr;

	u32 cr_tm;
	u64 xer_tm;
	u64 lr_tm;
	u64 ctr_tm;
	u64 amr_tm;
	u64 ppr_tm;
	u64 dscr_tm;
	u64 tar_tm;

	ulong gpr_tm[32];

	struct thread_fp_state fp_tm;

	struct thread_vr_state vr_tm;
	u32 vrsave_tm; /* also USPRG0 */
#endif

#ifdef CONFIG_KVM_EXIT_TIMING
	struct mutex exit_timing_lock;
	struct kvmppc_exit_timing timing_exit;
	struct kvmppc_exit_timing timing_last_enter;
	u32 last_exit_type;
	u32 timing_count_type[__NUMBER_OF_KVM_EXIT_TYPES];
	u64 timing_sum_duration[__NUMBER_OF_KVM_EXIT_TYPES];
	u64 timing_sum_quad_duration[__NUMBER_OF_KVM_EXIT_TYPES];
	u64 timing_min_duration[__NUMBER_OF_KVM_EXIT_TYPES];
	u64 timing_max_duration[__NUMBER_OF_KVM_EXIT_TYPES];
	u64 timing_last_exit;
	struct dentry *debugfs_exit_timing;
#endif

#ifdef CONFIG_PPC_BOOK3S
	ulong fault_dar;
	u32 fault_dsisr;
	unsigned long intr_msr;
	ulong fault_gpa;	/* guest real address of page fault (POWER9) */
#endif

#ifdef CONFIG_BOOKE
	ulong fault_dear;
	ulong fault_esr;
	ulong queued_dear;
	ulong queued_esr;
	spinlock_t wdt_lock;
	struct timer_list wdt_timer;
	u32 tlbcfg[4];
	u32 tlbps[4];
	u32 mmucfg;
	u32 eptcfg;
	u32 epr;
	u64 sprg9;
	u32 pwrmgtcr0;
	u32 crit_save;
	/* guest debug registers*/
	struct debug_reg dbg_reg;
#endif
	gpa_t paddr_accessed;
	gva_t vaddr_accessed;
	pgd_t *pgdir;

	u16 io_gpr; /* GPR used as IO source/target */
	u8 mmio_host_swabbed;
	u8 mmio_sign_extend;
	/* conversion between single and double precision */
	u8 mmio_sp64_extend;
	/*
	 * Number of simulations for vsx.
	 * If we use 2*8bytes to simulate 1*16bytes,
	 * then the number should be 2 and
	 * mmio_copy_type=KVMPPC_VSX_COPY_DWORD.
	 * If we use 4*4bytes to simulate 1*16bytes,
	 * the number should be 4 and
	 * mmio_vsx_copy_type=KVMPPC_VSX_COPY_WORD.
	 */
	u8 mmio_vsx_copy_nums;
	u8 mmio_vsx_offset;
	u8 mmio_vmx_copy_nums;
	u8 mmio_vmx_offset;
	u8 mmio_copy_type;
	u8 osi_needed;
	u8 osi_enabled;
	u8 papr_enabled;
	u8 watchdog_enabled;
	u8 sane;
	u8 cpu_type;
	u8 hcall_needed;
	u8 epr_flags; /* KVMPPC_EPR_xxx */
	u8 epr_needed;

	u32 cpr0_cfgaddr; /* holds the last set cpr0_cfgaddr */

	struct hrtimer dec_timer;
	u64 dec_jiffies;
	u64 dec_expires;
	unsigned long pending_exceptions;
	u8 ceded;
	u8 prodded;
	u8 doorbell_request;
	u8 irq_pending; /* Used by XIVE to signal pending guest irqs */
	u32 last_inst;

	struct swait_queue_head *wqp;
	struct kvmppc_vcore *vcore;
	int ret;
	int trap;
	int state;
	int ptid;
	int thread_cpu;
	int prev_cpu;
	bool timer_running;
	wait_queue_head_t cpu_run;
	struct machine_check_event mce_evt; /* Valid if trap == 0x200 */

	struct kvm_vcpu_arch_shared *shared;
#if defined(CONFIG_PPC_BOOK3S_64) && defined(CONFIG_KVM_BOOK3S_PR_POSSIBLE)
	bool shared_big_endian;
#endif
	unsigned long magic_page_pa; /* phys addr to map the magic page to */
	unsigned long magic_page_ea; /* effect. addr to map the magic page to */
	bool disable_kernel_nx;

	int irq_type;		/* one of KVM_IRQ_* */
	int irq_cpu_id;
	struct openpic *mpic;	/* KVM_IRQ_MPIC */
#ifdef CONFIG_KVM_XICS
	struct kvmppc_icp *icp; /* XICS presentation controller */
	struct kvmppc_xive_vcpu *xive_vcpu; /* XIVE virtual CPU data */
	__be32 xive_cam_word;    /* Cooked W2 in proper endian with valid bit */
	u8 xive_pushed;		 /* Is the VP pushed on the physical CPU ? */
	u8 xive_esc_on;		 /* Is the escalation irq enabled ? */
	union xive_tma_w01 xive_saved_state; /* W0..1 of XIVE thread state */
	u64 xive_esc_raddr;	 /* Escalation interrupt ESB real addr */
	u64 xive_esc_vaddr;	 /* Escalation interrupt ESB virt addr */
#endif

#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	struct kvm_vcpu_arch_shared shregs;

	struct mmio_hpte_cache mmio_cache;
	unsigned long pgfault_addr;
	long pgfault_index;
	unsigned long pgfault_hpte[2];
	struct mmio_hpte_cache_entry *pgfault_cache;

	struct task_struct *run_task;
	struct kvm_run *kvm_run;

	spinlock_t vpa_update_lock;
	struct kvmppc_vpa vpa;
	struct kvmppc_vpa dtl;
	struct dtl_entry *dtl_ptr;
	unsigned long dtl_index;
	u64 stolen_logged;
	struct kvmppc_vpa slb_shadow;

	spinlock_t tbacct_lock;
	u64 busy_stolen;
	u64 busy_preempt;

	u32 emul_inst;

	u32 online;
#endif

#ifdef CONFIG_KVM_BOOK3S_HV_EXIT_TIMING
	struct kvmhv_tb_accumulator *cur_activity;	/* What we're timing */
	u64	cur_tb_start;			/* when it started */
	struct kvmhv_tb_accumulator rm_entry;	/* real-mode entry code */
	struct kvmhv_tb_accumulator rm_intr;	/* real-mode intr handling */
	struct kvmhv_tb_accumulator rm_exit;	/* real-mode exit code */
	struct kvmhv_tb_accumulator guest_time;	/* guest execution */
	struct kvmhv_tb_accumulator cede_time;	/* time napping inside guest */

	struct dentry *debugfs_dir;
	struct dentry *debugfs_timings;
#endif /* CONFIG_KVM_BOOK3S_HV_EXIT_TIMING */
};

#define VCPU_FPR(vcpu, i)	(vcpu)->arch.fp.fpr[i][TS_FPROFFSET]
#define VCPU_VSX_FPR(vcpu, i, j)	((vcpu)->arch.fp.fpr[i][j])
#define VCPU_VSX_VR(vcpu, i)		((vcpu)->arch.vr.vr[i])

/* Values for vcpu->arch.state */
#define KVMPPC_VCPU_NOTREADY		0
#define KVMPPC_VCPU_RUNNABLE		1
#define KVMPPC_VCPU_BUSY_IN_HOST	2

/* Values for vcpu->arch.io_gpr */
#define KVM_MMIO_REG_MASK	0x003f
#define KVM_MMIO_REG_EXT_MASK	0xffc0
#define KVM_MMIO_REG_GPR	0x0000
#define KVM_MMIO_REG_FPR	0x0040
#define KVM_MMIO_REG_QPR	0x0080
#define KVM_MMIO_REG_FQPR	0x00c0
#define KVM_MMIO_REG_VSX	0x0100
#define KVM_MMIO_REG_VMX	0x0180

#define __KVM_HAVE_ARCH_WQP
#define __KVM_HAVE_CREATE_DEVICE

static inline void kvm_arch_hardware_disable(void) {}
static inline void kvm_arch_hardware_unsetup(void) {}
static inline void kvm_arch_sync_events(struct kvm *kvm) {}
static inline void kvm_arch_memslots_updated(struct kvm *kvm, u64 gen) {}
static inline void kvm_arch_flush_shadow_all(struct kvm *kvm) {}
static inline void kvm_arch_sched_in(struct kvm_vcpu *vcpu, int cpu) {}
static inline void kvm_arch_exit(void) {}
static inline void kvm_arch_vcpu_blocking(struct kvm_vcpu *vcpu) {}
static inline void kvm_arch_vcpu_unblocking(struct kvm_vcpu *vcpu) {}
static inline void kvm_arch_vcpu_block_finish(struct kvm_vcpu *vcpu) {}

#endif /* __POWERPC_KVM_HOST_H__ */
