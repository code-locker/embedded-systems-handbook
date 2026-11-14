#!/usr/bin/env bash
# 11_rtp_udp_loopback.sh — stream video over UDP/RTP to yourself.
#
#  receiver (background):
#    udpsrc ─► [application/x-rtp caps] ─► rtpjpegdepay ─► jpegdec ─► fakesink
#  sender:
#    videotestsrc ─► [I420] ─► jpegenc ─► rtpjpegpay ─► udpsink 127.0.0.1:PORT
#
# jpegenc       : compress (raw video is far too big for a network).
# rtpjpegpay    : PAYLOADER: cuts JPEG frames into RTP packets that fit the
#                 MTU (~1400 bytes), adds sequence numbers and timestamps.
#                 Needs I420 input (hence the caps filter).
# udpsink       : sends each packet as a UDP datagram.
# udpsrc        : receives datagrams. UDP carries no format information, so
#                 YOU must give it caps: application/x-rtp,media=video,
#                 encoding-name=JPEG,clock-rate=90000,payload=26 (normally from SDP).
# rtpjpegdepay  : DEPAYLOADER: reassembles packets into JPEG frames.
# EOS does not travel over UDP, so the receiver is stopped with `timeout`.
# Lesson 11 does this from C.
set -euo pipefail
PORT=${PORT:-$((20000 + $$ % 20000))}
LOG=$(mktemp)
timeout 4 gst-launch-1.0 udpsrc port="$PORT" \
    caps="application/x-rtp,media=video,encoding-name=JPEG,clock-rate=90000,payload=26" \
  ! rtpjpegdepay ! jpegdec ! fakesink sync=false silent=false -v > "$LOG" 2>&1 &
RX=$!
sleep 1
gst-launch-1.0 videotestsrc num-buffers=60 ! video/x-raw,format=I420,width=320,height=240 \
  ! jpegenc ! rtpjpegpay ! udpsink host=127.0.0.1 port="$PORT"
wait $RX || true   # timeout returns 124: expected
# fakesink silent=false + -v prints one "chain" line per received frame.
N=$(grep -c 'fakesink0.*chain' "$LOG" || true)
rm -f "$LOG"
echo "receiver got $N of 60 frames"
[ "$N" -gt 0 ]
