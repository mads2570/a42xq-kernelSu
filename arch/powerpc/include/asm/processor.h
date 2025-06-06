#ifndef _ASM_POWERPC_PROCESSOR_H
#define _ASM_POWERPC_PROCESSOR_H

/*
 * Copyright (C) 2001 PPC 64 Team, IBM Corp
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <asm/reg.h>

#ifdef CONFIG_VSX
#define TS_FPRWIDTH 2

#ifdef __BIG_ENDIAN__
#define TS_FPROFFSET 0
#define TS_VSRLOWOFFSET 1
#else
#define TS_FPROFFSET 1
#define TS_VSRLOWOFFSET 0
#endif

#else
#define TS_FPRWIDTH 1
#define TS_FPROFFSET 0
#endif

#ifdef CONFIG_PPC64
/* Default SMT priority is set to 3. Use 11- 13bits to save priority. */
#define PPR_PRIORITY 3
#ifdef __ASSEMBLY__
#define INIT_PPR (PPR_PRIORITY << 50)
#else
#define INIT_PPR ((u64)PPR_PRIORITY << 50)
#endif /* __ASSEMBLY__ */
#endif /* CONFIG_PPC64 */

#ifndef __ASSEMBLY__
#include <linux/types.h>
#include <asm/thread_info.h>
#include <asm/ptrace.h>
#include <asm/hw_breakpoint.h>

/* We do _not_ want to define new machine types at all, those must die
 * in favor of using the device-tree
 * -- BenH.
 */

/* PREP sub-platform types. Unused */
#define _PREP_Motorola	0x01	/* motorola prep */
#define _PREP_Firm	0x02	/* firmworks prep */
#define _PREP_IBM	0x00	/* ibm prep */
#define _PREP_Bull	0x03	/* bull prep */

/* CHRP sub-platform types. These are arbitrary */
#define _CHRP_Motorola	0x04	/* motorola chrp, the cobra */
#define _CHRP_IBM	0x05	/* IBM chrp, the longtrail and longtrail 2 */
#define _CHRP_Pegasos	0x06	/* Genesi/bplan's Pegasos and Pegasos2 */
#define _CHRP_briq	0x07	/* TotalImpact's briQ */

#if defined(__KERNEL__) && defined(CONFIG_PPC32)

extern int _chrp_type;

#endif /* defined(__KERNEL__) && defined(CONFIG_PPC32) */

/*
 * Default implementation of macro that returns current
 * instruction pointer ("program counter").
 */
#define current_text_addr() ({ __label__ _l; _l: &&_l;})

/* Macros for adjusting thread priority (hardware multi-threading) */
#define HMT_very_low()   asm volatile("or 31,31,31   # very low priority")
#define HMT_low()	 asm volatile("or 1,1,1	     # low priority")
#define HMT_medium_low() asm volatile("or 6,6,6      # medium low priority")
#define HMT_medium()	 asm volatile("or 2,2,2	     # medium priority")
#define HMT_medium_high() asm volatile("or 5,5,5      # medium high priority")
#define HMT_high()	 asm volatile("or 3,3,3	     # high priority")

#ifdef __KERNEL__

struct task_struct;
void start_thread(struct pt_regs *regs, unsigned long fdptr, unsigned long sp);
void release_thread(struct task_struct *);

#ifdef CONFIG_PPC32

#if CONFIG_TASK_SIZE > CONFIG_KERNEL_START
#error User TASK_SIZE overlaps with KERNEL_START address
#endif
#define TASK_SIZE	(CONFIG_TASK_SIZE)

/* This decides where the kernel will search for a free chunk of vm
 * space during mmap's.
 */
#define TASK_UNMAPPED_BASE	(TASK_SIZE / 8 * 3)
#endif

#ifdef CONFIG_PPC64
/*
 * 64-bit user address space can have multiple limits
 * For now supported values are:
 */
#define TASK_SIZE_64TB  (0x0000400000000000UL)
#define TASK_SIZE_128TB (0x0000800000000000UL)
#define TASK_SIZE_512TB (0x0002000000000000UL)
#define TASK_SIZE_1PB   (0x0004000000000000UL)
#define TASK_SIZE_2PB   (0x0008000000000000UL)
/*
 * With 52 bits in the address we can support
 * upto 4PB of range.
 */
