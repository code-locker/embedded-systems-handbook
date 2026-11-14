#!/usr/bin/env bash
# 02_caps_filter.sh — choose the format with a caps filter.
#
#   videotestsrc ──► [video/x-raw,...] ──► videoconvert ──► fakesink
#
# videotestsrc    : pattern=ball draws a bouncing ball. Enum properties take
#                   their nick ("ball") or number; nicks are clearer.
# video/x-raw,... : not an element name but a CAPS FILTER (shorthand for
#                   `capsfilter caps="..."`). It forces the link between the
#                   two neighbours to use exactly these caps: 640x480, I420,
#                   30 fps. Without it the elements pick a default.
# videoconvert    : converts pixel formats (I420 → whatever the sink wants).
#                   Free (passthrough) when formats already match.
# fakesink        : discards. sync=false: do not wait for the clock, run as
#                   fast as possible (100 frames finish in milliseconds).
#
# -v prints the negotiated caps of every pad: look for "caps = video/x-raw".
set -euo pipefail
SINK=${SINK:-fakesink sync=false}
gst-launch-1.0 -v videotestsrc pattern=ball num-buffers=100 \
  ! video/x-raw,format=I420,width=640,height=480,framerate=30/1 \
  ! videoconvert \
  ! $SINK
