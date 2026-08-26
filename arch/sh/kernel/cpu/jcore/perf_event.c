// SPDX-License-Identifier: GPL-2.0
/*
 * Performance events support for the J-Core J4 fixed-function PMU.
 *
 * Modelled on arch/sh/kernel/cpu/sh4/perf_event.c.  Hardware spec:
 * jcore-cpu docs/pmu/perf-counters.md (RTL: core/perf.vhd, core/perf_pkg.vhd,
 * decoded in core/datapath.vhm).
 *
 * The block is EIGHT FIXED-FUNCTION 32-BIT COUNTERS.  There is no event-select
 * multiplexer: counter n counts event n and nothing else, always, for every
 * CPU privilege level.  Three consequences run through everything below.
 *
 *  1. Counters are not a scarce resource to be scheduled.  They free-run and
 *     are read-only from a measurement's point of view, so any number of perf
 *     events may share one counter.  The arch layer nevertheless hands out an
 *     opaque slot index (hw_perf_event::idx) chosen by first-free search, so
 *     this driver keeps a per-CPU slot -> counter map; ->read() is given only
 *     the slot, and that map is the only way back to the counter.
 *
 *  2. Counters are never cleared or preloaded.  A perf event's window is
 *     established by rebasing hw_perf_event::prev_count at ->enable() time,
 *     which is also how counts accrued while the event was descheduled are
 *     discarded.  (The RTL deliberately made the counters writable so that
 *     perf could preload a sampling period.  With no PMU interrupt there is no
 *     sampling -- see below -- so that capability is currently unused here.)
 *
 *  3. The counters WRAP at 2^32; they do not saturate, and there is no
 *     interrupt on overflow.  See "WRAP HANDLING" and "SAMPLING" below.
 *
 * WRAP HANDLING.  perf wants a 64-bit monotone count.  The hardware gives a
 * 32-bit free-running one.  jcore_pmu_update() implements the reader protocol
 * from the spec's section 3.1: total += (u32)(cur - prev).  That is exact for
 * any true delta below 2^32 and WRONG BY A MULTIPLE OF 2^32 above it -- and
 * silently so, which is precisely the defect the RTL half was written to fix
 * at 16 bits.  Nothing in perf guarantees a counting event is read often
 * enough, so this driver supplies the guarantee itself: while any event is
 * scheduled on a CPU, a pinned per-CPU hrtimer samples every implemented
 * counter once a second.  One second bounds the fastest counter (PMCYC, one
 * per cycle) below 2^32 for any core clock under 4.29 GHz, so no frequency
 * knowledge is needed.  The residual failure mode is honest and narrow: if
 * that timer is starved for longer than the wrap period -- a very long
 * irq-disabled section, or suspend -- counts are lost in units of 2^32.  PMOVF
 * cannot repair this (one sticky bit cannot distinguish one wrap from five)
 * but it can report it, so the poll uses it to warn rather than to correct.
 *
 * SAMPLING.  There is NO PMU interrupt.  The RTL half left that to SoC level:
 * it needs an AIC2 source line, which is not a core-level change.  PMOVF is a
 * polled flag.  register_sh_pmu() therefore sets PERF_PMU_CAP_NO_INTERRUPT for
 * every sh PMU, and perf_event_open() rejects any sampling event on such a PMU
 * with -EOPNOTSUPP.  Stated plainly: `perf stat` works, `perf record` and
 * `perf top` on these hardware events do not, and will not until an overflow
 * interrupt exists.
 */
#define pr_fmt(fmt)	"jcore-pmu: " fmt

#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/percpu.h>
#include <linux/perf_event.h>
#include <linux/sched/clock.h>
#include <asm/processor.h>
#include <cpu/perf_event.h>

/*
 * Poll period.  See "WRAP HANDLING" above for why one second is the right
 * order of magnitude and why it does not depend on the core clock.  Typed
 * 64-bit on purpose: this is a 32-bit target, and 2 * NSEC_PER_SEC does not
 * fit in the `long` that constant carries.
 */
#define JCORE_PMU_POLL_NS	((u64)NSEC_PER_SEC)

