/*
 * TLB flushing operations for SH with an MMU.
 *
 *  Copyright (C) 1999  Niibe Yutaka
 *  Copyright (C) 2003  Paul Mundt
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/mm.h>
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>

/*
 * Generic no-op default: only jcore has a software TSB that can alias
 * across a reused ASID generation nibble (security-review S-I3). jcore
 * overrides this in arch/sh/mm/tlb-jcore.c (CONFIG_CPU_JCORE only); every
 * other 32-bit SH MMU build links this weak default instead.
 */
void __weak jcore_tsb_flush_on_generation(unsigned long new_ctx)
{
}

void local_flush_tlb_page(struct vm_area_struct *vma, unsigned long page)
{
	unsigned int cpu = smp_processor_id();

	if (vma->vm_mm && cpu_context(cpu, vma->vm_mm) != NO_CONTEXT) {
		unsigned long flags;
		unsigned long asid;
		unsigned long saved_asid = MMU_NO_ASID;

		asid = cpu_asid(cpu, vma->vm_mm);
		page &= PAGE_MASK;

		local_irq_save(flags);
		if (vma->vm_mm != current->mm) {
			saved_asid = get_asid();
			set_asid(asid);
		}
		local_flush_tlb_one(asid, page);
		if (saved_asid != MMU_NO_ASID)
			set_asid(saved_asid);
		local_irq_restore(flags);
	}
}

void local_flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
			   unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned int cpu = smp_processor_id();

	if (cpu_context(cpu, mm) != NO_CONTEXT) {
		unsigned long flags;
		int size;

		local_irq_save(flags);
		size = (end - start + (PAGE_SIZE - 1)) >> PAGE_SHIFT;
		if (size > (MMU_NTLB_ENTRIES/4)) { /* Too many TLB to flush */
			cpu_context(cpu, mm) = NO_CONTEXT;
			/*
			 * active_mm, not mm: on the lazy-TLB path (kernel
			 * thread, kthread_use_mm(), the OOM reaper) this mm is
			 * still the one programmed into the ASID register even
			 * though it is not current->mm. Revoking the context
			 * without reprogramming would leave the CPU executing
			 * on an ASID we just orphaned -- and the way back in
			 * goes through switch_mm()'s prev == next arm, which
			 * skips activate_context() because the mm_cpumask bit
			 * is still set, so nothing downstream repairs it. For a
			 * user task active_mm == mm, so this only adds the lazy
			 * case.
			 */
			if (mm == current->active_mm)
				activate_context(mm, cpu);
		} else {
			unsigned long asid;
			unsigned long saved_asid = MMU_NO_ASID;

			asid = cpu_asid(cpu, mm);
			start &= PAGE_MASK;
			end += (PAGE_SIZE - 1);
			end &= PAGE_MASK;
			if (mm != current->mm) {
				saved_asid = get_asid();
				set_asid(asid);
			}
			while (start < end) {
				local_flush_tlb_one(asid, start);
				start += PAGE_SIZE;
			}
			if (saved_asid != MMU_NO_ASID)
				set_asid(saved_asid);
		}
		local_irq_restore(flags);
	}
}

void local_flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	unsigned int cpu = smp_processor_id();
	unsigned long flags;
	int size;

	local_irq_save(flags);
	size = (end - start + (PAGE_SIZE - 1)) >> PAGE_SHIFT;
	if (size > (MMU_NTLB_ENTRIES/4)) { /* Too many TLB to flush */
		local_flush_tlb_all();
	} else {
		unsigned long asid;
		unsigned long saved_asid = get_asid();

		asid = cpu_asid(cpu, &init_mm);
		start &= PAGE_MASK;
		end += (PAGE_SIZE - 1);
		end &= PAGE_MASK;
		set_asid(asid);
		while (start < end) {
			local_flush_tlb_one(asid, start);
			start += PAGE_SIZE;
		}
		set_asid(saved_asid);
	}
	local_irq_restore(flags);
}

void local_flush_tlb_mm(struct mm_struct *mm)
{
	unsigned int cpu = smp_processor_id();

	/* Invalidate all TLB of this process. */
	/* Instead of invalidating each TLB, we get new MMU context. */
	if (cpu_context(cpu, mm) != NO_CONTEXT) {
		unsigned long flags;

		local_irq_save(flags);
		cpu_context(cpu, mm) = NO_CONTEXT;
		/* active_mm, not mm -- see local_flush_tlb_range() above. */
		if (mm == current->active_mm)
			activate_context(mm, cpu);
		local_irq_restore(flags);
	}
}

void __flush_tlb_global(void)
{
	unsigned long flags;

	local_irq_save(flags);

	/*
	 * This is the most destructive of the TLB flushing options,
	 * and will tear down all of the UTLB/ITLB mappings, including
	 * wired entries.
	 */
	__raw_writel(__raw_readl(MMUCR) | MMUCR_TI, MMUCR);

	local_irq_restore(flags);
}
