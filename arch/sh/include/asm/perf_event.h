/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_SH_PERF_EVENT_H
#define __ASM_SH_PERF_EVENT_H

struct hw_perf_event;

/*
 * Upper bound on any registered sh_pmu's num_events, and the size of the
 * per-CPU slot arrays in arch/sh/kernel/perf_event.c.  sh_pmu_add() indexes
 * cpu_hw_events::events[] with a slot number drawn from [0, num_events), so
 * this MUST NOT be smaller than the largest num_events any backend declares
 * -- the WARN_ON() in register_sh_pmu() fires only after the overrun has
 * become possible.  Raised from 2 to 8 for the J-Core J4 fixed-function PMU,
 * whose eight counters all run simultaneously and are individually
 * shareable; the SH-4 backends still declare 2 and simply leave the rest of
 * the array unused.
 */
#define MAX_HWEVENTS	8

struct sh_pmu {
	const char	*name;
	unsigned int	num_events;
	void		(*disable_all)(void);
	void		(*enable_all)(void);
	void		(*enable)(struct hw_perf_event *, int);
	void		(*disable)(struct hw_perf_event *, int);
	u64		(*read)(int);
	int		(*event_map)(int);
	unsigned int	max_events;
	unsigned long	raw_event_mask;
	const int	(*cache_events)[PERF_COUNT_HW_CACHE_MAX]
				       [PERF_COUNT_HW_CACHE_OP_MAX]
				       [PERF_COUNT_HW_CACHE_RESULT_MAX];
};

/* arch/sh/kernel/perf_event.c */
extern int register_sh_pmu(struct sh_pmu *);
extern int reserve_pmc_hardware(void);
extern void release_pmc_hardware(void);

#endif /* __ASM_SH_PERF_EVENT_H */
