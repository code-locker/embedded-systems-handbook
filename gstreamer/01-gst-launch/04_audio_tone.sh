#!/usr/bin/env bash
# 04_audio_tone.sh — audio works exactly the same way.
#
#   audiotestsrc ─► audioconvert ─► audioresample ─► [S16LE 16 kHz mono] ─► fakesink
#
# audiotestsrc  : sine wave generator. freq=440 (A4). num-buffers=50 buffers
#                 of samplesperbuffer (default 1024) samples.
# audioconvert  : converts sample format (F32 → S16) and channel layout.
# audioresample : converts the sample rate (44.1/48 kHz → 16 kHz).
# caps filter   : asks for what a speech recogniser typically wants.
#
# Hear it: SINK=autoaudiosink ./04_audio_tone.sh
set -euo pipefail
SINK=${SINK:-fakesink sync=false}
gst-launch-1.0 audiotestsrc freq=440 num-buffers=50 \
  ! audioconvert ! audioresample \
  ! audio/x-raw,format=S16LE,rate=16000,channels=1 \
  ! $SINK
