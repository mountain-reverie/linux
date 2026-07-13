# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux/SuperH SH-2 backends.
#

# J-Core J4 (CPU_JCORE) provides its own cpu_probe() in
# arch/sh/kernel/cpu/jcore/probe.c, which duplicates this one; building
# both leads to a duplicate symbol at link time. ex.o/entry.o are NOT
# duplicated: jcore's ex.S emits jcore_vbr_base (distinct from this
# ex.o's vbr_base) reusing sh2/entry.S's exception_handler dispatch,
# ret_from_fork, address_error_trap_handler, etc., so sh2/entry.o must
# still be built for CPU_JCORE.
ifeq ($(CONFIG_CPU_JCORE),y)
obj-y	:= ex.o entry.o
else
obj-y	:= ex.o probe.o entry.o
endif

obj-$(CONFIG_CPU_SUBTYPE_SH7619) += setup-sh7619.o clock-sh7619.o

# SMP setup
smp-$(CONFIG_CPU_J2)			:= smp-j2.o
obj-$(CONFIG_SMP)			+= $(smp-y)
