#!/usr/bin/env bash
# 02_hw_h264_encode.sh — H.264 encode/decode on the SoC's hardware video codec
# through the V4L2 memory-to-memory (M2M) elements v4l2h264enc / v4l2h264dec.
#
# Which codec IP? On TDA4VM/J721E, TI documents Imagination D5520 (decode) and
# VXE384 (encode) cores, driven by TI's V4L2 drivers in its SDK kernel. Newer
# TI SoCs (J721S2, J784S4, AM62A) use the Chips&Media Wave5 VPU, which has a
# mainline driver (drivers/media/platform/chips-media/wave5). Check your SoC's
# SDK documentation. GStreamer does not care: both appear as V4L2 M2M devices.
#
# REQUIRES: a kernel whose V4L2 codec driver is bound to the codec hardware, and
# GStreamer's video4linux2 plugin (plugins-good) — e.g. TI's Processor/Edge AI
# SDK image for J721E, or a recent BeagleBoard image with the codec enabled.
# h264parse comes from gst-plugins-bad (gstreamer1.0-plugins-bad).
# UNVERIFIED on the course PC (no codec hardware). Exits 0 with a message when the
# elements are missing.
#
# Notes:
#  * The element name depends on how many M2M devices exist; with several,
#    GStreamer names them v4l2videoNh264enc. List them:  gst-inspect-1.0 | grep v4l2
#  * Encoder controls (bitrate, GOP, profile) are exposed through the
#    extra-controls structure, e.g. extra-controls="controls,video_bitrate=4000000".
#    The available control names come from the driver: v4l2-ctl -d /dev/videoN -l
#  * The encoder wants NV12 (or similar) input: put the caps before it so a
#    CPU videoconvert is not silently needed.
set -eu
OUT=${1:-/tmp/hw_test.h264}
for e in v4l2h264enc h264parse; do
    if ! gst-inspect-1.0 --exists $e; then
        echo "$e not available here (needs the TI SDK image / V4L2 codec driver / plugins-bad) — skipping"
        exit 0
    fi
done

# Encode 300 frames of test video at 1080p to an elementary stream.
#  videotestsrc num-buffers=300   : 10 s at 30 fps, then EOS
#  video/x-raw,format=NV12,...    : the encoder's native input format
#  v4l2h264enc                    : hardware encode (V4L2 M2M)
#  h264parse                      : sets stream-format/alignment caps, SPS/PPS
#  filesink                       : write the .h264 file
gst-launch-1.0 -e videotestsrc num-buffers=300 \
    ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 \
    ! v4l2h264enc ! h264parse ! filesink location="$OUT"
ls -l "$OUT"

# Decode it again with the hardware decoder and count frames.
#  filesrc ! h264parse  : split the byte stream into access units
#  v4l2h264dec          : hardware decode, outputs NV12 (often dmabuf-backed)
#  fakesink -v          : print the decoder's output caps
if gst-inspect-1.0 --exists v4l2h264dec; then
    gst-launch-1.0 -v filesrc location="$OUT" ! h264parse ! v4l2h264dec \
        ! fakesink sync=false
fi

# Camera -> HW encoder -> RTP (see lesson 11 for the receiver):
#  gst-launch-1.0 v4l2src device=/dev/video0 ! video/x-raw,format=NV12,width=1280,height=720 \
#      ! v4l2h264enc ! h264parse config-interval=-1 ! rtph264pay pt=96 \
#      ! udpsink host=<PC-IP> port=5000
