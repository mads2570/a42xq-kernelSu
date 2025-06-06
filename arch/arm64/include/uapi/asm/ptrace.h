/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Based on arch/arm/include/asm/ptrace.h
 *
 * Copyright (C) 1996-2003 Russell King
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _UAPI__ASM_PTRACE_H
#define _UAPI__ASM_PTRACE_H

#include <linux/types.h>

#include <asm/hwcap.h>
#include <asm/sigcontext.h>


/*
 * PSR bits
 */
#define PSR_MODE_EL0t	0x00000000
#define PSR_MODE_EL1t	0x00000004
#define PSR_MODE_EL1h	0x00000005
#define PSR_MODE_EL2t	0x00000008
#define PSR_MODE_EL2h	0x00000009
#define PSR_MODE_EL3t	0x0000000c
#define PSR_MODE_EL3h	0x0000000d
#define PSR_MODE_MASK	0x0000000f

/* AArch32 CPSR bits */
#define PSR_MODE32_BIT		0x00000010

/* AArch64 SPSR bits */
#define PSR_F_BIT	0x00000040
#define PSR_I_BIT	0x00000080
#define PSR_A_BIT	0x00000100
#define PSR_D_BIT	0x00000200
#define PSR_SSBS_BIT	0x00001000
#define PSR_PAN_BIT	0x00400000
#define PSR_UAO_BIT	0x00800000
#define PSR_DIT_BIT	0x01000000
#define PSR_V_BIT	0x10000000
#define PSR_C_BIT	0x20000000
#define PSR_Z_BIT	0x40000000
#define PSR_N_BIT	0x80000000

/*
 * Groups of PSR bits
 */
#define PSR_f		0xff000000	/* Flags		*/
#define PSR_s		0x00ff0000	/* Status		*/
#define PSR_x		0x0000ff00	/* Extension		*/
#define PSR_c		0x000000ff	/* Control		*/


#ifndef __ASSEMBLY__

/*
 * User structures for general purpose, floating point and debug registers.
 */
struct user_pt_regs {
	__u64		regs[31];
	__u64		sp;
	__u64		pc;
	__u64		pstate;
};

struct user_fpsimd_state {
	__uint128_t	vregs[32];
	__u32		fpsr;
	__u32		fpcr;
	__u32		__reserved[2];
};

struct user_hwdebug_state {
	__u32		dbg_info;
	__u32		pad;
	struct {
		__u64	addr;
		__u32	ctrl;
		__u32	pad;
	}		dbg_regs[16];
};

/* SVE/FP/SIMD state (NT_ARM_SVE) */

struct user_sve_header {
	__u32 size; /* total meaningful regset content in bytes */
	__u32 max_size; /* maxmium possible size for this thread */
	__u16 vl; /* current vector length */
	__u16 max_vl; /* maximum possible vector length */
	__u16 flags;
	__u16 __reserved;
};

/* Definitions for user_sve_header.flags: */
#define SVE_PT_REGS_MASK		(1 << 0)

#define SVE_PT_REGS_FPSIMD		0
#define SVE_PT_REGS_SVE			SVE_PT_REGS_MASK

/*
 * Common SVE_PT_* flags:
 * These must be kept in sync with prctl interface in <linux/prctl.h>
 */
#define SVE_PT_VL_INHERIT		((1 << 17) /* PR_SVE_VL_INHERIT */ >> 16)
#define SVE_PT_VL_ONEXEC		((1 << 18) /* PR_SVE_SET_VL_ONEXEC */ >> 16)


/*
 * The remainder of the SVE state follows struct user_sve_header.  The
 * total size of the SVE state (including header) depends on the
 * metadata in the header:  SVE_PT_SIZE(vq, flags) gives the total size
 * of the state in bytes, including the header.
 *
 * Refer to <asm/sigcontext.h> for details of how to pass the correct
 * "vq" argument to these macros.
 */

/* Offset from the start of struct user_sve_header to the register data */
#define SVE_PT_REGS_OFFSET					\
	((sizeof(struct user_sve_header) + (SVE_VQ_BYTES - 1))	\
		/ SVE_VQ_BYTES * SVE_VQ_BYTES)

/*
 * The register data content and layout depends on the value of the
 * flags field.
 */

/*
 * (flags & SVE_PT_REGS_MASK) == SVE_PT_REGS_FPSIMD case:
 *
 * The payload starts at offset SVE_PT_FPSIMD_OFFSET, and is of type
 * struct user_fpsimd_state.  Additional data might be appended in the
 * future: use SVE_PT_FPSIMD_SIZE(vq, flags) to compute the total size.
 * SVE_PT_FPSIMD_SIZE(vq, flags) will never be less than
 * sizeof(struct user_fpsimd_state).
 */

