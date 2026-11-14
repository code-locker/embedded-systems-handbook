#!/usr/bin/env bash
# 10_overlay_and_compositor.sh — draw on video and combine two videos.
#
#   videotestsrc ─► timeoverlay ─────────────────► compositor ─► videoconvert ─► fakesink
#   videotestsrc pattern=ball ─► [160x120] ──────► (sink_1 at 10,10)
#
# timeoverlay  : burns the running time into the picture (plugins-base, pango).
# compositor   : mixes several videos into one (picture-in-picture, grids).
#                Its sink pads are REQUEST pads named sink_%u; per-pad
#                properties (xpos, ypos, alpha, zorder) are set with the
#                syntax  compositor name=c sink_1::xpos=10 .
# Both sources must END for the compositor to send EOS, so both have
# num-buffers.
set -euo pipefail
SINK=${SINK:-fakesink sync=false}
gst-launch-1.0 compositor name=c sink_1::xpos=10 sink_1::ypos=10 \
  ! videoconvert ! $SINK \
  videotestsrc num-buffers=60 ! video/x-raw,width=640,height=480 ! timeoverlay ! c. \
  videotestsrc num-buffers=60 pattern=ball ! video/x-raw,width=160,height=120 ! c.
