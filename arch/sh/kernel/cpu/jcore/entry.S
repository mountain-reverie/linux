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
ENTRY(jcore_tlb_fault_entry)
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

	.align	4
1:	.long	MMU_TEA
2:	.long	do_page_fault
3:	.long	MMU_FSR
