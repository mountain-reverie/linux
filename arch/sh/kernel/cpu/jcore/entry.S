/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/sh/kernel/cpu/jcore/entry.S
 *
 * J-Core J4: TLB-miss real-fault tail.
 *
 * J4's fixed TLB-miss vectors (ex.S) are hardware-assisted: PTEH/ASIDR/
 * TSBPTR are pre-loaded before entry, so the hot path (tlbmiss.S) needs
 * no register-save frame at all, unlike SH-3/SH-4's EXPEVT-dispatched
 * model (arch/sh/kernel/cpu/sh3/entry.S). Only the rare case where the
 * page-table walk (__jcore_tlb_walk(), tlb-jcore.c) finds no valid
 * mapping needs a full pt_regs, to hand off to the generic
 * do_page_fault().
 *
 * jcore_tlb_fault_entry is entered (by jmp, from tlbmiss.S's
 * jcore_tlb_real_fault) with SR.MD=1/BL=1/RB=1 still set, PR already
 * restored to the interrupted context's original PR (tlbmiss.S saves/
 * restores it around the __jcore_tlb_walk() call), and SPC/SSR/GBR/
 * MACH/MACL untouched since vector entry. r0-r3 are dead scratch;
 * r4-r14 hold whatever the interrupted (bank-0) context left in them,
 * *except* r0-r7 are banked, so the interrupted context's real r0-r7
 * values are read via the r*_bank control-register alias without ever
 * toggling SR.RB (the same trick arch/sh/kernel/cpu/sh3/entry.S's
 * save_regs() uses).
 *
 * NOTE: this is the one part of the TLB-miss path SP2's bare-metal
 * harness does *not* exercise (it drives tlbmiss.S / __jcore_tlb_walk()
 * directly against a stubbed TSB). Task 5 (head/boot bring-up) is
 * expected to validate this tail once full boot brings do_page_fault()
 * and real user/kernel mode transitions into play.
 */
#include <linux/linkage.h>
#include <cpu/mmu_context.h>

/* struct pt_regs offsets (arch/sh/include/uapi/asm/ptrace_32.h) */
OFF_R0		=  0
OFF_SP		=  (15*4)
OFF_PC		=  (16*4)
OFF_PR		=  (16*4+4)
OFF_SR		=  (16*4+8)
OFF_GBR		=  (16*4+12)
OFF_MACH	=  (16*4+16)
OFF_MACL	=  (16*4+20)
OFF_TRA		=  (16*4+24)
PT_SIZE		=  (16*4+28)

	.text
	.align	4
/*
 * JCORE_SAVE_REGS / JCORE_RESTORE_REGS_RTE
 *
 * Build and tear down a pt_regs frame around a J4 exception. Factored into
 * macros because J4 has THREE fixed vectors that all need the identical frame
 * -- TLB real-fault, general exception (VBR+0x100) and interrupt (VBR+0x600).
 * Previously only the TLB tail existed, and the other two vectors had no
 * handler at all; duplicating ~100 lines of frame code three ways to fix that
 * would guarantee the copies drift.
 *
 * Entered with SR.MD=1/BL=1/RB=1 as hardware left them. r0-r7 are BANKED, so
 * the interrupted context's real r0-r7 are read through the r*_bank aliases
 * without ever toggling SR.RB -- the same trick sh3/entry.S's save_regs()
 * uses. OFF_PC..OFF_TRA sit beyond mov.l's disp4 reach, hence the R0-indexed
 * addressing (offset in r0, data in r1; R0 cannot be both index and dest).
 */
	.macro	JCORE_SAVE_REGS
	add	#-PT_SIZE, r15

	stc	r0_bank, r0
	mov.l	r0, @(OFF_R0+0*4, r15)
	stc	r1_bank, r0
	mov.l	r0, @(OFF_R0+1*4, r15)
	stc	r2_bank, r0
	mov.l	r0, @(OFF_R0+2*4, r15)
	stc	r3_bank, r0
	mov.l	r0, @(OFF_R0+3*4, r15)
	stc	r4_bank, r0
	mov.l	r0, @(OFF_R0+4*4, r15)
	stc	r5_bank, r0
	mov.l	r0, @(OFF_R0+5*4, r15)
	stc	r6_bank, r0
	mov.l	r0, @(OFF_R0+6*4, r15)
	stc	r7_bank, r0
	mov.l	r0, @(OFF_R0+7*4, r15)
	mov.l	r8,  @(OFF_R0+8*4, r15)
	mov.l	r9,  @(OFF_R0+9*4, r15)
	mov.l	r10, @(OFF_R0+10*4, r15)
	mov.l	r11, @(OFF_R0+11*4, r15)
	mov.l	r12, @(OFF_R0+12*4, r15)
	mov.l	r13, @(OFF_R0+13*4, r15)
	mov.l	r14, @(OFF_R0+14*4, r15)

	mov	r15, r0
	add	#PT_SIZE, r0
	mov.l	r0, @(OFF_SP, r15)	! pre-fault-frame SP

	! OFF_PC..OFF_TRA sit beyond the +/-60 reach of mov.l's disp4
	! form, so store these via R0-indexed addressing (data in r1,
	! r0 holds the byte offset) rather than @(disp,r15).
	stc	spc, r1
	mov	#OFF_PC, r0
	mov.l	r1, @(r0, r15)
	sts	pr, r1
	mov	#OFF_PR, r0
	mov.l	r1, @(r0, r15)
	stc	ssr, r1
	mov	#OFF_SR, r0
	mov.l	r1, @(r0, r15)
	stc	gbr, r1
	mov	#OFF_GBR, r0
	mov.l	r1, @(r0, r15)
	sts	mach, r1
	mov	#OFF_MACH, r0
	mov.l	r1, @(r0, r15)
	sts	macl, r1
	mov	#OFF_MACL, r0
	mov.l	r1, @(r0, r15)
	mov	#-1, r1
	mov	#OFF_TRA, r0
	mov.l	r1, @(r0, r15)

	.endm

