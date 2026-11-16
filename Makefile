# =============================================================================
#  Makefile — builds every example in this repo.
#
#  Rule of the repo: every .c / .cpp file is ONE standalone program.
#  It is compiled to build/<same path without extension>, e.g.
#      dsa/01-pointers/c/01_pointer_basics.c  ->  build/dsa/01-pointers/c/01_pointer_basics
#
#  Common commands:
#      make                 build all host examples: dsa/, linux-internals/, protocols/
#                           (debug + AddressSanitizer/UBSan)
#      make gst             build all GStreamer examples (needs gstreamer dev pkgs)
#      make arm             cross-compile arm-architecture/ for aarch64 (run on the board)
#      make drivers         build linux-device-drivers/ kernel modules (KDIR=... for the board)
#      make test            build + run every host example (each one self-checks)
#      make test-gst        build + run every GStreamer example (headless)
#      make run P=dsa/06-linked-lists/c/01_singly_linked_list.c
#                           build + run exactly one program
#      make SAN=0           build without sanitizers (use this with valgrind!)
#      make SAN=tsan        build with ThreadSanitizer instead of ASan/UBSan
#                           (data races, lock-order inversions); `make run` then
#                           runs the program with ASLR off, which TSan needs
#      make test-tsan       build + run every linux-internals/ program under TSan
#      make OPT=1           build optimised (-O2), e.g. for benchmarks
#      make clean
#
#  Files whose name ends in _bug.c / _bug.cpp contain INTENTIONAL bugs used
#  for debugging lessons. They are built, but `make test` does not run them.
# =============================================================================

# make has built-in defaults CC=cc / CXX=g++; use gcc unless the user sets one.
ifeq ($(origin CC),default)
  CC := gcc
endif
ifeq ($(origin CXX),default)
  CXX := g++
endif
BUILD   ?= build

CSTD    := -std=c11
CXXSTD  := -std=c++17
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
SAN     ?= 1
OPT     ?= 0

ifeq ($(OPT),1)
  DBG := -O2 -g
else
  DBG := -O0 -g3
endif

ifeq ($(SAN),1)
  # -fno-sanitize-recover: any UBSan report aborts the program, so `make test`
  # cannot pass while undefined behaviour is happening.
  SANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
endif

# ThreadSanitizer cannot be combined with ASan, so it is a separate mode.
# TSan needs a fixed address-space layout: with the 32-bit mmap randomisation
# of recent Ubuntu kernels it aborts with "unexpected memory mapping", so
# programs are started under `setarch -R` (ASLR off for that process only).
ifeq ($(SAN),tsan)
  SANFLAGS   := -fsanitize=thread -fno-omit-frame-pointer
  RUN_PREFIX := setarch $(shell uname -m) -R
endif

CFLAGS   += $(CSTD)   $(WARN) $(DBG) $(SANFLAGS)
CXXFLAGS += $(CXXSTD) $(WARN) $(DBG) $(SANFLAGS)
LDLIBS   += -lm -lpthread

# Rebuild everything when the compiler or flags change (e.g. SAN=0 after a
# sanitizer build). build/.flags holds the current flag set; it is rewritten
# only when that set differs, and every program depends on it.
FLAGS_ID    := $(CC) $(CXX) $(CFLAGS) $(CXXFLAGS) $(LDLIBS)
FLAGS_STAMP := $(BUILD)/.flags
$(shell mkdir -p $(BUILD); echo '$(FLAGS_ID)' | cmp -s - $(FLAGS_STAMP) || echo '$(FLAGS_ID)' > $(FLAGS_STAMP))

# ---- host programs: built and run on this PC ---------------------------------
HOST_DIRS := dsa linux-internals protocols
DSA_C    := $(shell find $(HOST_DIRS) -name '*.c'   2>/dev/null | sort)
DSA_CXX  := $(shell find $(HOST_DIRS) -name '*.cpp' 2>/dev/null | sort)
DSA_BINS := $(addprefix $(BUILD)/,$(DSA_C:.c=) $(DSA_CXX:.cpp=))
LI_BINS  := $(filter $(BUILD)/linux-internals/%,$(DSA_BINS))

# ---- ARM (aarch64) programs: cross-compiled, run on the BeagleBone AI-64 -----
CROSS     ?= aarch64-linux-gnu-
ARM_SRC   := $(shell find arm-architecture -name '*.c' -o -name '*.S' 2>/dev/null | sort)
ARM_BINS  := $(addprefix $(BUILD)/,$(basename $(ARM_SRC)))
ARM_FLAGS := -O1 -g -static -Wall -Wextra

# ---- Linux device drivers ----------------------------------------------------
#   <lesson>/kernel/  : out-of-tree module, built with Kbuild against KDIR
#   <lesson>/user/    : user-space test programs (need the module loaded to run)
KDIR      ?= /lib/modules/$(shell uname -r)/build
DRV_KDIRS := $(sort $(dir $(shell find linux-device-drivers -path '*/kernel/Makefile' 2>/dev/null)))
DRV_USER  := $(shell find linux-device-drivers -path '*/user/*.c' 2>/dev/null | sort)
DRV_UBINS := $(addprefix $(BUILD)/,$(DRV_USER:.c=))

