// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/sh/mm/tlb-jcore.c
 *
 * J-Core J4 MMU: software TLB-miss walker + TLB maintenance ops.
 *
 * The J4 MMU has no hardware page-table walker: on a TLB miss the CPU
 * loads PTEH (faulting VPN)/ASIDR (current ASID_TAG)/TSBPTR (precomputed
 * TSB slot) and jumps to a fixed vector (VBR + 0x400/0x420/0x440, see
 * arch/sh/kernel/cpu/jcore/{tlbmiss.S,ex.S}). The hot path there probes
 * the two-word TSB tag directly; __jcore_tlb_walk() below is the slow
 * path it falls back to on a TSB miss, walking the real Linux page
 * table and re-populating both PTEL and the TSB slot for next time
 * (docs/mmu/linux-spec.md §4.2).
 */
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/io.h>

#include <asm/pgtable.h>
#include <asm/mmu_context.h>
#include <asm/tlb-jcore.h>
#include <cpu/mmu_context.h>

/*
 * jcore_read_tsbptr() - read back the (unchanged) TSB slot address the
 * hot path already computed. Kept as a one-line helper so
 * __jcore_tlb_walk() has no dependency beyond it and jcore_pte_to_ptel().
 */
static inline unsigned long jcore_read_tsbptr(void)
{
	unsigned long tsbptr;

	__asm__ __volatile__("stc tsbptr, %0" : "=r" (tsbptr));
	return tsbptr;
}

/*
 * jcore_tlb_walk_mark_accessed() - the *only* piece of __jcore_tlb_walk()
 * that isn't a pure function of its arguments: it writes the software
 * _PAGE_ACCESSED bit back into the live page table. Split out as its own
 * (weak, overridable) symbol so a bare-metal SP2 unit-test harness can
 * link a stub that just stores to the pte without any struct-page/mm
 * bookkeeping, while the kernel build gets the real thing.
 */
void __weak jcore_tlb_walk_mark_accessed(pte_t *ptep, pte_t entry)
{
	set_pte(ptep, entry);
}

/*
 * __jcore_tlb_walk() - software TLB-miss slow path.
 *
 * @pgd:       root of the page table to walk (current_pgd, stashed in the
 *             MMU_TTB scratch MMIO register by switch_mm()/set_TTB()).
 * @addr:      faulting address (read from MMU_TEA by the asm caller).
 * @pteh_tag:  the expected ASID_TAG (TSB tag_lo) for this walk -- read
 *             from ASIDR by the asm caller, passed through unchanged.
 *
 * Two-level walk (pgd -> pte); intermediate pud_offset()/pmd_offset()
 * calls fold away via the generic <asm-generic/pgtable-nop{ud,md}.h>
 * shims for jcore's (non-PAE, non-J64) page-table depth -- no
 * CONFIG_64BIT/PAE arm here, those are a separate, later port.
 *
 * On success: sets the software _PAGE_ACCESSED bit if unset, builds the
 * hardware PTEL image via jcore_pte_to_ptel(), loads it into PTEL, and
 * rewrites the 3-word TSB slot (tag_hi = VPN, tag_lo = ASID_TAG,
 * data = PTEL) so the next miss to this VPN/ASID hits the fast path.
 * Returns 0 and expects the caller (tlbmiss.S) to execute LDTLB.RN.
 *
 * On failure (not present, or software PROTNONE/STALE marker set):
 * returns nonzero; the caller falls through to the generic fault path.
 */
int __jcore_tlb_walk(pgd_t *pgd, unsigned long addr, unsigned long pteh_tag)
{
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep;
	pte_t entry;
	unsigned long ptel;
	unsigned long tsb_slot;
	unsigned long vpn;

	pgd += pgd_index(addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return -EFAULT;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return -EFAULT;

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return -EFAULT;

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return -EFAULT;

	ptep = pte_offset_kernel(pmd, addr);
	entry = *ptep;

	if (!(pte_val(entry) & _PAGE_VALID))
		return -EFAULT;

	if (pte_val(entry) & (_PAGE_PROTNONE | _PAGE_STALE))
		return -EFAULT;	/* prot-none, or lazy-shootdown stale */

	if (!(pte_val(entry) & _PAGE_ACCESSED)) {
		pte_val(entry) |= _PAGE_ACCESSED;
		jcore_tlb_walk_mark_accessed(ptep, entry);
	}

	ptel = jcore_pte_to_ptel(pte_val(entry));

	__asm__ __volatile__("ldc %0, ptel" : : "r" (ptel));

	vpn = addr & PAGE_MASK;
	tsb_slot = jcore_read_tsbptr();
	*(unsigned long *)(tsb_slot + 0) = vpn;		/* tag_hi */
	*(unsigned long *)(tsb_slot + 4) = pteh_tag;		/* tag_lo (ASID_TAG) */
	*(unsigned long *)(tsb_slot + 8) = ptel;		/* data   (PTEL)      */

	return 0;
}

/*
 * __update_tlb() - proactively prime the TLB/TSB for a freshly-faulted-in
 * pte (called from update_mmu_cache() right after the generic fault
 * handler installs the pte). Not strictly required for correctness (the
 * next access would just retake a TLB miss and __jcore_tlb_walk() would
 * populate it lazily), but avoids that guaranteed extra trap.
 */
void __update_tlb(struct vm_area_struct *vma, unsigned long address, pte_t pte)
{
	unsigned long flags, pteh, ptel;

	/* Handle debugger faulting in the debuggee. */
	if (vma && current->active_mm != vma->vm_mm)
		return;

	local_irq_save(flags);

	pteh = address & PAGE_MASK;
	ptel = jcore_pte_to_ptel(pte_val(pte));

	__asm__ __volatile__(
		"ldc	%0, pteh\n\t"
		"ldc	%1, ptel\n\t"
		"ldtlb.rn"
		: : "r" (pteh), "r" (ptel) : "memory");

	local_irq_restore(flags);
}

/*
 * local_flush_tlb_all() - MMUCR.TI is a self-clearing write-1 strobe that
 * invalidates every TLB entry (hardware-spec.md §2.3).
 */
void local_flush_tlb_all(void)
{
	unsigned long flags, status;

	/* MMUCR (unlike PTEH/PTEL/ASIDR/TSBPTR) is MMIO-only, not an
	 * LDC/STC control register -- hardware-spec.md §2.1/§2.3. */
	local_irq_save(flags);
	status = __raw_readl(MMUCR);
	status |= MMUCR_TI;
	__raw_writel(status, MMUCR);
	local_irq_restore(flags);
}

/*
 * local_flush_tlb_one() - J4 has no single-entry TLB invalidate op, only
 * the MMUCR.TI full-flush strobe. Emulate one-entry invalidation with a
 * full flush; SP2/SP3 can revisit this if TLB-flush-storm cost from
 * unmap()/mprotect() hot loops (tlbflush_32.c's local_flush_tlb_range()/
 * local_flush_tlb_kernel_range()) turns out to matter in practice.
 */
void local_flush_tlb_one(unsigned long asid, unsigned long page)
{
	local_flush_tlb_all();
}
