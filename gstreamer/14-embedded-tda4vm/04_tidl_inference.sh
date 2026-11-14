#!/usr/bin/env bash
# 04_tidl_inference.sh — SKETCH of a TI deep-learning pipeline on the TDA4VM:
#   camera/test -> MSC scale -> colour convert -> tidlpreproc -> tidlinferer
#   (runs the model on the C7x DSP + MMA accelerator via TIDL) -> tidlpostproc
#   (draws boxes/labels) -> display or encoder
#
# TI-SPECIFIC: tidlinferer, tidlpreproc, tidlpostproc, tiovx* exist ONLY on
# TI's Edge AI SDK image, together with compiled models (TI model zoo, e.g.
# under /opt/model_zoo). UNVERIFIED on the course PC.
# PROPERTY NAMES BELOW ARE PLACEHOLDERS IN <ANGLE BRACKETS> on purpose: they
# change between SDK versions. Get the real ones with
#   gst-inspect-1.0 tidlinferer ; gst-inspect-1.0 tidlpreproc ; gst-inspect-1.0 tidlpostproc
# and from TI's generated pipelines: TI's edgeai-gst-apps prints the full
# gst-launch string it builds from a YAML config — copy from there.
set -eu
for e in tidlinferer tidlpreproc tidlpostproc tiovxmultiscaler; do
    if ! gst-inspect-1.0 --exists $e; then
        echo "$e not available (TI Edge AI SDK image required) — skipping"
        echo "Pipeline shape (for reading):"
        sed -n '/^# SHAPE/,/^# END/p' "$0"
        exit 0
    fi
done
echo "TI elements found. Fill in the <placeholders> from gst-inspect-1.0 and run the"
echo "pipeline at the bottom of this file by hand."

# SHAPE
#   v4l2src / videotestsrc  ── NV12 1080p
#     ! tiovxmultiscaler name=msc
#   msc. ! queue ! <model input size, e.g. 320x320> NV12
#     ! tiovxdlcolorconvert ! video/x-raw,format=RGB
#     ! tidlpreproc <model/config property> ! tidlinferer <model property>
#     ! queue ! post.<tensor pad>
#   msc. ! queue ! <display size> NV12 ! post.<image pad>
#   tidlpostproc name=post <model/config property> ! kmssink (or v4l2h264enc ! ...)
#
#   (pad names: see "Pad Templates" in gst-inspect-1.0 tidlpostproc)
#   Two branches meet again in tidlpostproc: one carries the tensor (results),
#   the other the image to draw on. Queues on both: they run in different
#   threads and the inference branch is slower.
# END
