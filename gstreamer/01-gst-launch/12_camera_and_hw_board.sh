#!/usr/bin/env bash
# 12_camera_and_hw_board.sh — REAL HARDWARE pipelines (BeagleBone AI-64).
#
# NOT runnable on a PC without a camera. Read them; run them on the board.
# Each pipeline is guarded: it only runs if its elements and device exist.
#
# 1) USB camera preview / capture (plugins-good: v4l2src):
#    v4l2src device=/dev/video0 : captures from a V4L2 device (UVC webcam,
#                                 CSI camera via TI's ISP driver).
#    image/jpeg,... / video/x-raw,...: pick a mode the camera really offers
#                                 (list them: v4l2-ctl --list-formats-ext).
#    -e                         : Ctrl-C sends EOS so files are finalised.
#
# 2) Hardware H.264 encode via V4L2 mem2mem (v4l2h264enc, plugins-good):
#    only exists if the kernel exposes an encoder /dev/videoN. h264parse is
#    in plugins-bad. On TDA4VM the codec driver/firmware comes with TI's SDK
#    image; check with gst-inspect-1.0 v4l2h264enc on the board.
#
# 3) TI-specific elements (tiovxmultiscaler, tiovxdlcolorconvert,
#    tidlinferer ...) exist only on TI's Edge AI SDK image: see lesson 14.
set -uo pipefail
DEV=${DEV:-/dev/video0}
have() { gst-inspect-1.0 --exists "$1"; }

if [ -e "$DEV" ] && have v4l2src; then
  echo "--- 1) capture 100 frames from $DEV"
  gst-launch-1.0 -e v4l2src device="$DEV" num-buffers=100 ! videoconvert ! fakesink sync=false
else
  echo "skip 1: no $DEV"
fi

if have v4l2h264enc && have h264parse; then
  echo "--- 2) HW H.264 encode of a test pattern"
  gst-launch-1.0 -e videotestsrc num-buffers=300 ! video/x-raw,width=1280,height=720,format=NV12 \
    ! v4l2h264enc ! h264parse ! matroskamux ! filesink location=/tmp/hw.mkv
else
  echo "skip 2: v4l2h264enc (V4L2 encoder device) or h264parse (plugins-bad) missing"
fi
