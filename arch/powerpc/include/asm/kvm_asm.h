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
 * Copyright IBM Corp. 2008
 *
 * Authors: Hollis Blanchard <hollisb@us.ibm.com>
 */

#ifndef __POWERPC_KVM_ASM_H__
#define __POWERPC_KVM_ASM_H__

#ifdef __ASSEMBLY__
#ifdef CONFIG_64BIT
#define PPC_STD(sreg, offset, areg)  std sreg, (offset)(areg)
#define PPC_LD(treg, offset, areg)   ld treg, (offset)(areg)
#else
#define PPC_STD(sreg, offset, areg)  stw sreg, (offset+4)(areg)
#define PPC_LD(treg, offset, areg)   lwz treg, (offset+4)(areg)
#endif
#endif

/* IVPR must be 64KiB-aligned. */
#define VCPU_SIZE_ORDER 4
#define VCPU_SIZE_LOG   (VCPU_SIZE_ORDER + 12)
#define VCPU_SIZE_BYTES (1<<VCPU_SIZE_LOG)

#define BOOKE_INTERRUPT_CRITICAL 0
#define BOOKE_INTERRUPT_MACHINE_CHECK 1
#define BOOKE_INTERRUPT_DATA_STORAGE 2
#define BOOKE_INTERRUPT_INST_STORAGE 3
#define BOOKE_INTERRUPT_EXTERNAL 4
#define BOOKE_INTERRUPT_ALIGNMENT 5
#define BOOKE_INTERRUPT_PROGRAM 6
#define BOOKE_INTERRUPT_FP_UNAVAIL 7
#define BOOKE_INTERRUPT_SYSCALL 8
#define BOOKE_INTERRUPT_AP_UNAVAIL 9
#define BOOKE_INTERRUPT_DECREMENTER 10
#define BOOKE_INTERRUPT_FIT 11
#define BOOKE_INTERRUPT_WATCHDOG 12
#define BOOKE_INTERRUPT_DTLB_MISS 13
#define BOOKE_INTERRUPT_ITLB_MISS 14
#define BOOKE_INTERRUPT_DEBUG 15

/* E500 */
#ifdef CONFIG_SPE_POSSIBLE
#define BOOKE_INTERRUPT_SPE_UNAVAIL 32
#define BOOKE_INTERRUPT_SPE_FP_DATA 33
#define BOOKE_INTERRUPT_SPE_FP_ROUND 34
#endif

#ifdef CONFIG_PPC_E500MC
#define BOOKE_INTERRUPT_ALTIVEC_UNAVAIL 32
#define BOOKE_INTERRUPT_ALTIVEC_ASSIST 33
#endif

#define BOOKE_INTERRUPT_PERFORMANCE_MONITOR 35
#define BOOKE_INTERRUPT_DOORBELL 36
#define BOOKE_INTERRUPT_DOORBELL_CRITICAL 37

/* booke_hv */
#define BOOKE_INTERRUPT_GUEST_DBELL 38
#define BOOKE_INTERRUPT_GUEST_DBELL_CRIT 39
#define BOOKE_INTERRUPT_HV_SYSCALL 40
#define BOOKE_INTERRUPT_HV_PRIV 41
#define BOOKE_INTERRUPT_LRAT_ERROR 42

/* book3s */

