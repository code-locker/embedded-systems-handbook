#!/usr/bin/env bash
# 09_audio_video_mux.sh — two sources into one container, then probe it.
#
#   videotestsrc ─► jpegenc ─► queue ─┐
#                                     ├─► matroskamux name=mux ─► filesink
#   audiotestsrc ─► audioconvert ─────┤   (request pads video_0, audio_0)
#      ─► vorbisenc ─► queue ─────────┘
#
# vorbisenc    : audio encoder (plugins-base, libvorbis).
# mux.         : link into a new request pad of the element named "mux".
# queue        : each input of a muxer is fed by its own thread so the muxer
#                can interleave audio and video by timestamp.
# num-buffers is chosen so both streams last about 2 s:
#   60 video frames @ 30 fps = 2 s; 86 audio buffers × 1024 / 44100 Hz ≈ 2 s.
# gst-discoverer-1.0 then prints the streams found in the file.
set -euo pipefail
OUT=${OUT:-/tmp/gst-course-av.mkv}
gst-launch-1.0 -e matroskamux name=mux ! filesink location="$OUT" \
  videotestsrc num-buffers=60 ! video/x-raw,width=320,height=240,framerate=30/1 \
     ! jpegenc ! queue ! mux. \
  audiotestsrc num-buffers=86 ! audioconvert ! vorbisenc ! queue ! mux.
gst-discoverer-1.0 "$OUT" | head -20
