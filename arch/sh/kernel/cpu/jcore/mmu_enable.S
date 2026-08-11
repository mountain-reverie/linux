/* SPDX-License-Identifier: GPL-2.0
 *
 * arch/sh/kernel/cpu/jcore/mmu_enable.S
 *
 * J-Core J4 boot MMU-enable sequence, factored out of head_32.S so that
 * BOTH the real boot path (head_32.S, P1, MMU off) and the SP3 cosim
 * harness link and execute the exact same instruction sequence -- zero
 * drift between what boots real hardware and what the RTL testbench
 * exercises.
 *
 * INPUT CONTRACT:
 *   r4 = boot-TSB base address, as a P1 KERNEL VIRTUAL address (plain
 *        `jcore_boot_tsb` in the real kernel, whose link address is
 *        already in P1). NOT a physical address: the hardware walker
 *        applies the P1 fold to whatever it derives from TSBBR, and
 *        TSBPTR/JCORE_TSB_SLOT hand TSBBR's own high bits back to
 *        software to dereference. See the long note at the call site in
 *        arch/sh/kernel/head_32.S. The address MUST already be aligned to
 *        its own size so that the low JCORE_BOOT_TSB_SIZE_LOG bits are
 *        zero (TSB_SIZE_LOG is OR'd into those bits below). This routine
 *        does NOT reference jcore_boot_tsb itself -- the caller supplies
 *        the base, so a cosim harness can pass in its own TSB base
 *        without linking any kernel symbols.
 *
 * Scratch: r0, r1. All other registers preserved.
 *
 * Sequence (linux-spec.md §11, hardware-spec.md §2.6/§2.7/§2.3):
 *   1. Program TSBBR (TSB base [31:N+5] | TSB_SIZE_LOG [3:0]) to point
 *      at the boot TSB, whose PA is passed in r4. TSB_SIZE_LOG counts
 *      SETS of JCORE_TSB_SET_BYTES (32), not 16-byte entries -- see
 *      JCORE_BOOT_TSB_BYTES in cpu/mmu_context.h.
 *   2. Program TSBCFG (HASH_MODE=1, HASH_SHIFT=TSB_SIZE_LOG) so
 *      hardware's XOR-fold hash is well spread from the very first
 *      TLB miss.
 *   3. ASIDR = 0 (kernel ASID_TAG), PTEH = 0 (no faulting VPN yet).
 *   4. MMUCR = AT=1 | TI=1 in one write: enables translation and
 *      flushes any stale TLB content left over from a warm reset, in
 *      the same atomic MMIO write (mirrors enable_mmu(),
 *      asm/mmu_context.h). MMUCR is written LAST -- order matters.
 *
 * Returns via rts. Callers at P1 with MMU off can bsr/jsr this directly.
 */
#include <linux/linkage.h>
#include <cpu/mmu_context.h>

#ifdef CONFIG_CPU_JCORE

	.text
	.balign 4
ENTRY(jcore_mmu_enable)
	mov	r4, r0
	or	#JCORE_BOOT_TSB_SIZE_LOG, r0
	mov.l	.LJCORE_TSBBR, r1
	mov.l	r0, @r1

	mov	#JCORE_BOOT_TSB_SIZE_LOG, r0
	shll2	r0
	or	#1, r0			! HASH_MODE=1 (XOR fold)
	mov.l	.LJCORE_TSBCFG, r1
	mov.l	r0, @r1

	mov	#0, r0
	ldc	r0, asidr		! ASIDR = 0 (kernel ASID_TAG)
	ldc	r0, pteh		! PTEH = 0 (no faulting VPN yet)

	mov.l	.LJCORE_MMUCR_INIT, r0
	mov.l	.LJCORE_MMUCR, r1
	rts
	 mov.l	r0, @r1			! AT=1 | TI=1 (enable + flush, atomic)

	.balign 4
.LJCORE_TSBBR:		.long	TSBBR
.LJCORE_TSBCFG:		.long	TSBCFG
.LJCORE_MMUCR:		.long	MMUCR
.LJCORE_MMUCR_INIT:	.long	(MMUCR_AT | MMUCR_TI)

#endif /* CONFIG_CPU_JCORE */
