/*
 * AMD Memory Encryption Support
 *
 * Copyright (C) 2016 Advanced Micro Devices, Inc.
 *
 * Author: Tom Lendacky <thomas.lendacky@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define DISABLE_BRANCH_PROFILING

#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/dma-direct.h>
#include <linux/swiotlb.h>
#include <linux/mem_encrypt.h>

#include <asm/tlbflush.h>
#include <asm/fixmap.h>
#include <asm/setup.h>
#include <asm/bootparam.h>
#include <asm/set_memory.h>
#include <asm/cacheflush.h>
#include <asm/processor-flags.h>
#include <asm/msr.h>
#include <asm/cmdline.h>

#include "mm_internal.h"

/*
 * Since SME related variables are set early in the boot process they must
 * reside in the .data section so as not to be zeroed out when the .bss
 * section is later cleared.
 */
u64 sme_me_mask __section(.data) = 0;
EXPORT_SYMBOL(sme_me_mask);
DEFINE_STATIC_KEY_FALSE(sev_enable_key);
EXPORT_SYMBOL_GPL(sev_enable_key);

bool sev_enabled __section(.data);

/* Buffer used for early in-place encryption by BSP, no locking needed */
static char sme_early_buffer[PAGE_SIZE] __aligned(PAGE_SIZE);

/*
 * This routine does not change the underlying encryption setting of the
 * page(s) that map this memory. It assumes that eventually the memory is
 * meant to be accessed as either encrypted or decrypted but the contents
 * are currently not in the desired state.
 *
 * This routine follows the steps outlined in the AMD64 Architecture
 * Programmer's Manual Volume 2, Section 7.10.8 Encrypt-in-Place.
 */
static void __init __sme_early_enc_dec(resource_size_t paddr,
				       unsigned long size, bool enc)
{
	void *src, *dst;
	size_t len;

	if (!sme_me_mask)
		return;

	wbinvd();

	/*
	 * There are limited number of early mapping slots, so map (at most)
	 * one page at time.
	 */
	while (size) {
		len = min_t(size_t, sizeof(sme_early_buffer), size);

		/*
		 * Create mappings for the current and desired format of
		 * the memory. Use a write-protected mapping for the source.
		 */
		src = enc ? early_memremap_decrypted_wp(paddr, len) :
			    early_memremap_encrypted_wp(paddr, len);

		dst = enc ? early_memremap_encrypted(paddr, len) :
			    early_memremap_decrypted(paddr, len);

		/*
		 * If a mapping can't be obtained to perform the operation,
		 * then eventual access of that area in the desired mode
		 * will cause a crash.
		 */
		BUG_ON(!src || !dst);

		/*
		 * Use a temporary buffer, of cache-line multiple size, to
		 * avoid data corruption as documented in the APM.
		 */
		memcpy(sme_early_buffer, src, len);
		memcpy(dst, sme_early_buffer, len);

		early_memunmap(dst, len);
		early_memunmap(src, len);

		paddr += len;
		size -= len;
	}
}

void __init sme_early_encrypt(resource_size_t paddr, unsigned long size)
{
	__sme_early_enc_dec(paddr, size, true);
}

void __init sme_early_decrypt(resource_size_t paddr, unsigned long size)
{
	__sme_early_enc_dec(paddr, size, false);
}

static void __init __sme_early_map_unmap_mem(void *vaddr, unsigned long size,
					     bool map)
{
	unsigned long paddr = (unsigned long)vaddr - __PAGE_OFFSET;
	pmdval_t pmd_flags, pmd;

	/* Use early_pmd_flags but remove the encryption mask */
	pmd_flags = __sme_clr(early_pmd_flags);

	do {
		pmd = map ? (paddr & PMD_MASK) + pmd_flags : 0;
		__early_make_pgtable((unsigned long)vaddr, pmd);

		vaddr += PMD_SIZE;
		paddr += PMD_SIZE;
		size = (size <= PMD_SIZE) ? 0 : size - PMD_SIZE;
	} while (size);

	__native_flush_tlb();
}

