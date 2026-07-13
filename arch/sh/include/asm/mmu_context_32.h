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

static inline void set_asid(unsigned long asid)
{
	unsigned long tag = jcore_encode_asid_tag(asid, 0);

	__asm__ __volatile__ ("ldc %0, asidr"
			      : : "r" (tag));
}

static inline unsigned long get_asid(void)
{
	unsigned long asid;

	__asm__ __volatile__ ("stc asidr, %0"
			      : "=r" (asid));
	return asid & MMU_CONTEXT_ASID_MASK;
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
