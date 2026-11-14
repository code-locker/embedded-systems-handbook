#!/usr/bin/env bash
# 03_rtp_vp8_loopback.sh — a COMPRESSED inter-frame codec (VP8) over RTP,
# sender and receiver in one script, on 127.0.0.1.
#
# Why VP8 here? It is in gstreamer1.0-plugins-good (vpx), so it runs on a stock
# desktop. H.264 is the usual choice in products: see 04/05 (needs extra
# packages) and, on the BeagleBone AI-64, the hardware encoder (lesson 14).
#
#   bash 03_rtp_vp8_loopback.sh              # receiver shows a window
#   SINK=fakesink bash 03_rtp_vp8_loopback.sh
#
# Sender elements:
#   videotestsrc is-live  camera-like source, 150 frames then EOS
#   vp8enc deadline=1     "realtime" speed; keyframe-max-dist=30 = a keyframe
#                         every second, so a receiver joining late (or after
#                         packet loss) recovers within 1 s
#   rtpvp8pay             RTP packetization for VP8 (dynamic payload type 96)
#   udpsink               send to the receiver
# Receiver elements:
#   udpsrc caps=...       VP8 caps; payload=96 must match the sender
#   rtpjitterbuffer       reorder + absorb jitter
#   rtpvp8depay ! vp8dec  rebuild frames and decode
#   $SINK                 display or discard
set -eu
PORT=${PORT:-5002}
SINK=${SINK:-autovideosink}   # unquoted below on purpose: SINK="fakesink silent=false" works

timeout -s INT 15 gst-launch-1.0 -e \
  udpsrc port="$PORT" \
    caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=VP8,payload=96" \
  ! rtpjitterbuffer latency=100 ! rtpvp8depay ! vp8dec ! videoconvert \
  ! $SINK sync=false &
RX=$!
sleep 1   # let the receiver bind its port first

gst-launch-1.0 -e \
  videotestsrc is-live=true num-buffers=150 pattern=smpte \
  ! video/x-raw,width=320,height=240,framerate=30/1 \
  ! vp8enc deadline=1 keyframe-max-dist=30 \
  ! rtpvp8pay \
  ! udpsink host=127.0.0.1 port="$PORT"

sleep 1               # let the last packets drain through the jitterbuffer
kill -INT "$RX" 2>/dev/null || true
wait "$RX" || true
echo "done: sent 150 VP8 frames over RTP/UDP"