#define TASK_SIZE_4PB   (0x0010000000000000UL)

/*
 * For now 512TB is only supported with book3s and 64K linux page size.
 */
#if defined(CONFIG_PPC_BOOK3S_64) && defined(CONFIG_PPC_64K_PAGES)
/*
 * Max value currently used:
 */
#define TASK_SIZE_USER64		TASK_SIZE_4PB
#define DEFAULT_MAP_WINDOW_USER64	TASK_SIZE_128TB
#define TASK_CONTEXT_SIZE		TASK_SIZE_512TB
#else
#define TASK_SIZE_USER64		TASK_SIZE_64TB
#define DEFAULT_MAP_WINDOW_USER64	TASK_SIZE_64TB
/*
 * We don't need to allocate extended context ids for 4K page size, because
 * we limit the max effective address on this config to 64TB.
 */
#define TASK_CONTEXT_SIZE		TASK_SIZE_64TB
#endif

/*
 * 32-bit user address space is 4GB - 1 page
 * (this 1 page is needed so referencing of 0xFFFFFFFF generates EFAULT
 */
#define TASK_SIZE_USER32 (0x0000000100000000UL - (1*PAGE_SIZE))

#define TASK_SIZE_OF(tsk) (test_tsk_thread_flag(tsk, TIF_32BIT) ? \
		TASK_SIZE_USER32 : TASK_SIZE_USER64)
#define TASK_SIZE	  TASK_SIZE_OF(current)
/* This decides where the kernel will search for a free chunk of vm
 * space during mmap's.
 */
#define TASK_UNMAPPED_BASE_USER32 (PAGE_ALIGN(TASK_SIZE_USER32 / 4))
#define TASK_UNMAPPED_BASE_USER64 (PAGE_ALIGN(DEFAULT_MAP_WINDOW_USER64 / 4))

#define TASK_UNMAPPED_BASE ((is_32bit_task()) ? \
		TASK_UNMAPPED_BASE_USER32 : TASK_UNMAPPED_BASE_USER64 )
#endif

/*
 * Initial task size value for user applications. For book3s 64 we start
 * with 128TB and conditionally enable upto 512TB
 */
#ifdef CONFIG_PPC_BOOK3S_64
#define DEFAULT_MAP_WINDOW	((is_32bit_task()) ?			\
				 TASK_SIZE_USER32 : DEFAULT_MAP_WINDOW_USER64)
#else
#define DEFAULT_MAP_WINDOW	TASK_SIZE
#endif

#ifdef __powerpc64__

#define STACK_TOP_USER64 DEFAULT_MAP_WINDOW_USER64
#define STACK_TOP_USER32 TASK_SIZE_USER32

#define STACK_TOP (is_32bit_task() ? \
		   STACK_TOP_USER32 : STACK_TOP_USER64)

#define STACK_TOP_MAX TASK_SIZE_USER64

#else /* __powerpc64__ */

#define STACK_TOP TASK_SIZE
#define STACK_TOP_MAX	STACK_TOP

#endif /* __powerpc64__ */

typedef struct {
	unsigned long seg;
} mm_segment_t;

#define TS_FPR(i) fp_state.fpr[i][TS_FPROFFSET]
#define TS_CKFPR(i) ckfp_state.fpr[i][TS_FPROFFSET]

/* FP and VSX 0-31 register set */
struct thread_fp_state {
	u64	fpr[32][TS_FPRWIDTH] __attribute__((aligned(16)));
	u64	fpscr;		/* Floating point status */
};

/* Complete AltiVec register set including VSCR */
struct thread_vr_state {
	vector128	vr[32] __attribute__((aligned(16)));
	vector128	vscr __attribute__((aligned(16)));
};

