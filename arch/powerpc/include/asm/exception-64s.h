#ifndef _ASM_POWERPC_EXCEPTION_H
#define _ASM_POWERPC_EXCEPTION_H
/*
 * Extracted from head_64.S
 *
 *  PowerPC version
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *
 *  Rewritten by Cort Dougan (cort@cs.nmt.edu) for PReP
 *    Copyright (C) 1996 Cort Dougan <cort@cs.nmt.edu>
 *  Adapted for Power Macintosh by Paul Mackerras.
 *  Low-level exception handlers and MMU support
 *  rewritten by Paul Mackerras.
 *    Copyright (C) 1996 Paul Mackerras.
 *
 *  Adapted for 64bit PowerPC by Dave Engebretsen, Peter Bergner, and
 *    Mike Corrigan {engebret|bergner|mikejc}@us.ibm.com
 *
 *  This file contains the low-level support and setup for the
 *  PowerPC-64 platform, including trap and interrupt dispatch.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 */
/*
 * The following macros define the code that appears as
 * the prologue to each of the exception handlers.  They
 * are split into two parts to allow a single kernel binary
 * to be used for pSeries and iSeries.
 *
 * We make as much of the exception code common between native
 * exception handlers (including pSeries LPAR) and iSeries LPAR
 * implementations as possible.
 */
#include <asm/head-64.h>
#include <asm/feature-fixups.h>

/* PACA save area offsets (exgen, exmc, etc) */
#define EX_R9		0
#define EX_R10		8
#define EX_R11		16
#define EX_R12		24
#define EX_R13		32
#define EX_DAR		40
#define EX_DSISR	48
#define EX_CCR		52
#define EX_CFAR		56
#define EX_PPR		64
#define EX_LR		72
#if defined(CONFIG_RELOCATABLE)
#define EX_CTR		80
#define EX_SIZE		11	/* size in u64 units */
#else
#define EX_SIZE		10	/* size in u64 units */
#endif

/*
 * maximum recursive depth of MCE exceptions
 */
#define MAX_MCE_DEPTH	4

/*
 * EX_R3 is only used by the bad_stack handler. bad_stack reloads and
 * saves DAR from SPRN_DAR, and EX_DAR is not used. So EX_R3 can overlap
 * with EX_DAR.
 */
#define EX_R3		EX_DAR

#define STF_ENTRY_BARRIER_SLOT						\
	STF_ENTRY_BARRIER_FIXUP_SECTION;				\
	nop;								\
	nop;								\
	nop

#define STF_EXIT_BARRIER_SLOT						\
	STF_EXIT_BARRIER_FIXUP_SECTION;					\
	nop;								\
	nop;								\
	nop;								\
	nop;								\
	nop;								\
	nop

#define ENTRY_FLUSH_SLOT						\
	ENTRY_FLUSH_FIXUP_SECTION;					\
	nop;								\
	nop;								\
	nop;

/*
 * r10 must be free to use, r13 must be paca
 */
#define INTERRUPT_TO_KERNEL						\
	STF_ENTRY_BARRIER_SLOT;						\
	ENTRY_FLUSH_SLOT

/*
 * Macros for annotating the expected destination of (h)rfid
 *
 * The nop instructions allow us to insert one or more instructions to flush the
 * L1-D cache when returning to userspace or a guest.
 */
#define RFI_FLUSH_SLOT							\
	RFI_FLUSH_FIXUP_SECTION;					\
	nop;								\
	nop;								\
	nop

#define RFI_TO_KERNEL							\
	rfid

#define RFI_TO_USER							\
	STF_EXIT_BARRIER_SLOT;						\
	RFI_FLUSH_SLOT;							\
	rfid;								\
	b	rfi_flush_fallback

#define RFI_TO_USER_OR_KERNEL						\
	STF_EXIT_BARRIER_SLOT;						\
	RFI_FLUSH_SLOT;							\
	rfid;								\
	b	rfi_flush_fallback

#define RFI_TO_GUEST							\
	STF_EXIT_BARRIER_SLOT;						\
	RFI_FLUSH_SLOT;							\
	rfid;								\
	b	rfi_flush_fallback

#define HRFI_TO_KERNEL							\
	hrfid

#define HRFI_TO_USER							\
	STF_EXIT_BARRIER_SLOT;						\
	RFI_FLUSH_SLOT;							\
	hrfid;								\
	b	hrfi_flush_fallback

