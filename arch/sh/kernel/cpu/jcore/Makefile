# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux/SuperH J-Core J4 CPU-specific backend.
#

obj-y	:= ex.o entry.o exception.o probe.o setup.o mmu_enable.o

# Hardware performance counters (0xFF001000, jcore-cpu core/perf.vhd).
obj-$(CONFIG_PERF_EVENTS)	+= perf_event.o
