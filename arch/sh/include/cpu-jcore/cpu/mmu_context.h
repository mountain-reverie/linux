/* SPDX-License-Identifier: GPL-2.0 */
/*
 * J-Core J4 MMU MMIO register layout.
 *
 * PTEH/PTEL/PTEU and ASIDR remain LDC/STC-only control registers for
 * WRITES (hardware-spec.md §2.1, §2.1a) -- the LDC forms are still how
 * software installs a translation, and the design deliberately keeps them
 * (D7: preserves the hypervisor's one-trap guest-refill path). TSB-walker
 * Phase 3 (jcore-cpu task-1) added read-only P4 MMIO aliases for
 * PTEH/PTEL/ASIDR alongside the pre-existing TSBPTR alias below, so kernel
 * code that only needs to read back the current value no longer has to
 * use STC.
 */
#ifndef __ASM_CPU_JCORE_MMU_CONTEXT_H
#define __ASM_CPU_JCORE_MMU_CONTEXT_H

#define JCORE_PTEH	0xFF000000	/* PTEH (read-only P4 alias of STC PTEH) */
#define JCORE_PTEL	0xFF000004	/* PTEL (read-only P4 alias of STC PTEL) */
#define MMU_TTB		0xFF000008	/* Translation table base (SW scratch, HW ignores) */
#define MMU_TEA		0xFF00000C	/* TLB Exception Address */
#define MMU_FSR		0xFF00002C	/* TLB Fault Status (direction/cause; read-only) */

#define MMUCR		0xFF000010	/* MMU Control Register */

#define TSBBR		0xFF000014	/* TSB Base Register (boot config, MMIO only) */
#define TSBCFG		0xFF000018	/* TSB Config Register (boot config, MMIO only) */
#define TSBPTR		0xFF00001C	/* TSB Pointer (read-only mirror of STC TSBPTR) */
#define JCORE_ASIDR	0xFF000038	/* ASIDR (read-only P4 alias of STC ASIDR) */

/*
 * TSB slot-address helper (docs/soc/p4-mmio-map.md §3.2, offset 0x048,
 * "TSBSLOT"). Write a VA, then read the same address back to get the TSB
 * slot address for that VA -- exactly what the hardware TSB walker
 * (core/tlb_walk.vhd) and TSBPTR-on-fault both use, computed by the one
 * RTL function that owns that computation (core/datapath_pkg.vhd
 * tsb_ptr()). Replaces the C-side jcore_tsb_slot_offset() mirror that
 * used to reimplement tsb_ptr()'s hash bit-for-bit in software (removed
 * with this register; see arch/sh/mm/tlb-jcore.c __update_tlb()).
 *
 * P4 MMIO offset 0x048 (docs/soc/p4-mmio-map.md), decoded in
 * core/datapath.vhm alongside TSBBR/TSBCFG/TSBPTR as P4_TSBSLOT.
 */
#define JCORE_TSB_SLOT	0xFF000048

/*
 * TSB victim selector (jcore-cpu Phase-2 Task 4, core/datapath.vhm
 * P4_TSBVSEED / P4_TSBVICT).
 *
 * JCORE_TSB_VSEED is WRITE-ONLY. It seeds the hardware LFSR that nominates
 * which way of a 2-way TSB set to replace when neither way's tag matches.
 * The seed comes from the OS precisely because it must NOT be public: this
 * is an open-source core, so the polynomial and any constant seed compiled
 * into the RTL are readable by anyone, and a victim sequence an attacker can
 * replay offline is worth no more than a fixed choice. Reading the register
 * back returns a hard zero -- if software could recover the seed, so could
 * an attacker.
 *
 * JCORE_TSB_VICTIM is READ-ONLY and exposes ONLY the 1-bit way nomination.
 * Each read advances the LFSR one step, so nothing beyond the bits actually
 * consumed is observable. Neither the seed nor the LFSR state is readable.
 */
#define JCORE_TSB_VSEED		0xFF00004C
#define JCORE_TSB_VICTIM	0xFF000050

/* Ways per TSB set (core/tlb_walk.vhd `tsb_ways`). */
#define JCORE_TSB_WAYS		2
/* Bytes per TSB entry; a set is JCORE_TSB_WAYS * JCORE_TSB_ENTRY_BYTES. */
#define JCORE_TSB_ENTRY_BYTES	16
#define JCORE_TSB_SET_BYTES	(JCORE_TSB_WAYS * JCORE_TSB_ENTRY_BYTES)

