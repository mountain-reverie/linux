/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_SH_PGTABLE_BITS_JCORE_H
#define __ASM_SH_PGTABLE_BITS_JCORE_H

/*
 * J-core (J4) MMU: 32-bit pte_t layout + the single pte -> PTEL conversion.
 *
 * This header is intentionally free of any other kernel header dependency
 * (no <linux/...>, no other <asm/...>) so that jcore_pte_to_ptel() can be
 * compiled both by the kernel proper and by a host/bare-metal unit test
 * (see arch/sh/mm/tests/jcore_pte_to_ptel_test.c and SP2's bare-metal
 * harness).
 *
 * The BASE page size is fixed at 16 KB (PAGE_SHIFT == 14), so a
 * page-aligned physical address always has its low 14 bits clear.
 * HugeTLB pages of 64K..256M also exist; which size a given PTE
 * describes is encoded per-PTE in the 3-bit size slot below
 * (_PAGE_JCORE_SZ_BITS, bits 10/12/13).
 *
 * ---------------------------------------------------------------------
 * Linux pte_t (pte_low, 32-bit) layout chosen for jcore:
 *
 *   bit:  31........14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
 *         |   PFN     |SZ2|SZ1|SP|SZ0|PN|AC|WR|EX|US|DI|CA|GL|ST|VA|
 *
 *   31:14  PFN            physical page number (PA[31:14]), same
 *                         convention as the rest of arch/sh (pfn_pte()
 *                         shifts the pfn left by PAGE_SHIFT and ORs in
 *                         pgprot_val()).
 *   13,12  _PAGE_JCORE_SZ_BITS[2:1]  page-size slot bits 2 and 1 (see
 *                         below); named SZ2/SZ1 in the diagram above.
 *   11     _PAGE_SPECIAL  software only (0x800, NOT bit 10)
 *   10     _PAGE_JCORE_SZ_BITS[0]  page-size slot bit 0 (SZ0 above)
 *    9     _PAGE_PROTNONE software only (vma protection None)
 *    8     _PAGE_ACCESSED software only (referenced)
 *    7     _PAGE_WRITE    hw: PTEL.W  (bit 7)
 *    6     _PAGE_EXEC     hw: PTEL.X  (bit 6)
 *    5     _PAGE_USER     hw: PTEL.U  (bit 5)
 *    4     _PAGE_DIRTY    hw: PTEL.D  (bit 4)
 *    3     _PAGE_CACHEABLE hw: PTEL.C (bit 3)
 *    2     _PAGE_GLOBAL   hw: PTEL.G  (bit 2)
 *    1     _PAGE_STALE    hw: PTEL.STALE (bit 1) - TLB-entry validity
 *                         tracking bit written through to hardware, but
 *                         not one of the standard Linux permission bits.
 *    0     _PAGE_VALID    hw: PTEL.V  (bit 0)
 *
 * Bits 0..7 of the Linux pte therefore sit at EXACTLY the same bit
 * position as the corresponding hardware PTEL bit (the SP0 contract's
 * "W7 X6 U5 D4 C3 G2 STALE1 V0" image), which is what makes
 * jcore_pte_to_ptel() a couple of masks instead of a bit-shuffle.
 *
 * Compatibility aliases: the generic (non-X2TLB) SH pgtable_32.h code
 * (PAGE_KERNEL, PAGE_SHARED, pte_write(), ...) is written in terms of
 * _PAGE_PRESENT / _PAGE_RW / _PAGE_CACHABLE / _PAGE_HW_SHARED. Alias
 * those to the jcore names below so that generic code keeps working
 * unmodified for CONFIG_CPU_JCORE.
 * ---------------------------------------------------------------------
 */

#define _PAGE_VALID	0x001	/* V-bit    : TLB entry / page valid */
#define _PAGE_STALE	0x002	/* STALE-bit: hw TLB-entry staleness marker */
#define _PAGE_GLOBAL	0x004	/* G-bit    : global (ASID-independent) */
#define _PAGE_CACHEABLE	0x008	/* C-bit    : cacheable */
#define _PAGE_DIRTY	0x010	/* D-bit    : page written */
#define _PAGE_USER	0x020	/* U-bit    : user-mode access allowed */
#define _PAGE_EXEC	0x040	/* X-bit    : executable */
#define _PAGE_WRITE	0x080	/* W-bit    : write access allowed */

#define _PAGE_ACCESSED	0x100	/* software: page referenced */
#define _PAGE_PROTNONE	0x200	/* software: vma prot none */
#define _PAGE_SPECIAL	0x800	/* software: special page */

/* Hardware bit-image bits, exposed as a single mask for convenience. */
#define _PAGE_HW_BITS_MASK \
	(_PAGE_VALID | _PAGE_STALE | _PAGE_GLOBAL | _PAGE_CACHEABLE | \
	 _PAGE_DIRTY | _PAGE_USER | _PAGE_EXEC | _PAGE_WRITE)

/* Compat aliases so unmodified generic (non-X2TLB) pgtable_32.h code
 * (PAGE_KERNEL/PAGE_SHARED/pte_write()/...) keeps working for jcore. */
#define _PAGE_PRESENT	_PAGE_VALID
#define _PAGE_RW	_PAGE_WRITE
#define _PAGE_CACHABLE	_PAGE_CACHEABLE
#define _PAGE_HW_SHARED	_PAGE_GLOBAL

