/* Host-compiled (gcc, not cross). Proves jcore_pte_to_ptel builds the exact
 * hardware PTEL image the SP0 contract mandates. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#define CONFIG_CPU_JCORE 1
#include "jcore_pte_to_ptel_host.h"   /* thin shim including the real logic */

/* slot -> expected PageMask nibble at ptel[11:8]; pm table = {1,2,..,8} */
static void test_size_slots(void)
{
	unsigned long base_pa = 0x00100000UL;          /* PA[31:14] set */
	unsigned long flags   = _PAGE_VALID | _PAGE_CACHEABLE; /* bits 0,3 */
	unsigned int slot;
	static const unsigned pm[8] = {1,2,3,4,5,6,7,8};

	for (slot = 0; slot < 8; slot++) {
		unsigned long p = jcore_pte_set_size(base_pa | flags, slot);
		unsigned long ptel = jcore_pte_to_ptel(p);
		unsigned nib = (ptel >> 8) & 0xF;
		if (nib != pm[slot]) { printf("slot %u: pm %u != %u\n", slot, nib, pm[slot]); exit(1); }
	}
	{
		unsigned long a = jcore_pte_to_ptel(base_pa | flags);
		unsigned long b = jcore_pte_to_ptel(jcore_pte_set_size(base_pa | flags, 0));
		if (a != b) { printf("base != slot0: %#lx %#lx\n", a, b); exit(1); }
	}
	for (slot = 0; slot < 8; slot++)
		if (jcore_pte_size_slot(jcore_pte_set_size(base_pa | flags, slot)) != slot)
			{ printf("slot %u round-trip failed\n", slot); exit(1); }
	printf("test_size_slots OK\n");
}

/*
 * Standalone mirror of the non-X2TLB swap-pte encoding from
 * arch/sh/include/asm/pgtable_32.h (too kernel-entangled -- pte_t,
 * PTE_BIT_FUNC, page.h -- to host-include directly). This reproduces the
 * exact macros so the bit arithmetic underlying the _PAGE_SWP_EXCLUSIVE
 * fix can be exercised on the host:
 *
 *   __swp_entry(type, offset)   -> swp.val = (type & 0x1f) | offset << 10
 *   __swp_entry_to_pte(x)       -> pte.pte_low = x.val << 1
 *   __pte_to_swp_entry(pte)     -> swp.val = pte_val(pte) >> 1
 *   __swp_type(x)               -> x.val & 0x1f
 *   __swp_offset(x)             -> x.val >> 10
 */
#define SWP_ENTRY(type, offset)	((((type) & 0x1fUL) | ((offset) << 10)))
#define SWP_ENTRY_TO_PTE(x)	((x) << 1)
#define PTE_TO_SWP_ENTRY(pte)	((pte) >> 1)
#define SWP_TYPE(x)		((x) & 0x1fUL)
#define SWP_OFFSET(x)		((x) >> 10)

static void test_swp_exclusive_no_collision(void)
{
	/* type=20 (> 15, needs bit 4 of the 5-bit type field) so any
	 * collision with a stray hw bit shows up immediately. */
	const unsigned long type = 20;
	const unsigned long offset = 0x12345UL;
	unsigned long swp = SWP_ENTRY(type, offset);
	unsigned long pte_low = SWP_ENTRY_TO_PTE(swp);

	/* _PAGE_SWP_EXCLUSIVE must not overlap the type field (pte_low
	 * bits 1..5) nor the offset field (pte_low bits 11..31). */
	assert((_PAGE_SWP_EXCLUSIVE & 0x3EUL) == 0);
	assert((_PAGE_SWP_EXCLUSIVE & 0xFFFFF800UL) == 0);
	assert(_PAGE_SWP_EXCLUSIVE == _PAGE_EXEC);

	/* Round-trip without the exclusive marker. */
	swp = PTE_TO_SWP_ENTRY(pte_low);
	assert(SWP_TYPE(swp) == type);
	assert(SWP_OFFSET(swp) == offset);

	/* Set exclusive, verify type/offset survive unchanged. */
	pte_low |= _PAGE_SWP_EXCLUSIVE;
	assert(pte_low & _PAGE_SWP_EXCLUSIVE);
	swp = PTE_TO_SWP_ENTRY(pte_low);
	assert(SWP_TYPE(swp) == type);
	assert(SWP_OFFSET(swp) == offset);

	/* Clear exclusive, verify type/offset still unchanged and the
	 * marker is gone. */
	pte_low &= ~(unsigned long)_PAGE_SWP_EXCLUSIVE;
	assert(!(pte_low & _PAGE_SWP_EXCLUSIVE));
	swp = PTE_TO_SWP_ENTRY(pte_low);
	assert(SWP_TYPE(swp) == type);
	assert(SWP_OFFSET(swp) == offset);
}