/* Every counter this driver maps.  All eight, today. */
#define JCORE_PMU_WANTED_CNTS	GENMASK(JCORE_PMU_NR_COUNTERS - 1, 0)

/*
 * The arch layer indexes cpu_hw_events::events[] with the slot number it
 * hands us, so num_events must not exceed MAX_HWEVENTS or sh_pmu_add() walks
 * off the end of that array.  Checked here rather than trusted to the
 * WARN_ON() in register_sh_pmu(), which fires after the damage is possible.
 */
static_assert(JCORE_PMU_NR_COUNTERS <= MAX_HWEVENTS);

/* PMIDR[7:0] as read at probe time. */
static u32 jcore_pmu_cnts_mask __read_mostly;

/**
 * struct jcore_pmu_cpu - per-CPU software shadow of one PMU page
 * @total:	64-bit accumulation of each counter, extended past its wrap
 * @prev:	raw value of each counter at its last sample
 * @primed:	bit n set once @prev[n] holds a real sample
 * @active:	bit i set while slot i is scheduled, used to run the poll timer
 * @slot_cnt:	slot -> counter map, see consequence 1 in the file comment
 * @last_ns:	local_clock() at the last full (all-counter) sample
 * @poll:	anti-wrap timer, see "WRAP HANDLING" in the file comment
 *
 * The PMU page is per-CPU in hardware, so all of this is per-CPU too.  It is
 * only ever touched on the CPU it describes, from ->read()/->enable()/
 * ->disable() (which perf invokes on the event's own CPU) and from @poll
 * (pinned).  local_irq_save() around each update is therefore enough; no lock
 * is needed and none would help, since the hardware read and the shadow
 * update must be atomic with respect to each other and nothing else.
 */
struct jcore_pmu_cpu {
	u64		total[JCORE_PMU_NR_COUNTERS];
	u32		prev[JCORE_PMU_NR_COUNTERS];
	unsigned long	primed;
	unsigned long	active;
	u8		slot_cnt[MAX_HWEVENTS];
	u64		last_ns;
	struct hrtimer	poll;
};

static DEFINE_PER_CPU(struct jcore_pmu_cpu, jcore_pmu_cpu);

static inline u32 jcore_pmu_readl(unsigned long reg)
{
	return __raw_readl((void __iomem *)reg);
}

static inline void jcore_pmu_writel(u32 val, unsigned long reg)
{
	__raw_writel(val, (void __iomem *)reg);
}

/*
 * Fold one counter's hardware value into its 64-bit shadow and return the
 * shadow.  Caller holds off interrupts on the counter's own CPU.
 *
 * The subtraction is deliberately done in u32 so that it wraps modulo 2^32:
 * that IS the reconstruction, and it is exact while the true delta stays
 * below 2^32.  The first sample of a counter only primes @prev -- the
 * counters have been free-running since reset, so folding that first raw
 * value in would add an arbitrary constant to @total.  It would in fact be
 * harmless (every consumer of @total takes a difference) but it would make
 * the shadow mean nothing on its own, and a debugger reading it should not
 * have to know that.
 */
static u64 jcore_pmu_update(struct jcore_pmu_cpu *pc, unsigned int n)
{
	u32 cur = jcore_pmu_readl(JCORE_PMCNT(n));

	if (likely(test_bit(n, &pc->primed)))
		pc->total[n] += (u32)(cur - pc->prev[n]);
	else
		__set_bit(n, &pc->primed);

	pc->prev[n] = cur;

	return pc->total[n];
}

/*
 * Sample every counter and service PMOVF.  Caller holds off interrupts.
 *
 * PMOVF is read and cleared HERE AND NOWHERE ELSE.  It is a single register
 * covering all eight counters, so a partial reader would clear wrap flags
 * belonging to counters it is not updating and destroy the evidence.  The
 * flags are not used to correct @total: sticky means "at least one wrap", and
 * the modular difference above already handles exactly the one-wrap case.
 * What they can do -- the one question a wrapping counter cannot answer by
 * itself -- is say "you sampled too slowly", and that is what they are used
 * for.  A wrap seen at a poll that ran on schedule is expected (PMCYC wraps
 * every 10.7 s at 400 MHz) and silent; a wrap seen after the poll was starved
 * for longer than the interval we promised means counts were lost.
 */