#define HRFI_TO_USER_OR_KERNEL						\
	STF_EXIT_BARRIER_SLOT;						\
	RFI_FLUSH_SLOT;							\
	hrfid;								\
	b	hrfi_flush_fallback

#define HRFI_TO_GUEST							\
	STF_EXIT_BARRIER_SLOT;						\
	RFI_FLUSH_SLOT;							\
	hrfid;								\
	b	hrfi_flush_fallback

#define HRFI_TO_UNKNOWN							\
	STF_EXIT_BARRIER_SLOT;						\
	RFI_FLUSH_SLOT;							\
	hrfid;								\
	b	hrfi_flush_fallback

#ifdef CONFIG_RELOCATABLE
#define __EXCEPTION_PROLOG_2_RELON(label, h)				\
	mfspr	r11,SPRN_##h##SRR0;	/* save SRR0 */			\
	LOAD_HANDLER(r12,label);					\
	mtctr	r12;							\
	mfspr	r12,SPRN_##h##SRR1;	/* and SRR1 */			\
	li	r10,MSR_RI;						\
	mtmsrd 	r10,1;			/* Set RI (EE=0) */		\
	bctr;
#else
/* If not relocatable, we can jump directly -- and save messing with LR */
#define __EXCEPTION_PROLOG_2_RELON(label, h)				\
	mfspr	r11,SPRN_##h##SRR0;	/* save SRR0 */			\
	mfspr	r12,SPRN_##h##SRR1;	/* and SRR1 */			\
	li	r10,MSR_RI;						\
	mtmsrd 	r10,1;			/* Set RI (EE=0) */		\
	b	label;
#endif
#define EXCEPTION_PROLOG_2_RELON(label, h)				\
	__EXCEPTION_PROLOG_2_RELON(label, h)

/*
 * As EXCEPTION_PROLOG(), except we've already got relocation on so no need to
 * rfid. Save LR in case we're CONFIG_RELOCATABLE, in which case
 * EXCEPTION_PROLOG_2_RELON will be using LR.
 */
#define EXCEPTION_RELON_PROLOG(area, label, h, extra, vec)		\
	SET_SCRATCH0(r13);		/* save r13 */			\
	EXCEPTION_PROLOG_0(area);					\
	EXCEPTION_PROLOG_1(area, extra, vec);				\
	EXCEPTION_PROLOG_2_RELON(label, h)

/*
 * We're short on space and time in the exception prolog, so we can't
 * use the normal LOAD_REG_IMMEDIATE macro to load the address of label.
 * Instead we get the base of the kernel from paca->kernelbase and or in the low
 * part of label. This requires that the label be within 64KB of kernelbase, and
 * that kernelbase be 64K aligned.
 */
#define LOAD_HANDLER(reg, label)					\
	ld	reg,PACAKBASE(r13);	/* get high part of &label */	\
	ori	reg,reg,FIXED_SYMBOL_ABS_ADDR(label);

#define __LOAD_HANDLER(reg, label)					\
	ld	reg,PACAKBASE(r13);					\
	ori	reg,reg,(ABS_ADDR(label))@l;

/*
 * Branches from unrelocated code (e.g., interrupts) to labels outside
 * head-y require >64K offsets.
 */
#define __LOAD_FAR_HANDLER(reg, label)					\
	ld	reg,PACAKBASE(r13);					\
	ori	reg,reg,(ABS_ADDR(label))@l;				\
	addis	reg,reg,(ABS_ADDR(label))@h;

/* Exception register prefixes */
#define EXC_HV	H
#define EXC_STD

#if defined(CONFIG_RELOCATABLE)
/*
 * If we support interrupts with relocation on AND we're a relocatable kernel,
 * we need to use CTR to get to the 2nd level handler.  So, save/restore it
 * when required.
 */
#define SAVE_CTR(reg, area)	mfctr	reg ; 	std	reg,area+EX_CTR(r13)
#define GET_CTR(reg, area) 			ld	reg,area+EX_CTR(r13)
#define RESTORE_CTR(reg, area)	ld	reg,area+EX_CTR(r13) ; mtctr reg
#else
/* ...else CTR is unused and in register. */
#define SAVE_CTR(reg, area)
#define GET_CTR(reg, area) 	mfctr	reg
#define RESTORE_CTR(reg, area)
#endif

/*
 * PPR save/restore macros used in exceptions_64s.S  
 * Used for P7 or later processors
 */
