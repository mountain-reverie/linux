// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/sh/mm/cache-jcore.c
 *
 * Cache maintenance for the J-Core J4 (CONFIG_CPU_JCORE).
 *
 * The J4 drives the same cache-control register (CCR) as the J2: a
 * "jcore,cache" block in which word N controls core N's L1-I and L1-D.
 * cache-j2.c drives it for CONFIG_CPU_J2, but neither that file nor the
 * arch/sh/kernel/cpu/sh2/probe.c code that maps it is built for
 * CONFIG_CPU_JCORE -- sh2/Makefile drops probe.o and mm/Makefile picks
 * this file instead -- so the mapping is repeated here rather than shared.
 *
 * Register layout, read off the RTL that decodes it
 * (jcore-cpu:cache/icache_modereg.vhm):
 *
 *   The block decodes address bits [5:2], so the per-core stride is four
 *   bytes: one u32 per core, indexed as "base + cpu" on a u32 __iomem *.
 *   A write that lands outside a decoded word hits the VHDL "when others"
 *   arm and is dropped with no bus error, so getting the stride wrong is
 *   silent. (sh2/probe.c indexes the same block as "base + 4 * cpu", which
 *   is 16 bytes per core and lands in exactly that hole. It is a
 *   CONFIG_CPU_J2-only file, not built here, and is not touched by this
 *   change.)
 *
 *     bit 0   L1-I enable          bit 8   L1-I invalidate (write-1, pulse)
 *     bit 1   L1-D enable          bit 9   L1-D invalidate (write-1, pulse)
 *     bit 28  raise this core's interrupt (the IPI, see cpu/sh2/smp-j2.c)
 *
 *   A read returns the two enable bits and a hard-wired 1 in bit 31; the
 *   invalidate and interrupt bits always read as zero. A read/modify/write
 *   is therefore safe: it preserves this core's enable bits and cannot
 *   re-raise an interrupt.
 *
 * Every write below targets *this* core's word and no other. Word N+1 holds
 * core N+1's invalidate bits, so a loop over the words would let one core
 * invalidate another core's entire L1-I and L1-D -- the cross-domain reach
 * docs/cache/l2-spec.md 16.2 "P-R8" forbids, on the register P-R8 says the
 * SoC ships outside P4. Cross-core reach comes from the IPI in
 * cacheop_on_each_cpu() instead, which is why that function has a J-Core
 * arm.
 */

#include <linux/bits.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/preempt.h>
#include <linux/smp.h>

#include <asm/cache.h>
#include <asm/cacheflush.h>
#include <asm/io.h>
#include <asm/processor.h>
#include <asm/smp.h>

#define JCORE_CCR_IC_INVALIDATE	BIT(8)
#define JCORE_CCR_DC_INVALIDATE	BIT(9)

static u32 __iomem *jcore_ccr_base;
static unsigned int jcore_ccr_words;

/*
 * Pulse the requested invalidate bits in the issuing core's own CCR word.
 *
 * Preemption is disabled across the whole sequence and not merely around
 * the read of the CPU id: if we migrated between picking the word and
 * writing it we would invalidate some other core's caches, which is the
 * very reach this file exists to avoid.
 */
static void jcore_ccr_invalidate(u32 bits)
{
	unsigned int cpu;

	if (!jcore_ccr_base)
		return;

	preempt_disable();
	cpu = hard_smp_processor_id();
	if (cpu < jcore_ccr_words) {
		u32 __iomem *ccr = jcore_ccr_base + cpu;

		__raw_writel(__raw_readl(ccr) | bits, ccr);
	}
	preempt_enable();
}

static void jcore_flush_icache(void *args)
{
	jcore_ccr_invalidate(JCORE_CCR_IC_INVALIDATE);
}

static void jcore_flush_dcache(void *args)
{
	jcore_ccr_invalidate(JCORE_CCR_DC_INVALIDATE);
}

static void jcore_flush_both(void *args)
{
	jcore_ccr_invalidate(JCORE_CCR_IC_INVALIDATE | JCORE_CCR_DC_INVALIDATE);
}

