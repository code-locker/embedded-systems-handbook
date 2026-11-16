# SPDX-License-Identifier: GPL-2.0
#
# Makefile — out-of-tree kernel modules for lesson 04-interrupts-timers-and-deferred-work
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
# Device-tree overlay for gpio_button_irq (needs a full kernel SOURCE tree for
# <dt-bindings/...> and the TI "k3-pinctrl.h" — i.e. the board tree):
#   make dtbo KDIR=<board kernel tree>
# Extra compiler warnings:
#   make W=1
#
# Modules land next to the sources (Kbuild's rule for M= builds);
# `make clean` removes every generated file, including *.dtbo.

ifneq ($(KERNELRELEASE),)
# ---- Kbuild part ------------------------------------------------------------
obj-m := irqsim_demo.o timers_demo.o gpio_button_irq.o

# This lesson has no intentionally broken module; BUGS=1 is accepted (and
# ignored) so `make drivers BUGS=1` works the same for every lesson.

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

# Overlays: run the C preprocessor first (for #include and macros such as
# GPIO_ACTIVE_LOW / J721E_IOPAD), then dtc. -@ keeps the labels so the
# overlay can be resolved against the base DTB (which the kernel builds with -@).
DTC ?= dtc
DTSO := $(wildcard *.dtso)
DTBO := $(DTSO:.dtso=.dtbo)
DTS_CPP_FLAGS := -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
		 -I $(KDIR)/include -I $(KDIR)/arch/arm64/boot/dts/ti

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) BUGS=$(BUGS) $(KCC_ARG) modules

dtbo: $(DTBO)

%.dtbo: %.dtso
	cpp $(DTS_CPP_FLAGS) $< | $(DTC) -@ -I dts -O dtb -o $@ -

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f *.dtbo

.PHONY: all modules dtbo clean
endif
