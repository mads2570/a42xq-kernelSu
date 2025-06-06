/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASMARM_ARCH_TIMER_H
#define __ASMARM_ARCH_TIMER_H

#include <asm/barrier.h>
#include <asm/errno.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/types.h>

#include <clocksource/arm_arch_timer.h>

#ifdef CONFIG_ARM_ARCH_TIMER

#ifdef CONFIG_ARM_ERRATUM_858921
DECLARE_PER_CPU(bool, timer_erratum_858921_workaround_enabled);
static __always_inline bool erratum_858921_workaround_enabled(void)
{
	return this_cpu_read(timer_erratum_858921_workaround_enabled);
}
#else
static __always_inline bool erratum_858921_workaround_enabled(void)
{
	return false;
}
#endif

int arch_timer_arch_init(void);

/*
 * These register accessors are marked inline so the compiler can
 * nicely work out which register we want, and chuck away the rest of
 * the code. At least it does so with a recent GCC (4.6.3).
 */
static __always_inline
void arch_timer_reg_write_cp15(int access, enum arch_timer_reg reg, u32 val)
{
	if (access == ARCH_TIMER_PHYS_ACCESS) {
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			asm volatile("mcr p15, 0, %0, c14, c2, 1" : : "r" (val));
			break;
		case ARCH_TIMER_REG_TVAL:
			asm volatile("mcr p15, 0, %0, c14, c2, 0" : : "r" (val));
			break;
		}
	} else if (access == ARCH_TIMER_VIRT_ACCESS) {
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			asm volatile("mcr p15, 0, %0, c14, c3, 1" : : "r" (val));
			break;
		case ARCH_TIMER_REG_TVAL:
			asm volatile("mcr p15, 0, %0, c14, c3, 0" : : "r" (val));
			break;
		}
	}

	isb();
}

static __always_inline
u32 arch_timer_reg_read_cp15(int access, enum arch_timer_reg reg)
{
	u32 val = 0;

	if (access == ARCH_TIMER_PHYS_ACCESS) {
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			asm volatile("mrc p15, 0, %0, c14, c2, 1" : "=r" (val));
			break;
		case ARCH_TIMER_REG_TVAL:
			asm volatile("mrc p15, 0, %0, c14, c2, 0" : "=r" (val));
			break;
		}
	} else if (access == ARCH_TIMER_VIRT_ACCESS) {
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			asm volatile("mrc p15, 0, %0, c14, c3, 1" : "=r" (val));
			break;
		case ARCH_TIMER_REG_TVAL:
			asm volatile("mrc p15, 0, %0, c14, c3, 0" : "=r" (val));
			break;
		}
	}

	return val;
}

static inline u32 arch_timer_get_cntfrq(void)
{
	u32 val;
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r" (val));
	return val;
}

#define L32_BITS 0x00000000FFFFFFFF
static inline u64 arch_counter_get_cntpct(void)
{
	u64 cval;

	isb();
	if (erratum_858921_workaround_enabled()) {
		do {
			asm volatile("mrrc p15, 0, %Q0, %R0, c14" : "=r"(cval));
		} while ((cval & L32_BITS) == L32_BITS);
	} else {
		asm volatile("mrrc p15, 0, %Q0, %R0, c14" : "=r" (cval));
	}
	return cval;
}

static inline u64 arch_counter_get_cntvct(void)
{
	u64 cval;

	isb();
	if (erratum_858921_workaround_enabled()) {
		do {
			asm volatile("mrrc p15, 1, %Q0, %R0, c14" : "=r"(cval));
		} while ((cval & L32_BITS) == L32_BITS);
	} else {
		asm volatile("mrrc p15, 1, %Q0, %R0, c14" : "=r" (cval));
	}
	return cval;
}

static inline u32 arch_timer_get_cntkctl(void)
{
	u32 cntkctl;
	asm volatile("mrc p15, 0, %0, c14, c1, 0" : "=r" (cntkctl));
	return cntkctl;
}

static inline void arch_timer_set_cntkctl(u32 cntkctl)
{
	asm volatile("mcr p15, 0, %0, c14, c1, 0" : : "r" (cntkctl));
	isb();
}

#endif

#endif
