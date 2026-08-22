// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/sh/mm/tlb-jcore.c
 *
 * J-Core J4 MMU: software TLB-miss walker + TLB maintenance ops.
 *
 * The J4 MMU has no hardware page-table walker: on a TLB miss the CPU
 * loads PTEH (faulting VPN)/ASIDR (current ASID_TAG)/TSBPTR (precomputed
 * TSB slot) and jumps to the single TLB vector (VBR + 0x400, shared by all
 * six causes, see arch/sh/kernel/cpu/jcore/ex.S). The hot path there probes
 * the two-word TSB tag directly; __jcore_tlb_walk() below is the slow
 * path it falls back to on a TSB miss, walking the real Linux page
 * table and re-populating the TSB slot for next time
 * (docs/mmu/linux-spec.md §4.2). Software never installs a TLB entry
 * itself: it writes the TSB row and returns, and the hardware TSB
 * walker installs from that row when the access re-executes.
 */
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/string.h>

#include <asm/pgtable.h>
#include <asm/mmu_context.h>
#include <asm/tlb-jcore.h>
#include <cpu/mmu_context.h>

/*
 * jcore_read_tsbptr() - read back the (unchanged) TSB set address the
 * hot path already computed. Kept as a one-line helper so
 * __jcore_tlb_walk() has no dependency beyond it and jcore_pte_to_ptel().
 *
 * The value is a kernel virtual (P1) address and may be dereferenced as
 * one -- see the address-space note on jcore_tsb_slot_addr() below for
 * why that is true and what keeps it true.
 */
static inline unsigned long jcore_read_tsbptr(void)
{
	return __raw_readl(TSBPTR);
}

/*
 * jcore_tsb_write_entry() - the ONLY place that stores the three words of
 * a TSB entry.
 *
 * Commit order matters: the hardware TSB walker (core/tlb_walk.vhd)
 * compares tag_hi FIRST, then tag_lo, and only then reads data and
 * installs it -- so tag_hi is the commit point. Writing it before data/
 * tag_lo lets a walker that lands between the stores observe tag_hi
 * already matching the new VPN while tag_lo/data still belong to whatever
 * this slot held before: a torn read that installs a wrong translation
 * with no exception and no diagnostic (docs/mmu/hardware-spec.md §5;
 * jcore-cpu sim/tests/mmuwalktorn.S). Write data and tag_lo first, tag_hi
 * last, with a compiler barrier immediately before the tag_hi store so
 * gcc cannot itself reorder the commit point out from under this comment.
 *
 * core/tlb_walk.vhd cannot distinguish a torn TSB entry from a legitimate
 * one, so no bare-metal guard can catch a regression of this order --
 * this helper is the only thing standing between "correct" and "silently
 * wrong". A future writer must edit THIS function to get it wrong, with
 * the reasoning right in front of them, instead of being able to
 * open-code a fourth, unreviewed store site.
 *
 * @set:      byte address of the 32-byte, 2-way TSB SET (from TSBPTR or
 *            JCORE_TSB_SLOT -- hardware now returns a set, not an entry).
 * @way:      which way of the set to write; see jcore_tsb_pick_way().
 * @vpn:      tag_hi -- the CANONICAL 4 KB VPN of the faulting address,
 *            i.e. `addr & JCORE_TSB_TAG_MASK`. NOT `addr & PAGE_MASK`, and
 *            not a function of the page size the entry describes; see
 *            JCORE_TSB_TAG_MASK in <cpu/mmu_context.h>.
 * @asid_tag: tag_lo -- the ASID_TAG this entry is valid for.
 * @ptel:     data -- the hardware PTEL image.
 */
static inline void jcore_tsb_write_entry(unsigned long set,
					  unsigned int way,
					  unsigned long vpn,
					  unsigned long asid_tag,
					  unsigned long ptel)
{
	unsigned long slot = set + way * JCORE_TSB_ENTRY_BYTES;

	*(unsigned long *)(slot + 8) = ptel;		/* data   (PTEL)      */
	*(unsigned long *)(slot + 4) = asid_tag;	/* tag_lo (ASID_TAG)  */
	barrier();
	*(unsigned long *)(slot + 0) = vpn;		/* tag_hi -- commit point */
}

