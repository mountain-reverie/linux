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
 * TSB-walker Phase 3 (see jcore-cpu docs/mmu, task-2 of the phase-3 SDD)
 * DELETED the inlined software fast path that used to live directly at
 * jcore_vec_tlb. The hardware TSB walker (jcore-cpu core/tlb_walk.vhd,
 * Phase 1/2 of the same program) now resolves every TSB hit as a stall
 * before the CPU ever takes the exception -- so VBR+0x400 is reached
 * ONLY when the walker itself failed to find a matching entry in either
 * way. A software re-probe of way 0 at that point is provably useless:
 * the walker already checked both ways of the set. jcore_vec_tlb is
 * therefore now just the ordinary save-regs / call the C walker /
 * restore-and-return sequence, identical in shape to the other two J4
 * vectors below.
 *
 * That deletion also retires three constraints that existed only to keep
 * the old inlined fast path short enough to reach its slow-path branch:
 *   - the BF +/-256-byte reach requirement on the fast path's branch to
 *     the slow path,
 *   - the '.org jcore_vec_tlb + 0x12' exact-size assertion that caught
 *     BF-relaxation silently inverting the branch sense, and
 *   - the rule that the slow path had to live in the 0x420..0x600 gap
 *     between the TLB and interrupt vectors so that branch stayed in
 *     range.
 * None of them constrain anything anymore: there is no more BF from the
 * vector to a separate slow path, so nothing needs to stay in reach, and
 * removing them here rather than leaving them behind avoids puzzling the
 * next reader with layout rules that no longer protect anything.
 *
 * On entry to any vector (docs/mmu/linux-spec.md §4.1, hardware-spec.md):
 *   SR.MD=1, SR.BL=1, SR.RB=1   bank-1 GPRs selected; r0-r7 scratch, the
 *                               interrupted context's r0-r7 live in bank-0.
 *   PTEH   = faulting VPN (low PAGE_SHIFT bits clear)
 *   ASIDR  = current ASID_TAG
 *   TSBPTR = precomputed TSB slot address for (VPN, ASID_TAG) -- unused
 *            by this vector now; the walker already consulted it.
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
!   +0x400  all three TLB MISS causes, reached only on a hardware TSB-walker
!           miss; calls the C page-table walker (__jcore_tlb_walk())
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
	mov.l	5f, r6
	mov.l	@r6, r6			! r6 = current ASID_TAG (ASIDR P4 MMIO alias) (arg2)

	mov.l	3f, r0
	jsr	@r0			! __jcore_tlb_walk(pgd, addr, asid_tag)
	 nop

	! r0 = 0 on success (the walker has written the TSB row for this
	! VPN/ASID); nonzero means no valid mapping was found -- a real fault.
	tst	r0, r0
	bf	jcore_tlb_real_fault

	lds.l	@r15+, pr
	mov.l	@r15+, r7
	mov.l	@r15+, r6
	mov.l	@r15+, r5
	mov.l	@r15+, r4
	! Software no longer installs into the TLB: it only makes the
	! mapping *available* in the TSB and returns. The faulting access
	! re-executes, misses again, and the hardware TSB walker installs
	! from the row we just wrote.
	!
	! LIVELOCK WARNING: because we no longer install, a row the walker
	! refuses (wrong set, V clear, _PAGE_STALE) makes this fault repeat
	! forever instead of oopsing. jcore_tsb_write_entry() writing the
	! set the hardware itself computes (JCORE_TSB_SLOT), and the walk
	! returning -EFAULT for _PAGE_STALE, are what keep that from
	! happening; there is deliberately no retry counter here (the
	! handler is stateless and re-entered from scratch each fault).
	!
	! r15 is not banked and r4-r7 are, so the final pop must complete
	! before rte flips SR.RB -- do not fold it into the delay slot.
	rte
	 nop

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
5:	.long	JCORE_ASIDR

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