#define SAVE_PPR(area, ra)						\
BEGIN_FTR_SECTION_NESTED(940)						\
	ld	ra,area+EX_PPR(r13);	/* Read PPR from paca */	\
	std	ra,RESULT(r1);		/* Store PPR in RESULT for now */ \
END_FTR_SECTION_NESTED(CPU_FTR_HAS_PPR,CPU_FTR_HAS_PPR,940)

/*
 * This is called after we are finished accessing 'area', so we can now take
 * SLB faults accessing the thread struct, which will use PACA_EXSLB area.
 * This is required because the large_addr_slb handler uses EXSLB and it also
 * uses the common exception macros including this PPR saving.
 */
#define MOVE_PPR_TO_THREAD(ra, rb)					\
BEGIN_FTR_SECTION_NESTED(940)						\
	ld	ra,PACACURRENT(r13);					\
	ld	rb,RESULT(r1);		/* Read PPR from stack */	\
	std	rb,TASKTHREADPPR(ra);					\
END_FTR_SECTION_NESTED(CPU_FTR_HAS_PPR,CPU_FTR_HAS_PPR,940)

#define RESTORE_PPR_PACA(area, ra)					\
BEGIN_FTR_SECTION_NESTED(941)						\
	ld	ra,area+EX_PPR(r13);					\
	mtspr	SPRN_PPR,ra;						\
END_FTR_SECTION_NESTED(CPU_FTR_HAS_PPR,CPU_FTR_HAS_PPR,941)

/*
 * Get an SPR into a register if the CPU has the given feature
 */
#define OPT_GET_SPR(ra, spr, ftr)					\
BEGIN_FTR_SECTION_NESTED(943)						\
	mfspr	ra,spr;							\
END_FTR_SECTION_NESTED(ftr,ftr,943)

/*
 * Set an SPR from a register if the CPU has the given feature
 */
#define OPT_SET_SPR(ra, spr, ftr)					\
BEGIN_FTR_SECTION_NESTED(943)						\
	mtspr	spr,ra;							\
END_FTR_SECTION_NESTED(ftr,ftr,943)

/*
 * Save a register to the PACA if the CPU has the given feature
 */
#define OPT_SAVE_REG_TO_PACA(offset, ra, ftr)				\
BEGIN_FTR_SECTION_NESTED(943)						\
	std	ra,offset(r13);						\
END_FTR_SECTION_NESTED(ftr,ftr,943)

#define EXCEPTION_PROLOG_0(area)					\
	GET_PACA(r13);							\
	std	r9,area+EX_R9(r13);	/* save r9 */			\
	OPT_GET_SPR(r9, SPRN_PPR, CPU_FTR_HAS_PPR);			\
	HMT_MEDIUM;							\
	std	r10,area+EX_R10(r13);	/* save r10 - r12 */		\
	OPT_GET_SPR(r10, SPRN_CFAR, CPU_FTR_CFAR)

#define __EXCEPTION_PROLOG_1_PRE(area)					\
	OPT_SAVE_REG_TO_PACA(area+EX_PPR, r9, CPU_FTR_HAS_PPR);		\
	OPT_SAVE_REG_TO_PACA(area+EX_CFAR, r10, CPU_FTR_CFAR);		\
	INTERRUPT_TO_KERNEL;						\
	SAVE_CTR(r10, area);						\
	mfcr	r9;

#define __EXCEPTION_PROLOG_1_POST(area)					\
	std	r11,area+EX_R11(r13);					\
	std	r12,area+EX_R12(r13);					\
	GET_SCRATCH0(r10);						\
	std	r10,area+EX_R13(r13)

/*
 * This version of the EXCEPTION_PROLOG_1 will carry
 * addition parameter called "bitmask" to support
 * checking of the interrupt maskable level in the SOFTEN_TEST.
 * Intended to be used in MASKABLE_EXCPETION_* macros.
 */
#define MASKABLE_EXCEPTION_PROLOG_1(area, extra, vec, bitmask)			\
	__EXCEPTION_PROLOG_1_PRE(area);					\
	extra(vec, bitmask);						\
	__EXCEPTION_PROLOG_1_POST(area);

/*
 * This version of the EXCEPTION_PROLOG_1 is intended
 * to be used in STD_EXCEPTION* macros
 */
#define _EXCEPTION_PROLOG_1(area, extra, vec)				\
	__EXCEPTION_PROLOG_1_PRE(area);					\
	extra(vec);							\
	__EXCEPTION_PROLOG_1_POST(area);

#define EXCEPTION_PROLOG_1(area, extra, vec)				\
	_EXCEPTION_PROLOG_1(area, extra, vec)

