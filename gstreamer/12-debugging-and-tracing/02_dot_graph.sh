#!/usr/bin/env bash
# 02_dot_graph.sh — make gst-launch-1.0 dump pipeline graphs, render to PNG.
#
# gst-launch-1.0 writes a .dot at every state change when
# GST_DEBUG_DUMP_DOT_DIR is set: *.NULL_READY.dot, *.READY_PAUSED.dot,
# *.PAUSED_PLAYING.dot, *.PLAYING_PAUSED.dot ... The READY_PAUSED / PAUSED_PLAYING
# ones show the NEGOTIATED caps on every link — the best negotiation debug tool.
# Rendering needs graphviz: sudo apt install graphviz   (or xdot for a viewer)
set -eu
OUT=${1:-$(mktemp -d)}
export GST_DEBUG_DUMP_DOT_DIR=$OUT

# videotestsrc         : test frames (num-buffers=10 => terminates)
# tee name=t           : splits the stream; each "t." starts a branch
# queue                : one thread per branch (a tee without queues deadlocks)
# videoscale           : resizes to what the capsfilter after it asks
# fakesink             : discards
gst-launch-1.0 -q videotestsrc num-buffers=10 ! tee name=t \
    t. ! queue ! fakesink \
    t. ! queue ! videoscale ! video/x-raw,width=64,height=48 ! fakesink

echo "dot files in $OUT:"
ls -1 "$OUT"
if command -v dot >/dev/null; then
    for f in "$OUT"/*PAUSED_PLAYING*.dot; do
        dot -Tpng "$f" -o "${f%.dot}.png" && echo "rendered ${f%.dot}.png"
    done
else
    echo "graphviz 'dot' not installed: sudo apt install graphviz, then"
    echo "  dot -Tpng $OUT/<file>.dot -o graph.png"
fi
