#!/usr/bin/env bash
# 01_hello_test_pattern.sh — the smallest pipeline: a source and a sink.
#
#   videotestsrc ──► fakesink
#
# videotestsrc  : generates test video frames (colour bars by default).
#                 num-buffers=100 → send EOS after 100 frames, so gst-launch
#                 exits by itself instead of running forever.
# fakesink      : accepts any data and throws it away. The "/dev/null" of
#                 GStreamer: perfect for tests and benchmarks.
#
# Try: SINK=autovideosink ./01_hello_test_pattern.sh   (opens a window)
set -euo pipefail
SINK=${SINK:-fakesink}
gst-launch-1.0 videotestsrc num-buffers=100 ! $SINK
