/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_BOOK3S_64_HASH_4K_H
#define _ASM_POWERPC_BOOK3S_64_HASH_4K_H
/*
 * Entries per page directory level.  The PTE level must use a 64b record
 * for each page table entry.  The PMD and PGD level use a 32b record for
 * each entry by assuming that each entry is page aligned.
 */
#define H_PTE_INDEX_SIZE  9
#define H_PMD_INDEX_SIZE  7
#define H_PUD_INDEX_SIZE  9
#define H_PGD_INDEX_SIZE  9

/*
 * Each context is 512TB. But on 4k we restrict our max TASK size to 64TB
 * Hence also limit max EA bits to 64TB.
 */
#define MAX_EA_BITS_PER_CONTEXT		46

#ifndef __ASSEMBLY__
#define H_PTE_TABLE_SIZE	(sizeof(pte_t) << H_PTE_INDEX_SIZE)
#define H_PMD_TABLE_SIZE	(sizeof(pmd_t) << H_PMD_INDEX_SIZE)
#define H_PUD_TABLE_SIZE	(sizeof(pud_t) << H_PUD_INDEX_SIZE)
#define H_PGD_TABLE_SIZE	(sizeof(pgd_t) << H_PGD_INDEX_SIZE)

#define H_PAGE_F_GIX_SHIFT	53
#define H_PAGE_F_SECOND	_RPAGE_RPN44	/* HPTE is in 2ndary HPTEG */
#define H_PAGE_F_GIX	(_RPAGE_RPN43 | _RPAGE_RPN42 | _RPAGE_RPN41)
#define H_PAGE_BUSY	_RPAGE_RSV1     /* software: PTE & hash are busy */
#define H_PAGE_HASHPTE	_RPAGE_RSV2     /* software: PTE & hash are busy */

/* PTE flags to conserve for HPTE identification */
#define _PAGE_HPTEFLAGS (H_PAGE_BUSY | H_PAGE_HASHPTE | \
			 H_PAGE_F_SECOND | H_PAGE_F_GIX)
/*
 * Not supported by 4k linux page size
 */
#define H_PAGE_4K_PFN	0x0
#define H_PAGE_THP_HUGE 0x0
#define H_PAGE_COMBO	0x0

/* 8 bytes per each pte entry */
#define H_PTE_FRAG_SIZE_SHIFT  (H_PTE_INDEX_SIZE + 3)
#define H_PTE_FRAG_NR	(PAGE_SIZE >> H_PTE_FRAG_SIZE_SHIFT)
#define H_PMD_FRAG_SIZE_SHIFT  (H_PMD_INDEX_SIZE + 3)
#define H_PMD_FRAG_NR	(PAGE_SIZE >> H_PMD_FRAG_SIZE_SHIFT)

/* memory key bits, only 8 keys supported */
#define H_PTE_PKEY_BIT0	0
#define H_PTE_PKEY_BIT1	0
#define H_PTE_PKEY_BIT2	_RPAGE_RSV3
#define H_PTE_PKEY_BIT3	_RPAGE_RSV4
#define H_PTE_PKEY_BIT4	_RPAGE_RSV5

/*
 * On all 4K setups, remap_4k_pfn() equates to remap_pfn_range()
 */
#define remap_4k_pfn(vma, addr, pfn, prot)	\
	remap_pfn_range((vma), (addr), (pfn), PAGE_SIZE, (prot))

#ifdef CONFIG_HUGETLB_PAGE
static inline int hash__hugepd_ok(hugepd_t hpd)
{
	unsigned long hpdval = hpd_val(hpd);
	/*
	 * if it is not a pte and have hugepd shift mask
	 * set, then it is a hugepd directory pointer
	 */
	if (!(hpdval & _PAGE_PTE) &&
	    ((hpdval & HUGEPD_SHIFT_MASK) != 0))
		return true;
	return false;
}
#endif

/*
 * 4K PTE format is different from 64K PTE format. Saving the hash_slot is just
 * a matter of returning the PTE bits that need to be modified. On 64K PTE,
 * things are a little more involved and hence needs many more parameters to
 * accomplish the same. However we want to abstract this out from the caller by
 * keeping the prototype consistent across the two formats.
 */
static inline unsigned long pte_set_hidx(pte_t *ptep, real_pte_t rpte,
					 unsigned int subpg_index, unsigned long hidx,
					 int offset)
{
	return (hidx << H_PAGE_F_GIX_SHIFT) &
		(H_PAGE_F_SECOND | H_PAGE_F_GIX);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE

static inline char *get_hpte_slot_array(pmd_t *pmdp)
{
	BUG();
	return NULL;
}

static inline unsigned int hpte_valid(unsigned char *hpte_slot_array, int index)
{
	BUG();
	return 0;
}

static inline unsigned int hpte_hash_index(unsigned char *hpte_slot_array,
					   int index)
{
	BUG();
	return 0;
}

static inline void mark_hpte_slot_valid(unsigned char *hpte_slot_array,
					unsigned int index, unsigned int hidx)
{
	BUG();
}

static inline int hash__pmd_trans_huge(pmd_t pmd)
{
	return 0;
}

static inline int hash__pmd_same(pmd_t pmd_a, pmd_t pmd_b)
{
	BUG();
	return 0;
}

static inline pmd_t hash__pmd_mkhuge(pmd_t pmd)
{
	BUG();
	return pmd;
}

extern unsigned long hash__pmd_hugepage_update(struct mm_struct *mm,
					   unsigned long addr, pmd_t *pmdp,
					   unsigned long clr, unsigned long set);
extern pmd_t hash__pmdp_collapse_flush(struct vm_area_struct *vma,
				   unsigned long address, pmd_t *pmdp);
extern void hash__pgtable_trans_huge_deposit(struct mm_struct *mm, pmd_t *pmdp,
					 pgtable_t pgtable);
extern pgtable_t hash__pgtable_trans_huge_withdraw(struct mm_struct *mm, pmd_t *pmdp);
extern pmd_t hash__pmdp_huge_get_and_clear(struct mm_struct *mm,
				       unsigned long addr, pmd_t *pmdp);
extern int hash__has_transparent_hugepage(void);
#endif

static inline pmd_t hash__pmd_mkdevmap(pmd_t pmd)
{
	BUG();
	return pmd;
}

#endif /* !__ASSEMBLY__ */

#endif /* _ASM_POWERPC_BOOK3S_64_HASH_4K_H */