#define BOOK3S_INTERRUPT_SYSTEM_RESET	0x100
#define BOOK3S_INTERRUPT_MACHINE_CHECK	0x200
#define BOOK3S_INTERRUPT_DATA_STORAGE	0x300
#define BOOK3S_INTERRUPT_DATA_SEGMENT	0x380
#define BOOK3S_INTERRUPT_INST_STORAGE	0x400
#define BOOK3S_INTERRUPT_INST_SEGMENT	0x480
#define BOOK3S_INTERRUPT_EXTERNAL	0x500
#define BOOK3S_INTERRUPT_EXTERNAL_LEVEL	0x501
#define BOOK3S_INTERRUPT_EXTERNAL_HV	0x502
#define BOOK3S_INTERRUPT_ALIGNMENT	0x600
#define BOOK3S_INTERRUPT_PROGRAM	0x700
#define BOOK3S_INTERRUPT_FP_UNAVAIL	0x800
#define BOOK3S_INTERRUPT_DECREMENTER	0x900
#define BOOK3S_INTERRUPT_HV_DECREMENTER	0x980
#define BOOK3S_INTERRUPT_DOORBELL	0xa00
#define BOOK3S_INTERRUPT_SYSCALL	0xc00
#define BOOK3S_INTERRUPT_TRACE		0xd00
#define BOOK3S_INTERRUPT_H_DATA_STORAGE	0xe00
#define BOOK3S_INTERRUPT_H_INST_STORAGE	0xe20
#define BOOK3S_INTERRUPT_H_EMUL_ASSIST	0xe40
#define BOOK3S_INTERRUPT_HMI		0xe60
#define BOOK3S_INTERRUPT_H_DOORBELL	0xe80
#define BOOK3S_INTERRUPT_H_VIRT		0xea0
#define BOOK3S_INTERRUPT_PERFMON	0xf00
#define BOOK3S_INTERRUPT_ALTIVEC	0xf20
#define BOOK3S_INTERRUPT_VSX		0xf40
#define BOOK3S_INTERRUPT_FAC_UNAVAIL	0xf60
#define BOOK3S_INTERRUPT_H_FAC_UNAVAIL	0xf80

/* book3s_hv */

#define BOOK3S_INTERRUPT_HV_SOFTPATCH	0x1500

/*
 * Special trap used to indicate to host that this is a
 * passthrough interrupt that could not be handled
 * completely in the guest.
 */
#define BOOK3S_INTERRUPT_HV_RM_HARD	0x5555

#define BOOK3S_IRQPRIO_SYSTEM_RESET		0
#define BOOK3S_IRQPRIO_DATA_SEGMENT		1
#define BOOK3S_IRQPRIO_INST_SEGMENT		2
#define BOOK3S_IRQPRIO_DATA_STORAGE		3
#define BOOK3S_IRQPRIO_INST_STORAGE		4
#define BOOK3S_IRQPRIO_ALIGNMENT		5
#define BOOK3S_IRQPRIO_PROGRAM			6
#define BOOK3S_IRQPRIO_FP_UNAVAIL		7
#define BOOK3S_IRQPRIO_ALTIVEC			8
#define BOOK3S_IRQPRIO_VSX			9
#define BOOK3S_IRQPRIO_FAC_UNAVAIL		10
#define BOOK3S_IRQPRIO_SYSCALL			11
#define BOOK3S_IRQPRIO_MACHINE_CHECK		12
#define BOOK3S_IRQPRIO_DEBUG			13
#define BOOK3S_IRQPRIO_EXTERNAL			14
#define BOOK3S_IRQPRIO_DECREMENTER		15
#define BOOK3S_IRQPRIO_PERFORMANCE_MONITOR	16
#define BOOK3S_IRQPRIO_EXTERNAL_LEVEL		17
#define BOOK3S_IRQPRIO_MAX			18

#define BOOK3S_HFLAG_DCBZ32			0x1
#define BOOK3S_HFLAG_SLB			0x2
#define BOOK3S_HFLAG_PAIRED_SINGLE		0x4
#define BOOK3S_HFLAG_NATIVE_PS			0x8
#define BOOK3S_HFLAG_MULTI_PGSIZE		0x10
#define BOOK3S_HFLAG_NEW_TLBIE			0x20
#define BOOK3S_HFLAG_SPLIT_HACK			0x40

#define RESUME_FLAG_NV          (1<<0)  /* Reload guest nonvolatile state? */
#define RESUME_FLAG_HOST        (1<<1)  /* Resume host? */
#define RESUME_FLAG_ARCH1	(1<<2)
#define RESUME_FLAG_ARCH2	(1<<3)

#define RESUME_GUEST            0
#define RESUME_GUEST_NV         RESUME_FLAG_NV
#define RESUME_HOST             RESUME_FLAG_HOST
#define RESUME_HOST_NV          (RESUME_FLAG_HOST|RESUME_FLAG_NV)

#define KVM_GUEST_MODE_NONE	0
#define KVM_GUEST_MODE_GUEST	1
#define KVM_GUEST_MODE_SKIP	2
#define KVM_GUEST_MODE_GUEST_HV	3
#define KVM_GUEST_MODE_HOST_HV	4

#define KVM_INST_FETCH_FAILED	-1

/* Extract PO and XOP opcode fields */
#define PO_XOP_OPCODE_MASK 0xfc0007fe

#endif /* __POWERPC_KVM_ASM_H__ */
