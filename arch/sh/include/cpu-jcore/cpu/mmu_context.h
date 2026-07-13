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

#define MMUCR		0xFF000010	/* MMU Control Register */

#define TSBBR		0xFF000014	/* TSB Base Register (boot config, MMIO only) */
#define TSBCFG		0xFF000018	/* TSB Config Register (boot config, MMIO only) */
#define TSBPTR		0xFF00001C	/* TSB Pointer (read-only mirror of STC TSBPTR) */

/* MMUCR bit layout (hardware-spec.md §2.3) */
#define MMUCR_AT	(1 << 0)	/* Address Translation enable */
#define MMUCR_TI	(1 << 2)	/* TLB flush strobe (write-1) */

#define MMU_CONTROL_INIT	(MMUCR_AT | MMUCR_TI)

#define TRA	0xff000020
#define EXPEVT	0xff000024
#define INTEVT	0xff000028

#endif /* __ASM_CPU_JCORE_MMU_CONTEXT_H */
