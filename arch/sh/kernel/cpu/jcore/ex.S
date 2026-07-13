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
 * addresses. Task 5 (head_32.S / per_cpu_trap_init()) points VBR at
 * jcore_vbr_base (below) instead of the generic sh2/ex.S vbr_base, so
 * that these stubs actually land at literal VBR+0x400/0x420/0x440 --
 * hardware-spec.md's "same vector layout as SH-4" means the CPU does a
 * direct PC = VBR + fixed-offset jump here, not an indexed lookup.
 *
 * jcore_vbr_base is laid out exactly like sh2/ex.S's vbr_base (a
 * 256-entry, EXPEVT-style pointer table used for TRAPA/general
 * exceptions -- reusing sh2/entry.S's exception_handler dispatch
 * unchanged) so J4's generic non-MMU exception handling keeps working
 * byte-for-byte the same as plain SH-2. That table is exactly
 * 256 * 4 = 1024 = 0x400 bytes, so jcore_vec_imiss below begins at
 * precisely jcore_vbr_base + 0x400 with no padding, as long as nothing
 * else is emitted in between (hence no stray .align between the
 * .rept/.endr and ENTRY(jcore_vec_imiss)). Each vector stub is then
 * padded out to a fixed 0x20-byte stride with .balign so dload/dstore
 * land exactly on +0x420/+0x440.
 */
#include <linux/linkage.h>

	.text
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
	mov.l	1f, r0
	jmp	@r0
	 nop
	.align	4
1:	.long	jcore_tlb_miss
	.balign	0x20

ENTRY(jcore_vec_dload)			/* VBR + 0x420: D-side load miss */
	mov.l	2f, r0
	jmp	@r0
	 nop
	.align	4
2:	.long	jcore_tlb_miss
	.balign	0x20

ENTRY(jcore_vec_dstore)			/* VBR + 0x440: D-side store miss */
	mov.l	3f, r0
	jmp	@r0
	 nop
	.align	4
3:	.long	jcore_tlb_miss

	.align	4
ENTRY(jcore_tlb_vectors)
	.long	jcore_vec_imiss		/* VBR + 0x400 */
	.long	jcore_vec_dload		/* VBR + 0x420 */
	.long	jcore_vec_dstore	/* VBR + 0x440 */
