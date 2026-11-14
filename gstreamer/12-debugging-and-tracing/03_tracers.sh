#!/usr/bin/env bash
# 03_tracers.sh — the core tracers (libgstcoretracers: latency, leaks, stats,
# rusage, log, factories) run against a small pipeline.
#
# Tracers print records on category GST_TRACER at level 7 (TRACE).
# NOT in core: interlatency, proctime, framerate, cpuusage, queuelevel,
# scheduletime, bitrate — those come from RidgeRun's GstShark
# (https://github.com/RidgeRun/gst-shark), built separately.
set -eu
PIPE="videotestsrc num-buffers=50 ! video/x-raw,width=320,height=240 ! queue ! videoconvert ! fakesink"
# videotestsrc num-buffers=50 : terminates; queue : thread boundary; fakesink : sink
T="GST_DEBUG_NO_COLOR=1"
TMPDIR_LOG=$(mktemp); trap 'rm -f "$TMPDIR_LOG"' EXIT

echo "== latency: source->sink time for each buffer (ns). flags adds per-element"
env $T GST_DEBUG=GST_TRACER:7 GST_TRACERS="latency(flags=pipeline+element)" \
    gst-launch-1.0 -q $PIPE 2>"$TMPDIR_LOG" || true
grep -oE ':: latency, .*' "$TMPDIR_LOG" | cut -c1-200 | head -2
grep -oE ':: element-latency, .*' "$TMPDIR_LOG" | cut -c1-160 | head -3

echo "== rusage: CPU load of the process and of each thread"
env $T GST_DEBUG=GST_TRACER:7 GST_TRACERS=rusage \
    gst-launch-1.0 -q $PIPE 2>&1 | grep -oE 'proc-rusage,.*|thread-rusage,.*' | tail -2 | cut -c1-160 || true

echo "== stats: summarised by gst-stats-1.0 (counts buffers/events per pad)"
LOG=$(mktemp)
env $T GST_DEBUG=GST_TRACER:7 GST_DEBUG_FILE="$LOG" GST_TRACERS=stats gst-launch-1.0 -q $PIPE
if command -v gst-stats-1.0 >/dev/null; then
    gst-stats-1.0 "$LOG" | head -20
else
    echo "gst-stats-1.0 not found (it ships with gstreamer1.0-tools)"
fi
rm -f "$LOG"

echo "== leaks: objects still alive at exit (gst-launch is clean -> nothing listed)"
n=$(env $T GST_DEBUG=GST_TRACER:7 GST_TRACERS="leaks" \
    gst-launch-1.0 -q $PIPE 2>&1 | grep -c ':: object-alive,' || true)
echo "leaked objects: $n   (try it on ../../build/gstreamer/12-debugging-and-tracing/04_refleak_bug)"
