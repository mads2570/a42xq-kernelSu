/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_VDSO_VSYSCALL_H
#define __ASM_VDSO_VSYSCALL_H

#ifndef __ASSEMBLY__

#include <linux/timekeeper_internal.h>
#include <vdso/datapage.h>

extern struct vdso_data *vdso_data;

/*
 * Update the vDSO data page to keep in sync with kernel timekeeping.
 */
static __always_inline
struct vdso_data *__mips_get_k_vdso_data(void)
{
	return vdso_data;
}
#define __arch_get_k_vdso_data __mips_get_k_vdso_data

static __always_inline
int __mips_get_clock_mode(struct timekeeper *tk)
{
	u32 clock_mode = tk->tkr_mono.clock->archdata.vdso_clock_mode;

	return clock_mode;
}
#define __arch_get_clock_mode __mips_get_clock_mode

/* The asm-generic header needs to be included after the definitions above */
#include <asm-generic/vdso/vsyscall.h>

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_VDSO_VSYSCALL_H */