#define __EXCEPTION_PROLOG_2(label, h)					\
	ld	r10,PACAKMSR(r13);	/* get MSR value for kernel */	\
	mfspr	r11,SPRN_##h##SRR0;	/* save SRR0 */			\
	LOAD_HANDLER(r12,label)						\
	mtspr	SPRN_##h##SRR0,r12;					\
	mfspr	r12,SPRN_##h##SRR1;	/* and SRR1 */			\
	mtspr	SPRN_##h##SRR1,r10;					\
	h##RFI_TO_KERNEL;						\
	b	.	/* prevent speculative execution */
#define EXCEPTION_PROLOG_2(label, h)					\
	__EXCEPTION_PROLOG_2(label, h)

/* _NORI variant keeps MSR_RI clear */
#define __EXCEPTION_PROLOG_2_NORI(label, h)				\
	ld	r10,PACAKMSR(r13);	/* get MSR value for kernel */	\
	xori	r10,r10,MSR_RI;		/* Clear MSR_RI */		\
	mfspr	r11,SPRN_##h##SRR0;	/* save SRR0 */			\
	LOAD_HANDLER(r12,label)						\
	mtspr	SPRN_##h##SRR0,r12;					\
	mfspr	r12,SPRN_##h##SRR1;	/* and SRR1 */			\
	mtspr	SPRN_##h##SRR1,r10;					\
	h##RFI_TO_KERNEL;						\
	b	.	/* prevent speculative execution */

#define EXCEPTION_PROLOG_2_NORI(label, h)				\
	__EXCEPTION_PROLOG_2_NORI(label, h)

#define EXCEPTION_PROLOG(area, label, h, extra, vec)			\
	SET_SCRATCH0(r13);		/* save r13 */			\
	EXCEPTION_PROLOG_0(area);					\
	EXCEPTION_PROLOG_1(area, extra, vec);				\
	EXCEPTION_PROLOG_2(label, h);

#define __KVMTEST(h, n)							\
	lbz	r10,HSTATE_IN_GUEST(r13);				\
	cmpwi	r10,0;							\
	bne	do_kvm_##h##n

#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
/*
 * If hv is possible, interrupts come into to the hv version
 * of the kvmppc_interrupt code, which then jumps to the PR handler,
 * kvmppc_interrupt_pr, if the guest is a PR guest.
 */
#define kvmppc_interrupt kvmppc_interrupt_hv
#else
#define kvmppc_interrupt kvmppc_interrupt_pr
#endif

/*
 * Branch to label using its 0xC000 address. This results in instruction
 * address suitable for MSR[IR]=0 or 1, which allows relocation to be turned
 * on using mtmsr rather than rfid.
 *
 * This could set the 0xc bits for !RELOCATABLE as an immediate, rather than
 * load KBASE for a slight optimisation.
 */
#define BRANCH_TO_C000(reg, label)					\
	__LOAD_HANDLER(reg, label);					\
	mtctr	reg;							\
	bctr

#ifdef CONFIG_RELOCATABLE
#define BRANCH_TO_COMMON(reg, label)					\
	__LOAD_HANDLER(reg, label);					\
	mtctr	reg;							\
	bctr

#define BRANCH_LINK_TO_FAR(label)					\
	__LOAD_FAR_HANDLER(r12, label);					\
	mtctr	r12;							\
	bctrl

/*
 * KVM requires __LOAD_FAR_HANDLER.
 *
 * __BRANCH_TO_KVM_EXIT branches are also a special case because they
 * explicitly use r9 then reload it from PACA before branching. Hence
 * the double-underscore.
 */
#define __BRANCH_TO_KVM_EXIT(area, label)				\
	mfctr	r9;							\
	std	r9,HSTATE_SCRATCH1(r13);				\
	__LOAD_FAR_HANDLER(r9, label);					\
	mtctr	r9;							\
	ld	r9,area+EX_R9(r13);					\
	bctr

#else
#define BRANCH_TO_COMMON(reg, label)					\
	b	label

#define BRANCH_LINK_TO_FAR(label)					\
	bl	label

#define __BRANCH_TO_KVM_EXIT(area, label)				\
	ld	r9,area+EX_R9(r13);					\
	b	label

#endif

/* Do not enable RI */
#define EXCEPTION_PROLOG_NORI(area, label, h, extra, vec)		\
	EXCEPTION_PROLOG_0(area);					\
	EXCEPTION_PROLOG_1(area, extra, vec);				\
	EXCEPTION_PROLOG_2_NORI(label, h);


