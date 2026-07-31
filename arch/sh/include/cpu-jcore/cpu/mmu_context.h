/* SPDX-License-Identifier: GPL-2.0 */
/*
 * J-Core J4 MMU MMIO register layout.
 *
 * PTEH/PTEL/PTEU and ASIDR are LDC/STC-only control registers with no
 * MMIO address (hardware-spec.md §2.1, §2.1a) — they are not defined
 * here; access them via ldc/stc in asm.
 */
#ifndef __ASM_CPU_JCORE_MMU_CONTEXT_H
#define __ASM_CPU_JCORE_MMU_CONTEXT_H

#define MMU_TTB		0xFF000008	/* Translation table base (SW scratch, HW ignores) */
#define MMU_TEA		0xFF00000C	/* TLB Exception Address */
#define MMU_FSR	0xFF000028	/* TLB Fault Status (direction/cause; read-only) */

#define MMUCR		0xFF000010	/* MMU Control Register */

#define TSBBR		0xFF000014	/* TSB Base Register (boot config, MMIO only) */
#define TSBCFG		0xFF000018	/* TSB Config Register (boot config, MMIO only) */
#define TSBPTR		0xFF00001C	/* TSB Pointer (read-only mirror of STC TSBPTR) */

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
 * 6-14, each entry is 16 bytes). SP1 boot needs only a small early TSB
 * to get translation on; SP2/SP3 per-CPU TSBs (linux-spec.md §6.2) are
 * sized/allocated separately at CPU-up time. 256 entries (log2=8) is
 * 4096 bytes -- one 16KB page's worth of PTEs' hash spread, plenty for
 * the boot CPU's early miss traffic before proper per-mm TSBs exist.
 */
#define JCORE_BOOT_TSB_SIZE_LOG	8
#define JCORE_BOOT_TSB_BYTES	(16 << JCORE_BOOT_TSB_SIZE_LOG)

#ifndef __ASSEMBLY__
/* Defined in arch/sh/kernel/cpu/jcore/probe.c; consumed by tlb-jcore.c to
 * zero the boot TSB on ASID generation wrap (security-review S-I3). */
extern char jcore_boot_tsb[JCORE_BOOT_TSB_BYTES];
#endif /* __ASSEMBLY__ */

#endif /* __ASM_CPU_JCORE_MMU_CONTEXT_H */
