# SPDX-License-Identifier: GPL-2.0
#
# Makefile — out-of-tree kernel modules for lesson 05-sysfs-procfs-debugfs
#
# This one file has two personalities:
#   1. Run by YOU (make -C .../kernel): KERNELRELEASE is empty, so the "else"
#      part runs and calls the kernel's own build system (Kbuild) with M=here.
#   2. Run by Kbuild (inside $(KDIR)): KERNELRELEASE is set, so only the
#      obj-m lines are read — they say which .ko files to build.
#
# Host (this PC, the running kernel's headers):
#   make
# Board (BeagleBone AI-64, a configured+built 6.18.x-k3 kernel tree):
#   make KDIR=<board kernel tree> ARCH=arm64 CROSS_COMPILE=<toolchain prefix>
#   (use the SAME compiler that built that kernel, or vermagic/flags differ)
# Extra compiler warnings:
#   make W=1
#
# Modules land next to the sources (Kbuild's rule for M= builds);
# `make clean` removes every generated file.

ifneq ($(KERNELRELEASE),)
# ---- Kbuild part ------------------------------------------------------------
obj-m := sysfs_demo.o procfs_demo.o debugfs_demo.o

# (No *_bug modules in this lesson; BUGS=1 is accepted and ignored.)

else
# ---- normal make part -------------------------------------------------------
KDIR ?= /lib/modules/$(shell uname -r)/build
BUGS ?= 0

# Ubuntu builds its kernels with a specific gcc (e.g. gcc-12 for 6.8) and its
# headers pass flags an older default gcc may not know. For a HOST build use
# the compiler named in the kernel's config when it is installed.
ifeq ($(origin CC),default)
ifeq ($(CROSS_COMPILE),)
KCC := $(shell sed -n 's/^CONFIG_CC_VERSION_TEXT="\([^ ]*\) .*/\1/p' $(KDIR)/.config 2>/dev/null)
KCC_BIN := $(firstword $(shell command -v $(KCC) $(patsubst x86_64-linux-gnu-%,%,$(KCC)) 2>/dev/null))
ifneq ($(KCC_BIN),)
KCC_ARG := CC=$(notdir $(KCC_BIN))
endif
endif
endif

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) BUGS=$(BUGS) $(KCC_ARG) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all modules clean
endif
