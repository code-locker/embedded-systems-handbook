#!/usr/bin/env bash
# 06_decode_file.sh — play back the file from 05, two ways.
#
#  A) by hand:   filesrc ─► matroskademux ─► jpegdec ─► videoconvert ─► sink
#  B) automatic: filesrc ─► decodebin ─► videoconvert ─► sink
#
# filesrc        : reads bytes from a file.
# matroskademux  : splits the container into streams. Its src pads are
#                  SOMETIMES pads (they appear once the header is parsed);
#                  gst-launch links them automatically when they show up.
# jpegdec        : JPEG → raw video.
# decodebin      : an "auto" bin: typefinds the data, plugs the right demuxer
#                  and decoders, exposes raw pads. Same result, no knowledge
#                  of the format needed.
set -euo pipefail
IN=${IN:-/tmp/gst-course-test.mkv}
[ -f "$IN" ] || "$(dirname "$0")/05_encode_to_file.sh"
SINK=${SINK:-fakesink sync=false}
echo "--- A) explicit demux + decode"
gst-launch-1.0 filesrc location="$IN" ! matroskademux ! jpegdec ! videoconvert ! $SINK
echo "--- B) decodebin"
gst-launch-1.0 filesrc location="$IN" ! decodebin ! videoconvert ! $SINK
