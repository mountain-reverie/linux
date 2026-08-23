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

/*
 * huge_pte_alloc() - allocate EVERY pte table the huge page at @addr spans.
 *
 * A jcore huge pte is replicated across every base pte slot it covers (see
 * set_huge_pte_at() below and the rationale in <asm/hugetlb.h>), so every
 * table the span crosses has to exist, not just the one holding the head.
 *
 * At PAGE_SHIFT 14 one pte table covers PTRS_PER_PTE (4096) * 16 KB = 64 MB,
 * which is exactly PGDIR_SIZE (PGDIR_SHIFT = PTE_SHIFT + PTE_BITS = 26,
 * arch/sh/include/asm/pgtable-2level.h). Every registered huge size up to
 * 64 MB is therefore aligned inside a single table; only the 256 MB size
 * crosses one, and it crosses exactly four, entering each at index 0.
 *
 * Returns the HEAD slot -- the one at @addr -- which is what every generic
 * caller then passes around. A failure part-way through leaves the earlier
 * tables allocated; that is harmless (they are freed with the mm, and an
 * empty pte table is not a mapping) and matches how every other partial
 * page-table allocation in the tree behaves.
 */
pte_t *huge_pte_alloc(struct mm_struct *mm, struct vm_area_struct *vma,
			unsigned long addr, unsigned long sz)
{
	pte_t *head = NULL;
	unsigned long a;

	for (a = addr; a < addr + sz; a = (a & PGDIR_MASK) + PGDIR_SIZE) {
		pgd_t *pgd = pgd_offset(mm, a);
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		pte_t *pte;

		p4d = p4d_alloc(mm, pgd, a);
		if (!p4d)
			return NULL;
		pud = pud_alloc(mm, p4d, a);
		if (!pud)
			return NULL;
		pmd = pmd_alloc(mm, pud, a);
		if (!pmd)
			return NULL;
		pte = pte_alloc_huge(mm, pmd, a);
		if (!pte)
			return NULL;
		if (!head)
			head = pte;
	}

	return head;
}

/*
 * huge_pte_offset() - the pte slot @addr maps through, or NULL.
 *
 * The none/bad checks are not decoration. hugetlb_walk() is legitimately
 * called on addresses with no page table under them at all -- over a whole
 * VMA in copy_hugetlb_page_range(), from walk_hugetlb_range(), from
 * hugetlb_vma_maps_pfn() -- and the previous `if (pgd)` / `if (p4d)` form
 * tested only that the POINTER arithmetic produced something, which it
 * always does. An absent pmd would be handed to pte_offset_huge(), which
 * would take pmd_page_vaddr() of a none entry and return a pointer the
 * caller then dereferences. Both the generic huge_pte_offset()
 * (mm/hugetlb.c) and arm64's return NULL on !present; so does this now.
 *
 * @addr is deliberately NOT masked down to @sz here. Every generic caller
 * that looks a huge pte up already aligns, and at least one passes a @sz
 * that is not the mapping's size (mm/hugetlb.c hugetlb_mfill_atomic_pte()
 * passes PMD_SIZE), so masking by @sz would move a correctly-aligned lookup
 * onto the wrong slot. arm64 can mask because its size comes from the pte
 * level it found, not from @sz.
 */
pte_t *huge_pte_offset(struct mm_struct *mm,
		       unsigned long addr, unsigned long sz)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return NULL;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return NULL;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return NULL;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return NULL;

	return pte_offset_huge(pmd, addr);
}

#ifdef CONFIG_CPU_JCORE
/*
 * ---------------------------------------------------------------------------
 * Contiguous-run helpers. See <asm/hugetlb.h> for why a jcore huge pte is
 * replicated across every base pte slot it spans.
 * ---------------------------------------------------------------------------
 *
 * A run may cross a pte table boundary -- only the 256 MB size does, four
 * tables' worth -- so nothing may walk off the end of one table with ptep++.
 * huge_pte_step() re-walks whenever the address enters a new table.
 */
static pte_t *huge_pte_step(struct mm_struct *mm, unsigned long addr,
			    pte_t *ptep)
{
	if (pte_index(addr))
		return ptep + 1;
	return huge_pte_offset(mm, addr, PAGE_SIZE);
}

/*
 * The huge page size a PRESENT pte describes. Only ever call this on a
 * present pte: the swap/marker encoding lays the swap offset over pte bits
 * 11..31, which includes the size-slot bits {12,13}, so the "slot" of a
 * non-present pte is not a size at all.
 */
