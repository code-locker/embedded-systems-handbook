#!/usr/bin/env bash
# 06_tcp_mjpeg_loopback.sh — streaming over TCP instead of RTP/UDP.
#
# TCP never loses or reorders data, so no RTP and no jitterbuffer are needed;
# but a stalled network stalls the stream (retransmissions -> latency grows).
# We still need FRAMING: TCP is a byte stream, so multipartmux puts a MIME
# boundary between JPEGs and multipartdemux finds them again (this is the
# same "MJPEG over HTTP" format many IP cameras serve).
#
# Elements (all base/good):
#   tcpserversink host port   accepts clients, sends to all of them
#     recover-policy=keyframe  slow client: drop data until next keyframe
#                              (every JPEG is one) instead of blocking
#   tcpclientsrc              connects and reads the byte stream
#   multipartdemux            byte stream -> one buffer per JPEG
set -eu
PORT=${PORT:-5010}
SINK=${SINK:-autovideosink}   # unquoted below on purpose: SINK="fakesink silent=false" works

gst-launch-1.0 -e \
  videotestsrc is-live=true num-buffers=150 pattern=ball \
  ! video/x-raw,width=320,height=240,framerate=30/1 \
  ! jpegenc ! multipartmux \
  ! tcpserversink host=127.0.0.1 port="$PORT" recover-policy=keyframe &
TX=$!
sleep 1   # server must listen before the client connects

timeout -s INT 10 gst-launch-1.0 -e \
  tcpclientsrc host=127.0.0.1 port="$PORT" \
  ! multipartdemux ! image/jpeg ! jpegdec ! videoconvert \
  ! $SINK sync=false || true

wait "$TX" || true
echo "done: MJPEG over TCP"