int main(void)
{
	/* A kernel RWX cacheable global page at PA 0x0000_4000 (PFN in 16KB) */
	unsigned long pte = _PAGE_VALID|_PAGE_WRITE|_PAGE_EXEC|_PAGE_CACHEABLE|
			    _PAGE_GLOBAL|_PAGE_DIRTY | (0x00004000UL);
	unsigned long ptel = jcore_pte_to_ptel(pte);
	/* hardware layout: W7 X6 U5 D4 C3 G2 STALE1 V0, PageMask[11:8]=1, PPN[31:10] */
	assert((ptel & 0x1) == 0x1);            /* V0 */
	assert((ptel >> 7 & 1) == 1);           /* W7 */
	assert((ptel >> 6 & 1) == 1);           /* X6 */
	assert((ptel >> 5 & 1) == 0);           /* U5 (kernel) */
	assert((ptel >> 4 & 1) == 1);           /* D4 */
	assert((ptel >> 3 & 1) == 1);           /* C3 */
	assert((ptel >> 2 & 1) == 1);           /* G2 */
	assert(((ptel >> 8) & 0xF) == 1);       /* PageMask = 1 (16 KB) */
	assert((ptel & 0xFFFFFC00) == 0x00004000); /* PPN[31:10] = PA[31:10] */
	/* A user read-only page must NOT set W7/D4, must set U5 */
	unsigned long upte = _PAGE_VALID|_PAGE_USER|_PAGE_CACHEABLE | 0x00008000UL;
	unsigned long uptel = jcore_pte_to_ptel(upte);
	assert((uptel >> 7 & 1) == 0 && (uptel >> 5 & 1) == 1);

	/* High multi-bit PA: full PPN[31:10] must appear untruncated. */
	unsigned long hpte = _PAGE_VALID|_PAGE_WRITE|_PAGE_CACHEABLE |
			     0xABCD4000UL;
	unsigned long hptel = jcore_pte_to_ptel(hpte);
	assert((hptel & 0xFFFFFC00UL) == (0xABCD4000UL & 0xFFFFFC00UL));
	assert((hptel & 0xFFFFFC00UL) == 0xABCD4000UL);

	/* Software-only bits must never leak into the hw PTEL image. */
	unsigned long spte = _PAGE_VALID | _PAGE_ACCESSED | _PAGE_PROTNONE |
			     _PAGE_SPECIAL | (0x00004000UL);
	unsigned long sptel = jcore_pte_to_ptel(spte);
	/* PTEL bits 0..7 are the only hw-flag bits jcore_pte_to_ptel() ever
	 * populates from the pte's low byte; they must reflect exactly the
	 * pte's _PAGE_HW_BITS_MASK bits and nothing the soft bits contribute
	 * (note: PTEL bit 8 is PageMask, a different field in a different
	 * namespace than the pte's _PAGE_ACCESSED bit 8 -- comparing whole
	 * words would conflate the two, so restrict the check to bits 0..7). */
	assert((sptel & 0xFF) == (spte & _PAGE_HW_BITS_MASK));
	assert((sptel & 0xFF) == _PAGE_VALID);
	/* STALE is a genuine hw bit (PTEL.STALE, bit 1) so it's expected to
	 * pass through -- verify it does exactly that, not accidentally
	 * masked, when set alongside the soft bits above. */
	unsigned long stpte = spte | _PAGE_STALE;
	unsigned long stptel = jcore_pte_to_ptel(stpte);
	assert((stptel & 0xFF) == (stpte & _PAGE_HW_BITS_MASK));
	assert((stptel & 0xFF) == (_PAGE_VALID | _PAGE_STALE));

	test_swp_exclusive_no_collision();
	test_size_slots();

	printf("jcore_pte_to_ptel: all asserts passed\n");
	return 0;
}
