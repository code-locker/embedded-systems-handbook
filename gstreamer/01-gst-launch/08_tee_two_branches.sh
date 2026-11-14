#!/usr/bin/env bash
# 08_tee_two_branches.sh — one source, two consumers (e.g. display + record).
#
#                         ┌► queue ─► videoconvert ─► fakesink   ("display")
#   videotestsrc ─► tee ──┤
#                         └► queue ─► jpegenc ─► matroskamux ─► filesink ("record")
#
# tee name=t  : copies (by reference, no byte copy) each buffer to every
#               branch. Its src pads are REQUEST pads; "t." in the
#               description means "request a new pad from the element named t".
# queue       : MANDATORY after each tee branch. A queue starts a new thread
#               for its branch, so a slow branch cannot block the other one,
#               and the pipeline can preroll (each sink waits in PAUSED;
#               without queues the single thread would block in the first
#               sink and the second never gets a buffer → deadlock).
set -euo pipefail
OUT=${OUT:-/tmp/gst-course-tee.mkv}
SINK=${SINK:-fakesink sync=false}
gst-launch-1.0 -e videotestsrc num-buffers=90 ! video/x-raw,width=320,height=240 \
  ! tee name=t \
  t. ! queue ! videoconvert ! $SINK \
  t. ! queue ! jpegenc ! matroskamux ! filesink location="$OUT"
ls -l "$OUT"
