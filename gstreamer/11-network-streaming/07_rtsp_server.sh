#!/usr/bin/env bash
# 07_rtsp_server.sh — serve a pipeline over RTSP with gst-rtsp-server.
#
# NEEDS gst-rtsp-server's example binary "test-launch". It is NOT shipped as a
# command in most distros; build it from the gst-rtsp-server examples
# (examples/test-launch.c, needs libgstrtspserver-1.0-dev):
#   sudo apt install libgstrtspserver-1.0-dev gstreamer1.0-rtsp
#   gcc test-launch.c -o test-launch $(pkg-config --cflags --libs gstreamer-rtsp-server-1.0)
# Client side ("rtspsrc", plugins-good) works without the server package.
#
# The launch line must end in a payloader named pay0 (pay1 for a 2nd stream):
# the server links it to its own RTP session, writes the SDP from its caps,
# and creates one pipeline per client (or one shared, with --shared).
#
# Then play it from any machine:
#   gst-launch-1.0 rtspsrc location=rtsp://<ip>:8554/test latency=100 \
#       ! rtpjpegdepay ! jpegdec ! videoconvert ! autovideosink
#   (or: ffplay / vlc rtsp://<ip>:8554/test)
set -eu
TL=${TEST_LAUNCH:-test-launch}
command -v "$TL" >/dev/null || { echo "test-launch not found (see header)"; exit 1; }

exec "$TL" "( videotestsrc is-live=true \
  ! video/x-raw,format=I420,width=640,height=480,framerate=30/1 \
  ! jpegenc ! rtpjpegpay name=pay0 pt=26 )"
