# SPDX-License-Identifier: GPL-2.0
#
# Makefile for SuperH-specific library files..
#

lib-y  = delay.o memmove.o memchr.o \
	 checksum.o strlen.o div64.o div64-generic.o

# Extracted from libgcc.
#
# NOTE: these must be lib-y (not obj-y): arch/sh/lib/ is listed in
# libs-y at the top level, and the top-level Makefile only pulls in
# each libs-y directory's lib.a into the vmlinux link (KBUILD_VMLINUX_OBJS
# uses "%/lib.a" for libs-y dirs, discarding that directory's built-in.a).
# Objects placed in obj-y here are built but never linked into vmlinux,
# leaving __ashlsi3_r0/__lshrsi3_r0/etc. undefined at final link on any
# CPU whose compiler emits calls to them (e.g. SH-2 lacking a barrel
# shifter, as hit by CPU_JCORE/J4).
lib-y += movmem.o ashlsi3.o ashrsi3.o ashiftrt.o lshrsi3.o udiv_qrnnd.o

udivsi3-y			:= udivsi3_i4i-Os.o

ifneq ($(CONFIG_CC_OPTIMIZE_FOR_SIZE),y)
udivsi3-$(CONFIG_CPU_SH3)	:= udivsi3_i4i.o
udivsi3-$(CONFIG_CPU_SH4)	:= udivsi3_i4i.o
endif
udivsi3-y			+= udivsi3.o

lib-y				+= io.o

memcpy-y			:= memcpy.o
memcpy-$(CONFIG_CPU_SH4)	:= memcpy-sh4.o

memset-y			:= memset.o
memset-$(CONFIG_CPU_SH4)	:= memset-sh4.o

lib-$(CONFIG_MMU)		+= copy_page.o __clear_user.o
lib-$(CONFIG_MCOUNT)		+= mcount.o
lib-y				+= $(memcpy-y) $(memset-y) $(udivsi3-y)
