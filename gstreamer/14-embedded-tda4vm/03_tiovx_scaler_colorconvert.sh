#!/usr/bin/env bash
# 03_tiovx_scaler_colorconvert.sh — TI OpenVX-backed hardware elements from
# TI's Edge AI GStreamer plugins (edgeai-gst-plugins):
#   tiovxmultiscaler     : VPAC MSC — one input, several scaled outputs (request
#                          pads src_%u), no CPU pixel work
#   tiovxdlcolorconvert  : colour conversion (e.g. NV12 <-> RGB) offloaded from
#                          the A72 (TI implements it with TIOVX kernels)
#   tiovxisp             : VPAC VISS — raw Bayer sensor -> NV12 (needs the
#                          sensor's DCC tuning files)
#
# TI-SPECIFIC: these exist ONLY on TI's SDK images for J721E/TDA4VM (Edge AI
# SDK / Processor SDK Linux), not in upstream GStreamer or Debian.
# UNVERIFIED on the course PC. Property names DIFFER between SDK releases:
# run `gst-inspect-1.0 tiovxmultiscaler` (etc.) on YOUR board before editing.
# The script exits 0 with a message when the elements are missing.
set -eu
for e in tiovxmultiscaler tiovxdlcolorconvert; do
    if ! gst-inspect-1.0 --exists $e; then
        echo "$e not available (TI Edge AI SDK image required) — skipping"
        exit 0
    fi
done

# One 1080p NV12 stream -> two outputs scaled by the MSC hardware:
#   branch A: 1280x720 NV12 (e.g. for the encoder)
#   branch B: 640x360 NV12 -> RGB via tiovxdlcolorconvert (e.g. for a DL model)
#  videotestsrc num-buffers=100 : terminates by itself
#  tiovxmultiscaler name=msc    : request pads are created by "msc." links;
#                                 each output's size is set by the caps after it
#  queue                        : one thread per branch (lesson 08)
#  fakesink                     : discard; swap for kmssink/v4l2h264enc on the board
gst-launch-1.0 -v videotestsrc num-buffers=100 \
    ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 \
    ! tiovxmultiscaler name=msc \
    msc. ! queue ! video/x-raw,format=NV12,width=1280,height=720 ! fakesink sync=false \
    msc. ! queue ! video/x-raw,format=NV12,width=640,height=360 \
         ! tiovxdlcolorconvert ! video/x-raw,format=RGB ! fakesink sync=false

# Raw CSI sensor through the ISP (sketch — sensor name, DCC file paths and the
# Bayer format depend on the SDK and sensor; copy them from TI's example
# configs in /opt/edgeai-gst-apps on the SDK image):
#  gst-launch-1.0 v4l2src device=/dev/videoX io-mode=dmabuf-import \
#      ! video/x-bayer,width=1920,height=1080,format=rggb \
#      ! tiovxisp <sensor + dcc properties from gst-inspect / TI examples> \
#      ! video/x-raw,format=NV12 ! tiovxmultiscaler ! ... ! kmssink
