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
