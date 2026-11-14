#!/usr/bin/env bash
# 04_rtp_h264_sender.sh — the "classic" low-latency H.264 RTP stream.
#
# NEEDS PACKAGES NOT IN A BASE INSTALL:
#   x264enc    -> gstreamer1.0-plugins-ugly   (software H.264 encoder)
#   h264parse  -> gstreamer1.0-plugins-bad
#   rtph264pay -> gstreamer1.0-plugins-good
# On the BeagleBone AI-64 replace "x264enc ..." with the TI hardware encoder
# (v4l2h264enc, available on TI's SDK / Edge AI image) — see lesson 14.
#
#   HOST=192.168.7.1 bash 04_rtp_h264_sender.sh
#
# Elements:
#   videotestsrc is-live   camera-like source
#   x264enc tune=zerolatency  disables B-frames and look-ahead: each frame
#                          leaves the encoder immediately (default settings
#                          buffer ~1-2 s of frames!)
#           speed-preset=ultrafast  lowest CPU
#           key-int-max=30 IDR every 1 s at 30 fps (recovery after loss)
#           bitrate=2000   kbit/s
#   h264parse config-interval=-1  repeat SPS/PPS before every IDR so a
#                          receiver that joins late can start decoding
#   rtph264pay pt=96       RFC 6184 packetization (FU-A fragments for big NALs)
#   udpsink                send
set -eu
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-5004}
NUM=${NUM:-300}

for e in x264enc h264parse rtph264pay; do
  gst-inspect-1.0 --exists "$e" || { echo "missing element: $e (see header)"; exit 1; }
done

exec gst-launch-1.0 -e \
  videotestsrc is-live=true num-buffers="$NUM" \
  ! video/x-raw,width=1280,height=720,framerate=30/1 \
  ! x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=2000 \
  ! h264parse config-interval=-1 \
  ! rtph264pay pt=96 \
  ! udpsink host="$HOST" port="$PORT"
