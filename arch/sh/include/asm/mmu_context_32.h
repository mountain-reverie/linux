/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_SH_MMU_CONTEXT_32_H
#define __ASM_SH_MMU_CONTEXT_32_H

#ifdef CONFIG_CPU_JCORE
/*
 * J-Core J4: ASID lives in the dedicated ASIDR control register
 * (LDC/STC-only, no MMIO address — hardware-spec.md §2.1a), not in
 * PTEH like SH-3/SH-4. ASIDR holds a 16-bit ASID_TAG:
 *   bits [11:0]  = 12-bit ASID
 *   bits [15:12] = low 4 bits of the TLB generation/rollover counter
 * (linux-spec.md §5.2-5.3). ASID 0 is reserved for the kernel/global
 * mappings (_PAGE_GLOBAL suppresses the ASID compare in hardware).
 */
#define JCORE_ASID_GEN_SHIFT	12
#define JCORE_ASID_GEN_BITS	4
#define JCORE_ASID_GEN_MASK	((1U << JCORE_ASID_GEN_BITS) - 1)

static inline u16 jcore_encode_asid_tag(u16 asid, u64 gen)
{
	return (asid & MMU_CONTEXT_ASID_MASK) |
	       (((u32)(gen >> JCORE_ASID_GEN_SHIFT) & JCORE_ASID_GEN_MASK)
			<< JCORE_ASID_GEN_SHIFT);
}

/*
 * True iff version bump lands gen_low back on 0 (every 16th rollover) --
 * point where gen_low nibble is about to be reused. Triggers TSB rebuild
 * to reject stale entries per security-review S-I3.
 */
static inline bool jcore_asid_gen_wrapped(unsigned long ctx)
{
	return (((ctx >> JCORE_ASID_GEN_SHIFT) & JCORE_ASID_GEN_MASK) == 0);
}

static inline void set_asid(unsigned long asid)
{
	/*
	 * Thread the current TLB generation (version nibble of the running
	 * context) into ASID_TAG[15:12]. asid_cache(cpu) holds the full
	 * cpu_context (asid | version<<12) for the CPU we are switching on:
	 * activate_context()/switch_mm() set it (via get_mmu_context) before
	 * calling set_asid. The generation-tagged tag lets recycled 12-bit
	 * ASIDs coexist in the global TSB for 16 generations without false
	 * hits; jcore_tsb_flush_on_generation() rebuilds the TSB on wrap
	 * (security-review S-I3, hardware-spec §2.1a).
	 *
	 * Caveat: this composes asid_cache(cpu)'s CURRENT gen_low with the
	 * *passed* asid, not the passed asid's own gen_low. That's exactly
	 * right for activate_context()/switch_mm(). The tlbflush_32.c
	 * save/restore dance instead does set_asid(saved_asid) where
	 * saved_asid was captured by get_asid() (full tag, own gen_low) from
	 * a *different* mm switched on the same cpu earlier -- so this
	 * recomposes it with whatever gen_low is live now. That's harmless
	 * today only because jcore's local_flush_tlb_one() is actually a
	 * full local_flush_tlb_all() that ignores the programmed ASID_TAG
	 * entirely; it would be a latent bug if local_flush_tlb_one() ever
	 * became a tag-qualified single-entry invalidate.
	 */
	unsigned long gen = asid_cache(raw_smp_processor_id());
	unsigned long tag = jcore_encode_asid_tag(asid, gen);

	__asm__ __volatile__ ("ldc %0, asidr" : : "r" (tag));
}

static inline unsigned long get_asid(void)
{
	unsigned long tag;

	/*
	 * Read the P4 MMIO alias (0xFF000038) rather than STC ASIDR -- that
	 * form is retiring (Phase 3, jcore-cpu task-1); the LDC write side
	 * in set_asid() above is unaffected (D7).
	 *
	 * This must read HARDWARE, not asid_cache(cpu): asid_cache(cpu)
	 * tracks the full context of the mm currently ACTIVE on this CPU,
	 * but set_asid()'s own comment records that tlbflush_32.c's save/
	 * restore dance calls set_asid(saved_asid) with a tag captured by an
	 * *earlier* get_asid() from a *different* mm, recomposed with
	 * whatever gen_low happens to be live now. That is exactly a case
	 * where the value ASIDR holds is not the running mm's asid_cache
	 * entry, so asid_cache(cpu) is not an equivalent, cheaper substitute
	 * here -- only the hardware register reflects what the last
	 * set_asid() actually programmed.
	 */
	tag = __raw_readl(JCORE_ASIDR);
	/*
	 * Return the full 16-bit ASID_TAG (asid | gen_low<<12), not just the
	 * 12-bit ASID: tlbflush_32.c saves this and restores it verbatim via
	 * set_asid(), so gen_low must survive the round-trip.
	 *
	 * MMU_NO_ASID sentinel invariant: MMU_NO_ASID is 0x10000 (bit 16;
	 * see mmu_context.h), which is unrepresentable in the 16-bit tag
	 * returned here. Every possible saved_asid = get_asid() value is
	 * therefore provably != MMU_NO_ASID, unconditionally -- no
	 * reasoning about live user mm / asid != 0 is needed.
	 */
	return tag & 0xffff;
}

/*
 * TTB is not consulted by the J4 hardware page-table walker; it is kept
 * purely as an SH-4-compatible MMIO scratch register (0xFF000008,
 * hardware-spec.md §2.4) that Linux uses to stash current_pgd for the
 * slow-path TLB-miss walker.
 */
static inline void set_TTB(pgd_t *pgd)
{
	__raw_writel((unsigned long)pgd, MMU_TTB);
}

static inline pgd_t *get_TTB(void)
{
	return (pgd_t *)__raw_readl(MMU_TTB);
}
#else /* !CONFIG_CPU_JCORE */

#ifdef CONFIG_CPU_HAS_PTEAEX
static inline void set_asid(unsigned long asid)
{
	__raw_writel(asid, MMU_PTEAEX);
}

static inline unsigned long get_asid(void)
{
	return __raw_readl(MMU_PTEAEX) & MMU_CONTEXT_ASID_MASK;
}
#else
static inline void set_asid(unsigned long asid)
{
	unsigned long __dummy;

	__asm__ __volatile__ ("mov.l	%2, %0\n\t"
			      "and	%3, %0\n\t"
			      "or	%1, %0\n\t"
			      "mov.l	%0, %2"
			      : "=&r" (__dummy)
			      : "r" (asid), "m" (__m(MMU_PTEH)),
			        "r" (0xffffff00));
}

static inline unsigned long get_asid(void)
{
	unsigned long asid;

	__asm__ __volatile__ ("mov.l	%1, %0"
			      : "=r" (asid)
			      : "m" (__m(MMU_PTEH)));
	asid &= MMU_CONTEXT_ASID_MASK;
	return asid;
}
#endif /* CONFIG_CPU_HAS_PTEAEX */

/* MMU_TTB is used for optimizing the fault handling. */
static inline void set_TTB(pgd_t *pgd)
{
	__raw_writel((unsigned long)pgd, MMU_TTB);
}

static inline pgd_t *get_TTB(void)
{
	return (pgd_t *)__raw_readl(MMU_TTB);
}
#endif /* CONFIG_CPU_JCORE */
#endif /* __ASM_SH_MMU_CONTEXT_32_H */
