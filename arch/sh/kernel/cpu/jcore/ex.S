/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/sh/kernel/cpu/jcore/ex.S
 *
 * J-Core J4 fixed TLB-miss vectors + software TLB-miss handler.
 *
 * Like SH-3/SH-4, J4 delivers ALL TLB faults to a single hardware-fixed
 * vector, VBR + 0x400 (I/D, miss/protection alike; see the
 * mmu-m3-vector-decision project note and docs/mmu/design-spec.md). The
 * six causes are still distinguishable via EXPEVT on the slow path, but
 * the hot path never needs to: by the time it fires, hardware has already
 * loaded PTEH/ASIDR/TSBPTR for the faulting access, so there is nothing
 * left to distinguish. (An earlier SP0 layout gave each access type its
 * own vector -- 0x400/0x420/0x440 -- but the handler is byte-identical for
 * all three, so that only triplicated the hot-path I-cache footprint.)
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
 * The shared TLB-miss fast path, inlined verbatim at each fixed vector.
 * Two separate CMP/EQ (VPN vs PTEH, then ASID_TAG vs ASIDR) avoid any
 * VPN/ASID-bit-overlap ambiguity. On a TSB hit that is a genuine MISS
 * (EXPEVT < 0x0A0) this runs to LDTLB.RN with NO taken branch. Each
 * TSB slot is 3 words: tag_hi (VPN), tag_lo (ASID_TAG), data (PTEL).
 * Uses only bank-1 scratch (r0-r3) and no literal pool, so one copy is
 * 16 insns = 32 bytes and fits exactly inside the 0x20 vector stride.
 *
 * PROTECTION FAULTS: because all six TLB causes share this one vector,
 * a *protection* fault (EXPEVT 0x0A0 IPROT, 0x0C0 DPROT_R/W) against a
 * VPN/ASID that already has a valid TSB slot also TSB-hits here. Simply
 * running LDTLB.RN would reinstall the identical (e.g. read-only)
 * entry and let the faulting instruction retry -- an infinite livelock,
 * since do_page_fault() is never reached to fix the pte up. So on a TSB
 * hit we additionally check EXPEVT and divert protection faults to
 * jcore_tlb_fastpath_prot (which hands off to the real do_page_fault()
 * tail) instead of falling through to LDTLB.RN.
 *
 * EXPEVT is read in-core (STC EXPEVT, encoded .word 0x0052-family --
 * this assembler does not know the mnemonic, matching the convention
 * already used for the other MMU overlay opcodes in this tree, e.g.
 * sim/tests/mmufault.S) rather than via the P4 MMIO alias: this is the
 * hottest path in the kernel and an uncached P4 read here would cost
 * tens of cycles for no reason.
 *
 * The EXPEVT>=0x0A0 test avoids 'mov #imm,Rn' sign-extension pitfalls
 * (mov #0xA0,rN would yield 0xFFFFFFA0, not 0xA0) by first subtracting
 * 128 (0x80, representable directly as an 8-bit signed immediate) and
 * then testing strictly-positive with CMP/PL: EXPEVT-128 > 0 iff
 * EXPEVT > 0x80 iff EXPEVT is IPROT/DPROT_R/DPROT_W (0x0A0 or 0x0C0);
 * EXPEVT-128 <= 0 for IMISS/DMISS_R/DMISS_W (0x040/0x060/0x080). The
 * known EXPEVT lossiness (DPROT_R vs DPROT_W both read back 0x0C0)
 * does not matter here since both are protection faults.
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
	.word	0x0252			/* stc expevt, r2 */
	add	#-128, r2		/* r2 = EXPEVT - 0x80 */
	cmp/pl	r2			/* T = (EXPEVT > 0x80), i.e. a protection
					 * fault (0x0A0/0x0C0) rather than a miss
					 * (0x040/0x060/0x080) */
	bt	jcore_tlb_fastpath_prot
	ldtlb.rn r3			/* PTEL<-r3 (loaded 4 insns ago) + install + return
					 * in one insn -- fused LDC+LDTLB.RN. Slow path
					 * still uses plain ldtlb.rn (walker sets PTEL CSR). */
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
! Must itself be 0x20-aligned so the 0x400-byte generic-exception table
! below places the inlined TLB fast path at exactly +0x400 relative to
! this symbol (not just some arbitrary boundary).
!
	.balign	0x20
ENTRY(jcore_vbr_base)
vector	=	0
	.rept	256
	.long	jcore_exception_entry + vector * 6
vector	=	vector + 1
	.endr
! --- exactly 0x400 bytes above this point; do not insert an .align ---
ENTRY(jcore_vec_tlb)			/* VBR + 0x400: ALL TLB faults (I/D, miss/prot) */
ENTRY(jcore_tlb_miss)			/* alias: SP2 bare-metal harness entry */
	JCORE_TLB_FASTPATH

!
! Reached by BT from the inlined fast path above on a TSB hit whose
! EXPEVT says protection fault, not miss. Unlike jcore_tlb_miss_slow's
! jcore_tlb_real_fault, no r4-r7/PR save/restore is needed here: the
! fast path above never touches PR or bank-1 r4-r7, so register state
! already matches what jcore_tlb_fault_entry expects on entry (see its
! header comment in entry.S). Hands off directly to the pt_regs-building
! real-fault tail; do_page_fault() re-walks the page table and will
! grant write access (setting _PAGE_DIRTY) if this was in fact a
! copy-on-write/dirty-tracking fault rather than a true permission
! violation -- routing through jcore_tlb_miss_slow's walker instead
! would be pointless, since a page-table walk cannot fix up a
! protection fault on an already-present, already-valid pte.
!
	.align	4
jcore_tlb_fastpath_prot:
	mov.l	5f, r0
	jmp	@r0
	 nop
	.align	4
5:	.long	jcore_tlb_fault_entry

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
ENTRY(jcore_tlb_vectors)
	.long	jcore_vec_tlb		/* VBR + 0x400: all TLB faults (I/D, miss/prot) */
