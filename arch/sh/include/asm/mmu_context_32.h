/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_SH_MMU_CONTEXT_32_H
#define __ASM_SH_MMU_CONTEXT_32_H

#ifdef CONFIG_CPU_JCORE
/*
 * J-Core J4: ASID lives in the dedicated ASIDR control register, not in
 * PTEH like SH-3/SH-4. Writes use LDC; reads use the read-only MMIO alias
 * at JCORE_ASIDR (0xFF000038) — `STC ASIDR,Rn` was retired with the other
 * six MMU read encodings once the hardware TSB walker made the software
 * fast path dead code (hardware-spec.md §2.1a, §3.1). ASIDR holds a plain
 * 12-bit ASID in bits [11:0]; bits [15:12] are always zero. ASID 0 is
 * reserved for the kernel/global mappings (_PAGE_GLOBAL suppresses the
 * ASID compare in hardware).
 *
 * ASIDR used to carry a 4-bit generation nibble in bits [15:12] so
 * recycled 12-bit ASIDs could coexist in a shared TSB for 16 generations
 * without false hits (security-review S-I3). That stopped isolating
 * anything once local_flush_tlb_all() started zeroing the TSB on EVERY
 * version wrap (the mitigation ran on an already-empty TSB), and per-CPU
 * TSBs removed the cross-CPU sharing it was protecting in the first
 * place, so the nibble was retired.
 */
static inline void set_asid(unsigned long asid)
{
	__asm__ __volatile__ ("ldc %0, asidr"
			      : : "r" (asid & MMU_CONTEXT_ASID_MASK));
}

static inline unsigned long get_asid(void)
{
	/*
	 * Read the P4 MMIO alias (0xFF000038) rather than STC ASIDR -- that
	 * form was retired with the other six MMU read encodings; the LDC
	 * write side above is unaffected.
	 *
	 * This must read HARDWARE, not asid_cache(cpu): tlbflush_32.c's
	 * save/restore-around-flush dance captures the ASID of one mm and
	 * restores it later, so ASIDR does not always hold the running mm's
	 * asid_cache entry.
	 */
	return __raw_readl(JCORE_ASIDR) & MMU_CONTEXT_ASID_MASK;
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
