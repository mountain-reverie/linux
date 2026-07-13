// SPDX-License-Identifier: GPL-2.0
/*
 * arch/sh/kernel/cpu/jcore/setup.c
 *
 * J-Core J4 subtype setup.
 *
 * J4 boards are entirely device-tree described (CONFIG_SH_DEVICE_TREE,
 * arch/sh/boards/of-generic.c drives plat_early_device_setup()/timer/irq
 * probing from DT), so -- like CPU_SUBTYPE_J2 (arch/sh/kernel/cpu/sh2/,
 * which also has no setup-*.c of its own) -- there is no board-specific
 * static device table to register here. This file exists as the home
 * for J4-specific subtype init that SP2/SP3 SoC bring-up will add
 * (e.g. AIC/PIT wiring beyond what of-generic.c already provides); for
 * SP1 the generic weak plat_early_device_setup() in
 * arch/sh/kernel/setup.c is sufficient.
 */
#include <linux/init.h>