# ---- GStreamer sources ------------------------------------------------------
GST_PKGS := gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-base-1.0
GST_C    := $(shell find gstreamer -name '*.c'   2>/dev/null | sort)
GST_CXX  := $(shell find gstreamer -name '*.cpp' 2>/dev/null | sort)
GST_BINS := $(addprefix $(BUILD)/,$(GST_C:.c=) $(GST_CXX:.cpp=))
GST_CFLAGS = $(shell pkg-config --cflags $(GST_PKGS))
GST_LIBS   = $(shell pkg-config --libs   $(GST_PKGS))

.PHONY: all dsa host internals gst arm drivers drivers-clean test test-gst test-tsan run clean list help

all: host
dsa host: $(DSA_BINS)
internals: $(LI_BINS)
gst: check-gst $(GST_BINS)
arm: $(ARM_BINS)

# ---- pattern rules ----------------------------------------------------------
define HOST_RULES
$(BUILD)/$(1)/%: $(1)/%.c $$(FLAGS_STAMP)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) $$< -o $$@ $$(LDLIBS)

$(BUILD)/$(1)/%: $(1)/%.cpp $$(FLAGS_STAMP)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) $$< -o $$@ $$(LDLIBS)
endef
$(foreach d,$(HOST_DIRS),$(eval $(call HOST_RULES,$(d))))

# aarch64: static binaries, so you can scp one file to the board and run it.
# Also writes a disassembly next to it (<prog>.dis) to study the instructions.
$(BUILD)/arm-architecture/%: arm-architecture/%.c
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(ARM_FLAGS) $< -o $@
	$(CROSS)objdump -d --no-show-raw-insn $@ > $@.dis

$(BUILD)/arm-architecture/%: arm-architecture/%.S
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(ARM_FLAGS) $< -o $@
	$(CROSS)objdump -d --no-show-raw-insn $@ > $@.dis

# Kernel modules for this PC by default; for the board:
#   make drivers KDIR=<board kernel build dir> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
drivers: $(DRV_UBINS)
	@for d in $(DRV_KDIRS); do $(MAKE) -C $$d KDIR=$(KDIR) || exit 1; done

drivers-clean:
	@for d in $(DRV_KDIRS); do $(MAKE) -C $$d KDIR=$(KDIR) clean; done

$(BUILD)/linux-device-drivers/%: linux-device-drivers/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

# GStreamer programs: ASan is fine, but GLib keeps some memory alive for the
# whole process, so leak detection is turned off when they run (see test-gst).
$(BUILD)/gstreamer/%: gstreamer/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GST_CFLAGS) $< -o $@ $(GST_LIBS) $(LDLIBS)

$(BUILD)/gstreamer/%: gstreamer/%.cpp $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GST_CFLAGS) $< -o $@ $(GST_LIBS) $(LDLIBS)

check-gst:
	@pkg-config --exists $(GST_PKGS) || { \
	  echo "GStreamer development packages not found. Install them with:"; \
	  echo "  sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \\"; \
	  echo "       gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-tools"; \
	  exit 1; }

# ---- running ----------------------------------------------------------------
test: host
	@./scripts/run-all.sh $(addprefix $(BUILD)/,$(HOST_DIRS))

test-gst: gst
	@ASAN_OPTIONS=detect_leaks=0 ./scripts/run-all.sh $(BUILD)/gstreamer

# Concurrency lessons again, under ThreadSanitizer, in their own build dir so
# the ASan build in build/ is not thrown away. halt_on_error: the first race
# report fails the program, so a racy lesson cannot pass.
test-tsan:
	@$(MAKE) --no-print-directory SAN=tsan BUILD=$(BUILD)/tsan internals
	@TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" \
	  setarch $(shell uname -m) -R ./scripts/run-all.sh $(BUILD)/tsan/linux-internals

# make run P=path/to/file.c  -> build and run that one program
run:
	@test -n "$(P)" || { echo "usage: make run P=dsa/01-pointers/c/01_pointer_basics.c"; exit 1; }
	@bin=$(BUILD)/$(basename $(P)); \
	case "$$bin" in /*) exe=$$bin ;; *) exe=./$$bin ;; esac; \
	case "$(P)" in \
	  gstreamer/*)        $(MAKE) --no-print-directory check-gst $$bin ;; \
	  arm-architecture/*) $(MAKE) --no-print-directory $$bin && \
	                      echo "built $$bin (aarch64): copy it to the board to run it; disassembly in $$bin.dis" && exit 0 ;; \
	  *)                  $(MAKE) --no-print-directory $$bin ;; esac && \
	echo "---- running $$exe ----" && $(RUN_PREFIX) $$exe

list:
	@printf '%s\n' $(DSA_C) $(DSA_CXX) $(GST_C) $(GST_CXX) $(ARM_SRC) $(DRV_USER) $(DRV_KDIRS)

clean:
	rm -rf $(BUILD)

help:
	@sed -n '2,30p' Makefile
