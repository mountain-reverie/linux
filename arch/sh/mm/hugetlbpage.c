// SPDX-License-Identifier: GPL-2.0
/*
 * arch/sh/mm/hugetlbpage.c
 *
 * SuperH HugeTLB page support.
 *
 * Cloned from sparc64 by Paul Mundt.
 *
 * Copyright (C) 2002, 2003 David S. Miller (davem@redhat.com)
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/pagemap.h>
#include <linux/sysctl.h>

#include <asm/mman.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include <asm/cacheflush.h>

pte_t *huge_pte_alloc(struct mm_struct *mm, struct vm_area_struct *vma,
			unsigned long addr, unsigned long sz)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte = NULL;

	pgd = pgd_offset(mm, addr);
	if (pgd) {
		p4d = p4d_alloc(mm, pgd, addr);
		if (p4d) {
			pud = pud_alloc(mm, p4d, addr);
			if (pud) {
				pmd = pmd_alloc(mm, pud, addr);
				if (pmd)
					pte = pte_alloc_huge(mm, pmd, addr);
			}
		}
	}

	return pte;
}

pte_t *huge_pte_offset(struct mm_struct *mm,
		       unsigned long addr, unsigned long sz)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte = NULL;

	pgd = pgd_offset(mm, addr);
	if (pgd) {
		p4d = p4d_offset(pgd, addr);
		if (p4d) {
			pud = pud_offset(p4d, addr);
			if (pud) {
				pmd = pmd_offset(pud, addr);
				if (pmd)
					pte = pte_offset_huge(pmd, addr);
			}
		}
	}

	return pte;
}

#ifdef CONFIG_CPU_JCORE
static __init int jcore_hugetlb_init(void)
{
	/* Register every hardware-supported huge size (64K..256M). Orders are
	 * relative to PAGE_SHIFT(14): shift 16..28 in steps of 2 (x4 each).
	 *
	 * With jcore_defconfig (16KB pages, SPARSEMEM, !SPARSEMEM_VMEMMAP,
	 * SECTION_SIZE_BITS=26) MAX_FOLIO_ORDER resolves to PFN_SECTION_SHIFT
	 * = 26-14 = 12. The 16M (order 10) and 64M (order 12) sizes fit
	 * within a single memory section and behave as ordinary hugetlb
	 * pages. The 256M size (order 14) exceeds MAX_FOLIO_ORDER and is a
	 * true gigantic page: hugetlb_add_hstate() below emits a benign
	 * WARN_ON at boot for it, and it can only be populated at runtime
	 * via CONTIG_ALLOC (CONFIG_CMA is enabled in jcore_defconfig to
	 * provide MEMORY_ISOLATION+COMPACTION => CONTIG_ALLOC=y), e.g. with
	 * hugepagesz=256M/nr_hugepages= on the command line or through
	 * /proc/sys/vm/nr_hugepages at runtime given sufficiently
	 * contiguous free memory. We register it anyway because the J4 TLB
	 * natively supports this PageMask. */
	unsigned int shift;

	for (shift = 16; shift <= 28; shift += 2)
		hugetlb_add_hstate(shift - PAGE_SHIFT);
	return 0;
}
arch_initcall(jcore_hugetlb_init);
#endif /* CONFIG_CPU_JCORE */