/*
 * Restores from pt_regs and returns. The handler may legitimately have
 * modified regs->pc / regs->sr (do_page_fault's force_sig path, signal
 * delivery), so those are reloaded from the frame rather than left in
 * SPC/SSR.
 */
	.macro	JCORE_RESTORE_REGS_RTE
	! Restore control regs, then GPRs, then rte. do_page_fault() may
	! have modified regs->pc/sr (e.g. force_sig path) -- honour that.
	! Same R0-indexed addressing as the save side: offset in r0,
	! loaded value in r1 (R0 can't double as both index and dest).
	mov	#OFF_PC, r0
	mov.l	@(r0, r15), r1
	ldc	r1, spc
	mov	#OFF_SR, r0
	mov.l	@(r0, r15), r1
	ldc	r1, ssr
	mov	#OFF_PR, r0
	mov.l	@(r0, r15), r1
	lds	r1, pr
	mov	#OFF_GBR, r0
	mov.l	@(r0, r15), r1
	ldc	r1, gbr
	mov	#OFF_MACH, r0
	mov.l	@(r0, r15), r1
	lds	r1, mach
	mov	#OFF_MACL, r0
	mov.l	@(r0, r15), r1
	lds	r1, macl

	mov.l	@(OFF_R0+14*4, r15), r14
	mov.l	@(OFF_R0+13*4, r15), r13
	mov.l	@(OFF_R0+12*4, r15), r12
	mov.l	@(OFF_R0+11*4, r15), r11
	mov.l	@(OFF_R0+10*4, r15), r10
	mov.l	@(OFF_R0+9*4, r15),  r9
	mov.l	@(OFF_R0+8*4, r15),  r8
	mov.l	@(OFF_R0+7*4, r15),  r0
	ldc	r0, r7_bank
	mov.l	@(OFF_R0+6*4, r15),  r0
	ldc	r0, r6_bank
	mov.l	@(OFF_R0+5*4, r15),  r0
	ldc	r0, r5_bank
	mov.l	@(OFF_R0+4*4, r15),  r0
	ldc	r0, r4_bank
	mov.l	@(OFF_R0+3*4, r15),  r0
	ldc	r0, r3_bank
	mov.l	@(OFF_R0+2*4, r15),  r0
	ldc	r0, r2_bank
	mov.l	@(OFF_R0+1*4, r15),  r0
	ldc	r0, r1_bank
	mov.l	@(OFF_R0+0*4, r15),  r0
	ldc	r0, r0_bank

	add	#PT_SIZE, r15
	rte
	 nop
	.endm

ENTRY(jcore_tlb_fault_entry)
	JCORE_SAVE_REGS

	mov	r15, r4			! arg0: regs
	! arg1: error_code, MMUFSR. low byte laid out as
	! FAULT_CODE_* image (USER/PROT/ITLB/INITIAL/WRITE) so no mapping
	! needed; extu.b drops diagnostic KIND/VALID field above it.
	! MMUFSR overwritten by next fault episode, so read here before
	! interrupts enabled or user page tables touched.
	mov.l	3f, r5
	mov.l	@r5, r5			! read MMUFSR
	extu.b	r5, r5			! extract low byte (error_code)
	mov.l	1f, r6
	mov.l	@r6, r6			! arg2: faulting address (MMU_TEA)

	mov.l	2f, r0
	jsr	@r0			! do_page_fault(regs, error_code, address)
	 nop

	JCORE_RESTORE_REGS_RTE



/*
 * jcore_general_entry -- the tail of VBR+0x100 (ex.S vectors here).
 *
 * J4 sends Error, Slot Illegal, General Illegal, TRAPA and -- since TLB
 * protection was split off the miss vector to match SH-4 (SH7750 manual
 * Rev 2.0 02/99: misses H'400, protection H'100) -- IPROT and DPROT_R/W here
 * too. EXPEVT is the discriminator, exactly as on SH-3/SH-4.
 *
 * Protection faults are page faults and go to do_page_fault() with the same
 * MMUFSR-derived error_code the TLB tail uses. Everything else is handed to
 * the C dispatcher with its raw EXPEVT.
 *
 * This replaces the J2 path the port inherited: jcore_vbr_base used to be a
 * 256-entry POINTER table feeding sh2/entry.S's exception_handler, which
 * expects SH-2's stack-pushed PC/SR at fixed offsets. J4 delivers SPC/SSR in
 * registers and branches to VBR+0x100 as CODE, so that table was being
 * executed as instructions. Guard: mmulinuxexc.
 */
ENTRY(jcore_general_entry)
	JCORE_SAVE_REGS

	! Named labels for control flow, numeric ONLY for the literal pool: this
	! routine both branches and loads literals, and reusing 1:/2:/3: for
	! both makes `mov.l 1f` resolve to whichever comes next textually.
	stc	expevt, r0
	mov.l	4f, r1			! EXPEVT_IPROT (0x0A0)
	cmp/eq	r0, r1
	bt	.Ljcore_gen_prot
	mov.l	5f, r1			! EXPEVT_DPROT (0x0C0, R and W alike)
	cmp/eq	r0, r1
	bf	.Ljcore_gen_other

.Ljcore_gen_prot:
	! ---- protection fault: a page fault by another name ----
	mov	r15, r4			! arg0: regs
	mov.l	3f, r5
	mov.l	@r5, r5			! MMUFSR
	extu.b	r5, r5			! arg1: error_code (FAULT_CODE_* image)
	mov.l	1f, r6
	mov.l	@r6, r6			! arg2: faulting address (MMU_TEA)
	mov.l	2f, r0
	jsr	@r0			! do_page_fault()
	 nop
	bra	.Ljcore_gen_done
	 nop

.Ljcore_gen_other:
	! ---- everything else: raw EXPEVT to the C dispatcher ----
	mov	r15, r4			! arg0: regs
	stc	expevt, r5		! arg1: EXPEVT
	mov.l	6f, r0
	jsr	@r0			! jcore_handle_exception()
	 nop

.Ljcore_gen_done:
	JCORE_RESTORE_REGS_RTE

/*
 * jcore_irq_entry -- the tail of VBR+0x600.
 *
 * J4 vectors every interrupt to one fixed address and leaves the source in
 * INTEVT, like SH-3/SH-4. Nothing was here before: VBR+0x600 landed in
 * whatever slow-path code happened to follow the inlined TLB fast path.
 * Guard: mmulinuxexc sub-test D.
 */
ENTRY(jcore_irq_entry)
	JCORE_SAVE_REGS

	stc	intevt, r4		! arg0: irq (raw INTEVT code)
	mov	r15, r5			! arg1: regs
	mov.l	7f, r0
	jsr	@r0			! do_IRQ()
	 nop

	JCORE_RESTORE_REGS_RTE

	.align	4
4:	.long	0x000000A0		! EXPEVT: instruction TLB protection
5:	.long	0x000000C0		! EXPEVT: data TLB protection (read or write)
6:	.long	jcore_handle_exception
7:	.long	do_IRQ

	.align	4
1:	.long	MMU_TEA
2:	.long	do_page_fault
3:	.long	MMU_FSR
