// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2004 - 2007  Paul Mundt
 */
#include <linux/mm.h>
#include <linux/dma-map-ops.h>
#include <asm/cacheflush.h>
#include <asm/addrspace.h>

void arch_dma_prep_coherent(struct page *page, size_t size)
{
	__flush_purge_region(page_address(page), size);
}

void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
		enum dma_data_direction dir)
{
	void *addr = sh_cacheop_vaddr(phys_to_virt(paddr));

	switch (dir) {
	case DMA_FROM_DEVICE:		/* invalidate only */
		__flush_invalidate_region(addr, size);
		break;
	case DMA_TO_DEVICE:		/* writeback only */
		__flush_wback_region(addr, size);
		break;
	case DMA_BIDIRECTIONAL:		/* writeback and invalidate */
		__flush_purge_region(addr, size);
		break;
	default:
		BUG();
	}
}

void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
		enum dma_data_direction dir)
{
	void *addr = sh_cacheop_vaddr(phys_to_virt(paddr));

	switch (dir) {
	case DMA_TO_DEVICE:
		/*
		 * The device only read; nothing the CPU holds went stale.
		 */
		break;
	case DMA_FROM_DEVICE:
	case DMA_BIDIRECTIONAL:
		/*
		 * The device wrote into the buffer behind the caches, so any
		 * line the CPU still holds for it is stale and must go before
		 * the CPU reads it back.
		 */
		__flush_invalidate_region(addr, size);
		break;
	default:
		BUG();
	}
}
