/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/sh/kernel/cpu/jcore/ex.S
 *
 * J-Core J4 fixed TLB-miss vectors + software TLB-miss handler.
 *
 * Like SH-3/SH-4, J4 delivers the three TLB MISS causes (IMISS, DMISS_R,
 * DMISS_W) to one hardware-fixed vector, VBR + 0x400. PROTECTION faults
 * (IPROT, DPROT_R, DPROT_W) go to VBR + 0x100 instead -- which is what SH-4
 * itself does (SH7750 hardware manual Rev 2.0 02/99: misses H'400, protection
 * H'100), and what J4 now does after the vectors were split.
 *
 * That split is why this file no longer tests EXPEVT on the hot path. While
 * all six causes shared 0x400, a protection fault against a VPN/ASID with a
 * still-valid TSB entry would TSB-hit here, and reinstalling that entry
 * livelocks -- do_page_fault() never runs to repair the pte. The fast path had
 * to check the cause on EVERY hit to avoid it. With protection vectored
 * elsewhere the hazard is excluded by construction, and the check is gone.
 *
 * Rather than have the vector *jump* to a common routine -- which would
 * cost a literal load + taken branch + delay slot on every single TLB
 * miss -- the fast path is INLINED at the vector (the JCORE_TLB_FASTPATH
 * macro). Hardware lands directly on the first instruction; a TSB hit runs
 * straight to LDTLB.RN with no taken branch.
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
 * 256 * 4 = 1024 = 0x400 bytes, so jcore_vec_tlb below begins at
 * precisely jcore_vbr_base + 0x400 with no padding -- nothing may be
 * emitted between the .rept/.endr and jcore_vec_tlb.
 */
#include <linux/linkage.h>
#include <cpu/mmu_context.h>

	.text

/*
 * The TLB-miss fast path, inlined verbatim at the one fixed vector
 * (jcore_vec_tlb, VBR+0x400 -- see the M3 single-vector decision above).
 * Two separate CMP/EQ (VPN vs PTEH, then ASID_TAG vs ASIDR) avoid any
 * VPN/ASID-bit-overlap ambiguity. On a TSB hit that is a genuine MISS
 * (EXPEVT < 0x0A0) this runs to LDTLB.RN with NO taken branch -- but
 * every TSB hit, miss or not, now pays 4 extra straight-line insns
 * (STC EXPEVT, ADD, CMP/PL, a not-taken BT) to make that determination;
 * see the EXPEVT paragraph below. Each TSB slot is 3 words: tag_hi
 * (VPN), tag_lo (ASID_TAG), data (PTEL). Uses only bank-1 scratch
 * (r0-r3) and no literal pool, so the whole macro is 13 insns = 26
 * bytes -- 6 bytes of headroom inside the 0x20 vector stride, enforced
 * at build time by the '.org jcore_vec_tlb + 0x20' assertion right
 * after its expansion below (recount before trusting any prose
 * description of this size, including this one -- it has been wrong
 * twice already).
 *
 * Uses only bank-1 scratch (r0, r1, r3) and no literal pool, so the whole
 * macro is 9 insns = 18 bytes -- well inside the 0x20 vector budget, which the
 * '.org jcore_vec_tlb + 0x20' assertion after its expansion enforces (recount
 * before trusting any prose description of this size, including this one -- it
 * has been wrong twice already).
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
	ldtlb.rn r3			/* PTEL<-r3 (loaded 4 insns ago) + install + return
					 * in one insn -- fused LDC+LDTLB.RN. Slow path
					 * still uses plain ldtlb.rn (walker sets PTEL CSR). */
	.endm

!
! J4 Exception Vector Page -- VBR points here (per_cpu_trap_init, traps_32.c).
!
! J4 uses SH-4-style FIXED vectors: hardware branches to VBR+offset as CODE.
! This used to be a 256-entry POINTER table (sh2/ex.S's vbr_base layout) whose
! entries fed sh2/entry.S's exception_handler -- correct for SH-2's
! table-driven vectoring and stack-pushed PC/SR, but J4 has neither. VBR+0x100
! and VBR+0x600 were therefore *executing .long values as instructions*; only
! VBR+0x400 was ever right, because it is the only one CI exercised. The table
! is gone and each vector now holds real code.
!
!   +0x000  not a J4 vector -- trap loudly rather than run into 0x100
!   +0x100  Error / Slot Illegal / General Illegal / TRAPA, and (since the
!           miss/protection split) IPROT + DPROT_R/W. EXPEVT discriminates.
!   +0x400  all three TLB MISS causes; fast path inlined, not jumped to
!   +0x600  interrupts; INTEVT carries the source
!
! Guard: mmulinuxexc drives +0x100 and +0x600 through these; mmulinux covers
! +0x400.
!
	.balign	0x20
ENTRY(jcore_vbr_base)
	! VBR+0x000 is not a J4 vector. Landing here means VBR is wrong or a
	! vector offset is; spin rather than fall through into +0x100 and
	! mis-handle it as a general exception.
0:	bra	0b
	 nop

	.org	jcore_vbr_base + 0x100
ENTRY(jcore_vec_general)
	mov.l	9f, r0
	jmp	@r0
	 nop
	.align	4
9:	.long	jcore_general_entry

	.org	jcore_vbr_base + 0x400
ENTRY(jcore_vec_tlb)			/* VBR + 0x400: ALL TLB faults (I/D, miss/prot) */
ENTRY(jcore_tlb_miss)			/* alias: SP2 bare-metal harness entry */
	JCORE_TLB_FASTPATH
	! Build-time size assert: the fast path is budgeted 0x20 bytes and
	! this pads it out to that 32-byte boundary. There is no further
	! fixed vector after jcore_vec_tlb -- the budget is a self-imposed
	! ceiling on the hot path, not a hardware stride -- but an edit that
	! grows the macro past it must still fail the build here instead of
	! silently pushing the code that follows around; a prose comment
	! claiming a byte count is not a substitute for the assembler
	! checking it. A plain '.if (. -
	! jcore_vec_tlb) > 0x20' does NOT work with this assembler: BF/BT
	! inside the macro are relaxable (variable-length until link-time
	! relaxation), so '.' minus a backward label spanning them is a
	! non-constant expression at .if-evaluation time and errors out
	! unconditionally, pass or fail. '.org' has no such restriction --
	! it errors iff the location counter has already moved past the
	! target when reached, which is exactly the assert we want.
	! EXACT-SIZE assert -- this is the one that catches BF relaxation.
	! The fast path is 9 instructions = 0x12 bytes. If `bf jcore_tlb_miss_slow`
	! ever falls outside BF's +/-256-byte reach, gas silently rewrites each one
	! as `bt .+2; bra slow; nop`, which INVERTS the branch sense: a TSB hit --
	! the case this macro exists to make branchless -- starts taking two taken
	! branches. The 0x20 budget below CANNOT catch that: relaxed, the macro is
	! 13 insns = 0x1A bytes, still under 0x20. It sailed through once already.
	! .org errors iff the location counter is already past the target, so this
	! fails the build the moment the macro grows by even one instruction.
	.org	jcore_vec_tlb + 0x12

	! Vector-stride budget (self-imposed, not a hardware stride) and the pad
	! that places the slow path at a known offset.
	.org	jcore_vec_tlb + 0x20

	! The slow path MUST live in the 0x420..0x600 gap, i.e. between the TLB
	! vector and the interrupt vector -- NOT after 0x600. The fast path
	! reaches it with BF, whose range is +/-256 bytes; push it past the
	! interrupt vector and the assembler silently relaxes each `bf slow`
	! into `bt .+2; bra slow; nop`. That inverts the branch sense, so a TSB
	! HIT -- the case the whole macro is shaped around -- starts taking two
	! taken branches instead of falling through. It costs cycles and nothing
	! warns you; it was caught by disassembling the vector page, not by any
	! test. The gap is 0x1E0 bytes and the slow path needs ~0x100, so it
	! fits comfortably. Verify with objdump after touching this layout.

!
! Shared slow path (one copy). Reached by BF from the inlined fast path
! above when the TSB probe misses. Walks the real
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
	.org	jcore_vbr_base + 0x600
ENTRY(jcore_vec_irq)
	mov.l	8f, r0
	jmp	@r0
	 nop
	.align	4
8:	.long	jcore_irq_entry

ENTRY(jcore_tlb_vectors)
	.long	jcore_vec_tlb		/* VBR + 0x400: all TLB faults (I/D, miss/prot) */
