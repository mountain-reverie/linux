/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 1999 Niibe Yutaka
 * Copyright (C) 2003 - 2007 Paul Mundt
 *
 * ASID handling idea taken from MIPS implementation.
 */
#ifndef __ASM_SH_MMU_CONTEXT_H
#define __ASM_SH_MMU_CONTEXT_H

#include <cpu/mmu_context.h>
#include <asm/tlbflush.h>
#include <linux/random.h>
#include <linux/uaccess.h>
#include <linux/mm_types.h>

#include <asm/io.h>
#include <asm-generic/mm_hooks.h>

/*
 * The MMU "context" consists of two things:
 *    (a) TLB cache version (or round, cycle whatever expression you like)
 *    (b) ASID (Address Space IDentifier)
 */
#ifdef CONFIG_CPU_HAS_PTEAEX
#define MMU_CONTEXT_ASID_MASK		0x0000ffff
#elif defined(CONFIG_CPU_JCORE)
#define MMU_CONTEXT_ASID_MASK		0x00000fff
#else
#define MMU_CONTEXT_ASID_MASK		0x000000ff
#endif

#define MMU_CONTEXT_VERSION_MASK	(~0UL & ~MMU_CONTEXT_ASID_MASK)
#define MMU_CONTEXT_FIRST_VERSION	(MMU_CONTEXT_ASID_MASK + 1)

/*
 * Impossible ASID value, to differentiate from NO_CONTEXT. Must fall outside
 * the range get_asid() can return, so it works as the "nothing captured yet"
 * sentinel in tlbflush_32.c's save/restore-around-flush dance. One past the
 * ASID mask satisfies that on every subtype, jcore included now that
 * get_asid() returns a plain 12-bit ASID rather than a generation-tagged
 * 16-bit ASID_TAG.
 */
#define MMU_NO_ASID			MMU_CONTEXT_FIRST_VERSION
#define NO_CONTEXT			0UL

#define asid_cache(cpu)		(cpu_data[cpu].asid_cache)

#ifdef CONFIG_MMU
#define cpu_context(cpu, mm)	((mm)->context.id[cpu])

#define cpu_asid(cpu, mm)	\
	(cpu_context((cpu), (mm)) & MMU_CONTEXT_ASID_MASK)

/*
 * Virtual Page Number mask
 */
#define MMU_VPN_MASK	0xfffff000

#include <asm/mmu_context_32.h>

/*
 * Get MMU context if needed.
 */
static inline void get_mmu_context(struct mm_struct *mm, unsigned int cpu)
{
	unsigned long asid = asid_cache(cpu);

	/* Check if we have old version of context. */
	if (((cpu_context(cpu, mm) ^ asid) & MMU_CONTEXT_VERSION_MASK) == 0)
		/* It's up to date, do nothing */
		return;

	/* It's old, we need to get new context with new version. */
	if (!(++asid & MMU_CONTEXT_ASID_MASK)) {
		/*
		 * ASID space exhausted: flush this CPU's TLB and TSB and start
		 * a new version. Local, not the SMP-broadcast flush_tlb_all():
		 * each CPU wraps its own asid_cache and owns its own TLB and
		 * its own TSB row, so there is nothing on another CPU that this
		 * wrap invalidates.
		 */
		local_flush_tlb_all();

		/*
		 * Fix version; Note that we avoid version #0
		 * to distinguish NO_CONTEXT.
		 */
		if (!asid)
			asid = MMU_CONTEXT_FIRST_VERSION;
	}

	cpu_context(cpu, mm) = asid_cache(cpu) = asid;
}

/*
 * Initialize the context related info for a new mm_struct
 * instance.
 */
#define init_new_context init_new_context
static inline int init_new_context(struct task_struct *tsk,
				   struct mm_struct *mm)
{
	int i;

	for_each_online_cpu(i)
		cpu_context(i, mm) = NO_CONTEXT;

	return 0;
}

/*
 * After we have set current->mm to a new value, this activates
 * the context for the new mm so we see the new mappings.
 */
static inline void activate_context(struct mm_struct *mm, unsigned int cpu)
{
	get_mmu_context(mm, cpu);
	set_asid(cpu_asid(cpu, mm));
}

static inline void switch_mm(struct mm_struct *prev,
			     struct mm_struct *next,
			     struct task_struct *tsk)
{
	unsigned int cpu = smp_processor_id();