/*
 * jcore_tsb_pick_way() - choose which way of @set to write for @vpn.
 *
 * The rule is: if either way already holds this VPN, OVERWRITE THAT WAY.
 * Only when neither matches do we take the hardware's victim nomination.
 *
 * The search is EXACT EQUALITY and must stay that way (contract K2). It is
 * correct across page sizes and across size transitions precisely because
 * @vpn is the canonical 4 KB tag -- a total function of the address alone --
 * so "the way holding this VPN" is well defined at every instant. A masked or
 * size-aware search here would be a symptom that someone had reintroduced a
 * size-dependent tag somewhere.
 *
 * This is not an optimisation and must not be "optimised" into always
 * taking the victim. Overwriting the matching way is what makes a stale
 * duplicate of a VPN unconstructable. Suppose a fresh entry for VPN X went
 * to way 1 while a stale entry for the same X still sat in way 0: the next
 * walk probes way 0 first, matches its tag, and installs the STALE PTEL --
 * wrong permissions, no exception, no diagnostic. Hardware cannot detect
 * that; the tags are both legitimate. One index function plus both ways in
 * a single 32-byte line is precisely what lets software rule it out here,
 * for the cost of one extra load it has already paid for (the set is one
 * cache line).
 *
 * The victim nomination comes from a hardware LFSR seeded by the OS at
 * init (jcore_tsb_victim_seed_init(), arch/sh/kernel/cpu/jcore/probe.c);
 * see JCORE_TSB_VICTIM.
 */