struct debug_reg {
#ifdef CONFIG_PPC_ADV_DEBUG_REGS
	/*
	 * The following help to manage the use of Debug Control Registers
	 * om the BookE platforms.
	 */
	uint32_t	dbcr0;
	uint32_t	dbcr1;
#ifdef CONFIG_BOOKE
	uint32_t	dbcr2;
#endif
	/*
	 * The stored value of the DBSR register will be the value at the
	 * last debug interrupt. This register can only be read from the
	 * user (will never be written to) and has value while helping to
	 * describe the reason for the last debug trap.  Torez
	 */
	uint32_t	dbsr;
	/*
	 * The following will contain addresses used by debug applications
	 * to help trace and trap on particular address locations.
	 * The bits in the Debug Control Registers above help define which
	 * of the following registers will contain valid data and/or addresses.
	 */
	unsigned long	iac1;
	unsigned long	iac2;
#if CONFIG_PPC_ADV_DEBUG_IACS > 2
	unsigned long	iac3;
	unsigned long	iac4;
#endif
	unsigned long	dac1;
	unsigned long	dac2;
#if CONFIG_PPC_ADV_DEBUG_DVCS > 0
	unsigned long	dvc1;
	unsigned long	dvc2;
#endif
#endif
};

struct thread_struct {
	unsigned long	ksp;		/* Kernel stack pointer */

#ifdef CONFIG_PPC64
	unsigned long	ksp_vsid;
#endif
	struct pt_regs	*regs;		/* Pointer to saved register state */
	mm_segment_t	addr_limit;	/* for get_fs() validation */
#ifdef CONFIG_BOOKE
	/* BookE base exception scratch space; align on cacheline */
	unsigned long	normsave[8] ____cacheline_aligned;
#endif
#ifdef CONFIG_PPC32
	void		*pgdir;		/* root of page-table tree */
	unsigned long	ksp_limit;	/* if ksp <= ksp_limit stack overflow */
#endif
	/* Debug Registers */
	struct debug_reg debug;
	struct thread_fp_state	fp_state;
	struct thread_fp_state	*fp_save_area;
	int		fpexc_mode;	/* floating-point exception mode */
	unsigned int	align_ctl;	/* alignment handling control */
#ifdef CONFIG_HAVE_HW_BREAKPOINT
	struct perf_event *ptrace_bps[HBP_NUM];
	/*
	 * Helps identify source of single-step exception and subsequent
	 * hw-breakpoint enablement
	 */
	struct perf_event *last_hit_ubp;
#endif /* CONFIG_HAVE_HW_BREAKPOINT */
	struct arch_hw_breakpoint hw_brk; /* info on the hardware breakpoint */
	unsigned long	trap_nr;	/* last trap # on this thread */
	u8 load_fp;
#ifdef CONFIG_ALTIVEC
	u8 load_vec;
	struct thread_vr_state vr_state;
	struct thread_vr_state *vr_save_area;
	unsigned long	vrsave;
	int		used_vr;	/* set if process has used altivec */
#endif /* CONFIG_ALTIVEC */
#ifdef CONFIG_VSX
	/* VSR status */
	int		used_vsr;	/* set if process has used VSX */
#endif /* CONFIG_VSX */
#ifdef CONFIG_SPE
	unsigned long	evr[32];	/* upper 32-bits of SPE regs */
	u64		acc;		/* Accumulator */
	unsigned long	spefscr;	/* SPE & eFP status */
	unsigned long	spefscr_last;	/* SPEFSCR value on last prctl
					   call or trap return */
	int		used_spe;	/* set if process has used spe */
#endif /* CONFIG_SPE */
#ifdef CONFIG_PPC_TRANSACTIONAL_MEM
	u8	load_tm;
	u64		tm_tfhar;	/* Transaction fail handler addr */
	u64		tm_texasr;	/* Transaction exception & summary */
	u64		tm_tfiar;	/* Transaction fail instr address reg */
	struct pt_regs	ckpt_regs;	/* Checkpointed registers */

	unsigned long	tm_tar;
	unsigned long	tm_ppr;
	unsigned long	tm_dscr;