#define __KVM_HANDLER(area, h, n)					\
	BEGIN_FTR_SECTION_NESTED(947)					\
	ld	r10,area+EX_CFAR(r13);					\
	std	r10,HSTATE_CFAR(r13);					\
	END_FTR_SECTION_NESTED(CPU_FTR_CFAR,CPU_FTR_CFAR,947);		\
	BEGIN_FTR_SECTION_NESTED(948)					\
	ld	r10,area+EX_PPR(r13);					\
	std	r10,HSTATE_PPR(r13);					\
	END_FTR_SECTION_NESTED(CPU_FTR_HAS_PPR,CPU_FTR_HAS_PPR,948);	\
	ld	r10,area+EX_R10(r13);					\
	std	r12,HSTATE_SCRATCH0(r13);				\
	sldi	r12,r9,32;						\
	ori	r12,r12,(n);						\
	/* This reloads r9 before branching to kvmppc_interrupt */	\
	__BRANCH_TO_KVM_EXIT(area, kvmppc_interrupt)

#define __KVM_HANDLER_SKIP(area, h, n)					\
	cmpwi	r10,KVM_GUEST_MODE_SKIP;				\
	beq	89f;							\
	BEGIN_FTR_SECTION_NESTED(948)					\
	ld	r10,area+EX_PPR(r13);					\
	std	r10,HSTATE_PPR(r13);					\
	END_FTR_SECTION_NESTED(CPU_FTR_HAS_PPR,CPU_FTR_HAS_PPR,948);	\
	ld	r10,area+EX_R10(r13);					\
	std	r12,HSTATE_SCRATCH0(r13);				\
	sldi	r12,r9,32;						\
	ori	r12,r12,(n);						\
	/* This reloads r9 before branching to kvmppc_interrupt */	\
	__BRANCH_TO_KVM_EXIT(area, kvmppc_interrupt);			\
89:	mtocrf	0x80,r9;						\
	ld	r9,area+EX_R9(r13);					\
	ld	r10,area+EX_R10(r13);					\
	b	kvmppc_skip_##h##interrupt

#ifdef CONFIG_KVM_BOOK3S_64_HANDLER
#define KVMTEST(h, n)			__KVMTEST(h, n)
#define KVM_HANDLER(area, h, n)		__KVM_HANDLER(area, h, n)
#define KVM_HANDLER_SKIP(area, h, n)	__KVM_HANDLER_SKIP(area, h, n)

#else
#define KVMTEST(h, n)
#define KVM_HANDLER(area, h, n)
#define KVM_HANDLER_SKIP(area, h, n)
#endif

#define NOTEST(n)

#define EXCEPTION_PROLOG_COMMON_1()					   \
	std	r9,_CCR(r1);		/* save CR in stackframe	*/ \
	std	r11,_NIP(r1);		/* save SRR0 in stackframe	*/ \
	std	r12,_MSR(r1);		/* save SRR1 in stackframe	*/ \
	std	r10,0(r1);		/* make stack chain pointer	*/ \
	std	r0,GPR0(r1);		/* save r0 in stackframe	*/ \
	std	r10,GPR1(r1);		/* save r1 in stackframe	*/ \


/*
 * The common exception prolog is used for all except a few exceptions
 * such as a segment miss on a kernel address.  We have to be prepared
 * to take another exception from the point where we first touch the
 * kernel stack onwards.
 *
 * On entry r13 points to the paca, r9-r13 are saved in the paca,
 * r9 contains the saved CR, r11 and r12 contain the saved SRR0 and
 * SRR1, and relocation is on.
 */
#define EXCEPTION_PROLOG_COMMON(n, area)				   \
	andi.	r10,r12,MSR_PR;		/* See if coming from user	*/ \
	mr	r10,r1;			/* Save r1			*/ \
	subi	r1,r1,INT_FRAME_SIZE;	/* alloc frame on kernel stack	*/ \
	beq-	1f;							   \
	ld	r1,PACAKSAVE(r13);	/* kernel stack to use		*/ \
1:	cmpdi	cr1,r1,-INT_FRAME_SIZE;	/* check if r1 is in userspace	*/ \
	blt+	cr1,3f;			/* abort if it is		*/ \
	li	r1,(n);			/* will be reloaded later	*/ \
	sth	r1,PACA_TRAP_SAVE(r13);					   \
	std	r3,area+EX_R3(r13);					   \
	addi	r3,r13,area;		/* r3 -> where regs are saved*/	   \
	RESTORE_CTR(r1, area);						   \
	b	bad_stack;						   \
