#!/usr/bin/env bash
# 02_udp_jpeg_receiver.sh — receive RTP/JPEG from 01_udp_jpeg_sender.sh.
#
#   bash 02_udp_jpeg_receiver.sh                 # window, 15 s
#   SINK=fakesink DURATION=5 bash 02_udp_jpeg_receiver.sh   # headless
#
# A UDP receiver never gets EOS (EOS is a GStreamer event; it does not cross
# the network), so the script stops itself after DURATION seconds with
# SIGINT; gst-launch -e turns SIGINT into a clean EOS.
#
# Elements:
#   udpsrc          recvfrom() on PORT. caps=... is MANDATORY: packets carry
#                   no description of the stream. These caps are what an SDP
#                   file would say: video, 90 kHz clock, JPEG, payload 26.
#                   buffer-size: kernel receive buffer; raise it for HD.
#   rtpjitterbuffer reorders packets by sequence number and waits up to
#                   `latency` ms for late ones. Bigger = smoother, slower.
#   rtpjpegdepay    reassembles packets into whole JPEG frames (drops a frame
#                   if one of its packets is missing).
#   jpegdec         JPEG -> raw video.
#   videoconvert    converts to whatever the display wants.
#   $SINK           autovideosink (window) or fakesink. sync=false: show frames
#                   as soon as they arrive (lowest latency).
set -eu
PORT=${PORT:-5000}
SINK=${SINK:-autovideosink}   # unquoted below on purpose: SINK="fakesink silent=false" works
DURATION=${DURATION:-15}

exec timeout -s INT "$DURATION" gst-launch-1.0 -e \
  udpsrc port="$PORT" buffer-size=2097152 \
    caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=JPEG,payload=26" \
  ! rtpjitterbuffer latency=50 \
  ! rtpjpegdepay \
  ! jpegdec \
  ! videoconvert \
  ! $SINK sync=false
