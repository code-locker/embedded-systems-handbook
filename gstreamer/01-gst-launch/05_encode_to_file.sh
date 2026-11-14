#!/usr/bin/env bash
# 05_encode_to_file.sh — encode video to a file (MJPEG in Matroska).
#
#   videotestsrc ─► jpegenc ─► matroskamux ─► filesink
#
# jpegenc      : compresses each frame to JPEG (image/jpeg caps).
# matroskamux  : packs compressed streams into an .mkv container
#                (adds headers, timestamps, an index). Its sink pads are
#                REQUEST pads (video_%u, audio_%u): gst-launch requests one.
# filesink     : writes bytes to a file.
# -e           : on Ctrl-C, send EOS first so the muxer writes a valid file
#                (without it the .mkv/.mp4 can be truncated/unplayable).
#                Here num-buffers ends it, but get used to typing -e.
#
# Requires: gstreamer1.0-plugins-good (jpeg, matroska).
set -euo pipefail
OUT=${OUT:-/tmp/gst-course-test.mkv}
gst-launch-1.0 -e videotestsrc num-buffers=60 \
  ! video/x-raw,width=320,height=240,framerate=30/1 \
  ! jpegenc ! matroskamux ! filesink location="$OUT"
ls -l "$OUT"
