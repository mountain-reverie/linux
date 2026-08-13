/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arch/sh/include/asm/tlb-jcore.h
 *
 * J-Core J4 MMU: TLB-miss handler declarations.
 *
 * Declares the software TLB-miss walker (__jcore_tlb_walk) and the
 * weak-symbol accessible bit setter (jcore_tlb_walk_mark_accessed) so
 * that bare-metal test harnesses can link a replacement for the latter.
 */
#ifndef __ASM_SH_TLB_JCORE_H
#define __ASM_SH_TLB_JCORE_H

#include <asm/pgtable.h>

/*
 * __jcore_tlb_walk() - software TLB-miss slow path
 *
 * @pgd:       root of the page table to walk
 * @addr:      faulting address
 * @pteh_tag:  expected ASID_TAG (TSB tag_lo)
 *
 * Returns 0 on success (TSB row written; the caller just returns and the
 * hardware walker installs from that row on the re-executed access);
 * returns nonzero on failure (not present or protected).
 */
int __jcore_tlb_walk(pgd_t *pgd, unsigned long addr, unsigned long pteh_tag);

/*
 * jcore_tlb_walk_mark_accessed() - update the _PAGE_ACCESSED bit in the pte
 *
 * Weak symbol, overridable by bare-metal test harness stubs.
 */
void jcore_tlb_walk_mark_accessed(pte_t *ptep, pte_t entry);

#endif /* __ASM_SH_TLB_JCORE_H */