3:	EXCEPTION_PROLOG_COMMON_1();					   \
	beq	4f;			/* if from kernel mode		*/ \
	ACCOUNT_CPU_USER_ENTRY(r13, r9, r10);				   \
	SAVE_PPR(area, r9);						   \
4:	EXCEPTION_PROLOG_COMMON_2(area)					   \
	beq	5f;			/* if from kernel mode		*/ \
	MOVE_PPR_TO_THREAD(r9, r10);					   \
5:	EXCEPTION_PROLOG_COMMON_3(n)					   \
	ACCOUNT_STOLEN_TIME

/* Save original regs values from save area to stack frame. */
#define EXCEPTION_PROLOG_COMMON_2(area)					   \
	ld	r9,area+EX_R9(r13);	/* move r9, r10 to stackframe	*/ \
	ld	r10,area+EX_R10(r13);					   \
	std	r9,GPR9(r1);						   \
	std	r10,GPR10(r1);						   \
	ld	r9,area+EX_R11(r13);	/* move r11 - r13 to stackframe	*/ \
	ld	r10,area+EX_R12(r13);					   \
	ld	r11,area+EX_R13(r13);					   \
	std	r9,GPR11(r1);						   \
	std	r10,GPR12(r1);						   \
	std	r11,GPR13(r1);						   \
	BEGIN_FTR_SECTION_NESTED(66);					   \
	ld	r10,area+EX_CFAR(r13);					   \
	std	r10,ORIG_GPR3(r1);					   \
	END_FTR_SECTION_NESTED(CPU_FTR_CFAR, CPU_FTR_CFAR, 66);		   \
	GET_CTR(r10, area);						   \
	std	r10,_CTR(r1);

#define EXCEPTION_PROLOG_COMMON_3(n)					   \
	std	r2,GPR2(r1);		/* save r2 in stackframe	*/ \
	SAVE_4GPRS(3, r1);		/* save r3 - r6 in stackframe   */ \
	SAVE_2GPRS(7, r1);		/* save r7, r8 in stackframe	*/ \
	mflr	r9;			/* Get LR, later save to stack	*/ \
	ld	r2,PACATOC(r13);	/* get kernel TOC into r2	*/ \
	std	r9,_LINK(r1);						   \
	lbz	r10,PACAIRQSOFTMASK(r13);				   \
	mfspr	r11,SPRN_XER;		/* save XER in stackframe	*/ \
	std	r10,SOFTE(r1);						   \
	std	r11,_XER(r1);						   \
	li	r9,(n)+1;						   \
	std	r9,_TRAP(r1);		/* set trap number		*/ \
	li	r10,0;							   \
	ld	r11,exception_marker@toc(r2);				   \
	std	r10,RESULT(r1);		/* clear regs->result		*/ \
	std	r11,STACK_FRAME_OVERHEAD-16(r1); /* mark the frame	*/

/*
 * Exception vectors.
 */
#define STD_EXCEPTION(vec, label)				\
	EXCEPTION_PROLOG(PACA_EXGEN, label, EXC_STD, KVMTEST_PR, vec);

/* Version of above for when we have to branch out-of-line */
#define __OOL_EXCEPTION(vec, label, hdlr)			\
	SET_SCRATCH0(r13)					\
	EXCEPTION_PROLOG_0(PACA_EXGEN)				\
	b hdlr;

#define STD_EXCEPTION_OOL(vec, label)				\
	EXCEPTION_PROLOG_1(PACA_EXGEN, KVMTEST_PR, vec);	\
	EXCEPTION_PROLOG_2(label, EXC_STD)

#define STD_EXCEPTION_HV(loc, vec, label)			\
	EXCEPTION_PROLOG(PACA_EXGEN, label, EXC_HV, KVMTEST_HV, vec);

#define STD_EXCEPTION_HV_OOL(vec, label)			\
	EXCEPTION_PROLOG_1(PACA_EXGEN, KVMTEST_HV, vec);	\
	EXCEPTION_PROLOG_2(label, EXC_HV)

#define STD_RELON_EXCEPTION(loc, vec, label)		\
	/* No guest interrupts come through here */	\
	EXCEPTION_RELON_PROLOG(PACA_EXGEN, label, EXC_STD, NOTEST, vec);

