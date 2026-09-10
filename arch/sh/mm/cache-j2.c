// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/sh/mm/cache-j2.c
 *
 * Copyright (C) 2015-2016 Smart Energy Instruments, Inc.
 */

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/preempt.h>
#include <linux/smp.h>

#include <asm/cache.h>
#include <asm/addrspace.h>
#include <asm/processor.h>
#include <asm/smp.h>
#include <asm/cacheflush.h>
#include <asm/io.h>

#define ICACHE_ENABLE	0x1
#define DCACHE_ENABLE	0x2
#define CACHE_ENABLE	(ICACHE_ENABLE | DCACHE_ENABLE)
#define ICACHE_FLUSH	0x100
#define DCACHE_FLUSH	0x200
#define CACHE_FLUSH	(ICACHE_FLUSH | DCACHE_FLUSH)

u32 __iomem *j2_ccr_base;

/*
 * Write the issuing core's own cache-control word, and no other.
 *
 * These three used to loop over for_each_possible_cpu(), which reached
 * every *other* core's whole-cache invalidate bits through the per-core
 * window of the same register -- the cross-domain reach
 * docs/cache/l2-spec.md 16.2 "P-R8" forbids, on the register P-R8 says the
 * SoC ships outside P4. The cross-core reach the loop provided is now taken
 * from the IPI in cacheop_on_each_cpu(), which grew a CONFIG_CPU_J2 arm in
 * the same change.
 *
 * Preemption is disabled across the whole sequence, not just the read of
 * the CPU id: migrating between picking the word and writing it would
 * reintroduce exactly the cross-core write being removed.
 */
static void j2_flush_ccr(u32 bits)
{
	unsigned int cpu;

	preempt_disable();
	cpu = hard_smp_processor_id();
	__raw_writel(bits, j2_ccr_base + cpu);
	preempt_enable();
}

static void j2_flush_icache(void *args)
{
	j2_flush_ccr(CACHE_ENABLE | ICACHE_FLUSH);
}

static void j2_flush_dcache(void *args)
{
	j2_flush_ccr(CACHE_ENABLE | DCACHE_FLUSH);
}

static void j2_flush_both(void *args)
{
	j2_flush_ccr(CACHE_ENABLE | CACHE_FLUSH);
}

void __init j2_cache_init(void)
{
	if (!j2_ccr_base)
		return;

	local_flush_cache_all = j2_flush_both;
	local_flush_cache_mm = j2_flush_both;
	local_flush_cache_dup_mm = j2_flush_both;
	local_flush_cache_page = j2_flush_both;
	local_flush_cache_range = j2_flush_both;
	local_flush_dcache_folio = j2_flush_dcache;
	local_flush_icache_range = j2_flush_icache;
	local_flush_icache_folio = j2_flush_icache;
	local_flush_cache_sigtramp = j2_flush_icache;

	pr_info("Initial J2 CCR is %.8x\n", __raw_readl(j2_ccr_base));
}
