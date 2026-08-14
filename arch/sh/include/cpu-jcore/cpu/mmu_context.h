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
/* Defined in arch/sh/kernel/cpu/jcore/probe.c; consumed by tlb-jcore.c to
 * zero the boot TSB on ASID generation wrap (security-review S-I3). */
extern char jcore_boot_tsb[JCORE_BOOT_TSB_BYTES];
#endif /* __ASSEMBLY__ */

#endif /* __ASM_CPU_JCORE_MMU_CONTEXT_H */