/*
 * Non-X2TLB swap-pte encoding (see arch/sh/include/asm/pgtable_32.h):
 * __swp_entry_to_pte() does `pte_low = swp.val << 1`, where swp.val packs
 * `type` in bits 0..4 and `offset` in bits 10..31. After the <<1 shift,
 * pte_low bits 1..5 hold `type` and pte_low bits 11..31 hold `offset`;
 * pte_low bits 6..9 are always zero. The generic code stashes the swap
 * exclusive-marker in _PAGE_USER (bit 6 on stock SH, safely inside that
 * free zone). jcore's _PAGE_USER is bit 5 instead, which collides with
 * `type`'s MSB -- reusing it would silently corrupt swap type >= 16 (and
 * any offset bit 0, since offset starts at bit 10 anyway) whenever a page
 * is marked/cleared exclusive. Override to _PAGE_EXEC (bit 6), which is
 * still inside the pte_low bits 6..9 free zone and carries no meaning on a
 * non-present (swap) pte.
 */
#define _PAGE_SWP_EXCLUSIVE	_PAGE_EXEC

/* PTEL PageMask[11:8] field value for 16 KB pages. */
#define _PAGE_JCORE_PAGEMASK_16KB	0x1UL

/* PTEL bit 10 is the boundary below which PFN bits are reserved/masked
 * by PageMask; PPN sits at PTEL[31:10]. */
#define _PAGE_JCORE_PPN_SHIFT		10
#define _PAGE_JCORE_PAGEMASK_SHIFT	8

/* 3-bit page-size slot packed into free flag bits {10,12,13}:
 *   slot bit0 -> pte bit10, bit1 -> bit12, bit2 -> bit13.
 * slot 0 = base (16 KB); slots 1..7 = 64K,256K,1M,4M,16M,64M,256M.
 * Helpers are unsigned-long based so this header stays host-compilable. */
#define _PAGE_JCORE_SZ_BITS	((1UL<<10) | (1UL<<12) | (1UL<<13))

#ifndef __ASSEMBLY__
/* slot -> hardware PageMask pm (page size = 4KB << 2*pm) */
static const unsigned char jcore_pm_for_slot[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

static inline unsigned int jcore_pte_size_slot(unsigned long pte_val)
{
	return ((pte_val >> 10) & 1) | ((pte_val >> 11) & 2) | ((pte_val >> 11) & 4);
}

static inline unsigned long jcore_pte_set_size(unsigned long pte_val, unsigned int slot)
{
	pte_val &= ~_PAGE_JCORE_SZ_BITS;
	pte_val |= ((slot & 1UL) << 10) | ((slot & 2UL) << 11) | ((slot & 4UL) << 11);
	return pte_val;
}

/*
 * jcore_pte_to_ptel() - convert a Linux pte_t value into the hardware
 * PTEL image the J4 MMU TLB-fill/walker expects.
 *
 * Pure function: no kernel dependencies, safe to link into a bare-metal
 * (SP2) harness or a host unit test.
 *
 * Hardware PTEL image: PPN[31:10] | PageMask[11:8] | W7 X6 U5 D4 C3 G2
 * STALE1 V0.
 *
 * Guarded by __ASSEMBLY__ (not defined by the kernel proper, but passed
 * explicitly by bare-metal .S harnesses -- e.g. jcore-cpu's SP2
 * sim/tests/mmulinux.S -- that #include this header purely for the
 * _PAGE_* bit values) so the function body, which is not valid gas input,
 * does not get pulled into an assembly file.
 */
static inline unsigned long jcore_pte_to_ptel(unsigned long pte_val)
{
	unsigned long ppn = pte_val & ~0x3FFFUL;	/* PA[31:14], low 14 bits are flags */
	unsigned long hwbits = pte_val & _PAGE_HW_BITS_MASK; /* bits 0..7, hw-positioned already */
	unsigned long pagemask = (unsigned long)jcore_pm_for_slot[jcore_pte_size_slot(pte_val)]
				 << _PAGE_JCORE_PAGEMASK_SHIFT;

	/*
	 * Withhold PTEL.W (write permission) on clean pages to trap the first
	 * write. J-Core hardware does not set PTEL.D, so without this guard,
	 * clean writable pages install writable, the first write never traps,
	 * and _PAGE_DIRTY is never set -- causing data loss in writeback-managed
	 * mappings. SH-4 solves this with a hardware initial-page-write exception.
	 * J-Core has none, so we instead trap via DPROT_W, and the handler
	 * (FAULT_CODE_WRITE + FAULT_FLAG_WRITE in do_page_fault) sets _PAGE_DIRTY
	 * via generic mm, which then re-faults with write permission granted. This
	 * applies only to user mappings; PAGE_KERNEL includes _PAGE_DIRTY, so
	 * kernel mappings install writable and take no extra faults on the direct map.
	 */
	if (!(pte_val & _PAGE_DIRTY))
		hwbits &= ~_PAGE_WRITE;

	return ppn | pagemask | hwbits;
}
#endif /* !__ASSEMBLY__ */

#endif /* __ASM_SH_PGTABLE_BITS_JCORE_H */
