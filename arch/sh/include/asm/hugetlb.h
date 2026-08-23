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

#ifdef CONFIG_CPU_JCORE
/*
 * J4 huge pages live at the PTE level and are REPLICATED across every base
 * pte slot they span, arm64-contiguous-PTE style, with the PFN incrementing
 * one base page per slot.
 *
 * WHY REPLICATION IS NOT OPTIONAL. A pte-level huge page that occupies only
 * the slot at its aligned address covers exactly PAGE_SIZE of VA as far as
 * the page table is concerned. The huge_pte_offset()/huge_ptep_*() API is
 * fine with that -- every generic caller aligns first -- but three generic
 * walkers bypass that API entirely and index the pte table with the RAW,
 * PAGE_SIZE-granular address:
 *
 *   - mm/gup.c follow_pmd_mask(): !pmd_leaf() for a pte-level huge page, so
 *     it falls through to follow_page_pte(), which does
 *     pte_offset_map_lock(mm, pmd, address). There is no hugetlb special
 *     case left in mm/gup.c. An empty slot means no_page_table() ->
 *     faultin_page() -> hugetlb_fault() finds the (aligned) pte already
 *     present and returns 0 -> __get_user_pages retries -> livelock.
 *   - mm/gup.c gup_fast_pte_range(): pte_offset_map() plus ptep++ per
 *     PAGE_SIZE.
 *   - mm/pagewalk.c folio_walk_start(): pte_offset_map_lock() with the raw
 *     addr. Its kerneldoc says outright that "the page table entry stored
 *     in @fw might not correspond to the first physical entry of a logical
 *     hugetlb entry" -- i.e. generic mm requires the non-head slots to be
 *     populated.
 *
 * Every other architecture that puts huge pages at the pte level replicates
 * for the same reason (arm64 PTE_CONT, riscv NAPOT); arm64 additionally has
 * a hardware requirement that the entries be identical-but-for-the-PFN.
 * Cost is 4 bytes per 16 KB of huge mapping, 0.024%.
 *
 * THE HEAD SLOT IS AUTHORITATIVE FOR THE SOFT BITS, which is what lets
 * huge_ptep_get() stay the generic single read instead of arm64's OR-reduce
 * over the run -- a reduce would be 16384 reads for a 256 MB page. Every
 * writer of young/dirty writes the head: generic mm through
 * huge_ptep_set_access_flags() below, which rewrites the whole run, and the
 * TLB-miss walker through __jcore_tlb_walk() (arch/sh/mm/tlb-jcore.c),
 * which resolves at the huge-ALIGNED address and so writes _PAGE_ACCESSED
 * into the head. That second half is load-bearing: without it the walker
 * would mark whichever slot the fault happened to index and the head would
 * never go young, so a huge page would look permanently cold to reclaim.
 *
 * See docs/mmu/pagemask-walker-contract.md K7 (obligation K7a).
 */
#define __HAVE_ARCH_HUGE_SET_HUGE_PTE_AT
void set_huge_pte_at(struct mm_struct *mm, unsigned long addr, pte_t *ptep,
		     pte_t pte, unsigned long sz);

#define __HAVE_ARCH_HUGE_PTE_CLEAR
void huge_pte_clear(struct mm_struct *mm, unsigned long addr, pte_t *ptep,
		    unsigned long sz);

#define __HAVE_ARCH_HUGE_PTEP_GET_AND_CLEAR
pte_t huge_ptep_get_and_clear(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep, unsigned long sz);

#define __HAVE_ARCH_HUGE_PTEP_SET_WRPROTECT
void huge_ptep_set_wrprotect(struct mm_struct *mm, unsigned long addr,
			     pte_t *ptep);

#define __HAVE_ARCH_HUGE_PTEP_SET_ACCESS_FLAGS
int huge_ptep_set_access_flags(struct vm_area_struct *vma, unsigned long addr,
			       pte_t *ptep, pte_t pte, int dirty);
#endif /* CONFIG_CPU_JCORE */

#include <asm-generic/hugetlb.h>

#endif /* _ASM_SH_HUGETLB_H */
