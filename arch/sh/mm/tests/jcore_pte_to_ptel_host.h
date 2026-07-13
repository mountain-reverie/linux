/* Thin shim so the pure jcore_pte_to_ptel() logic can be compiled by the
 * host gcc (not the sh cross-compiler) with no other kernel headers. */
#ifndef __JCORE_PTE_TO_PTEL_HOST_H
#define __JCORE_PTE_TO_PTEL_HOST_H

#include "../../include/asm/pgtable-bits-jcore.h"

#endif /* __JCORE_PTE_TO_PTEL_HOST_H */
