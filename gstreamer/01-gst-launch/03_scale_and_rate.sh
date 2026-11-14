#!/usr/bin/env bash
# 03_scale_and_rate.sh — change resolution and frame rate.
#
#   videotestsrc ─► [1280x720@30] ─► videoscale ─► videorate ─► [320x240@10] ─► fakesink
#
# videoscale   : resizes frames. It changes width/height so the caps after it
#                can differ from the caps before it.
# videorate    : drops or duplicates frames so the output has the requested
#                framerate (30 → 10 fps: keeps 1 frame in 3).
# The second caps filter is the REQUEST to the converters: "give me 320x240
# at 10 fps". Negotiation makes videoscale and videorate do the work.
# 90 frames at 30 fps = 3 s of video → 30 frames at 10 fps reach the sink.
set -euo pipefail
SINK=${SINK:-fakesink sync=false}
gst-launch-1.0 videotestsrc num-buffers=90 \
  ! video/x-raw,width=1280,height=720,framerate=30/1 \
  ! videoscale ! videorate \
  ! video/x-raw,width=320,height=240,framerate=10/1 \
  ! $SINK
