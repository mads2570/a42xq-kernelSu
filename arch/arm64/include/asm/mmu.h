/*
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
#ifndef __ASM_MMU_H
#define __ASM_MMU_H

#define MMCF_AARCH32	0x1	/* mm context flag for AArch32 executables */
#define USER_ASID_BIT	48
#define USER_ASID_FLAG	(UL(1) << USER_ASID_BIT)
#define TTBR_ASID_MASK	(UL(0xffff) << 48)

#define BP_HARDEN_EL2_SLOTS 4

#ifndef __ASSEMBLY__

typedef struct {
	atomic64_t	id;
	void		*vdso;
	unsigned long	flags;
} mm_context_t;

/*
 * This macro is only used by the TLBI code, which cannot race with an
 * ASID change and therefore doesn't need to reload the counter using
 * atomic64_read.
 */
#define ASID(mm)	((mm)->context.id.counter & 0xffff)

static __always_inline bool arm64_kernel_unmapped_at_el0(void)
{
	return IS_ENABLED(CONFIG_UNMAP_KERNEL_AT_EL0) &&
	       cpus_have_const_cap(ARM64_UNMAP_KERNEL_AT_EL0);
}

typedef void (*bp_hardening_cb_t)(void);

struct bp_hardening_data {
	int			hyp_vectors_slot;
	bp_hardening_cb_t	fn;

	/*
	 * template_start is only used by the BHB mitigation to identify the
	 * hyp_vectors_slot sequence.
	 */
	const char *template_start;
};

#if (defined(CONFIG_HARDEN_BRANCH_PREDICTOR) ||	\
     defined(CONFIG_HARDEN_EL2_VECTORS))
extern char __bp_harden_hyp_vecs_start[], __bp_harden_hyp_vecs_end[];
extern atomic_t arm64_el2_vector_last_slot;
#endif  /* CONFIG_HARDEN_BRANCH_PREDICTOR || CONFIG_HARDEN_EL2_VECTORS */

#ifdef CONFIG_HARDEN_BRANCH_PREDICTOR
DECLARE_PER_CPU_READ_MOSTLY(struct bp_hardening_data, bp_hardening_data);

static inline struct bp_hardening_data *arm64_get_bp_hardening_data(void)
{
	return this_cpu_ptr(&bp_hardening_data);
}

static inline void arm64_apply_bp_hardening(void)
{
	struct bp_hardening_data *d;

	if (!cpus_have_const_cap(ARM64_HARDEN_BRANCH_PREDICTOR))
		return;

	d = arm64_get_bp_hardening_data();
	if (d->fn)
		d->fn();
}
#else
static inline struct bp_hardening_data *arm64_get_bp_hardening_data(void)
{
	return NULL;
}

static inline void arm64_apply_bp_hardening(void)	{ }
#endif	/* CONFIG_HARDEN_BRANCH_PREDICTOR */

extern void paging_init(void);
extern void bootmem_init(void);
extern void __iomem *early_io_map(phys_addr_t phys, unsigned long virt);
extern void init_mem_pgprot(void);
extern void create_pgd_mapping(struct mm_struct *mm, phys_addr_t phys,
			       unsigned long virt, phys_addr_t size,
			       pgprot_t prot, bool page_mappings_only);
extern void *fixmap_remap_fdt(phys_addr_t dt_phys, int *size, pgprot_t prot);
extern void mark_linear_text_alias_ro(void);
#ifdef CONFIG_MEMORY_HOTPLUG
extern void hotplug_paging(phys_addr_t start, phys_addr_t size);
#ifdef CONFIG_MEMORY_HOTREMOVE
extern void remove_pagetable(unsigned long start,
	unsigned long end, bool direct);
#endif
#endif

#endif	/* !__ASSEMBLY__ */
#endif
