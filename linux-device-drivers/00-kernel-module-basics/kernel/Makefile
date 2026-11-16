# SPDX-License-Identifier: GPL-2.0
#
# Makefile — out-of-tree kernel modules for lesson 00-kernel-module-basics
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
# `make clean` removes every generated file. (No *_bug modules here.)

ifneq ($(KERNELRELEASE),)
# ---- Kbuild part ------------------------------------------------------------
# obj-m: build as loadable module (.ko). In-tree code says obj-$(CONFIG_FOO)
# instead, which becomes obj-y (built in), obj-m (module) or nothing.
obj-m := kmb_hello.o kmb_lib.o kmb_consumer.o kmb_multi.o kmb_initfail.o

# kmb_multi.ko has no kmb_multi.c: it is linked from these two objects.
kmb_multi-y := kmb_multi_main.o kmb_multi_stats.o

# ccflags-y: extra compiler flags for EVERY file in this directory.
ccflags-y := -DKMB_HIST_BUCKETS=8

# CFLAGS_<object>: flags for ONE file. -DDEBUG turns its pr_debug() calls
# into always-on messages (no dynamic debug needed).
CFLAGS_kmb_multi_stats.o := -DDEBUG

else
# ---- normal make part -------------------------------------------------------
KDIR ?= /lib/modules/$(shell uname -r)/build

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
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KCC_ARG) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all modules clean
endif
