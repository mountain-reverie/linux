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
#include <linux/init.h>
#include <linux/io.h>
#include <linux/random.h>
#include <asm/processor.h>
#include <asm/cache.h>
#include <cpu/mmu_context.h>

/* hardware-spec.md §2.9: read-only MMIO, per-CPU-distinct. */
#define JCORE_CPUINFO_MMIO	0xFF000020

/*
 * Boot TSB (head_32.S's JCORE MMU-enable arm programs TSBBR/TSBCFG to
 * point here before setting MMUCR.AT). Lives in BSS, aligned to its own
 * size (8 KB = 256 32-byte 2-way sets) so head_32.S can just OR
 * TSB_SIZE_LOG into the low bits of its physical address without any
 * masking (hardware-spec.md §2.6).
 */
char jcore_boot_tsb[JCORE_BOOT_TSB_BYTES] __aligned(JCORE_BOOT_TSB_BYTES);

/*
 * jcore_tsb_victim_seed_init() - hand the hardware TSB victim selector its
 * seed.
 *
 * The LFSR that nominates which way of a 2-way TSB set to replace has NO
 * seed of its own. That is deliberate: this is an open-source core, so a
 * constant compiled into the RTL would be published along with the
 * polynomial, and anyone could compute the eviction sequence offline.
 * Seeding it from the kernel RNG is what makes the sequence unavailable to
 * anyone who cannot already read kernel memory.
 *
 * JCORE_TSB_VSEED is write-only in hardware, and the seed is never read
 * back here -- it leaves this function and becomes unobservable.
 *
 * This lives here, not in arch/sh/mm/tlb-jcore.c, on purpose: the jcore-cpu
 * bare-metal cosim harnesses (sim/linux_sim.sh) link tlb-jcore.o directly
 * and would then need a get_random_u32() stub. probe.o is not in that link.
 *
 * An unseeded selector is not a correctness problem -- hardware self-heals
 * its all-zero LFSR state to a fixed constant, so both ways are still used,
 * just predictably until this initcall runs.
 */
static int __init jcore_tsb_victim_seed_init(void)
{
	__raw_writel(get_random_u32(), (void __iomem *)JCORE_TSB_VSEED);
	return 0;
}
early_initcall(jcore_tsb_victim_seed_init);

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
