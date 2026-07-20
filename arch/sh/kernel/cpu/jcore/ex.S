/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/sh/kernel/cpu/jcore/ex.S
 *
 * J-Core J4 fixed TLB-miss vectors + software TLB-miss handler.
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
 * All three run the exact same hot path: by the time any fires, hardware
 * has already loaded PTEH/ASIDR/TSBPTR for the faulting access, so there
 * is nothing left to distinguish. Rather than have each vector *jump* to
 * a common routine -- which would cost a literal load + taken branch +
 * delay slot on every single TLB miss -- the fast path is INLINED at
 * each fixed vector (the JCORE_TLB_FASTPATH macro). Hardware lands
 * directly on the first instruction of the fast path; a TSB hit runs
 * straight to LDTLB.RN with no taken branch. The machine code is
 * deliberately duplicated across the three vectors (the source is not,
 * thanks to the macro) because these vectors are as hot as the MMU gets.
 *
 * The rarer paths -- a TSB miss (walk the page table in C) and a real
 * fault (hand off to do_page_fault) -- are shared, co-located below the
 * vectors so the fast path's BF-to-slow branches stay in range and so
 * everything the linker must keep contiguous lives in one object.
 *
 * On entry to any vector (docs/mmu/linux-spec.md §4.1, hardware-spec.md):
 *   SR.MD=1, SR.BL=1, SR.RB=1   bank-1 GPRs selected; r0-r7 scratch, the
 *                               interrupted context's r0-r7 live in bank-0.
 *   PTEH   = faulting VPN (low PAGE_SHIFT bits clear)
 *   ASIDR  = current ASID_TAG
 *   TSBPTR = precomputed TSB slot address for (VPN, ASID_TAG)
 *
 * jcore_vbr_base is laid out exactly like sh2/ex.S's vbr_base (a
 * 256-entry, EXPEVT-style pointer table used for TRAPA/general
 * exceptions -- reusing sh2/entry.S's exception_handler dispatch
 * unchanged) so J4's generic non-MMU exception handling keeps working
 * byte-for-byte the same as plain SH-2. That table is exactly
 * 256 * 4 = 1024 = 0x400 bytes, so jcore_vec_imiss below begins at
 * precisely jcore_vbr_base + 0x400 with no padding -- nothing may be
 * emitted between the .rept/.endr and jcore_vec_imiss. Each fast-path
 * copy is < 0x20 bytes and padded to a fixed 0x20 stride with .balign so
 * dload/dstore land exactly on +0x420/+0x440.
 */
#include <linux/linkage.h>
#include <cpu/mmu_context.h>

	.text

/*
 * The shared TLB-miss fast path, inlined verbatim at each fixed vector.
 * Two separate CMP/EQ (VPN vs PTEH, then ASID_TAG vs ASIDR) avoid any
 * VPN/ASID-bit-overlap ambiguity. On a TSB hit this runs to LDTLB.RN
 * with NO taken branch. On a miss it falls to jcore_tlb_miss_slow. Each
 * TSB slot is 3 words: tag_hi (VPN), tag_lo (ASID_TAG), data (PTEL).
 * Uses only bank-1 scratch (r0-r3) and no literal pool, so one copy is
 * 12 insns = 24 bytes and fits inside the 0x20 vector stride.
 */
	.macro	JCORE_TLB_FASTPATH
	stc	tsbptr, r0
	mov.l	@r0+, r1		/* r1 = tag_hi (expected VPN); r0 += 4 */
	cmp/eq	pteh, r1		/* VPN match? -- fused CSR-vs-Rn compare
					 * (reads PTEH onto ybus + sets T in one
					 * insn; replaces stc pteh,r2 + cmp/eq). */
	bf	jcore_tlb_miss_slow
	mov.l	@r0+, r1		/* r1 = tag_lo (expected ASID_TAG); r0 += 4 */
	mov.l	@r0, r3			/* r3 = TTE data (PTEL) -- loaded here so its
					 * load-use latency hides behind the ASID
					 * compare/bf and 'ldc r3,ptel' never stalls.
					 * Harmless on a miss (r3 is scratch). */
	cmp/eq	asidr, r1		/* ASID_TAG match? -- fused compare */
	bf	jcore_tlb_miss_slow
	ldc	r3, ptel		/* r3 ready (loaded 2 insns ago; no stall) */
	ldtlb.rn			/* install TLB entry + return (no delay slot) */
	.endm

!
! J4 generic exception dispatch (byte-identical to sh2/ex.S's vbr_base,
! duplicated here -- not shared -- because it must sit immediately
! before the fixed TLB-miss vectors within this one object file so the
! linker cannot interpose other code between the table and the vectors).
!
jcore_exception_entry:
no	=	0
	.rept	256
	mov.l	r1,@-sp
	bra	jcore_exception_trampoline
	mov	#no,r1
no	=	no + 1
	.endr
jcore_exception_trampoline:
	mov.l	r0,@-sp
	mov.l	$jcore_exception_handler,r0
	extu.b	r1,r1
	jmp	@r0
	  extu.w	r1,r1

	.align	2
$jcore_exception_entry:
	.long	jcore_exception_entry
$jcore_exception_handler:
	.long	exception_handler
!
! J4 Exception Vector Base -- VBR is pointed here (not at sh2/ex.S's
! vbr_base) for CONFIG_CPU_JCORE; see per_cpu_trap_init() (traps_32.c).
! Must itself be 0x20-aligned so the absolute .balign 0x20 padding
! between the fixed TLB-miss stubs below lands on +0x400/+0x420/+0x440
! relative to this symbol (not just some arbitrary 0x20 boundary).
!
	.balign	0x20
ENTRY(jcore_vbr_base)
vector	=	0
	.rept	256
	.long	jcore_exception_entry + vector * 6
vector	=	vector + 1
	.endr
! --- exactly 0x400 bytes above this point; do not insert an .align ---
ENTRY(jcore_vec_imiss)			/* VBR + 0x400: I-side miss/protection */
ENTRY(jcore_tlb_miss)			/* alias: SP2 bare-metal harness entry */
	JCORE_TLB_FASTPATH
	.balign	0x20

ENTRY(jcore_vec_dload)			/* VBR + 0x420: D-side load miss */
	JCORE_TLB_FASTPATH
	.balign	0x20

ENTRY(jcore_vec_dstore)			/* VBR + 0x440: D-side store miss */
	JCORE_TLB_FASTPATH

!
! Shared slow path (one copy). Reached by BF from any of the three
! inlined fast paths above when the TSB probe misses. Walks the real
! page table in C; on success installs the entry, on failure hands off
! to the pt_regs-building real-fault tail in entry.S.
!
	.align	4
jcore_tlb_miss_slow:
	! Bank-1 r4-r7 are pure scratch to the miss handler (the interrupted
	! context's r4-r7 live in bank-0, untouched), but PR is
	! architecturally shared (not banked) and must be preserved across
	! the jsr below.
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

	.align	4
ENTRY(jcore_tlb_vectors)
	.long	jcore_vec_imiss		/* VBR + 0x400 */
	.long	jcore_vec_dload		/* VBR + 0x420 */
	.long	jcore_vec_dstore	/* VBR + 0x440 */