static void jcore_pmu_sample_all(struct jcore_pmu_cpu *pc)
{
	u64 now = local_clock();
	unsigned int n;
	u32 ovf;

	ovf = jcore_pmu_readl(JCORE_PMOVF) & jcore_pmu_cnts_mask;
	if (ovf)
		jcore_pmu_writel(ovf, JCORE_PMOVF);	/* write-1-to-clear */

	for (n = 0; n < JCORE_PMU_NR_COUNTERS; n++)
		jcore_pmu_update(pc, n);

	if (ovf && pc->last_ns && now - pc->last_ns > 2 * JCORE_PMU_POLL_NS)
		pr_warn_ratelimited("counters %#x wrapped over a %llu ms gap; counts short by a multiple of 2^32\n",
				    ovf, div_u64(now - pc->last_ns, NSEC_PER_MSEC));

	pc->last_ns = now;
}

static enum hrtimer_restart jcore_pmu_poll(struct hrtimer *hrt)
{
	struct jcore_pmu_cpu *pc = container_of(hrt, struct jcore_pmu_cpu, poll);
	unsigned long flags;

	local_irq_save(flags);
	jcore_pmu_sample_all(pc);
	local_irq_restore(flags);

	/*
	 * Self-terminating rather than cancelled from ->disable(): perf calls
	 * ->disable() with interrupts already off and, on the timer's own CPU,
	 * hrtimer_cancel() there would be waiting for a callback that cannot
	 * run.  Re-arming only while a slot is live keeps an idle CPU idle.
	 */
	if (!READ_ONCE(pc->active))
		return HRTIMER_NORESTART;

	hrtimer_forward_now(hrt, ns_to_ktime(JCORE_PMU_POLL_NS));

	return HRTIMER_RESTART;
}

/*
 * Generic hardware events.
 *
 * The value is the fixed-function counter number.  -1 means "this hardware
 * cannot count that", following the sh4 template's discipline: the arch layer
 * turns it into -EINVAL, which is a better answer than a number that looks
 * like a measurement.
 *
 * PERF_COUNT_HW_INSTRUCTIONS is the one judgement call.  PMINS counts
 * instruction DISPATCHES; this core has no retirement stage to count instead,
 * and an instruction restarted by a precise exception (TLB fault, P4
 * privilege refusal) is dispatched -- and counted -- again.  Exposing it as
 * INSTRUCTIONS anyway, because: on fault-free code dispatch and retirement
 * coincide exactly (asserted by the RTL guard sim/tests/pmucnt.S check 0x10);
 * the divergence is bounded by the restart count, which PMWLK makes visible;
 * and refusing the event would leave IPC -- the measurement this whole task
 * exists to unblock -- unobtainable.  The number is an UPPER BOUND on retired
 * instructions, so IPC read next to a large PMWLK is optimistic.
 *
 * The refusals are as deliberate as the mappings:
 *
 *  CACHE_REFERENCES/CACHE_MISSES  There are two reference streams (I-fetch and
 *	data) and no counter that sums them, so either choice would be a
 *	sidedness lie; the I-side is available honestly under PERF_TYPE_HW_CACHE
 *	below.  There is no miss counter at all -- the L1s sit outside `entity
 *	cpu` in the RTL -- and PMIFW/PMDAW are wait CYCLES, which are a miss
 *	cost, not a miss count.  Mapping those here would report a plausible,
 *	wrong number.
 *  BRANCH_INSTRUCTIONS/BRANCH_MISSES  No branch counter, no predictor.
 *  BUS_CYCLES  No bus-clock counter.  PMIFW/PMDAW count core cycles spent
 *	waiting, which is a different quantity.
 *  REF_CPU_CYCLES  PMCYC is the core clock.  On a fixed-frequency part it
 *	would equal a reference clock, but that is a claim about the SoC, not
 *	about anything this driver can see.
 *
 * STALLED_CYCLES_FRONTEND/BACKEND do map, with a caveat that belongs in the
 * number: PMIFW and PMDAW count only cycles lost to an unacked bus access.
 * On this in-order core the instruction-fetch port IS the frontend, so
 * FRONTEND is very nearly complete; BACKEND is a strict subset -- register
 * interlocks, the multiplier and the shifter stall the pipe without a bus
 * access outstanding and are invisible here.  Both therefore UNDER-report,
 * which is the safe direction and is the opposite of the misses mistake
 * above: they count the right kind of thing, just not all of it.
 */