#define STD_RELON_EXCEPTION_OOL(vec, label)			\
	EXCEPTION_PROLOG_1(PACA_EXGEN, NOTEST, vec);		\
	EXCEPTION_PROLOG_2_RELON(label, EXC_STD)

#define STD_RELON_EXCEPTION_HV(loc, vec, label)		\
	EXCEPTION_RELON_PROLOG(PACA_EXGEN, label, EXC_HV, KVMTEST_HV, vec);

#define STD_RELON_EXCEPTION_HV_OOL(vec, label)			\
	EXCEPTION_PROLOG_1(PACA_EXGEN, KVMTEST_HV, vec);	\
	EXCEPTION_PROLOG_2_RELON(label, EXC_HV)

/* This associate vector numbers with bits in paca->irq_happened */
#define SOFTEN_VALUE_0x500	PACA_IRQ_EE
#define SOFTEN_VALUE_0x900	PACA_IRQ_DEC
#define SOFTEN_VALUE_0x980	PACA_IRQ_DEC
#define SOFTEN_VALUE_0xa00	PACA_IRQ_DBELL
#define SOFTEN_VALUE_0xe80	PACA_IRQ_DBELL
#define SOFTEN_VALUE_0xe60	PACA_IRQ_HMI
#define SOFTEN_VALUE_0xea0	PACA_IRQ_EE
#define SOFTEN_VALUE_0xf00	PACA_IRQ_PMI

#define __SOFTEN_TEST(h, vec, bitmask)					\
	lbz	r10,PACAIRQSOFTMASK(r13);				\
	andi.	r10,r10,bitmask;					\
	li	r10,SOFTEN_VALUE_##vec;					\
	bne	masked_##h##interrupt

#define _SOFTEN_TEST(h, vec, bitmask)	__SOFTEN_TEST(h, vec, bitmask)

#define SOFTEN_TEST_PR(vec, bitmask)					\
	KVMTEST(EXC_STD, vec);						\
	_SOFTEN_TEST(EXC_STD, vec, bitmask)

#define SOFTEN_TEST_HV(vec, bitmask)					\
	KVMTEST(EXC_HV, vec);						\
	_SOFTEN_TEST(EXC_HV, vec, bitmask)

#define KVMTEST_PR(vec)							\
	KVMTEST(EXC_STD, vec)

#define KVMTEST_HV(vec)							\
	KVMTEST(EXC_HV, vec)

#define SOFTEN_NOTEST_PR(vec, bitmask)	_SOFTEN_TEST(EXC_STD, vec, bitmask)
#define SOFTEN_NOTEST_HV(vec, bitmask)	_SOFTEN_TEST(EXC_HV, vec, bitmask)

#define __MASKABLE_EXCEPTION(vec, label, h, extra, bitmask)		\
	SET_SCRATCH0(r13);    /* save r13 */				\
	EXCEPTION_PROLOG_0(PACA_EXGEN);					\
	MASKABLE_EXCEPTION_PROLOG_1(PACA_EXGEN, extra, vec, bitmask);	\
	EXCEPTION_PROLOG_2(label, h);

#define MASKABLE_EXCEPTION(vec, label, bitmask)				\
	__MASKABLE_EXCEPTION(vec, label, EXC_STD, SOFTEN_TEST_PR, bitmask)

#define MASKABLE_EXCEPTION_OOL(vec, label, bitmask)			\
	MASKABLE_EXCEPTION_PROLOG_1(PACA_EXGEN, SOFTEN_TEST_PR, vec, bitmask);\
	EXCEPTION_PROLOG_2(label, EXC_STD)

#define MASKABLE_EXCEPTION_HV(vec, label, bitmask)			\
	__MASKABLE_EXCEPTION(vec, label, EXC_HV, SOFTEN_TEST_HV, bitmask)

#define MASKABLE_EXCEPTION_HV_OOL(vec, label, bitmask)			\
	MASKABLE_EXCEPTION_PROLOG_1(PACA_EXGEN, SOFTEN_TEST_HV, vec, bitmask);\
	EXCEPTION_PROLOG_2(label, EXC_HV)

#define __MASKABLE_RELON_EXCEPTION(vec, label, h, extra, bitmask)	\
	SET_SCRATCH0(r13);    /* save r13 */				\
	EXCEPTION_PROLOG_0(PACA_EXGEN);					\
	MASKABLE_EXCEPTION_PROLOG_1(PACA_EXGEN, extra, vec, bitmask);	\
	EXCEPTION_PROLOG_2_RELON(label, h)