	if (likely(prev != next)) {
		cpumask_set_cpu(cpu, mm_cpumask(next));
		set_TTB(next->pgd);
		activate_context(next, cpu);
	} else {
		if (!cpumask_test_and_set_cpu(cpu, mm_cpumask(next)))
			activate_context(next, cpu);
		else
			/*
			 * The arm that reprograms nothing. Reaching it with a
			 * revoked context means we are about to run on an ASID
			 * some earlier local_flush_tlb_mm()/_range() orphaned
			 * -- the lazy-TLB hole those two now close by keying on
			 * active_mm. Unreachable once they do; kept as the
			 * tripwire for a regression, since the failure is
			 * otherwise silent (stale translations, no fault).
			 */
			VM_WARN_ON_ONCE(cpu_context(cpu, next) == NO_CONTEXT);
	}
}

#include <asm-generic/mmu_context.h>

#else

#define set_asid(asid)			do { } while (0)
#define get_asid()			(0)
#define cpu_asid(cpu, mm)		({ (void)cpu; NO_CONTEXT; })
#define switch_and_save_asid(asid)	(0)
#define set_TTB(pgd)			do { } while (0)
#define get_TTB()			(0)

#include <asm-generic/nommu_context.h>

#endif /* CONFIG_MMU */

#if defined(CONFIG_CPU_SH3) || defined(CONFIG_CPU_SH4) || defined(CONFIG_CPU_JCORE)
/*
 * If this processor has an MMU, we need methods to turn it off/on ..
 * paging_init() will also have to be updated for the processor in
 * question.
 */
#if defined(CONFIG_CPU_JCORE)
/*
 * J-Core J4 MMUCR: AT (bit0) enables translation, TI (bit2) is a
 * write-1-to-flush-all-TLB-entries strobe (hardware-spec.md §2.3,
 * linux-spec.md §7.1). enable_mmu() sets AT and pulses TI so the MMU
 * comes up with a clean TLB; disable_mmu() clears AT and pulses TI.
 */
static inline void enable_mmu(void)
{
	unsigned int cpu = smp_processor_id();

	/*
	 * Program this CPU's own TSB, then enable translation -- via the same
	 * jcore_mmu_enable() head_32.S calls for CPU0, so there is exactly one
	 * MMU-programming sequence in the tree and TSBBR/TSBCFG/ASIDR/PTEH/
	 * MMUCR are always set as a group.
	 *
	 * This is what makes the per-CPU ASID namespace sound: a secondary
	 * enters _stext and comes up on row 0 like everyone else, and this is
	 * where it moves to its own row. That window is provably empty rather
	 * than merely short -- the kernel is linked at PAGE_OFFSET +
	 * __MEMORY_START = 0x90000000, which is P1 and untranslated, and
	 * nothing between _stext and start_secondary() makes a P0/P3 access,
	 * so no TSB row can have been created on row 0 by then.
	 *
	 * Re-running this on CPU0 from setup.c after head_32.S already ran it
	 * is idempotent: same base, same TI pulse, same ASIDR = 0.
	 */
	jcore_mmu_enable((unsigned long)jcore_boot_tsb[cpu]);
	ctrl_barrier();

	/*
	 * Seed this CPU's TSB victim LFSR. JCORE_TSB_VSEED is a per-core
	 * register, so this cannot be a global initcall -- it used to be an
	 * early_initcall in probe.c and therefore ran on CPU0 only, leaving
	 * every secondary with an unseeded selector. Write-only in hardware;
	 * the seed is never read back, which is the point (an open-source core
	 * publishes the polynomial, so a recoverable seed is no seed at all).
	 */
	__raw_writel(get_random_u32(), (void __iomem *)JCORE_TSB_VSEED);

	if (asid_cache(cpu) == NO_CONTEXT)
		asid_cache(cpu) = MMU_CONTEXT_FIRST_VERSION;

	set_asid(asid_cache(cpu) & MMU_CONTEXT_ASID_MASK);
}

static inline void disable_mmu(void)
{
	/* Disable MMU, flush TLB (AT=0, TI=1) */
	__raw_writel(MMUCR_TI, MMUCR);
	ctrl_barrier();
}
#else
static inline void enable_mmu(void)
{
	unsigned int cpu = smp_processor_id();

	/* Enable MMU */
	__raw_writel(MMU_CONTROL_INIT, MMUCR);
	ctrl_barrier();

	if (asid_cache(cpu) == NO_CONTEXT)
		asid_cache(cpu) = MMU_CONTEXT_FIRST_VERSION;

	set_asid(asid_cache(cpu) & MMU_CONTEXT_ASID_MASK);
}

static inline void disable_mmu(void)
{
	unsigned long cr;

	cr = __raw_readl(MMUCR);
	cr &= ~MMU_CONTROL_INIT;
	__raw_writel(cr, MMUCR);

	ctrl_barrier();
}
#endif
#else
/*
 * MMU control handlers for processors lacking memory
 * management hardware.
 */
#define enable_mmu()	do { } while (0)
#define disable_mmu()	do { } while (0)
#endif

#endif /* __ASM_SH_MMU_CONTEXT_H */