	/*
	 * Checkpointed FP and VSX 0-31 register set.
	 *
	 * When a transaction is active/signalled/scheduled etc., *regs is the
	 * most recent set of/speculated GPRs with ckpt_regs being the older
	 * checkpointed regs to which we roll back if transaction aborts.
	 *
	 * These are analogous to how ckpt_regs and pt_regs work
	 */
	struct thread_fp_state ckfp_state; /* Checkpointed FP state */
	struct thread_vr_state ckvr_state; /* Checkpointed VR state */
	unsigned long	ckvrsave; /* Checkpointed VRSAVE */
#endif /* CONFIG_PPC_TRANSACTIONAL_MEM */
#ifdef CONFIG_PPC_MEM_KEYS
	unsigned long	amr;
	unsigned long	iamr;
	unsigned long	uamor;
#endif
#ifdef CONFIG_KVM_BOOK3S_32_HANDLER
	void*		kvm_shadow_vcpu; /* KVM internal data */
#endif /* CONFIG_KVM_BOOK3S_32_HANDLER */
#if defined(CONFIG_KVM) && defined(CONFIG_BOOKE)
	struct kvm_vcpu	*kvm_vcpu;
#endif
#ifdef CONFIG_PPC64
	unsigned long	dscr;
	unsigned long	fscr;
	/*
	 * This member element dscr_inherit indicates that the process
	 * has explicitly attempted and changed the DSCR register value
	 * for itself. Hence kernel wont use the default CPU DSCR value
	 * contained in the PACA structure anymore during process context
	 * switch. Once this variable is set, this behaviour will also be
	 * inherited to all the children of this process from that point
	 * onwards.
	 */
	int		dscr_inherit;
	unsigned long	ppr;	/* used to save/restore SMT priority */
	unsigned long	tidr;
#endif
#ifdef CONFIG_PPC_BOOK3S_64
	unsigned long	tar;
	unsigned long	ebbrr;
	unsigned long	ebbhr;
	unsigned long	bescr;
	unsigned long	siar;
	unsigned long	sdar;
	unsigned long	sier;
	unsigned long	mmcr2;
	unsigned 	mmcr0;

	unsigned 	used_ebb;
	unsigned int	used_vas;
#endif
};

#define ARCH_MIN_TASKALIGN 16

#define INIT_SP		(sizeof(init_stack) + (unsigned long) &init_stack)
#define INIT_SP_LIMIT \
	(_ALIGN_UP(sizeof(init_thread_info), 16) + (unsigned long) &init_stack)

#ifdef CONFIG_SPE
#define SPEFSCR_INIT \
	.spefscr = SPEFSCR_FINVE | SPEFSCR_FDBZE | SPEFSCR_FUNFE | SPEFSCR_FOVFE, \
	.spefscr_last = SPEFSCR_FINVE | SPEFSCR_FDBZE | SPEFSCR_FUNFE | SPEFSCR_FOVFE,
#else
#define SPEFSCR_INIT
#endif

#ifdef CONFIG_PPC32
#define INIT_THREAD { \
	.ksp = INIT_SP, \
	.ksp_limit = INIT_SP_LIMIT, \
	.addr_limit = KERNEL_DS, \
	.pgdir = swapper_pg_dir, \
	.fpexc_mode = MSR_FE0 | MSR_FE1, \
	SPEFSCR_INIT \
}
#else
#define INIT_THREAD  { \
	.ksp = INIT_SP, \
	.addr_limit = KERNEL_DS, \
	.fpexc_mode = 0, \
	.ppr = INIT_PPR, \
	.fscr = FSCR_TAR | FSCR_EBB \
}
#endif

#define task_pt_regs(tsk)	((struct pt_regs *)(tsk)->thread.regs)

unsigned long get_wchan(struct task_struct *p);

#define KSTK_EIP(tsk)  ((tsk)->thread.regs? (tsk)->thread.regs->nip: 0)
#define KSTK_ESP(tsk)  ((tsk)->thread.regs? (tsk)->thread.regs->gpr[1]: 0)

/* Get/set floating-point exception mode */
#define GET_FPEXC_CTL(tsk, adr) get_fpexc_mode((tsk), (adr))
#define SET_FPEXC_CTL(tsk, val) set_fpexc_mode((tsk), (val))

extern int get_fpexc_mode(struct task_struct *tsk, unsigned long adr);
extern int set_fpexc_mode(struct task_struct *tsk, unsigned int val);

#define GET_ENDIAN(tsk, adr) get_endian((tsk), (adr))
#define SET_ENDIAN(tsk, val) set_endian((tsk), (val))

extern int get_endian(struct task_struct *tsk, unsigned long adr);
extern int set_endian(struct task_struct *tsk, unsigned int val);