/*
 * TSB tags AND the TSB set index are BOTH at the architecture's finest page
 * granularity (4 KB), independent of PAGE_SIZE and of the size of the page
 * the entry describes.  Do NOT substitute PAGE_MASK.
 * docs/mmu/pagemask-walker-contract.md C1/C2; hardware-spec.md §7.0a;
 * linux-spec.md §4.3a.
 *
 * WHY IT CANNOT BE PAGE_MASK. The hardware walker compares tag_hi against the
 * RAW faulting VA with an exact 32-bit equality, for every page size, so a
 * PAGE_MASK-aligned tag under PAGE_SHIFT=14 can never match a first touch
 * outside a page's base 4 KB sub-page and the fault repeats forever.  The
 * read-order and index-granularity reasons it must stay that way, and the
 * one-row-per-touched-sub-page cost that follows, are in the contract (C1,
 * C2, C7).  Guards: jcore-cpu sim/tests/mmupmsub4k.S, mmupmsubi.S, mmupmmix.S.
 */
#define JCORE_TSB_TAG_SHIFT	12
#define JCORE_TSB_TAG_MASK	(~((1UL << JCORE_TSB_TAG_SHIFT) - 1))

/* MMUCR bit layout (hardware-spec.md §2.3) */
#define MMUCR_AT	(1 << 0)	/* Address Translation enable */
#define MMUCR_TI	(1 << 2)	/* TLB flush strobe (write-1) */

#define MMU_CONTROL_INIT	(MMUCR_AT | MMUCR_TI)

/* J4's TLB is a 32-entry, fully-associative, software-loaded array
 * (docs/architecture/tlb.md). generic sh mm code (tlbflush_32.c) uses
 * this only as a coarse flush-all-vs-per-page threshold, not a
 * hardware-precise figure. */
#define MMU_NTLB_ENTRIES	32

#define TRA	0xff000020
#define EXPEVT	0xff000024
#define INTEVT	0xff000028

/*
 * Boot TSB sizing (hardware-spec.md §2.6: TSB_SIZE_LOG valid range is
 * 6-14). SP1 boot needs only a small early TSB to get translation on;
 * SP2/SP3 per-CPU TSBs (linux-spec.md §6.2) are sized/allocated
 * separately at CPU-up time. 256 SETS (log2=8) is 8192 bytes.
 *
 * NOTE TSB_SIZE_LOG counts SETS, not entries: hardware's tsb_ptr() returns
 * a 32-byte-aligned SET address (base | idx << 5), so the region is
 * JCORE_TSB_SET_BYTES << TSB_SIZE_LOG. Sizing this as 16 << SIZE_LOG would
 * allocate exactly half the region hardware indexes and let the top half of
 * the hash range walk off the end of the object.
 */
#define JCORE_BOOT_TSB_SIZE_LOG	8
#define JCORE_BOOT_TSB_BYTES	(JCORE_TSB_SET_BYTES << JCORE_BOOT_TSB_SIZE_LOG)

#ifndef __ASSEMBLY__
#include <linux/threads.h>

/*
 * Per-CPU TSBs, defined in arch/sh/kernel/cpu/jcore/probe.c. Row `cpu` is
 * the TSB that CPU's TSBBR points at; see the comment there for why sharing
 * one row across CPUs is a cross-address-space bug rather than a slowdown.
 * Consumed by tlb-jcore.c's local_flush_tlb_all() and by enable_mmu().
 */
extern char jcore_boot_tsb[NR_CPUS][JCORE_BOOT_TSB_BYTES];

/*
 * arch/sh/kernel/cpu/jcore/mmu_enable.S. Programs TSBBR/TSBCFG from @tsb_base,
 * zeroes ASIDR/PTEH, then enables translation and flushes the TLB in one
 * MMUCR write. @tsb_base is a P1 KERNEL VIRTUAL address, aligned to its own
 * size -- never a physical address; see the contract note in that file.
 */
void jcore_mmu_enable(unsigned long tsb_base);
#endif /* __ASSEMBLY__ */

#endif /* __ASM_CPU_JCORE_MMU_CONTEXT_H */
