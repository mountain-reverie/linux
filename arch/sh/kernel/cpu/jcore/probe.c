// SPDX-License-Identifier: GPL-2.0
/*
 * arch/sh/kernel/cpu/jcore/probe.c
 *
 * CPU Subtype Probing for J-Core J4 (MMU).
 *
 * Modeled on arch/sh/kernel/cpu/sh2/probe.c. J4 exposes a read-only,
 * per-CPU-distinct CPUINFO MMIO register (hardware-spec.md §2.9,
 * linux-spec.md §6.1) at a fixed physical address; low nibble is the
 * hart/core id, upper bits are capability flags. SP1 only needs enough
 * here to populate boot_cpu_data for /proc/cpuinfo and family dispatch
 * -- full capability-flag decoding is deferred to SP2/SP3 SMP bring-up.
 */
#include <linux/build_bug.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/random.h>
#include <linux/smp.h>
#include <linux/threads.h>
#include <asm/processor.h>
#include <asm/cache.h>
#include <cpu/mmu_context.h>

/* hardware-spec.md §2.9: read-only MMIO, per-CPU-distinct. */
#define JCORE_CPUINFO_MMIO	0xFF000020

/*
 * Per-CPU TSBs. One row per possible CPU, each pointed at by that CPU's own
 * TSBBR (programmed in enable_mmu(), asm/mmu_context.h).
 *
 * These MUST NOT be shared. Linux allocates ASIDs per CPU -- asid_cache is
 * per-CPU and cpu_context is indexed by cpu -- so two CPUs hand the same
 * 12-bit ASID to different address spaces as a matter of course, both
 * counters starting at version 1 and advancing in lockstep. A shared TSB
 * turns that into a cross-address-space hit: the peer's row matches on both
 * tag_hi and tag_lo and the walker installs its PTEL, with no exception and
 * no diagnostic. Guarded by jcore-cpu sim/tests/dualcore/mmusmpasid.S, whose
 * SHARED_TSB flavour is exactly this bug.
 *
 * Element size equals the alignment, so every row is individually aligned to
 * JCORE_BOOT_TSB_BYTES and jcore_mmu_enable() can OR TSB_SIZE_LOG straight
 * into the low bits of any of them. Adding a per-row header would silently
 * break that.
 *
 * The name is deliberately unchanged from the old single-TSB days: three
 * jcore-cpu cosim harnesses (sim/tests/mmulinux.S, mmulinuxexc.S, mmuhuge.S)
 * define this symbol themselves to satisfy tlb-jcore.o, and head_32.S passes
 * `jcore_boot_tsb` (= row 0) to jcore_mmu_enable() before any CPU numbering
 * exists.
 */
char jcore_boot_tsb[NR_CPUS][JCORE_BOOT_TSB_BYTES]
	__aligned(JCORE_BOOT_TSB_BYTES);

/*
 * Machine-check the geometry invariant the comment above documents: element
 * size must equal JCORE_BOOT_TSB_BYTES for every row to land naturally
 * aligned, and JCORE_BOOT_TSB_BYTES itself must be a power of two for
 * jcore_mmu_enable() to OR TSB_SIZE_LOG straight into the low bits. A silent
 * failure here leaves TSBBR pointing into garbage with no diagnostic.
 */
static_assert(sizeof(jcore_boot_tsb[0]) == JCORE_BOOT_TSB_BYTES);
static_assert((JCORE_BOOT_TSB_BYTES & (JCORE_BOOT_TSB_BYTES - 1)) == 0);

/*
 * Re-seed every online CPU's TSB victim LFSR once the kernel RNG has
 * actually been initialised. enable_mmu() (asm/mmu_context.h) already seeds
 * JCORE_TSB_VSEED for every CPU the moment its MMU comes up, so no CPU is
 * ever left unseeded -- but CPU0's call happens from setup_arch(), before
 * random_init_early()/random_init() have mixed in any entropy, and on a
 * J-core FPGA target there is no RDSEED and no bootloader entropy to fall
 * back on. This late_initcall re-draws a real seed for every online CPU
 * (secondaries get a second, better draw too; that's harmless -- the
 * register is write-only and re-seeding never weakens it).
 *
 * The bare-metal cosim harnesses link tlb-jcore.o and define their own
 * jcore_boot_tsb stub directly (sim/tests/mmulinux.S etc.) -- they do not
 * link this file, so nothing added here needs a harness-side stub.
 */
static void jcore_reseed_tsb_vseed(void *unused)
{
	__raw_writel(get_random_u32(), (void __iomem *)JCORE_TSB_VSEED);
}

static int __init jcore_reseed_tsb_vseed_init(void)
{
	/*
	 * on_each_cpu() runs the callback on every online CPU, including the
	 * caller, and works correctly under !CONFIG_SMP (it just calls the
	 * function locally with interrupts disabled) -- no #ifdef needed.
	 */
	on_each_cpu(jcore_reseed_tsb_vseed, NULL, 1);
	return 0;
}
late_initcall(jcore_reseed_tsb_vseed_init);

void __ref cpu_probe(void)
{
	unsigned int cpuinfo = __raw_readl((void __iomem *)JCORE_CPUINFO_MMIO);

	boot_cpu_data.type	= CPU_JCORE;
	boot_cpu_data.family	= CPU_FAMILY_SH2;

	/*
	 * J4 has split I/D caches; real geometry (ways/sets/linesz) is
	 * SoC-configurable and not yet exposed via CPUINFO or DT -- SP2
	 * cache bring-up (arch/sh/kernel/cpu/init.c jcore_cache_init())
	 * fills these in properly. Zero-initialise here so /proc/cpuinfo
	 * doesn't print garbage in the meantime.
	 */
	boot_cpu_data.dcache.ways	= 0;
	boot_cpu_data.dcache.sets	= 0;
	boot_cpu_data.dcache.entry_shift = 0;
	boot_cpu_data.dcache.linesz	= L1_CACHE_BYTES;
	boot_cpu_data.dcache.flags	= 0;
	boot_cpu_data.icache		= boot_cpu_data.dcache;

	/* Capability flags (cpuinfo >> 16, linux-spec.md §6.1) reserved
	 * for SP2/SP3 SMP + feature discovery; not consumed in SP1. */
	(void)cpuinfo;
}