static const int jcore_general_events[] = {
	[PERF_COUNT_HW_CPU_CYCLES]		= JCORE_PMU_CYC,
	[PERF_COUNT_HW_INSTRUCTIONS]		= JCORE_PMU_INS,
	[PERF_COUNT_HW_CACHE_REFERENCES]	= -1,
	[PERF_COUNT_HW_CACHE_MISSES]		= -1,
	[PERF_COUNT_HW_BRANCH_INSTRUCTIONS]	= -1,
	[PERF_COUNT_HW_BRANCH_MISSES]		= -1,
	[PERF_COUNT_HW_BUS_CYCLES]		= -1,
	[PERF_COUNT_HW_STALLED_CYCLES_FRONTEND]	= JCORE_PMU_IFW,
	[PERF_COUNT_HW_STALLED_CYCLES_BACKEND]	= JCORE_PMU_DAW,
	[PERF_COUNT_HW_REF_CPU_CYCLES]		= -1,
};

#define C(x)	PERF_COUNT_HW_CACHE_##x

/*
 * Cache events.
 *
 * Only one entry is real.  With the L1s present the CPU's fetch port goes to
 * the I-cache, so PMIFR is exactly the L1I reference stream; without a cache
 * in the SoC it is the same traffic seen at the bus, which is the same number
 * for this purpose.  Note it must not be counter 0: the arch layer reads a 0
 * in this table as "unsupported" (-EOPNOTSUPP), so counter index 0 cannot be
 * expressed here at all.  PMCYC is not a cache event, so nothing is lost, but
 * a future table edit must not forget it.
 *
 * What is refused, and why, since each of these is a plausible mistake:
 *
 *  L1D accesses  PMDAR counts completed data accesses WITHOUT a direction
 *	bit -- reads and writes together.  perf's L1D encoding has no
 *	direction-agnostic slot, so filing it under OP_READ would over-report
 *	reads by the write count and filing it under both would report it
 *	twice.  It is reachable exactly and unambiguously as raw event 4.
 *  Any RESULT_MISS  There are no miss counters in this RTL (spec section 8.2:
 *	the L1s are instantiated above `entity cpu`, so the pulse would have to
 *	enter the CPU as a new input port that every SoC top must drive).
 *	PMIFW/PMDAW are wait cycles and are not miss counts.
 *  DTLB/ITLB misses  There is one TSB walker and it serves both sides;
 *	PMWLK is armed for I-side and D-side misses alike (jcore-cpu
 *	core/cpu.vhd, walk_i_miss / walk_d_miss into one tlb_walk instance).
 *	Filing a combined figure under either side would be wrong.  The pair is
 *	available as raw events 6 (walks) and 7 (walks that hit), and
 *	6 - 7 is the number of TLB misses that reached software.
 *  LL, NODE, BPU  No L2, no interconnect counters, no branch predictor.
 */
static const int jcore_cache_events
			[PERF_COUNT_HW_CACHE_MAX]
			[PERF_COUNT_HW_CACHE_OP_MAX]
			[PERF_COUNT_HW_CACHE_RESULT_MAX] =
{
	[C(L1D)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
	},

	[C(L1I)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= JCORE_PMU_IFR,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
	},

	[C(LL)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
	},

	[C(DTLB)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
	},

	[C(ITLB)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
	},

	[C(BPU)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
	},

	[C(NODE)] = {
		[C(OP_READ)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_WRITE)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
		[C(OP_PREFETCH)] = {
			[C(RESULT_ACCESS)]	= -1,
			[C(RESULT_MISS)]	= -1,
		},
	},
};

static int jcore_pmu_event_map(int event)
{
	return jcore_general_events[event];
}

/*
 * Bind slot -> counter.  Called from both ->enable() and ->disable() because
 * the arch layer reaches ->read() from either side of a start/stop: sh_pmu_add
 * calls ->disable() before the first ->enable(), and sh_pmu_stop() calls
 * ->read() after ->disable().  The map must be valid at both points.
 */