static inline unsigned long huge_pte_size(pte_t pte)
{
	return PAGE_SIZE << (2 * jcore_pte_size_slot(pte_val(pte)));
}

void set_huge_pte_at(struct mm_struct *mm, unsigned long addr, pte_t *ptep,
		     pte_t pte, unsigned long sz)
{
	unsigned long n = sz >> PAGE_SHIFT;
	bool present = pte_present(pte);
	unsigned long i;

	for (i = 0; i < n; i++) {
		if (i) {
			addr += PAGE_SIZE;
			ptep = huge_pte_step(mm, addr, ptep);
			if (!ptep)
				return;
			/*
			 * Each slot names its OWN base page. That is what
			 * makes the raw-indexed generic walkers (GUP,
			 * folio_walk_start) hand back the right struct page,
			 * and it costs the hardware nothing: PageMask takes
			 * the low PA bits from the VA, and the PPN bits above
			 * the page-offset field are identical across the run
			 * by construction. A non-present pte -- a migration
			 * entry or a uffd-wp marker -- is replicated
			 * VERBATIM: its "PFN" field is a swap offset and
			 * incrementing it would corrupt the entry (arm64 does
			 * the same, arch/arm64/mm/hugetlbpage.c).
			 */
			if (present)
				pte = __pte(pte_val(pte) +
					    (1UL << PFN_PTE_SHIFT));
		}
		set_pte_at(mm, addr, ptep, pte);
	}
}

void huge_pte_clear(struct mm_struct *mm, unsigned long addr, pte_t *ptep,
		    unsigned long sz)
{
	unsigned long n = sz >> PAGE_SHIFT;
	unsigned long i;

	for (i = 0; i < n; i++) {
		if (i) {
			addr += PAGE_SIZE;
			ptep = huge_pte_step(mm, addr, ptep);
			if (!ptep)
				return;
		}
		pte_clear(mm, addr, ptep);
	}
}

/*
 * The head slot is authoritative for the soft bits (see <asm/hugetlb.h>), so
 * the value returned is a single read of it rather than arm64's OR-reduce of
 * young/dirty over the whole run -- which for a 256 MB page would be 16384
 * reads on a path that is already tearing down 16384 entries.
 *
 * No TLB flush here, exactly as the generic ptep_get_and_clear() it replaces:
 * the callers own the flush.
 */
pte_t huge_ptep_get_and_clear(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep, unsigned long sz)
{
	pte_t orig = ptep_get(ptep);

	huge_pte_clear(mm, addr, ptep, sz);
	return orig;
}

/*
 * Read-modify-write each slot in place rather than recomputing a value per
 * slot: every slot keeps its own PFN that way, with no PFN arithmetic to get
 * wrong. The size comes from the pte itself, since this hook is not given
 * one; mm only ever calls it on a present huge pte.
 */
void huge_ptep_set_wrprotect(struct mm_struct *mm, unsigned long addr,
			     pte_t *ptep)
{
	unsigned long n = huge_pte_size(ptep_get(ptep)) >> PAGE_SHIFT;
	unsigned long i;

	for (i = 0; i < n; i++) {
		if (i) {
			addr += PAGE_SIZE;
			ptep = huge_pte_step(mm, addr, ptep);
			if (!ptep)
				return;
		}
		set_pte_at(mm, addr, ptep, pte_wrprotect(ptep_get(ptep)));
	}
}

/*
 * @pte carries the head PFN and the new flags, so rewriting the whole run
 * from it is exactly set_huge_pte_at(). The size comes from the OLD pte,
 * which mm only ever gives us present.
 *
 * This is the one place generic mm sets young/dirty on a huge mapping, and
 * it writes every slot -- including the head, which is what huge_ptep_get()
 * reads. See <asm/hugetlb.h> on why that matters.
 */
int huge_ptep_set_access_flags(struct vm_area_struct *vma, unsigned long addr,
			       pte_t *ptep, pte_t pte, int dirty)
{
	pte_t orig = ptep_get(ptep);
	unsigned long sz = huge_pte_size(orig);

	if (pte_same(orig, pte))
		return 0;

	set_huge_pte_at(vma->vm_mm, addr, ptep, pte, sz);
	flush_tlb_range(vma, addr, addr + sz);
	return 1;
}
#endif /* CONFIG_CPU_JCORE */

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
