/* SPDX-License-Identifier: GPL-2.0 */
/*
 * J-Core J4 hardware performance counter (PMU) MMIO layout.
 *
 * Spec: jcore-cpu docs/pmu/perf-counters.md; RTL: jcore-cpu core/perf.vhd,
 * decoded in core/datapath.vhm.  The block has a 4 KB per-CPU P4 page of its
 * own and, unlike the MMU page at 0xFF000000, all twelve offset bits are
 * decoded -- 0xFF001100 is not a second PMCR and everything from 0x040 up
 * reads as a hard zero.
 *
 * Every register here is privileged (SR.MD = 1) and has no read side effect,
 * so a refused access leaves no trace and reads may be repeated freely.
 */
#ifndef __ASM_CPU_JCORE_PERF_EVENT_H
#define __ASM_CPU_JCORE_PERF_EVENT_H

#define JCORE_PMU_BASE		0xFF001000

/* bit 0 = EN.  Resets SET: the counters free-run out of reset. */
#define JCORE_PMCR		(JCORE_PMU_BASE + 0x000)
#define JCORE_PMCR_EN		0x00000001

/* Sticky per-counter wrap flags, bit n = counter n.  WRITE-1-TO-CLEAR. */
#define JCORE_PMOVF		(JCORE_PMU_BASE + 0x004)

/*
 * [31:16] magic, [15:8] counter width in bits, [7:0] implemented-counter
 * bitmask.  The magic is the only reliable presence test: see the long
 * comment in perf_event.c: on a bitstream predating the PMU, 0xFF001008 is
 * NOT a hard zero, it aliases the MMU page's TTB.
 */
#define JCORE_PMIDR		(JCORE_PMU_BASE + 0x008)
#define JCORE_PMIDR_MAGIC	0x4A50		/* "JP" */
#define JCORE_PMIDR_MAGIC_SHIFT	16
#define JCORE_PMIDR_WIDTH_SHIFT	8
#define JCORE_PMIDR_WIDTH_MASK	0xff
#define JCORE_PMIDR_CNTS_MASK	0xff

/* 0x00C-0x01C is reserved for a future PMSEL0..3 (programmable event select). */

/*
 * The eight counters are one contiguous, naturally indexed block: address
 * bits [4:2] are the counter number AND the PMOVF bit number, so there is one
 * numbering to get wrong rather than three.
 */
#define JCORE_PMCNT(n)		(JCORE_PMU_BASE + 0x020 + ((n) * 4))

#define JCORE_PMU_NR_COUNTERS	8

#define JCORE_PMU_CYC		0	/* cycles (a level, not a pulse) */
#define JCORE_PMU_INS		1	/* instruction DISPATCHES, not retires */
#define JCORE_PMU_IFR		2	/* completed instruction-fetch bus reads */
#define JCORE_PMU_IFW		3	/* cycles with a fetch outstanding, unacked */
#define JCORE_PMU_DAR		4	/* completed data-access bus reads/writes */
#define JCORE_PMU_DAW		5	/* cycles with a data access outstanding, unacked */
#define JCORE_PMU_WLK		6	/* TSB walks armed (I-side AND D-side) */
#define JCORE_PMU_WHT		7	/* TSB walks that found a usable PTE */

#endif /* __ASM_CPU_JCORE_PERF_EVENT_H */
