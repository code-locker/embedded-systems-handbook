#!/usr/bin/env bash
# 05_rtp_h264_receiver.sh — receive the stream of 04_rtp_h264_sender.sh.
#
# NEEDS:  h264parse  -> gstreamer1.0-plugins-bad
#         avdec_h264 -> gstreamer1.0-libav   (software decoder, FFmpeg)
# On the BeagleBone AI-64 use the hardware decoder v4l2h264dec (TI SDK image)
# instead of avdec_h264.
#
# Elements:
#   udpsrc caps=...    H264, payload 96, 90 kHz (must match the sender)
#   rtpjitterbuffer latency=50  small = low latency; on Wi-Fi use 200+
#   rtph264depay       NAL units out of RTP packets
#   h264parse          frames whole access units for the decoder
#   avdec_h264         decode (max-threads=1 avoids frame-threading delay)
#   videoconvert ! $SINK sync=false   show immediately
set -eu
PORT=${PORT:-5004}
SINK=${SINK:-autovideosink}   # unquoted below on purpose: SINK="fakesink silent=false" works
DURATION=${DURATION:-15}

for e in h264parse avdec_h264; do
  gst-inspect-1.0 --exists "$e" || { echo "missing element: $e (see header)"; exit 1; }
done

exec timeout -s INT "$DURATION" gst-launch-1.0 -e \
  udpsrc port="$PORT" \
    caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96" \
  ! rtpjitterbuffer latency=50 \
  ! rtph264depay ! h264parse ! avdec_h264 max-threads=1 \
  ! videoconvert ! $SINK sync=false