#define GET_UNALIGN_CTL(tsk, adr)	get_unalign_ctl((tsk), (adr))
#define SET_UNALIGN_CTL(tsk, val)	set_unalign_ctl((tsk), (val))

extern int get_unalign_ctl(struct task_struct *tsk, unsigned long adr);
extern int set_unalign_ctl(struct task_struct *tsk, unsigned int val);

extern void load_fp_state(struct thread_fp_state *fp);
extern void store_fp_state(struct thread_fp_state *fp);
extern void load_vr_state(struct thread_vr_state *vr);
extern void store_vr_state(struct thread_vr_state *vr);

static inline unsigned int __unpack_fe01(unsigned long msr_bits)
{
	return ((msr_bits & MSR_FE0) >> 10) | ((msr_bits & MSR_FE1) >> 8);
}

static inline unsigned long __pack_fe01(unsigned int fpmode)
{
	return ((fpmode << 10) & MSR_FE0) | ((fpmode << 8) & MSR_FE1);
}

#ifdef CONFIG_PPC64
#define cpu_relax()	do { HMT_low(); HMT_medium(); barrier(); } while (0)

#define spin_begin()	HMT_low()

#define spin_cpu_relax()	barrier()

#define spin_cpu_yield()	spin_cpu_relax()

#define spin_end()	HMT_medium()

#define spin_until_cond(cond)					\
do {								\
	if (unlikely(!(cond))) {				\
		spin_begin();					\
		do {						\
			spin_cpu_relax();			\
		} while (!(cond));				\
		spin_end();					\
	}							\
} while (0)

#else
#define cpu_relax()	barrier()
#endif

/* Check that a certain kernel stack pointer is valid in task_struct p */
int validate_sp(unsigned long sp, struct task_struct *p,
                       unsigned long nbytes);

/*
 * Prefetch macros.
 */
#define ARCH_HAS_PREFETCH
#define ARCH_HAS_PREFETCHW
#define ARCH_HAS_SPINLOCK_PREFETCH

static inline void prefetch(const void *x)
{
	if (unlikely(!x))
		return;

	__asm__ __volatile__ ("dcbt 0,%0" : : "r" (x));
}

static inline void prefetchw(const void *x)
{
	if (unlikely(!x))
		return;

	__asm__ __volatile__ ("dcbtst 0,%0" : : "r" (x));
}

#define spin_lock_prefetch(x)	prefetchw(x)

#define HAVE_ARCH_PICK_MMAP_LAYOUT

#ifdef CONFIG_PPC64
static inline unsigned long get_clean_sp(unsigned long sp, int is_32)
{
	if (is_32)
		return sp & 0x0ffffffffUL;
	return sp;
}
#else
static inline unsigned long get_clean_sp(unsigned long sp, int is_32)
{
	return sp;
}
#endif

extern unsigned long cpuidle_disable;
enum idle_boot_override {IDLE_NO_OVERRIDE = 0, IDLE_POWERSAVE_OFF};

extern int powersave_nap;	/* set if nap mode can be used in idle loop */
extern unsigned long power7_idle_insn(unsigned long type); /* PNV_THREAD_NAP/etc*/
extern void power7_idle_type(unsigned long type);
extern unsigned long power9_idle_stop(unsigned long psscr_val);
extern unsigned long power9_offline_stop(unsigned long psscr_val);
extern void power9_idle_type(unsigned long stop_psscr_val,
			      unsigned long stop_psscr_mask);

extern void flush_instruction_cache(void);
extern void hard_reset_now(void);
extern void poweroff_now(void);
extern int fix_alignment(struct pt_regs *);
extern void cvt_fd(float *from, double *to);
extern void cvt_df(double *from, float *to);
extern void _nmask_and_or_msr(unsigned long nmask, unsigned long or_val);

#ifdef CONFIG_PPC64
/*
 * We handle most unaligned accesses in hardware. On the other hand 
 * unaligned DMA can be very expensive on some ppc64 IO chips (it does
 * powers of 2 writes until it reaches sufficient alignment).
 *
 * Based on this we disable the IP header alignment in network drivers.
 */
#define NET_IP_ALIGN	0
#endif

#endif /* __KERNEL__ */
#endif /* __ASSEMBLY__ */
#endif /* _ASM_POWERPC_PROCESSOR_H */
