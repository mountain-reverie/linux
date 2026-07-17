/* Host-compiled (gcc, not cross). Proves ASID_TAG generation threading
 * and gen_low-wrap detection */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

/* Standalone-compiled mirror of jcore_encode_asid_tag + the
 * (not-yet-existing) jcore_asid_gen_wrapped predicate from
 * arch/sh/include/asm/mmu_context_32.h.
 *
 * Constants match the kernel (byte-for-byte). */
#define CONFIG_CPU_JCORE 1
#define MMU_CONTEXT_ASID_MASK 0x00000fffUL
#define JCORE_ASID_GEN_SHIFT 12
#define JCORE_ASID_GEN_BITS 4
#define JCORE_ASID_GEN_MASK ((1U << JCORE_ASID_GEN_BITS) - 1)

typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* jcore_encode_asid_tag: encode a Linux ASID + generation into the J4
 * hardware ASID_TAG 16-bit value for ASIDR. */
static inline u16 jcore_encode_asid_tag(u16 asid, u64 gen)
{
	return (asid & MMU_CONTEXT_ASID_MASK) |
	       (((u32)(gen >> JCORE_ASID_GEN_SHIFT) & JCORE_ASID_GEN_MASK)
			<< JCORE_ASID_GEN_SHIFT);
}

/* jcore_asid_gen_wrapped: true iff the generation's low nibble is 0,
 * indicating the TLB generation counter has wrapped and gen_low is about
 * to be reused. Triggers TSB rebuild to reject stale entries. */
static inline bool jcore_asid_gen_wrapped(unsigned long ctx)
{
	return (((ctx >> JCORE_ASID_GEN_SHIFT) & JCORE_ASID_GEN_MASK) == 0);
}

static void test_encode_threads_gen_low(void)
{
	/* version 3 in high nibble with asid 0x1ab in low 12 bits */
	assert(jcore_encode_asid_tag(0x1ab, 0x3000) == 0x31ab);

	/* version 0x11 with asid 0x7ff (max 12-bit asid) */
	unsigned long ctx = (0x11UL << 12) | 0x7ff;
	assert(jcore_encode_asid_tag(0x7ff, ctx) == 0x17ff);

	/* gen 0 -> tag == asid (the old hardcoded behaviour) */
	assert(jcore_encode_asid_tag(0x123, 0) == 0x123);
}

static void test_gen_wrap_predicate(void)
{
	/* version 16 -> gen_low 0 (wrapped) */
	assert(jcore_asid_gen_wrapped((16UL << 12) | 0x1) == true);

	/* version 1 -> gen_low 1 (not wrapped) */
	assert(jcore_asid_gen_wrapped((1UL << 12) | 0x1) == false);

	/* version 31 -> gen_low 15 (not wrapped) */
	assert(jcore_asid_gen_wrapped((0x1fUL << 12)) == false);

	/* version 32 -> gen_low 0 (wrapped) */
	assert(jcore_asid_gen_wrapped((0x20UL << 12)) == true);

	/* ctx == 0 (full wrap: asid=0, gen_low=0) -- the security-critical
	 * trigger case, must be detected as wrapped */
	assert(jcore_asid_gen_wrapped(0) == true);
}

int main(void)
{
	test_encode_threads_gen_low();
	test_gen_wrap_predicate();
	return 0; /* assert() aborts on failure; exit 0 == pass */
}