/*
 * The CCR's only data-side operation is "invalidate the whole L1-D", so the
 * region helpers below ignore their start/size arguments and flush the
 * entire cache. That is safe rather than merely conservative: the J-Core
 * L1-D is write-through with no dirty bit and no writeback path at all
 * (jcore-cpu:cache/dcache_cacheable_mux.vhd, core/cpu.vhd), so no valid
 * line can hold the only copy of anything and invalidating more than was
 * asked for cannot lose a write.
 *
 * The same property is why __flush_wback_region() has nothing to do: main
 * memory already holds every byte the CPU has stored. If the L1-D ever
 * becomes write-back (docs/decisions/0007 decision 2), this file must be
 * revisited -- and note that the CCR offers no writeback primitive to
 * revisit it *with*, so the fix is a hardware one, not a bigger loop here.
 *
 * Each region helper reaches every core, because the buffer a driver hands
 * to the DMA API may be read from a core other than the one that called
 * dma_sync_*(). Since the CCR word is per core, that reach has to be an
 * IPI; cacheop_on_each_cpu() is where it lives, and it only sends one when
 * more than one CPU is online.
 *
 * These helpers have two callers with different needs, and the asymmetry
 * between them is deliberate. Read this before "fixing" it.
 *
 * The DMA path -- arch_sync_dma_for_cpu()/_for_device() -- must invalidate,
 * or a device write is never seen. It keeps them.
 *
 * sys_cacheflush(2) must not. It is unprivileged and validates only that the
 * range lies in one of the caller's own VMAs, so with these helpers behind it
 * a process holding a single page can invalidate the entire L1-D as often as
 * it likes: the CCR's dc_inv clears every valid bit in one cycle
 * (jcore-cpu:cache/dcache_ccl.vhm) and cacheop_on_each_cpu() carries it to
 * every online core. That is a denial of service against everything else on
 * the machine and the Flush half of a Flush+Reload. It is also new: before
 * this file existed the J4 had no cacheops arm, sys_cacheflush(2) resolved to
 * noop__flush_region() and did nothing, so no J4 userspace can depend on the
 * data side doing work. asm/cacheflush.h's cacheflush_user_dside_acts() is
 * where the syscall stops.
 *
 * Nothing correct is lost by stopping it:
 *
 *  - CACHEFLUSH_D_WB has nothing to do, for the write-through reason above.
 *  - CACHEFLUSH_D_INVAL and _D_PURGE exist so a process can re-read memory
 *    that a device wrote behind the cache. Userspace cannot hold such a
 *    buffer cached in the first place: dma_mmap_*() maps coherent memory
 *    through dma_pgprot() -> pgprot_noncached(), which on SH is
 *    pgprot_writecombine() and clears _PAGE_CACHABLE (asm/pgtable_32.h), and
 *    a streaming mapping is synced by the kernel in arch_sync_dma_for_cpu(),
 *    not by the process that owns the pages.
 *  - Neither honoured start/size anyway, so no range semantics are being
 *    withdrawn -- there were none to withdraw.
 *
 * Revisit that decision if the L1-D becomes write-back (decisions/0007
 * decision 2), or if a cached user mapping of a DMA buffer becomes reachable.
 * Do not revisit it merely because the two callers now call different things.
 *
 * CACHEFLUSH_I is *not* gated, and stays as an accepted residual with a
 * stated reason. Self-modifying code and JITs need an I-cache invalidate to
 * be correct; the CCR offers only the whole-cache one; and a JIT that writes
 * on one core and branches to the code on another needs the IPI as well. So
 * an unprivileged whole-L1-I invalidate stays reachable. It is a weaker
 * primitive than the data-side one -- the I-cache holds no data, which makes
 * it a denial of service and an eviction-timing signal rather than a data
 * channel -- and docs/security/threat-model.md section 8, item L5 records it
 * as accepted rather than closed.
 */
static void jcore__flush_wback_region(void *start, int size)
{
}

static void jcore__flush_purge_region(void *start, int size)
{
	cacheop_on_each_cpu(jcore_flush_dcache, NULL, 1);
}

static void jcore__flush_invalidate_region(void *start, int size)
{
	cacheop_on_each_cpu(jcore_flush_dcache, NULL, 1);
}

/*
 * Map the CCR.
 *
 * This cannot be done from jcore_cache_init(): cpu_cache_init() runs from
 * the top of mem_init(), while mem_init_done is still 0, and an MMU J-Core
 * build has no CONFIG_IOREMAP_FIXED (it depends on X2TLB), so ioremap()
 * takes the BUG() stub in arch/sh/mm/ioremap.h in that window. An initcall
 * is the first point at which the region can be mapped. Until it runs the
 * helpers above are no-ops -- which is what the entire J4 build did before
 * this file existed, so nothing regresses in the gap.
 *
 * of_iomap()/of_address_to_resource() honour the size in "reg", unlike
 * sh2/probe.c's fixed ioremap(addr, 4) of a block it then indexes per core.
 */
static int __init jcore_cache_ccr_init(void)
{
	struct device_node *np;
	struct resource res;
	void __iomem *base;

	np = of_find_compatible_node(NULL, NULL, "jcore,cache");
	if (!np) {
		pr_warn("J-Core: no jcore,cache node, cache maintenance is a no-op\n");
		return 0;
	}

	if (of_address_to_resource(np, 0, &res)) {
		pr_warn("J-Core: jcore,cache has no usable reg\n");
		goto out;
	}

	base = ioremap(res.start, resource_size(&res));
	if (!base) {
		pr_warn("J-Core: cannot map jcore,cache at %pa\n", &res.start);
		goto out;
	}

	jcore_ccr_words = resource_size(&res) / sizeof(u32);
	if (jcore_ccr_words < nr_cpu_ids)
		pr_warn("J-Core: jcore,cache covers %u of %u cores; the rest keep stale lines\n",
			jcore_ccr_words, nr_cpu_ids);

	/* Published last: the helpers test this pointer before the count. */
	jcore_ccr_base = base;

	pr_info("J-Core CCR at %pa, %u core word(s), CCR[0] is %.8x\n",
		&res.start, jcore_ccr_words, __raw_readl(jcore_ccr_base));
out:
	of_node_put(np);
	return 0;
}
core_initcall(jcore_cache_ccr_init);

void __init jcore_cache_init(void)
{
	local_flush_cache_all = jcore_flush_both;
	local_flush_cache_mm = jcore_flush_both;
	local_flush_cache_dup_mm = jcore_flush_both;
	local_flush_cache_page = jcore_flush_both;
	local_flush_cache_range = jcore_flush_both;
	local_flush_dcache_folio = jcore_flush_dcache;
	local_flush_icache_range = jcore_flush_icache;
	local_flush_icache_folio = jcore_flush_icache;
	local_flush_cache_sigtramp = jcore_flush_icache;

	__flush_wback_region = jcore__flush_wback_region;
	__flush_purge_region = jcore__flush_purge_region;
	__flush_invalidate_region = jcore__flush_invalidate_region;
}
