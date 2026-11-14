#!/usr/bin/env bash
# 01_v4l2_camera_preview.sh — capture from a V4L2 camera (USB UVC webcam, or a
# CSI camera exposed by the driver) on the BeagleBone AI-64 or any Linux PC.
#
# Elements: v4l2src, videoconvert, fakesink/autovideosink — core plugins-good/base,
# so this part runs on Debian images too (no TI SDK needed for UVC cameras).
# A raw CSI sensor (e.g. IMX219, Bayer output) needs the TI ISP element
# (tiovxisp, TI SDK image) — see 03_tiovx_scaler_colorconvert.sh.
#
# Usage: bash 01_v4l2_camera_preview.sh [/dev/videoN] [--display]
# UNVERIFIED on the course PC (no camera attached); exits 0 with a message
# when no camera is present.
set -eu
DEV=${1:-/dev/video0}
SINK="fakesink sync=false"
[ "${2:-}" = "--display" ] && SINK="autovideosink"   # on the board without X: kmssink

if [ ! -e "$DEV" ]; then
    echo "no camera at $DEV — plug a UVC camera or pass /dev/videoN (skipping)"
    exit 0
fi

# Step 1: ask the driver what it can do. Pick caps from this list instead of
# letting videoconvert/videoscale fix mismatches on the CPU.
if command -v v4l2-ctl >/dev/null; then          # apt install v4l-utils
    v4l2-ctl -d "$DEV" --list-formats-ext | head -30
fi

# Step 2: capture 100 frames, print negotiated caps (-v), stop by itself.
#  v4l2src device=...    : the capture device; io-mode=auto picks mmap/dmabuf
#                          (io-mode=dmabuf exports dmabufs for zero copy downstream)
#  num-buffers=100       : stop after 100 frames -> EOS -> gst-launch exits
#  video/x-raw,...       : request a mode the camera lists (YUY2 640x480@30 is
#                          supported by nearly every UVC camera)
#  videoconvert          : YUY2 -> whatever the sink wants (CPU! drop it when
#                          the consumer accepts the native format)
#  fakesink / autovideosink : discard or show
gst-launch-1.0 -v v4l2src device="$DEV" num-buffers=100 \
    ! video/x-raw,format=YUY2,width=640,height=480,framerate=30/1 \
    ! videoconvert ! $SINK

# Many UVC cameras only reach 30 fps at 720p/1080p in MJPEG:
#  gst-launch-1.0 v4l2src device=$DEV num-buffers=100 \
#      ! image/jpeg,width=1280,height=720,framerate=30/1 ! jpegdec ! videoconvert ! $SINK
# On the TI SDK image the JPEG decode can go to hardware if a V4L2 JPEG
# decoder is exposed (check: gst-inspect-1.0 | grep -i v4l2.*jpeg).
