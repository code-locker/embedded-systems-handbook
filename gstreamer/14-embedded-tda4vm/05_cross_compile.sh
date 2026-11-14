#!/usr/bin/env bash
# 05_cross_compile.sh — cross-compile one course program for the BeagleBone
# AI-64 (aarch64) on an x86-64 PC.
#
# You need (a) a cross compiler and (b) the board's GStreamer headers + .so
# files (a "sysroot"). Two ways to get (b):
#   1. Copy the board's filesystem: on the board
#        sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
#      then on the PC
#        rsync -a --relative board:/usr/include board:/usr/lib/aarch64-linux-gnu board:/lib/aarch64-linux-gnu ~/ai64-sysroot/
#      and run:  SYSROOT=~/ai64-sysroot bash 05_cross_compile.sh
#   2. Debian/Ubuntu multiarch on the PC:
#        sudo dpkg --add-architecture arm64 && sudo apt update
#        sudo apt install libgstreamer1.0-dev:arm64 libgstreamer-plugins-base1.0-dev:arm64
#      (Ubuntu needs ports.ubuntu.com in sources for arm64) and run with SYSROOT unset.
# Compiler:  sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
# Easiest of all: build natively ON the board (`make run P=...` works there).
# Run from the repository root:  bash gstreamer/14-embedded-tda4vm/05_cross_compile.sh [file.c]
#
# Status on the course PC: the compiler exists but no arm64 GStreamer dev
# files, so the script stops at the check below. UNVERIFIED end-to-end.
set -eu
SRC=${1:-gstreamer/02-first-pipeline/01_hello_pipeline.c}
OUT=build-aarch64/$(basename "${SRC%.*}")
CC=${CC:-aarch64-linux-gnu-gcc}
PKGS="gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-base-1.0"

command -v "$CC" >/dev/null || { echo "$CC not found: sudo apt install gcc-aarch64-linux-gnu"; exit 0; }

# pkg-config must look ONLY at arm64 .pc files, never the host's x86 ones:
#   PKG_CONFIG_LIBDIR replaces the default search path (PKG_CONFIG_PATH only adds)
#   PKG_CONFIG_SYSROOT_DIR prefixes -I/-L paths with the sysroot
if [ -n "${SYSROOT:-}" ]; then
    export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
    export PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig
    SYSFLAGS="--sysroot=$SYSROOT"
else
    export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
    SYSFLAGS=""
fi
if ! pkg-config --exists $PKGS; then
    echo "arm64 GStreamer .pc files not found in: $PKG_CONFIG_LIBDIR"
    echo "set SYSROOT=<board sysroot> or install the :arm64 dev packages (see header) — skipping"
    exit 0
fi

mkdir -p "$(dirname "$OUT")"
# No sanitizers: the board image usually lacks libasan for aarch64; -O2 for real speed.
"$CC" $SYSFLAGS -std=c11 -O2 -g -Wall -Wextra \
    $(pkg-config --cflags $PKGS) "$SRC" -o "$OUT" \
    $(pkg-config --libs $PKGS) -lm -lpthread
file "$OUT"
echo "copy and run:  scp $OUT debian@<board-ip>:~ && ssh debian@<board-ip> ./$(basename "$OUT")"
