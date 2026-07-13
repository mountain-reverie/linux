/* Host-compiled (gcc, not cross). Proves jcore_pte_to_ptel builds the exact
 * hardware PTEL image the SP0 contract mandates. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#define CONFIG_CPU_JCORE 1
#include "jcore_pte_to_ptel_host.h"   /* thin shim including the real logic */

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
	printf("jcore_pte_to_ptel: all asserts passed\n");
	return 0;
}
