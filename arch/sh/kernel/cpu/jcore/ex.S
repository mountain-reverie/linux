/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/sh/kernel/cpu/jcore/ex.S
 *
 * J-Core J4 fixed TLB-miss vector stubs.
 *
 * Unlike SH-3/SH-4 (single vector + EXPEVT-indexed exception_handling_
 * table), J4 delivers TLB misses to three distinct, hardware-fixed VBR
 * offsets by access type (SP0 vector decision; see the
 * mmu-m3-vector-decision project note and docs/mmu/design-spec.md):
 *
 *   VBR + 0x400   I-side miss / I-side protection violation
 *   VBR + 0x420   D-side load miss
 *   VBR + 0x440   D-side store miss
 *
 * All three share the exact same hot path (jcore_tlb_miss, tlbmiss.S):
 * by the time any of them fires, hardware has already loaded
 * PTEH/ASIDR/TSBPTR for the faulting access, so there is nothing left
 * to distinguish here -- each stub is just a jump.
 *
 * This file defines the vector entry points and a lookup table of their
 * addresses; splicing them into the literal VBR+0x4xx byte offsets is
 * head.S/Task 5's job (VBR points at a RAM page Task 5 populates at
 * boot), which is why these are plain labeled routines rather than a
 * `.org`-positioned table here.
 */
#include <linux/linkage.h>

	.text
	.align	4
ENTRY(jcore_vec_imiss)
	mov.l	1f, r0
	jmp	@r0
	 nop
	.align	4
1:	.long	jcore_tlb_miss

	.align	4
ENTRY(jcore_vec_dload)
	mov.l	1f, r0
	jmp	@r0
	 nop
	.align	4
1:	.long	jcore_tlb_miss

	.align	4
ENTRY(jcore_vec_dstore)
	mov.l	1f, r0
	jmp	@r0
	 nop
	.align	4
1:	.long	jcore_tlb_miss

	.align	4
ENTRY(jcore_tlb_vectors)
	.long	jcore_vec_imiss		/* VBR + 0x400 */
	.long	jcore_vec_dload		/* VBR + 0x420 */
	.long	jcore_vec_dstore	/* VBR + 0x440 */
