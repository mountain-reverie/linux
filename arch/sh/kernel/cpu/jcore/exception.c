// SPDX-License-Identifier: GPL-2.0
/*
 * arch/sh/kernel/cpu/jcore/exception.c
 *
 * J-Core J4 general-exception dispatch (the tail of VBR+0x100).
 *
 * J4 implements the SH-4 privileged model -- SPC/SSR/EXPEVT/INTEVT, VBR fixed
 * vectors, SR.RB banking -- so the vector at VBR+0x100 is reached as CODE with
 * the cause in EXPEVT, exactly as on SH-3/SH-4. entry.S builds pt_regs, splits
 * off the TLB protection faults (which are page faults and go straight to
 * do_page_fault), and calls here with everything else.
 *
 * This exists because the port previously routed generic exceptions through
 * sh2/entry.S's exception_handler, which reads the interrupted PC and SR from
 * fixed offsets on the STACK. That is correct for SH-2, which pushes them; J4
 * delivers them in SPC/SSR and never pushes. The J2 path could not have worked
 * on J4, and VBR+0x100 was not even code -- see the vector-page comment in
 * ex.S.
 */

#include <linux/kernel.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <asm/traps.h>

/* SH-4 architectural EXPEVT codes J4 raises at VBR+0x100. */
#define EXPEVT_ADDR_ERR_LOAD	0x0e0
#define EXPEVT_ADDR_ERR_STORE	0x100
#define EXPEVT_TRAPA		0x160
#define EXPEVT_ILLEGAL		0x180	/* general illegal instruction */
#define EXPEVT_SLOT_ILLEGAL	0x1a0	/* illegal instruction in a delay slot */

/*
 * Called from jcore_general_entry with a fully-built pt_regs. Protection
 * faults never arrive here -- entry.S routes those to do_page_fault() before
 * calling us -- so this is the non-page-fault half of VBR+0x100.
 */
asmlinkage void jcore_handle_exception(struct pt_regs *regs,
				       unsigned long expevt)
{
	switch (expevt) {
	case EXPEVT_TRAPA:
		/*
		 * TRAPA is the syscall entry on SH. The immediate is in TRA
		 * (shifted left 2 by hardware); generic code expects it in
		 * regs->tra, which entry.S has not populated for this path
		 * yet -- syscall dispatch is deliberately NOT wired up here.
		 * Trapping loudly beats dispatching a syscall with a bogus
		 * number: the bare-metal guard (mmulinuxexc sub-test A) only
		 * asserts that control reaches this dispatcher with the right
		 * EXPEVT/TRA, which is the mechanism this change is proving.
		 */
		die_if_kernel("TRAPA (syscall dispatch not wired up on J4)",
			      regs, expevt);
		force_sig(SIGILL);
		break;

	case EXPEVT_ILLEGAL:
	case EXPEVT_SLOT_ILLEGAL:
		die_if_kernel("illegal instruction", regs, expevt);
		force_sig(SIGILL);
		break;

	case EXPEVT_ADDR_ERR_LOAD:
	case EXPEVT_ADDR_ERR_STORE:
		die_if_kernel("unaligned access", regs, expevt);
		force_sig(SIGBUS);
		break;

	default:
		die_if_kernel("unexpected exception", regs, expevt);
		force_sig(SIGILL);
		break;
	}
}
