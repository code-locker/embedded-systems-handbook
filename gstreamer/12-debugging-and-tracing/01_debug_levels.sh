#!/usr/bin/env bash
# 01_debug_levels.sh — GST_DEBUG levels and category globbing, live.
#
# Levels: 0 none 1 ERROR 2 WARNING 3 FIXME 4 INFO 5 DEBUG 6 LOG 7 TRACE 9 MEMDUMP
# Syntax: GST_DEBUG=<default level>,<category glob>:<level>,...
#   later entries win; globs match category names (see --gst-debug-help).
# Everything here uses core + plugins-base only and ends by itself.
set -eu

PIPE="videotestsrc num-buffers=5 ! video/x-raw,width=160,height=120 ! videoconvert ! fakesink"
# videotestsrc num-buffers=5  : 5 frames then EOS, so every run terminates
# video/x-raw,...             : a capsfilter, forces the negotiated size
# videoconvert                : format glue (passthrough here)
# fakesink                    : discards buffers

echo "== 1. Level 2 (warnings+errors): the setting to always run with in production"
GST_DEBUG=2 gst-launch-1.0 -q $PIPE

echo "== 2. One element at DEBUG, everything else at WARNING (count lines only)"
GST_DEBUG=2,videotestsrc:5 GST_DEBUG_NO_COLOR=1 gst-launch-1.0 -q $PIPE 2>&1 | grep -c videotestsrc || true

echo "== 3. Glob: every caps-related category (GST_CAPS, basetransform-caps...) at DEBUG (first 5 lines)"
GST_DEBUG='*CAPS*:5' GST_DEBUG_NO_COLOR=1 gst-launch-1.0 -q $PIPE 2>&1 | head -5 || true

echo "== 4. Negotiation story: GST_PADS + GST_CAPS at DEBUG into a file"
LOG=$(mktemp)
GST_DEBUG='GST_PADS:5,GST_CAPS:5' GST_DEBUG_FILE="$LOG" GST_DEBUG_NO_COLOR=1 gst-launch-1.0 -q $PIPE
echo "   $(wc -l < "$LOG") lines written to a file instead of stderr"
rm -f "$LOG"

echo "== 5. List all categories that exist (first 10)"
gst-launch-1.0 --gst-debug-no-color --gst-debug-help 2>/dev/null | head -10

echo "== 6a. -v prints the negotiated caps of every pad (notify::caps)"
gst-launch-1.0 -v $PIPE 2>&1 | grep 'caps =' | head -3
echo "== 6b. -m prints every bus message (state changes, stream-status, eos...)"
gst-launch-1.0 -m $PIPE 2>&1 | grep 'Got message' | cut -c1-110 | head -5
