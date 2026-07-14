/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/sh/kernel/cpu/jcore/tlbmiss.S
 *
 * J-Core J4 software TLB-miss hot path.
 *
 * Entered directly at VBR + 0x400 (I-side miss/protection), VBR + 0x420
 * (D-side load miss), or VBR + 0x440 (D-side store miss) -- see ex.S.
 * All three share this single routine: by the time hardware jumps here
 * it has already loaded, for the faulting access (docs/mmu/linux-spec.md
 * §4.1, hardware-spec.md §2/§7):
 *
 *   SR.MD=1, SR.BL=1, SR.RB=1   bank-1 GPRs selected; r0-r7 are scratch,
 *                               the interrupted context's r0-r7 live
 *                               untouched in bank-0.
 *   PTEH    = faulting VPN (low PAGE_SHIFT bits clear)
 *   ASIDR   = current ASID_TAG (unchanged since the last context switch)
 *   TSBPTR  = precomputed TSB slot address for (VPN, ASID_TAG)
 *
 * Each TSB slot is 3 words: tag_hi (expected VPN), tag_lo (expected
 * ASID_TAG), data (PTEL image). Two separate CMP/EQ (VPN vs PTEH, then
 * ASID_TAG vs ASIDR) avoid any VPN/ASID-bit-overlap ambiguity.
 *
 * This file makes no assumption about a surrounding pt_regs frame on
 * the fast path or the walker call -- jcore_tlb_miss is a standalone,
 * labeled routine SP2's bare-metal harness can jsr/jmp into directly
 * against a stubbed TSB + PTEH/ASIDR/TSBPTR/MMU_TTB/MMU_TEA. Only the
 * rare real-fault tail (jcore_tlb_real_fault, entry.S) needs a full
 * pt_regs, and that is not part of the SP2 harness surface.
 */
#include <linux/linkage.h>
#include <cpu/mmu_context.h>

	.text
	.align	4
ENTRY(jcore_tlb_miss)
	stc	tsbptr, r0
	mov.l	@r0+, r1		! r1 = tag_hi (expected VPN); r0 += 4
	stc	pteh, r2		! r2 = faulting VPN
	cmp/eq	r1, r2			! VPN match?
	bf	jcore_tlb_miss_slow
	mov.l	@r0+, r1		! r1 = tag_lo (expected ASID_TAG); r0 += 4
	mov.l	@r0, r3			! r3 = TTE data (PTEL image) -- loaded here,
					! not after the check, so its load-use
					! latency hides behind the ASID stc/cmp/bf
					! below and 'ldc r3,ptel' never stalls on the
					! hit path. Harmless on a miss (r3 is bank-1
					! scratch; the slow path re-derives everything).
	stc	asidr, r2		! r2 = current ASID_TAG
	cmp/eq	r1, r2			! ASID_TAG match?
	bf	jcore_tlb_miss_slow
	ldc	r3, ptel		! r3 ready (loaded 3 insns ago; no stall)
	ldtlb.rn			! install TLB entry + return (no delay slot)
	! NOT REACHED: LDTLB.RN returns directly to the faulting instruction.

	.align	4
jcore_tlb_miss_slow:
	! TSB probe missed. Walk the real page table in C. Bank-1 r4-r7/PR
	! are pure scratch to the miss handler (the interrupted context's
	! r4-r7 live in bank-0, untouched), but PR is architecturally
	! shared (not banked) and must be preserved across the jsr below.
	mov.l	r4, @-r15
	mov.l	r5, @-r15
	mov.l	r6, @-r15
	mov.l	r7, @-r15
	sts.l	pr, @-r15

	mov.l	1f, r4
	mov.l	@r4, r4			! r4 = current_pgd (MMU_TTB scratch)  (arg0)
	mov.l	2f, r5
	mov.l	@r5, r5			! r5 = faulting address (MMU_TEA)     (arg1)
	stc	asidr, r6		! r6 = current ASID_TAG               (arg2)

	mov.l	3f, r0
	jsr	@r0			! __jcore_tlb_walk(pgd, addr, asid_tag)
	 nop

	! r0 = 0 on success (PTEL already loaded by the walker); nonzero
	! means no valid mapping was found -- a real fault.
	tst	r0, r0
	bf	jcore_tlb_real_fault

	lds.l	@r15+, pr
	mov.l	@r15+, r7
	mov.l	@r15+, r6
	mov.l	@r15+, r5
	mov.l	@r15+, r4
	ldtlb.rn			! install TLB entry + return (no delay slot)

jcore_tlb_real_fault:
	! Restore PR (the interrupted context's real return-context state;
	! r4-r7 are scratch and are popped only to keep the stack balanced)
	! then hand off to the pt_regs-building fault tail in entry.S.
	lds.l	@r15+, pr
	mov.l	@r15+, r7
	mov.l	@r15+, r6
	mov.l	@r15+, r5
	mov.l	@r15+, r4
	mov.l	4f, r0
	jmp	@r0
	 nop

	.align	4
1:	.long	MMU_TTB
2:	.long	MMU_TEA
3:	.long	__jcore_tlb_walk
4:	.long	jcore_tlb_fault_entry