void __init sme_unmap_bootdata(char *real_mode_data)
{
	struct boot_params *boot_data;
	unsigned long cmdline_paddr;

	if (!sme_active())
		return;

	/* Get the command line address before unmapping the real_mode_data */
	boot_data = (struct boot_params *)real_mode_data;
	cmdline_paddr = boot_data->hdr.cmd_line_ptr | ((u64)boot_data->ext_cmd_line_ptr << 32);

	__sme_early_map_unmap_mem(real_mode_data, sizeof(boot_params), false);

	if (!cmdline_paddr)
		return;

	__sme_early_map_unmap_mem(__va(cmdline_paddr), COMMAND_LINE_SIZE, false);
}

void __init sme_map_bootdata(char *real_mode_data)
{
	struct boot_params *boot_data;
	unsigned long cmdline_paddr;

	if (!sme_active())
		return;

	__sme_early_map_unmap_mem(real_mode_data, sizeof(boot_params), true);

	/* Get the command line address after mapping the real_mode_data */
	boot_data = (struct boot_params *)real_mode_data;
	cmdline_paddr = boot_data->hdr.cmd_line_ptr | ((u64)boot_data->ext_cmd_line_ptr << 32);

	if (!cmdline_paddr)
		return;

	__sme_early_map_unmap_mem(__va(cmdline_paddr), COMMAND_LINE_SIZE, true);
}

void __init sme_early_init(void)
{
	unsigned int i;

	if (!sme_me_mask)
		return;

	early_pmd_flags = __sme_set(early_pmd_flags);

	__supported_pte_mask = __sme_set(__supported_pte_mask);

	/* Update the protection map with memory encryption mask */
	for (i = 0; i < ARRAY_SIZE(protection_map); i++)
		protection_map[i] = pgprot_encrypted(protection_map[i]);

	if (sev_active())
		swiotlb_force = SWIOTLB_FORCE;
}

static void __init __set_clr_pte_enc(pte_t *kpte, int level, bool enc)
{
	pgprot_t old_prot, new_prot;
	unsigned long pfn, pa, size;
	pte_t new_pte;

	switch (level) {
	case PG_LEVEL_4K:
		pfn = pte_pfn(*kpte);
		old_prot = pte_pgprot(*kpte);
		break;
	case PG_LEVEL_2M:
		pfn = pmd_pfn(*(pmd_t *)kpte);
		old_prot = pmd_pgprot(*(pmd_t *)kpte);
		break;
	case PG_LEVEL_1G:
		pfn = pud_pfn(*(pud_t *)kpte);
		old_prot = pud_pgprot(*(pud_t *)kpte);
		break;
	default:
		return;
	}

	new_prot = old_prot;
	if (enc)
		pgprot_val(new_prot) |= _PAGE_ENC;
	else
		pgprot_val(new_prot) &= ~_PAGE_ENC;

	/* If prot is same then do nothing. */
	if (pgprot_val(old_prot) == pgprot_val(new_prot))
		return;

	pa = pfn << PAGE_SHIFT;
	size = page_level_size(level);

	/*
	 * We are going to perform in-place en-/decryption and change the
	 * physical page attribute from C=1 to C=0 or vice versa. Flush the
	 * caches to ensure that data gets accessed with the correct C-bit.
	 */
	clflush_cache_range(__va(pa), size);

	/* Encrypt/decrypt the contents in-place */
	if (enc)
		sme_early_encrypt(pa, size);
	else
		sme_early_decrypt(pa, size);

	/* Change the page encryption mask. */
	new_pte = pfn_pte(pfn, new_prot);
	set_pte_atomic(kpte, new_pte);
}

