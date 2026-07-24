/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_SH_HUGETLB_H
#define _ASM_SH_HUGETLB_H

#include <asm/cacheflush.h>
#include <asm/page.h>
#include <asm/pgtable.h>

#define __HAVE_ARCH_HUGE_PTEP_CLEAR_FLUSH
static inline pte_t huge_ptep_clear_flush(struct vm_area_struct *vma,
					  unsigned long addr, pte_t *ptep)
{
	return *ptep;
}

static inline void arch_clear_hugetlb_flags(struct folio *folio)
{
	clear_bit(PG_dcache_clean, &folio->flags.f);
}
#define arch_clear_hugetlb_flags arch_clear_hugetlb_flags

static inline pte_t arch_make_huge_pte(pte_t entry, unsigned int shift,
				       vm_flags_t flags)
{
	/* base PAGE_SHIFT is 14; each slot step is 2 shift bits (x4 size). */
	unsigned int slot = (shift - PAGE_SHIFT) / 2;
	return __pte(jcore_pte_set_size(pte_val(entry), slot));
}
#define arch_make_huge_pte arch_make_huge_pte

#include <asm-generic/hugetlb.h>

#endif /* _ASM_SH_HUGETLB_H */