#define SVE_PT_FPSIMD_OFFSET		SVE_PT_REGS_OFFSET

#define SVE_PT_FPSIMD_SIZE(vq, flags)	(sizeof(struct user_fpsimd_state))

/*
 * (flags & SVE_PT_REGS_MASK) == SVE_PT_REGS_SVE case:
 *
 * The payload starts at offset SVE_PT_SVE_OFFSET, and is of size
 * SVE_PT_SVE_SIZE(vq, flags).
 *
 * Additional macros describe the contents and layout of the payload.
 * For each, SVE_PT_SVE_x_OFFSET(args) is the start offset relative to
 * the start of struct user_sve_header, and SVE_PT_SVE_x_SIZE(args) is
 * the size in bytes:
 *
 *	x	type				description
 *	-	----				-----------
 *	ZREGS		\
 *	ZREG		|
 *	PREGS		| refer to <asm/sigcontext.h>
 *	PREG		|
 *	FFR		/
 *
 *	FPSR	uint32_t			FPSR
 *	FPCR	uint32_t			FPCR
 *
 * Additional data might be appended in the future.
 */

#define SVE_PT_SVE_ZREG_SIZE(vq)	SVE_SIG_ZREG_SIZE(vq)
#define SVE_PT_SVE_PREG_SIZE(vq)	SVE_SIG_PREG_SIZE(vq)
#define SVE_PT_SVE_FFR_SIZE(vq)		SVE_SIG_FFR_SIZE(vq)
#define SVE_PT_SVE_FPSR_SIZE		sizeof(__u32)
#define SVE_PT_SVE_FPCR_SIZE		sizeof(__u32)

#define __SVE_SIG_TO_PT(offset) \
	((offset) - SVE_SIG_REGS_OFFSET + SVE_PT_REGS_OFFSET)

#define SVE_PT_SVE_OFFSET		SVE_PT_REGS_OFFSET

#define SVE_PT_SVE_ZREGS_OFFSET \
	__SVE_SIG_TO_PT(SVE_SIG_ZREGS_OFFSET)
#define SVE_PT_SVE_ZREG_OFFSET(vq, n) \
	__SVE_SIG_TO_PT(SVE_SIG_ZREG_OFFSET(vq, n))
#define SVE_PT_SVE_ZREGS_SIZE(vq) \
	(SVE_PT_SVE_ZREG_OFFSET(vq, SVE_NUM_ZREGS) - SVE_PT_SVE_ZREGS_OFFSET)

#define SVE_PT_SVE_PREGS_OFFSET(vq) \
	__SVE_SIG_TO_PT(SVE_SIG_PREGS_OFFSET(vq))
#define SVE_PT_SVE_PREG_OFFSET(vq, n) \
	__SVE_SIG_TO_PT(SVE_SIG_PREG_OFFSET(vq, n))
#define SVE_PT_SVE_PREGS_SIZE(vq) \
	(SVE_PT_SVE_PREG_OFFSET(vq, SVE_NUM_PREGS) - \
		SVE_PT_SVE_PREGS_OFFSET(vq))

#define SVE_PT_SVE_FFR_OFFSET(vq) \
	__SVE_SIG_TO_PT(SVE_SIG_FFR_OFFSET(vq))

#define SVE_PT_SVE_FPSR_OFFSET(vq)				\
	((SVE_PT_SVE_FFR_OFFSET(vq) + SVE_PT_SVE_FFR_SIZE(vq) +	\
			(SVE_VQ_BYTES - 1))			\
		/ SVE_VQ_BYTES * SVE_VQ_BYTES)
#define SVE_PT_SVE_FPCR_OFFSET(vq) \
	(SVE_PT_SVE_FPSR_OFFSET(vq) + SVE_PT_SVE_FPSR_SIZE)

/*
 * Any future extension appended after FPCR must be aligned to the next
 * 128-bit boundary.
 */

#define SVE_PT_SVE_SIZE(vq, flags)					\
	((SVE_PT_SVE_FPCR_OFFSET(vq) + SVE_PT_SVE_FPCR_SIZE		\
			- SVE_PT_SVE_OFFSET + (SVE_VQ_BYTES - 1))	\
		/ SVE_VQ_BYTES * SVE_VQ_BYTES)

#define SVE_PT_SIZE(vq, flags)						\
	 (((flags) & SVE_PT_REGS_MASK) == SVE_PT_REGS_SVE ?		\
		  SVE_PT_SVE_OFFSET + SVE_PT_SVE_SIZE(vq, flags)	\
		: SVE_PT_FPSIMD_OFFSET + SVE_PT_FPSIMD_SIZE(vq, flags))

#endif /* __ASSEMBLY__ */

#endif /* _UAPI__ASM_PTRACE_H */