static int __init early_set_memory_enc_dec(unsigned long vaddr,
					   unsigned long size, bool enc)
{
	unsigned long vaddr_end, vaddr_next;
	unsigned long psize, pmask;
	int split_page_size_mask;
	int level, ret;
	pte_t *kpte;

	vaddr_next = vaddr;
	vaddr_end = vaddr + size;

	for (; vaddr < vaddr_end; vaddr = vaddr_next) {
		kpte = lookup_address(vaddr, &level);
		if (!kpte || pte_none(*kpte)) {
			ret = 1;
			goto out;
		}

		if (level == PG_LEVEL_4K) {
			__set_clr_pte_enc(kpte, level, enc);
			vaddr_next = (vaddr & PAGE_MASK) + PAGE_SIZE;
			continue;
		}

		psize = page_level_size(level);
		pmask = page_level_mask(level);

		/*
		 * Check whether we can change the large page in one go.
		 * We request a split when the address is not aligned and
		 * the number of pages to set/clear encryption bit is smaller
		 * than the number of pages in the large page.
		 */
		if (vaddr == (vaddr & pmask) &&
		    ((vaddr_end - vaddr) >= psize)) {
			__set_clr_pte_enc(kpte, level, enc);
			vaddr_next = (vaddr & pmask) + psize;
			continue;
		}

		/*
		 * The virtual address is part of a larger page, create the next
		 * level page table mapping (4K or 2M). If it is part of a 2M
		 * page then we request a split of the large page into 4K
		 * chunks. A 1GB large page is split into 2M pages, resp.
		 */
		if (level == PG_LEVEL_2M)
			split_page_size_mask = 0;
		else
			split_page_size_mask = 1 << PG_LEVEL_2M;

		kernel_physical_mapping_init(__pa(vaddr & pmask),
					     __pa((vaddr_end & pmask) + psize),
					     split_page_size_mask);
	}

	ret = 0;

out:
	__flush_tlb_all();
	return ret;
}

int __init early_set_memory_decrypted(unsigned long vaddr, unsigned long size)
{
	return early_set_memory_enc_dec(vaddr, size, false);
}

int __init early_set_memory_encrypted(unsigned long vaddr, unsigned long size)
{
	return early_set_memory_enc_dec(vaddr, size, true);
}

/*
 * SME and SEV are very similar but they are not the same, so there are
 * times that the kernel will need to distinguish between SME and SEV. The
 * sme_active() and sev_active() functions are used for this.  When a
 * distinction isn't needed, the mem_encrypt_active() function can be used.
 *
 * The trampoline code is a good example for this requirement.  Before
 * paging is activated, SME will access all memory as decrypted, but SEV
 * will access all memory as encrypted.  So, when APs are being brought
 * up under SME the trampoline area cannot be encrypted, whereas under SEV
 * the trampoline area must be encrypted.
 */
bool sme_active(void)
{
	return sme_me_mask && !sev_enabled;
}
EXPORT_SYMBOL(sme_active);

bool sev_active(void)
{
	return sme_me_mask && sev_enabled;
}
EXPORT_SYMBOL(sev_active);

/* Architecture __weak replacement functions */
void __init mem_encrypt_free_decrypted_mem(void)
{
	unsigned long vaddr, vaddr_end, npages;
	int r;

	vaddr = (unsigned long)__start_bss_decrypted_unused;
	vaddr_end = (unsigned long)__end_bss_decrypted;
	npages = (vaddr_end - vaddr) >> PAGE_SHIFT;

	/*
	 * The unused memory range was mapped decrypted, change the encryption
	 * attribute from decrypted to encrypted before freeing it.
	 */
	if (mem_encrypt_active()) {
		r = set_memory_encrypted(vaddr, npages);
		if (r) {
			pr_warn("failed to free unused decrypted pages\n");
			return;
		}
	}

	free_init_pages("unused decrypted", vaddr, vaddr_end);
}

void __init mem_encrypt_init(void)
{
	if (!sme_me_mask)
		return;

	/* Call into SWIOTLB to update the SWIOTLB DMA buffers */
	swiotlb_update_mem_attributes();

	/*
	 * With SEV, DMA operations cannot use encryption, we need to use
	 * SWIOTLB to bounce buffer DMA operation.
	 */
	if (sev_active())
		dma_ops = &swiotlb_dma_ops;

	/*
	 * With SEV, we need to unroll the rep string I/O instructions.
	 */
	if (sev_active())
		static_branch_enable(&sev_enable_key);

	pr_info("AMD %s active\n",
		sev_active() ? "Secure Encrypted Virtualization (SEV)"
			     : "Secure Memory Encryption (SME)");
}

