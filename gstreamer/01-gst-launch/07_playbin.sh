#!/usr/bin/env bash
# 07_playbin.sh — the one-element media player.
#
#   playbin uri=file:///... (contains uridecodebin + sinks + volume + ...)
#
# playbin      : a complete player in one element. Give it a URI
#                (file://, http://, rtsp://) and it builds the whole
#                decoding + rendering pipeline. video-sink / audio-sink let
#                you replace its default sinks (here: fakesink, headless).
#                The `video-sink="fakesink sync=false"` value is itself parsed
#                as a small pipeline description (a "bin description").
set -euo pipefail
IN=${IN:-/tmp/gst-course-test.mkv}
[ -f "$IN" ] || "$(dirname "$0")/05_encode_to_file.sh"
VSINK=${VSINK:-fakesink sync=false}
gst-launch-1.0 playbin uri="file://$IN" video-sink="$VSINK" audio-sink=fakesink