#define MASKABLE_RELON_EXCEPTION(vec, label, bitmask)			\
	__MASKABLE_RELON_EXCEPTION(vec, label, EXC_STD, SOFTEN_NOTEST_PR, bitmask)

#define MASKABLE_RELON_EXCEPTION_OOL(vec, label, bitmask)		\
	MASKABLE_EXCEPTION_PROLOG_1(PACA_EXGEN, SOFTEN_NOTEST_PR, vec, bitmask);\
	EXCEPTION_PROLOG_2(label, EXC_STD);

#define MASKABLE_RELON_EXCEPTION_HV(vec, label, bitmask)		\
	__MASKABLE_RELON_EXCEPTION(vec, label, EXC_HV, SOFTEN_TEST_HV, bitmask)

#define MASKABLE_RELON_EXCEPTION_HV_OOL(vec, label, bitmask)		\
	MASKABLE_EXCEPTION_PROLOG_1(PACA_EXGEN, SOFTEN_TEST_HV, vec, bitmask);\
	EXCEPTION_PROLOG_2_RELON(label, EXC_HV)

/*
 * Our exception common code can be passed various "additions"
 * to specify the behaviour of interrupts, whether to kick the
 * runlatch, etc...
 */

/*
 * This addition reconciles our actual IRQ state with the various software
 * flags that track it. This may call C code.
 */
#define ADD_RECONCILE	RECONCILE_IRQ_STATE(r10,r11)

#define ADD_NVGPRS				\
	bl	save_nvgprs

#define RUNLATCH_ON				\
BEGIN_FTR_SECTION				\
	CURRENT_THREAD_INFO(r3, r1);		\
	ld	r4,TI_LOCAL_FLAGS(r3);		\
	andi.	r0,r4,_TLF_RUNLATCH;		\
	beql	ppc64_runlatch_on_trampoline;	\
END_FTR_SECTION_IFSET(CPU_FTR_CTRL)

#define EXCEPTION_COMMON(area, trap, label, hdlr, ret, additions) \
	EXCEPTION_PROLOG_COMMON(trap, area);			\
	/* Volatile regs are potentially clobbered here */	\
	additions;						\
	addi	r3,r1,STACK_FRAME_OVERHEAD;			\
	bl	hdlr;						\
	b	ret

/*
 * Exception where stack is already set in r1, r1 is saved in r10, and it
 * continues rather than returns.
 */
#define EXCEPTION_COMMON_NORET_STACK(area, trap, label, hdlr, additions) \
	EXCEPTION_PROLOG_COMMON_1();				\
	EXCEPTION_PROLOG_COMMON_2(area);			\
	EXCEPTION_PROLOG_COMMON_3(trap);			\
	/* Volatile regs are potentially clobbered here */	\
	additions;						\
	addi	r3,r1,STACK_FRAME_OVERHEAD;			\
	bl	hdlr

#define STD_EXCEPTION_COMMON(trap, label, hdlr)			\
	EXCEPTION_COMMON(PACA_EXGEN, trap, label, hdlr,		\
		ret_from_except, ADD_NVGPRS;ADD_RECONCILE)

/*
 * Like STD_EXCEPTION_COMMON, but for exceptions that can occur
 * in the idle task and therefore need the special idle handling
 * (finish nap and runlatch)
 */
#define STD_EXCEPTION_COMMON_ASYNC(trap, label, hdlr)		\
	EXCEPTION_COMMON(PACA_EXGEN, trap, label, hdlr,		\
		ret_from_except_lite, FINISH_NAP;ADD_RECONCILE;RUNLATCH_ON)

/*
 * When the idle code in power4_idle puts the CPU into NAP mode,
 * it has to do so in a loop, and relies on the external interrupt
 * and decrementer interrupt entry code to get it out of the loop.
 * It sets the _TLF_NAPPING bit in current_thread_info()->local_flags
 * to signal that it is in the loop and needs help to get out.
 */
#ifdef CONFIG_PPC_970_NAP
#define FINISH_NAP				\
BEGIN_FTR_SECTION				\
	CURRENT_THREAD_INFO(r11, r1);		\
	ld	r9,TI_LOCAL_FLAGS(r11);		\
	andi.	r10,r9,_TLF_NAPPING;		\
	bnel	power4_fixup_nap;		\
END_FTR_SECTION_IFSET(CPU_FTR_CAN_NAP)
#else
#define FINISH_NAP
#endif

#endif	/* _ASM_POWERPC_EXCEPTION_H */