static void jcore_pmu_bind(struct jcore_pmu_cpu *pc, struct hw_perf_event *hwc,
			   int idx)
{
	if (WARN_ON_ONCE(hwc->config >= JCORE_PMU_NR_COUNTERS))
		return;

	pc->slot_cnt[idx] = hwc->config;
}

static u64 jcore_pmu_read(int idx)
{
	struct jcore_pmu_cpu *pc = this_cpu_ptr(&jcore_pmu_cpu);
	unsigned long flags;
	u64 val;

	local_irq_save(flags);
	val = jcore_pmu_update(pc, pc->slot_cnt[idx]);
	local_irq_restore(flags);

	return val;
}

/*
 * There is nothing to switch off in a fixed-function counter, and switching
 * off the whole block here would stop the seven counters other events are
 * using.  Dropping the slot from ->active is the whole job; it lets the poll
 * timer retire once the last event on this CPU goes away.
 */
static void jcore_pmu_disable(struct hw_perf_event *hwc, int idx)
{
	struct jcore_pmu_cpu *pc = this_cpu_ptr(&jcore_pmu_cpu);
	unsigned long flags;

	local_irq_save(flags);
	jcore_pmu_bind(pc, hwc, idx);
	__clear_bit(idx, &pc->active);
	local_irq_restore(flags);
}

/*
 * Start counting for this event.  The counter itself is already running and
 * is shared, so "start" means: take the counter's current value as this
 * event's origin.  That is also what makes ->disable()/->enable() around a
 * context switch discard the counts that belonged to somebody else, without
 * ever writing hardware.
 */
static void jcore_pmu_enable(struct hw_perf_event *hwc, int idx)
{
	struct jcore_pmu_cpu *pc = this_cpu_ptr(&jcore_pmu_cpu);
	unsigned long flags;

	local_irq_save(flags);

	jcore_pmu_bind(pc, hwc, idx);
	local64_set(&hwc->prev_count, jcore_pmu_update(pc, pc->slot_cnt[idx]));
	__set_bit(idx, &pc->active);

	if (!hrtimer_active(&pc->poll))
		hrtimer_start(&pc->poll, ns_to_ktime(JCORE_PMU_POLL_NS),
			      HRTIMER_MODE_REL_PINNED_HARD);

	local_irq_restore(flags);
}

/*
 * PMCR.EN is the only global control the block has, and the spec is explicit
 * that its purpose is to FREEZE, not to arm: it resets SET and the counters
 * free-run out of reset.  Using it for perf's pmu_disable/pmu_enable bracket
 * is what makes those callbacks mean what perf's contract says they mean --
 * all eight counters stop together, so a transaction sees a coherent set of
 * values -- and it keeps perf's own add/del work out of the measurement.
 *
 * Two consequences, neither hidden:
 *
 *  - Cycles elapsed inside the bracket are not counted.  The bracket is a
 *    couple of P4 accesses, served inside the datapath rather than on the bus;
 *    it is entered on the order of a context switch, so the loss is a tiny
 *    fraction of PMCYC.  It applies to every counter equally, so every ratio
 *    (IPC, hit rate, stall share) is unaffected -- only absolute cycle counts
 *    are very slightly short.
 *  - Counters 6 and 7 back the architected P4_TSBCNT alias at 0xFF000054, so
 *    freezing them freezes that register too.  Nothing in arch/sh reads it
 *    today, but a hypervisor virtualizing TSBCNT must virtualize this page
 *    with it, exactly as the spec says.
 */
static void jcore_pmu_disable_all(void)
{
	jcore_pmu_writel(0, JCORE_PMCR);
}

static void jcore_pmu_enable_all(void)
{
	jcore_pmu_writel(JCORE_PMCR_EN, JCORE_PMCR);
}