static inline unsigned int jcore_tsb_pick_way(unsigned long set,
					      unsigned long vpn)
{
	const unsigned long *tags = (const unsigned long *)set;
	unsigned int way;

	for (way = 0; way < JCORE_TSB_WAYS; way++)
		if (tags[way * (JCORE_TSB_ENTRY_BYTES / sizeof(long))] == vpn)
			return way;

	return __raw_readl(JCORE_TSB_VICTIM) & (JCORE_TSB_WAYS - 1);
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
 * @asid_tag:  the expected ASID_TAG (TSB tag_lo) for this walk -- read
 *             from ASIDR by the asm caller, passed through unchanged. It
 *             becomes tag_lo; tag_hi is computed here from @addr.
 *
 * Two-level walk (pgd -> pte); intermediate pud_offset()/pmd_offset()
 * calls fold away via the generic <asm-generic/pgtable-nop{ud,md}.h>
 * shims for jcore's (non-PAE, non-J64) page-table depth -- no
 * CONFIG_64BIT/PAE arm here, those are a separate, later port.
 *
 * On success: sets the software _PAGE_ACCESSED bit if unset, builds the
 * hardware PTEL image via jcore_pte_to_ptel(), and rewrites one way of
 * the 32-byte TSB set (tag_hi = VPN, tag_lo = ASID_TAG, data = PTEL).
 * Returns 0. The caller does NOT install anything: it simply returns,
 * the access re-executes and misses again, and the hardware TSB walker
 * installs from the row written here.
 *
 * On failure (not present, or software PROTNONE/STALE marker set):
 * returns nonzero; the caller falls through to the generic fault path.
 */
int __jcore_tlb_walk(pgd_t *pgd, unsigned long addr, unsigned long asid_tag)
{
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep;
	pte_t entry;
	unsigned long ptel;
	unsigned long tsb_set;
	unsigned long vpn;

	/*
	 * jcore_pte_to_ptel() below is only correct at PAGE_SHIFT 14: at 12 the
	 * PFN overlaps the pte's page-size slot. arch/sh/mm/Kconfig makes that
	 * configuration unselectable and carries the full argument; this catches
	 * a hand-edited .config. docs/mmu/pagemask-walker-contract.md K3.
	 */
	BUILD_BUG_ON(PAGE_SHIFT != 14);

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

	/*
	 * CANONICAL 4 KB TAG -- never PAGE_MASK. The hardware walker compares
	 * tag_hi against the RAW faulting VA at 4 KB granularity with an exact
	 * 32-bit equality, for every page size; a 16 KB-aligned tag can never
	 * match a first touch outside the page's base 4 KB sub-page and the
	 * fault repeats forever. See JCORE_TSB_TAG_MASK in
	 * <cpu/mmu_context.h> and docs/mmu/pagemask-walker-contract.md C1/C2.
	 * The entry's page size travels in @ptel (PTEL[11:8]) and nowhere else.
	 */
	vpn = addr & JCORE_TSB_TAG_MASK;
	tsb_set = jcore_read_tsbptr();
	jcore_tsb_write_entry(tsb_set, jcore_tsb_pick_way(tsb_set, vpn),
			      vpn, asid_tag, ptel);

	return 0;
}

/*
 * jcore_tsb_slot_addr() - the TSB SET address hardware would compute for
 * a fault at @addr, via the JCORE_TSB_SLOT MMIO helper (write VA, read
 * slot address; see its definition in cpu/mmu_context.h). This used to be
 * jcore_tsb_slot_offset(), a from-scratch C reimplementation of the RTL's
 * tsb_ptr() hash that had to be kept bit-for-bit in sync by hand; that
 * requirement is gone now that the kernel asks the hardware function
 * directly instead of mirroring it.
 *
 * Note VA[31:12] is a *fixed* 12-bit shift in hardware, NOT PAGE_SHIFT:
 * hardware hashes 4 KB-granular page numbers regardless of the kernel's
 * 16 KB base page size, so the four 4 KB-granular VAs inside one 16 KB
 * page can land in four different sets.
 *
 * That is exactly why the TAG is 4 KB-granular too (JCORE_TSB_TAG_MASK):
 * index granularity and tag granularity are one architectural constant, and
 * neither may be a function of the entry's page size. The cost is that a page
 * larger than 4 KB owns one row per TOUCHED 4 KB sub-page, in different sets;
 * that is never a correctness cost, because every such row carries the
 * identical data word. It is a hint cache, not an authority -- but only for
 * as long as the rows in it are not stale, which is what the memset() in
 * local_flush_tlb_all() below is for.
 *
 * ADDRESS SPACE -- why dereferencing this return value is correct.
 *
 * Both this helper and jcore_read_tsbptr() return
 * `(TSBBR & ~0x1F) | (hash << 5)`: TSBBR's own bits [31:5] verbatim, with
 * only the low index bits substituted. So whichever address space TSBBR
 * is programmed in, that is the space the answer comes back in --
 * hardware never converts.
 *
 * TSBBR is therefore programmed with the boot TSB's P1 KERNEL VIRTUAL
 * address (arch/sh/kernel/head_32.S, .LJCORE_TSB_PHYS -> r4 ->
 * jcore_mmu_enable()), NOT its physical address, and that is a deliberate
 * contract with the RTL, not an accident:
 *
 *   - The hardware TSB walker fetches entries with its own bus master and
 *     applies the SH P1 fold (PA = VA & 0x1FFFFFFF) to its own address
 *     before driving it -- jcore-cpu core/cpu.vhd, g_dstore_squash, the
 *     `walk_own` takeover arm, whose comment states outright that "every
 *     TSBBR the guards and linux@jcore program is a P1 kernel address ...
 *     which the software miss handler reads through the fold". Feed the
 *     walker a bare PA instead and it is left unfolded and the entry is
 *     fetched from the wrong place.
 *   - Software gets a P1 address back, which is untranslated and cached
 *     and so may be dereferenced directly -- here, in
 *     jcore_tsb_pick_way(), in jcore_tsb_write_entry(), and in the
 *     assembly fast path (arch/sh/kernel/cpu/jcore/ex.S), which loads
 *     straight from STC TSBPTR with no conversion.
 *
 * A physical TSBBR would break BOTH ends. Software would be dereferencing
 * e.g. 0x10001000, which is P0 and therefore TRANSLATED once MMUCR.AT is
 * set: a translated access to an unrelated user VA, and inside the TLB
 * vector (SR.BL=1) a miss on it is a double fault rather than a
 * diagnostic. Nothing in the guard suite can catch that -- the SP2 cosim
 * harness maps VA==PA -- which is exactly why the invariant is written
 * down here instead of being left to a test.
 *
 * So: do NOT "fix" this by adding __va()/__pa() at this boundary. The
 * conversion belongs at the single point where TSBBR is programmed, and
 * it is already there.
 */
static inline unsigned long jcore_tsb_slot_addr(unsigned long addr)
{
	__raw_writel(addr, JCORE_TSB_SLOT);
	return __raw_readl(JCORE_TSB_SLOT);
}

/*
 * __update_tlb() - proactively prime the TSB for a freshly-faulted-in
 * pte (called from update_mmu_cache() right after the generic fault
 * handler installs the pte). Not strictly required for correctness (the
 * next access would just retake a TLB miss and __jcore_tlb_walk() would
 * populate the row lazily), but avoids that guaranteed extra trap into
 * software.
 *
 * Only the TSB is written: software no longer installs TLB entries at
 * all, the hardware walker is the sole installer and it installs from
 * this row. That also removes the old TLB-vs-TSB divergence hazard -- a
 * stale row cannot be masked by a fresher hardware-TLB entry any more.
 */
void __update_tlb(struct vm_area_struct *vma, unsigned long address, pte_t pte)
{
	unsigned long flags, pteh, ptel, asid_tag, tsb_set;

	/* Handle debugger faulting in the debuggee. */
	if (vma && current->active_mm != vma->vm_mm)
		return;

	local_irq_save(flags);

	/*
	 * Same canonical 4 KB tag as __jcore_tlb_walk() -- never PAGE_MASK.
	 * See JCORE_TSB_TAG_MASK in <cpu/mmu_context.h>.
	 *
	 * For hugetlb, generic mm hands this function the huge-ALIGNED address,
	 * so only that sub-page's set is primed. That is correct-but-partial and
	 * must stay that way (contract K8): every other sub-page is covered on
	 * demand by the slow path, and filling the whole span would be one row
	 * per 4 KB sub-page -- 65536 rows into a 512-row TSB for a 256 MB page.
	 */
	pteh = address & JCORE_TSB_TAG_MASK;
	ptel = jcore_pte_to_ptel(pte_val(pte));

	/*
	 * Prime the TSB set the walker would probe for
	 * this VPN, with the same three-word layout __jcore_tlb_walk()
	 * writes (tag_hi = VPN, tag_lo = ASID_TAG, data = PTEL), into the
	 * way jcore_tsb_pick_way() selects -- the MATCHING way if this VPN
	 * is already present, so this cannot leave a stale duplicate of it
	 * in the other way.
	 */
	/*
	 * Deliberately a raw 32-bit read, not get_asid() (which returns only
	 * the 12-bit ASID). tag_lo is compared full-width by the RTL, and
	 * bits[15:12] are zero by construction here now that the generation
	 * nibble set_asid() used to pack into them has been retired -- so the
	 * two disagreeing about ASID_TAG's width is safe, not a live bug.
	 */
	asid_tag = __raw_readl(JCORE_ASIDR);
	tsb_set = jcore_tsb_slot_addr(address);
	jcore_tsb_write_entry(tsb_set, jcore_tsb_pick_way(tsb_set, pteh),
			      pteh, asid_tag, ptel);

	local_irq_restore(flags);
}

/*
 * local_flush_tlb_all() - MMUCR.TI is a self-clearing write-1 strobe that
 * invalidates every TLB entry (hardware-spec.md §2.3).
 *
 * MMUCR.TI reaches the *hardware* TLB only. The software TSB
 * (jcore_boot_tsb) is a second, independent translation cache that the
 * hardware walker installs from on a miss without consulting the page
 * table. A TSB
 * slot that survived a flush is therefore not a stale hint but a stale
 * *translation*: write-protecting a pte and flushing (fork()/COW,
 * mprotect(), dirty tracking) would leave the pre-flush W=1 slot live and
 * the next write would silently succeed against the now-shared page. It
 * also strands the fast path in a protection-fault livelock once the
 * walker starts withholding PTEL.W from clean ptes. So invalidate both
 * caches here, together, always.
 *
 * A full memset() rather than a targeted slot invalidate: the boot TSB is
 * 8 KB (JCORE_BOOT_TSB_BYTES: 256 sets x 32 bytes), local_flush_tlb_one()
 * has no single-entry
 * hardware primitive to pair with anyway and funnels straight here, and a
 * flush must never be cheaper than correct.
 *
 * SMP scope: this is the local flush and it now zeroes only THIS CPU's TSB
 * row. That is complete rather than a compromise: every cross-CPU flush
 * already runs local_flush_tlb_all() on each CPU through an IPI
 * (arch/sh/kernel/smp.c flush_tlb_all/mm/range -> on_each_cpu), so each CPU
 * invalidates its own TLB and its own TSB. The old shared-TSB race -- one
 * CPU memset()ing rows another was walking -- is gone with the sharing.
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
	/*
	 * After the hardware strobe, and with interrupts off, so nothing on
	 * this CPU can re-populate a slot in between. jcore_boot_tsb lives
	 * in kernel BSS (P1, untranslated), so the memset() itself cannot
	 * recurse into a TLB miss.
	 */
	memset(jcore_boot_tsb[raw_smp_processor_id()], 0,
	       JCORE_BOOT_TSB_BYTES);
	local_irq_restore(flags);
}

/*
 * local_flush_tlb_one() - J4 has no single-entry TLB invalidate op, only
 * the MMUCR.TI full-flush strobe. Emulate one-entry invalidation with a
 * full flush; SP2/SP3 can revisit this if TLB-flush-storm cost from
 * unmap()/mprotect() hot loops (tlbflush_32.c's local_flush_tlb_range()/
 * local_flush_tlb_kernel_range()) turns out to matter in practice.
 *
 * IF THAT REVISIT HAPPENS, note there is no such thing as a per-page TSB
 * invalidate for a page larger than 4 KB (contract K4). Because the TSB tag
 * and index are both 4 KB-granular (JCORE_TSB_TAG_MASK), one page owns up to
 * one row per TOUCHED 4 KB sub-page, in different sets. A targeted
 * invalidation must therefore either stay a whole-TSB flush -- what
 * local_flush_tlb_all() does -- or iterate every 4 KB sub-page of the range.
 * Zeroing "the row for @page" would strand every other sub-page's row, and
 * a stranded row is a stale TRANSLATION the walker installs without ever
 * consulting software, not a stale hint.
 */
void local_flush_tlb_one(unsigned long asid, unsigned long page)
{
	local_flush_tlb_all();
}
