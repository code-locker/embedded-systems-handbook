#!/usr/bin/env bash
# 01_udp_jpeg_sender.sh — send a test pattern as RTP/JPEG over UDP.
#
# Pair with 02_udp_jpeg_receiver.sh (same PORT). Run the receiver first, on
# this PC or on another machine (set HOST to the receiver's IP).
#   HOST=192.168.7.2 PORT=5000 bash 01_udp_jpeg_sender.sh
#
# Elements (all installed with gstreamer1.0-plugins-base / -good):
#   videotestsrc   source of test frames; is-live=true makes it behave like a
#                  camera (frames produced at the frame rate, not as fast as
#                  possible); num-buffers=N sends EOS after N frames so the
#                  script ends by itself.
#   video/x-raw,.. caps filter: I420 because RTP/JPEG (RFC 2435) only carries
#                  4:2:0 / 4:2:2 JPEG; size and rate fixed so we know them.
#   jpegenc        each frame becomes an independent JPEG (intra only:
#                  no frame depends on another -> a lost packet costs 1 frame).
#   rtpjpegpay     cuts each JPEG into RTP packets of <= mtu bytes, adds the
#                  RTP header (seq number, 90 kHz timestamp, payload type 26).
#   udpsink        sendto() each packet to HOST:PORT. sync=true (default):
#                  packets leave at buffer timestamps = smooth bandwidth.
set -eu
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-5000}
NUM=${NUM:-300}          # 300 frames at 30 fps = 10 s

exec gst-launch-1.0 -e \
  videotestsrc is-live=true num-buffers="$NUM" pattern=ball \
  ! video/x-raw,format=I420,width=640,height=480,framerate=30/1 \
  ! jpegenc quality=80 \
  ! rtpjpegpay mtu=1400 \
  ! udpsink host="$HOST" port="$PORT"