static struct sh_pmu jcore_pmu = {
	.name		= "jcore",
	/*
	 * Not a count of hardware counters -- it is the number of perf events
	 * the arch layer may schedule on one CPU at a time.  Fixed-function
	 * counters are shareable (see the file comment), so this is a driver
	 * choice; eight lets every event this driver knows about be counted
	 * simultaneously, which is the whole reason the RTL is fixed-function
	 * rather than multiplexed.
	 */
	.num_events	= JCORE_PMU_NR_COUNTERS,
	.event_map	= jcore_pmu_event_map,
	.max_events	= ARRAY_SIZE(jcore_general_events),
	/*
	 * A raw event is just a counter number.  The arch layer masks without
	 * validating, so `-e r9` silently becomes r1 rather than failing; that
	 * is the sh4 template's behaviour too and is not worth an arch change.
	 */
	.raw_event_mask	= JCORE_PMU_NR_COUNTERS - 1,
	.cache_events	= &jcore_cache_events,
	.read		= jcore_pmu_read,
	.disable	= jcore_pmu_disable,
	.enable		= jcore_pmu_enable,
	.disable_all	= jcore_pmu_disable_all,
	.enable_all	= jcore_pmu_enable_all,
};

static int __init jcore_pmu_init(void)
{
	u32 idr, width, cnts;
	unsigned int cpu;

	if (boot_cpu_data.type != CPU_JCORE)
		return -ENODEV;

	/*
	 * PMIDR's magic is the presence test, and it is doing real work rather
	 * than being belt and braces.  The spec says an absent PMU reads as a
	 * hard zero, which is true of a build of THIS RTL with PRIV_ARCH
	 * false -- but not of a bitstream predating the block.  There, the P4
	 * decode matched on VA[7:0] alone across the whole 16 MB P4 window, so
	 * 0xFF001008 lands on the MMU page's TTB at offset 0x08 and reads
	 * whatever software last put there.  A zero test would misfire; the
	 * magic will not.
	 */
	idr = jcore_pmu_readl(JCORE_PMIDR);
	if ((idr >> JCORE_PMIDR_MAGIC_SHIFT) != JCORE_PMIDR_MAGIC) {
		pr_info("no hardware PMU (PMIDR %#010x), software events only\n",
			idr);
		return -ENODEV;
	}

	width = (idr >> JCORE_PMIDR_WIDTH_SHIFT) & JCORE_PMIDR_WIDTH_MASK;
	cnts = idr & JCORE_PMIDR_CNTS_MASK;

	/*
	 * Both checks refuse rather than adapt, and refusing is the point of
	 * having the ID register at all: it is what lets this driver say
	 * "counter absent" instead of reporting the hard zero an undecoded P4
	 * offset returns, which would look exactly like a real measurement of
	 * nothing.
	 *
	 * The width is load-bearing -- jcore_pmu_update() reconstructs across
	 * the wrap in u32 arithmetic and would be wrong for any other width.
	 *
	 * The mask is checked all-or-nothing because this driver maps all
	 * eight counters and the tables it maps them through are const.  The
	 * mask's designed future is to WIDEN (spec section 8.2: when L1 miss
	 * counters arrive, an older driver sees them reported absent rather
	 * than reading zero), and extra bits are ignored here, so a subset
	 * build is the case that does not exist yet.  If one ever ships, the
	 * fix is per-event filtering, not a silently degraded PMU.
	 */
	if (width != 32) {
		pr_warn("PMIDR reports %u-bit counters, driver assumes 32\n",
			width);
		return -ENODEV;
	}

	if ((cnts & JCORE_PMU_WANTED_CNTS) != JCORE_PMU_WANTED_CNTS) {
		pr_warn("PMIDR implements counters %#x, driver needs %#lx\n",
			cnts, (unsigned long)JCORE_PMU_WANTED_CNTS);
		return -ENODEV;
	}

	jcore_pmu_cnts_mask = cnts;

	for_each_possible_cpu(cpu) {
		struct jcore_pmu_cpu *pc = &per_cpu(jcore_pmu_cpu, cpu);

		hrtimer_setup(&pc->poll, jcore_pmu_poll, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL_PINNED_HARD);
	}

	/*
	 * Clear any wrap flags accumulated since reset, so that the first one
	 * the poll reports is one this driver could have prevented.
	 */
	jcore_pmu_writel(JCORE_PMIDR_CNTS_MASK, JCORE_PMOVF);

	return register_sh_pmu(&jcore_pmu);
}
early_initcall(jcore_pmu_init);
